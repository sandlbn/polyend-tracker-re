// Sample Loader: long names, folders with more than 100 files, list marquee
// (public issue #54; spec 06 §5.2 "Large folders and long names", spec 20
// §1.6 / §3.5). The folder shapes follow releases/test-issue54/make_ot54.py:
// files are written one at a time, so the fake card's directory order
// (FakeStorage::creationOrder) is the creation order, and every name ends in
// cNNN = its creation number.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "formats/wav.h"
#include "ui/screens.h"
#include "ui/widgets.h"

using namespace ot;

namespace {

LoaderScreen& ld(Harness& h) { return *static_cast<LoaderScreen*>(h.app->ui.screen(kModLoader)); }

// A short mono WAV whose length tells the file apart: 32 + n frames.
std::vector<uint8_t> wavBytes(uint32_t frames) {
    std::vector<int16_t> s(frames);
    for (uint32_t i = 0; i < frames; ++i) s[i] = int16_t((i % 50) * 400 - 10000);
    std::vector<uint8_t> out(frames * 2 + 64);
    out.resize(wavEncode(s.data(), frames, 1, out.data(), out.size()));
    return out;
}

// make_ot54.py shuffled_names: "<word>_<abc>_cNNN.wav", alphabetical order
// unlike the creation order (own generator, same shape).
std::vector<std::string> shuffledNames(int count, uint32_t seed, const char* prefix = "") {
    static const char* kWords[] = {"kick", "snare", "hat",  "pad",   "bass", "lead",  "arp",   "vox",   "fx",    "loop",
                                   "perc", "tom",   "ride", "crash", "pluck", "stab", "chord", "drone", "noise", "bell"};
    uint32_t r = seed * 2654435761u + 12345u;
    auto next = [&] {
        r = r * 1664525u + 1013904223u;
        return r >> 8;
    };
    std::vector<std::string> v;
    for (int c = 1; c <= count; ++c) {
        std::string n = std::string(prefix) + kWords[next() % 20] + "_";
        for (int k = 0; k < 3; ++k) n += char('a' + next() % 26);
        char tail[16];
        std::snprintf(tail, sizeof(tail), "_c%03d.wav", c);
        v.push_back(n + tail);
    }
    return v;
}

int cmpNoCase(const std::string& a, const std::string& b) {
    for (size_t i = 0;; ++i) {
        char x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y || !x) return x - y;
    }
}
std::vector<std::string> sortedNoCase(std::vector<std::string> v) {
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return cmpNoCase(a, b) < 0; });
    return v;
}

void file(Harness& h, const std::string& path, uint32_t frames) { h.fs.addFile(path, wavBytes(frames)); }

std::string lenName(int n) {
    char stem[16];
    std::snprintf(stem, sizeof(stem), "len%03d_", n);
    return std::string(stem) + std::string(size_t(n) - std::strlen(stem) - 4, 'x') + ".wav";
}
const std::string kTwins = "Twins_same_beginning_same_beginning_same_beginning_same_beginning_";

