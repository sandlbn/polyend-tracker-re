// Project browser (spec 05 appendix E1.2), recents (spec 02 §7, appendix
// E2), memory pre-check (spec 02 §8.2), legacy project open/save (spec 10
// §1.5, §3.3, §4) and the card-removed screen (spec 05 §1.5).
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "test.h"
#include "ui_fakes.h"

#include "app/project_browser.h"
#include "app/project_io.h"
#include "formats/file_io.h"
#include "formats/instrument_pti.h"
#include "formats/pattern_mtp.h"
#include "formats/project_mt.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {

FileScreen& fileScr(Harness& h) { return *static_cast<FileScreen*>(h.app->ui.screen(kModFile)); }

void addProject(Harness& h, const char* dir) {
    // Minimal v17 project folder.
    std::string d(dir);
    for (size_t p = d.find('/'); p != std::string::npos; p = d.find('/', p + 1)) h.fs.dirs.insert(d.substr(0, p));
    h.fs.dirs.insert(d);
    Project p;
    p.setDefault();
    CHECK(saveProjectFile(h.fs, (d + "/project.mt").c_str(), p) == FmtStatus::Ok);
}

char g_names[64][40];

std::vector<uint8_t> wavBytes(int frames, int16_t base) {
    std::vector<int16_t> s(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) s[size_t(i)] = int16_t(base + i);
    std::vector<uint8_t> out(wav::kHeaderSize + size_t(frames) * 2);
    CHECK(wavEncode(s.data(), uint32_t(frames), 1, out.data(), out.size()) == out.size());
    return out;
}

// Header-only legacy .mti, structure version `ver`, 372-byte body.
std::vector<uint8_t> mtiBytes(const char* name, uint8_t ver) {
    Instrument ins;
    ins.setDefault();
    std::snprintf(ins.name, sizeof(ins.name), "%s", name);
    ins.lengthFrames = 12345;   // stale legacy length: ignored (spec 10 §3.3)
    std::vector<uint8_t> out(pti::kHeaderBlockSize);
    encodePtiHeader(ins, out.data());
    for (int i = 0; i < 4; ++i) out[8 + size_t(i)] = ver;
    return out;
}

// 8-track legacy pattern (6184 bytes), structure `ver`, note on track 1 step 1.
std::vector<uint8_t> legacyPattern(uint8_t ver, int8_t note) {
    Pattern p;
    p.setDefault();
    p.tracks[0].steps[0].note = note;
    p.tracks[0].steps[0].instrument = 0;
    std::vector<uint8_t> full(mtp::kFileSize);
    encodePattern(p, full.data());
    std::vector<uint8_t> out(full.begin(), full.begin() + mtp::kLegacyFileSize);
    for (int i = 0; i < 4; ++i) out[8 + size_t(i)] = ver;
    out[0x0C] = uint8_t(mtp::kLegacyFileSize & 0xFF);
    out[0x0D] = uint8_t(mtp::kLegacyFileSize >> 8);
    return out;
}

// Short legacy project.mt (v11, 1572 bytes; spec 10 §1.1).
std::vector<uint8_t> legacyProject() {
    Project p;
    p.setDefault();
    std::vector<uint8_t> full(mt::kFileSize);
    encodeProject(p, full.data());
    std::vector<uint8_t> out(full.begin(), full.begin() + 1572);
    for (int i = 0; i < 4; ++i) out[8 + size_t(i)] = 11;
    out[0x0C] = uint8_t(1572 & 0xFF);
    out[0x0D] = uint8_t(1572 >> 8);
    return out;
}

