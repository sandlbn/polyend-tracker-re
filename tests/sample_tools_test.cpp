// Sample tools (Batch E): auto-slice detector (spec 13 §F), slice page and
// zoom of Sample Playback (05 App. B §1.4b/§1.9/§1.10), snap to zero
// (spec 13 §H.2), Sample Loader gating and list filter (06 §5.2/§5.5),
// Instrument automation destination keys and Enc-push audition (App. C §3).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "app/auto_slice.h"
#include "audio/zero_snap.h"
#include "formats/instrument_pti.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {
PlaybackScreen& pb(Harness& h) { return *static_cast<PlaybackScreen*>(h.app->ui.screen(kModPlayback)); }
LoaderScreen& ld(Harness& h) { return *static_cast<LoaderScreen*>(h.app->ui.screen(kModLoader)); }
InstrumentScreen& ins(Harness& h) { return *static_cast<InstrumentScreen*>(h.app->ui.screen(kModInstrument)); }

// Silence with constant bursts (amplitude 10000) starting at the given frames.
std::vector<int16_t> bursts(uint32_t len, std::initializer_list<uint32_t> at, uint32_t burstLen = 8000) {
    std::vector<int16_t> s(len, 0);
    for (uint32_t a : at)
        for (uint32_t i = a; i < a + burstLen && i < len; ++i) s[i] = int16_t((i & 1) ? 10000 : -10000);
    return s;
}

void putWav(Harness& h, const char* path, const std::vector<int16_t>& s) {
    std::vector<uint8_t> out(s.size() * 2 + 64);
    out.resize(wavEncode(s.data(), uint32_t(s.size()), 1, out.data(), out.size()));
    h.fs.files[path] = out;
}

uint32_t frameOf(uint16_t v, uint32_t len) { return uint32_t(uint64_t(v) * len / 65535); }
}  // namespace

// ---------------------------------------------------------------------------
// §F detector

TEST(autoslice_threshold_formula) {
    const uint32_t flat[5] = {100, 100, 100, 100, 100};
    CHECK_NEAR(AutoSlicer::threshold(flat, 5), 162.951f, 0.01f);   // var 0 -> C = 1.62951
    const uint32_t spike[5] = {0, 0, 0, 0, 100000};
    // mean 20000, var 1.6e9 -> C = 1.62951 - 0.25
    CHECK_NEAR(AutoSlicer::threshold(spike, 5), 20000.0f * 1.37951f, 1.0f);
    const uint32_t busy[5] = {0, 0, 0, 0, 200000};
    // var 6.4e9 -> low 32 bits 2105032704 -> C = 1.62951 - 0.3289 = 1.3006
    CHECK_NEAR(AutoSlicer::threshold(busy, 5), 40000.0f * float(1.62951 - 1.5625e-10 * 2105032704.0), 1.0f);
}

TEST(autoslice_short_sample_does_nothing) {
    std::vector<int16_t> s(10239, 1000);
    uint16_t out[48] = {};
    uint8_t n = 7;
    AutoSlicer d;
    CHECK(!d.start(s.data(), uint32_t(s.size()), out, &n));
    CHECK(!d.busy());
    CHECK_EQ(n, 7);
}

TEST(autoslice_incremental_onsets) {
    const uint32_t len = 200000;
    const auto s = bursts(len, {50000, 120000});
    uint16_t out[48] = {};
    uint8_t n = 0;
    AutoSlicer d;
    CHECK(d.start(s.data(), len, out, &n));
    // One 2048-frame block per step.
    int steps = 0;
    while (d.step()) {
        ++steps;
        CHECK_EQ(n, 0);   // count published at the end only
    }
    CHECK_EQ(steps + 1, int((len + 2047) / 2048));
    CHECK_EQ(n, 3);
    CHECK_EQ(out[0], 0);   // silent ring: forced first slice at 0
    // Refined onsets sit 640 frames (5 sub-blocks) before the first
    // 128-frame sub-block over the threshold.
    const uint32_t p1 = frameOf(out[1], len), p2 = frameOf(out[2], len);
    CHECK(p1 >= 50000 - 640 - 130 && p1 <= 50000 - 640 + 130);
    CHECK(p2 >= 120000 - 640 - 130 && p2 <= 120000 - 640 + 130);
    CHECK_EQ(out[1], uint16_t((65535.0f / float(len)) * 49408.0f));   // s = 43008, k = 55
}

