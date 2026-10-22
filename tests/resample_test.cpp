// [I] Live resampling (app/resample.h): arming, bar sync, fixed / manual
// length, sources, the new instrument, placement on a track, the reserved
// sample memory and cancelling.
#include <cmath>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {

void addSaw(Harness& h, const char* path, uint32_t frames) {
    std::vector<int16_t> s(frames);
    for (uint32_t i = 0; i < frames; ++i) s[i] = int16_t((i % 100) * 300 - 15000);
    std::vector<uint8_t> out(frames * 2 + 64);
    out.resize(wavEncode(s.data(), frames, 1, out.data(), out.size()));
    h.fs.files[path] = out;
}

// A sample on track 1 every step; tempo 120 (one 16-step bar = 2 s).
void setup(Harness& h) {
    App& a = *h.app;
    addSaw(h, "saw.wav", 3000);
    CHECK(a.importSample("saw.wav", 0));
    for (int s = 0; s < 64; ++s) {
        Step& st = a.pattern().tracks[0].steps[s];
        st.note = 60;
        st.instrument = 0;
    }
    a.pattern().tracks[0].lastStep = 63;
    a.patternEdited();
    a.project.setTempo(120.0f);
}

constexpr double kBlockS = 128.0 / 44122.6484;
int blocksFor(double seconds) { return int(seconds / kBlockS + 0.5); }
constexpr uint32_t kBarFrames = 88245;   // 2 s at the live rate 44122.65 Hz

bool near(uint32_t a, uint32_t b, uint32_t tol) { return a > b ? a - b <= tol : b - a <= tol; }

int peakOf(App& a, int slot) {
    const int16_t* l = a.bank.left(slot);
    int pk = 0;
    for (uint32_t i = 0; l && i < a.bank.frames(slot); ++i) pk = std::abs(l[i]) > pk ? std::abs(l[i]) : pk;
    return pk;
}

}  // namespace

TEST(resample_fixed_bars_master) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.resampleCfg.length = 1;   // 1 bar
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    CHECK(a.resampler.state() == Resampler::kArmed);
    // All free memory is held back while it runs.
    CHECK(a.bank.reserved() > 0);
    CHECK_EQ(a.bank.freeSamples(), 0u);
    h.audio.pump(50);   // stopped: stays armed
    CHECK(a.resampler.state() == Resampler::kArmed);
    a.seq.play(0);
    h.audio.pump(10);
    CHECK(a.resampler.state() == Resampler::kRecording);   // play start = bar 1
    h.audio.pump(blocksFor(2.2));
    CHECK(a.resampler.state() == Resampler::kDone);
    h.run(5);
    CHECK(!a.resampler.busy());
    CHECK_EQ(a.bank.reserved(), 0u);
    CHECK(a.instrPresent[1]);
    CHECK(a.bank.stereo(1));
    CHECK(near(a.bank.frames(1), kBarFrames, 128));
    CHECK(peakOf(a, 1) > 1000);
    CHECK_STR(a.instruments[1].name, "Resample Master");
    CHECK_EQ(a.currentInstrument(), 1);
    // The sequencer kept playing throughout.
    CHECK(a.seq.isPlaying());
}

TEST(resample_starts_on_the_next_bar_and_manual_stop_snaps) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.resampleCfg.length = 0;   // Manual
    a.seq.play(0);
    h.audio.pump(blocksFor(0.5));   // a quarter into bar 1
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    h.audio.pump(blocksFor(1.0));
    CHECK(a.resampler.state() == Resampler::kArmed);   // waits for bar 2
    h.audio.pump(blocksFor(0.6));
    CHECK(a.resampler.state() == Resampler::kRecording);
    h.audio.pump(blocksFor(2.5));   // into the middle of the second recorded bar
    CHECK(a.resampleToggle() == App::ResampleAction::kStopping);
    CHECK(a.resampler.state() == Resampler::kStopping);
    h.audio.pump(blocksFor(1.2));   // 5.8 s: the next bar (6.0 s) has not begun
    CHECK(a.resampler.state() == Resampler::kStopping);
    h.audio.pump(blocksFor(0.4));
    CHECK(a.resampler.state() == Resampler::kDone);
    CHECK_EQ(a.resampler.barsDone(), 2u);
    h.run(5);
    CHECK(near(a.bank.frames(1), 2 * kBarFrames, 256));
}