void addLegacyProject(Harness& h, const std::string& d, int wavFrames, int wavDeclaredFrames = -1) {
    h.fs.dirs.insert("Projects");
    h.fs.dirs.insert(d);
    h.fs.dirs.insert(d + "/patterns");
    h.fs.dirs.insert(d + "/instruments");
    h.fs.dirs.insert(d + "/samples");
    h.fs.files[d + "/project.mt"] = legacyProject();
    h.fs.files[d + "/patterns/pattern_01.mtp"] = legacyPattern(3, 48);
    h.fs.files[d + "/instruments/instrument_01.mti"] = mtiBytes("kick.wav", 8);
    auto w = wavBytes(wavFrames, 100);
    if (wavDeclaredFrames >= 0) {
        const uint32_t ds = uint32_t(wavDeclaredFrames) * 2;   // data chunk claims more than the file holds
        w[40] = uint8_t(ds);
        w[41] = uint8_t(ds >> 8);
        w[42] = uint8_t(ds >> 16);
        w[43] = uint8_t(ds >> 24);
    }
    h.fs.files[d + "/samples/instr01.wav"] = w;
}

}  // namespace

// ---------------------------------------------------------------------------
// Browser model

TEST(browser_lists_folders_and_projects) {
    Harness h;
    addProject(h, "Projects/b");
    addProject(h, "Projects/A/song");
    h.fs.dirs.insert("Projects/A/empty");
    ProjectBrowser br;
    br.bind(g_names, 64);
    br.reset(*h.app, kProjectsDir);
    CHECK(br.atRoot());
    CHECK(br.count() == 2);
    CHECK(std::strcmp(br.name(0), "A") == 0 && br.isFolder(0));
    CHECK(std::strcmp(br.name(1), "b") == 0 && br.isProject(1));
    CHECK(!br.enter(*h.app, 1));   // Enter on a project: no-op
    CHECK(br.enter(*h.app, 0));
    CHECK(std::strcmp(br.path(), "Projects/A") == 0);
    CHECK(br.count() == 3);
    CHECK(br.isUp(0));
    CHECK(std::strcmp(br.name(1), "empty") == 0 && br.isFolder(1));
    CHECK(std::strcmp(br.name(2), "song") == 0 && br.isProject(2));
    CHECK(br.enter(*h.app, 0));   // ".."
    CHECK(br.atRoot());
}

TEST(browser_new_folder_rules) {
    Harness h;
    ProjectBrowser br;
    br.bind(g_names, 64);
    br.reset(*h.app, kProjectsDir);
    CHECK(br.newFolder(*h.app, "New folder") == ProjectBrowser::Result::Ok);
    CHECK(std::strcmp(br.path(), "Projects/New folder") == 0);   // entered
    CHECK(h.fs.dirs.count("Projects/New folder"));
    CHECK(br.up(*h.app));
    CHECK(br.newFolder(*h.app, "New folder") == ProjectBrowser::Result::Exists);
    CHECK(br.newFolder(*h.app, "") == ProjectBrowser::Result::Invalid);
    // len(path) + len(name) + 2 >= 253 is refused.
    for (int i = 0; i < 7; ++i) CHECK(br.newFolder(*h.app, "abcdefghijklmnopqrstuvwxyz012345") == ProjectBrowser::Result::Ok);
    CHECK(std::strlen(br.path()) == 8 + 7 * 33);   // 239
    CHECK(br.newFolder(*h.app, "abcdefghijkl") == ProjectBrowser::Result::TooLong);
    CHECK(br.newFolder(*h.app, "abcdefghijk") == ProjectBrowser::Result::Ok);   // 239+11+2 = 252
}

TEST(browser_delete_refuses_active_project) {
    Harness h;
    App& a = *h.app;
    addProject(h, "Projects/other");
    h.fs.dirs.insert("Projects/A");
    CHECK(saveProjectAs(a, "Projects/A", "song"));
    ProjectBrowser br;
    br.bind(g_names, 64);
    br.reset(a, kProjectsDir);
    const int iA = br.find("A"), iO = br.find("other");
    CHECK(iA >= 0 && iO >= 0);
    CHECK(br.canDelete(a, iA) == ProjectBrowser::Result::ContainsActive);
    CHECK(br.remove(a, iA) == ProjectBrowser::Result::ContainsActive);
    CHECK(h.fs.files.count("Projects/A/song/project.mt"));
    CHECK(br.enter(a, iA));
    CHECK(br.canDelete(a, br.find("song")) == ProjectBrowser::Result::Active);
    CHECK(br.canDelete(a, 0) == ProjectBrowser::Result::Invalid);   // ".."
    CHECK(br.up(a));
    CHECK(br.remove(a, br.find("other")) == ProjectBrowser::Result::Ok);
    CHECK(!h.fs.exists("Projects/other"));
    CHECK(br.find("other") < 0);
    // A never-saved project protects nothing.
    a.newProject();
    CHECK(br.canDelete(a, br.find("A")) == ProjectBrowser::Result::Ok);
}