TEST(autoslice_spacing_and_max) {
    // Bursts every 6000 frames: the 10240-frame spacing drops some.
    const uint32_t len = 400000;
    std::vector<int16_t> s(len, 0);
    for (uint32_t a = 20000; a + 400 < len; a += 6000)
        for (uint32_t i = a; i < a + 400; ++i) s[i] = 12000;
    uint16_t out[48] = {};
    uint8_t n = 0;
    AutoSlicer d;
    CHECK(d.start(s.data(), len, out, &n));
    d.finish();
    CHECK(n > 2);
    for (int k = 2; k < n; ++k) CHECK(frameOf(out[k], len) > frameOf(out[k - 1], len) + 9000);
    // Many onsets: never more than 48.
    std::vector<int16_t> t(48 * 13000 + 60000, 0);
    for (uint32_t a = 15000; a + 300 < t.size(); a += 11000)
        for (uint32_t i = a; i < a + 300; ++i) t[i] = 15000;
    AutoSlicer e;
    CHECK(e.start(t.data(), uint32_t(t.size()), out, &n));
    e.finish();
    CHECK_EQ(n, 48);
}

// ---------------------------------------------------------------------------
// Slice page

TEST(playback_slice_page_auto_evenly_confirm) {
    Harness h;
    App& a = *h.app;
    putWav(h, "b.wav", bursts(200000, {50000, 120000}));
    CHECK(a.importSample("b.wav", 0));
    a.setCurrentInstrument(0);
    a.instruments[0].playMode = uint8_t(PlayMode::BeatSlice);
    h.tap(btn::SamplePlayback);
    PlaybackScreen& p = pb(h);
    CHECK_EQ(p.sel, 6);   // new instrument: selection = Play Mode
    CHECK_STR(a.ui.bar.label[0].top, "Slice");
    CHECK_STR(a.ui.bar.label[0].bottom, "-");
    CHECK_STR(a.ui.bar.label[4].top, "Zoom");
    CHECK_STR(a.ui.bar.label[5].top, "More");
    // More page.
    h.tap(btn::F6);
    CHECK(p.more);
    CHECK_STR(a.ui.bar.label[0].bottom, "Auto");
    CHECK_STR(a.ui.bar.label[1].bottom, "Evenly");
    CHECK_STR(a.ui.bar.label[2].top, "No of Slices");
    // Auto without slices: no confirmation, runs over the main loop.
    h.down(btn::F1);
    CHECK(!a.ui.askOpen());
    h.up(btn::F1);
    h.run(2000);
    CHECK(!p.slicer.busy());
    CHECK_EQ(a.instruments[0].sliceCount, 3);
    // Pads 0..count-1 lit in Beat Slice.
    uint8_t lv[48] = {};
    p.padLeds(lv);
    CHECK(lv[2] > 0 && lv[3] == 0);
    // No of Slices (More page b2) and Evenly with the overwrite confirmation.
    h.tap(btn::F3);
    CHECK_EQ(p.sel, 2);
    h.enc(2);
    CHECK_EQ(a.cfg.sliceCount, 10);
    h.tap(btn::F2);
    CHECK(a.ui.askOpen());
    h.tap(btn::F7);   // Cancel
    CHECK_EQ(a.instruments[0].sliceCount, 3);
    h.tap(btn::F2);
    h.tap(btn::F8);   // Confirm
    CHECK_EQ(a.instruments[0].sliceCount, 10);
    CHECK_EQ(a.instruments[0].slices[1], 6553);
    // Auto again asks first.
    h.tap(btn::F1);
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);
    h.run(2000);
    CHECK_EQ(a.instruments[0].sliceCount, 3);
    // Page 0: Slice "N of M", Insert adds after the selected slice, pads select.
    h.tap(btn::F6);
    CHECK(!p.more);
    CHECK_STR(a.ui.bar.label[0].bottom, "1 of 3");
    h.pad(2);
    CHECK_EQ(a.instruments[0].selectedSlice, 2);
    h.pad(30);   // no such slice: selection kept
    CHECK_EQ(a.instruments[0].selectedSlice, 2);
    h.tap(btn::Insert);
    CHECK_EQ(a.instruments[0].sliceCount, 4);
    CHECK_EQ(a.instruments[0].selectedSlice, 3);
    CHECK_EQ(a.instruments[0].slices[3], uint16_t((a.instruments[0].slices[2] + 65535) / 2));
    // Delete removes it when Slice/Adjust is selected.
    h.tap(btn::F1);
    h.tap(btn::Delete);
    CHECK_EQ(a.instruments[0].sliceCount, 3);
    CHECK_EQ(a.instruments[0].selectedSlice, 2);
    // Enc-push = pad 12 (no slice 12 here: selection unchanged).
    h.tap(btn::EncPush);
    CHECK_EQ(a.instruments[0].selectedSlice, 2);
    // Zoom (b4) is spp-based.
    h.tap(btn::F5);
    CHECK_EQ(p.sel, 4);
    h.enc(10);
    CHECK(p.zoom() > 1.0f);
}