TEST(resample_places_on_track_with_undo) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.resampleCfg.length = 1;   // 1 bar = 16 steps
    a.resampleCfg.place = 3;    // track 3
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    a.seq.play(0);
    h.audio.pump(blocksFor(2.3));
    h.run(5);
    const Track& t = a.pattern().tracks[2];
    for (int s = 0; s < 64; ++s) {
        const bool on = s % 16 == 0;
        CHECK_EQ(t.steps[s].note, on ? 60 : kNoteEmpty);
        if (on) CHECK_EQ(t.steps[s].instrument, 1);
    }
    a.seq.stop();
    a.undoEdit();
    CHECK_EQ(a.pattern().tracks[2].steps[0].note, kNoteEmpty);
    // Longer than the pattern: placed once.
    a.pattern().tracks[0].lastStep = 7;
    a.patternEdited();
    a.resampleCfg.length = 1;
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    a.seq.play(0);
    h.audio.pump(blocksFor(2.3));
    h.run(5);
    CHECK_EQ(a.pattern().tracks[2].steps[0].note, 60);
    CHECK_EQ(a.pattern().tracks[2].steps[0].instrument, 2);
}

TEST(resample_track_and_line_sources) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.resampleCfg.length = 1;
    a.resampleCfg.source = 2;   // track 2: silent
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    a.seq.play(0);
    h.audio.pump(blocksFor(2.3));
    h.run(5);
    CHECK(a.instrPresent[1]);
    CHECK_EQ(peakOf(a, 1), 0);
    CHECK_STR(a.instruments[1].name, "Resample Track 2");
    a.resampleCfg.source = 1;   // track 1: the saw
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    h.audio.pump(blocksFor(4.3));
    h.run(5);
    CHECK(a.instrPresent[2]);
    CHECK(peakOf(a, 2) > 1000);
    a.resampleCfg.source = audio::AudioEngine::kTapLineIn;   // no line input here: silence
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    h.audio.pump(blocksFor(4.3));
    h.run(5);
    CHECK(a.instrPresent[3]);
    CHECK_EQ(peakOf(a, 3), 0);
    CHECK_STR(a.instruments[3].name, "Resample Line in");
}

// A synth on a track (2.x: Synth 1..3 play on shared slots, not the
// track's voice): a Track take records the synth the track plays, dry; a
// take of another track does not pick it up.
TEST(resample_track_source_synth) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.setSynthModel(0, int(SynthModel::ACD));
    for (int s = 0; s < 64; s += 4) {
        Step& st = a.pattern().tracks[2].steps[s];
        st.note = 48;
        st.instrument = uint8_t(kFirstSynthInstrument);   // Synth 1
    }
    a.pattern().tracks[2].lastStep = 63;
    a.patternEdited();
    a.resampleCfg.length = 1;
    a.resampleCfg.source = 3;   // track 3: the synth
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    a.seq.play(0);
    h.audio.pump(blocksFor(2.3));
    h.run(5);
    CHECK(a.instrPresent[1]);
    CHECK(peakOf(a, 1) > 1000);
    CHECK_STR(a.instruments[1].name, "Resample Track 3");
    a.resampleCfg.source = 2;   // track 2: silent, Synth 1 still sounding on track 3
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    h.audio.pump(blocksFor(4.3));
    h.run(5);
    CHECK(a.instrPresent[2]);
    CHECK_EQ(peakOf(a, 2), 0);
}

TEST(resample_slot_memory_cancel_and_transport) {
    Harness h;
    App& a = *h.app;
    setup(h);
    // The slot goes above the highest loaded instrument (nothing moves).
    addSaw(h, "b.wav", 500);
    CHECK(a.importSample("b.wav", 10));
    CHECK_EQ(a.nextResampleSlot(), 11);
    // Cancel while armed: memory released.
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    CHECK(a.resampleToggle() == App::ResampleAction::kCancelled);
    CHECK(!a.resampler.busy());
    CHECK_EQ(a.bank.reserved(), 0u);
    CHECK(a.bank.freeSamples() > 0u);
    // Loads during a resample cannot use the held memory.
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    CHECK(!a.importSample("b.wav", 20));
    // Stopping the transport ends it with what was recorded.
    a.seq.play(0);
    h.audio.pump(blocksFor(0.7));
    a.seq.stop();
    h.audio.pump(2);
    CHECK(a.resampler.state() == Resampler::kDone);
    CHECK(a.resampler.end() == Resampler::kEndTransport);
    h.run(5);
    CHECK(a.instrPresent[11]);
    CHECK(near(a.bank.frames(11), uint32_t(0.7 * 44122.65), 512));
    // Last slot taken: refused.
    CHECK(a.importSample("b.wav", 47));
    CHECK(a.resampleToggle() == App::ResampleAction::kNoSlot);
    CHECK(!a.resampler.busy());
    // A new project while armed: cancelled, memory back.
    CHECK(a.deleteInstrument(47));
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    CHECK(a.newProject());
    h.run(5);
    CHECK(!a.resampler.busy());
    CHECK_EQ(a.bank.reserved(), 0u);
}

