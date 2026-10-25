// Public issue #55: deleting a folder tree (File > delete project, New
// Project's Workspace clear) with very deep or long paths. The card takes
// paths up to IStorage::kMaxPath characters; a longer path fails instead of
// being cut (a cut path names another file or the folder itself).
// removeTree checks the whole tree first and refuses before deleting
// anything; it walks the tree without recursion.
#include <string>

#include "test.h"
#include "ui_fakes.h"

using namespace ot;

namespace {
void addFile(FakeStorage& fs, const std::string& p) { fs.files[p] = std::vector<uint8_t>{1, 2, 3}; }
bool anyUnder(const FakeStorage& fs, const std::string& root) {
    const std::string pre = root + "/";
    for (auto& f : fs.files)
        if (f.first.compare(0, pre.size(), pre) == 0) return true;
    for (auto& d : fs.dirs)
        if (d == root || d.compare(0, pre.size(), pre) == 0) return true;
    return false;
}
}  // namespace

TEST(remove_tree_deletes_a_deep_tree) {
    // 120 levels of one-letter folders (path 251 characters), files on the
    // way: all gone, with constant stack (no recursion).
    Harness h;
    FakeStorage& fs = h.fs;
    fs.dirs.insert("Projects/p");
    std::string p = "Projects/p";
    for (int i = 0; i < 120; ++i) {
        p += "/a";
        fs.dirs.insert(p);
        if (i % 10 == 0) addFile(fs, p + "/f.bin");
    }
    addFile(fs, "Projects/p/project.mt");
    CHECK(p.size() <= IStorage::kMaxPath);
    CHECK(h.app->removeTree("Projects/p"));
    CHECK(!anyUnder(fs, "Projects/p"));
    CHECK(fs.dirs.count("Projects"));   // the parent stays
}

TEST(remove_tree_refuses_a_too_long_path_before_deleting) {
    // A computer made a folder whose child path is longer than the card's
    // limit: nothing is deleted, not even the files that come first.
    Harness h;
    FakeStorage& fs = h.fs;
    fs.dirs.insert("Projects/p");
    addFile(fs, "Projects/p/a_first.mt");
    fs.dirs.insert("Projects/p/instruments");
    addFile(fs, "Projects/p/instruments/instrument_01.pti");
    std::string d = "Projects/p/zz";
    fs.dirs.insert(d);
    while (d.size() <= IStorage::kMaxPath) {   // the last folder is already too long
        d += "/" + std::string(40, 'x');
        fs.dirs.insert(d);
    }
    addFile(fs, d + "/" + std::string(60, 'y') + ".wav");
    CHECK(d.size() > IStorage::kMaxPath);
    const size_t files = fs.files.size(), dirs = fs.dirs.size();
    CHECK(!h.app->removeTree("Projects/p"));
    CHECK_EQ(fs.files.size(), files);
    CHECK_EQ(fs.dirs.size(), dirs);
    CHECK(fs.files.count("Projects/p/a_first.mt"));
}

TEST(remove_tree_deletes_long_file_names) {
    // FAT long names reach 255 characters; a name cut at 95 named another
    // file (or none) and the delete failed.
    Harness h;
    FakeStorage& fs = h.fs;
    fs.dirs.insert("Projects/p");
    addFile(fs, "Projects/p/" + std::string(200, 'n') + ".wav");
    addFile(fs, "Projects/p/project.mt");
    CHECK(h.app->removeTree("Projects/p"));
    CHECK(!anyUnder(fs, "Projects/p"));
}

TEST(remove_tree_on_a_missing_or_plain_path) {
    Harness h;
    FakeStorage& fs = h.fs;
    CHECK(h.app->removeTree("Projects/none"));   // nothing to do
    addFile(fs, "Projects/file.bin");
    CHECK(h.app->removeTree("Projects/file.bin"));   // a file: removed, as before
    CHECK(!fs.files.count("Projects/file.bin"));
}

TEST(storage_refuses_paths_over_the_limit) {
    // The fake card follows the device: a longer path fails, never cut.
    FakeStorage fs;
    std::string p = "Projects/" + std::string(IStorage::kMaxPath, 'q');
    fs.dirs.insert("Projects");
    CHECK(!fs.mkdir(p.c_str()));
    CHECK(fs.open(p.c_str(), IStorage::kWrite) < 0);
}