// The folders of make_ot54.py under Samples/ (and, with root, 120 files at
// the card root).
struct Card54 {
    std::vector<std::string> big374, mixed150, inner250;
};
Card54 build54(Harness& h, bool root = false) {
    Card54 c;
    h.fs.creationOrder = true;
    const std::string s = "Samples";
    h.fs.mkdir(s.c_str());
    // 1. Long names.
    std::string d = s + "/OT54 1 Long names";
    h.fs.mkdir(d.c_str());
    for (int n : {39, 40, 41, 60, 100, 200}) file(h, d + "/" + lenName(n), uint32_t(n));
    file(h, d + "/" + kTwins + "END_A.wav", 300);
    file(h, d + "/" + kTwins + "END_B.wav", 301);
    // 2. Exactly 100 and 101 files.
    for (int count : {100, 101}) {
        d = s + "/OT54 2 Exactly " + std::to_string(count);
        h.fs.mkdir(d.c_str());
        int k = 0;
        for (auto& n : shuffledNames(count, uint32_t(count))) file(h, d + "/" + n, uint32_t(32 + ++k));
    }
    // 3. 374 files.
    d = s + "/OT54 3 Big 374";
    h.fs.mkdir(d.c_str());
    c.big374 = shuffledNames(374, 374);
    for (size_t i = 0; i < c.big374.size(); ++i) file(h, d + "/" + c.big374[i], uint32_t(32 + i + 1));
    // 4. 150 wav + 5 subfolders + 30 .txt, interleaved.
    d = s + "/OT54 4 Mixed 150 wav 5 dirs 30 txt";
    h.fs.mkdir(d.c_str());
    c.mixed150 = shuffledNames(150, 150);
    int k = 0;
    for (size_t i = 0; i < c.mixed150.size(); ++i) {
        file(h, d + "/" + c.mixed150[i], uint32_t(32 + i + 1));
        if (i % 30 == 15) {
            char sub[64];
            std::snprintf(sub, sizeof(sub), "/subfolder_%d_after_c%03d", ++k, int(i + 1));
            h.fs.mkdir((d + sub).c_str());
            file(h, d + sub + "/inside.wav", 77);
        }
        if (i % 5 == 0) {
            char txt[64];
            std::snprintf(txt, sizeof(txt), "/notes_after_c%03d.txt", int(i + 1));
            h.fs.addFile(d + txt, {'n', 'o'});
        }
    }
    // 5. 120 folders, no files.
    d = s + "/OT54 5 Folders only 120";
    h.fs.mkdir(d.c_str());
    for (int i = 1; i <= 120; ++i) {
        char sub[32];
        std::snprintf(sub, sizeof(sub), "/dir_c%03d", i);
        h.fs.mkdir((d + sub).c_str());
        file(h, d + sub + "/one.wav", 40);
    }
    // 6. Nested: 250 files one level deeper.
    h.fs.mkdir((s + "/OT54 6 Nested").c_str());
    d = s + "/OT54 6 Nested/inner 250";
    h.fs.mkdir(d.c_str());
    c.inner250 = shuffledNames(250, 250);
    for (size_t i = 0; i < c.inner250.size(); ++i) file(h, d + "/" + c.inner250[i], uint32_t(32 + i + 1));
    if (root)
        for (auto& n : shuffledNames(120, 120, "ROOT54_")) file(h, n, 50);
    return c;
}

std::string label(LoaderScreen& l, int i) {
    char b[wg::kListItemCap];
    l.entryLabel(i, b, sizeof(b));
    return b;
}
int findLabel(LoaderScreen& l, const std::string& want) {
    for (int i = 0; i < l.count; ++i)
        if (label(l, i) == want) return i;
    return -1;
}
// Cursor onto the entry with this label and Enter (F1).
bool enter(Harness& h, const std::string& want) {
    LoaderScreen& l = ld(h);
    const int i = findLabel(l, want);
    if (i < 0) return false;
    l.sdCursor = i;
    h.tap(btn::F1);
    return true;
}
bool openSamples(Harness& h) {
    h.tap(btn::SampleLoader);
    return enter(h, "/Samples");
}
std::vector<std::string> fileNames(LoaderScreen& l) {
    std::vector<std::string> v;
    for (int i = 0; i < l.count; ++i)
        if (!l.isDir[i]) v.push_back(l.entries[i]);
    return v;
}
int dirCount(LoaderScreen& l) {
    int n = 0;
    for (int i = 0; i < l.count; ++i) n += l.isDir[i] && std::strcmp(l.entries[i], "..") != 0;
    return n;
}

}  // namespace

// The parent lists a folder with more than 100 files as fixed 100-file
// ranges; exactly 100 files and folders of subfolders stay one entry.
TEST(loader54_parent_labels) {
    Harness h;
    build54(h);
    CHECK(openSamples(h));
    LoaderScreen& l = ld(h);
    const char* want[] = {"/..",
                          "/OT54 1 Long names",
                          "/OT54 2 Exactly 100",
                          "/OT54 2 Exactly 101 (1-100)",
                          "/OT54 2 Exactly 101 (101-200)",
                          "/OT54 3 Big 374 (1-100)",
                          "/OT54 3 Big 374 (101-200)",
                          "/OT54 3 Big 374 (201-300)",
                          "/OT54 3 Big 374 (301-400)",
                          "/OT54 4 Mixed 150 wav 5 dirs 30 txt (1-100)",
                          "/OT54 4 Mixed 150 wav 5 dirs 30 txt (101-200)",
                          "/OT54 5 Folders only 120",
                          "/OT54 6 Nested"};
    CHECK_EQ(l.count, int(sizeof(want) / sizeof(want[0])));
    for (int i = 0; i < l.count && i < int(sizeof(want) / sizeof(want[0])); ++i) CHECK(label(l, i) == std::string(want[i]));
    // Nested: the split happens at any depth.
    CHECK(enter(h, "/OT54 6 Nested"));
    CHECK_EQ(l.count, 4);
    CHECK(label(l, 1) == std::string("/inner 250 (1-100)"));
    CHECK(label(l, 3) == std::string("/inner 250 (201-300)"));
}