TEST(resample_key_from_any_screen) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.resampleCfg.length = 1;
    h.down(btn::Shift);
    h.tap(btn::SampleRecorder);
    h.up(btn::Shift);
    CHECK(a.resampler.state() == Resampler::kArmed);
    CHECK(a.ui.active() != kModRecorder);   // no screen switch
    h.run(300);
    CHECK(h.disp.lastHasText("Resample armed: starts with Play"));
    h.down(btn::Shift);
    h.tap(btn::SampleRecorder);
    h.up(btn::Shift);
    CHECK(!a.resampler.busy());
    // Without Shift the key still opens the Sample Recorder.
    h.tap(btn::SampleRecorder);
    CHECK(a.ui.active() == kModRecorder);
}

TEST(resample_screen) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.seq.play(0);
    // Entering the Sample Recorder while playing: "Resample" opens the
    // Resample screen and keeps the pattern running.
    h.tap(btn::SampleRecorder);
    CHECK(a.ui.askOpen());
    h.tap(btn::F6);
    CHECK(a.ui.active() == kModResample);
    CHECK(a.seq.isPlaying());
    ResampleScreen& r = *static_cast<ResampleScreen*>(a.ui.screen(kModResample));
    CHECK_EQ(r.sel, 0);
    // Settings: F2 Length (tap again: next), F5 Place on, encoder.
    a.resampleCfg = ResampleSettings{};
    h.tap(btn::F2);
    h.tap(btn::F2);
    CHECK_EQ(a.resampleCfg.length, 4);   // 4 -> 8 bars
    h.enc(-3);
    CHECK_EQ(a.resampleCfg.length, 1);   // 1 bar
    h.tap(btn::F5);
    h.enc(2);
    CHECK_EQ(a.resampleCfg.place, 2);
    // Gain / Monitor only for Line in: F6 does nothing for Master.
    h.tap(btn::F6);
    CHECK_EQ(r.sel, 4);
    h.tap(btn::F1);
    h.enc(9);
    CHECK_EQ(a.resampleCfg.source, 9);   // Line in
    CHECK(a.capture.metering());         // meter + monitor the input
    h.tap(btn::F6);
    CHECK_EQ(r.sel, 5);
    const uint8_t g0 = a.cfg.recGain[0];
    h.tap(btn::Up);
    CHECK_EQ(a.cfg.recGain[0], g0 + 1);
    h.tap(btn::Right);
    CHECK_EQ(r.sel, 6);
    h.tap(btn::Down);   // Off (below On)
    CHECK_EQ(a.cfg.recMonitor, 0);
    CHECK(a.inputMonitorMuted);
    h.tap(btn::F7);     // again: toggle
    CHECK_EQ(a.cfg.recMonitor, 1);
    CHECK(!a.inputMonitorMuted);
    // Left / Right step through the columns; lists: Up = the item above.
    h.tap(btn::Left);
    h.tap(btn::Left);
    h.tap(btn::Left);
    h.tap(btn::Left);
    h.tap(btn::Left);
    h.tap(btn::Left);
    CHECK_EQ(r.sel, 0);
    h.tap(btn::Up);
    CHECK_EQ(a.resampleCfg.source, 8);   // Track 8: Gain / Monitor go away
    CHECK(!a.capture.metering());
    h.tap(btn::Up);
    h.tap(btn::Up);
    h.tap(btn::Up);
    h.tap(btn::Up);
    h.tap(btn::Up);
    h.tap(btn::Up);
    h.tap(btn::Up);
    h.tap(btn::Up);
    CHECK_EQ(a.resampleCfg.source, 0);   // Master
    h.tap(btn::Right);
    h.tap(btn::Down);
    CHECK_EQ(a.resampleCfg.length, 2);   // 2 bars
    h.tap(btn::Up);
    h.tap(btn::Right);
    CHECK_EQ(r.sel, 2);   // Slices
    h.tap(btn::Right);   // Into
    h.tap(btn::Right);
    h.tap(btn::Right);   // stays on Place on (Gain / Monitor need Line in)
    CHECK_EQ(r.sel, 4);
    h.tap(btn::Up);
    CHECK_EQ(a.resampleCfg.place, 1);
    h.tap(btn::Down);
    // Record arms, records the next bar, the result lands in slot 2.
    h.tap(btn::Rec);
    CHECK(a.resampler.state() == Resampler::kArmed);
    h.audio.pump(blocksFor(4.5));
    h.run(30);
    CHECK(a.instrPresent[1]);
    CHECK_EQ(a.pattern().tracks[1].steps[0].instrument, 1);
    CHECK(a.seq.isPlaying());
    // Play stops and starts the pattern here.
    h.tap(btn::Play);
    CHECK(!a.seq.isPlaying());
    h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    // Sample Rec goes to the Sample Recorder (the stock prompt while playing).
    h.tap(btn::SampleRecorder);
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);
    CHECK(a.ui.active() == kModRecorder);
    CHECK(!a.seq.isPlaying());
    CHECK(!a.inputMonitorMuted);
}