// ---------------------------------------------------------------------------
// File screen with folders

TEST(file_screen_save_as_into_subfolder_and_open) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Rec);
    h.pad(36);
    h.tap(btn::File);
    h.tap(btn::F5);   // Save As -> Save browser at "Projects"
    FileScreen& s = fileScr(h);
    CHECK(s.state == 3);
    h.tap(btn::F4);   // New Folder keyboard
    CHECK(a.ui.kb.isOpen());
    CHECK(std::strcmp(a.ui.kb.text(), "New folder") == 0);
    h.tap(btn::F8);   // Create -> entered
    CHECK(h.fs.dirs.count("Projects/New folder"));
    CHECK(s.count == 1);   // ".."
    h.tap(btn::F8);   // Save -> keyboard with the project name
    for (int i = 0; i < 40; ++i) h.tap(btn::Delete);
    h.type("deep");
    h.tap(btn::F8);
    CHECK(h.fs.files.count("Projects/New folder/deep/project.mt"));
    CHECK(std::strcmp(a.project.folder, "Projects/New folder") == 0);
    CHECK(s.state == 0);
    // New, then open it through the folder.
    h.tap(btn::F1);
    CHECK(std::strcmp(a.project.name, "New Project") == 0);
    h.tap(btn::F2);
    CHECK(s.state == 1 && s.count == 1);
    h.tap(btn::F8);   // a folder: Open does nothing
    CHECK(a.ui.active() == kModFile);
    h.tap(btn::F1);   // Enter
    CHECK(s.count == 2);
    h.tap(btn::Down);
    h.tap(btn::F8);
    CHECK(a.ui.active() == kModPattern);
    CHECK(std::strcmp(a.project.name, "deep") == 0);
    CHECK(a.pattern().tracks[0].steps[0].note == 36);
}

TEST(file_screen_delete_folder_with_active_project_refused) {
    Harness h;
    App& a = *h.app;
    h.fs.dirs.insert("Projects/F");
    CHECK(saveProjectAs(a, "Projects/F", "act"));
    h.tap(btn::File);
    h.tap(btn::F2);
    h.tap(btn::F3);
    CHECK(!a.ui.askOpen());   // refused (2 s message)
    CHECK(h.fs.dirs.count("Projects/F"));
    h.fs.dirs.insert("Projects/G");
    fileScr(h).list();
    h.tap(btn::Down);
    h.tap(btn::F3);
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);
    CHECK(!h.fs.dirs.count("Projects/G"));
}

// ---------------------------------------------------------------------------
// Recents

TEST(recents_compaction_and_mru_on_leave) {
    Harness h;
    App& a = *h.app;
    addProject(h, "Projects/b");
    addProject(h, "Other/c");
    Recents r;
    r.clear();
    std::snprintf(r.paths[0], Recents::kSlotSize, "Projects/gone");
    std::snprintf(r.paths[3], Recents::kSlotSize, "Other/c");
    CHECK(saveRecentsFile(h.fs, kRecentsPath, r) == FmtStatus::Ok);
    Recents l;
    CHECK(loadRecents(a, l) == 1);
    CHECK(std::strcmp(l.paths[0], "Other/c") == 0);
    CHECK(!l.paths[1][0]);
    // Open b, then leave it (New): b goes to the top.
    CHECK(a.openProject(kProjectsDir, "b"));
    CHECK(loadRecents(a, l) == 1);   // the current project is added on leave
    a.newProject();
    CHECK(loadRecents(a, l) == 2);
    CHECK(std::strcmp(l.paths[0], "Projects/b") == 0);
    CHECK(std::strcmp(l.paths[1], "Other/c") == 0);
    // Open from Recent by index.
    CHECK(openRecent(a, 1));
    CHECK(std::strcmp(a.project.folder, "Other") == 0);
    CHECK(std::strcmp(a.project.name, "c") == 0);
    CHECK(!openRecent(a, 5));
}

