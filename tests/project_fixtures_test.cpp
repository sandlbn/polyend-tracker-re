// Regression test over every project on the user's SD-card backup
// (fixtures/sdcard-backup-2026-10-15/Projects, read in place, never copied
// into the repo; skipped when absent). Field bug: "Legowelt - Tracker Demo"
// opened with empty patterns and the open blocked the main loop for 8 s.
//
// For each project: open it through the background job (spec 05 appendix E3)
// one main-loop pass at a time, check that every pass does a bounded amount
// of card I/O, that every pattern has the same number of non-empty steps as
// a direct read of its file, that the reloaded Workspace (boot path) agrees,
// and that playing the current pattern makes sound when it should.
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "app/project_io.h"
#include "formats/bytes.h"
#include "formats/crc32.h"
#include "formats/file_io.h"
#include "formats/patch_pb.h"
#include "formats/pattern_mtp.h"
#include "ot_profile.h"
#if defined(OT_MOD_ACD1) && defined(OT_MOD_FAT1) && defined(OT_MOD_VAP1) && defined(OT_MOD_PRC1) && defined(OT_MOD_WTF1)
#define OT_SYNTH_PACK 1
#else
#define OT_SYNTH_PACK 0
#endif

using namespace ot;
namespace sfs = std::filesystem;

