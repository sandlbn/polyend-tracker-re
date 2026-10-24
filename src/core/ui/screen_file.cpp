// File / project manager (spec 05 §2.12 and §5.5, appendix E1-E3).
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "app/project_browser.h"
#include "app/project_io.h"
#include "app/render.h"
#include "formats/file_io.h"
#include "formats/it_export.h"
#include "formats/it_import.h"
#include "formats/mod_import.h"
#include "ui/screen_games.h"
#include "ui/screens.h"
#include "sdram_bss.h"

namespace ot {

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Browser of the Open (state 1) and Save (state 3) pages; its names live in
// FileScreen::entries (spec 05 appendix E1.2).
OT_SDRAM_BSS SdramLazy<ProjectBrowser> g_browserMem;   // SDRAM on the device (CPU round 3)
ProjectBrowser& projBrowser() { return g_browserMem.get(); }

// Target remembered for a deferred open (spec 05 appendix E1.2: heap copies
// of the path/name; pending 2 = SD list, 3 = Recent list).
OT_SDRAM_BSS char g_openFolder[ProjectBrowser::kPathMax + 1];
char g_openName[ProjectBrowser::kNameCap];
int g_openRecent = 0;
int g_openKind = 2;

char g_saveName[mt::kNameMax + 1];
App* g_app = nullptr;   // the File screen's App (static keyboard callbacks)

// Completion of a background open (spec 05 appendix E3 result 0x1a): the
// Pattern editor is shown, like the stock boot / open flow.
void openDone(void* ctx, bool ok) {
    auto* s = static_cast<FileScreen*>(ctx);
    App& a = *g_app;
    if (!ok) {
        a.ui.notify("Cannot open project");
        return;
    }
    a.ui.notify(a.lastJobErrors() ? "Opened; some files could not be read" : "Project opened");
    s->state = 0;
    a.ui.switchTo(kModPattern);
}

bool recentPath(App& a, int index, char* folder, int fcap, char* name, int ncap) {
    Recents& r = recentsBuffer();
    const int n = loadRecents(a, r);
    if (index < 0 || index >= n) return false;
    return splitProjectPath(r.paths[index], folder, fcap, name, ncap);
}

// ---- Import Mod browser (state 2; spec 05 appendix E1.2, spec 11 §1.1) ----
constexpr const char* kModDir = "Mod";
constexpr int kModEntries = ProjectBrowser::kMaxEntries;   // the same limit as Open (2.x: 200)
constexpr int kModNameCap = 56;
enum : uint8_t { kMbUp = 0, kMbFolder = 1, kMbFile = 2 };
char g_modPath[ProjectBrowser::kPathMax + 1] = "Mod";
// In browserListMem() (screens.h): names, then the kinds.
static_assert(kModEntries * (kModNameCap + 1) <= kBrowserListBytes, "Import Mod list");
char (*const g_modNames)[kModNameCap] = reinterpret_cast<char (*)[kModNameCap]>(g_browserLists);
uint8_t* const g_modKinds = g_browserLists + kModEntries * kModNameCap;
int g_modCount = 0;
OT_SDRAM_BSS char g_importPath[ProjectBrowser::kPathMax + kModNameCap + 2];
char g_importName[kModNameCap];

bool modListFn(const IStorage::DirEntry& e, void*) {
    if (e.name[0] == '.' || g_modCount >= kModEntries) return g_modCount < kModEntries;
    if (std::strlen(e.name) >= size_t(kModNameCap)) return true;   // too long to import by path
    if (!e.isDir && !moduleListFilter(e.name)) return true;          // spec 11 §1.1 filter 5
    std::snprintf(g_modNames[g_modCount], kModNameCap, "%s", e.name);
    g_modKinds[g_modCount++] = e.isDir ? kMbFolder : kMbFile;
    return true;
}

// ".." first (below the root), then folders, then files, by name.
void modList(App& a) {
    g_modCount = 0;
    if (std::strcmp(g_modPath, kModDir) != 0) {
        std::snprintf(g_modNames[0], kModNameCap, "..");
        g_modKinds[0] = kMbUp;
        g_modCount = 1;
    }
    const int first = g_modCount;
    // spec 05 §1.3 storage-state progress popup while the list loads (stock
    // title table 0xa2d54 "Loading files list").
    a.ui.progress(kLoadingFiles, 0);
    a.fs().listDir(g_modPath, modListFn, nullptr);
    a.ui.progressDone();
    for (int i = first + 1; i < g_modCount; ++i)
        for (int j = i; j > first; --j) {
            const bool swap = g_modKinds[j] < g_modKinds[j - 1] ||
                              (g_modKinds[j] == g_modKinds[j - 1] && std::strcmp(g_modNames[j], g_modNames[j - 1]) < 0);
            if (!swap) break;
            char t[kModNameCap];
            std::memcpy(t, g_modNames[j], kModNameCap);
            std::memcpy(g_modNames[j], g_modNames[j - 1], kModNameCap);
            std::memcpy(g_modNames[j - 1], t, kModNameCap);
            const uint8_t k = g_modKinds[j];
            g_modKinds[j] = g_modKinds[j - 1];
            g_modKinds[j - 1] = k;
        }
}

bool modIsFile(int i) { return i >= 0 && i < g_modCount && g_modKinds[i] == kMbFile; }
bool modIsDir(int i) { return i >= 0 && i < g_modCount && g_modKinds[i] != kMbFile; }

// ---- background import / export (spec 11 §0.1 sub-ops 0x13 / 0x14) -------
// SDRAM on the device (CPU round 3): main-loop jobs.
OT_SDRAM_BSS SdramLazy<ModImport> g_modImportMem;
OT_SDRAM_BSS SdramLazy<ItImport> g_itImportMem;
OT_SDRAM_BSS SdramLazy<ItExport> g_itExportMem;
ModImport& modImport() { return g_modImportMem.get(); }
ItImport& itImport() { return g_itImportMem.get(); }
ItExport& itExport() { return g_itExportMem.get(); }
ModuleKind g_importKind = ModuleKind::None;
char g_exportPath[mt::kNameMax + 16];

int16_t* reserveSample(void* ctx, int slot, uint32_t frames, bool stereo) {
    return static_cast<App*>(ctx)->bank.reserve(slot, frames, stereo);
}
PatternStock* importPattern(void* ctx, int number) {
    App& a = *static_cast<App*>(ctx);
    a.markPatternDirty(number);
    a.clearExtTracks(number);   // the import overwrites the whole pattern (12 channels)
    return &a.storedStock(number);
}
int importUnit(void*, uint32_t& cost) {
    const ImportStep r = g_importKind == ModuleKind::Mod ? modImport().step(cost) : itImport().step(cost);
    return int(r);
}
int importPercent(void*) { return g_importKind == ModuleKind::Mod ? modImport().percent() : itImport().percent(); }
const char* importError() { return g_importKind == ModuleKind::Mod ? modImport().error() : itImport().error(); }
const char* importWarning() { return g_importKind == ModuleKind::Mod ? modImport().warning() : itImport().warning(); }

// spec 11 §1.1 Result: pattern 1 is current, the project is changed (the
// Workspace files are written by the autosave), the Pattern editor shows.
void importDone(void* ctx, bool ok) {
    auto* s = static_cast<FileScreen*>(ctx);
    App& a = *g_app;
    if (g_importKind == ModuleKind::Mod) modImport().abort();
    else itImport().abort();
    for (int i = 0; i < kSampleInstrumentCount; ++i)
        if (a.instrPresent[i]) a.markInstrumentDirty(i, true);
    a.project.setCurrentPattern(1);
    a.markPatternDirty(1);
    a.markProjectDirty();
    a.reloadCurrentFromStore();
    a.refreshPadNotes();
    if (!ok) {
        a.ui.notify(importError()[0] ? importError() : "Import failed");
        return;
    }
    a.ui.notify(importWarning()[0] ? importWarning() : "Module imported");
    s->state = 0;
    a.ui.switchTo(kModPattern);
}

const int16_t* exportSamples(void* ctx, int slot, uint32_t* frames, bool* stereo) {
    App& a = *static_cast<App*>(ctx);
    *frames = a.bank.frames(slot);
    *stereo = a.bank.stereo(slot);
    return *frames ? a.bank.left(slot) : nullptr;
}
const PatternStock* exportPattern(void* ctx, int number) {
    App& a = *static_cast<App*>(ctx);
    return a.patternPresent(number) ? &a.storedStock(number) : nullptr;
}
int exportUnit(void*, uint32_t& cost) { return int(itExport().step(cost)); }
int exportPercent(void*) { return itExport().percent(); }
void exportDone(void*, bool ok) {
    App& a = *g_app;
    itExport().abort();
    char msg[64];
    if (ok) std::snprintf(msg, sizeof(msg), "Exported %s", g_exportPath);
    else std::snprintf(msg, sizeof(msg), "%s", itExport().error()[0] ? itExport().error() : "Export failed");
    a.ui.notify(msg);
}
}  // namespace