// Part k holds the files k*100+1..k*100+100 in directory (creation) order,
// sorted alphabetically within the part; the real folder path is kept.
TEST(loader54_part_contents) {
    Harness h;
    const Card54 c = build54(h);
    CHECK(openSamples(h));
    LoaderScreen& l = ld(h);
    CHECK(enter(h, "/OT54 3 Big 374 (101-200)"));
    CHECK_STR(l.dir, "Samples/OT54 3 Big 374");
    CHECK_EQ(l.part, 2);
    CHECK_EQ(l.count, 101);
    CHECK_STR(l.entries[0], "..");
    const auto want2 = sortedNoCase(std::vector<std::string>(c.big374.begin() + 100, c.big374.begin() + 200));
    CHECK(fileNames(l) == want2);
    h.tap(btn::Left);   // cursor to ".."
    h.tap(btn::F1);
    CHECK(enter(h, "/OT54 3 Big 374 (301-400)"));
    const auto want4 = sortedNoCase(std::vector<std::string>(c.big374.begin() + 300, c.big374.end()));
    CHECK_EQ(int(fileNames(l).size()), 74);
    CHECK(fileNames(l) == want4);
    // 101 files: the second part holds only the 101st file written.
    l.sdCursor = 0;
    h.tap(btn::F1);
    CHECK(enter(h, "/OT54 2 Exactly 101 (101-200)"));
    CHECK_EQ(l.count, 2);
    CHECK(std::strstr(l.entries[1], "_c101.wav") != nullptr);
    l.sdCursor = 0;
    h.tap(btn::F1);
    CHECK(enter(h, "/OT54 2 Exactly 100"));
    CHECK_EQ(l.part, 0);
    CHECK_EQ(l.count, 101);
}

// Subfolders never count and are listed (folders first) in part 1 only,
// including those created after the 100th file; other files are not listed.
TEST(loader54_subfolders_in_part1_only) {
    Harness h;
    const Card54 c = build54(h);
    CHECK(openSamples(h));
    LoaderScreen& l = ld(h);
    CHECK(enter(h, "/OT54 4 Mixed 150 wav 5 dirs 30 txt (1-100)"));
    CHECK_EQ(dirCount(l), 5);
    CHECK_EQ(l.count, 1 + 5 + 100);
    for (int i = 1; i <= 5; ++i) CHECK(l.isDir[i]);
    CHECK(label(l, 5) == std::string("/subfolder_5_after_c136"));
    CHECK(fileNames(l) == sortedNoCase(std::vector<std::string>(c.mixed150.begin(), c.mixed150.begin() + 100)));
    for (int i = 0; i < l.count; ++i) CHECK(std::strstr(l.entries[i], ".txt") == nullptr);
    l.sdCursor = 0;
    h.tap(btn::F1);
    CHECK(enter(h, "/OT54 4 Mixed 150 wav 5 dirs 30 txt (101-200)"));
    CHECK_EQ(dirCount(l), 0);
    CHECK_EQ(l.count, 1 + 50);
    CHECK(fileNames(l) == sortedNoCase(std::vector<std::string>(c.mixed150.begin() + 100, c.mixed150.end())));
    // A folder of 120 subfolders (no files) is one entry; inside, the usual
    // 101-name limit.
    l.sdCursor = 0;
    h.tap(btn::F1);
    CHECK(enter(h, "/OT54 5 Folders only 120"));
    CHECK_EQ(l.part, 0);
    CHECK_EQ(l.count, 101);
    CHECK_EQ(dirCount(l), 100);
}