// #9: the Sample Recorder's Source list routes the codec to a microphone
// (Mic LG / Mic HG); the Resample screen must route Line in again.
TEST(resample_screen_routes_line_in) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.cfg.recSource = 0;
    h.tap(btn::SampleRecorder);
    CHECK(h.routedInput == 0);
    h.enc(3);   // Radio, Mic LG, Mic HG
    CHECK(h.routedInput == 3);
    h.tap(btn::Pattern);
    a.seq.play(0);
    h.tap(btn::SampleRecorder);
    h.tap(btn::F6);   // Resample
    CHECK(a.ui.active() == kModResample);
    CHECK(h.routedInput == 0);
}

// Line in on the Resample screen: the input is heard (monitor) and recorded
// while the pattern plays.
TEST(resample_line_in_monitor_and_take) {
    Harness h;
    App& a = *h.app;
    setup(h);
    for (int s = 0; s < 64; ++s) a.pattern().tracks[0].steps[s].note = -1;   // the saw off
    a.patternEdited();
    h.lineGen = [](int16_t* b, uint64_t blk) {
        for (int i = 0; i < 128; ++i) {
            const int16_t v = int16_t(6000.0f * std::sin(2.0f * 3.14159265f * 1000.0f * float(blk * 128 + uint64_t(i)) / 44100.0f));
            b[2 * i] = v;
            b[2 * i + 1] = v;
        }
    };
    a.seq.play(0);
    h.tap(btn::SampleRecorder);
    CHECK(a.ui.askOpen());
    h.tap(btn::F6);   // Resample
    a.resampleCfg = ResampleSettings{};
    a.resampleCfg.length = 1;
    h.enc(9);         // From: Line in
    CHECK_EQ(a.resampleCfg.source, audio::AudioEngine::kTapLineIn);
    h.run(10);
    h.audio.pump(10);
    const int before = h.audio.pump(40);
    CHECK(before > 1000);
    h.tap(btn::Rec);
    CHECK(a.resampler.state() == Resampler::kArmed);
    int during = 0;
    for (int i = 0; i < 20; ++i) {
        const int p = h.audio.pump(blocksFor(0.25));
        h.run(1);
        if (a.resampler.state() == Resampler::kRecording) during = p;
    }
    CHECK(during > 1000);
    h.run(10);
    const int slot = 1;
    CHECK(a.instrPresent[slot]);
    CHECK(peakOf(a, slot) > 1000);
}

// #7: a take holds all free sample memory; adding a sample in the Sample
// Loader meanwhile is refused with a notice that says why.
TEST(resample_loader_add_refused_with_notice) {
    Harness h;
    App& a = *h.app;
    setup(h);
    addSaw(h, "kick.wav", 4410);
    a.resampleCfg.length = 1;
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    h.tap(btn::SampleLoader);
    LoaderScreen& l = *static_cast<LoaderScreen*>(a.ui.screen(kModLoader));
    int idx = -1;
    for (int i = 0; i < l.count; ++i)
        if (!std::strcmp(l.entries[i], "kick.wav")) idx = i;
    CHECK(idx >= 0);
    while (l.sdCursor < idx) h.tap(btn::Down);
    l.insCursor = 5;   // an empty slot
    h.tap(btn::F1);
    h.run(20);
    CHECK(!a.instrPresent[5]);
    CHECK(h.disp.lastHasText("reserved by the resample take"));
    CHECK(a.resampler.state() == Resampler::kArmed);   // the take is untouched
    // Cancelled: the memory is free again and the same add loads.
    CHECK(a.resampleToggle() == App::ResampleAction::kCancelled);
    h.tap(btn::F1);
    h.run(20);
    CHECK(a.instrPresent[5]);
    CHECK_EQ(a.bank.frames(5), 4410u);
}