// Import the module remembered in g_importPath (spec 11 §1.1 steps 1-4).
static void importModule(FileScreen& s, App& a) {
    a.finishJob();
    g_importKind = moduleKind(g_importName);
    const AppMemory& m = a.mem();
    bool ok = false;
    if (g_importKind == ModuleKind::Mod) ok = modImport().begin(a.fs(), g_importPath, m.scratch, m.scratchBytes);
    else if (g_importKind == ModuleKind::It) ok = itImport().begin(a.fs(), g_importPath, m.scratch, m.scratchBytes);
    if (!ok) {
        // Refused before anything changed: the current project stays.
        a.ui.notify(g_importKind == ModuleKind::None ? "Unknown file type" : importError());
        return;
    }
    // Step 1: a new project named after the file (up to the first '.').
    a.newProject();
    char name[mt::kNameMax + 1];
    moduleProjectName(g_importName, name, sizeof(name));
    a.project.setName(name);
    // newProject() resolves and saves the synth patches through the same
    // scratch buffer (resolvePatch, savePatch), so the tables begin() read
    // into it are gone (the IT sample and pattern offsets): read them again.
    if (g_importKind == ModuleKind::Mod) ok = modImport().begin(a.fs(), g_importPath, m.scratch, m.scratchBytes);
    else ok = itImport().begin(a.fs(), g_importPath, m.scratch, m.scratchBytes);
    if (!ok) {
        a.ui.notify(importError());
        return;
    }
    ModuleTarget t;
    t.project = &a.project;
    t.instruments = a.instruments;
    t.present = a.instrPresent;
    t.reserve = reserveSample;
    t.pattern = importPattern;
    t.ctx = &a;
    if (g_importKind == ModuleKind::Mod) modImport().apply(t);
    else itImport().apply(t);
    if (!a.startExternalJob("Importing mod file", importUnit, importPercent, nullptr, importDone, &s)) {
        a.ui.notify("Import failed");
        return;
    }
}

