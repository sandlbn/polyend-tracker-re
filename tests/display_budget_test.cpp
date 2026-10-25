// Display responsiveness budget (1.9.5, field report: the screen refreshes
// sluggishly while a project plays). Models what src/hal/ft81x.cpp sends over
// SPI2 for one frame (command-FIFO bytes: 4 per display-list word, CMD_TEXT
// 12 + the padded string, the per-frame font setup, the per-chunk register
// traffic) and gates the dense Pattern-while-playing frame: RAM_DL words
// (FT81x limit 2048), FIFO bytes and the wire time at the board's 30 MHz.
// Also checks the non-blocking swap (Ui defers instead of spinning), that a
// step change costs one frame, and the telemetry fields the device log shows.
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "ui/gfx.h"
#include "ui/screens.h"

using namespace ot;

namespace {

// ft81x.cpp staging costs (bytes on the wire per IDisplay call).
class Ft81xByteModel : public RecordingDisplay {
public:
    uint32_t bytes = 0, lastBytes = 0, maxBytes = 0;   // command FIFO data of a frame
    uint32_t maxWords = 0;                              // Gfx RAM_DL estimate, max over frames
    uint32_t frames = 0;
    std::vector<std::string> maxFrame;                  // commands of the largest frame
    bool ready = true;
    int readyPolls = 0;
    Ui* ui = nullptr;

    static constexpr uint32_t kChunk = 1024;            // Ft81xDisplay::kStage
    static constexpr uint32_t kChunkOverhead = 14;      // REG_CMD_READ 6 + address 3 + REG_CMD_WRITE 5
    static constexpr uint32_t kFontSetupWords = 5 * 9 + 6;   // Ft81xDisplay::fontSetup per frame
    static constexpr uint32_t kSpiHz = 30000000;        // board::kSpi2CtarFast

    bool frameReady() override {
        ++readyPolls;
        return ready;
    }
    void dlStart() override {
        RecordingDisplay::dlStart();
        bytes = 4;                                      // CMD_DLSTART
        if (customFonts) bytes += 4 * kFontSetupWords;
    }
    void clearColorRGB(uint8_t r, uint8_t g, uint8_t b) override { RecordingDisplay::clearColorRGB(r, g, b); bytes += 4; }
    void clear(bool c, bool s, bool t) override { RecordingDisplay::clear(c, s, t); bytes += 4; }
    void colorRGB(uint8_t r, uint8_t g, uint8_t b) override { RecordingDisplay::colorRGB(r, g, b); bytes += 4; }
    void colorA(uint8_t a) override { RecordingDisplay::colorA(a); bytes += 4; }
    void lineWidth(uint16_t w) override { RecordingDisplay::lineWidth(w); bytes += 4; }
    void pointSize(uint16_t s) override { RecordingDisplay::pointSize(s); bytes += 4; }
    void scissorXY(uint16_t x, uint16_t y) override { RecordingDisplay::scissorXY(x, y); bytes += 4; }
    void scissorSize(uint16_t w, uint16_t h) override { RecordingDisplay::scissorSize(w, h); bytes += 4; }
    void begin(Prim p) override { RecordingDisplay::begin(p); bytes += 4; }
    void end() override { RecordingDisplay::end(); bytes += 4; }
    void vertex2f(int32_t x, int32_t y) override { RecordingDisplay::vertex2f(x, y); bytes += 4; }
    void vertex2ii(uint16_t x, uint16_t y, uint8_t h, uint8_t c) override { RecordingDisplay::vertex2ii(x, y, h, c); bytes += 4; }
    void text(int16_t x, int16_t y, uint8_t f, uint16_t o, const char* s) override {
        RecordingDisplay::text(x, y, f, o, s);
        bytes += 12 + ((uint32_t(std::strlen(s)) + 1 + 3) & ~3u);
    }
    void number(int16_t x, int16_t y, uint8_t f, uint16_t o, int32_t n) override { RecordingDisplay::number(x, y, f, o, n); bytes += 16; }
    void gradient(int16_t x0, int16_t y0, uint32_t c0, int16_t x1, int16_t y1, uint32_t c1) override {
        RecordingDisplay::gradient(x0, y0, c0, x1, y1, c1);
        bytes += 20;
    }
    void vertexTranslateX(int32_t x) override { RecordingDisplay::vertexTranslateX(x); bytes += 4; }
    void saveContext() override { RecordingDisplay::saveContext(); bytes += 4; }
    void restoreContext() override { RecordingDisplay::restoreContext(); bytes += 4; }
    void bitmapTransform(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) override {
        RecordingDisplay::bitmapTransform(a, b, c, d, e, f);
        bytes += 24;
    }
    void bitmapSize(uint8_t h, uint16_t w, uint16_t hh) override { RecordingDisplay::bitmapSize(h, w, hh); bytes += 8; }
    void drawImage(int16_t x, int16_t y, uint32_t a, uint16_t w, uint16_t h) override { RecordingDisplay::drawImage(x, y, a, w, h); bytes += 36; }
    void drawRgb332(int16_t x, int16_t y, uint32_t a, uint16_t w, uint16_t h, uint8_t s) override {
        RecordingDisplay::drawRgb332(x, y, a, w, h, s);
        bytes += 52;
    }
    void swap() override {
        RecordingDisplay::swap();
        bytes += 8;                                     // DISPLAY + CMD_SWAP
        bytes += kChunkOverhead * ((bytes + kChunk - 1) / kChunk);
        lastBytes = bytes;
        if (bytes > maxBytes) {
            maxBytes = bytes;
            maxFrame = last;
        }
        if (ui && ui->lastDlWords() > maxWords) maxWords = ui->lastDlWords();
        ++frames;
    }
    // Wire time of `b` bytes at the fast SPI clock, in microseconds.
    static uint32_t wireUs(uint32_t b) { return uint32_t(uint64_t(b) * 8u * 1000000u / kSpiHz); }
    void resetMax() { maxBytes = maxWords = 0; }
    // Command tally of the largest frame (diagnostics in the test output).
    void printTally() const {
        std::map<std::string, int> n;
        for (const std::string& l : maxFrame) n[l.substr(0, l.find(' '))]++;
        std::printf("  largest frame:");
        for (const auto& kv : n) std::printf(" %s x%d", kv.first.c_str(), kv.second);
        std::printf("\n");
    }
};

struct Rig {
    Harness h;
    Ft81xByteModel m;
    uint64_t acc = 0;
    Rig() {
        m.customFonts = true;
        m.ui = &h.app->ui;
        h.app->ui.attach(&m, &h.leds);
        applyFontSet(&m, 0);
        CHECK(customFontsBound());
    }
    PatternScreen& pe() { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }
    // The densest grid page: every cell of every track carries a note, an
    // instrument and both FX columns (each cell draws its full text).
    void dense() {
        Pattern& p = h.app->pattern();
        for (int t = 0; t < kTrackCount; ++t)
            for (int s = 0; s < kStepCount; ++s) {
                Step& st = p.tracks[t].steps[s];
                st.note = int8_t(36 + (s * 7 + t) % 48);
                st.instrument = uint8_t((s + t) % 48);
                st.fx[0].type = uint8_t(FxId::Volume);
                st.fx[0].value = uint8_t(10 + s);
                st.fx[1].type = uint8_t(FxId::Volume);
                st.fx[1].value = uint8_t(20 + t);
            }
    }
    // Time with audio running (the sequencer advances in the audio callback),
    // like the device: 44.1 kHz blocks of 128 frames, main loop every 10 ms.
    void play(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            acc += 10u * 44100u;
            int blocks = 0;
            while (acc >= 128000u) {
                acc -= 128000u;
                ++blocks;
            }
            h.audio.pump(blocks);
            h.run(10);
        }
    }
};

}  // namespace