// Sliced takes: the slice grid per setting, length and time signature.
TEST(resample_slice_grid) {
    Harness h;
    App& a = *h.app;
    int n = -1;
    a.cfg.tsNum = 3;   // 4/4, 4 steps per beat: 16 steps per bar
    a.cfg.tsDen = 3;
    CHECK_EQ(a.resampleSliceGrid(1, n), 0);   // Slices Off
    CHECK_EQ(n, 0);
    a.resampleCfg.slices = 1;                // per beat
    CHECK_EQ(a.resampleSliceGrid(1, n), 4);
    CHECK_EQ(n, 4);
    CHECK_EQ(a.resampleSliceGrid(8, n), 4);
    CHECK_EQ(n, 32);
    CHECK_EQ(a.resampleSliceGrid(16, n), 8);   // 64 beats: 2 beats per slice
    CHECK_EQ(n, 32);
    CHECK_EQ(a.resampleSliceGrid(0, n), 0);    // no whole bars: not sliced
    CHECK_EQ(n, 0);
    a.resampleCfg.slices = 2;                // per step
    CHECK_EQ(a.resampleSliceGrid(1, n), 1);
    CHECK_EQ(n, 16);
    CHECK_EQ(a.resampleSliceGrid(2, n), 1);
    CHECK_EQ(n, 32);
    CHECK_EQ(a.resampleSliceGrid(4, n), 2);    // 64 steps: 2 per slice
    CHECK_EQ(n, 32);
    CHECK_EQ(a.resampleSliceGrid(13, n), 8);   // Manual, 208 steps
    CHECK_EQ(n, 26);
    a.cfg.tsNum = 6;   // 7 beats per bar
    a.resampleCfg.slices = 1;
    CHECK_EQ(a.resampleSliceGrid(13, n), 8);   // 91 beats: 2 per slice, 45 whole
    CHECK_EQ(n, 45);
}

// A 1-bar take sliced per step: Beat Slice, 16 equal slices, placed one per
// step (note 24 + i), repeating with the take; the slices hold the audio.
TEST(resample_sliced_take_placed_per_step) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.cfg.tsNum = 3;
    a.cfg.tsDen = 3;
    a.resampleCfg.length = 1;   // 1 bar
    a.resampleCfg.slices = 2;   // per step
    a.resampleCfg.place = 2;    // track 2
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    a.seq.play(0);
    h.audio.pump(blocksFor(2.3));
    h.run(5);
    CHECK(a.instrPresent[1]);
    const Instrument& ins = a.instruments[1];
    CHECK_EQ(ins.playMode, uint8_t(PlayMode::BeatSlice));
    CHECK_EQ(ins.sliceCount, 16);
    for (int i = 0; i < 16; ++i) CHECK(near(ins.slices[i], uint32_t(i * 65535 / 16), 1));
    CHECK(h.disp.lastHasText("16 slices"));
    const Pattern& p = a.pattern();
    for (int s = 0; s < 64; ++s) {
        CHECK_EQ(p.tracks[1].steps[s].note, 24 + s % 16);
        CHECK_EQ(p.tracks[1].steps[s].instrument, 1);
    }
    // Every slice has sound (the saw plays on every step).
    const uint32_t len = a.bank.frames(1);
    const int16_t* l = a.bank.left(1);
    for (int i = 0; i < 16; ++i) {
        int pk = 0;
        for (uint32_t f = len * uint32_t(i) / 16; f < len * uint32_t(i + 1) / 16; ++f) pk = std::abs(int(l[f])) > pk ? std::abs(int(l[f])) : pk;
        CHECK(pk > 1000);
    }
}

// Per beat over 2 bars on a 64-step pattern: 8 slices, one every 4 steps,
// the gaps untouched; Slices with Place on Off only slice.
TEST(resample_sliced_take_per_beat) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.cfg.tsNum = 3;
    a.cfg.tsDen = 3;
    a.resampleCfg.length = 2;   // 2 bars
    a.resampleCfg.slices = 1;   // per beat
    a.resampleCfg.place = 3;    // track 3
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    a.seq.play(0);
    h.audio.pump(blocksFor(4.3));
    h.run(5);
    CHECK(a.instrPresent[1]);
    CHECK_EQ(a.instruments[1].sliceCount, 8);
    const Pattern& p = a.pattern();
    for (int s = 0; s < 64; ++s) {
        const int8_t want = s % 4 == 0 ? int8_t(24 + (s % 32) / 4) : kNoteEmpty;
        CHECK_EQ(p.tracks[2].steps[s].note, want);
    }
    // Unsliced placement is unchanged: the whole take every 2 bars.
    a.resampleCfg.slices = 0;
    a.resampleCfg.place = 4;
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    h.audio.pump(blocksFor(8.5));
    h.run(5);
    CHECK(a.instrPresent[2]);
    CHECK_EQ(a.instruments[2].playMode, uint8_t(PlayMode::OneShot));
    CHECK_EQ(a.instruments[2].sliceCount, 0);
    for (int s = 0; s < 64; ++s) CHECK_EQ(p.tracks[3].steps[s].note, s % 32 == 0 ? int8_t(60) : kNoteEmpty);
}