// Export "As .it" to Mod/<project>.it (spec 11 §2.1). OpenTracker creates
// Mod/ when it is missing (stock fails then).
static void exportIt(App& a) {
    a.finishJob();
    a.seq.stop();
    a.commitCurrentPattern();
    a.fs().mkdir(kModDir);
    std::snprintf(g_exportPath, sizeof(g_exportPath), "%s/%s.it", kModDir,
                  a.project.name[0] ? a.project.name : "New Project");
    ItExportSource src;
    src.project = &a.project;
    src.instruments = a.instruments;
    src.present = a.instrPresent;
    src.samples = exportSamples;
    src.pattern = exportPattern;
    src.ctx = &a;
    const AppMemory& m = a.mem();
    if (!itExport().begin(a.fs(), g_exportPath, src, m.scratch, m.scratchBytes)) {
        a.ui.notify(itExport().error());
        return;
    }
    if (!a.startExternalJob("Exporting to .it file", exportUnit, exportPercent, nullptr, exportDone, nullptr)) {
        itExport().abort();
        a.ui.notify("Export failed");
    }
}

void FileScreen::start(int opt) {
    (void)opt;
    g_app = &app_;
    projBrowser().bind(entries, ProjectBrowser::kMaxEntries);
    state = 0;
    refreshBar();
}

// spec 05 appendix E1.6 / spec 13 §I.3: when a project stays selected in the
// browser for more than 2000 ms, "<project folder>/cover" (.jpg, else .jpeg;
// <= 400 x 400 px, <= 330,000 bytes) is decoded into RAM_G 670,000 and shown
// in the 400 x 400 preview area. Runs from the screen update, outside the
// display list. SPEC GAP: only projects of the SD list (focused) get a cover;
// the preview position is OpenTracker's (right of the SD list).
void FileScreen::update(uint32_t now) {
    char key[sizeof(coverKey_)] = {};
    const bool browser = (state == 1 || state == 3) && focus == 0 && !ui_.kb.isOpen() && !ui_.askOpen();
    if (browser && projBrowser().isProject(cursor)) projBrowser().entryPath(cursor, key, int(sizeof(key)));
    if (std::strcmp(key, coverKey_) != 0) {
        if (coverState == 2) ui_.dirty();
        std::snprintf(coverKey_, sizeof(coverKey_), "%s", key);
        coverState = key[0] ? 1 : 0;
        coverAt_ = now;
        coverW = coverH = 0;
        return;
    }
    if (coverState != 1 || now - coverAt_ <= 2000) return;
    coverState = 3;
    IDisplay* d = app_.hal().display;
    if (!d) return;
    char base[sizeof(coverKey_) + 8];
    std::snprintf(base, sizeof(base), "%s/cover", coverKey_);
    uint16_t w = 0, h = 0;
    if (d->loadJpegBase(IDisplay::kRamGCover, app_.fs(), base, IDisplay::kCoverMaxSide, IDisplay::kCoverMaxSide, w,
                        h)) {
        coverState = 2;
        coverW = w;
        coverH = h;
        ui_.dirty();
    }
}

void FileScreen::list() {
    App& a = app_;
    g_app = &a;
    projBrowser().bind(entries, ProjectBrowser::kMaxEntries);
    // spec 05 §1.3 progress popup while the list loads ("Loading projects list").
    ui_.progress(kLoadingProjects, 0);
    projBrowser().list(a);
    count = projBrowser().count();
    // Recent list (spec 02 §7, appendix E2): compacted, last path component.
    Recents& rec = recentsBuffer();
    recentCount_ = loadRecents(a, rec);
    for (int i = 0; i < recentCount_; ++i) Recents::displayName(rec.paths[i], recent_[i], sizeof(recent_[0]));
    ui_.progressDone();
    cursor = clampi(cursor, 0, (focus ? recentCount_ : count) > 0 ? (focus ? recentCount_ : count) - 1 : 0);
}

void FileScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    b.clear();
    if (state == 0) {
        b.setName(0, "New", "project");
        b.set(1, "Open");
        b.set(2, "Import Mod");
        b.set(3, "Save");
        b.set(4, "Save As");
        b.set(5, "Export");
#if OT_GAMES
        b.set(6, "Games");   // spec 14 §1: File F7 opens the Games browser
#endif
    } else if (state == 1 || state == 3) {
        // spec 05 appendix E1.2: labels greyed by selection kind (0x4742c).
        const bool sd = focus == 0;
        b.set(0, "Enter", nullptr, sd && (projBrowser().isFolder(cursor) || projBrowser().isUp(cursor)));
        b.set(2, "Delete", nullptr, sd && (projBrowser().isFolder(cursor) || projBrowser().isProject(cursor)));
        b.set(3, "New Folder", nullptr, sd);
        b.set(6, "Cancel");
        if (state == 1) b.set(7, "Open", nullptr, sd ? projBrowser().isProject(cursor) : recentCount_ > 0);
        else b.set(7, "Save");
    } else if (state == 2) {
        // spec 05 appendix E1.2 import mode (0x478c0).
        b.set(0, "Enter", nullptr, modIsDir(cursor));
        b.set(2, "Delete", nullptr, modIsFile(cursor));
        b.set(6, "Cancel");
        b.set(7, "Import", nullptr, modIsFile(cursor));
    } else if (state == 4) {
        b.setName(0, "Export", "Song");
        b.setName(1, "Export", "Song Stems");
        b.setName(2, "Export", "Pattern");
        b.setName(3, "Export", "Patt Stems");   // [S snap 027]
        b.setName(4, "Export", "As .it");
        b.setName(5, "Export", "Samples");
        b.set(7, "Back");
        // Two-line grey set (+0x44): both lines font 3, drawn by draw()
        // [S snap 027]; the labels keep their text and enable state.
        for (int i = 0; i < 6; ++i) b.label[i].hidden = true;
    }
}

