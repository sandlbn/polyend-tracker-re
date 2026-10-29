#include "host_storage.h"

#include <dirent.h>
#include <fcntl.h>
#include <cerrno>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace sim {

namespace {
bool isDir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool mkdirs(const std::string& p) {
    if (p.empty() || isDir(p)) return true;
    const size_t slash = p.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !mkdirs(p.substr(0, slash))) return false;
    return ::mkdir(p.c_str(), 0755) == 0 || isDir(p);
}
}  // namespace

HostStorage::~HostStorage() {
    for (auto* f : files_) if (f) std::fclose(f);
    if (rootFd_ >= 0) ::close(rootFd_);
}

bool HostStorage::init(const std::string& root) {
    if (rootFd_ >= 0) return false;
    root_ = root;
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
    if (!mkdirs(root_)) return false;
    rootFd_ = ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    return rootFd_ >= 0;
}

int HostStorage::resolve(const char* path, std::string& leaf, bool* existsOut) const {
    std::vector<std::string> parts;
    std::string p(path ? path : "");
    for (size_t i = 0; i < p.size();) {
        if (p[i] == '/') { ++i; continue; }
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const auto comp = p.substr(i, j - i);
        i = j;
        if (comp == "..") return -1;
        if (comp != ".") parts.push_back(comp);
    }
    int fd = ::openat(rootFd_, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    leaf = ".";
    if (existsOut) *existsOut = true;
    for (size_t i = 0; i < parts.size(); ++i) {
        leaf = parts[i];
        struct stat st;
        bool exists = ::fstatat(fd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
        if (!exists) {
            if (errno != ENOENT) { ::close(fd); return -1; }
            // A separate descriptor keeps directory scan offsets independent.
            int scan = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            DIR* d = scan >= 0 ? ::fdopendir(scan) : nullptr;
            if (!d) {
                if (scan >= 0) ::close(scan);
                ::close(fd);
                return -1;
            }
            while (dirent* e = ::readdir(d)) {
                if (::strcasecmp(e->d_name, leaf.c_str()) == 0) {
                    leaf = e->d_name;
                    exists = ::fstatat(fd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
                    break;
                }
            }
            ::closedir(d);
        }
        if (exists && S_ISLNK(st.st_mode)) { ::close(fd); return -1; }
        if (i + 1 == parts.size()) {
            if (existsOut) *existsOut = exists;
            return fd;
        }
        const int next = ::openat(fd, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(fd);
        if (next < 0) return -1;
        fd = next;
    }
    return fd;
}

ot::IStorage::Handle HostStorage::open(const char* path, OpenMode mode) {
    if (refuse()) return -1;
    int slot = -1;
    for (int i = 0; i < kMaxOpen; ++i)
        if (!files_[i]) { slot = i; break; }
    if (slot < 0) return -1;
    std::string leaf;
    int parent = resolve(path, leaf);
    if (parent < 0) return -1;
    int flags = O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
    const char* fm = "rb";
    switch (mode) {
        case kRead: flags |= O_RDONLY; break;
        case kWrite: flags |= O_WRONLY | O_CREAT; fm = "wb"; break;
        case kReadWrite: flags |= O_RDWR | O_CREAT; fm = "r+b"; break;
    }
    int fd = ::openat(parent, leaf.c_str(), flags, 0666);
    ::close(parent);
    if (fd < 0) return -1;
    struct stat st;
    // Validate before truncating: only regular files belong on the emulated card.
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (mode == kWrite && ::ftruncate(fd, 0) != 0)) {
        ::close(fd);
        return -1;
    }
    std::FILE* f = ::fdopen(fd, fm);
    if (!f) ::close(fd);
    if (!f) return -1;
    files_[slot] = f;
    return slot;
}

int32_t HostStorage::read(Handle h, void* dst, uint32_t len) {
    if (refuse()) return -1;
    std::FILE* f = get(h);
    if (!f) return -1;
    std::fflush(f);
    const size_t n = std::fread(dst, 1, len, f);
    if (n < len && std::ferror(f)) return -1;
    return int32_t(n);
}

int32_t HostStorage::write(Handle h, const void* src, uint32_t len) {
    if (refuse()) return -1;
    std::FILE* f = get(h);
    if (!f) return -1;
    // Switching from reading to writing on a "r+" stream needs a positioning call.
    std::fseek(f, 0, SEEK_CUR);
    const size_t n = std::fwrite(src, 1, len, f);
    return n == len || n > 0 ? int32_t(n) : -1;
}

bool HostStorage::seek(Handle h, uint32_t pos) {
    if (refuse()) return false;
    std::FILE* f = get(h);
    return f && std::fseek(f, long(pos), SEEK_SET) == 0;
}

int32_t HostStorage::size(Handle h) {
    if (refuse()) return -1;
    std::FILE* f = get(h);
    if (!f) return -1;
    std::fflush(f);
    struct stat st;
    if (::fstat(::fileno(f), &st) != 0) return -1;
    return int32_t(st.st_size);
}

void HostStorage::close(Handle h) {
    if (refuse()) return;
    if (std::FILE* f = get(h)) {
        std::fclose(f);
        files_[h] = nullptr;
    }
}

bool HostStorage::exists(const char* path) {
    if (refuse()) return false;
    std::string leaf;
    bool ex = false;
    int fd = resolve(path, leaf, &ex);
    if (fd < 0) return false;
    ::close(fd);
    return ex;
}

bool HostStorage::mkdir(const char* path) {
    if (refuse()) return false;
    std::string leaf;
    int fd = resolve(path, leaf);
    if (fd < 0) return false;
    struct stat st;
    bool ok = ::mkdirat(fd, leaf.c_str(), 0755) == 0 ||
        (::fstatat(fd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode));
    ::close(fd);
    return ok;
}

bool HostStorage::remove(const char* path) {
    if (refuse()) return false;
    std::string leaf;
    int fd = resolve(path, leaf);
    if (fd < 0) return false;
    struct stat st;
    bool ok = leaf != "." && ::fstatat(fd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        ::unlinkat(fd, leaf.c_str(), S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0) == 0;
    ::close(fd);
    return ok;
}

bool HostStorage::rename(const char* from, const char* to) {
    if (refuse()) return false;
    std::string a, b;
    bool exA = false, exB = false;
    int fa = resolve(from, a, &exA);
    int fb = resolve(to, b, &exB);
    struct stat sa, sb;
    const bool same = fa >= 0 && fb >= 0 && a == b &&
        ::fstat(fa, &sa) == 0 && ::fstat(fb, &sb) == 0 &&
        sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
    // FatFs does not replace existing destinations; a self-rename is a no-op.
    bool ok = fa >= 0 && fb >= 0 && exA && (!exB || same) && a != "." && b != "." &&
        ::renameat(fa, a.c_str(), fb, b.c_str()) == 0;
    if (fa >= 0) ::close(fa);
    if (fb >= 0) ::close(fb);
    return ok;
}

int32_t HostStorage::listDir(const char* path, DirFn fn, void* ctx) {
    if (refuse()) return -1;
    std::string leaf;
    int parent = resolve(path, leaf);
    if (parent < 0) return -1;
    int fd = ::openat(parent, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    ::close(parent);
    if (fd < 0) return -1;
    struct Ent {
        std::string name;
        bool dir;
        uint32_t size;
    };
    std::vector<Ent> ents;
    if (DIR* d = ::fdopendir(fd)) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            struct stat st;
            if (::fstatat(::dirfd(d), n.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 || S_ISLNK(st.st_mode)) continue;
            ents.push_back({n, S_ISDIR(st.st_mode), S_ISDIR(st.st_mode) ? 0u : uint32_t(st.st_size)});
        }
        ::closedir(d);
    } else {
        ::close(fd);
        return -1;
    }
    std::sort(ents.begin(), ents.end(), [](const Ent& x, const Ent& y) { return x.name < y.name; });
    int32_t n = 0;
    for (const Ent& e : ents) {
        DirEntry de{e.name.c_str(), e.dir, e.size};
        ++n;
        if (fn && !fn(de, ctx)) break;
    }
    return n;
}

}  // namespace sim