// ".." from a part (and from a folder entered from a part) returns to the
// listing it came from with the cursor on the entry.
TEST(loader54_back_to_part_entry) {
    Harness h;
    build54(h);
    CHECK(openSamples(h));
    LoaderScreen& l = ld(h);
    const int big2 = findLabel(l, "/OT54 3 Big 374 (101-200)");
    CHECK(big2 > 0);
    CHECK(enter(h, "/OT54 3 Big 374 (101-200)"));
    h.tap(btn::Down);
    h.tap(btn::Down);
    l.sdCursor = 0;
    h.tap(btn::F1);   // ".."
    CHECK_STR(l.dir, "Samples");
    CHECK_EQ(l.part, 0);
    CHECK_EQ(l.sdCursor, big2);
    CHECK(label(l, l.sdCursor) == std::string("/OT54 3 Big 374 (101-200)"));
    // Two levels: part 1 of Mixed -> its third subfolder -> back twice.
    const int mixed1 = findLabel(l, "/OT54 4 Mixed 150 wav 5 dirs 30 txt (1-100)");
    CHECK(enter(h, "/OT54 4 Mixed 150 wav 5 dirs 30 txt (1-100)"));
    const int sub3 = findLabel(l, "/subfolder_3_after_c076");
    CHECK(sub3 > 0);
    CHECK(enter(h, "/subfolder_3_after_c076"));
    CHECK_STR(l.dir, "Samples/OT54 4 Mixed 150 wav 5 dirs 30 txt/subfolder_3_after_c076");
    CHECK_EQ(l.part, 0);
    CHECK_EQ(l.count, 2);
    l.sdCursor = 0;
    h.tap(btn::F1);
    CHECK_EQ(l.part, 1);
    CHECK_EQ(l.sdCursor, sub3);
    CHECK_EQ(l.count, 106);
    l.sdCursor = 0;
    h.tap(btn::F1);
    CHECK_EQ(l.sdCursor, mixed1);
    // Opening and closing the loader keeps the part.
    CHECK(enter(h, "/OT54 3 Big 374 (201-300)"));
    h.tap(btn::Pattern);
    h.tap(btn::SampleLoader);
    CHECK_EQ(l.part, 3);
    CHECK_EQ(l.count, 101);
}

// Add / Add next / Preview / Import inside a part use the real folder path.
TEST(loader54_add_from_part) {
    Harness h;
    App& a = *h.app;
    const Card54 c = build54(h);
    CHECK(openSamples(h));
    LoaderScreen& l = ld(h);
    CHECK(enter(h, "/OT54 3 Big 374 (201-300)"));
    const auto part3 = sortedNoCase(std::vector<std::string>(c.big374.begin() + 200, c.big374.begin() + 300));
    // Fifth file of the part; its creation number gives the frame count.
    l.sdCursor = 5;
    CHECK_STR(l.entries[5], part3[4].c_str());
    const int created = int(std::find(c.big374.begin(), c.big374.end(), part3[4]) - c.big374.begin()) + 1;
    h.tap(btn::F1);
    if (a.ui.askOpen()) h.tap(btn::F8);
    CHECK(a.instrPresent[0]);
    CHECK_EQ(int(a.bank.frames(0)), 32 + created);
    const std::string stem = part3[4].substr(0, part3[4].size() - 4);
    CHECK_STR(a.instruments[0].name, stem.c_str());
    // Add next: two marked files into slots 2 and 3, cursor moves on.
    h.tap(btn::Right);
    h.tap(btn::Down);   // instrument 2
    h.tap(btn::Left);
    l.sdCursor = 10;
    h.down(btn::Shift);
    h.tap(btn::Down);
    h.up(btn::Shift);
    h.tap(btn::F2);
    if (a.ui.askOpen()) h.tap(btn::F8);
    CHECK(a.instrPresent[1] && a.instrPresent[2]);
    CHECK(std::string(a.instruments[1].name) == part3[9].substr(0, std::min<size_t>(31, part3[9].size() - 4)));
    CHECK(std::string(a.instruments[2].name) == part3[10].substr(0, std::min<size_t>(31, part3[10].size() - 4)));
    CHECK_EQ(l.insCursor, 3);
    // Preview streams the file from its real path.
    l.sdCursor = 20;
    h.down(btn::F3);
    CHECK(a.engine.previewBufferActive());
    h.up(btn::F3);
    // Import (F5 -> trim -> F8 Normal) into slot 4 (a plain move clears
    // the marks).
    l.sdCursor = 29;
    h.tap(btn::Down);
    const int created30 = int(std::find(c.big374.begin(), c.big374.end(), part3[29]) - c.big374.begin()) + 1;
    h.tap(btn::F5);
    CHECK(l.trim);
    h.tap(btn::F8);
    CHECK(!l.trim);
    CHECK(a.instrPresent[3]);
    CHECK_EQ(int(a.bank.frames(3)), 32 + created30);
}

