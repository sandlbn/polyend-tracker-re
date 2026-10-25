// File > Export naming and sample export, Performance FX on the engine,
// the horizontal pattern arrangement and Shift + Power screenshots.
#include <cmath>

#include "test.h"
#include "ui_fakes.h"

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
PerformanceScreen& perfScreen(Harness& h) { return *static_cast<PerformanceScreen*>(h.app->ui.screen(kModPerformance)); }
}  // namespace

TEST(export_names) {
    Harness h;
    App& a = *h.app;
    char p[160];
    CHECK(a.exportName(App::ExportKind::Song, p, sizeof(p)));
    // spec 16 §A.12: a new folder per export with master.wav; _P<n>_ unpadded.
    CHECK_STR(p, "Export/New Project/New Project_Song_0001/master.wav");
    CHECK(a.exportName(App::ExportKind::Song, p, sizeof(p), true));
    CHECK_STR(p, "Export/New Project/New Project_Song_S0001");
    CHECK(a.exportName(App::ExportKind::Pattern, p, sizeof(p)));
    CHECK_STR(p, "Export/New Project/New Project_P1_0001/master.wav");
    h.fs.mkdir("Export/New Project/New Project_P1_0001");
    CHECK(a.exportName(App::ExportKind::Pattern, p, sizeof(p)));
    CHECK_STR(p, "Export/New Project/New Project_P1_0002/master.wav");
    CHECK(a.exportName(App::ExportKind::Pattern, p, sizeof(p), true));
    CHECK_STR(p, "Export/New Project/New Project_P1_S0001");
    CHECK(a.exportName(App::ExportKind::Selection, p, sizeof(p)));
    CHECK_STR(p, "Export/New Project/Selection/Selection0001.wav");
}

TEST(export_samples_from_file_screen) {
    Harness h;
    App& a = *h.app;
    addWav(h, "kick.wav", 3000);
    addWav(h, "snare.wav", 5000);
    CHECK(a.importSample("kick.wav", 0));
    CHECK(a.importSample("snare.wav", 4));
    h.tap(btn::File);
    h.tap(btn::F6);   // Export page
    CHECK_STR(a.ui.bar.label[5].bottom, "Samples");
    CHECK(a.ui.bar.label[5].enabled);
    CHECK(a.ui.bar.label[4].enabled);    // As .it (spec 11 §2, mod_it_test)
    h.tap(btn::F6);
    const char* k = "Projects/New Project/export/samples/01_kick.wav";
    const char* s = "Projects/New Project/export/samples/05_snare.wav";
    CHECK(h.fs.files.count(k));
    CHECK(h.fs.files.count(s));
    WavInfo info;
    CHECK(wavParseHeader(h.fs.files[s].data(), h.fs.files[s].size(), info));
    CHECK_EQ(info.channels, 1);
    CHECK_EQ(info.sampleRate, 44100u);
    CHECK_EQ(info.dataSize, 5000u * 2u);
    CHECK(std::memcmp(h.fs.files[s].data() + 44, a.bank.left(4), 5000 * 2) == 0);
    CHECK_EQ(a.exportSamples(), 2);
}

TEST(export_external_clock_prompt) {
    Harness h;
    App& a = *h.app;
    a.cfg.clockIn = 2;
    h.tap(btn::File);
    h.tap(btn::F6);
    h.tap(btn::F3);   // pattern
    CHECK(a.ui.askOpen());
    h.tap(btn::F7);   // cancel
    bool any = false;
    for (auto& f : h.fs.files) any = any || f.first.find("_P01_") != std::string::npos;
    CHECK(!any);
}