TEST(playback_auto_finishes_when_leaving) {
    Harness h;
    App& a = *h.app;
    putWav(h, "b.wav", bursts(400000, {50000, 300000}));
    CHECK(a.importSample("b.wav", 0));
    a.setCurrentInstrument(0);
    a.instruments[0].playMode = uint8_t(PlayMode::Slice);
    h.tap(btn::SamplePlayback);
    PlaybackScreen& p = pb(h);
    p.slicesAuto();
    CHECK(p.slicer.busy());
    a.ui.update(a.millis());
    CHECK(p.slicer.busy());
    h.tap(btn::SampleLoader);
    CHECK(!p.slicer.busy());
    CHECK_EQ(a.instruments[0].sliceCount, 3);
}

TEST(playback_zoom_points_and_multiselect) {
    Harness h;
    App& a = *h.app;
    putWav(h, "z.wav", bursts(120000, {1000}));
    CHECK(a.importSample("z.wav", 0));
    a.setCurrentInstrument(0);
    a.instruments[0].playMode = uint8_t(PlayMode::ForwardLoop);
    a.instruments[0].startPoint = 1000;
    a.instruments[0].loopPoint1 = 2000;
    a.instruments[0].loopPoint2 = 60000;
    a.instruments[0].endPoint = 65000;
    h.tap(btn::SamplePlayback);
    PlaybackScreen& p = pb(h);
    // App. B §1.9: spp from len/600 (full) down to len/65535+1, ~100 levels.
    h.tap(btn::F6);
    CHECK_EQ(p.sel, 5);
    CHECK_STR(a.ui.bar.label[5].bottom, "1.00");
    h.enc(1);
    const uint32_t mx = 120000 / 600, mn = 120000 / 65535 + 1, st = (mx - mn) / 100 > 1 ? (mx - mn) / 100 : 1;
    CHECK_EQ(p.view.spp, mx - st);
    h.enc(127);
    h.enc(127);
    CHECK_EQ(p.view.spp, mn);
    // One detent = one pixel of the view.
    const int px = p.view.pixelUnits();
    h.tap(btn::F2);
    h.enc(1);
    CHECK_EQ(a.instruments[0].startPoint, 1000 + px);
    // Holding Start + End moves both (§1.5).
    h.down(btn::F2);
    h.down(btn::F5);
    CHECK_EQ(p.heldPoints, 9);
    h.enc(-2);
    h.up(btn::F2);
    h.up(btn::F5);
    CHECK_EQ(a.instruments[0].startPoint, 1000 + px - 2 * px);
    CHECK_EQ(a.instruments[0].endPoint, 65000 - 2 * px);
    CHECK_EQ(p.heldPoints, 0);
    // Start pushes the loop window right, length kept (§1.8).
    a.instruments[0].startPoint = 1000;
    a.instruments[0].loopPoint1 = 1001;
    a.instruments[0].loopPoint2 = 1501;
    h.tap(btn::F2);
    h.enc(3);
    CHECK_EQ(a.instruments[0].loopPoint1, a.instruments[0].startPoint + 1);
    CHECK_EQ(a.instruments[0].loopPoint2 - a.instruments[0].loopPoint1, 500);
    // Play Mode: F7/F8 first select, then step; to Wavetable fixes the window.
    h.tap(btn::F8);
    CHECK_EQ(p.sel, 6);
    a.instruments[0].wavetableWindowSize = 1000;
    h.tap(btn::F8);
    h.tap(btn::F8);
    h.tap(btn::F8);
    h.tap(btn::F8);
    h.tap(btn::F8);
    CHECK_EQ(a.instruments[0].playMode, uint8_t(PlayMode::Wavetable));
    CHECK_EQ(a.instruments[0].wavetableWindowSize, 512);
    CHECK_EQ(a.instruments[0].wavetableWindowCount, 120000u / 512);
    // Window size change rescales the position.
    a.instruments[0].wavetableCurrentWindow = 40;
    h.tap(btn::F3);
    h.enc(1);
    CHECK_EQ(a.instruments[0].wavetableWindowSize, 1024);
    CHECK_EQ(a.instruments[0].wavetableCurrentWindow, 20u);
}

