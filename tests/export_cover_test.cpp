// Export Cancel (spec 05 appendix E1.4), Render selection "render & load"
// (spec 06 §1.11), Enc-push audition on a single-row multi-track selection
// (spec 06 §1.4) and the File browser project cover (spec 05 appendix E1.6,
// spec 13 §I.3).
#include <string>

#include "test.h"
#include "ui_fakes.h"

#include "app/render.h"
#include "formats/file_io.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {
void addWav(Harness& h, const char* path, uint32_t frames) {
    std::vector<int16_t> s(frames);
    for (uint32_t i = 0; i < frames; ++i) s[i] = int16_t((i % 100) * 300 - 15000);
    std::vector<uint8_t> out(frames * 2 + 64);
    out.resize(wavEncode(s.data(), frames, 1, out.data(), out.size()));
    h.fs.files[path] = out;
}
void queue(Harness& h, InputEvent::Type t, uint8_t id) {
    InputEvent e;
    e.type = t;
    e.id = id;
    e.timeMs = h.clock.ms;
    h.input.q.push_back(e);
}
int filesUnder(Harness& h, const std::string& prefix) {
    int n = 0;
    for (auto& f : h.fs.files)
        if (f.first.compare(0, prefix.size(), prefix) == 0) ++n;
    return n;
}
PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }
FileScreen& fileScreen(Harness& h) { return *static_cast<FileScreen*>(h.app->ui.screen(kModFile)); }

void be16(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}
// Header-only baseline JPEG (the fake display only probes the header).
std::vector<uint8_t> jpeg(uint16_t w, uint16_t hgt) {
    std::vector<uint8_t> v = {0xFF, 0xD8, 0xFF, 0xC0};
    be16(v, 17);
    v.push_back(8);
    be16(v, hgt);
    be16(v, w);
    v.push_back(3);
    for (int c = 0; c < 3; ++c) {
        v.push_back(uint8_t(c + 1));
        v.push_back(0x11);
        v.push_back(0);
    }
    v.push_back(0xFF);
    v.push_back(0xD9);
    return v;
}
void addProject(Harness& h, const char* dir) {
    h.fs.dirs.insert(dir);
    Project p;
    p.setDefault();
    CHECK(saveProjectFile(h.fs, (std::string(dir) + "/project.mt").c_str(), p) == FmtStatus::Ok);
}
bool lastHas(Harness& h, const char* prefix) {
    for (auto& l : h.disp.last)
        if (l.compare(0, std::strlen(prefix), prefix) == 0) return true;
    return false;
}
}  // namespace

// ---- R1: export Cancel -----------------------------------------------------

TEST(export_render_cancel_removes_partial_file) {
    Harness h;
    App& a = *h.app;
    char p[160];
    CHECK(a.exportName(App::ExportKind::Pattern, p, sizeof(p)));
    queue(h, InputEvent::kButtonDown, btn::F8);
    CHECK(!a.exportAudio(App::ExportKind::Pattern, p));
    CHECK(exportWasCancelled());
    CHECK(!h.fs.files.count(p));
    CHECK(!a.rendering());
    // The render frame showed only "Cancel" on the bar.
    CHECK(h.disp.lastHasText("Exporting Pattern"));   // the title names the export type
    CHECK(h.disp.lastHasText("Cancel"));
    // Without Cancel the same render completes.
    CHECK(a.exportAudio(App::ExportKind::Pattern, p));
    CHECK(!exportWasCancelled());
    CHECK(h.fs.files.count(p));
    WavInfo info;
    CHECK(wavParseHeader(h.fs.files[p].data(), h.fs.files[p].size(), info));
    CHECK(info.dataSize > 0);
}