TEST(performance_fx_reach_the_engine) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Performance);
    PerformanceScreen& p = perfScreen(h);
    const perf::Source& s = p.src;
    // Default slots show FX 1..12 with zero values (spec 12 §A.2).
    CHECK_EQ(s.type(0), 1);
    CHECK_EQ(s.value(0, 1), 0);
    s.setValue(0, 1, 30);
    s.setValue(1, 1, 40);
    s.setValue(3, 1, -20);
    h.pad(12);   // column 0, row 1
    for (int t = 0; t < 8; ++t) CHECK_EQ(a.engine.voice(t).perf().volume, 30);
    h.pad(13);   // panning row 1: offset in pan units, not halved (spec 12 §E.2)
    CHECK_EQ(a.engine.voice(3).perf().pan, 40);
    h.pad(12 + 3);   // LP cutoff row 1
    CHECK_EQ(a.engine.voice(0).perf().filterType, 0);
    CHECK_NEAR(a.engine.voice(0).perf().cutoff, -0.2, 1e-6);
    // F1 release toggles "performance FX enabled" for track 1: reverted there only.
    h.tap(btn::F1);
    CHECK(!p.fxOn[0]);
    CHECK_EQ(a.engine.voice(0).perf().volume, 0);
    CHECK_EQ(a.engine.voice(0).perf().filterType, -1);
    CHECK_EQ(a.engine.voice(1).perf().volume, 30);
    h.tap(btn::F1);
    CHECK_EQ(a.engine.voice(0).perf().volume, 30);
    // Row 0 reverts the slot.
    h.pad(0);
    CHECK_EQ(a.engine.voice(0).perf().volume, 0);
    // Other types: reverse, start, LFO speed, bit depth, overdrive, sends.
    s.setType(6, 11);
    s.setValue(6, 1, 1);
    h.pad(12 + 6);
    CHECK(a.engine.voice(2).perf().reverse);
    s.setType(7, 9);
    s.setValue(7, 2, 50);
    h.pad(24 + 7);
    CHECK_EQ(a.engine.voice(2).perf().start, 32767);   // (50+100)*131070/200 - 65535
    s.setType(8, 13);
    s.setValue(8, 1, -5);
    h.pad(12 + 8);
    CHECK_EQ(a.engine.voice(2).perf().lfoSpeed[kAutoPanning], -5);
    s.setType(10, 21);
    s.setValue(10, 3, 70);
    h.pad(36 + 10);
    CHECK_EQ(a.engine.voice(5).perf().od, 70);
    s.setType(11, 20);
    s.setValue(11, 1, -6);
    h.pad(12 + 11);
    CHECK_EQ(a.engine.voice(5).perf().bits, -6);
    s.setType(9, 8);
    s.setValue(9, 1, 40);
    h.pad(12 + 9);
    CHECK_EQ(a.engine.voice(5).perf().rsend, 40);
    // Leaving the screen reverts everything.
    h.tap(btn::Pattern);
    for (int t = 0; t < 8; ++t) {
        const audio::PerfOffsets& o = a.engine.voice(t).perf();
        CHECK(o.volume == 0 && o.pan == 0 && !o.reverse && o.start == 0 && o.od == 0 && o.bits == 0 && o.filterType == -1);
    }
}

TEST(performance_offsets_change_the_sound) {
    Harness h;
    App& a = *h.app;
    addWav(h, "saw.wav", 44100);
    CHECK(a.importSample("saw.wav", 0));
    VoiceNoteOn n;
    n.instrument = 0;
    n.note = 60;
    a.engine.voiceNoteOn(0, n);
    h.audio.pump(2);
    const float r0 = a.engine.voice(0).rate();
    audio::PerfOffsets p;
    p.tune = 12;
    a.engine.setPerf(0, p);
    h.audio.pump(1);
    CHECK_NEAR(a.engine.voice(0).rate() / r0, 2.0, 0.02);
    // Volume -100 silences the track.
    p = audio::PerfOffsets{};
    p.volume = -100;
    a.engine.setPerf(0, p);
    h.audio.pump(4);
    CHECK_EQ(h.audio.pump(4), 0);
    a.engine.setPerf(0, audio::PerfOffsets{});
    CHECK(h.audio.pump(4) > 0);
}