namespace {
// A decaying burst (sharp attack) of `amp` at frame `at`, `len` frames long.
void burst(std::vector<int16_t>& x, uint32_t at, uint32_t len, float amp, float hz) {
    for (uint32_t i = 0; i < len && at + i < x.size(); ++i) {
        const float env = std::exp(-float(i) / float(len / 4 + 1));
        const float v = amp * env * std::sin(2.0f * 3.14159265f * hz * float(i) / 44100.0f + 1.0f);
        x[at + i] = int16_t(std::fmax(-32767.0f, std::fmin(32767.0f, float(x[at + i]) + v)));
    }
}
}  // namespace

// findHitCells: hits at the starts of cells, a hat over a ringing kick tail,
// nothing in silence or in a steady tone.
TEST(resample_find_hit_cells) {
    constexpr int kCells = 16;
    constexpr uint32_t kCell = 5515, kN = kCell * kCells;
    std::vector<int16_t> x(kN, 0);
    burst(x, 0, 20000, 20000.0f, 55.0f);          // kick ringing over cells 0..3
    burst(x, 4 * kCell, 3000, 12000.0f, 180.0f);  // snare
    burst(x, 6 * kCell, 800, 3000.0f, 7000.0f);   // hat
    burst(x, 7 * kCell, 30000, 20000.0f, 55.0f);  // kick ringing on...
    burst(x, 9 * kCell, 800, 3000.0f, 7000.0f);   // ...with a hat over its tail
    uint16_t out[48];
    const int n = findHitCells(x.data(), nullptr, kN, kCells, out, 48);
    CHECK_EQ(n, 5);
    const uint16_t want[5] = {0, 4, 6, 7, 9};
    for (int i = 0; i < 5 && i < n; ++i) CHECK_EQ(out[i], want[i]);
    // Silence: cell 0 only. A steady tone (a pad): cell 0 only.
    std::vector<int16_t> z(kN, 0);
    CHECK_EQ(findHitCells(z.data(), z.data(), kN, kCells, out, 48), 1);
    for (uint32_t i = 0; i < kN; ++i) z[i] = int16_t(8000.0f * std::sin(2.0f * 3.14159265f * 220.0f * float(i) / 44100.0f));
    CHECK_EQ(findHitCells(z.data(), z.data(), kN, kCells, out, 48), 1);
    // More hits than room: the count is still reported, `max` are written.
    std::vector<int16_t> y(kN, 0);
    for (int k = 0; k < kCells; ++k) burst(y, uint32_t(k) * kCell, 800, 6000.0f, 3000.0f);
    CHECK_EQ(findHitCells(y.data(), nullptr, kN, kCells, out, 4), 16);
}

// Step hits on a beat with empty steps: slices only where a hit starts, the
// empty steps stay empty, each slice runs to the next hit.
TEST(resample_sliced_take_step_hits) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.cfg.tsNum = 3;
    a.cfg.tsDen = 3;
    for (int s = 0; s < 64; ++s) {
        const int k = s % 16;
        a.pattern().tracks[0].steps[s].note = (k == 0 || k == 4 || k == 6 || k == 10) ? 60 : kNoteEmpty;
    }
    a.patternEdited();
    a.resampleCfg.length = 1;   // 1 bar
    a.resampleCfg.slices = 4;   // step hits
    a.resampleCfg.place = 2;
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    a.seq.play(0);
    h.audio.pump(blocksFor(2.3));
    h.run(5);
    CHECK(a.instrPresent[1]);
    const Instrument& ins = a.instruments[1];
    CHECK_EQ(ins.playMode, uint8_t(PlayMode::BeatSlice));
    CHECK_EQ(ins.sliceCount, 4);
    const int at[4] = {0, 4, 6, 10};
    for (int i = 0; i < 4; ++i) CHECK(near(ins.slices[i], uint32_t(at[i] * 65535 / 16), 1));
    const Pattern& p = a.pattern();
    for (int s = 0; s < 64; ++s) {
        const int k = s % 16;
        int8_t want = kNoteEmpty;
        for (int i = 0; i < 4; ++i)
            if (k == at[i]) want = int8_t(24 + i);
        CHECK_EQ(p.tracks[1].steps[s].note, want);
    }
    CHECK(h.disp.lastHasText("4 slices"));
}

