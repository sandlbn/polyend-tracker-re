// HostStorage on std::filesystem (portable: Linux, macOS, mingw-w64). Card
// paths are UTF-8 with '/' separators; host paths are built per component.
#include "host_storage.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace sim {

namespace {
// UTF-8 <-> host path (narrow on POSIX, wide on Windows).
fs::path fromUtf8(const std::string& s) {
#ifdef _WIN32
    return fs::u8path(s);
#else
    return fs::path(s);
#endif
}
std::string toUtf8(const fs::path& p) {
#ifdef _WIN32
    return p.u8string();
#else
    return p.string();
#endif
}
// strcasecmp in the C locale: ASCII letters fold, other bytes match exactly.
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char x = uint8_t(a[i]), y = uint8_t(b[i]);
        if (x >= 'A' && x <= 'Z') x = uint8_t(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = uint8_t(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}
bool isDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
bool pathExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}
// A symlink on the card (also a dangling one): never followed, so a card path
// cannot reach outside the root (#103).
bool isLink(const fs::path& p) {
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(p, ec));
}
std::FILE* openFile(const fs::path& p, const char* mode) {
#ifdef _WIN32
    wchar_t wmode[8] = {};
    for (int i = 0; mode[i] && i < 7; ++i) wmode[i] = wchar_t(mode[i]);
    return ::_wfopen(p.c_str(), wmode);
#else
    return std::fopen(p.c_str(), mode);
#endif
}
}  // namespace

bool HostStorage::init(const std::string& root) {
    root_ = root;
    while (root_.size() > 1 && (root_.back() == '/' || root_.back() == '\\')) root_.pop_back();
    const fs::path p = fromUtf8(root_);
    std::error_code ec;
    if (!isDir(p)) fs::create_directories(p, ec);
    return isDir(p);
}

bool HostStorage::resolve(const char* path, std::string& out, bool* existsOut) const {
    fs::path cur = fromUtf8(root_);
    bool exists = true;
    const std::string p(path ? path : "");
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == '/') ++i;
        if (i >= p.size()) break;
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string comp = p.substr(i, j - i);
        i = j;
        if (comp == ".") continue;
        if (comp == "..") return false;   // not on a FAT card root
        if (!exists) return false;        // missing intermediate directory
        const fs::path cand = cur / fromUtf8(comp);
        if (isLink(cand)) return false;
        if (pathExists(cand)) {
            cur = cand;
            continue;
        }
        // FAT semantics: case-insensitive match.
        bool found = false;
        std::error_code ec;
        for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path name = it->path().filename();
            if (iequals(toUtf8(name), comp)) {
                if (isLink(it->path())) return false;
                cur /= name;
                found = true;
                break;
            }
        }
        if (!found) {
            cur = cand;
            exists = false;
        }
    }
    out = toUtf8(cur);
    if (existsOut) *existsOut = exists;
    return true;
}

ot::IStorage::Handle HostStorage::open(const char* path, OpenMode mode) {
    if (refuse()) return -1;
    std::string hs;
    bool ex = false;
    if (!resolve(path, hs, &ex)) return -1;
    const fs::path hp = fromUtf8(hs);
    if (ex && isDir(hp)) return -1;
    int slot = -1;
    for (int i = 0; i < kMaxOpen; ++i)
        if (!files_[i]) { slot = i; break; }
    if (slot < 0) return -1;
    std::FILE* f = nullptr;
    switch (mode) {
        case kRead: f = ex ? openFile(hp, "rb") : nullptr; break;
        case kWrite: f = openFile(hp, "wb"); break;
        case kReadWrite: f = openFile(hp, ex ? "r+b" : "w+b"); break;
    }
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
    const long pos = std::ftell(f);
    if (pos < 0 || std::fseek(f, 0, SEEK_END) != 0) return -1;
    const long end = std::ftell(f);
    std::fseek(f, pos, SEEK_SET);
    return end < 0 ? -1 : int32_t(end);
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
    std::string hp;
    bool ex = false;
    return resolve(path, hp, &ex) && ex;
}

bool HostStorage::mkdir(const char* path) {
    if (refuse()) return false;
    std::string hs;
    bool ex = false;
    if (!resolve(path, hs, &ex)) return false;
    const fs::path hp = fromUtf8(hs);
    if (ex) return isDir(hp);
    std::error_code ec;
    return fs::create_directory(hp, ec) || isDir(hp);
}

bool HostStorage::remove(const char* path) {
    if (refuse()) return false;
    std::string hs;
    bool ex = false;
    if (!resolve(path, hs, &ex) || !ex || hs == root_) return false;
    // fs::remove refuses a non-empty directory (rmdir semantics).
    std::error_code ec;
    return fs::remove(fromUtf8(hs), ec) && !ec;
}

bool HostStorage::rename(const char* from, const char* to) {
    if (refuse()) return false;
    std::string a, b;
    bool exA = false, exB = false;
    if (!resolve(from, a, &exA) || !exA || !resolve(to, b, &exB)) return false;
    if (exB && a != b) return false;   // FatFs f_rename fails if the target exists
    std::error_code ec;
    fs::rename(fromUtf8(a), fromUtf8(b), ec);
    return !ec;
}

int32_t HostStorage::listDir(const char* path, DirFn fn, void* ctx) {
    if (refuse()) return -1;
    std::string hs;
    bool ex = false;
    if (!resolve(path, hs, &ex) || !ex) return -1;
    const fs::path hp = fromUtf8(hs);
    if (!isDir(hp)) return -1;
    struct Ent {
        std::string name;
        bool dir;
        uint32_t size;
    };
    std::vector<Ent> ents;
    std::error_code ec;
    fs::directory_iterator it(hp, ec);
    if (ec) return -1;
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        if (isLink(it->path())) continue;   // not reachable through resolve()
        const fs::file_status st = it->status(e2);
        if (e2 || !fs::exists(st)) continue;
        const bool dir = fs::is_directory(st);
        uint32_t size = 0;
        if (!dir) {
            const auto sz = fs::file_size(it->path(), e2);
            size = e2 ? 0u : uint32_t(sz);
        }
        ents.push_back({toUtf8(it->path().filename()), dir, size});
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