TEST(export_stems_cancel_removes_folder) {
    Harness h;
    App& a = *h.app;
    char p[160];
    CHECK(a.exportName(App::ExportKind::Song, p, sizeof(p), true));
    queue(h, InputEvent::kButtonDown, btn::F8);
    CHECK(!a.exportAudio(App::ExportKind::Song, p, nullptr, true));
    CHECK(exportWasCancelled());
    CHECK_EQ(filesUnder(h, std::string(p) + "/"), 0);
    CHECK(!h.fs.dirs.count(p));
}

TEST(export_cancel_from_file_screen_returns_to_export_page) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::File);
    h.tap(btn::F6);   // Export page
    FileScreen& fs = fileScreen(h);
    CHECK_EQ(fs.state, 4);
    // F1 "Song" press, then F8 Cancel and the F1 release while it renders.
    queue(h, InputEvent::kButtonDown, btn::F1);
    queue(h, InputEvent::kButtonDown, btn::F8);
    queue(h, InputEvent::kButtonUp, btn::F1);
    h.run(20);
    CHECK(exportWasCancelled());
    CHECK_EQ(fs.state, 4);
    CHECK_STR(a.ui.bar.label[0].bottom, "Song");   // Export labels back
    CHECK(!a.ui.isDown(btn::F1));                   // deferred release delivered
    CHECK(!a.ui.isDown(btn::F8));
    CHECK_EQ(filesUnder(h, "Export/New Project/"), 0);
    h.run(20);
    CHECK(!h.disp.lastHasText("Export failed"));
    CHECK(!h.disp.lastHasText("Export done"));
}

TEST(export_samples_cancel) {
    Harness h;
    App& a = *h.app;
    addWav(h, "kick.wav", 300000);
    addWav(h, "snare.wav", 5000);
    CHECK(a.importSample("kick.wav", 0));
    CHECK(a.importSample("snare.wav", 4));
    queue(h, InputEvent::kButtonDown, btn::F8);
    CHECK(a.exportSamples() < 0);
    CHECK(exportWasCancelled());
    CHECK_EQ(filesUnder(h, "Projects/New Project/export/samples/"), 0);
    CHECK_EQ(a.exportSamples(), 2);
    CHECK(!exportWasCancelled());
    CHECK_EQ(filesUnder(h, "Projects/New Project/export/samples/"), 2);
}

// ---- R2: Render selection "& Load" ------------------------------------------

TEST(render_selection_and_load_into_free_slot) {
    Harness h;
    App& a = *h.app;
    addWav(h, "kick.wav", 3000);
    CHECK(a.importSample("kick.wav", 0));
    Step& s = a.pattern().tracks[0].steps[0];
    s.note = 60;
    s.instrument = 0;
    a.patternEdited();
    a.editMode = true;
    h.tap(btn::F8);   // page 1
    h.tap(btn::F1);   // Render selection -> name keyboard
    CHECK(a.ui.kb.isOpen());
    CHECK_STR(a.ui.kb.text(), "Selection0001");
    h.tap(btn::F7);   // render & load
    CHECK(!a.ui.kb.isOpen());
    CHECK(h.fs.files.count("Export/New Project/Selection/Selection0001.wav"));
    CHECK(a.instrPresent[1]);
    CHECK(a.bank.loaded(1));
    {
        // [I] The render is written at 44.1 kHz, so the import takes it as is:
        // the instrument has exactly the file's frames (one rate conversion,
        // in the export, none in the import).
        const auto& f = h.fs.files["Export/New Project/Selection/Selection0001.wav"];
        WavInfo info;
        CHECK(wavParseHeader(f.data(), f.size(), info));
        CHECK_EQ(info.sampleRate, 44100u);
        CHECK_EQ(a.bank.frames(1), info.dataSize / 4u);
    }
    CHECK_EQ(a.currentInstrument(), 1);
    CHECK(a.ui.active() == kModLoader);   // spec 16 §B.8: the instrument list opens
    h.tap(btn::Pattern);
    // Plain Render leaves the instruments alone.
    h.tap(btn::F1);
    CHECK_STR(a.ui.kb.text(), "Selection0002");
    h.tap(btn::F8);
    CHECK(h.fs.files.count("Export/New Project/Selection/Selection0002.wav"));
    CHECK(!a.instrPresent[2]);
}