// Opens the Save browser (state 3) or the Open browser (state 1) at the root
// "Projects" (spec 05 appendix E1.2).
static void enterBrowser(FileScreen& s, int mode) {
    s.state = mode;
    s.focus = 0;
    s.cursor = 0;
    projBrowser().bind(s.entries, ProjectBrowser::kMaxEntries);
    g_app->ui.progress(kLoadingProjects, 0);   // closed by list()
    projBrowser().reset(*g_app, kProjectsDir);
    s.list();
}

static void runPending(FileScreen& s, App& a);

// Save finished (op 0x0d result 0x1b): report, then run the pending action.
static void saveDone(void* ctx, bool ok) {
    auto* s = static_cast<FileScreen*>(ctx);
    App& a = *g_app;
    a.ui.notify(ok ? "Saved" : "Save failed");
    if (ok) runPending(*s, a);
    else s->pending = 0;
}

// Runs the action remembered from the "save changes?" popup once the save is
// done (spec 05 appendix E1, pending 1 = new, 2 = open SD, 3 = open Recent).
static void runPending(FileScreen& s, App& a) {
    const int act = s.pending;
    s.pending = 0;
    if (act == 1) {
        a.newProject();
        a.ui.notify("New project");
    } else if (act == 2 || act == 3) {
        s.doOpenSelected();
    } else if (act == 4) {
        importModule(s, a);
    } else if (act == 5) {
        exportIt(a);
    }
}

void FileScreen::askSaveThen(int action) {
    pending = action;
    char text[64];
    std::snprintf(text, sizeof(text), "Save the changes to \"%s\"?", app_.project.name);
    ui_.ask("Unsaved changes", text, "Cancel", "Don't Save", "Save",
            [](void* ctx, int key) {
                auto* self = static_cast<FileScreen*>(ctx);
                App& a = self->app_;
                if (key == btn::F6) {
                    self->pending = 0;
                    return;
                }
                if (key == btn::F8) {
                    if (!canDirectSave(a)) {
                        // spec 05 appendix E1.1/E1.3: a never-saved project goes
                        // through Save As (the Save browser); the action runs
                        // once it is saved.
                        enterBrowser(*self, 3);
                        self->refreshBar();
                        return;
                    }
                    // The action runs only after a completed save (E3 result 0x1b).
                    if (!a.startSaveProject(saveDone, self)) {
                        self->ui_.notify("Save failed");
                        self->pending = 0;
                    }
                    return;
                }
                runPending(*self, a);
                self->refreshBar();
            },
            this);
}

void FileScreen::doNew() {
    App& a = app_;
    if (a.seq.isPlaying()) {
        // spec 05 appendix E1.1 / E6: stop-pattern confirmation first (pending 2).
        ui_.ask("This action will stop the pattern", "continue?", nullptr, "Cancel", "Yes",
                [](void* ctx, int key) {
                    auto* self = static_cast<FileScreen*>(ctx);
                    if (key != btn::F8) return;
                    self->app_.seq.stop();
                    self->doNew();
                    self->refreshBar();
                },
                this);
        return;
    }
    if (a.unsavedChanges()) {
        askSaveThen(1);
        return;
    }
    a.newProject();
    ui_.notify("New project");
}

void FileScreen::doOpenSelected() {
    // spec 05 appendix E3 OPEN: op 9 runs in the background with the
    // progress popup; openDone() switches to the Pattern editor.
    App& a = app_;
    g_app = &a;
    const bool started = g_openKind == 3 ? startOpenRecent(a, g_openRecent, openDone, this)
                                         : a.startOpenProject(g_openFolder, g_openName, openDone, this);
    if (!started) ui_.notify("Cannot open project");
}

// ---- Save page keyboard (spec 05 appendix E1.2 SAVE, 0x45324 / 0x4688c) ----
static void openSaveKeyboard(FileScreen& s, App& a, const char* initial);

static void saveNow(void* ctx, int key) {
    auto* s = static_cast<FileScreen*>(ctx);
    App& a = *g_app;
    if (key != btn::F8) {
        openSaveKeyboard(*s, a, g_saveName);   // Overwrite -> Cancel: back to the keyboard
        return;
    }
    // spec 05 appendix E3: Save As into the Save browser's current folder.
    s->start(0);   // back to the File main page
    if (!startSaveProjectInto(a, projBrowser().path(), g_saveName, saveDone, s)) {
        a.ui.notify("Save failed");
        s->pending = 0;
    }
}

static void saveKeyboardDone(void* ctx, bool ok, const char* text) {
    auto* s = static_cast<FileScreen*>(ctx);
    App& a = *g_app;
    if (!ok) return;   // keyboard Cancel: back to the Save browser
    std::snprintf(g_saveName, sizeof(g_saveName), "%s", text);
    if (!g_saveName[0]) {
        // spec 05 appendix E1.2: Save requires 1..32 chars; the keyboard stays.
        a.ui.notify("Name cannot be empty");
        openSaveKeyboard(*s, a, g_saveName);
        return;
    }
    if (a.projectExists(projBrowser().path(), g_saveName)) {
        a.ui.ask("Project exists", "Overwrite it?", nullptr, "Cancel", "Overwrite", saveNow, s);
        return;
    }
    saveNow(s, btn::F8);
}