// ---------------------------------------------------------------------------
// §H.2 snap to zero

TEST(snap_to_zero_search_and_indicator) {
    std::vector<int16_t> x(1000, 500);
    for (int i = 600; i < 1000; ++i) x[i] = -500;   // sign change between 599 and 600
    CHECK_EQ(audio::snapToZero(x.data(), 1000, 560), 599);
    CHECK_EQ(audio::snapToZero(x.data(), 1000, 650), 599);
    CHECK_EQ(audio::snapToZero(x.data(), 1000, 300), 300);   // none within 64: exact
    x[540] = 0;
    CHECK_EQ(audio::snapToZero(x.data(), 1000, 560), 540);   // nearer hit wins
    // Indicator: off -> only an exact zero; on -> crossing inside the window.
    const uint16_t m540 = uint16_t((540 * 65535 + 999) / 1000);
    CHECK(audio::markerOnCrossing(x.data(), 1000, m540, false));
    CHECK(!audio::markerOnCrossing(x.data(), 1000, uint16_t(560 * 65535 / 1000 + 1), false));
    CHECK(audio::markerOnCrossing(x.data(), 1000, uint16_t(560 * 65535 / 1000 + 1), true));
    CHECK(!audio::markerOnCrossing(x.data(), 1000, uint16_t(200 * 65535 / 1000), true));
}

TEST(snap_to_zero_project_option_and_markers_unchanged) {
    Harness h;
    App& a = *h.app;
    std::vector<int16_t> s(44100);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(10000 * std::sin(double(i) * 0.01));
    putWav(h, "s.wav", s);
    CHECK(a.importSample("s.wav", 0));
    a.setCurrentInstrument(0);
    a.instruments[0].startPoint = 1000;
    h.tap(btn::SamplePlayback);
    PlaybackScreen& p = pb(h);
    CHECK_EQ(p.zeroMask() & 1, 0);
    a.project.setU8(0x602, 1);
    a.markProjectDirty();
    h.audio.pump(1);
    CHECK(audio::gSnapToZero);
    // 1000/65535*44100 = 672.9 frames; the sine crosses at 628 (pi/0.01*2).
    CHECK_EQ(p.zeroMask() & 1, 1);
    a.previewOn(0, 0, 60, 127);
    h.audio.pump(2);
    a.previewAllOff();
    CHECK_EQ(a.instruments[0].startPoint, 1000);   // markers never rewritten
    a.project.setU8(0x602, 0);
    a.markProjectDirty();
    h.audio.pump(1);
    CHECK(!audio::gSnapToZero);
}

// ---------------------------------------------------------------------------
// Sample Loader

TEST(loader_lists_only_valid_wav_headers) {
    Harness h;
    h.fs.mkdir("S");
    putWav(h, "S/good.wav", std::vector<int16_t>(1000, 5));
    h.fs.files["S/bad.wav"] = std::vector<uint8_t>(100, 0x41);
    h.fs.files["S/notes.txt"] = {1, 2, 3};
    h.fs.files["S/inst.pti"] = std::vector<uint8_t>(400, 0);
    h.tap(btn::SampleLoader);
    LoaderScreen& l = ld(h);
    std::snprintf(l.dir, sizeof(l.dir), "S");
    l.list();
    bool good = false, bad = false, txt = false, pti = false;
    for (int i = 0; i < l.count; ++i) {
        good |= !std::strcmp(l.entries[i], "good.wav");
        bad |= !std::strcmp(l.entries[i], "bad.wav");
        txt |= !std::strcmp(l.entries[i], "notes.txt");
        pti |= !std::strcmp(l.entries[i], "inst.pti");
    }
    CHECK(good && pti && !bad && !txt);
}