// The Render & Load import of both export modes, exact lengths: a 44100 Hz
// file stays as is, a 44117 Hz file is converted. 44117 is the header rate,
// not the device rate (44117.647 Hz), so after ~10 s the device-rate
// instrument is 6 frames longer.
TEST(render_and_load_length_in_both_export_modes) {
    Harness h;
    App& a = *h.app;
    constexpr uint32_t kBlocks = 3447;
    const uint32_t deviceFrames = kBlocks * audio::kBlock;
    CHECK_EQ(a.engine.exportWav(h.fs, "device.wav", false, kBlocks, nullptr, nullptr, /*resampleTo44100=*/false),
             int32_t(kBlocks));
    CHECK_EQ(a.engine.exportWav(h.fs, "resampled.wav", false, kBlocks, nullptr, nullptr, /*resampleTo44100=*/true),
             int32_t(kBlocks));
    CHECK(a.importSample("device.wav", 0));
    CHECK(a.importSample("resampled.wav", 1));
    CHECK_EQ(a.bank.frames(0), uint32_t(uint64_t(deviceFrames) * 44100 / 44117));
    CHECK_EQ(a.bank.frames(1), uint32_t(uint64_t(deviceFrames) * 2499 / 2500));
}

TEST(render_and_load_needs_a_free_slot) {
    Harness h;
    App& a = *h.app;
    for (int i = 0; i < kSampleInstrumentCount; ++i) a.instrPresent[i] = true;
    a.editMode = true;
    CHECK(!pe(h).renderSelection("full", true));
    CHECK(!h.fs.files.count("Export/New Project/Selection/full.wav"));
    h.run(20);
    CHECK(h.disp.lastHasText("Not enough instruments"));
}

TEST(render_selection_cancel) {
    Harness h;
    App& a = *h.app;
    a.editMode = true;
    queue(h, InputEvent::kButtonDown, btn::F8);
    CHECK(!pe(h).renderSelection("cut", true));
    CHECK(exportWasCancelled());
    CHECK(!h.fs.files.count("Export/New Project/Selection/cut.wav"));
    CHECK(!a.instrPresent[0]);
}

// ---- R15: Enc-push on a single-row multi-track selection ---------------------

TEST(enc_push_single_row_multitrack_auditions) {
    Harness h;
    App& a = *h.app;
    PatternScreen& p = pe(h);
    a.editMode = true;
    p.col = 0;
    p.selState = 2;
    p.anchorTrack = 0;
    p.anchorStep = 4;
    p.track = 2;
    p.step = 4;
    const SelRect r = p.target();
    CHECK_EQ(r.rows(), 1);
    CHECK_EQ(r.t0, 0);
    CHECK_EQ(r.t1, 2);
    const Pattern before = a.pattern();
    h.down(btn::EncPush);
    CHECK(a.seq.isSelectionPlaying());
    CHECK(std::memcmp(&before, &a.pattern(), sizeof(Pattern)) == 0);   // nothing written
    CHECK_EQ(p.step, 4);                                                // no advance
    h.up(btn::EncPush);
    a.seq.stop();
    // A multi-row selection still writes the last note into its notes.
    a.pattern().tracks[1].steps[5].note = 30;
    p.anchorStep = 4;
    p.step = 5;
    h.tap(btn::EncPush);
    CHECK(a.pattern().tracks[1].steps[5].note != 30);
}

// ---- R4: project cover in the File browser -----------------------------------