TEST(pattern_horizontal_arrangement) {
    Harness h;
    App& a = *h.app;
    PatternScreen& pe = *static_cast<PatternScreen*>(a.ui.screen(kModPattern));
    const int len = a.pattern().tracks[0].lastStep + 1;
    char last[12];
    std::snprintf(last, sizeof(last), "\"%d\"", len);   // step numbers are left-aligned
    // A step number in the left gutter (x 3) or a horizontal step header.
    auto stepShown = [&]() {
        for (auto& l : h.disp.last)
            if (l.compare(0, 5, "TEXT ") == 0 && l.find(last) != std::string::npos && l.find("TEXT 3 ") == 0)
                return true;
        return false;
    };
    // Revolver (0): the window wraps, so the last step shows above step 1.
    h.run(30);
    CHECK(stepShown());
    a.cfg.arrangement = 2;
    a.ui.dirty();
    h.run(30);
    CHECK(!stepShown());   // rows outside 0..len-1 are blank
    CHECK_EQ(pe.windowTop(), -2);        // cursor 0 kept within anchor-5..anchor+6
    // Arrangement 2 arrows (spec 06 §1.5 table 0xa71e4): Right = step+1, Left = step-1.
    for (int i = 0; i < 11; ++i) h.tap(btn::Right);
    CHECK_EQ(pe.step, 11);
    CHECK_EQ(pe.windowTop(), -2);        // the window has not moved
    h.tap(btn::Right);
    CHECK_EQ(pe.windowTop(), -1);        // scrolls by one at anchor+6
    for (int i = 0; i < 12; ++i) h.tap(btn::Left);
    CHECK_EQ(pe.step, 0);
    CHECK_EQ(pe.windowTop(), -2);
}

TEST(screenshot_notice_after_slow_capture) {
    // The capture blocks for ~2.6 s on the device (T00037). The notice must
    // still show afterwards: its time starts after the capture, not at the
    // key press.
    Harness h;
    App& a = *h.app;
    h.disp.readback = true;
    h.disp.readbackClock = &h.clock.ms;
    h.disp.readbackMs = 6;   // 481 calls: ~2.9 s
    h.down(btn::Shift);
    h.tap(btn::Power);
    h.up(btn::Shift);
    CHECK(h.fs.files.count("Snapshots/Snapshot_001.bmp"));
    CHECK(std::string(a.ui.notice()) == "Snapshots/Snapshot_001.bmp");
    h.run(1500);
    CHECK(std::string(a.ui.notice()) == "Snapshots/Snapshot_001.bmp");
    h.run(1000);
    CHECK(a.ui.notice()[0] == 0);
}

TEST(screenshot_bmp) {
    Harness h;
    App& a = *h.app;
    // No readback: reported, nothing written.
    h.down(btn::Shift);
    h.tap(btn::Power);
    h.up(btn::Shift);
    CHECK(!h.fs.files.count("Snapshots/Snapshot_001.bmp"));
    h.disp.readback = true;
    h.tap(btn::Power);   // without Shift: no screenshot
    CHECK(!h.fs.files.count("Snapshots/Snapshot_001.bmp"));
    h.down(btn::Shift);
    h.tap(btn::Power);
    h.tap(btn::Power);
    h.up(btn::Shift);
    CHECK(h.fs.files.count("Snapshots/Snapshot_002.bmp"));
    const auto& f = h.fs.files["Snapshots/Snapshot_001.bmp"];
    CHECK_EQ(f.size(), 1536054u);
    CHECK(f[0] == 'B' && f[1] == 'M');
    auto u32 = [&](size_t o) { return uint32_t(f[o]) | uint32_t(f[o + 1]) << 8 | uint32_t(f[o + 2]) << 16 | uint32_t(f[o + 3]) << 24; };
    CHECK_EQ(u32(2), 1536054u);
    CHECK_EQ(u32(10), 54u);
    CHECK_EQ(u32(18), 800u);
    CHECK_EQ(u32(22), 480u);
    CHECK_EQ(f[28], 32);
    // Bottom-up rows, BGRA: first stored pixel is (x 0, y 479).
    CHECK_EQ(f[54 + 0], 0x5A);
    CHECK_EQ(f[54 + 1], uint8_t(479 & 0xFF));
    CHECK_EQ(f[54 + 2], 0);
    // Pixel (x 10, y 0) is in the last stored row.
    const size_t o = 54 + size_t(479) * 3200 + 10 * 4;
    CHECK_EQ(f[o + 2], 10);
    CHECK_EQ(f[o + 1], 0);
    (void)a;
}