TEST(loader_import_gating_by_free_memory) {
    Harness h(true, 120000);   // 240000 bytes of sample memory
    App& a = *h.app;
    putWav(h, "big.wav", std::vector<int16_t>(118000, 100));
    h.tap(btn::SampleLoader);
    LoaderScreen& l = ld(h);
    int idx = -1;
    for (int i = 0; i < l.count; ++i)
        if (!std::strcmp(l.entries[i], "big.wav")) idx = i;
    CHECK(idx >= 0);
    while (l.sdCursor < idx) h.tap(btn::Down);
    h.tap(btn::F5);
    CHECK(l.trim);
    // n = 118000: 2n >= free - 10000 -> Normal disabled; low quality allowed.
    CHECK(!l.importAllowed(false));
    CHECK(l.importAllowed(true));
    CHECK(!a.ui.bar.label[7].enabled);
    CHECK(a.ui.bar.label[6].enabled);
    h.tap(btn::F8);   // disabled: nothing happens
    CHECK(l.trim);
    CHECK(!a.instrPresent[0]);
    // Shorter selection enables Normal.
    h.tap(btn::F3);
    for (int i = 0; i < 4; ++i) h.enc(-127);
    CHECK(l.importAllowed(false));
    CHECK(a.ui.bar.label[7].enabled);
    h.tap(btn::F8);
    CHECK(!l.trim);
    CHECK(a.instrPresent[0]);
    // Occupied target slot: F5 asks to replace first.
    h.tap(btn::F5);
    CHECK(a.ui.askOpen());
    CHECK(!l.trim);
    h.tap(btn::F7);   // Cancel
    CHECK(!a.ui.askOpen());
    CHECK(!l.trim);
}

TEST(loader_focus_keys) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::SampleLoader);
    LoaderScreen& l = ld(h);
    // Volume bar: Up = +1.
    h.tap(btn::F7);
    CHECK_EQ(l.focus, 2);
    const int v = a.cfg.previewVolume;
    h.tap(btn::Up);
    CHECK_EQ(a.cfg.previewVolume, v + 1);
    // Delete on an empty instrument selection asks nothing.
    h.tap(btn::Left);
    CHECK_EQ(l.focus, 1);
    h.tap(btn::Delete);
    CHECK(!a.ui.askOpen());
    // F7 held past a hold tick returns to the SD list on release.
    h.tap(btn::Left);
    CHECK_EQ(l.focus, 0);
    h.down(btn::F7);
    h.run(1500);
    h.up(btn::F7);
    CHECK_EQ(l.focus, 0);
}

// ---------------------------------------------------------------------------
// Instrument screen

TEST(instrument_automation_destination_keys) {
    Harness h;
    h.tap(btn::InstrParams);
    h.tap(btn::InstrParams);   // own key: Parameters -> Automation
    InstrumentScreen& s = ins(h);
    CHECK_EQ(s.mode, 1);
    h.tap(btn::F1);   // selects Destination
    CHECK_EQ(s.dest, 0);
    h.tap(btn::F1);   // previous, clamped
    CHECK_EQ(s.dest, 0);
    h.tap(btn::F2);   // next
    CHECK_EQ(s.dest, 1);
    h.tap(btn::F2);
    CHECK_EQ(s.dest, 2);
    h.tap(btn::F1);
    CHECK_EQ(s.dest, 1);
    // Destination Off: columns > 1 unselectable.
    h.tap(btn::Right);
    h.tap(btn::Right);
    h.tap(btn::Right);
    CHECK_EQ(s.column, 1);
    h.tap(btn::F5);
    CHECK_EQ(s.column, 1);
}

