// HostStorage check (desktop plan D2): the host folder as the SD card keeps
// FAT-like semantics on every platform. Run by `make -C sim check`:
//   host-storage-check <scratch dir>
// The scratch directory is created and must be empty or missing.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "host_storage.h"

namespace {
int gFails = 0, gChecks = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++gChecks;                                                            \
        if (!(c)) {                                                           \
            ++gFails;                                                         \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
        }                                                                     \
    } while (0)

bool writeFile(sim::HostStorage& s, const char* path, const char* text) {
    const auto h = s.open(path, ot::IStorage::kWrite);
    if (h < 0) return false;
    const int32_t n = s.write(h, text, uint32_t(std::strlen(text)));
    s.close(h);
    return n == int32_t(std::strlen(text));
}

std::string readFile(sim::HostStorage& s, const char* path) {
    const auto h = s.open(path, ot::IStorage::kRead);
    if (h < 0) return "<none>";
    char buf[256] = {};
    const int32_t n = s.read(h, buf, sizeof(buf) - 1);
    s.close(h);
    return n >= 0 ? std::string(buf, size_t(n)) : "<error>";
}

struct Lister {
    std::vector<std::string> names;
    std::vector<bool> dirs;
    std::vector<uint32_t> sizes;
    static bool fn(const ot::IStorage::DirEntry& e, void* ctx) {
        auto* l = static_cast<Lister*>(ctx);
        l->names.push_back(e.name);
        l->dirs.push_back(e.isDir);
        l->sizes.push_back(e.size);
        return true;
    }
};
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: host-storage-check <scratch dir>\n");
        return 2;
    }
    const std::string root = std::string(argv[1]) + "/card";
    sim::HostStorage s;
    CHECK(s.init(root + "/"));            // created, trailing slash dropped
    CHECK(s.root() == root);
    CHECK(s.exists(""));                  // the root itself

    // open / write / read / seek / size
    CHECK(writeFile(s, "hello.txt", "hello world"));
    CHECK(readFile(s, "hello.txt") == "hello world");
    {
        const auto h = s.open("hello.txt", ot::IStorage::kReadWrite);
        CHECK(h >= 0);
        CHECK(s.size(h) == 11);
        CHECK(s.seek(h, 6));
        char buf[8] = {};
        CHECK(s.read(h, buf, 5) == 5);
        CHECK(std::string(buf) == "world");
        CHECK(s.seek(h, 0));
        CHECK(s.write(h, "HELLO", 5) == 5);
        CHECK(s.seek(h, 20));   // past the end is allowed (FAT expands on write)
        s.close(h);
    }
    CHECK(readFile(s, "hello.txt") == "HELLO world");
    CHECK(s.open("missing.txt", ot::IStorage::kRead) < 0);
    {
        const auto h = s.open("rw-new.bin", ot::IStorage::kReadWrite);   // created by r/w
        CHECK(h >= 0);
        CHECK(s.write(h, "abc", 3) == 3);
        CHECK(s.size(h) == 3);
        s.close(h);
    }

    // Case-insensitive component lookup, the created name's case is kept.
    CHECK(s.mkdir("Projects"));
    CHECK(s.mkdir("projects"));           // exists already (same folder) -> true
    CHECK(s.exists("PROJECTS"));
    CHECK(writeFile(s, "projects/Song/x.txt", "nope") == false);   // missing intermediate dir
    CHECK(s.mkdir("projects/Song"));
    CHECK(writeFile(s, "PROJECTS/song/X.TXT", "case"));
    CHECK(readFile(s, "Projects/Song/x.txt") == "case");
    {
        Lister l;
        CHECK(s.listDir("", &Lister::fn, &l) == 3);
        // sorted, real case
        CHECK(l.names.size() == 3 && l.names[0] == "Projects" && l.names[1] == "hello.txt" &&
              l.names[2] == "rw-new.bin");
        CHECK(l.dirs.size() == 3 && l.dirs[0] && !l.dirs[1]);
        CHECK(l.sizes.size() == 3 && l.sizes[0] == 0 && l.sizes[1] == 11 && l.sizes[2] == 3);
        Lister l2;
        CHECK(s.listDir("projects/song", &Lister::fn, &l2) == 1);
        CHECK(l2.names.size() == 1 && l2.names[0] == "X.TXT");
        CHECK(s.listDir("hello.txt", &Lister::fn, &l2) == -1);   // not a directory
        CHECK(s.listDir("nothere", &Lister::fn, &l2) == -1);
    }
    {
        // A callback returning false stops the listing.
        struct Stop {
            static bool fn(const ot::IStorage::DirEntry&, void*) { return false; }
        };
        CHECK(s.listDir("", &Stop::fn, nullptr) == 1);
    }

    // '..' is refused; directories cannot be opened as files.
    CHECK(!s.exists("../card/hello.txt"));
    CHECK(s.open("../escape.txt", ot::IStorage::kWrite) < 0);
    CHECK(s.open("Projects", ot::IStorage::kRead) < 0);
    CHECK(s.exists("./hello.txt"));

    // rename: target exists -> refused (FatFs f_rename); same name in another case is fine.
    CHECK(writeFile(s, "a.txt", "A"));
    CHECK(writeFile(s, "b.txt", "B"));
    CHECK(!s.rename("a.txt", "b.txt"));
    CHECK(readFile(s, "b.txt") == "B");
    CHECK(s.rename("a.txt", "c.txt"));
    CHECK(!s.exists("a.txt") && readFile(s, "c.txt") == "A");
    CHECK(!s.rename("nothere.txt", "d.txt"));
    CHECK(s.rename("Projects/Song", "Projects/Tune"));
    CHECK(readFile(s, "projects/tune/x.txt") == "case");

    // remove: files, empty directories; not a non-empty directory, not the root.
    CHECK(!s.remove("Projects/Tune"));
    CHECK(s.remove("projects/tune/x.txt"));
    CHECK(s.remove("Projects/Tune"));
    CHECK(!s.exists("Projects/Tune"));
    CHECK(!s.remove(""));
    CHECK(!s.remove("nothere"));
    CHECK(s.remove("c.txt"));

    // 32 handles; the 33rd open fails; closing one frees a slot.
    {
        ot::IStorage::Handle hs[sim::HostStorage::kMaxOpen];
        bool all = true;
        for (int i = 0; i < sim::HostStorage::kMaxOpen; ++i) {
            hs[i] = s.open("hello.txt", ot::IStorage::kRead);
            all = all && hs[i] >= 0;
        }
        CHECK(all);
        CHECK(s.open("hello.txt", ot::IStorage::kRead) < 0);
        s.close(hs[5]);
        const auto again = s.open("hello.txt", ot::IStorage::kRead);
        CHECK(again == hs[5]);
        for (int i = 0; i < sim::HostStorage::kMaxOpen; ++i) s.close(hs[i]);
        CHECK(s.read(hs[0], nullptr, 0) == -1);   // closed handle
    }
    CHECK(s.read(-1, nullptr, 0) == -1 && s.size(99) == -1 && !s.seek(40, 0));

    // USB Storage lock: every access refused and counted.
    s.setLocked(true);
    CHECK(s.locked());
    CHECK(!s.exists("hello.txt"));
    CHECK(s.open("hello.txt", ot::IStorage::kRead) < 0);
    CHECK(!s.mkdir("x"));
    CHECK(s.listDir("", &Lister::fn, nullptr) == -1);
    CHECK(s.lockedAccess() == 4);
    s.setLocked(false);
    CHECK(s.exists("hello.txt"));
    CHECK(s.lockedAccess() == 4);

    // A file name with non-ASCII characters (UTF-8) round-trips.
    CHECK(writeFile(s, "Samples/caf\xc3\xa9.wav", "x") == false);   // no Samples yet
    CHECK(s.mkdir("Samples"));
    CHECK(writeFile(s, "Samples/caf\xc3\xa9.wav", "x"));
    CHECK(s.exists("samples/CAF\xc3\xa9.WAV"));   // ASCII letters fold, others match exactly

    std::printf("host-storage-check: %d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