namespace {

std::string backupRoot() {
    if (const char* e = std::getenv("OT_LEGACY_FIXTURES")) return std::string(e) + "/sdcard-backup-2026-10-15";
    if (const char* e = std::getenv("OT_FIXTURES")) return std::string(e) + "/../../../fixtures/sdcard-backup-2026-10-15";
    return "../../fixtures/sdcard-backup-2026-10-15";
}

// Storage that counts the bytes moved since the last reset and, when given
// the clock, advances it like a card would (100 us per call + 20 bytes/us),
// so the job's time budget (kJobPassUs) is what bounds a pass.
struct MeteredStorage : FakeStorage {
    uint64_t io = 0;
    std::set<std::string> written;   // files opened for writing (or renamed from such a file)
    Handle open(const char* path, OpenMode mode) override {
        if (mode != kRead) written.insert(path);
        return FakeStorage::open(path, mode);
    }
    bool rename(const char* from, const char* to) override {
        const bool ok = FakeStorage::rename(from, to);
        if (ok && written.count(from)) written.insert(to);
        return ok;
    }
    FakeClock* clock = nullptr;
    double us = 0;
    void spend(int32_t r) {
        if (r > 0) io += uint32_t(r);
        if (!clock) return;
        us += 100.0 + (r > 0 ? r / 20.0 : 0.0);
        while (us >= 1000.0) {
            ++clock->ms;
            us -= 1000.0;
        }
    }
    int32_t read(Handle h, void* d, uint32_t n) override {
        const int32_t r = FakeStorage::read(h, d, n);
        spend(r);
        return r;
    }
    int32_t write(Handle h, const void* s, uint32_t n) override {
        const int32_t r = FakeStorage::write(h, s, n);
        spend(r);
        return r;
    }
};

std::vector<uint8_t> readHostFile(const sfs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Non-empty steps of tracks 1-12 of a pattern file read straight from its
// bytes (spec 02 §4.2: note 0xFF and both FX types 0 = empty; legacy
// 6184-byte files hold 8 tracks, the upgrade adds empty ones).
int rawNonEmpty(const std::vector<uint8_t>& f) {
    const int tracks = f.size() >= mtp::kStockFileSize ? kStockTrackCount : kLegacyTrackCount;
    int n = 0;
    for (int t = 0; t < tracks; ++t)
        for (int s = 0; s < kStepCount; ++s) {
            const size_t o = mtp::kTracksOff + size_t(t) * mtp::kTrackSize + 1 + size_t(s) * mtp::kStepSize;
            if (o + 5 >= f.size()) continue;
            if (f[o] != 0xFF || f[o + 2] != 0 || f[o + 4] != 0) ++n;
        }
    return n;
}
// Track t of a pattern file image is the empty track (every step empty).
bool rawTrackEmpty(const std::vector<uint8_t>& f, int t) {
    for (int s = 0; s < kStepCount; ++s) {
        const size_t o = mtp::kTracksOff + size_t(t) * mtp::kTrackSize + 1 + size_t(s) * mtp::kStepSize;
        if (f[o] != 0xFF || f[o + 1] || f[o + 2] || f[o + 3] || f[o + 4] || f[o + 5]) return false;
    }
    return true;
}

template <typename P>   // PatternStock (store) or Pattern (working, 16 tracks)
int modelNonEmpty(const P& p) {
    int n = 0;
    for (const Track& t : p.tracks)
        for (const Step& s : t.steps)
            if (s.note != -1 || s.fx[0].type != 0 || s.fx[1].type != 0) ++n;
    return n;
}

// A sample note on an unmuted track with a loaded instrument (the pattern
// should then make sound).
bool expectsAudio(App& a, const Pattern& p) {
    for (int t = 0; t < 8; ++t) {
        if (a.trackMuted(t)) continue;
        const Track& tr = p.tracks[t];
        for (int s = 0; s <= tr.lastStep && s < kStepCount; ++s) {
            const Step& st = tr.steps[s];
            if (st.note >= 0 && st.instrument < kSampleInstrumentCount && a.instrPresent[st.instrument]) return true;
        }
    }
    return false;
}

std::string stockProjectRoot() {
    if (const char* e = std::getenv("OT_LEGACY_FIXTURES")) return std::string(e) + "/stock-project";
    if (const char* e = std::getenv("OT_FIXTURES")) return std::string(e) + "/../../../fixtures/stock-project";
    return "../../fixtures/stock-project";
}

// Files of the card image under `dir` (relative path -> bytes).
std::map<std::string, std::vector<uint8_t>> filesUnder(const FakeStorage& fs, const std::string& dir) {
    std::map<std::string, std::vector<uint8_t>> out;
    const std::string pre = dir + "/";
    for (const auto& kv : fs.files)
        if (kv.first.compare(0, pre.size(), pre) == 0) out[kv.first.substr(pre.size())] = kv.second;
    return out;
}
bool anyUnder(const FakeStorage& fs, const std::string& dir) {
    const std::string pre = dir + "/";
    for (const auto& kv : fs.files)
        if (kv.first.compare(0, pre.size(), pre) == 0) return true;
    for (const auto& d : fs.dirs)
        if (d == dir || d.compare(0, pre.size(), pre) == 0) return true;
    return false;
}

// Open -> background open checks -> Save for every project under
// `projects` (Tracker Mini format, docs/MINI_PLAN.md §1): project.mt, the
// instruments and patternsMetadata are byte-identical after the save; every
// .mtp becomes a 0x3030 file whose tracks 1-12 are the original bytes (a
// current 12-track file) and whose tracks 13-16 are empty (8-track or
// pre-v5 legacy files: tracks 9-16 reset); the three N.patch files appear.
// project.mt after an Open -> Save is the original, except for the one
// documented OpenTracker fill: a project that never stored recorder settings
// (0x134..0x139 all zero) gets gain 50 / monitor on (byte 0) written back (README
// "Recorder source ... saved per project"), and the CRC at 0x910 follows.
bool projectMtSame(const std::vector<uint8_t>& o, const std::vector<uint8_t>& m) {
    if (o.size() != m.size() || m.size() != mt::kFileSize) return o == m;
    const bool recUnset = std::all_of(o.begin() + 0x134, o.begin() + 0x13A, [](uint8_t b) { return b == 0; });
    for (size_t i = 0; i < m.size(); ++i) {
        if (m[i] == o[i]) continue;
        if (recUnset && ((i >= 0x134 && i <= 0x139) || i >= mt::kCrcOff)) continue;
        return false;
    }
    if (recUnset) {
        const uint8_t rec[6] = {0, 50, 50, 50, 50, 0};   // 0x139: stock polarity, 0 = monitor On (spec 22 §5)
        if (std::memcmp(m.data() + 0x134, rec, 6) != 0) return false;
    }
    return rdU32(m.data() + mt::kCrcOff) == crc32(m.data(), mt::kCrcOff);
}

// Only files the save actually wrote count as "same": a file the save left
// alone (a WAV in samples/) proves nothing.
// `last` (optional) receives the folder before / after the save of the last
// project and the files the save wrote (paths relative to the folder).
struct SaveRecord {
    std::map<std::string, std::vector<uint8_t>> before, after;
    std::set<std::string> written;
};
// Returns the number of projects opened.
int openSaveProjects(const sfs::path& projects, int& audible, int& saved, SaveRecord* last = nullptr) {
    std::error_code ec;
    std::vector<sfs::path> dirs;
    for (auto it = sfs::recursive_directory_iterator(projects, ec); it != sfs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().filename() == "project.mt") dirs.push_back(it->path().parent_path());
    std::sort(dirs.begin(), dirs.end());
    CHECK(!dirs.empty());
    int opened = 0;
    for (const sfs::path& dir : dirs) {
        const std::string rel = "Projects/" + sfs::relative(dir, projects).generic_string();
        Harness h(false, 0xB40000 / 2);   // device sample pool (spec 02 §8.2)
        MeteredStorage ms;
        HalRefs hr = h.app->hal();
        hr.storage = &ms;
        App& a = *h.app;
        CHECK(a.init(hr, a.mem()));
        CHECK(a.boot());
        // Card image: the project folder (and its parents).
        for (size_t p = rel.find('/'); p != std::string::npos; p = rel.find('/', p + 1)) ms.dirs.insert(rel.substr(0, p));
        ms.dirs.insert(rel);
        int expected[kPatternCount + 1] = {};
        for (auto it = sfs::recursive_directory_iterator(dir, ec); it != sfs::recursive_directory_iterator(); it.increment(ec)) {
            const std::string r = rel + "/" + sfs::relative(it->path(), dir).generic_string();
            if (it->is_directory()) {
                ms.dirs.insert(r);
                continue;
            }
            ms.files[r] = readHostFile(it->path());
            const std::string fn = it->path().filename().string();
            int num = 0;
            if (it->path().parent_path().filename() == "patterns" && std::sscanf(fn.c_str(), "pattern_%d.mtp", &num) == 1 &&
                num >= 1 && num <= kPatternCount)
                expected[num] = rawNonEmpty(ms.files[r]);
        }
        const std::string name = dir.filename().string();
        const std::string folder = rel.substr(0, rel.size() - name.size() - 1);
        if (!projectFitsMemory(a, folder.c_str(), name.c_str())) {
            std::printf("  %-40s too big for the sample pool, skipped\n", name.c_str());
            continue;
        }
        if (name.size() > mt::kNameMax) {
            // spec 05 appendix E3 OPEN (0x1d2e0): names over 32 chars are refused.
            CHECK(!a.startOpenProject(folder.c_str(), name.c_str()));
            std::printf("  %-40s name > 32 chars: refused like stock\n", name.c_str());
            continue;
        }
        // Background open, one main-loop pass at a time.
        h.tap(btn::Pattern);
        ms.clock = &h.clock;
        CHECK(a.startOpenProject(folder.c_str(), name.c_str()));
        int passes = 0;
        uint64_t maxIo = 0;
        bool popup = false;
        while (a.jobBusy() && passes < 200000) {
            ms.io = 0;
            h.clock.ms += 1;
            a.loop();
            ++passes;
            if (ms.io > maxIo) maxIo = ms.io;
            popup = popup || h.disp.lastHasText("Opening project");
        }
        ms.clock = nullptr;
        CHECK(!a.jobBusy());
        CHECK(a.lastJobOk());
        CHECK(a.lastJobErrors() == 0);
        CHECK(std::strcmp(a.project.name, name.c_str()) == 0);
        // spec 05 §1.3: bounded work per pass: kJobPassUs of card time
        // (8 ms x 20 B/us = 160 KB) plus at most one unit past the budget.
        CHECK(maxIo <= kJobPassBytes);
        CHECK(passes == 1 || popup);
        int bad = 0, steps = 0;
        for (int n = 1; n <= kPatternCount; ++n) {
            const int got = modelNonEmpty(a.stored(n));
            if (got != expected[n]) {
                if (!bad) std::printf("  %s pattern %d: %d non-empty steps, file has %d\n", name.c_str(), n, got, expected[n]);
                ++bad;
            }
            steps += got;
        }
        CHECK(bad == 0);
        CHECK(modelNonEmpty(a.pattern()) == expected[a.patternNumber()]);
        // Boot path: the same Workspace loads to the same patterns.
        CHECK(a.loadWorkspace());
        CHECK(a.lastJobErrors() == 0);
        int bad2 = 0;
        for (int n = 1; n <= kPatternCount; ++n) bad2 += modelNonEmpty(a.stored(n)) != expected[n];
        CHECK(bad2 == 0);
        // Audio: play the current pattern.
        int peak = 0;
        const bool want = expectsAudio(a, a.pattern());
        if (want) {
            h.tap(btn::Play);
            CHECK(a.seq.isPlaying());
            for (int k = 0; k < 6 && peak <= 100; ++k) {
                peak = std::max(peak, h.audio.pump(500));
                h.run(10);
            }
            h.tap(btn::Play);
            CHECK(peak > 100);
            if (peak > 100) ++audible;
        }
        int instr = 0;
        for (int i = 0; i < kSampleInstrumentCount; ++i) instr += a.instrPresent[i];
        // Save into the project's own folder.
        const auto before = filesUnder(ms, rel);
        ms.written.clear();
        CHECK(a.saveProject());
        const auto after = filesUnder(ms, rel);
        std::set<std::string> wrote;
        for (const std::string& w : ms.written)
            if (w.compare(0, rel.size() + 1, rel + "/") == 0) wrote.insert(w.substr(rel.size() + 1));
        CHECK(!anyUnder(ms, rel + "/ot2"));
        for (int k = 0; k < kSynthSlotCount; ++k) CHECK(after.count(std::to_string(k) + ".patch") == 1);
        int same = 0, changed = 0, converted = 0, upgraded = 0, untouched = 0;
        for (const auto& kv : after) {
            auto it = before.find(kv.first);
            if (it == before.end()) continue;   // new file (converted legacy instrument, N.patch)
            const bool legacyMt = kv.first == "project.mt" && it->second.size() > 8 && it->second[8] < 17;
            const bool isMtp = kv.first.size() > 4 && kv.first.compare(kv.first.size() - 4, 4, ".mtp") == 0;
            if (isMtp) {
                // 1.9.x pattern -> Tracker Mini 16-track file.
                const std::vector<uint8_t>& o = it->second;
                const std::vector<uint8_t>& m = kv.second;
                CHECK_EQ(m.size(), size_t(mtp::kMiniFileSize));
                if (m.size() != mtp::kMiniFileSize) continue;
                const bool current = o.size() == mtp::kStockFileSize && o[8] == 5 && o[0x0C] == 0x2C && o[0x0D] == 0x24;
                const int keep = current ? kStockTrackCount : kLegacyTrackCount;
                if (current)
                    CHECK(std::memcmp(o.data() + mtp::kTracksOff, m.data() + mtp::kTracksOff, size_t(keep) * mtp::kTrackSize) == 0);
                for (int t = keep; t < kTrackCount; ++t) CHECK(rawTrackEmpty(m, t));
                ++upgraded;
                continue;
            }
            if (it->second == kv.second || (kv.first == "project.mt" && !legacyMt && projectMtSame(it->second, kv.second))) {
                if (wrote.count(kv.first)) ++same;
                else ++untouched;
            } else if (legacyMt) {
                ++converted;
            } else {
                if (!changed) std::printf("  %s: %s changed by the save\n", name.c_str(), kv.first.c_str());
                ++changed;
            }
        }
        for (const auto& kv : before)
            if (!after.count(kv.first)) ++converted;   // .mti / renamed .pti replaced by the save
        CHECK(changed == 0);
        // Proof that the save reproduced the files: a current-format
        // project.mt is rewritten byte-identical; a legacy one is converted.
        auto mtB = before.find("project.mt");
        if (mtB != before.end() && mtB->second.size() > 8 && mtB->second[8] >= 17) {
            CHECK(wrote.count("project.mt") == 1);
            CHECK(after.count("project.mt") && projectMtSame(mtB->second, after.at("project.mt")));
            CHECK(same > 0);
        } else {
            CHECK(converted > 0);
        }
        ++saved;
        if (last) *last = SaveRecord{before, after, wrote};
        std::printf("  %-40s %3d passes, max %6llu B/pass, %5d steps, %2d instr, peak %d, same %d untouched %d conv %d mtp %d\n",
                    name.c_str(), passes, static_cast<unsigned long long>(maxIo), steps, instr, peak, same, untouched,
                    converted, upgraded);
        ++opened;
    }
    return opened;
}

}  // namespace

TEST(fixture_backup_projects_open_incrementally) {
    const sfs::path projects = sfs::path(backupRoot()) / "Projects";
    std::error_code ec;
    if (!sfs::is_directory(projects, ec)) {
        std::printf("  SKIP: %s absent\n", projects.string().c_str());
        return;
    }
    int audible = 0, saved = 0;
    const int opened = openSaveProjects(projects, audible, saved);
    std::printf("  %d projects opened, %d audible, %d saved\n", opened, audible, saved);
    CHECK(opened > 0);
}

TEST(fixture_stock_project_open_save_byte_identical) {
    const sfs::path projects = sfs::path(stockProjectRoot());
    std::error_code ec;
    if (!sfs::is_directory(projects, ec)) {
        std::printf("  SKIP: %s absent\n", projects.string().c_str());
        return;
    }
    int audible = 0, saved = 0;
    CHECK(openSaveProjects(projects, audible, saved) == 1);
    CHECK(saved == 1);
}

std::string demoProjectRoot() {
    if (const char* e = std::getenv("OT_LEGACY_FIXTURES")) return std::string(e) + "/demo-sd/Projects";
    if (const char* e = std::getenv("OT_FIXTURES")) return std::string(e) + "/../../../fixtures/demo-sd/Projects";
    return "../../fixtures/demo-sd/Projects";
}

// The current-format 1.9.2 project "OT Demo" (v17 project.mt, 0x242C v5
// pattern, v9 .pti) through App-level Open -> Save: project.mt rewritten
// byte-identical with the slot area 0x74C..0x80B zero, every .pti rewritten
// byte-identical, the pattern becomes 0x3030 with tracks 1-12 the original
// bytes and 13-16 empty, and stock 1.9.2's decoder reads tracks 1-8 of the
// saved file unchanged (spec 21 §10.3).
TEST(fixture_demo_project_open_save_mini_upgrade) {
    const sfs::path projects = sfs::path(demoProjectRoot());
    std::error_code ec;
    if (!sfs::is_directory(projects / "OT Demo", ec)) {
        std::printf("  SKIP: %s absent\n", (projects / "OT Demo").string().c_str());
        return;
    }
    int audible = 0, saved = 0;
    SaveRecord rec;
    CHECK(openSaveProjects(projects, audible, saved, &rec) == 1);
    CHECK(saved == 1);
    // The fixture is what it claims to be.
    const std::vector<uint8_t>& mt0 = rec.before.at("project.mt");
    CHECK_EQ(mt0.size(), size_t(mt::kFileSize));
    CHECK_EQ(int(mt0[8]), 17);
    const std::vector<uint8_t>& pat0 = rec.before.at("patterns/pattern_01.mtp");
    CHECK_EQ(pat0.size(), size_t(mtp::kStockFileSize));
    CHECK_EQ(int(pat0[8]), 5);
    int pti = 0;
    for (const auto& kv : rec.before) {
        if (kv.first.size() < 4 || kv.first.compare(kv.first.size() - 4, 4, ".pti") != 0) continue;
        CHECK_EQ(int(kv.second[8]), 9);
        CHECK(rec.written.count(kv.first) == 1);
        CHECK(rec.after.count(kv.first) && rec.after.at(kv.first) == kv.second);
        ++pti;
    }
    CHECK(pti == 2);
    // project.mt: rewritten, identical, slot area zero.
    CHECK(rec.written.count("project.mt") == 1);
    const std::vector<uint8_t>& mt1 = rec.after.at("project.mt");
    // Identical except the documented recorder fill (projectMtSame): the
    // demo never stored recorder settings.
    CHECK(projectMtSame(mt0, mt1));
    CHECK(mt0[0x135] == 0 && mt1[0x135] == 50);
    bool zero = true;
    for (uint32_t o = mt::kSynthSlots; o < mt::kSynthSlots + 3 * mt::kSynthSlotSize; ++o) zero = zero && mt1[o] == 0;
    CHECK(zero);
    CHECK_EQ(mt::kSynthSlots + 3 * mt::kSynthSlotSize, 0x80Cu);
    // The pattern: 0x3030, tracks 1-12 the original bytes, 13-16 empty.
    CHECK(rec.written.count("patterns/pattern_01.mtp") == 1);
    const std::vector<uint8_t>& pat1 = rec.after.at("patterns/pattern_01.mtp");
    CHECK_EQ(pat1.size(), size_t(mtp::kMiniFileSize));
    CHECK(std::memcmp(pat0.data() + mtp::kTracksOff, pat1.data() + mtp::kTracksOff, size_t(kStockTrackCount) * mtp::kTrackSize) == 0);
    for (int t = kStockTrackCount; t < kTrackCount; ++t) CHECK(rawTrackEmpty(pat1, t));
    // What stock 1.9.2 reads from the saved file: tracks 1-8 as before.
    PatternStock orig, stock;
    decodePattern(pat0.data(), pat0.size(), orig);
    decodePattern(pat1.data(), pat1.size(), stock);
    int notes = 0;
    for (int t = 0; t < kLegacyTrackCount; ++t) {
        for (int s = 0; s < kStepCount; ++s) {
            const Step& x = orig.tracks[t].steps[s];
            const Step& y = stock.tracks[t].steps[s];
            CHECK(x.note == y.note && x.instrument == y.instrument && x.fx[0].type == y.fx[0].type &&
                  x.fx[0].value == y.fx[0].value && x.fx[1].type == y.fx[1].type && x.fx[1].value == y.fx[1].value);
            notes += x.note >= 0;
        }
        CHECK_EQ(int(orig.tracks[t].lastStep), int(stock.tracks[t].lastStep));
    }
    CHECK(notes > 0);
}

// Field bug #1 regression on a synthetic card: a pattern file that exists
// but cannot be read is reported (job error) instead of silently becoming an
// empty pattern, and the readable ones still load.
namespace {
struct FlakyStorage : FakeStorage {
    std::string failPath;
    int failReads = 0;
    std::map<Handle, std::string> paths;
    Handle open(const char* path, OpenMode m) override {
        const Handle h = FakeStorage::open(path, m);
        if (h >= 0) paths[h] = path;
        return h;
    }
    int32_t read(Handle h, void* d, uint32_t n) override {
        if (failReads > 0 && paths[h] == failPath) {
            --failReads;
            return -1;
        }
        return FakeStorage::read(h, d, n);
    }
};
}  // namespace

TEST(unreadable_pattern_is_reported_not_silently_empty) {
    Harness h(false);
    FlakyStorage fsx;
    HalRefs hr = h.app->hal();
    hr.storage = &fsx;
    App& a = *h.app;
    CHECK(a.init(hr, a.mem()));
    CHECK(a.boot());
    // Two patterns with a note each in the Workspace.
    for (int n = 1; n <= 2; ++n) {
        a.selectPattern(n);
        a.pattern().tracks[0].steps[n].note = 60;
        a.patternEdited();
    }
    a.autosave();
    // A transient read error is retried.
    fsx.failPath = "Workspace/patterns/pattern_02.mtp";
    fsx.failReads = 1;
    CHECK(a.loadWorkspace());
    CHECK(a.lastJobErrors() == 0);
    CHECK(a.stored(2).tracks[0].steps[2].note == 60);
    // A persistent one is reported.
    fsx.failReads = 100;
    CHECK(a.loadWorkspace());
    CHECK(a.lastJobErrors() == 1);
    CHECK(a.stored(1).tracks[0].steps[1].note == 60);
    fsx.failReads = 0;
}

// The periodic autosave runs in the background without a popup and leaves
// the UI responsive.
TEST(autosave_runs_in_background) {
    Harness h;
    App& a = *h.app;
    a.pattern().tracks[0].steps[0].note = 61;
    a.patternEdited();
    h.clock.ms += 10001;
    a.loop();
    CHECK(a.jobBusy());
    for (int i = 0; i < 50 && a.jobBusy(); ++i) {
        CHECK(!h.disp.lastHasText("Saving project"));
        h.clock.ms += 1;
        a.loop();
    }
    CHECK(!a.jobBusy());
    Pattern p;
    CHECK(loadPatternFile(h.fs, "Workspace/patterns/pattern_01.mtp", p) == FmtStatus::Ok);
    CHECK(p.tracks[0].steps[0].note == 61);
}

// ---------------------------------------------------------------------------
// Tracker Mini project I/O (docs/MINI_PLAN.md §1, docs/specs/21 §2, §4, §7)

namespace {
constexpr const char* kPat1 = "Workspace/patterns/pattern_01.mtp";

// Note on track 13 (index 12) of the current pattern, through the store.
void setTrack13(App& a, int step, int8_t note) {
    a.pattern().tracks[12].steps[step].note = note;
    a.pattern().tracks[12].steps[step].instrument = note < 0 ? 0 : 60;
    a.patternEdited();
}
int8_t storedTrack13(App& a, int num, int step) {
    const Track* t = a.findStoredTrack(num, 12);
    return t ? t->steps[step].note : int8_t(kNoteEmpty);
}
// Track t, step s of a 16-track pattern file on the card (raw bytes).
int8_t fileNote(FakeStorage& fs, const char* path, int t, int s) {
    auto it = fs.files.find(path);
    if (it == fs.files.end() || it->second.size() != mtp::kMiniFileSize) return -100;
    return int8_t(it->second[mtp::kTracksOff + size_t(t) * mtp::kTrackSize + 1 + size_t(s) * mtp::kStepSize]);
}
[[maybe_unused]] std::vector<uint8_t> encoded(const SynthPatch& p) {
    std::vector<uint8_t> b(kPatchMaxFileSize);
    size_t n = 0;
    CHECK(encodePatch(p, b.data(), b.size(), &n) == PatchStatus::kOk);
    b.resize(n);
    return b;
}
// Notices through the Ui platform hook (Ui::notify), cleared on read.
std::string g_note;
void onNote(const char* t) { g_note = t; }
std::string noticeText(Harness& h) {
    (void)h;
    std::string n;
    n.swap(g_note);
    for (const char* t : {"Track memory full", "patch is too large", "patch is damaged"})
        if (n.find(t) != std::string::npos) return t;
    return n;
}
void hookNotices(App& a) {
    a.ui.platform.notified = &onNote;
    g_note.clear();
}
std::vector<uint8_t> vendorPatch(const char* engine, const char* name) {
    const char* root = std::getenv("OT_VENDOR_FIXTURES");
    if (!root) return {};
    const sfs::path p = sfs::path(root) / "mini-patches-2.0" / engine / (std::string(name) + ".patch");
    std::error_code ec;
    if (!sfs::is_regular_file(p, ec)) return {};
    return readHostFile(p);
}
}  // namespace

#if OT_SYNTH_PACK   // the synth engines (modules) build the Init patches
TEST(mini_new_project_patterns_and_patches) {
    Harness h;
    App& a = *h.app;
    // New project: 0x3030 pattern, the three slot patches (Init: no library).
    CHECK_EQ(h.fs.files[kPat1].size(), size_t(mtp::kMiniFileSize));
    for (int k = 0; k < kSynthSlotCount; ++k) {
        const std::string p = "Workspace/" + std::to_string(k) + ".patch";
        CHECK(h.fs.files.count(p) == 1);
        CHECK(a.synthPatchSource(k) == App::PatchSource::Init);
        CHECK(h.fs.files[p] == encoded(a.synthPatch(k)));
    }
    CHECK_EQ(a.synthPatch(0).engine, int32_t(kSynthEngineAcd));   // 2.0: Synth 1 ACD (Mini: WTFM)
    CHECK_EQ(a.synthPatch(1).engine, int32_t(kSynthEngineAcd));
    CHECK_EQ(a.synthPatch(2).engine, int32_t(kSynthEngineVap));
    a.pattern().tracks[0].steps[0].note = 60;
    a.patternEdited();
    CHECK(a.saveProjectAs("Plain"));
    CHECK_EQ(h.fs.files["Projects/Plain/patterns/pattern_01.mtp"].size(), size_t(mtp::kMiniFileSize));
    for (int k = 0; k < kSynthSlotCount; ++k)
        CHECK(h.fs.files["Projects/Plain/" + std::to_string(k) + ".patch"] == encoded(a.synthPatch(k)));
    CHECK(!anyUnder(h.fs, "Projects/Plain/ot2"));
    for (const auto& kv : h.fs.files) CHECK(kv.first.find(".tmp") == std::string::npos);
}
#endif

TEST(mini_track13_lives_in_the_pattern_file) {
    Harness h;
    App& a = *h.app;
    setTrack13(a, 3, 50);
    a.autosave();
    CHECK_EQ(fileNote(h.fs, kPat1, 12, 3), 50);
    CHECK(!h.fs.files.count(std::string(kPat1) + ".tmp"));
    // Workspace reload (boot path).
    CHECK(a.loadWorkspace());
    CHECK(a.lastJobErrors() == 0);
    CHECK(storedTrack13(a, 1, 3) == 50);
    CHECK(a.pattern().tracks[12].steps[3].instrument == 60);
    // Save As, a new project drops it, Open brings it back.
    CHECK(a.saveProjectAs("Side"));
    CHECK_EQ(fileNote(h.fs, "Projects/Side/patterns/pattern_01.mtp", 12, 3), 50);
    CHECK(a.newProject());
    CHECK(storedTrack13(a, 1, 3) == kNoteEmpty);
    CHECK(a.openProject("Projects", "Side"));
    CHECK(storedTrack13(a, 1, 3) == 50);
    // Emptying track 13 frees its pool block; the file keeps an empty track.
    setTrack13(a, 3, kNoteEmpty);
    a.autosave();
    CHECK(a.findStoredTrack(1, 12) == nullptr);
    CHECK_EQ(fileNote(h.fs, kPat1, 12, 3), int8_t(kNoteEmpty));
    CHECK(a.saveProject());
    CHECK_EQ(fileNote(h.fs, "Projects/Side/patterns/pattern_01.mtp", 12, 3), int8_t(kNoteEmpty));
}

TEST(mini_workspace_1_9_2_pattern_upgraded_by_autosave) {
    // A Workspace left by 1.9.x firmware: 12-track files load and the
    // autosave rewrites them as 0x3030 (not "unsaved").
    Harness h;
    App& a = *h.app;
    a.pattern().tracks[11].steps[5].note = 70;
    a.patternEdited();
    a.autosave();
    PatternStock ps;
    CHECK(loadPatternFile(h.fs, kPat1, ps) == FmtStatus::Ok);
    std::vector<uint8_t> stock(mtp::kStockFileSize);
    encodePattern(ps, stock.data());
    h.fs.files[kPat1] = stock;
    CHECK(a.loadWorkspace());
    CHECK(a.stored(1).tracks[11].steps[5].note == 70);
    CHECK(!a.unsavedChanges());
    a.autosave();
    CHECK_EQ(h.fs.files[kPat1].size(), size_t(mtp::kMiniFileSize));
    CHECK_EQ(fileNote(h.fs, kPat1, 11, 5), 70);
}

#if OT_SYNTH_PACK   // the synth engines (modules) build the Init patches
TEST(mini_patch_open_copies_file_library_and_init_fallbacks) {
    Harness h;
    App& a = *h.app;
    hookNotices(a);
    // A patch of the slot's engine (ACD, slot 2) changed from Init.
    SynthPatch p = a.synthPatch(1);
    p.params[0].bits = patchFloatToBits(0.25f);
    a.synthPatch(1) = p;
    a.markSynthPatchDirty(1);
    a.autosave();
    const std::vector<uint8_t> v1 = encoded(p);
    CHECK(h.fs.files["Workspace/1.patch"] == v1);
    CHECK(a.saveProjectAs("Pat"));
    CHECK(h.fs.files["Projects/Pat/1.patch"] == v1);
    // Open copies <proj>/N.patch to the Workspace and loads it.
    CHECK(a.newProject());
    CHECK(a.openProject("Projects", "Pat"));
    CHECK(a.synthPatchSource(1) == App::PatchSource::File);
    CHECK(h.fs.files["Workspace/1.patch"] == v1);
    CHECK(encoded(a.synthPatch(1)) == v1);
    // Missing file, no library patch of the record's name: Init.
    h.fs.files.erase("Workspace/1.patch");
    CHECK(a.loadWorkspace());
    CHECK(a.synthPatchSource(1) == App::PatchSource::Init);
#ifdef OT_MOD_ACD1
    CHECK(noticeText(h).empty());
#else   // a profile without the ACD module: the slot is reported, nothing else
    CHECK(noticeText(h) == "Synth 1 (ACD) not available");
#endif
    // A patch of another engine is refused (silently): a VAP patch on the
    // ACD slot 0.
    h.fs.files["Workspace/0.patch"] = encoded(a.synthPatch(2));
    // Too large (> 8 KB) and damaged files are refused with a notice.
    h.fs.files["Workspace/2.patch"] = std::vector<uint8_t>(kPatchMaxFileSize + 100, 0x08);
    CHECK(a.loadWorkspace());
    CHECK(a.synthPatchSource(0) == App::PatchSource::Init);
    CHECK(a.synthPatchSource(2) == App::PatchSource::Init);
    CHECK(noticeText(h) == "patch is too large");
    h.fs.files["Workspace/2.patch"] = {0x0F, 0x01, 0x02};   // wire type 7
    CHECK(a.loadWorkspace());
    CHECK(noticeText(h) == "patch is damaged");
    CHECK(a.lastJobErrors() == 0);

    // Library fallback by the slot's patch name (vendor patches; skipped
    // when absent): Patches/ACD/<name>.patch, byte-exact through the save.
    const std::vector<uint8_t> lib = vendorPatch("ACD", "Acid Saw");
    if (lib.empty()) {
        std::printf("  SKIP library part: no vendor patches (OT_VENDOR_FIXTURES)\n");
        return;
    }
    h.fs.dirs.insert("Patches");
    h.fs.dirs.insert("Patches/ACD");
    h.fs.files["Patches/ACD/Acid Saw.patch"] = lib;
    SynthSlotRecord r = a.project.synthSlot(1);
    std::snprintf(r.patchName, sizeof(r.patchName), "%s", "Acid Saw");
    a.project.setSynthSlot(1, r);
    a.markProjectDirty();
    a.autosave();
    h.fs.files.erase("Workspace/1.patch");
    CHECK(a.loadWorkspace());
    CHECK(a.synthPatchSource(1) == App::PatchSource::Library);
    CHECK(encoded(a.synthPatch(1)) == lib);
    CHECK(a.saveProjectAs("Lib"));
    CHECK(h.fs.files["Projects/Lib/1.patch"] == lib);
    // A library patch of another engine is refused: Init.
    h.fs.files["Patches/ACD/Acid Saw.patch"] = vendorPatch("VAP", "Init").empty() ? std::vector<uint8_t>{0x08, 0x04}
                                                                                   : vendorPatch("VAP", "Init");
    h.fs.files.erase("Workspace/1.patch");
    CHECK(a.loadWorkspace());
    CHECK(a.synthPatchSource(1) == App::PatchSource::Init);
}
#endif

// spec 21 §3.3: a slot record that fails the load check takes the slot
// default, and its macro values come from the bipolar flags of the patch
// then loaded (63 bipolar, else 0). The project.mt check runs before the
// patch exists, so the load applies them after the patch resolves.
TEST(mini_slot_reset_at_load_takes_bipolar_macro_defaults) {
    const std::vector<uint8_t> bip = vendorPatch("ACD", "Dusty Acid 2");   // factory ACD, M1-M6 bipolar
    if (bip.empty()) {
        std::printf("  SKIP: no vendor patches (OT_VENDOR_FIXTURES)\n");
        return;
    }
    SynthPatch dp;
    CHECK(decodePatch(bip.data(), bip.size(), &dp) == PatchStatus::kOk);
    for (int k = 0; k < 6; ++k) CHECK(dp.macros[k].bipolar);
    Harness h;
    App& a = *h.app;
    auto breakRecord = [](std::vector<uint8_t>& mt, int s) {
        mt[mt::synthSlotOff(s) + 4] = 9;   // polyphony 9 > 8: the whole record is reset
    };
    // Workspace load: slot 2 (ACD by default) with the bipolar patch.
    h.fs.files["Workspace/1.patch"] = bip;
    breakRecord(h.fs.files["Workspace/project.mt"], 1);
    CHECK(a.loadWorkspace());
    CHECK(a.synthPatchSource(1) == App::PatchSource::File);
    for (int k = 0; k < 6; ++k) CHECK_EQ(a.project.synthSlot(1).macros[k], 63);
    for (int k = 0; k < 6; ++k) CHECK_EQ(a.project.synthSlot(2).macros[k], 0);   // not reset: untouched
    // The autosave stores the corrected record.
    a.autosave();
    Project q;
    CHECK(loadProjectFile(h.fs, "Workspace/project.mt", q) == FmtStatus::Ok);
    for (int k = 0; k < 6; ++k) CHECK_EQ(q.synthSlot(1).macros[k], 63);
    // Open of a saved project with a broken record: the same through the copy.
    CHECK(a.saveProjectAs("Bip"));
    breakRecord(h.fs.files["Projects/Bip/project.mt"], 1);
    CHECK(a.newProject());
    CHECK(a.openProject("Projects", "Bip"));
    CHECK(a.synthPatchSource(1) == App::PatchSource::File);
    for (int k = 0; k < 6; ++k) CHECK_EQ(a.project.synthSlot(1).macros[k], 63);
    a.autosave();
    CHECK(loadProjectFile(h.fs, "Workspace/project.mt", q) == FmtStatus::Ok);
    for (int k = 0; k < 6; ++k) CHECK_EQ(q.synthSlot(1).macros[k], 63);
    // A valid record keeps its stored macro values (no reset, no defaults).
    CHECK(a.loadWorkspace());
    for (int k = 0; k < 6; ++k) CHECK_EQ(a.project.synthSlot(1).macros[k], 63);
}

// A project's own N.patch that is refused at load (too large, damaged) is
// never overwritten by Save: the slot plays the fallback, the Workspace
// keeps the original, Save leaves the project's file alone (notice), Save As
// carries the original bytes. A real change of the slot's patch is saved.
#if OT_SYNTH_PACK   // the synth engines (modules) build the Init patches
TEST(mini_refused_project_patch_survives_save) {
    Harness h;
    App& a = *h.app;
    hookNotices(a);
    CHECK(a.saveProjectAs("Big"));
    std::vector<uint8_t> big(9000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 7 + 3);
    const std::vector<uint8_t> bad{0x0F, 0x01, 0x02};   // wire type 7: damaged
    h.fs.files["Projects/Big/1.patch"] = big;
    h.fs.files["Projects/Big/2.patch"] = bad;
    CHECK(a.newProject());
    CHECK(a.openProject("Projects", "Big"));
    CHECK(noticeText(h) == "patch is too large");
    CHECK(a.synthPatchSource(1) == App::PatchSource::Init);
    CHECK(a.synthPatchSource(2) == App::PatchSource::Init);
    CHECK(h.fs.files["Workspace/1.patch"] == big);
    a.autosave();
    CHECK(h.fs.files["Workspace/1.patch"] == big);   // the fallback is not written over it
    CHECK(a.saveProject());
    CHECK(h.fs.files["Projects/Big/1.patch"] == big);
    CHECK(h.fs.files["Projects/Big/2.patch"] == bad);
    CHECK(g_note == "Synth 2 patch kept unchanged");
    // After a reboot (Workspace load) the same.
    CHECK(a.loadWorkspace());
    CHECK(a.saveProject());
    CHECK(h.fs.files["Projects/Big/1.patch"] == big);
    CHECK(h.fs.files["Projects/Big/2.patch"] == bad);
    // Save As: the new project gets the original bytes.
    CHECK(a.saveProjectAs("Big2"));
    CHECK(h.fs.files["Projects/Big2/1.patch"] == big);
    CHECK(h.fs.files["Projects/Big2/2.patch"] == bad);
    CHECK(h.fs.files["Projects/Big/1.patch"] == big);
    for (const auto& kv : h.fs.files) CHECK(kv.first.find(".tmp") == std::string::npos);
    // The user changes slot 2's patch: that is saved (slot 3 still kept).
    a.synthPatch(1).params[0].bits = patchFloatToBits(0.5f);
    a.markSynthPatchDirty(1);
    CHECK(a.saveProject());
    CHECK(h.fs.files["Projects/Big2/1.patch"] == encoded(a.synthPatch(1)));
    CHECK(h.fs.files["Projects/Big2/2.patch"] == bad);
    CHECK(g_note == "Synth 3 patch kept unchanged");
    // A new project forgets the refusal.
    CHECK(a.newProject());
    g_note.clear();
    CHECK(a.saveProjectAs("Fresh"));
    CHECK(h.fs.files["Projects/Fresh/2.patch"] == encoded(a.synthPatch(2)));
    CHECK(g_note.empty());
}
#endif

#if OT_SYNTH_PACK   // the synth engines (modules) build the Init patches
TEST(mini_dirty_patch_rewritten_by_autosave_only) {
    Harness h;
    App& a = *h.app;
    const std::vector<uint8_t> before = h.fs.files["Workspace/2.patch"];
    CHECK(!a.startAutosave());   // nothing dirty
    a.synthPatch(2).params[1].bits = patchFloatToBits(0.123f);
    CHECK(!a.startAutosave());   // not marked: not written
    CHECK(h.fs.files["Workspace/2.patch"] == before);
    a.markSynthPatchDirty(2);
    CHECK(a.unsavedChanges());
    a.autosave();
    CHECK(!a.synthPatchDirty(2));
    CHECK(h.fs.files["Workspace/2.patch"] == encoded(a.synthPatch(2)));
    CHECK(h.fs.files["Workspace/2.patch"] != before);
}
#endif

TEST(mini_pool_full_at_load_keeps_tracks_in_the_file) {
    // Pool of 2 usable blocks; a pattern file with 4 ext tracks (written by
    // a device with a bigger pool, or the Mini).
    Harness h(false);
    App& a = *h.app;
    AppMemory m = a.mem();
    m.extTrackBlocks = 3;
    CHECK(a.init(a.hal(), m));
    CHECK(a.boot());
    hookNotices(a);
    static Pattern full;
    full.setDefault();
    for (auto& t : full.tracks) t.lastStep = a.stored(1).tracks[0].lastStep;
    for (int e = 0; e < 4; ++e) {
        full.tracks[12 + e].steps[0].note = int8_t(30 + e);
        full.tracks[12 + e].steps[0].instrument = uint8_t(60 + e);
    }
    std::vector<uint8_t> img(mtp::kMiniFileSize);
    encodePattern16(full, img.data());
    h.fs.files[kPat1] = img;
    CHECK(a.loadWorkspace());
    CHECK(noticeText(h) == "Track memory full");
    CHECK(storedTrack13(a, 1, 0) == 30);
    CHECK(a.findStoredTrack(1, 15) == nullptr);   // did not fit
    // Editing the pattern: the autosave keeps the tracks that did not fit.
    a.pattern().tracks[0].steps[1].note = 50;
    a.patternEdited();
    a.autosave();
    for (int e = 0; e < 4; ++e) CHECK_EQ(fileNote(h.fs, kPat1, 12 + e, 0), 30 + e);
    CHECK_EQ(fileNote(h.fs, kPat1, 0, 1), 50);
    // Deleting the pattern on purpose drops them.
    a.defaultStored(1);
    a.reloadCurrentFromStore();
    a.markPatternDirty(1);
    a.autosave();
    for (int e = 0; e < 4; ++e) CHECK_EQ(fileNote(h.fs, kPat1, 12 + e, 0), int8_t(kNoteEmpty));
}

TEST(mini_pool_full_edit_refused_shows_what_is_stored) {
    // VERIFY_M1 R2: with the pool full an edit of tracks 13-16 is refused,
    // the working pattern re-assembled from the store and a notice shown.
    Harness h(false);
    App& a = *h.app;
    AppMemory m = a.mem();
    m.extTrackBlocks = 2;   // one usable block
    CHECK(a.init(a.hal(), m));
    CHECK(a.boot());
    hookNotices(a);
    setTrack13(a, 0, 40);
    CHECK(a.pattern().tracks[12].steps[0].note == 40);
    a.pattern().tracks[13].steps[0].note = 41;
    a.pattern().tracks[13].steps[0].instrument = 61;
    a.pattern().tracks[0].steps[0].note = 42;   // stock track edit in the same burst
    a.patternEdited();
    CHECK(a.pattern().tracks[13].steps[0].note == kNoteEmpty);   // refused, shown as stored
    CHECK(a.pattern().tracks[12].steps[0].note == 40);
    CHECK(a.pattern().tracks[0].steps[0].note == 42);
    CHECK(noticeText(h) == "Track memory full");
    a.autosave();
    CHECK(a.loadWorkspace());
    CHECK(a.pattern().tracks[12].steps[0].note == 40);
    CHECK(a.pattern().tracks[13].steps[0].note == kNoteEmpty);
    CHECK(a.pattern().tracks[0].steps[0].note == 42);
}

// Power loss at every card operation of an autosave that rewrites a
// pattern (tracks 13-16 included) and a patch: afterwards the old or the
// new version loads, never neither.
namespace {
struct CrashStorage : FakeStorage {
    int budget = -1;   // mutating operations left; -1 = unlimited
    int ops = 0;
    bool spend() {
        ++ops;
        if (budget < 0) return true;
        if (budget == 0) return false;
        --budget;
        return true;
    }
    Handle open(const char* path, OpenMode m) override {
        if (m != kRead && !spend()) return -1;
        return FakeStorage::open(path, m);
    }
    int32_t write(Handle h, const void* s, uint32_t n) override {
        if (budget == 0) return -1;
        if (!spend()) return -1;
        if (budget == 0 && n > 1) {   // the power fails half-way through this write
            FakeStorage::write(h, s, n / 2);
            return -1;
        }
        return FakeStorage::write(h, s, n);
    }
    bool remove(const char* p) override { return spend() && FakeStorage::remove(p); }
    bool rename(const char* f, const char* t) override { return spend() && FakeStorage::rename(f, t); }
    bool mkdir(const char* p) override { return spend() && FakeStorage::mkdir(p); }
};

const uint32_t kV1 = 0x3E800000u, kV2 = 0x3F000000u;   // 0.25f, 0.5f

// Boot on `cs`, write v1 (track 13 note 40, slot 2 param 0 = 0.25), edit to v2.
[[maybe_unused]] void crashSetup(Harness& h, CrashStorage& cs) {
    HalRefs hr = h.app->hal();
    hr.storage = &cs;
    CHECK(h.app->init(hr, h.app->mem()));
    CHECK(h.app->boot());
    setTrack13(*h.app, 7, 40);
    h.app->synthPatch(1).params[0].bits = kV1;
    h.app->markSynthPatchDirty(1);
    h.app->autosave();
    setTrack13(*h.app, 7, 41);
    h.app->synthPatch(1).params[0].bits = kV2;
    h.app->markSynthPatchDirty(1);
    h.app->markProjectDirty();
}
}  // namespace

#if OT_SYNTH_PACK   // the synth engines (modules) build the Init patches
TEST(mini_power_loss_mid_write_keeps_old_or_new) {
    int total = 0;
    {
        Harness h(false);
        CrashStorage cs;
        crashSetup(h, cs);
        cs.ops = 0;
        h.app->autosave();
        total = cs.ops;
    }
    CHECK(total > 6);
    int oldSeen = 0, newSeen = 0, stockLost = 0, oldPatch = 0, newPatch = 0;
    for (int k = 0; k <= total; ++k) {
        Harness h(false);
        CrashStorage cs;
        crashSetup(h, cs);
        cs.budget = k;
        h.app->autosave();
        // Stock project.mt is not written atomically (as 1.9.2): a crash
        // inside its write loses the project itself.
        auto mt = cs.files.find("Workspace/project.mt");
        if (mt == cs.files.end() || mt->second.size() != mt::kFileSize) {
            ++stockLost;
            continue;
        }
        // Power back: a fresh device boots from the same card.
        Harness h2(false);
        h2.fs.files = cs.files;
        h2.fs.dirs = cs.dirs;
        CHECK(h2.app->boot());
        App& b = *h2.app;
        CHECK_EQ(b.lastJobErrors(), 0);
        const int8_t n = storedTrack13(b, 1, 7);
        if (n != 40 && n != 41) std::printf("  crash after %d ops: track 13 = %d\n", k, n);
        CHECK(n == 40 || n == 41);
        const uint32_t pv = b.synthPatch(1).params[0].bits;
        if (pv != kV1 && pv != kV2) std::printf("  crash after %d ops: patch param = %08x\n", k, pv);
        CHECK(b.synthPatchSource(1) == App::PatchSource::File);
        CHECK(pv == kV1 || pv == kV2);
        oldSeen += n == 40;
        newSeen += n == 41;
        oldPatch += pv == kV1;
        newPatch += pv == kV2;
        // The boot cleaned up: no .tmp left.
        for (const auto& kv : h2.fs.files) CHECK(kv.first.find(".tmp") == std::string::npos);
    }
    std::printf("  %d crash points: pattern %d old / %d new, patch %d old / %d new, %d inside the project.mt write\n",
                total + 1, oldSeen, newSeen, oldPatch, newPatch, stockLost);
    CHECK(oldSeen > 0 && newSeen > 0);
    CHECK(oldPatch > 0 && newPatch > 0);
    CHECK(stockLost <= 3);
}
#endif

// A new N.patch (no old file) whose unknown fields 10 / 11 follow Extra, as
// in 32 factory patches: the power fails at every write operation. The
// boot restores the orphan .tmp only when it is whole; a file cut right
// after Extra (fields 10 / 11 lost) must never load (writePatchAtomic
// writes Extra last; completePatch wants the full re-encoded size).
#if OT_SYNTH_PACK   // the synth engines (modules) build the Init patches
TEST(mini_power_loss_new_patch_with_fields_after_extra) {
    const uint8_t tail[] = {0x52, 0x02, 'x', 'y', 0x52, 0x01, 'z', 0x5A, 0x03, 'a', 'b', 'c'};   // 10, 10, 11
    auto setup = [&](Harness& h, CrashStorage& cs) {
        HalRefs hr = h.app->hal();
        hr.storage = &cs;
        CHECK(h.app->init(hr, h.app->mem()));
        CHECK(h.app->boot());
        cs.files.erase("Workspace/1.patch");
        SynthPatch& p = h.app->synthPatch(1);
        p.params[0].bits = kV2;
        std::memcpy(p.opaque, tail, sizeof(tail));
        p.opaqueLen = uint16_t(sizeof(tail));
        h.app->markSynthPatchDirty(1);
    };
    std::vector<uint8_t> full;
    int total = 0;
    {
        Harness h(false);
        CrashStorage cs;
        setup(h, cs);
        full = encoded(h.app->synthPatch(1));
        size_t beg = 0, end = 0;
        CHECK(patchFieldSpan(full.data(), full.size(), 9, &beg, &end));
        CHECK(end + sizeof(tail) == full.size());   // the unknown fields follow Extra
        cs.ops = 0;
        h.app->autosave();
        total = cs.ops;
        CHECK(cs.files["Workspace/1.patch"] == full);
    }
    int whole = 0, none = 0;
    for (int k = 0; k <= total; ++k) {
        Harness h(false);
        CrashStorage cs;
        setup(h, cs);
        cs.budget = k;
        h.app->autosave();
        Harness h2(false);
        h2.fs.files = cs.files;
        h2.fs.dirs = cs.dirs;
        CHECK(h2.app->boot());
        App& b = *h2.app;
        if (b.synthPatchSource(1) == App::PatchSource::File) {
            CHECK(encoded(b.synthPatch(1)) == full);
            CHECK_EQ(int(b.synthPatch(1).opaqueLen), int(sizeof(tail)));
            ++whole;
        } else {
            ++none;
        }
        for (const auto& kv : h2.fs.files) CHECK(kv.first.find(".tmp") == std::string::npos);
    }
    std::printf("  %d crash points: new patch whole %d, not there %d\n", total + 1, whole, none);
    CHECK(whole > 0 && none > 0);
    // The power fails once exactly end-of-Extra bytes of the .tmp are on the
    // card (the cut that keeps a decodable Extra and drops fields 10 / 11
    // when Extra is written in file order): nothing incomplete loads.
    {
        struct ByteCut : FakeStorage {
            size_t left = SIZE_MAX;   // bytes the card still takes
            bool dead = false;
            Handle open(const char* p, OpenMode m) override { return dead && m != kRead ? -1 : FakeStorage::open(p, m); }
            int32_t write(Handle h, const void* src, uint32_t n) override {
                if (dead) return -1;
                if (n > left) {
                    FakeStorage::write(h, src, uint32_t(left));
                    dead = true;
                    return -1;
                }
                left -= n;
                return FakeStorage::write(h, src, n);
            }
            bool remove(const char* p) override { return !dead && FakeStorage::remove(p); }
            bool rename(const char* x, const char* y) override { return !dead && FakeStorage::rename(x, y); }
        };
        Harness h(false);
        ByteCut bc;
        HalRefs hr = h.app->hal();
        hr.storage = &bc;
        CHECK(h.app->init(hr, h.app->mem()));
        CHECK(h.app->boot());
        bc.files.erase("Workspace/1.patch");
        SynthPatch& p = h.app->synthPatch(1);
        p.params[0].bits = kV2;
        std::memcpy(p.opaque, tail, sizeof(tail));
        p.opaqueLen = uint16_t(sizeof(tail));
        h.app->markSynthPatchDirty(1);
        size_t beg = 0, end = 0;
        CHECK(patchFieldSpan(full.data(), full.size(), 9, &beg, &end));
        bc.left = end;
        h.app->autosave();
        CHECK(bc.dead);
        Harness h2(false);
        h2.fs.files = bc.files;
        h2.fs.dirs = bc.dirs;
        CHECK(h2.app->boot());
        CHECK(h2.app->synthPatchSource(1) != App::PatchSource::File || encoded(h2.app->synthPatch(1)) == full);
    }
    // A .tmp with Extra still zero-filled (any cut before the last write) is refused.
    {
        Harness h;
        std::vector<uint8_t> cut = full;
        size_t beg = 0, end = 0;
        CHECK(patchFieldSpan(cut.data(), cut.size(), 9, &beg, &end));
        std::fill(cut.begin() + long(beg), cut.begin() + long(end), uint8_t(0));
        h.fs.files.erase("Workspace/1.patch");
        h.fs.files["Workspace/1.patch.tmp"] = cut;
        CHECK(h.app->loadWorkspace());
        CHECK(h.app->synthPatchSource(1) != App::PatchSource::File);
        CHECK(!h.fs.files.count("Workspace/1.patch.tmp"));
    }
}
#endif

// The mixer's mute of track 13 is project.mt 0x742[4]: the sequencer reads
// it and it survives the autosave and a reload.
namespace {
int noteOnsCh13(const FakeMidi& m, size_t from) {
    int n = 0;
    for (size_t i = from; i + 2 < m.sent.size(); ++i)
        if (m.sent[i] == 0x9C && m.sent[i + 2] > 0) ++n;   // note on, channel 13
    return n;
}
int playAndCount(Harness& h) {
    const size_t from = h.midi.sent.size();
    h.tap(btn::Play);
    for (int k = 0; k < 20; ++k) {
        h.audio.pump(500);
        h.run(10);
    }
    h.tap(btn::Play);
    h.run(20);
    return noteOnsCh13(h.midi, from);
}
}  // namespace

TEST(mixer_mute_track13_reaches_sequencer_and_survives_reload) {
    Harness h;
    App& a = *h.app;
    a.cfg.midiOut = 3;   // MIDI out on both ports
    a.applySequencerSettings();
    for (int s = 0; s < 32; s += 2) {
        a.pattern().tracks[12].steps[s].note = 48;
        a.pattern().tracks[12].steps[s].instrument = 60;   // MIDI channel 13
    }
    a.patternEdited();
    CHECK(playAndCount(h) > 0);   // unmuted: track 13 sends notes
    // Track Mixer, bank B, F5 = track 13.
    h.tap(btn::Master);
    h.tap(btn::Master);
    h.tap(btn::Right);
    h.tap(btn::F5);
    CHECK(a.project.u8(0x742 + 4) == 1);
    CHECK(a.trackMuted(12));
    CHECK(playAndCount(h) == 0);  // the sequencer stops track 13
    a.autosave();
    CHECK(a.loadWorkspace());
    CHECK(a.trackMuted(12));
    CHECK(playAndCount(h) == 0);
    // A fresh boot on the same card agrees.
    Harness h2(false);
    h2.fs.files = h.fs.files;
    h2.fs.dirs = h.fs.dirs;
    CHECK(h2.app->boot());
    h2.app->cfg.midiOut = 3;
    h2.app->applySequencerSettings();
    CHECK(h2.app->trackMuted(12));
    CHECK(playAndCount(h2) == 0);
    // Unmuted again: notes again.
    a.setTrackMute(12, false);
    CHECK(a.project.u8(0x742 + 4) == 0);
    CHECK(playAndCount(h) > 0);
}