TEST(instrument_encpush_beat_slice_note_and_reset) {
    Harness h;
    App& a = *h.app;
    putWav(h, "b.wav", bursts(50000, {1000}));
    CHECK(a.importSample("b.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::InstrParams);
    InstrumentScreen& s = ins(h);
    CHECK_EQ(s.auditionNote(), a.padNote(12));
    a.instruments[0].playMode = uint8_t(PlayMode::BeatSlice);
    a.instruments[0].sliceCount = 6;
    a.instruments[0].selectedSlice = 5;
    CHECK_EQ(s.auditionNote(), 29);
    h.down(btn::EncPush);
    CHECK(a.auditionTrack(App::kAuditionInstrument) >= 0);
    h.up(btn::EncPush);
    uint8_t lv[48] = {};
    s.padLeds(lv);
    CHECK(lv[5] > 0 && lv[6] == 0);
    // Filter disabled: F6/F7 (Cutoff/Resonance) do nothing.
    h.tap(btn::F6);
    CHECK(s.column != 5);
    // Up on the Filter type list moves up (index - 1).
    a.instruments[0].filterEnabled = 1;
    a.instruments[0].filterType = 1;   // High-pass (list index 2)
    h.tap(btn::F5);
    h.tap(btn::Up);
    CHECK_EQ(a.instruments[0].filterType, 0);
    CHECK_EQ(a.instruments[0].filterEnabled, 1);
    // Automation reset copies one field only.
    h.tap(btn::InstrParams);
    a.instruments[0].env[0].attackMs = 500;
    a.instruments[0].env[0].releaseMs = 2000;
    h.tap(btn::F3);   // Type
    h.tap(btn::Right);   // Attack
    h.tap(btn::Delete);
    CHECK_EQ(a.instruments[0].env[0].attackMs, Instrument().env[0].attackMs);
    CHECK_EQ(a.instruments[0].env[0].releaseMs, 2000);
}



// SD Card list Preview streams the file (user report 2.0.5: 30 s / 60 s files
// were silent): sound starts after the first chunk while the rest decodes,
// and a file longer than the free sample memory plays the part that fits.
namespace {
void putWavCh(Harness& h, const char* path, const std::vector<int16_t>& s, uint16_t ch) {
    std::vector<uint8_t> out(s.size() * 2 + 64);
    out.resize(wavEncode(s.data(), uint32_t(s.size() / ch), ch, out.data(), out.size()));
    h.fs.files[path] = out;
}
int selectSd(Harness& h, const char* name) {
    LoaderScreen& l = ld(h);
    int idx = -1;
    for (int i = 0; i < l.count; ++i)
        if (!std::strcmp(l.entries[i], name)) idx = i;
    while (idx >= 0 && l.sdCursor < idx) h.tap(btn::Down);
    return idx;
}
}  // namespace

TEST(loader_sd_preview_streams_long_files) {
    for (int k = 0; k < 3; ++k) {
        const bool fits = k != 1;
        const uint16_t ch = k == 2 ? 2 : 1;
        Harness h(true, 300000);   // 300000 samples of sample memory
        App& a = *h.app;
        std::vector<int16_t> s(fits ? 250000 : 600000);
        for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t((i % 100) * 200 - 10000);
        putWavCh(h, "long.wav", s, ch);
        const uint32_t frames = uint32_t(s.size() / ch);
        h.tap(btn::SampleLoader);
        CHECK(selectSd(h, "long.wav") >= 0);
        h.down(btn::F3);
        CHECK(a.engine.previewBufferActive());
        const uint32_t first = a.engine.previewBufferAvail();
        CHECK(first > 0 && first < frames);   // playing before the decode is done
        CHECK(h.audio.pump(4) > 1000);        // sounds at once
        h.run(200);                           // decodes ahead while it plays
        CHECK(a.engine.previewBufferAvail() > first);
        CHECK(h.audio.pump(40) > 1000);
        CHECK(a.engine.previewBufferActive());
        h.up(btn::F3);
        CHECK(!a.engine.previewBufferActive());
        CHECK(a.bank.freeSamples() == 300000u);   // the preview never claims the memory
    }
}

// Replacing an instrument while a preview streams moves the instruments
// above it into the old free area: the stream stops before it could write
// over them (Fable review).
TEST(loader_sd_preview_stops_when_the_bank_grows) {
    Harness h(true, 3000000);
    App& a = *h.app;
    std::vector<int16_t> big(2500000, 7000), a0(20000), b1(20000), c0(400000);
    for (size_t i = 0; i < a0.size(); ++i) a0[i] = int16_t(100 + i % 7);
    for (size_t i = 0; i < b1.size(); ++i) b1[i] = int16_t(-300 - int(i % 11));
    for (size_t i = 0; i < c0.size(); ++i) c0[i] = int16_t(50 + i % 13);
    putWav(h, "big.wav", big);
    putWav(h, "a0.wav", a0);
    putWav(h, "b1.wav", b1);
    putWav(h, "c0.wav", c0);
    CHECK(a.importSample("a0.wav", 0));
    CHECK(a.importSample("b1.wav", 1));
    h.tap(btn::SampleLoader);
    CHECK(selectSd(h, "big.wav") >= 0);
    h.down(btn::F3);
    CHECK(a.engine.previewBufferActive());
    CHECK(a.engine.previewBufferAvail() < big.size());   // still streaming
    CHECK(a.importSample("c0.wav", 0));   // slot 1 moves up into the old free area
    h.run(300);
    const int16_t* d = a.bank.data(1);
    bool same = d != nullptr && a.bank.frames(1) == b1.size();
    for (size_t i = 0; same && i < b1.size(); ++i) same = d[i] == b1[i];
    CHECK(same);
    h.up(btn::F3);
}

// .pti files on the SD Card list preview too (user report): the payload
// streams like a WAV; planar stereo keeps both halves.
TEST(loader_sd_preview_plays_pti) {
    for (const bool stereo : {false, true}) {
        Harness h(true, 1000000);
        App& a = *h.app;
        const uint32_t n = 200000;
        std::vector<int16_t> l(n), r(n);
        for (uint32_t i = 0; i < n; ++i) {
            l[i] = int16_t((i % 100) * 200 - 10000);
            r[i] = int16_t(-(int(i % 70) * 250) + 8000);
        }
        Instrument ins;
        ins.lengthFrames = n;
        ins.stereo = stereo;
        std::vector<uint8_t> f(ptiFileSize(n, stereo));
        f.resize(encodePti(ins, l.data(), stereo ? r.data() : nullptr, f.data(), f.size()));
        CHECK(!f.empty());
        h.fs.files["inst.pti"] = f;
        h.tap(btn::SampleLoader);
        CHECK(selectSd(h, "inst.pti") >= 0);
        const int16_t* tail = a.bank.tail();
        h.down(btn::F3);
        CHECK(a.engine.previewBufferActive());
        CHECK(a.engine.previewBufferAvail() > 0 && a.engine.previewBufferAvail() < n);   // streaming
        CHECK(h.audio.pump(4) > 1000);
        h.run(300);
        CHECK(a.engine.previewBufferAvail() == n);
        bool same = true;
        for (uint32_t i = 0; same && i < n; ++i) same = tail[i] == l[i] && (!stereo || tail[n + i] == r[i]);
        CHECK(same);
        h.up(btn::F3);
        CHECK(!a.engine.previewBufferActive());
    }
}

// User report: with Preview held, Down to the next SD file must audition it
// (the instrument list already did; the SD list kept the old file / went quiet).
TEST(loader_sd_preview_follows_the_cursor) {
    Harness h;
    App& a = *h.app;
    std::vector<int16_t> quiet(44100, 40), loud(44100);
    for (size_t i = 0; i < loud.size(); ++i) loud[i] = int16_t((i / 50) % 2 ? 12000 : -12000);
    putWavCh(h, "a_quiet.wav", quiet, 1);
    putWavCh(h, "b_loud.wav", loud, 1);
    h.tap(btn::SampleLoader);
    CHECK(selectSd(h, "a_quiet.wav") >= 0);
    h.down(btn::F3);
    CHECK(a.engine.previewBufferActive());
    CHECK(h.audio.pump(4) < 1000);   // the quiet file
    h.tap(btn::Down);                // to b_loud.wav, Preview still held
    h.run(50);
    CHECK(a.engine.previewBufferActive());
    CHECK(h.audio.pump(8) > 5000);   // the loud file plays now
    h.up(btn::F3);
    CHECK(!a.engine.previewBufferActive());
}

// User report (stock behaviour per the Polyend forum): loading a sample into
// an existing instrument keeps its settings; only the audio and name change.
TEST(import_into_existing_instrument_keeps_its_settings) {
    Harness h;
    App& a = *h.app;
    std::vector<int16_t> s1(20000, 1000), s2(30000, -1000);
    putWavCh(h, "chord_a.wav", s1, 1);
    putWavCh(h, "chord_b.wav", s2, 1);
    CHECK(a.importSample("chord_a.wav", 2));
    Instrument& in = a.instruments[2];
    in.volume = 33;
    in.tune = 7;
    in.filterEnabled = 1;
    in.cutoff = 0.4f;
    in.reverbSend = 55;
    in.playMode = uint8_t(PlayMode::ForwardLoop);
    in.env[0].releaseMs = 1234;
    CHECK(a.importSample("chord_b.wav", 2));
    CHECK_STR(a.instruments[2].name, "chord_b");
    CHECK_EQ(a.instruments[2].lengthFrames, 30000u);
    CHECK_EQ(int(a.instruments[2].volume), 33);
    CHECK_EQ(int(a.instruments[2].tune), 7);
    CHECK_EQ(int(a.instruments[2].filterEnabled), 1);
    CHECK_EQ(int(a.instruments[2].reverbSend), 55);
    CHECK_EQ(int(a.instruments[2].playMode), int(PlayMode::ForwardLoop));
    CHECK_EQ(int(a.instruments[2].env[0].releaseMs), 1234);
    // An empty slot still starts from the defaults.
    CHECK(a.importSample("chord_a.wav", 5));
    CHECK_EQ(int(a.instruments[5].volume), 50);
}

TEST(playback_wavetable_shows_one_window_and_its_position) {
    // MANUAL_AUDIT_2026-10-23 #12 (manual p.130-131, spec 05 appB §1.4d:
    // a single-window graph and the position on a bar under it).
    Harness h(true, 400000);
    App& a = *h.app;
    std::vector<int16_t> s(2048 * 4);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(8000 * std::sin(double(i) * 0.05));
    std::vector<uint8_t> wav(s.size() * 2 + 64);
    wav.resize(wavEncode(s.data(), uint32_t(s.size()), 1, wav.data(), wav.size()));
    h.fs.files["wt.wav"] = wav;
    CHECK(a.importSample("wt.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::SamplePlayback);
    PlaybackScreen& p = *static_cast<PlaybackScreen*>(a.ui.screen(kModPlayback));
    Instrument& in = a.instruments[0];
    in.playMode = uint8_t(PlayMode::Wavetable);
    in.sampleType = 1;
    in.wavetableWindowSize = 2048;
    in.wavetableWindowCount = 4;
    in.wavetableCurrentWindow = 2;
    a.ui.dirty();
    h.run(40);
    CHECK_EQ(p.wavetableShownWindow(), 2u);
    // The marker of window 2 of 4 sits at 2/3 of the position bar.
    const int mx = p.wavetablePositionX();
    CHECK(mx > 0);
    bool found = false;
    for (const std::string& l : h.disp.last) {
        int x, y;
        if (std::sscanf(l.c_str(), "VERTEX %d %d", &x, &y) == 2 && std::abs(x - mx) <= 5 && y > 300 && y < 340) found = true;
    }
    CHECK(found);
}

TEST(playback_granular_shows_the_grain_span) {
    // MANUAL_AUDIT_2026-10-23 #28 (manual p.135-136: the grain as a blue
    // span around the blue position line, the play head red).
    Harness h(true, 400000);
    App& a = *h.app;
    std::vector<int16_t> s(44100);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(8000 * std::sin(double(i) * 0.05));
    std::vector<uint8_t> wav(s.size() * 2 + 64);
    wav.resize(wavEncode(s.data(), uint32_t(s.size()), 1, wav.data(), wav.size()));
    h.fs.files["g.wav"] = wav;
    CHECK(a.importSample("g.wav", 0));
    a.setCurrentInstrument(0);
    Instrument& in = a.instruments[0];
    in.playMode = uint8_t(PlayMode::Granular);
    in.granularPosition = 32768;
    in.grainLength = 4410;   // 100 ms of a 1 s sample: ~60 px wide
    h.tap(btn::SamplePlayback);
    a.ui.dirty();
    h.run(40);
    char want[16];
    std::snprintf(want, sizeof(want), "COLOR %06X", unsigned(PlaybackScreen::kGrainColour & 0xFFFFFF));
    bool blue = false;
    for (const std::string& l : h.disp.last) blue = blue || l == want;
    CHECK(blue);
}