// Names of any length are listed and load (spec 06 §5.2: 39..200 tested).
TEST(loader54_long_names) {
    Harness h;
    App& a = *h.app;
    build54(h);
    CHECK(openSamples(h));
    LoaderScreen& l = ld(h);
    CHECK(enter(h, "/OT54 1 Long names"));
    CHECK_EQ(l.count, 1 + 8);
    int i200 = -1;
    for (int i = 0; i < l.count; ++i)
        if (std::strlen(l.entries[i]) == 200) i200 = i;
    CHECK(i200 > 0);
    CHECK(findLabel(l, kTwins + "END_A.wav") > 0);
    CHECK(findLabel(l, kTwins + "END_B.wav") > 0);
    l.sdCursor = i200;
    h.tap(btn::F1);
    CHECK(a.instrPresent[0]);
    CHECK_EQ(int(a.bank.frames(0)), 200);
    CHECK(std::string(a.instruments[0].name) == lenName(200).substr(0, 31));
    // The twins load as two different files.
    l.insCursor = 1;
    l.sdCursor = findLabel(l, kTwins + "END_B.wav");
    h.tap(btn::F1);
    CHECK_EQ(int(a.bank.frames(1)), 301);
}

// The card root has no parent and is not split: at most 101 names.
TEST(loader54_root_caps) {
    Harness h;
    build54(h, true);
    h.tap(btn::SampleLoader);
    LoaderScreen& l = ld(h);
    while (l.depth() > 0) {
        l.sdCursor = 0;
        h.tap(btn::F1);
    }
    l.list();
    CHECK(l.count <= LoaderScreen::kMaxUnsplit);
    CHECK(l.count >= 100);
    CHECK_EQ(l.part, 0);
    int parts = 0;
    for (int i = 0; i < l.count; ++i) parts += l.entryPart(i) ? 1 : 0;
    CHECK_EQ(parts, 0);   // Samples itself holds 6 folders, no files
}

// Marquee formula (spec 20 §1.6): offset t^2/30 for 10 ticks, then linear,
// a 30-px gap before the repeat; only the selected row wider than its
// column scrolls, the others stay cut at the column edge.
TEST(loader54_marquee_widget) {
    CHECK_EQ(wg::marqueeOffset(0), 0);
    CHECK_EQ(wg::marqueeOffset(3), 0);     // 9/30
    CHECK_EQ(wg::marqueeOffset(6), 1);     // 36/30
    CHECK_EQ(wg::marqueeOffset(10), 3);    // 100/30
    CHECK_EQ(wg::marqueeOffset(11), 4);    // (220-100)/30
    CHECK_EQ(wg::marqueeOffset(40), 23);   // (800-100)/30
    static const std::string kLongItem = "A selected sample name that is far wider than the list column";
    static const std::string kLongOther = "An unselected sample name that is also wider than the column";
    RecordingDisplay d;
    wg::ListView lv;
    lv.x = 1;
    lv.y = 61;
    lv.w = 297;
    lv.h = 362;
    lv.count = 3;
    lv.sel = 1;
    lv.selStyle = wg::ListSel::kRed;
    lv.item = [](void*, int i, char* o, int cap) {
        std::snprintf(o, size_t(cap), "%s", i == 1 ? kLongItem.c_str() : i == 0 ? kLongOther.c_str() : "short");
    };
    const int innerW = 297 - 6, left = 1 + 3, right = left + innerW;
    const int tw = textWidth(lv.font, kLongItem.c_str());
    CHECK(1 + 13 + tw > right);
    auto rowTexts = [&](int row) {
        std::vector<std::pair<int, std::string>> v;
        char want[16];
        std::snprintf(want, sizeof(want), " %d f", 61 + 17 + 27 * row);
        for (auto& s : d.last) {
            int x = 0;
            if (s.rfind("TEXT ", 0) != 0 || s.find(want) == std::string::npos) continue;
            std::sscanf(s.c_str(), "TEXT %d", &x);
            const size_t q = s.find('"');
            v.push_back({x, s.substr(q + 1, s.size() - q - 2)});
        }
        return v;
    };
    const uint32_t t0 = 5000;
    bool wrapped = false;
    for (uint32_t t = 0; t <= 800; ++t) {
        wg::marqueeFrame(t0 + t * wg::kMarqueeTickMs);
        Gfx g(d);
        g.beginFrame(0);
        wg::list(g, theme(0), lv);
        g.endFrame();
        CHECK(wg::marqueeRunning());
        const int period = tw + wg::kMarqueeGap;
        const int off = wg::marqueeOffset(t) % period;
        wrapped |= wg::marqueeOffset(t) >= period;
        // Every drawn run is a piece of the item at its scrolled position,
        // overlapping the column, starting inside or at the left glyph edge.
        auto runs = rowTexts(1);
        CHECK(!runs.empty());
        for (auto& r : runs) {
            bool ok = false;
            for (int copy = 0; copy < 2 && !ok; ++copy)
                for (size_t i = 0; i < kLongItem.size() && !ok; ++i)
                    if (kLongItem.compare(i, r.second.size(), r.second) == 0 &&
                        r.first == 1 + 13 - off + copy * period + textWidth(lv.font, kLongItem.c_str(), int(i)))
                        ok = true;
            CHECK(ok);
            CHECK(r.first < right);
            CHECK(r.first + textWidth(lv.font, r.second.substr(0, 1).c_str()) > left);
        }
        if (t == 0) CHECK(runs.size() == 1 && runs[0].first == 14);
        // The unselected wide row stays cut at x 14, its last glyph starting
        // inside the column.
        auto other = rowTexts(0);
        CHECK(other.size() == 1 && other[0].first == 14);
        CHECK(kLongOther.compare(0, other[0].second.size(), other[0].second) == 0);
        CHECK(other[0].second.size() < kLongOther.size());
        CHECK(14 + textWidth(lv.font, other[0].second.c_str(), int(other[0].second.size()) - 1) < right);
        CHECK(rowTexts(2).size() == 1);
    }
    CHECK(wrapped);
    // A new selection restarts at offset 0; a short selected item does not
    // run the marquee.
    lv.sel = 2;
    wg::marqueeFrame(t0 + 900 * wg::kMarqueeTickMs);
    {
        Gfx g(d);
        g.beginFrame(0);
        wg::list(g, theme(0), lv);
        g.endFrame();
    }
    CHECK(!wg::marqueeRunning());
    lv.sel = 1;
    wg::marqueeFrame(t0 + 901 * wg::kMarqueeTickMs);
    {
        Gfx g(d);
        g.beginFrame(0);
        wg::list(g, theme(0), lv);
        g.endFrame();
    }
    CHECK(rowTexts(1).size() == 1 && rowTexts(1)[0].first == 14);
}