TEST(file_browser_cover_after_two_seconds) {
    Harness h;
    App& a = *h.app;
    h.fs.dirs.insert("Projects");
    addProject(h, "Projects/song");
    addProject(h, "Projects/plain");
    h.fs.files["Projects/song/cover.jpg"] = jpeg(400, 400);
    h.tap(btn::File);
    h.tap(btn::F2);   // Open
    FileScreen& fs = fileScreen(h);
    CHECK_EQ(fs.state, 1);
    int song = -1, plain = -1;
    for (int i = 0; i < fs.count; ++i) {
        if (!std::strcmp(fs.entries[i], "song")) song = i;
        if (!std::strcmp(fs.entries[i], "plain")) plain = i;
    }
    CHECK(song >= 0 && plain >= 0);
    h.enc(int8_t(song - fs.cursor));
    CHECK_EQ(fs.cursor, song);
    h.run(1500);
    CHECK(h.disp.loads.empty());
    CHECK(!lastHas(h, "IMAGE"));
    h.run(700);
    CHECK_EQ(int(h.disp.loads.size()), 1);
    CHECK_STR(h.disp.loads[0].c_str(), "LOADJPEG 670000 400x400 Projects/song/cover.jpg");
    CHECK_EQ(fs.coverState, 2);
    h.run(20);
    CHECK(lastHas(h, "IMAGE 350 27 670000 400 400"));
    // Another selection hides it and restarts the timer; no cover = no image.
    h.enc(int8_t(plain - fs.cursor));
    h.run(20);
    CHECK(!lastHas(h, "IMAGE"));
    h.run(2500);
    CHECK_EQ(fs.coverState, 3);
    CHECK(!lastHas(h, "IMAGE"));
    (void)a;
}

TEST(file_browser_cover_jpeg_fallback_and_limits) {
    Harness h;
    h.fs.dirs.insert("Projects");
    addProject(h, "Projects/alt");
    h.fs.files["Projects/alt/cover.jpeg"] = jpeg(200, 100);
    h.tap(btn::File);
    h.tap(btn::F2);   // Open
    FileScreen& fs = fileScreen(h);
    int alt = -1;
    for (int i = 0; i < fs.count; ++i)
        if (!std::strcmp(fs.entries[i], "alt")) alt = i;
    CHECK(alt >= 0);
    h.enc(int8_t(alt - fs.cursor));
    h.run(2200);
    CHECK_EQ(fs.coverState, 2);
    CHECK_EQ(fs.coverW, 200);
    CHECK_EQ(fs.coverH, 100);
    h.run(20);
    CHECK(lastHas(h, "IMAGE 450 176 670000 200 100"));
    // Too large: refused, nothing drawn.
    h.tap(btn::F7);   // Cancel: back to the File main page
    CHECK_EQ(fs.coverState, 0);
    h.fs.files.erase("Projects/alt/cover.jpeg");
    h.fs.files["Projects/alt/cover.jpg"] = jpeg(401, 10);
    h.tap(btn::F2);
    h.enc(int8_t(alt - fs.cursor));
    h.run(2200);
    CHECK_EQ(fs.coverState, 3);
    h.run(20);
    CHECK(!lastHas(h, "IMAGE"));
}

// Config > General "Export sample rate" reaches the export: 44.1 kHz
// (default) writes 44100, the device rate writes 44117 as stock.
TEST(export_follows_the_export_rate_option) {
    for (uint8_t rate : {uint8_t(1), uint8_t(0)}) {
        Harness h;
        App& a = *h.app;
        a.cfg.exportRate = rate;
        addWav(h, "kick.wav", 3000);
        CHECK(a.importSample("kick.wav", 0));
        Step& s = a.pattern().tracks[0].steps[0];
        s.note = 60;
        s.instrument = 0;
        a.patternEdited();
        a.editMode = true;
        h.tap(btn::F8);   // page 1
        h.tap(btn::F1);   // Render selection -> name keyboard
        h.tap(btn::F8);   // Render
        const auto& f = h.fs.files["Export/New Project/Selection/Selection0001.wav"];
        WavInfo info;
        CHECK(wavParseHeader(f.data(), f.size(), info));
        CHECK_EQ(info.sampleRate, rate ? 44100u : 44117u);
    }
}