// Step hits with a hit on every step over 4 bars (64 > 48): the grid doubles
// and every other step starts a slice.
TEST(resample_sliced_take_hits_coarsen) {
    Harness h;
    App& a = *h.app;
    setup(h);   // the saw on every step
    a.cfg.tsNum = 3;
    a.cfg.tsDen = 3;
    a.resampleCfg.length = 3;   // 4 bars
    a.resampleCfg.slices = 4;
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    a.seq.play(0);
    h.audio.pump(blocksFor(8.3));
    h.run(5);
    CHECK(a.instrPresent[1]);
    CHECK_EQ(a.instruments[1].sliceCount, 32);
    for (int i = 0; i < 32; ++i) CHECK(near(a.instruments[1].slices[i], uint32_t(i * 2 * 65535 / 64), 1));
}

// A hit 20 ms late (micro timing, Line in latency) still starts its cell.
TEST(resample_find_hit_cells_late) {
    constexpr int kCells = 16;
    constexpr uint32_t kCell = 5515, kN = kCell * kCells;
    std::vector<int16_t> x(kN, 0);
    burst(x, 0, 3000, 12000.0f, 180.0f);
    burst(x, 4 * kCell + 882, 3000, 12000.0f, 180.0f);   // +20 ms
    uint16_t out[48];
    CHECK_EQ(findHitCells(x.data(), nullptr, kN, kCells, out, 48), 2);
    CHECK_EQ(out[1], 4);
}

// ---------------------------------------------------------------------------
// [I] Into: Instrument N (ResampleSettings::into)

namespace {

// One take of `bars` bars (playing; armed now, recorded from the next bar).
void take(Harness& h, uint8_t lengthIdx) {
    App& a = *h.app;
    a.resampleCfg.length = lengthIdx;
    CHECK(a.resampleToggle() == App::ResampleAction::kArmed);
    if (!a.seq.isPlaying()) a.seq.play(0);
    for (int i = 0; i < 4000 && a.resampler.state() != Resampler::kDone; ++i) h.audio.pump(1);
    CHECK(a.resampler.state() == Resampler::kDone);
    h.run(5);
    CHECK(!a.resampler.busy());
}

}  // namespace

TEST(resample_into_same_length_in_place) {
    Harness h;
    App& a = *h.app;
    setup(h);
    take(h, 1);   // Into: New -> instrument 2
    CHECK(a.instrPresent[1]);
    const uint32_t frames = a.bank.frames(1);
    const int16_t* at = a.bank.left(1);
    std::snprintf(a.instruments[1].name, sizeof(a.instruments[1].name), "Loop");
    a.instruments[1].volume = 40;
    a.resampleCfg.into = 2;
    take(h, 1);   // the same bars: overwritten where it is, settings kept
    CHECK(!a.instrPresent[2]);
    CHECK_EQ(a.bank.frames(1), frames);
    CHECK(a.bank.left(1) == at);
    CHECK_STR(a.instruments[1].name, "Loop");
    CHECK_EQ(a.instruments[1].volume, 40);
    CHECK_EQ(a.instruments[1].lengthFrames, frames);
    CHECK(peakOf(a, 1) > 1000);
    CHECK(strstr(a.ui.notice(), "(replaced)") != nullptr);
    CHECK_EQ(a.lastResample.slot, 1);
    // Many repeat takes keep the slot's length (each is fitted to it).
    for (int k = 0; k < 3; ++k) take(h, 1);
    CHECK_EQ(a.bank.frames(1), frames);
    CHECK(!a.instrPresent[2]);
}

TEST(resample_into_other_length_saves_new) {
    Harness h;
    App& a = *h.app;
    setup(h);
    take(h, 1);   // instrument 2, 1 bar
    const uint32_t frames = a.bank.frames(1);
    a.resampleCfg.into = 2;
    take(h, 2);   // 2 bars: does not fit, the take is kept in a new slot
    CHECK_EQ(a.bank.frames(1), frames);
    CHECK(a.instrPresent[2]);
    CHECK(near(a.bank.frames(2), 2 * kBarFrames, 256));
    CHECK_STR(a.instruments[2].name, "Resample Master");
    CHECK_STR(a.ui.notice(), "Instr 2 is another length: saved as instrument 3");
}