static void openSaveKeyboard(FileScreen& s, App& a, const char* initial) {
    a.ui.kb.open("Save project", initial, mt::kNameMax, "Save", saveKeyboardDone, &s);
}

// "This action will stop the pattern / continue?" (spec 05 appendix E1,
// E6: Cancel F7, Yes F8), then the action again with the sequencer stopped.
// Open and Import (both 0x46970) need a stopped sequencer; the owner's 1.9.2
// device asks first instead of ignoring the key (field report, 1.9.5).
void FileScreen::askStopThen(int action) {
    stopThen_ = action;
    ui_.ask("This action will stop the pattern", "continue?", nullptr, "Cancel", "Yes",
            [](void* ctx, int key) {
                auto* self = static_cast<FileScreen*>(ctx);
                const int act = self->stopThen_;
                self->stopThen_ = 0;
                if (key != btn::F8) return;
                self->app_.seq.stop();
                if (act == 1) self->openAction();
                else if (act == 2) self->importAction();
                self->refreshBar();
            },
            this);
}

// OPEN selected (spec 05 appendix E1.2, btn7 0x46970 / id 0x21 0x46ab4).
void FileScreen::openAction() {
    App& a = app_;
    g_app = &a;
    if (focus == 0) {
        if (!projBrowser().isProject(cursor)) return;
    } else if (recentCount_ <= 0) {
        return;
    }
    if (a.seq.isPlaying()) {
        askStopThen(1);
        return;
    }
    if (focus == 0) {
        std::snprintf(g_openFolder, sizeof(g_openFolder), "%s", projBrowser().path());
        std::snprintf(g_openName, sizeof(g_openName), "%s", entries[cursor]);
        g_openKind = 2;
    } else {
        g_openRecent = clampi(cursor, 0, recentCount_ - 1);
        if (!recentPath(a, g_openRecent, g_openFolder, sizeof(g_openFolder), g_openName, sizeof(g_openName)))
            return;
        g_openKind = 3;
    }
    // spec 02 §1.1 / §8.2: sample-memory pre-check before opening.
    if (!projectFitsMemory(a, g_openFolder, g_openName)) {
        ui_.notify("Not enough sample memory");
        return;
    }
    if (a.unsavedChanges()) askSaveThen(g_openKind);
    else doOpenSelected();
}

// IMPORT (Import browser btn7, 0x46970): a .mod / .it file.
void FileScreen::importAction() {
    App& a = app_;
    g_app = &a;
    if (!modIsFile(cursor)) return;
    if (a.seq.isPlaying()) {
        askStopThen(2);
        return;
    }
    std::snprintf(g_importName, sizeof(g_importName), "%s", g_modNames[cursor]);
    std::snprintf(g_importPath, sizeof(g_importPath), "%s/%s", g_modPath, g_modNames[cursor]);
    if (a.unsavedChanges()) askSaveThen(4);   // pending 4 = import mod
    else importModule(*this, a);
}

// Import Mod browser buttons (spec 05 appendix E1.2 import mode, spec 11 §1.1).
void FileScreen::onImportButton(uint8_t id) {
    App& a = app_;
    count = g_modCount;
    switch (id) {
        case btn::F7:   // Cancel
            state = 0;
            pending = 0;
            break;
        case btn::F1:   // Enter: a folder, or ".." up
            if (!modIsDir(cursor)) break;
            if (g_modKinds[cursor] == kMbUp) {
                char* sl = std::strrchr(g_modPath, '/');
                if (sl) *sl = 0;
            } else if (std::strlen(g_modPath) + std::strlen(g_modNames[cursor]) + 2 < sizeof(g_modPath)) {
                std::strcat(g_modPath, "/");
                std::strcat(g_modPath, g_modNames[cursor]);
            }
            modList(a);
            count = g_modCount;
            cursor = 0;
            break;
        case btn::F3: {   // Delete file (0x45520)
            if (!modIsFile(cursor)) break;
            char text[64];
            std::snprintf(text, sizeof(text), "\"%.48s\"?", g_modNames[cursor]);
            ui_.ask("Delete file:", text, nullptr, "Cancel", "Delete",
                    [](void* ctx, int key) {
                        auto* self = static_cast<FileScreen*>(ctx);
                        if (key != btn::F8 || !modIsFile(self->cursor)) return;
                        char path[sizeof(g_importPath)];
                        std::snprintf(path, sizeof(path), "%s/%s", g_modPath, g_modNames[self->cursor]);
                        if (!self->app_.fs().remove(path)) self->ui_.notify("Delete failed");
                        modList(self->app_);
                        self->count = g_modCount;
                        self->cursor = clampi(self->cursor, 0, self->count ? self->count - 1 : 0);
                        self->refreshBar();
                    },
                    this);
            break;
        }
        case btn::F8:
        case btn::EncPush:
            importAction();
            break;
        case btn::Up: cursor = clampi(cursor - 1, 0, count - 1); break;
        case btn::Down: cursor = clampi(cursor + 1, 0, count - 1); break;
        default: break;
    }
}