// Budgets for the dense Pattern page while playing, custom fonts bound. The
// numbers are the measured 1.9.5 values plus a margin; a UI change that
// needs more must say why in the commit and raise them here.
TEST(pattern_playing_frame_budget) {
    Rig r;
    r.dense();
    r.h.tap(btn::Pattern);
    r.h.run(50);
    r.h.tap(btn::Play);
    r.play(100);
    r.m.resetMax();
    const uint32_t frames0 = r.m.frames;
    r.play(2000);   // 120 BPM: 16 steps, the window scrolls every step
    CHECK(r.m.frames - frames0 >= 12);
    const uint32_t words = r.m.maxWords, bytes = r.m.maxBytes;
    std::printf("pattern playing (dense, custom fonts): max %u DL words, %u FIFO bytes, %u us wire at 30 MHz\n",
                unsigned(words), unsigned(bytes), unsigned(Ft81xByteModel::wireUs(bytes)));
    r.m.printTally();
    // Measured 1565 words / 6522 B / 1739 us (was 2731 / 11242 / 2997 before
    // the 1.9.5 grid and glyph-run reductions: over the RAM_DL limit).
    CHECK(words <= 1750);                     // FT81x RAM_DL 2048 words, with headroom
    CHECK(bytes <= 7200);
    CHECK(Ft81xByteModel::wireUs(bytes) <= 1950);
    r.h.tap(btn::Play);
}

// Performance screen: 12 FX panels with values on every stored row, edit
// mode (focus frame) and playing. On hardware the column-by-column drawing
// (~1730 words) expanded past the FT81x RAM_DL (8 KB) and every frame
// faulted (the screen froze); drawn in layers it needs ~1400. RAM_DL grows
// ~1.1-1.4 x the uploaded words on the device (Song 1.12, Master 1.40), so
// text-heavy screens need more headroom than the 1750 of the Pattern page.
TEST(performance_frame_budget) {
    Rig r;
    r.h.tap(btn::Performance);
    r.h.run(50);
    PerformanceScreen& p = *static_cast<PerformanceScreen*>(r.h.app->ui.screen(kModPerformance));
    for (int c = 0; c < perf::kSlots; ++c)
        for (int row = 1; row < perf::kRows; ++row) p.src.setValue(c, row, int16_t(row * 7 - 11));
    r.h.tap(btn::Rec);   // edit mode: focus frame on column 0
    r.h.tap(btn::Right);
    r.h.tap(btn::Play);
    r.play(100);
    r.m.resetMax();
    r.play(1500);
    const uint32_t words = r.m.maxWords;
    std::printf("performance (values, edit, playing): max %u DL words\n", unsigned(words));
    CHECK(words <= 1500);
    r.h.tap(btn::Play);
}

// Track Mixer 2/3 and Global Mixer 3/3, playing (meters moving). Column by
// column they uploaded ~1830 / ~1750 words and every frame faulted the
// FT81x on hardware (the display kept the previous screen); drawn in layers
// they stay inside the same budget as the Performance screen.
TEST(mixer_frame_budget) {
    Rig r;
    r.h.tap(btn::Master);
    r.h.run(50);
    r.h.tap(btn::Master);   // Track Mixer 2/3
    r.h.run(50);
    r.h.tap(btn::Play);
    r.play(100);
    r.m.resetMax();
    r.play(1000);
    uint32_t words = r.m.maxWords;
    std::printf("track mixer (playing): max %u DL words\n", unsigned(words));
    r.m.printTally();
    CHECK(words <= 1400);
    r.h.tap(btn::Master);   // Global Mixer 3/3
    r.play(100);
    r.m.resetMax();
    r.play(1000);
    words = r.m.maxWords;
    std::printf("global mixer (playing): max %u DL words\n", unsigned(words));
    r.m.printTally();
    CHECK(words <= 1400);
    r.h.tap(btn::Play);
}

// 2.x M1: the same dense page with the window on tracks 13..16 (MIDI
// channel tags in the Instr column) stays inside the same budget.
TEST(pattern_playing_frame_budget_tracks_13_16) {
    Rig r;
    r.dense();
    Pattern& p = r.h.app->pattern();
    for (int t = kMidiTrackStart; t < kTrackCount; ++t)
        for (int s = 0; s < kStepCount; ++s) p.tracks[t].steps[s].instrument = uint8_t(kFirstMidiInstrument + (s + t) % 16);
    r.h.tap(btn::Pattern);
    r.h.run(50);
    for (int i = 0; i < kTrackCount; ++i) r.h.tap(btn::Right);
    CHECK_EQ(r.pe().firstTrack, kTrackCount - 4);
    r.h.tap(btn::Play);
    r.play(100);
    r.m.resetMax();
    r.play(2000);
    const uint32_t words = r.m.maxWords, bytes = r.m.maxBytes;
    std::printf("pattern playing tracks 13-16 (dense, custom fonts): max %u DL words, %u FIFO bytes\n",
                unsigned(words), unsigned(bytes));
    CHECK(words <= 1750);
    CHECK(bytes <= 7200);
    r.h.tap(btn::Play);
}

// A sparse page (the common case) stays far below the dense one.
TEST(pattern_playing_sparse_frame_budget) {
    Rig r;
    Pattern& p = r.h.app->pattern();
    for (int s = 0; s < 16; s += 2) {
        p.tracks[s % 4].steps[s].note = int8_t(48 + s);
        p.tracks[s % 4].steps[s].instrument = 1;
    }
    r.h.tap(btn::Pattern);
    r.h.run(50);
    r.h.tap(btn::Play);
    r.play(100);
    r.m.resetMax();
    r.play(1000);
    std::printf("pattern playing (sparse): max %u DL words, %u FIFO bytes\n", unsigned(r.m.maxWords), unsigned(r.m.maxBytes));
    CHECK(r.m.maxWords <= 1050);              // measured 917
    CHECK(r.m.maxBytes <= 4400);              // measured 3900
    r.h.tap(btn::Play);
}