TEST(resample_into_empty_slots) {
    Harness h;
    App& a = *h.app;
    setup(h);   // instrument 1 loaded
    // Empty and above every loaded slot: the take goes there.
    a.resampleCfg.into = 10;
    take(h, 1);
    CHECK(a.instrPresent[9]);
    CHECK(near(a.bank.frames(9), kBarFrames, 128));
    CHECK(strstr(a.ui.notice(), "-> instrument 10") != nullptr);
    // Empty below a loaded slot (committing there would move instrument 10):
    // a new slot instead.
    a.resampleCfg.into = 5;
    take(h, 1);
    CHECK(!a.instrPresent[4]);
    CHECK(a.instrPresent[10]);
    CHECK_STR(a.ui.notice(), "Instr 5 is another length: saved as instrument 11");
}

TEST(resample_into_asks_once) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.resampleCfg.length = 1;
    a.resampleCfg.into = 1;   // instrument 1 holds the saw (mono): asked anyway
    auto key = [&] {
        h.down(btn::Shift);
        h.tap(btn::SampleRecorder);
        h.up(btn::Shift);
    };
    key();
    CHECK(a.ui.askOpen());
    CHECK(!a.resampler.busy());
    h.tap(btn::F7);   // Cancel
    CHECK(!a.ui.askOpen());
    CHECK(!a.resampler.busy());
    key();
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);   // Yes: armed
    CHECK(a.resampler.state() == Resampler::kArmed);
    key();            // cancel the armed take
    CHECK(!a.resampler.busy());
    key();            // confirmed for this slot: no question
    CHECK(!a.ui.askOpen());
    CHECK(a.resampler.state() == Resampler::kArmed);
    key();
    // An empty slot: nothing to ask.
    a.resampleCfg.into = 20;
    key();
    CHECK(!a.ui.askOpen());
    CHECK(a.resampler.state() == Resampler::kArmed);
    key();
}

TEST(resample_screen_into) {
    Harness h;
    App& a = *h.app;
    setup(h);
    a.seq.play(0);
    h.tap(btn::SampleRecorder);
    h.tap(btn::F6);   // Resample
    CHECK(a.ui.active() == kModResample);
    ResampleScreen& r = *static_cast<ResampleScreen*>(a.ui.screen(kModResample));
    a.resampleIntoOk = 0;
    h.tap(btn::F4);
    CHECK_EQ(r.sel, 3);
    h.enc(3);
    CHECK_EQ(a.resampleCfg.into, 3);
    CHECK_EQ(a.resampleIntoOk, -1);   // Into changed: ask again
    h.tap(btn::F4);                   // again: the next item
    CHECK_EQ(a.resampleCfg.into, 4);
    h.tap(btn::Left);
    CHECK_EQ(r.sel, 2);
    h.tap(btn::Right);
    CHECK_EQ(r.sel, 3);
    h.tap(btn::Right);
    CHECK_EQ(r.sel, 4);   // Place on
    h.run(100);
    CHECK(h.disp.lastHasText("Into"));
    CHECK(h.disp.lastHasText("Instr 4"));
    CHECK(h.disp.lastHasText("1 saw"));   // the list shows slot names
    CHECK(h.disp.lastHasText("-> Instr 4"));   // empty, above every loaded slot
}

TEST(resample_into_records_the_take_with_its_fx) {
    // Take 1 placed on track 2, the source muted, delay / reverb on the take,
    // take 2 from Master into the same slot: the FX are in the new audio.
    Harness h;
    App& a = *h.app;
    setup(h);
    a.resampleCfg.place = 2;
    take(h, 1);   // instrument 2 on track 2
    const uint32_t n = a.bank.frames(1);
    std::vector<int16_t> before(a.bank.left(1), a.bank.left(1) + n);
    a.setTrackMute(0, true);
    a.instruments[1].delaySend = 100;
    a.instruments[1].reverbSend = 100;
    a.markInstrumentDirty(1);
    a.resampleCfg.place = 0;
    a.resampleCfg.into = 2;
    take(h, 1);
    CHECK_EQ(a.bank.frames(1), n);
    // Energy of the difference against the energy of the first take.
    double diff = 0.0, ref = 0.0;
    const int16_t* l = a.bank.left(1);
    for (uint32_t i = 0; i < n; ++i) {
        const double d = double(l[i]) - double(before[i]);
        diff += d * d;
        ref += double(before[i]) * double(before[i]);
    }
    std::printf("    take 2 vs take 1: difference energy %.1f %% of take 1\n", 100.0 * diff / ref);
    CHECK(diff > 0.05 * ref);
}