void FileScreen::onButton(uint8_t id, uint8_t st) {
    App& a = app_;
    g_app = &a;
    if (st != kPress) return;
    if (state == 0) {
        switch (id) {
            case btn::F1: doNew(); break;
            case btn::F2: enterBrowser(*this, 1); break;
#if OT_GAMES
            case btn::F7:   // spec 14 §1: Games browser
                openGames(a);
                return;
#endif
            case btn::F3:
                // spec 05 appendix E1.2 / spec 11 §1.1: browser mode 2 at "Mod";
                // Projects/ and Mod/ are created when missing (0x1f274).
                a.fs().mkdir(kProjectsDir);
                a.fs().mkdir(kModDir);
                std::snprintf(g_modPath, sizeof(g_modPath), "%s", kModDir);
                modList(a);
                state = 2;
                cursor = 0;
                focus = 0;
                count = g_modCount;
                break;
            case btn::F4:
                // spec 05 appendix E1.3: direct save into "<folder>/<name>".
                if (canDirectSave(a)) {
                    pending = 0;
                    if (!a.startSaveProject(saveDone, this)) ui_.notify("Save failed");
                    break;
                }
                [[fallthrough]];
            case btn::F5:
                pending = 0;
                enterBrowser(*this, 3);
                break;
            case btn::F6:
                if (a.seq.isPlaying()) {
                    // spec 05 appendix E1.4: stop-pattern confirmation first (pending 3).
                    ui_.ask("This action will stop the pattern", "continue?", nullptr, "Cancel", "Yes",
                            [](void* ctx, int key) {
                                auto* self = static_cast<FileScreen*>(ctx);
                                if (key != btn::F8) return;
                                self->app_.seq.stop();
                                self->state = 4;
                                self->refreshBar();
                            },
                            this);
                    break;
                }
                state = 4;
                break;
            default:
                Screen::onButton(id, st);
                break;
        }
    } else if (state == 1 || state == 3) {
        const int n = focus ? recentCount_ : count;
        switch (id) {
            case btn::F7:   // Cancel: back to the File main page (0x45a9c)
                state = 0;
                pending = 0;
                break;
            case btn::F1:   // Enter (0x4577c): folders only
                if (focus == 0) {
                    ui_.progress(kLoadingProjects, 0);
                    const bool entered = projBrowser().enter(a, cursor);
                    ui_.progressDone();
                    if (entered) {
                        count = projBrowser().count();
                        cursor = 0;
                    }
                }
                break;
            case btn::F8:
            case btn::EncPush:
                if (state == 3) {
                    if (id == btn::F8) openSaveKeyboard(*this, a, a.project.name);
                    break;
                }
                openAction();
                break;
            case btn::F3: {   // Delete project / folder (0x454ec)
                if (focus != 0) break;
                const ProjectBrowser::Result r = projBrowser().canDelete(a, cursor);
                if (r == ProjectBrowser::Result::Active || r == ProjectBrowser::Result::ContainsActive) {
                    ui_.notify("Cannot delete active project");
                    break;
                }
                if (r != ProjectBrowser::Result::Ok) break;
                char text[64];
                std::snprintf(text, sizeof(text), "\"%s\"?", entries[cursor]);
                ui_.ask(projBrowser().isProject(cursor) ? "Delete project:" : "Delete folder:", text, nullptr, "Cancel",
                        "Delete",
                        [](void* ctx, int key) {
                            auto* self = static_cast<FileScreen*>(ctx);
                            if (key != btn::F8) return;
                            if (projBrowser().remove(self->app_, self->cursor) != ProjectBrowser::Result::Ok)
                                self->ui_.notify("Delete failed");
                            self->count = projBrowser().count();
                            self->cursor = clampi(self->cursor, 0, self->count ? self->count - 1 : 0);
                            self->refreshBar();
                        },
                        this);
                break;
            }
            case btn::F4:   // New Folder (0x45284): keyboard "New folder", max 32
                if (focus != 0) break;
                ui_.kb.open("New folder", "New folder", ProjectBrowser::kFolderNameMax, "Create",
                            [](void* ctx, bool ok, const char* text) {
                                auto* self = static_cast<FileScreen*>(ctx);
                                if (!ok) return;
                                switch (projBrowser().newFolder(self->app_, text)) {
                                    case ProjectBrowser::Result::Ok:
                                        self->count = projBrowser().count();
                                        self->cursor = 0;
                                        break;
                                    case ProjectBrowser::Result::TooLong:
                                        self->ui_.notify("maximum characters in folder path exceeded");
                                        break;
                                    case ProjectBrowser::Result::Exists:
                                        self->ui_.notify("folder already exists");
                                        break;
                                    default:
                                        // SPEC GAP: no stock text for an invalid name / card error.
                                        self->ui_.notify("Cannot create folder");
                                        break;
                                }
                                self->refreshBar();
                            },
                            this);
                break;
            case btn::Left:
                if (state == 1) {
                    focus = 0;
                    cursor = 0;
                }
                break;
            case btn::Right:
                if (state == 1) {
                    focus = 1;
                    cursor = 0;
                }
                break;
            case btn::Up: cursor = clampi(cursor - 1, 0, n - 1); break;
            case btn::Down: cursor = clampi(cursor + 1, 0, n - 1); break;
            default: break;
        }
    } else if (state == 2) {
        onImportButton(id);
    } else if (state == 4) {
        if (id == btn::F8) {
            state = 0;
        } else if (id == btn::F1 || id == btn::F2 || id == btn::F3 || id == btn::F4) {
            // spec 05 §2.12 / spec 04 §6.1 modes 0..3: song, song stems, pattern, pattern stems.
            exportMode_ = id;
            if (a.cfg.clockIn != 0) {
                // spec 05 appendix E1.4: external clock -> render on the internal clock?
                char text[64];
                std::snprintf(text, sizeof(text), "Start rendering using internal clock (%d BPM)?",
                              int(a.project.tempo() + 0.5f));
                ui_.ask("External clock source is set.", text, nullptr, "Cancel", "Yes",
                        [](void* ctx, int key) {
                            auto* self = static_cast<FileScreen*>(ctx);
                            if (key == btn::F8) self->runExport(self->exportMode_);
                        },
                        this);
            } else {
                runExport(id);
            }
        } else if (id == btn::F5) {
            // spec 05 appendix E1.4 "As .it": the save prompt first when there
            // are unsaved changes (pending 5), then Mod/<project>.it.
            if (a.unsavedChanges()) askSaveThen(5);
            else exportIt(a);
        } else if (id == btn::F6) {
            // spec 05 appendix E1.4 "Samples": every used sample instrument as WAV.
            // F8 "Cancel" stops it and returns to the Export page (0x45268).
            const int n = a.exportSamples();
            char msg[48];
            if (exportWasCancelled()) msg[0] = 0;
            else if (n < 0) std::snprintf(msg, sizeof(msg), "Export failed");
            else std::snprintf(msg, sizeof(msg), "Exported %d sample%s", n, n == 1 ? "" : "s");
            if (msg[0]) ui_.notify(msg);
        }
    }
    refreshBar();
}