// Open Recent while the current project is saved on the card: leaving it
// rewrites the Recents list (recentsLeaveCurrent) in the middle of the open,
// and the list is one shared buffer (recentsBuffer()), so the entry to open
// must already be copied out.
TEST(open_recent_from_a_saved_project) {
    Harness h;
    App& a = *h.app;
    addProject(h, "Projects/b");
    addProject(h, "Projects/c");
    CHECK(a.openProject(kProjectsDir, "b"));
    CHECK(a.openProject(kProjectsDir, "c"));   // leaving b: recents = b
    Recents& l = recentsBuffer();
    CHECK(loadRecents(a, l) == 1);
    CHECK(std::strcmp(l.paths[0], "Projects/b") == 0);
    CHECK(openRecent(a, 0));                   // leaving c (saved): recents = c, b
    CHECK(std::strcmp(a.project.name, "b") == 0);
    CHECK(loadRecents(a, l) == 2);
    CHECK(std::strcmp(l.paths[0], "Projects/c") == 0);
    CHECK(std::strcmp(l.paths[1], "Projects/b") == 0);
    // The same through the job (startOpenRecent, the File screen's path).
    bool done = false, okDone = false;
    struct Ctx { bool* d; bool* o; } cx{&done, &okDone};
    CHECK(startOpenRecent(a, 0, [](void* p, bool ok) { auto* c = static_cast<Ctx*>(p); *c->d = true; *c->o = ok; }, &cx));
    a.finishJob();
    CHECK(done && okDone);
    CHECK(std::strcmp(a.project.name, "c") == 0);
    CHECK(loadRecents(a, l) == 2);
    CHECK(std::strcmp(l.paths[0], "Projects/b") == 0);
}

// ---------------------------------------------------------------------------
// Memory pre-check

TEST(open_refused_when_samples_exceed_bank) {
    Harness h(true, 4096);   // 8 KB sample bank
    App& a = *h.app;
    addProject(h, "Projects/big");
    h.fs.dirs.insert("Projects/big/instruments");
    Instrument ins;
    ins.setDefault();
    ins.lengthFrames = 6000;
    std::vector<int16_t> s(6000, 7);
    std::vector<uint8_t> f(ptiFileSize(6000, false));
    CHECK(encodePti(ins, s.data(), nullptr, f.data(), f.size()) == f.size());
    h.fs.files["Projects/big/instruments/1 big.pti"] = f;
    CHECK(projectSampleBytes(a, "Projects/big") == 12000);
    CHECK(!projectFitsMemory(a, kProjectsDir, "big"));
    h.tap(btn::Rec);
    h.pad(36);
    h.tap(btn::File);
    h.tap(btn::F2);
    h.tap(btn::F8);
    CHECK(!a.ui.askOpen());              // refused before the save prompt
    CHECK(a.ui.active() == kModFile);
    CHECK(!a.openProject(kProjectsDir, "big"));
    CHECK(a.pattern().tracks[0].steps[0].note == 36);   // workspace untouched
    h.fs.files["Projects/big/instruments/1 big.pti"].resize(pti::kDataOff + 4000);
    CHECK(projectFitsMemory(a, kProjectsDir, "big"));
}

// ---------------------------------------------------------------------------
// Legacy projects (spec 10)