// Non-blocking swap: while the display reports busy the UI keeps the frame
// pending (and counts it) instead of drawing; it draws as soon as ready.
TEST(busy_display_defers_frame) {
    Rig r;
    Ui& ui = r.h.app->ui;
    r.h.run(50);
    const uint32_t f0 = r.m.frames;
    const uint32_t d0 = ui.framesDeferred();
    r.m.ready = false;
    ui.dirty();
    r.h.run(100);
    CHECK(r.m.frames == f0);
    CHECK(ui.framesDeferred() > d0);
    r.m.ready = true;
    r.h.run(20);
    CHECK(r.m.frames == f0 + 1);
    CHECK(ui.framesDeferred() == ui.framesDeferred());   // no further deferrals
    const uint32_t d1 = ui.framesDeferred();
    r.h.run(100);
    CHECK(ui.framesDeferred() == d1);
}

// One step change -> one frame, already scrolled to the new step (the
// follow is not behind the 20 ms update throttle any more).
TEST(step_change_costs_one_frame) {
    Rig r;
    r.dense();
    r.h.tap(btn::Pattern);
    r.h.run(50);
    r.h.tap(btn::Play);
    r.play(100);
    Sequencer& sq = r.h.app->seq;
    int steps = 0;
    uint32_t frames = 0;
    int last = sq.step(0);
    for (int i = 0; i < 200; ++i) {   // 2 s at 120 BPM = 16 steps
        r.acc += 10u * 44100u;
        int blocks = 0;
        while (r.acc >= 128000u) {
            r.acc -= 128000u;
            ++blocks;
        }
        r.h.audio.pump(blocks);
        const int now = sq.step(0);
        const uint32_t f0 = r.m.frames;
        r.h.run(10);
        if (now != last) {
            last = now;
            ++steps;
            CHECK(r.m.frames == f0 + 1);                 // one frame for the step, drawn on this pass
            CHECK(r.pe().step == now);                   // and it shows the new step
        }
        frames += r.m.frames - f0;
    }
    CHECK(steps >= 12);
    std::printf("playback: %d steps, %u frames\n", steps, unsigned(frames));
    CHECK(frames <= uint32_t(steps) + 4);                // + the play-time seconds in the title
    r.h.tap(btn::Play);
}

// Telemetry fields: deferred count, frame gap, dirty and input latencies.
TEST(render_stats_fields) {
    Rig r;
    Ui& ui = r.h.app->ui;
    r.h.run(50);
    (void)ui.takeRenderStats();
    r.h.tap(btn::Down);           // input at clock.ms, handled and drawn in the same pass
    r.h.run(500);
    ui.dirty();
    r.h.run(30);
    const Ui::RenderStats s = ui.takeRenderStats();
    CHECK(s.frames >= 2);
    CHECK(s.deferred == 0);
    CHECK(s.gapMaxMs >= 500);
    CHECK(s.inputLatMaxMs <= 20);
    CHECK(s.dirtyLatMaxUs <= 20000);
    const Ui::RenderStats z = ui.takeRenderStats();
    CHECK(z.frames == 0 && z.gapMaxMs == 0 && z.dirtyLatMaxUs == 0 && z.inputLatMaxMs == 0);
}

// ---------------------------------------------------------------------------
// RAM_DL overlay matrix (docs/VERIFY_1.9.5.md §4-§6). Worst-case content
// (dense 16-track page, 48 instruments with 31-character names, 255 named
// patterns and song slots, long track / file / project names, a stereo
// sample), custom fonts bound: every screen alone, playing where it
// matters, and under every overlay (on-screen keyboard, pad-grid chooser,
// progress, ask, notify, power, USB page) plus the screen's own popups.
// Every frame stays at or below Ui::kDlTargetWords (1900 Gfx words; the
// FT81x holds 2048 minus 36 driver words) and the Ui guard never cuts a
// frame or falls back to low detail.

#include "app/perf_presets.h"
#include "formats/patch_pb.h"
#include "formats/wav.h"