void FileScreen::runExport(int id) {
    App& a = app_;
    const App::ExportKind k = id <= btn::F2 ? App::ExportKind::Song : App::ExportKind::Pattern;
    const bool stems = id == btn::F2 || id == btn::F4;
    char path[160];
    // spec 05 appendix E1.4: F8 "Cancel" during the render aborts it
    // (0x453d0 -> 0x1f864) and returns to the Export page without a message.
    const bool named = a.exportName(k, path, sizeof(path), stems);
    if (named && a.exportAudio(k, path, nullptr, stems)) ui_.notify("Export done");
    else if (!named || !exportWasCancelled()) ui_.notify("Export failed");
}

#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
bool FileScreen::encoderOnList() const {
    return state >= 1 && state <= 3;   // project / mod lists
}
#endif

void FileScreen::onEncoder(int d) {
    if (state == 2) {
        cursor = clampi(cursor + d, 0, g_modCount - 1);
        refreshBar();
        return;
    }
    if (state == 1 || state == 3) {
        cursor = clampi(cursor + d, 0, (focus ? recentCount_ : count) - 1);
        refreshBar();
    }
}

namespace {
// Column header label (spec 20 §4.11): 080808 band x 3..297 (+ 300 k),
// y 32..58 with 1-px soft edges, bold font-4 caption centred [S snap 029].
void headerLabel(Gfx& g, const Theme& th, int x, const char* text) {
    const uint32_t c = 0x080808;
    wg::panel(g, th, x + 2, 32, 294, 27, c, false);
    wg::blendFill(g, x + 2, 31, 295, 1, c, wg::kEdgeA);
    wg::blendFill(g, x + 2, 59, 295, 1, c, wg::kEdgeA);
    g.text(x + 150, 45, kFont4, th.slot(slot::kList + 4), text, IDisplay::kOptCenter);   // ink y 40..51 [S]
}
// List box (1 + 300 k, 61, 297, 362) under a header: bg 0a0a0a with its
// soft top row [S snap 029].
void listBox(Gfx& g, const Theme& th, int x) {
    const uint32_t c = th.slot(slot::kListRed + 3);
    wg::panel(g, th, x, 61, 297, 362, c);
    wg::blendFill(g, x, 60, 298, 1, c, wg::kEdgeA);
}
// Two-line grey label (+0x44): title and value both font 3 [S snap 027, 028].
void greyLabel(Gfx& g, const Theme& th, int i, const BarLabel& l) {
    const uint32_t c = th.slot(l.enabled ? slot::kBottomLabel : slot::kBottomLabelOff);
    const int cx = wg::labelCentre(i);
    const bool two = l.bottom[0] != 0;
    g.text(cx, two ? wg::kLabelY1 : wg::kLabelY, kFont3, c, l.top, IDisplay::kOptCenter);
    if (two) g.text(cx, wg::kLabelY2, kFont3, c, l.bottom, IDisplay::kOptCenter);
}

struct BrowserCtx {
    FileScreen* s;
    const char (*recent)[64];
};
void sdItem(void* c, int i, char* out, int cap) {
    // Stock names every folder (projects included) "/name" [S snap 007].
    std::snprintf(out, size_t(cap), "/%s", static_cast<BrowserCtx*>(c)->s->entries[i]);
}
bool sdMarked(void*, int i) { return !projBrowser().isUp(i); }   // folders green, "/.." white
void recentItem(void* c, int i, char* out, int cap) {
    std::snprintf(out, size_t(cap), "%s", static_cast<BrowserCtx*>(c)->recent[i]);
}
void modItem(void*, int i, char* out, int cap) {
    std::snprintf(out, size_t(cap), g_modKinds[i] == kMbFile ? "%s" : "/%s", g_modNames[i]);
}
bool modMarked(void*, int i) { return modIsDir(i) && g_modKinds[i] != kMbUp; }
int gTop[2] = {0, 0};
}  // namespace