TEST(open_legacy_project_converts_instruments) {
    Harness h;
    App& a = *h.app;
    addLegacyProject(h, "Projects/old", 1000);
    CHECK(projectSampleBytes(a, "Projects/old") == 2000);
    CHECK(a.openProject(kProjectsDir, "old"));
    CHECK(std::strcmp(a.project.name, "old") == 0);
    CHECK(std::strcmp(a.project.folder, kProjectsDir) == 0);
    CHECK(!a.neverSaved());
    CHECK(!a.unsavedChanges());
    // Workspace/project.mt written as v17 with the selected name (spec 10 §1.5).
    const auto& mtf = h.fs.files["Workspace/project.mt"];
    CHECK(mtf.size() == mt::kFileSize);
    CHECK(mtf[8] == 17);
    // .mti renamed and rebuilt as a full v9 .pti (spec 10 §3.3).
    CHECK(!h.fs.files.count("Workspace/instruments/instrument_01.mti"));
    const auto& ptf = h.fs.files["Workspace/instruments/instrument_01.pti"];
    CHECK(ptf.size() == pti::kDataOff + 2000);
    CHECK(ptf[8] == 9);
    // OpenTracker imports the WAV straight from the project folder (no copy
    // into Workspace/samples first; same importer, half the card traffic).
    CHECK(!h.fs.files.count("Workspace/samples/instr01.wav"));
    CHECK(a.instrPresent[0]);
    CHECK(a.instruments[0].lengthFrames == 1000);
    CHECK(std::strcmp(a.instruments[0].name, "kick") == 0);   // ".wav" stripped
    CHECK(a.bank.left(0)[0] == 100 && a.bank.left(0)[999] == 1099);
    // Pattern upgraded on load; the Workspace copy is a Tracker Mini 16-track
    // file (2.x, docs/MINI_PLAN.md §1).
    CHECK(a.pattern().tracks[0].steps[0].note == 48);
    CHECK(h.fs.files["Workspace/patterns/pattern_01.mtp"].size() == mtp::kMiniFileSize);
    CHECK(h.fs.files.count("Workspace/patterns/patternsMetadata"));
    // Save in place: 1.9.2 layout; legacy .mti removed, samples/ kept (README deviation).
    CHECK(canDirectSave(a));
    CHECK(a.saveProject());
    CHECK(h.fs.files.count("Projects/old/instruments/1 kick.pti"));
    CHECK(!h.fs.files.count("Projects/old/instruments/instrument_01.mti"));
    CHECK(h.fs.files.count("Projects/old/samples/instr01.wav"));
    CHECK(h.fs.files["Projects/old/project.mt"].size() == mt::kFileSize);
    CHECK(h.fs.files["Projects/old/patterns/pattern_01.mtp"].size() == mtp::kMiniFileSize);
    // Reopen: now through the .pti path.
    a.newProject();
    CHECK(a.openProject(kProjectsDir, "old"));
    CHECK(a.instrPresent[0] && a.instruments[0].lengthFrames == 1000);
    CHECK(a.bank.left(0)[500] == 600);
    CHECK(projectSampleBytes(a, "Projects/old") == 2000);
}

TEST(legacy_wav_short_file_tail_zero_filled) {
    Harness h;
    App& a = *h.app;
    addLegacyProject(h, "Projects/short", 1000, 2000);
    std::fill(h.bank.begin(), h.bank.end(), int16_t(0x1234));   // stale SDRAM
    CHECK(a.openProject(kProjectsDir, "short"));
    CHECK(a.instrPresent[0]);
    const uint32_t n = a.instruments[0].lengthFrames;
    CHECK(n >= 1000);
    const int16_t* s = a.bank.left(0);
    CHECK(s[999] == 1099);
    bool zero = true;
    for (uint32_t i = 1000; i < n; ++i) zero = zero && s[i] == 0;
    CHECK(zero);
}

TEST(legacy_mti_without_wav_clears_slot) {
    Harness h;
    App& a = *h.app;
    addLegacyProject(h, "Projects/nowav", 100);
    h.fs.files.erase("Projects/nowav/samples/instr01.wav");
    CHECK(a.openProject(kProjectsDir, "nowav"));
    CHECK(!a.instrPresent[0]);
}

