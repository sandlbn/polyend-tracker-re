// Host filesystem containment regression checks (no SDL dependency).
#include "../sim/src/host_storage.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
int main() {
    char tmp[] = "/tmp/ot-storage-XXXXXX";
    assert(mkdtemp(tmp));
    const fs::path base(tmp), root = base / "sd", outside = base / "outside";
    fs::create_directories(root / "Projects");
    fs::create_directory(outside);
    std::ofstream(outside / "victim") << "keep";
    fs::create_directory_symlink(outside, root / "Escape");
    fs::create_symlink(outside / "victim", root / "FileLink");
    fs::create_symlink(outside / "missing", root / "Dangling");
    {
        sim::HostStorage s;
        assert(s.init(root.string()));
        for (const char* p : {"/Escape/victim", "/escape/victim", "/FileLink", "/filelink", "/Dangling", "/../outside/victim"}) {
            assert(!s.exists(p));
            assert(s.open(p, ot::IStorage::kRead) < 0);
            assert(s.open(p, ot::IStorage::kWrite) < 0);
            assert(s.open(p, ot::IStorage::kReadWrite) < 0);
            assert(!s.remove(p));
            assert(!s.rename(p, "/moved"));
        }
        assert(!s.mkdir("/escape/new"));
        assert(!s.rename("/Projects", "/escape/moved"));
        assert(s.open("/Escape", ot::IStorage::kWrite) < 0);
        int count = 0;
        assert(s.listDir("/", [](const ot::IStorage::DirEntry& e, void* ctx) {
            assert(std::string(e.name) == "Projects");
            ++*static_cast<int*>(ctx);
            return true;
        }, &count) == 1);
        assert(count == 1);
        assert(s.listDir("/escape", nullptr, nullptr) < 0);
        assert(s.mkdir("/projects/New"));
        auto h = s.open("/PROJECTS/new/Test", ot::IStorage::kWrite);
        assert(h >= 0);
        assert(s.write(h, "abc", 3) == 3);
        s.close(h);
        h = s.open("/projects/NEW/test", ot::IStorage::kReadWrite);
        char buf[4] = {};
        assert(h >= 0 && s.read(h, buf, 3) == 3 && std::string(buf) == "abc");
        s.close(h);
        assert(s.rename("/projects/new/Test", "/PROJECTS/NEW/Test"));
        assert(!s.rename("/projects/new/test", "/FileLink"));
        assert(s.rename("/projects/new/test", "/Projects/New/Renamed"));
        assert(s.remove("/projects/new/renamed"));
        assert(s.remove("/projects/new"));
        assert(!s.remove("/"));
        // Replacing a previously valid directory with a symlink must be rejected.
        fs::remove(root / "Projects");
        fs::create_directory_symlink(outside, root / "Projects");
        assert(s.open("/Projects/victim", ot::IStorage::kWrite) < 0);
    }
    // FAT semantics on a plain card folder: a name keeps the case it was
    // created with (later lookups in any case find it, listings show it),
    // listings are sorted, and a rename onto an existing name is refused
    // (FatFs f_rename), also when that name is spelled in another case.
    {
        sim::HostStorage s;
        assert(s.init((base / "fat").string()));
        const auto put = [&](const char* p, const char* text) {
            const auto h = s.open(p, ot::IStorage::kWrite);
            if (h < 0) return false;
            const int32_t n = s.write(h, text, uint32_t(std::strlen(text)));
            s.close(h);
            return n == int32_t(std::strlen(text));
        };
        const auto get = [&](const char* p) {
            const auto h = s.open(p, ot::IStorage::kRead);
            if (h < 0) return std::string("<none>");
            char buf[64] = {};
            const int32_t n = s.read(h, buf, sizeof(buf) - 1);
            s.close(h);
            return n >= 0 ? std::string(buf, size_t(n)) : std::string("<error>");
        };
        const auto list = [&](const char* p) {
            std::vector<std::string> names;
            const int32_t n = s.listDir(p, [](const ot::IStorage::DirEntry& e, void* ctx) {
                static_cast<std::vector<std::string>*>(ctx)->push_back(e.name);
                return true;
            }, &names);
            assert(n == int32_t(names.size()));
            return names;
        };
        assert(s.mkdir("/Projects"));
        assert(s.mkdir("/projects"));   // the same folder
        assert(s.mkdir("/PROJECTS/Song"));
        assert(put("/projects/song/X.TXT", "case"));
        assert(get("/Projects/Song/x.txt") == "case");
        assert(put("/b.txt", "B"));
        assert(put("/a.txt", "A"));
        assert(put("/B.TXT", "b2"));   // the existing b.txt, rewritten
        assert((list("/") == std::vector<std::string>{"Projects", "a.txt", "b.txt"}));
        assert((list("/projects/SONG") == std::vector<std::string>{"X.TXT"}));
        assert(!s.rename("/a.txt", "/b.txt"));
        assert(!s.rename("/a.txt", "/B.TXT"));
        assert(s.mkdir("/Other"));
        assert(!s.rename("/Other", "/PROJECTS"));   // folders too
        assert(s.remove("/other"));
        assert(get("/a.txt") == "A" && get("/b.txt") == "b2");
        assert(s.rename("/a.txt", "/c.txt"));
        assert(!s.exists("/a.txt") && get("/C.TXT") == "A");
        assert((list("/") == std::vector<std::string>{"Projects", "b.txt", "c.txt"}));
    }
    std::ifstream in(outside / "victim");
    std::string content;
    in >> content;
    assert(content == "keep");
    assert(!fs::exists(outside / "missing"));
    fs::remove_all(base);
}
