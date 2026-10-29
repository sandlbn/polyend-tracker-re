// Host filesystem containment regression checks (no SDL dependency).
#include "../sim/src/host_storage.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

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
    std::ifstream in(outside / "victim");
    std::string content;
    in >> content;
    assert(content == "keep");
    assert(!fs::exists(outside / "missing"));
    fs::remove_all(base);
}