void FileScreen::draw(Gfx& g) {
    App& a = app_;
    const Theme& th = ui_.th();
    const uint32_t textC = th.slot(slot::kList + 4);
    if (state == 0) {
        // [I] no stock shot of the File menu: the Export layout (one panel
        // x 1..799, VISUAL_DIFF §8.16) with the project status in font 1/4.
        ui_.drawTitle(g, "File", a.project.name);
        wg::panel(g, th, 1, 30, 798, wg::kPanelH - 1);
        g.textf(400, 160, kFont4, textC, IDisplay::kOptCenter, "%s", a.project.name);
        g.text(400, 210, kFont1, a.unsavedChanges() ? textC : th.slot(slot::kBottomLabel),
               a.unsavedChanges() ? "Unsaved changes" : (a.neverSaved() ? "Not saved yet" : "Saved"), IDisplay::kOptCenter);
        char mem[48];
        std::snprintf(mem, sizeof(mem), "Sample memory %u%%",
                      unsigned(uint64_t(a.bank.usedBytes()) * 100 / (a.bank.capacityBytes() ? a.bank.capacityBytes() : 1)));
        g.text(400, 250, kFont1, th.slot(slot::kBottomLabel), mem, IDisplay::kOptCenter);
        return;
    }
    if (state == 4) {
        // One empty panel x 1..799, y 30..413 + fade [S snap 027].
        ui_.drawTitle(g, "File - Export", a.project.name);
        wg::panel(g, th, 1, 30, 798, wg::kPanelH - 1);
        for (int i = 0; i < 6; ++i) greyLabel(g, th, i, ui_.bar.label[i]);
        return;
    }
    if (state == 2) {
        // spec 05 appendix E1.2 import mode (0x478c0): one SD list of folders
        // and .mod / .it files; header + list, no focus frame, right panel
        // x 301..799 [S snap 029].
        ui_.drawTitle(g, "File - Import Mod", a.project.name);
        headerLabel(g, th, 1, "SD Card");
        listBox(g, th, 1);
        wg::ListView v;
        v.y = 61;
        v.w = 297;
        v.h = 362;
        v.background = false;
        v.set = slot::kListRed;
        v.count = g_modCount;
        v.sel = cursor;
        gTop[0] = wg::listFollow(cursor, g_modCount, wg::listVisibleRows(v.h), gTop[0]);
        v.top = gTop[0];
        v.item = modItem;
        v.marked = modMarked;
        wg::list(g, th, v);
        // The cursor row stays outlined on an empty folder [S snap 029].
        if (!g_modCount) wg::stroke(g, 4, 65, 295, 65 + wg::kListRowH, th.slot(slot::kFocusFrame));
        wg::panel(g, th, 301, 30, 498, wg::kPanelH - 1);
        if (!g_modCount)
            g.text(150, 240, kFont1, th.slot(slot::kBottomLabel), "No .mod / .it files", IDisplay::kOptCenter);
        return;
    }
    // spec 05 appendix E1.2 layout (0x475a8), spec 20 §4.11: header labels
    // at y 32, lists (1 / 301, 61, 297, 362), red selection on the focused
    // list and white on the other, folders green; third panel x 601..799
    // [S snap 007, 026]. File Save has no stock shot [I]: the same layout
    // with the SD list only (the Recent column stays an empty panel).
    ui_.drawTitle(g, state == 3 ? "File - Save Project" : "File - Open Project", a.project.name);
    BrowserCtx bc{this, recent_};
    for (int l = 0; l < 2; ++l) {
        const int x = l ? 301 : 1;
        if (l && state != 1) {
            wg::panel(g, th, x, 30, 297, wg::kPanelH - 1);
            continue;
        }
        headerLabel(g, th, x, l ? "Recent" : "SD Card");
        listBox(g, th, x);
        const int n = l ? recentCount_ : count;
        wg::ListView v;
        v.x = x;
        v.y = 61;
        v.w = 297;
        v.h = 362;
        v.background = false;
        v.set = slot::kListRed;
        v.count = n;
        v.sel = focus == l ? cursor : 0;
        v.selStyle = focus == l ? wg::ListSel::kRed : wg::ListSel::kWhite;
        if (focus == l) gTop[l] = wg::listFollow(cursor, n, wg::listVisibleRows(v.h), gTop[l]);
        v.top = focus == l ? gTop[l] : 0;
        v.item = l ? recentItem : sdItem;
        v.marked = l ? nullptr : sdMarked;
        v.ctx = &bc;
        wg::list(g, th, v);
        // The cursor row stays outlined on an empty list [S snap 007].
        if (n == 0)
            wg::stroke(g, x + 3, 65, x + 294, 65 + wg::kListRowH,
                       th.slot(focus == l ? slot::kFocusFrame : slot::kList));
        if (focus == l) wg::panelFocus(g, th, x, 297);
    }
    wg::panel(g, th, 601, 30, 198, wg::kPanelH - 1);
    // Cover preview (spec 05 appendix E1.6), centred right of the SD list.
    if (coverState == 2 && focus == 0) {
        const int x = 300 + (500 - coverW) / 2;
        const int y = kTitleH + (397 - coverH) / 2;
        g.fill(x - 2, y - 2, coverW + 4, coverH + 4, th.dim);
        g.color(0xFFFFFF);   // the bitmap is modulated by the current colour
        g.display().drawImage(int16_t(x), int16_t(y), IDisplay::kRamGCover, coverW, coverH);
    }
}

}  // namespace ot