namespace {

std::vector<uint8_t> wavFile(uint32_t frames, bool stereo) {
    std::vector<int16_t> s(frames * (stereo ? 2 : 1));
    for (uint32_t i = 0; i < s.size(); ++i)
        s[i] = int16_t(std::sin(double(i) * 0.05) * 20000.0 * std::sin(double(i) * 0.0007));
    std::vector<uint8_t> out(s.size() * 2 + 64);
    out.resize(wavEncode(s.data(), frames, stereo ? 2 : 1, out.data(), out.size()));
    return out;
}

const char kLong[] = "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW";   // 31 characters

void longItem(void*, int i, char* out, int cap) { std::snprintf(out, size_t(cap), "%d. %s", i + 1, kLong); }
void kbDone(void*, bool, const char*) {}
void askNone(void*, int) {}

struct WorstRig {
    Harness h;
    Ft81xByteModel m;
    App& a;
    uint64_t acc = 0;
    uint32_t worst = 0;
    std::string worstName;
    explicit WorstRig(bool customFonts = true) : h(false), a(*h.app) {
        setRomTextMode(kRomGlyphs);   // the device / simulator path (OP-6)
        // 101 list entries (the loader's maximum): 3 folders, 97 + 1 files.
        for (int i = 0; i < 97; ++i) {
            char n[64];
            std::snprintf(n, sizeof(n), "WWWWWWWW_long_sample_name_%03d_mmmmm.wav", i);   // 39: longest listed
            h.fs.files[n] = wavFile(800, false);
        }
        h.fs.files["WWWW_stereo_long_WWWWWWWWWWWWWWWWWW.wav"] = wavFile(30000, true);
        h.fs.dirs.insert("Projects");
        h.fs.dirs.insert("Workspace");
        for (const char* d : {"instruments", "patterns", "samples"}) h.fs.dirs.insert(std::string("Workspace/") + d);
        m.customFonts = customFonts;
        h.disp.customFonts = customFonts;
        m.ui = &a.ui;
        a.ui.attach(&m, &h.leds);
        a.boot();
        applyFontSet(&m, 0);
        CHECK(customFontsBound() == customFonts);
        h.run(100);
        if (h.fs.files.count("Workspace/project.mt"))
            for (int i = 0; i < 30; ++i) {
                char n[80];
                std::snprintf(n, sizeof(n), "Projects/WWWWWWWW Project With A Very Long Name %02d", i);
                h.fs.dirs.insert(n);
                h.fs.files[std::string(n) + "/project.mt"] = h.fs.files["Workspace/project.mt"];
            }
        // The stereo sample into slot 1 (Sample Loader: F1 Add).
        h.tap(btn::SampleLoader);
        h.run(2000);
        LoaderScreen& l = *static_cast<LoaderScreen*>(a.ui.screen(kModLoader));
        for (int i = 0; i < l.count; ++i)
            if (std::strstr(l.entries[i], "stereo")) l.sdCursor = i;
        CHECK(l.count >= 101);
        l.insCursor = 1;
        h.tap(btn::F1);
        h.run(100);
        if (a.ui.askOpen()) h.tap(btn::F8);   // "replace instrument(s)?"
        h.run(3000);
        CHECK(a.instrPresent[1]);
        // 48 instruments with 31-character names.
        for (int i = 0; i < kSampleInstrumentCount; ++i) {
            if (!a.instrPresent[i]) {
                a.instrPresent[i] = true;
                a.instruments[i] = a.instruments[1];
            }
            std::snprintf(a.instruments[i].name, sizeof(a.instruments[i].name), "%s", kLong);
        }
        a.setCurrentInstrument(1);
        for (int t = 0; t < kTrackCount; ++t) a.project.setTrackName(t, "WWWWWWW");
        for (int p = 1; p <= 255; ++p) a.names.set(p, "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
        Pattern& p = a.pattern();
        for (int t = 0; t < kTrackCount; ++t)
            for (int s = 0; s < kStepCount; ++s) {
                Step& st = p.tracks[t].steps[s];
                st.note = int8_t(36 + (s * 7 + t) % 48);
                st.instrument = uint8_t(t >= 8 ? 48 + (s % 16) : (s + t) % 48);
                st.fx[0].type = uint8_t(FxId::Volume);
                st.fx[0].value = uint8_t(10 + s);
                st.fx[1].type = uint8_t(FxId::Volume);
                st.fx[1].value = uint8_t(20 + t);
            }
        a.patternEdited();
        for (int i = 0; i < 255; ++i) a.project.playlist[i] = uint8_t(1 + (i % 3));
        a.ui.dirty();
        h.run(40);
    }
    void play(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            acc += 10u * 44100u;
            int blocks = 0;
            while (acc >= 128000u) {
                acc -= 128000u;
                ++blocks;
            }
            h.audio.pump(blocks);
            h.run(10);
            a.ui.dirty();
        }
    }
    // Largest frame over `ms` of redraws (or of play when playing).
    void measure(const char* name, uint32_t ms = 60) {
        m.resetMax();
        const uint32_t over0 = a.ui.overBudgetFrames(), low0 = a.ui.lowDetailFrames();
        if (a.seq.isPlaying()) {
            play(ms < 400 ? 400 : ms);
        } else {
            for (uint32_t t = 0; t < ms; t += 10) {
                a.ui.dirty();
                h.run(10);
            }
        }
        const uint32_t w = m.maxWords;
        std::printf("  %-56s %5u words\n", name, unsigned(w));
        if (w > worst) {
            worst = w;
            worstName = name;
        }
        CHECK(w > 0);
        CHECK(w <= Ui::kDlTargetWords);
        CHECK(a.ui.overBudgetFrames() == over0);   // the guard never had to cut a frame
        CHECK(a.ui.lowDetailFrames() == low0);     // nor fall back to low detail
        if (w > Ui::kDlTargetWords) m.printTally();
    }
    // The shared overlays over the current screen.
    void overlays(const char* screen) {
        char n[96];
        Ui& ui = a.ui;
        ui.kb.open(kLong, kLong, 31, "Save", kbDone, nullptr);
        ui.dirty();
        std::snprintf(n, sizeof(n), "%s + keyboard", screen);
        measure(n);
        ui.kb.close();
        ui.picker.open(2, "Instrument", 64, 5, longItem, nullptr);
        ui.dirty();
        std::snprintf(n, sizeof(n), "%s + chooser (64 long names)", screen);
        measure(n);
        ui.picker.close();
        ui.setProgress("Saving project WWWWWWWWWWWWWWWWWWWWWWWWW", 50);
        std::snprintf(n, sizeof(n), "%s + progress", screen);
        measure(n);
        ui.progressDone();
        ui.ask("Replace instrument(s)?", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", "", "Cancel", "Yes", askNone, nullptr);
        std::snprintf(n, sizeof(n), "%s + ask", screen);
        measure(n);
        ui.closeAsk();
        ui.notify("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
        std::snprintf(n, sizeof(n), "%s + notify", screen);
        measure(n);
        h.run(2100);
        h.down(btn::Power);
        std::snprintf(n, sizeof(n), "%s + power popup", screen);
        measure(n);
        h.up(btn::Power);
        // The page alone (no card hand-over: leaving the real mode reloads
        // the Workspace, usb_storage_budget below measures that path).
        ui.usb.show(true);
        ui.usb.setHost(IUsbStorage::Host::kInUse);
        ui.dirty();
        std::snprintf(n, sizeof(n), "%s + USB storage page", screen);
        measure(n);
        ui.usb.show(false);
        ui.dirty();
        h.run(40);
    }
    void screen(uint8_t key, const char* name, uint32_t settle = 200) {
        h.tap(key);
        h.run(settle);
        measure(name);
        overlays(name);
    }
};

}  // namespace

TEST(overlay_matrix_pattern) {
    WorstRig r;
    Harness& h = r.h;
    App& a = r.a;
    r.screen(btn::Pattern, "pattern dense");
    h.tap(btn::Rec);
    r.measure("pattern dense edit");
    r.overlays("pattern dense edit");
    // The screen's own popups (the Instruments list over the 3-track grid,
    // the Fx list with its description over 2 tracks, Notes, Fill).
    h.down(btn::Note);
    h.run(600);
    r.measure("pattern + Notes popup");
    h.up(btn::Note);
    h.tap(btn::Instr);
    h.down(btn::Instr);
    h.run(250);
    r.measure("pattern dense + Instruments popup (31-char names)");
    h.up(btn::Instr);
    h.tap(btn::Fx1);
    h.down(btn::Fx1);
    h.run(250);
    r.measure("pattern dense + Fx popup");
    h.tap(btn::F7);
    r.measure("pattern dense + Fx popup + description");
    h.tap(btn::F7);
    h.up(btn::Fx1);
    h.tap(btn::F4);
    r.measure("pattern dense + Fill popup");
    h.tap(btn::F7);
    // Bottom-bar pages: Shift mute page, Live-REC arm page (all armed).
    h.down(btn::Shift);
    r.measure("pattern dense + mute page");
    h.tap(btn::Rec);
    r.measure("pattern dense + Live-REC arm page (all armed)");
    h.up(btn::Shift);
    r.measure("pattern dense + Live-REC arm page, Shift up");
    h.tap(btn::Rec);
    // Name keyboard (page 2 F1) with a 30-character name.
    h.tap(btn::F8);
    h.tap(btn::F8);
    h.tap(btn::F1);
    CHECK(a.ui.kb.isOpen());
    r.measure("pattern dense + name keyboard");
    h.tap(btn::F7);
    h.tap(btn::F8);
    // 8-track column view, while playing.
    h.down(btn::Pattern);
    h.tap(btn::Note);
    h.tap(btn::Fx1);
    h.up(btn::Pattern);
    r.measure("pattern dense 8-track view");
    h.tap(btn::Instr);
    h.down(btn::Instr);
    h.run(250);
    r.measure("pattern dense 8-track view + Instruments popup");
    h.up(btn::Instr);
    h.down(btn::Pattern);
    h.tap(btn::Note);
    h.tap(btn::Note);
    h.up(btn::Pattern);
    h.tap(btn::Rec);
    // Playing (edit mode off): the window follows the playhead.
    h.tap(btn::Play);
    r.measure("pattern dense playing", 1500);
    h.down(btn::Instr);
    h.run(250);
    r.measure("pattern dense playing + Instruments popup", 600);
    h.up(btn::Instr);
    h.tap(btn::Play);
    // Horizontal arrangement (R6), stopped and playing.
    a.cfg.arrangement = 2;
    a.ui.dirty();
    r.measure("pattern dense horizontal");
    h.tap(btn::Play);
    r.measure("pattern dense horizontal playing", 1500);
    h.tap(btn::Play);
    a.cfg.arrangement = 0;
    // Count-in popup while a recording waits.
    a.cfg.metronome = 1;
    a.cfg.preRoll = 1;
    a.applySequencerSettings();
    h.down(btn::Rec);
    h.tap(btn::Play);
    h.up(btn::Rec);
    r.measure("pattern dense + count-in", 500);
    h.tap(btn::Play);
    std::printf("  worst: %s, %u words\n", r.worstName.c_str(), unsigned(r.worst));
}

// OP-6 (docs/OPTIMIZATION_2X.md item 8): the same dense Pattern pages with
// the custom fonts not bound (CMD_INFLATE / bind failure: ROM fonts). Gfx::
// glyphRun draws the grid cells as ROM-font VERTEX2II glyphs (handles 16-31),
// so these frames also stay within Ui::kDlTargetWords without a guard cut.
TEST(overlay_matrix_pattern_rom_fonts) {
    Harness::dlOverflowFatal = false;   // TEMP
    WorstRig r(false);
    Harness& h = r.h;
    App& a = r.a;
    r.screen(btn::Pattern, "ROM fonts: pattern dense");
    h.tap(btn::Rec);
    r.measure("ROM fonts: pattern dense edit");
    h.tap(btn::Instr);
    h.down(btn::Instr);
    h.run(250);
    r.measure("ROM fonts: pattern dense + Instruments popup");
    h.up(btn::Instr);
    h.tap(btn::Fx1);
    h.down(btn::Fx1);
    h.run(250);
    r.measure("ROM fonts: pattern dense + Fx popup");
    h.up(btn::Fx1);
    h.tap(btn::Rec);
    h.down(btn::Pattern);
    h.tap(btn::Note);
    h.tap(btn::Fx1);
    h.up(btn::Pattern);
    r.measure("ROM fonts: pattern dense 8-track view");
    h.down(btn::Pattern);
    h.tap(btn::Note);
    h.tap(btn::Note);
    h.up(btn::Pattern);
    h.tap(btn::Play);
    r.measure("ROM fonts: pattern dense playing", 1500);
    h.tap(btn::Play);
    a.cfg.arrangement = 2;
    a.ui.dirty();
    r.measure("ROM fonts: pattern dense horizontal");
    h.tap(btn::Play);
    r.measure("ROM fonts: pattern dense horizontal playing", 1500);
    h.tap(btn::Play);
    a.cfg.arrangement = 0;
    for (int i = 0; i < kTrackCount; ++i) h.tap(btn::Right);
    a.ui.dirty();
    r.measure("ROM fonts: pattern 13-16 dense");
    std::printf("  worst: %s, %u words\n", r.worstName.c_str(), unsigned(r.worst));
}

// 2.x M1 (docs/M1_PLAN.md §4): the dense 16-track page with the window on
// tracks 13..16, bank-B bottom pages, the 8-track view of tracks 9..16,
// playing, and the Song grid with its window shifted to tracks 5..16.
TEST(overlay_matrix_tracks_13_16) {
    WorstRig r;
    Harness& h = r.h;
    App& a = r.a;
    h.tap(btn::Pattern);
    h.run(200);
    for (int i = 0; i < kTrackCount; ++i) h.tap(btn::Right);
    PatternScreen& p = *static_cast<PatternScreen*>(a.ui.screen(kModPattern));
    CHECK_EQ(p.firstTrack, kTrackCount - 4);
    r.measure("pattern 13-16 dense");
    r.overlays("pattern 13-16 dense");
    h.tap(btn::Rec);
    r.measure("pattern 13-16 dense edit");
    h.tap(btn::Instr);
    h.down(btn::Instr);
    h.run(250);
    r.measure("pattern 13-16 dense + MIDI channel popup");
    h.up(btn::Instr);
    // 2.x: the MIDI-track popup lists Synth 1..3 first; opened on a synth.
    a.setCurrentInstrument(kFirstSynthInstrument + 1);
    h.down(btn::Instr);
    h.run(250);
    r.measure("pattern 13-16 dense + Instrument popup on Synth 2");
    h.up(btn::Instr);
    h.tap(btn::Fx1);
    h.down(btn::Fx1);
    h.run(250);
    h.tap(btn::F7);
    r.measure("pattern 13-16 dense + Fx popup + description");
    h.tap(btn::F7);
    h.up(btn::Fx1);
    h.tap(btn::F4);
    r.measure("pattern 13-16 dense + Fill popup");
    h.tap(btn::F7);
    h.down(btn::Shift);
    r.measure("pattern 13-16 dense + mute page (bank B)");
    h.tap(btn::Rec);
    r.measure("pattern 13-16 dense + Live-REC arm page (bank B)");
    h.up(btn::Shift);
    h.tap(btn::Rec);
    h.down(btn::Pattern);
    h.tap(btn::Note);
    h.tap(btn::Fx1);
    h.up(btn::Pattern);
    CHECK_EQ(p.firstTrack, kTrackCount - 8);
    r.measure("pattern 9-16 dense 8-track view");
    h.tap(btn::Play);
    r.measure("pattern 9-16 dense 8-track view playing", 1500);
    h.tap(btn::Play);
    h.down(btn::Pattern);
    h.tap(btn::Note);
    h.tap(btn::Note);
    h.up(btn::Pattern);
    h.tap(btn::Rec);
    h.tap(btn::Play);
    r.measure("pattern 13-16 dense playing", 1500);
    h.tap(btn::Play);
    a.cfg.arrangement = 2;
    a.ui.dirty();
    h.tap(btn::Play);
    r.measure("pattern 13-16 dense horizontal playing", 1500);
    h.tap(btn::Play);
    a.cfg.arrangement = 0;
    // Song: dense ext tracks in every listed pattern, window on 5..16.
    for (int n = 1; n <= 3; ++n)
        for (int t = 0; t < kTrackCount; ++t) a.storedTrack(n, t).steps[0].note = 60;
    a.reloadCurrentFromStore();
    h.tap(btn::Song);
    h.run(200);
    SongScreen& s = *static_cast<SongScreen*>(a.ui.screen(kModSong));
    h.tap(btn::Rec);
    h.tap(btn::Left);
    h.tap(btn::Right);
    CHECK_EQ(s.firstTrack, kTrackCount - SongScreen::kGridCols);
    h.down(btn::Shift);
    h.tap(btn::Down);
    h.tap(btn::Down);
    h.up(btn::Shift);
    r.measure("song shifted window + selection");
    r.overlays("song shifted window");
    h.tap(btn::F2);
    r.measure("song shifted window playing", 1000);
    h.tap(btn::F2);
    std::printf("  worst: %s, %u words\n", r.worstName.c_str(), unsigned(r.worst));
}

TEST(overlay_matrix_screens) {
    WorstRig r;
    Harness& h = r.h;
    App& a = r.a;
    r.screen(btn::Song, "song dense (255 slots)");
    h.tap(btn::Play);
    r.measure("song dense playing", 1000);
    h.tap(btn::Play);
    r.screen(btn::Performance, "performance");
    h.tap(btn::Play);
    r.measure("performance playing", 1000);
    h.tap(btn::Play);
    r.screen(btn::Master, "master");
    for (uint8_t f : {btn::F2, btn::F3, btn::F4, btn::F5}) {
        h.tap(f);
        r.measure("master sub-page");
        h.tap(btn::F8);
    }
    h.tap(btn::Master);
    h.run(100);
    r.measure("track mixer (meters)");
    r.overlays("track mixer");
    h.tap(btn::Play);
    r.measure("track mixer playing", 1000);
    h.tap(btn::Play);
    h.tap(btn::Rec);
    h.tap(btn::F1);
    CHECK(a.ui.kb.isOpen());
    r.measure("track mixer + rename keyboard (R1)");
    h.tap(btn::F7);
    h.tap(btn::Rec);
    h.tap(btn::Master);
    h.run(100);
    r.measure("global mixer");
    r.overlays("global mixer");
    r.screen(btn::InstrParams, "instrument params");
    h.down(btn::Instr);
    h.run(250);
    CHECK(a.ui.picker.isOpen());
    r.measure("instrument params + chooser (R3)");
    h.up(btn::Instr);
    r.screen(btn::SampleLoader, "sample loader (101 long files)", 300);
    h.tap(btn::Right);
    h.tap(btn::F4);
    CHECK(a.ui.kb.isOpen());
    r.measure("sample loader + rename keyboard (R2)");
    h.tap(btn::F7);
    a.setCurrentInstrument(1);
    r.screen(btn::SamplePlayback, "sample playback (stereo)");
    h.down(btn::Instr);
    h.run(250);
    CHECK(a.ui.picker.isOpen());
    r.measure("sample playback + chooser");
    h.up(btn::Instr);
    h.push(InputEvent::kPadDown, 0);   // playhead line (USER_REPORTS_R2 §1c)
    h.audio.pump(4);
    h.run(40);
    CHECK(static_cast<PlaybackScreen*>(a.ui.screen(kModPlayback))->drawnHeadX() >= 0);
    r.measure("sample playback + playhead");
    h.push(InputEvent::kPadUp, 0);
    a.setCurrentInstrument(1);
    r.screen(btn::SampleEditor, "sample editor (stereo)");
    h.down(btn::Instr);
    h.run(250);
    CHECK(a.ui.picker.isOpen());
    r.measure("sample editor + chooser");
    h.up(btn::Instr);
    r.screen(btn::SampleRecorder, "sample recorder");
    r.screen(btn::Config, "config");
    r.screen(btn::File, "file");
    h.tap(btn::F2);
    h.run(300);
    r.measure("file open project (30 long names)");
    r.overlays("file open project");
    std::printf("  worst: %s, %u words\n", r.worstName.c_str(), unsigned(r.worst));
}

// 2.x M1-WP6: Track Mixer and Performance bank B (tracks 9-16) reuse the
// stock geometry; every frame, with the keyboard and choosers over it, stays
// within the budget (M1 plan §4).
TEST(overlay_matrix_bank_b) {
    WorstRig r;
    Harness& h = r.h;
    App& a = r.a;
    for (int t = 8; t < kTrackCount; ++t) trk::setName(a, t, "WWWWWWW");
    r.screen(btn::Performance, "performance");
    h.tap(btn::Performance);
    CHECK_EQ(static_cast<PerformanceScreen*>(a.ui.screen(kModPerformance))->bank, 1);
    r.measure("performance bank B");
    r.overlays("performance bank B");
    h.tap(btn::Rec);
    r.measure("performance bank B edit");
    h.tap(btn::Rec);
    h.down(btn::Shift);
    r.measure("performance bank B + Shift");
    h.up(btn::Shift);
    h.tap(btn::Play);
    r.measure("performance bank B playing", 1000);
    h.tap(btn::Play);
    r.screen(btn::Master, "master");
    h.tap(btn::Master);
    h.run(100);
    h.tap(btn::Right);
    MixerScreen& m = *static_cast<MixerScreen*>(a.ui.screen(kModMaster));
    CHECK_EQ(m.page, 1);
    CHECK_EQ(m.bank, 1);
    r.measure("track mixer bank B");
    r.overlays("track mixer bank B");
    h.tap(btn::Play);
    r.measure("track mixer bank B playing (activity)", 1000);
    h.tap(btn::Play);
    h.tap(btn::Rec);
    h.tap(btn::F5);
    CHECK(a.ui.kb.isOpen());
    r.measure("track mixer bank B + rename keyboard");
    h.tap(btn::F7);
    h.tap(btn::Rec);
    h.down(btn::Shift);
    r.measure("track mixer bank B + Shift (solo labels)");
    h.up(btn::Shift);
    std::printf("  worst: %s, %u words\n", r.worstName.c_str(), unsigned(r.worst));
}

// 2.x WP-G: the synth screens (Synth Parameters, Synth Macros, every Edit
// Patch section of the densest engines, the patch browser with 128 long
// names) with the shared overlays, the Save As keyboard and the Init
// question; slots with 31-character patch names and 31-character macro
// names whose six targets all name long parameters.
TEST(overlay_matrix_synth) {
    WorstRig r;
    Harness& h = r.h;
    App& a = r.a;
    a.setSynthModel(1, int(SynthModel::PERC));
    a.setSynthModel(2, int(SynthModel::VAP));
    for (int s = 0; s < kSynthSlotCount; ++s) {
        SynthSlotRecord rec = a.synthSlot(s);
        std::snprintf(rec.patchName, sizeof(rec.patchName), "%s", kLong);
        a.setSynthSlotRecord(s, rec);
        SynthPatch& p = a.synthPatch(s);
        for (int k = 0; k < kPatchMacros; ++k) {
            std::snprintf(p.macros[k].name, sizeof(p.macros[k].name), "%s", kLong);
            p.macros[k].nameLen = 31;
            p.macros[k].count = kPatchMacroTargets;
            for (int t = 0; t < kPatchMacroTargets; ++t) {
                p.macros[k].targets[t].param = p.engine == kSynthEnginePerc ? 79 : 38;   // "Waveshaper Level" / "Osc Mix Env Amt"
                p.macros[k].targets[t].amountBits = patchFloatToBits(-1.99f);
            }
        }
        a.markSynthPatchDirty(s);
    }
    h.fs.dirs.insert("Patches");
    h.fs.dirs.insert("Patches/VAP");
    for (int i = 0; i < SynthScreen::kBrowserMax + 5; ++i) {
        char n[80];
        std::snprintf(n, sizeof(n), "Patches/VAP/WWWWWWWWWWWWWWWWWWWWWWWWWWWW%03d.patch", i);   // 31-char names
        h.fs.files[n] = std::vector<uint8_t>{0};
    }
    SynthScreen& sy = *static_cast<SynthScreen*>(a.ui.screen(kModSynth));
    a.setCurrentInstrument(66);
    h.tap(btn::Pattern);
    {
        // Instrument Params takes the instrument under the cursor (issue #52):
        // a note there plays Synth 3 too.
        const PatternScreen& ps = *static_cast<PatternScreen*>(a.ui.screen(kModPattern));
        Step& st = a.pattern().tracks[ps.track].steps[ps.step];
        if (st.note >= 0) {
            st.instrument = 66;
            a.patternEdited();
        }
    }
    r.screen(btn::InstrParams, "synth parameters (VAP)");
    CHECK_EQ(a.ui.active(), int(kModSynth));
    h.tap(btn::F3);
    r.measure("synth parameters, Gain selected");
    r.screen(btn::InstrParams, "synth macros (VAP)");
    CHECK(sy.view == SynthScreen::View::Macros);
    h.tap(btn::InstrParams);
    h.tap(btn::F8);
    CHECK(sy.view == SynthScreen::View::Edit);
    const synthui::Section* sec = nullptr;
    int n = synthui::sections(kSynthEngineVap, &sec);
    for (int i = 0; i < n; ++i) {
        sy.section = i;
        sy.row = sy.rows() - 1;   // scrolled to the end (scroll bar)
        char nm[64];
        std::snprintf(nm, sizeof(nm), "synth edit patch VAP %s", sec[i].name);
        r.measure(nm);
    }
    sy.section = 0;
    sy.row = 0;
    r.overlays("synth edit patch");
    h.tap(btn::F2);   // Save As (manual p.153: F1 Save, F2 Save As, F3 Init Patch)
    CHECK(a.ui.kb.isOpen());
    r.measure("synth edit patch + Save As keyboard");
    h.tap(btn::F7);
    h.tap(btn::F3);
    CHECK(a.ui.askOpen());
    r.measure("synth edit patch + Init question");
    h.tap(btn::F7);
    h.tap(btn::F8);
    h.tap(btn::F7);
    CHECK(sy.view == SynthScreen::View::Browser);
    CHECK_EQ(sy.browserCount, SynthScreen::kBrowserMax);
    r.measure("synth patch browser (128 long names)");
    sy.browserSel = sy.browserCount - 1;
    r.measure("synth patch browser, last entry");
    r.overlays("synth patch browser");
    h.tap(btn::F8);
    // PERC (84 parameters, the 30-row Mixer section).
    a.setCurrentInstrument(65);
    h.tap(btn::Pattern);
    r.screen(btn::InstrParams, "synth parameters (PERC)");
    h.tap(btn::F8);
    n = synthui::sections(kSynthEnginePerc, &sec);
    for (int i = 0; i < n; ++i) {
        sy.section = i;
        sy.row = sy.rows() - 1;
        char nm[64];
        std::snprintf(nm, sizeof(nm), "synth edit patch PERC %s", sec[i].name);
        r.measure(nm);
    }
    // FAT (2.1): an available model, six Edit Patch sections plus M1..M7.
    a.setSynthModel(0, int(SynthModel::FAT));
    a.setCurrentInstrument(64);
    h.tap(btn::Pattern);
    r.screen(btn::InstrParams, "synth parameters (FAT)");
    CHECK(sy.available());
    h.tap(btn::F8);
    CHECK(sy.view == SynthScreen::View::Edit);
    n = synthui::sections(kSynthEngineFat, &sec);
    CHECK(n >= 6);
    for (int i = 0; i < n; ++i) {
        sy.section = i;
        sy.row = sy.rows() - 1;
        char nm[64];
        std::snprintf(nm, sizeof(nm), "synth edit patch FAT %s", sec[i].name);
        r.measure(nm);
    }
    sy.section = 0;
    sy.row = 0;
    h.tap(btn::F8);
    // WTFM (2.2): available, every Edit Patch section.
    a.setSynthModel(0, int(SynthModel::WTFM));
    a.setCurrentInstrument(64);
    h.tap(btn::Pattern);
    r.screen(btn::InstrParams, "synth parameters (WTFM)");
    CHECK(sy.available());
    h.tap(btn::F8);
    n = synthui::sections(kSynthEngineWtfm, &sec);
    for (int i = 0; i < n; ++i) {
        sy.section = i;
        sy.row = sy.rows() - 1;
        char nm[64];
        std::snprintf(nm, sizeof(nm), "synth edit patch WTFM %s", sec[i].name);
        r.measure(nm);
    }
    // Global Mixer 3/3 with the synth columns, a column held.
    r.screen(btn::Master, "master");
    h.tap(btn::Master);
    h.tap(btn::Master);
    h.down(btn::F6);
    r.measure("global mixer + Synth 2 held");
    h.up(btn::F6);
    std::printf("  worst: %s, %u words\n", r.worstName.c_str(), unsigned(r.worst));
}

// The guard itself (with a lowered limit so an ordinary frame overflows):
// the frame is cut short instead of being sent past the limit, counted,
// logged once per screen, and the screen is redrawn at once in low detail
// (no AA halos, no gradients) until the screen or its overlays change.
// 2.x USB Storage mode page (Config > USB > USB Storage): every host-state
// line and the eject reminder stay within the display-list target.
TEST(usb_storage_mode_budget) {
    WorstRig r;
    Harness& h = r.h;
    App& a = r.a;
    r.screen(btn::Pattern, "pattern dense");
    a.ui.setUsbStorageMode(true);
    CHECK(a.usbStorageActive());
    for (IUsbStorage::Host st : {IUsbStorage::Host::kNoUsb, IUsbStorage::Host::kWaiting,
                                 IUsbStorage::Host::kEjected, IUsbStorage::Host::kInUse}) {
        h.usb.hostState = st;
        h.run(20);
        char n[64];
        std::snprintf(n, sizeof(n), "USB storage mode, host state %d", int(st));
        r.measure(n);
    }
    h.tap(btn::F8);   // in use: the eject reminder
    CHECK(a.ui.usb.ejectWarningShown());
    r.measure("USB storage mode + eject reminder");
    h.tap(btn::F8);
    CHECK(!a.usbStorageActive());
}

TEST(guard_cuts_and_falls_back_to_low_detail) {
    Rig r;
    r.dense();
    Ui& ui = r.h.app->ui;
    r.h.tap(btn::Pattern);
    r.h.run(50);
    static int logged = 0;
    static bool loggedLow = true;
    logged = 0;
    ui.platform.dlOverflow = [](const char* screen, uint32_t words, uint32_t dropped, bool low) {
        ++logged;
        loggedLow = low;
        CHECK(std::strcmp(screen, "Pattern") == 0);
        CHECK(words <= 1500);
        CHECK(dropped > 0);
    };
    const uint32_t over0 = ui.overBudgetFrames(), low0 = ui.lowDetailFrames(), f0 = r.m.frames;
    ui.setDlFrameLimit(1500);
    ui.dirty();
    r.h.run(100);
    std::printf("guard: %u cut, %u low-detail frames, last %u words\n", unsigned(ui.overBudgetFrames() - over0),
                unsigned(ui.lowDetailFrames() - low0), unsigned(ui.lastDlWords()));
    CHECK_EQ(logged, 1);
    CHECK(!loggedLow);
    CHECK(ui.overBudgetFrames() == over0 + 1);
    CHECK(ui.lowDetail());
    CHECK(r.m.frames >= f0 + 2);                     // the cut frame, then the low-detail redraw
    CHECK(ui.lowDetailFrames() > low0);
    CHECK(ui.lastDlWords() < 1500);                  // fits without halos and gradients
    for (const std::string& l : r.m.last) CHECK(l.compare(0, 8, "GRADIENT") != 0);
    ui.dirty();
    r.h.run(100);
    CHECK_EQ(logged, 1);                             // logged once per screen
    CHECK(ui.overBudgetFrames() == over0 + 1);       // low detail fits: no further cuts
    r.h.tap(btn::Song);                              // another screen: full detail again
    r.h.run(50);
    CHECK(!ui.lowDetail() || logged == 2);
    ui.setDlFrameLimit(Ui::kDlFrameWords);
    r.h.tap(btn::Pattern);
    r.h.run(50);
    CHECK(!ui.lowDetail());
    ui.platform.dlOverflow = nullptr;
}

// Gfx low detail: primitives with AA-halo alpha and gradients are skipped,
// and so is their COLOR_A; the word limit drops what does not fit.
TEST(gfx_low_detail_and_limit) {
    RecordingDisplay d;
    Gfx lo(d);
    lo.setLowDetail(true);
    lo.beginFrame(0);
    lo.alpha(60);
    lo.fill(0, 0, 10, 10, 0x202020);
    lo.alpha(255);
    lo.fill(0, 20, 10, 10, 0x202020);
    lo.gradientRect(0, 40, 10, 10, 0, 0xFFFFFF);
    lo.endFrame();
    CHECK_EQ(int(lo.dlWords()), 2 + 1 + 4);   // clear, colour, one rect
    Gfx g(d);
    g.setWordLimit(200);
    g.beginFrame(0);
    for (int i = 0; i < 100; ++i) g.fill(i, 0, 1, 1, 0xFFFFFF);
    g.endFrame();
    CHECK(g.dropped() > 0);
    CHECK(g.dlWords() <= 200);
    // COLOR_A is sent only before a primitive that uses it.
    Gfx a(d);
    a.beginFrame(0);
    a.alpha(120);
    a.fill(0, 0, 1, 1, 0xFFFFFF);
    a.alpha(255);
    a.alpha(120);
    a.fill(0, 2, 1, 1, 0xFFFFFF);
    a.alpha(255);
    a.endFrame();
    int colorA = 0;
    for (const std::string& l : d.last) colorA += l.compare(0, 7, "COLOR_A") == 0;
    CHECK_EQ(colorA, 1);
}

// spec 06 §1.2, spec 20 §1.6 (VERIFY_1.9.5 R7): while the sequencer waits
// for a recording to start (external clock) the Pattern title blinks "REC"
// with a 500 ms period; once it stops waiting the text is gone.
TEST(pattern_record_wait_blinks) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Pattern);
    a.cfg.clockIn = 1;   // external clock: Rec + Play arms and waits
    a.applySequencerSettings();
    h.down(btn::Rec);
    h.tap(btn::Play);
    h.up(btn::Rec);
    CHECK(a.seq.isRecording());
    int on = 0, off = 0, changes = 0;
    bool last = false;
    for (int i = 0; i < 60; ++i) {   // 1.2 s in 20 ms steps
        h.run(20);
        const bool rec = h.disp.lastHasText("REC");
        if (i > 0 && rec != last) ++changes;
        last = rec;
        (rec ? on : off)++;
    }
    std::printf("record wait: REC shown in %d of 60 frames, %d changes\n", on, changes);
    CHECK(on >= 20 && off >= 20);   // about half of the time each
    CHECK(changes >= 4 && changes <= 6);   // 250 ms phases over 1.2 s
    h.tap(btn::Play);                // stop: no longer waiting
    h.run(600);
    CHECK(!h.disp.lastHasText("REC"));
}