// In the Sample Loader the selected 200-character name scrolls: frames keep
// coming while it is selected and stop on a short name.
TEST(loader54_marquee_redraws_only_while_needed) {
    Harness h;
    build54(h);
    CHECK(openSamples(h));
    LoaderScreen& l = ld(h);
    CHECK(enter(h, "/OT54 1 Long names"));
    // Select "/.." (fits), then wait: no frames.
    const int shortI = 0;
    int longI = -1;
    for (int i = 1; i < l.count; ++i)
        if (std::strlen(l.entries[i]) == 200) longI = i;
    CHECK(longI > 0);
    l.sdCursor = 1;
    h.tap(btn::Up);
    h.run(500);   // the memory projection runs 300 ms after a move
    int s0 = h.disp.swaps;
    h.run(1000);
    CHECK_EQ(h.disp.swaps, s0);
    // The long one: frames about every tick, and the row's text moves.
    while (l.sdCursor < longI) h.tap(btn::Down);
    const int row = longI - wg::listFollow(longI, l.count, wg::listVisibleRows(362), 0);
    char want[16];
    std::snprintf(want, sizeof(want), " %d f", 61 + 17 + 27 * row);
    auto firstX = [&] {
        for (auto& s : h.disp.last) {
            int x = 0;
            if (s.rfind("TEXT ", 0) == 0 && s.find(want) != std::string::npos && s.find("00_xxx") != std::string::npos &&
                std::sscanf(s.c_str(), "TEXT %d", &x) == 1)
                return x;
        }
        return 9999;
    };
    CHECK_EQ(firstX(), 14);
    s0 = h.disp.swaps;
    h.run(1000);
    CHECK(h.disp.swaps - s0 >= 1000 / int(wg::kMarqueeTickMs) - 2);
    CHECK(firstX() < 14);   // scrolled
    // Back to the short name: the frames stop.
    while (l.sdCursor > shortI) h.tap(btn::Up);
    h.run(500);
    s0 = h.disp.swaps;
    h.run(1000);
    CHECK_EQ(h.disp.swaps, s0);
}