TEST(boot_with_legacy_workspace_is_unsaved_project) {
    Harness h(false);
    h.fs.dirs.insert("Workspace");
    h.fs.dirs.insert("Workspace/instruments");
    h.fs.dirs.insert("Workspace/patterns");
    h.fs.dirs.insert("Workspace/samples");
    h.fs.files["Workspace/project.mt"] = legacyProject();
    h.fs.files["Workspace/instruments/instrument_01.mti"] = mtiBytes("snare.wav", 6);
    h.fs.files["Workspace/samples/instr01.wav"] = wavBytes(300, -50);
    h.fs.files["Workspace/patterns/pattern_01.mtp"] = legacyPattern(1, 50);
    App& a = *h.app;
    CHECK(a.boot());
    CHECK(std::strcmp(a.project.name, "Unsaved Project") == 0);
    CHECK(!a.project.folder[0]);
    CHECK(a.neverSaved());
    CHECK(a.unsavedChanges());
    CHECK(a.instrPresent[0] && a.instruments[0].lengthFrames == 300);
    CHECK(h.fs.files["Workspace/instruments/instrument_01.pti"].size() == pti::kDataOff + 600);
    CHECK(a.pattern().tracks[0].steps[0].note == 50);
    CHECK(!canDirectSave(a));
}

// ---------------------------------------------------------------------------
// Card removed while running

namespace {
class RemovableStorage : public FakeStorage {
public:
    bool removed = false;
    int32_t listDir(const char* path, DirFn fn, void* ctx) override {
        if (removed) return -1;
        return FakeStorage::listDir(path, fn, ctx);
    }
};
}  // namespace

TEST(card_removed_screen_and_reload) {
    Harness h(false);
    RemovableStorage rs;
    HalRefs hr = h.app->hal();
    hr.storage = &rs;
    App& a = *h.app;
    CHECK(a.init(hr, a.mem()));
    CHECK(a.boot());
    CHECK(!serviceCardRemoved(a, 10000));
    a.togglePlay(false);
    CHECK(a.seq.isPlaying());
    rs.removed = true;
    CHECK(!serviceCardRemoved(a, 10100));   // within the probe interval
    CHECK(serviceCardRemoved(a, 10600));
    CHECK(cardMissing());
    CHECK(!a.seq.isPlaying());
    CHECK(h.disp.lastHasText("Please insert SD card to continue"));
    CHECK(serviceCardRemoved(a, 10700));
    rs.removed = false;
    CHECK(!serviceCardRemoved(a, 11200));
    CHECK(!cardMissing());
}

// ---------------------------------------------------------------------------
// User's old SD card (skipped when absent): open + play a 2021 project.

TEST(fixture_open_and_play_old_project) {
    namespace sfs = std::filesystem;
    std::string root;
    if (const char* e = std::getenv("OT_FIXTURES")) root = std::string(e) + "/../../../fixtures";
    else root = "../../fixtures";
    const sfs::path src = sfs::path(root) / "sdcard-2021/Projects/lqrdo";
    std::error_code ec;
    if (!sfs::is_directory(src, ec)) {
        std::printf("  SKIP: %s absent\n", src.string().c_str());
        return;
    }
    Harness h;
    App& a = *h.app;
    h.fs.dirs.insert("Projects/lqrdo");
    for (auto it = sfs::recursive_directory_iterator(src, ec); it != sfs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string rel = "Projects/lqrdo/" + sfs::relative(it->path(), src).generic_string();
        if (it->is_directory()) {
            h.fs.dirs.insert(rel);
        } else {
            std::ifstream f(it->path(), std::ios::binary);
            h.fs.files[rel] = std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
    }
    CHECK(projectFitsMemory(a, kProjectsDir, "lqrdo"));
    CHECK(a.openProject(kProjectsDir, "lqrdo"));
    int present = 0;
    for (int i = 0; i < kSampleInstrumentCount; ++i) present += a.instrPresent[i];
    CHECK(present == 5);
    CHECK(h.fs.files.count("Workspace/instruments/instrument_05.pti"));
    CHECK(!h.fs.files.count("Workspace/instruments/instrument_05.mti"));
    h.tap(btn::Pattern);
    h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    CHECK(h.audio.pump(3000) > 100);
}