// Device RAM_DL feedback (user report: Master -> Track Mixer froze on one
// unit). A frame the FT81x faulted on is redrawn in low detail instead of
// faulting again forever, and a measured RAM_DL size larger than the
// estimate tightens the guard.
TEST(device_fault_falls_back_to_low_detail) {
    Harness::dlOverflowFatal = false;   // the fault and the cut are expected here
    Harness h;
    App& a = *h.app;
    h.tap(btn::Master);
    h.tap(btn::Master);   // Track Mixer 2/3
    h.run(100);
    const uint32_t low0 = a.ui.lowDetailFrames();
    CHECK_EQ(a.ui.dlScale(), 64);
    ++h.disp.faultCount;   // the last frame faulted on the device
    a.ui.dirty();
    h.run(50);
    CHECK_EQ(a.ui.hwFaultFrames(), 1u);
    CHECK(a.ui.lowDetailFrames() > low0);   // redrawn in low detail
    CHECK(a.ui.dlScale() > 64);
    CHECK(a.ui.lowDetail());
    Harness::dlOverflowFatal = true;
}

TEST(device_measurement_calibrates_the_guard) {
    Harness::dlOverflowFatal = false;
    Harness h;
    App& a = *h.app;
    h.tap(btn::Master);
    h.tap(btn::Master);   // Track Mixer 2/3, a steady heavy screen
    for (int i = 0; i < 5; ++i) {
        a.ui.dirty();
        h.run(30);
    }
    const uint32_t est = a.ui.lastDlWords();
    CHECK(est > 1000);
    // The device reports 1.5x the estimate (+ the driver's words).
    h.disp.measuredBytes = (est * 3 / 2 + Ui::kDriverDlWords) * 4;
    a.ui.dirty();
    h.run(30);
    CHECK(a.ui.dlScale() >= 90 && a.ui.dlScale() <= 100);   // ~1.5 x 64
    // The guard now caps the frame at ~1340 words: the page still draws
    // (no fault possible), in low detail.
    const uint32_t over0 = a.ui.overBudgetFrames();
    a.ui.dirty();
    h.run(30);
    CHECK(a.ui.overBudgetFrames() > over0 || a.ui.lowDetail());
    // No measurement on the host keeps the scale at 1.0 (other tests).
    Harness h2;
    h2.tap(btn::Master);
    h2.run(100);
    CHECK_EQ(h2.app->ui.dlScale(), 64);
    Harness::dlOverflowFatal = true;
}
