// Sample Recorder (spec 05 §2.11, appendix C §2; spec 02 §8.3): input tap,
// sources, gain, meter, count-in, record/stop, auto-trim, crop + undo,
// naming / folders, save and save & load.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "app/input_gain.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {
RecorderScreen& rec(Harness& h) { return *static_cast<RecorderScreen*>(h.app->ui.screen(kModRecorder)); }

// Tone (440 Hz, amplitude 10000) in blocks [on, off), silence elsewhere.
void toneBlocks(Harness& h, uint64_t on, uint64_t off) {
    h.lineGen = [on, off](int16_t* b, uint64_t k) {
        for (int i = 0; i < 128; ++i) {
            const double t = double(k * 128 + uint64_t(i)) / 44100.0;
            const int16_t v = k >= on && k < off ? int16_t(10000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * t)) : 0;
            b[2 * i] = b[2 * i + 1] = v;
        }
    };
}

// Input flowing while the UI runs: ~3 audio blocks per 10 ms loop slice, as
// on the device (the meter reads the peak since its last frame).
void live(Harness& h, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
        h.audio.pump(3);
        h.run(10);
    }
}

void recordSome(Harness& h, uint32_t blocks) {
    h.tap(btn::F8);   // Record
    CHECK(h.app->capture.recording());
    h.lineBlocks = 0;
    h.audio.pump(int(blocks));
    h.tap(btn::F8);   // Stop
}
}  // namespace

TEST(recorder_record_trim_crop_undo) {
    Harness h;
    App& a = *h.app;
    toneBlocks(h, 10, 40);
    h.tap(btn::SampleRecorder);
    CHECK(a.ui.active() == kModRecorder);
    RecorderScreen& r = rec(h);
    CHECK_EQ(r.state, 0);
    CHECK_STR(a.ui.bar.label[0].top, "Source");
    CHECK_STR(a.ui.bar.label[0].bottom, "Line in ST");   // Config default Line in channel = Stereo (stock default)
    CHECK_STR(a.ui.bar.label[7].top, "Record");
    CHECK(r.sourceOk);
    recordSome(h, 60);
    CHECK(!a.capture.active());
    CHECK_EQ(r.state, 1);
    CHECK_EQ(r.len, 60u * 128u);
    CHECK_STR(a.ui.bar.label[4].top, "Crop");
    // Auto-trim (appendix C §2.10): first frame above -70 dBFS minus 44 frames.
    const uint32_t s0 = uint32_t(uint64_t(r.startM) * r.len / 65535);
    const uint32_t s1 = uint32_t(uint64_t(r.endM) * r.len / 65535);
    CHECK(s0 >= 10 * 128 - 44 - 2 && s0 <= 10 * 128 - 44 + 3);
    CHECK(s1 > 40 * 128 + 44 && s1 < 40 * 128 + 1500);
    // Crop (b4) keeps the selection; Undo (b5) restores it.
    h.tap(btn::F5);
    CHECK_EQ(r.len, s1 - s0);
    CHECK_EQ(r.undoDepth, 1);
    CHECK_EQ(r.startM, 0);
    CHECK_EQ(r.endM, 65535);
    h.tap(btn::F6);
    CHECK_EQ(r.len, 60u * 128u);
    CHECK_EQ(r.undoDepth, 0);
    // Undo depth is capped at 5 levels.
    for (int i = 0; i < 7; ++i) {
        r.startM = 1000;
        h.tap(btn::F5);
    }
    CHECK_EQ(r.undoDepth, RecorderScreen::kUndoLevels);
    for (int i = 0; i < 7; ++i) h.tap(btn::F6);
    CHECK_EQ(r.undoDepth, 0);
    CHECK(r.len < 60u * 128u);   // the oldest levels were dropped
    // Cancel (b6) asks; Continue discards and returns to the setup state.
    h.tap(btn::F7);
    CHECK(a.ui.askOpen());
    CHECK(h.disp.lastHasText("Yes"));   // manual p.196 "Confirm cancel with [Yes]" (MANUAL_AUDIT #30)
    h.tap(btn::F8);
    CHECK_EQ(r.state, 0);
}

TEST(recorder_autotrim_unit) {
    std::vector<int16_t> d(10000, 0);
    uint32_t s = 0, e = 0;
    autoTrim(d.data(), uint32_t(d.size()), &s, &e);
    CHECK_EQ(s, 0u);   // silence: full range kept
    CHECK_EQ(e, 10000u);
    for (int i = 3000; i < 4000; ++i) d[size_t(i)] = 8000;
    autoTrim(d.data(), uint32_t(d.size()), &s, &e);
    CHECK_EQ(s, 3000u - 44u);
    CHECK(e > 4000u + 44u && e < 4900u);
    CHECK_EQ(meterUnits(1.0f), 100);
    CHECK_EQ(meterUnits(0.0f), 0);
    CHECK(meterUnits(0.5f) > 70 && meterUnits(0.5f) < 85);
    CHECK(meterUnits(0.9f) > 85 && meterUnits(0.9f) < 100);
    // spec 22 §3: the gain is analog (codec level), stock integer scale.
    CHECK_EQ(input_gain::lineLevel(0), 0);
    CHECK_EQ(input_gain::lineLevel(50), 7);
    CHECK_EQ(input_gain::lineLevel(100), 15);
    CHECK_EQ(input_gain::lineLevel(7), 1);
    CHECK_EQ(input_gain::micDb(2, 50), 12);
    CHECK_EQ(input_gain::micDb(2, 100), 25);
    CHECK_EQ(input_gain::micDb(3, 50), 22);
    CHECK_EQ(input_gain::micDb(3, 100), 44);
}

TEST(recorder_save_and_load) {
    Harness h;
    App& a = *h.app;
    a.cfg.lineInChannel = 1;   // Mono Left: the mono file (stereo: recorder_stereo_* below)
    toneBlocks(h, 5, 30);
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    recordSome(h, 40);
    h.tap(btn::F5);   // crop to the trimmed range
    const uint32_t frames = r.len;
    h.tap(btn::F8);   // Save -> naming keyboard
    CHECK_EQ(r.state, 2);
    CHECK(a.ui.kb.isOpen());
    CHECK_STR(a.ui.kb.text(), "recording001");
    h.tap(btn::F7);   // Save & Load
    CHECK(!a.ui.kb.isOpen());
    CHECK(h.fs.dirs.count("Recordings"));
    CHECK(h.fs.files.count("Recordings/recording001.wav"));
    WavInfo info;
    const auto& w = h.fs.files["Recordings/recording001.wav"];
    CHECK(wavParseHeader(w.data(), w.size(), info));
    CHECK_EQ(info.channels, 1);
    CHECK_EQ(info.bitsPerSample, 16);
    CHECK_EQ(info.sampleRate, 44100u);
    CHECK_EQ(info.dataSize, frames * 2);
    // Loaded into the first free slot; the loader shows it.
    CHECK(a.instrPresent[0]);
    CHECK_EQ(a.bank.frames(0), frames);
    CHECK_STR(a.instruments[0].name, "recording001");
    CHECK_EQ(a.currentInstrument(), 0);
    CHECK(a.ui.active() == kModLoader);

    // Second recording: plain Save -> recording002; then a name clash.
    h.tap(btn::SampleRecorder);
    recordSome(h, 40);
    h.tap(btn::F8);
    CHECK_STR(a.ui.kb.text(), "recording002");
    h.tap(btn::F8);   // Save
    CHECK(h.fs.files.count("Recordings/recording002.wav"));
    CHECK_EQ(r.state, 1);
    CHECK(!a.instrPresent[1]);
    CHECK(!r.save("recording002", false, false));   // exists -> overwrite prompt
    CHECK(a.ui.askOpen());
    h.tap(btn::F7);   // Rename -> first free "<name><k>"
    CHECK(a.ui.kb.isOpen());
    CHECK_STR(a.ui.kb.text(), "recording0021");
    h.tap(btn::F8);
    CHECK(h.fs.files.count("Recordings/recording0021.wav"));
    CHECK(r.save("recording002", false, false) == false);
    h.tap(btn::F8);   // Overwrite
    CHECK(!a.ui.askOpen());
}

TEST(recorder_folders_and_names) {
    Harness h;
    App& a = *h.app;
    char folder[32], name[32];
    CHECK(a.recordingsFolder(folder, sizeof(folder)));
    CHECK_STR(folder, "Recordings");
    h.fs.files["Recordings/recording001.wav"] = {1};
    CHECK(a.recordingName(folder, name, sizeof(name)));
    CHECK_STR(name, "recording002");
    // Highest "Recordings N" is used; > 98 entries starts the next one.
    h.fs.dirs.insert("Recordings 2");
    CHECK(a.recordingsFolder(folder, sizeof(folder)));
    CHECK_STR(folder, "Recordings 2");
    for (int i = 0; i < 99; ++i) h.fs.files["Recordings 2/f" + std::to_string(i) + ".wav"] = {1};
    CHECK(a.recordingsFolder(folder, sizeof(folder)));
    CHECK_STR(folder, "Recordings 3");
    CHECK(h.fs.dirs.count("Recordings 3"));
}

// User report (2.0.1, also 1.9.4): "metering on the recording page seems a bit
// slow". The meter was read every 160 ms and fell 1 unit per 500 ms, so a
// peak kept the bar up for tens of seconds. Stock reads it every frame and
// lets it fall one unit per pass (> 500 us, appendix C §2.6 / appendix A
// timers): it must drop to the new level within ~0.1 s.
TEST(recorder_meter_follows_input_at_frame_rate) {
    CHECK_EQ(RecorderScreen::meterFall(90, 95, 16), 95);   // instant rise
    CHECK_EQ(RecorderScreen::meterFall(90, 10, 16), 74);   // 1 unit per ms
    CHECK_EQ(RecorderScreen::meterFall(90, 10, 500), 10);  // never below the reading
    CHECK(RecorderScreen::kMeterMs <= 20);                   // >= 50 readings / s
    Harness h;
    bool loud = true;
    h.lineGen = [&loud](int16_t* b, uint64_t) {
        for (int i = 0; i < 256; ++i) b[i] = loud ? int16_t((i / 2) % 2 ? 30000 : -30000) : 0;
    };
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    live(h, 100);
    CHECK(r.meter > 85);
    loud = false;
    live(h, 150);   // 150 ms of silence
    CHECK_EQ(r.meter, 0);
}

TEST(recorder_sources_gain_monitor_meter) {
    Harness h;
    App& a = *h.app;
    h.lineGen = [](int16_t* b, uint64_t) {
        for (int i = 0; i < 256; ++i) b[i] = int16_t((i / 2) % 2 ? 30000 : -30000);
    };
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    // Meter follows the input.
    live(h, 200);
    CHECK(r.meter > 85);
    // b0 again = next source: radio (no tuner -> not available), mics (no routing hook).
    h.tap(btn::F1);
    CHECK_EQ(a.cfg.recSource, 1);
    CHECK(!r.sourceOk);
    CHECK_STR(a.ui.bar.label[0].bottom, "n/a");
    CHECK_STR(a.ui.bar.label[1].top, "Radio Freq");
    h.tap(btn::F8);   // Record refused
    CHECK(!a.capture.active());
    h.tap(btn::F1);
    CHECK_EQ(a.cfg.recSource, 2);
    h.tap(btn::F1);
    h.tap(btn::F1);
    CHECK_EQ(a.cfg.recSource, 0);   // wraps
    CHECK(r.sourceOk);
    // Gain (b5) +10.
    h.tap(btn::F6);
    h.enc(10);
    CHECK_EQ(a.cfg.recGain[0], 60);
    // Monitor (b6): second press toggles; off keeps the input out of the mix.
    CHECK(!a.inputMonitorMuted);
    h.tap(btn::F7);
    h.tap(btn::F7);
    CHECK_EQ(a.cfg.recMonitor, 0);
    CHECK(a.inputMonitorMuted);
    h.tap(btn::Pattern);
    CHECK(!a.inputMonitorMuted);   // leaving restores the monitor path
}

TEST(recorder_count_in_and_memory_full) {
    // Small sample bank: 60000 samples of free memory.
    Harness h(true, 60000);
    App& a = *h.app;
    toneBlocks(h, 0, 100000);
    a.cfg.lineInChannel = 1;   // mono: 60000 frames
    a.cfg.metronome = 1;
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    h.tap(btn::Rec);
    CHECK(a.capture.state() == Capture::kCountIn);
    // One bar of (tsNum+1)*(tsDen+1) steps at the project tempo.
    const double stepSecs = 15.0 / double(a.project.tempo());
    const uint32_t countFrames = uint32_t((a.cfg.tsNum + 1) * (a.cfg.tsDen + 1) * uint32_t(stepSecs * 44100.0 + 0.5));
    h.audio.pump(int(countFrames / 128) - 1);
    CHECK(a.capture.state() == Capture::kCountIn);
    h.audio.pump(2);
    CHECK(a.capture.recording());
    CHECK(a.capture.length() > 0 && a.capture.length() <= 256);
    // Memory full: the tap stops, the screen reports it.
    h.audio.pump(600);
    CHECK(!a.capture.active());
    CHECK(a.capture.full());
    h.run(50);
    CHECK_EQ(r.state, 1);
    CHECK_EQ(r.len, a.capture.capacity());
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);
    CHECK(!a.ui.askOpen());
}

TEST(recorder_forces_midi_instrument) {
    // appendix C §2.2: entry forces the current instrument into 48..63 and
    // remembers the last MIDI instrument used there.
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(3);
    h.tap(btn::SampleRecorder);
    CHECK_EQ(a.currentInstrument(), kFirstMidiInstrument);
    a.setCurrentInstrument(kFirstMidiInstrument + 5);
    h.tap(btn::Pattern);
    a.setCurrentInstrument(7);
    h.tap(btn::SampleRecorder);
    CHECK_EQ(a.currentInstrument(), kFirstMidiInstrument + 5);
    h.tap(btn::Pattern);
    a.setCurrentInstrument(kFirstMidiInstrument + 9);   // already MIDI: kept
    h.tap(btn::SampleRecorder);
    CHECK_EQ(a.currentInstrument(), kFirstMidiInstrument + 9);
}

TEST(recorder_metronome_opens_config_and_returns) {
    // appendix C §2.9: F4 Metronome opens Config remembering the recorder;
    // the return start argument comes back to the setup state.
    Harness h;
    App& a = *h.app;
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    CHECK_STR(a.ui.bar.label[3].top, "Metronome");
    h.tap(btn::F4);
    CHECK(a.ui.active() == kModConfig);
    CHECK(r.returnsFromConfig());
    a.ui.switchTo(kModRecorder, kModConfig);
    CHECK(a.ui.active() == kModRecorder);
    CHECK(!r.returnsFromConfig());
    CHECK_EQ(r.state, 0);
    CHECK_EQ(r.sel, 3);
}

TEST(recorder_pads_play_chromatically) {
    // appendix C §2.11: b0 Preview plays at note 60 (own pitch); pads play
    // the selection through the pad -> note map, release stops.
    Harness h;
    App& a = *h.app;
    toneBlocks(h, 0, 1000);
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    recordSome(h, 40);
    r.startM = 0;
    r.endM = 65535;
    const uint32_t n = r.len;
    h.down(btn::F1);
    CHECK(a.engine.previewBufferActive());
    CHECK_EQ(r.playNote, 60);
    CHECK_EQ(r.playFrames, n);
    h.up(btn::F1);
    CHECK(!a.engine.previewBufferActive());
    CHECK_EQ(r.playNote, -1);
    for (uint8_t p : {uint8_t(0), uint8_t(20), uint8_t(47)}) {
        const int note = a.padNote(p);
        h.push(InputEvent::kPadDown, p);
        CHECK(a.engine.previewBufferActive());
        CHECK_EQ(r.playNote, note);
        const double expect = double(n - 1) / std::pow(2.0, double(note - 60) / 12.0) + 1.0;
        CHECK(std::fabs(double(r.playFrames) - expect) <= 2.0);
        h.push(InputEvent::kPadUp, p);
        CHECK(!a.engine.previewBufferActive());
    }
    // Two pads held: the last one sounds; releasing the first keeps it.
    h.push(InputEvent::kPadDown, 10);
    h.push(InputEvent::kPadDown, 30);
    CHECK_EQ(r.playNote, int(a.padNote(30)));
    h.push(InputEvent::kPadUp, 10);
    CHECK(a.engine.previewBufferActive());
    h.push(InputEvent::kPadUp, 30);
    CHECK(!a.engine.previewBufferActive());
}

TEST(recorder_midi_notes_play_chromatically) {
    // spec 17 §1.3 owner 4: incoming notes use the recorder preview, like
    // its pads (note 60 = own pitch); the last note sounds.
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    toneBlocks(h, 0, 1000);
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    recordSome(h, 40);
    r.startM = 0;
    r.endM = 65535;
    auto send = [&](uint8_t st, uint8_t n, uint8_t v) {
        h.midi.in.insert(h.midi.in.end(), {st, n, v});
        h.run(5);
    };
    send(0x90, 72, 100);
    CHECK(a.engine.previewBufferActive());
    CHECK_EQ(r.playNote, 72);
    CHECK_EQ(a.auditionTrack(uint16_t(App::kAuditionMidiIn + 72)), -1);   // no track voice
    const double expect = double(r.len - 1) / 2.0 + 1.0;
    CHECK(std::fabs(double(r.playFrames) - expect) <= 2.0);
    send(0x90, 60, 100);
    CHECK_EQ(r.playNote, 60);
    CHECK_EQ(r.playFrames, r.len);
    send(0x80, 72, 0);   // not the sounding note
    CHECK(a.engine.previewBufferActive());
    send(0x90, 60, 0);   // velocity-0 note-on = note-off
    CHECK(!a.engine.previewBufferActive());
    CHECK_EQ(r.playNote, -1);
}

// ---- Radio source (spec 01 §5.4-§5.6, spec 05 appendix C §2.4/§2.8) ---------

namespace {
struct FakeTuner final : IRadio {
    float lo = 0, hi = 0, step = 0, freq = 87.5f;
    int tunes = 0;
    void setBand(float a, float b, float s) override { lo = a; hi = b; step = s; }
    bool tune(float mhz) override { freq = mhz; ++tunes; return true; }
    bool seek(bool) override { return true; }
    bool busy() const override { return false; }
    float frequency() const override { return freq; }
};
struct FakeRouting {
    int calls = 0, last = -1;
    std::vector<int> sources;
    static bool fn(void* c, uint8_t src, uint8_t) {
        auto* r = static_cast<FakeRouting*>(c);
        ++r->calls;
        r->last = src;
        r->sources.push_back(src);
        return true;
    }
};
}  // namespace

TEST(recorder_radio_source_powers_tuner_routes_input_and_records) {
    Harness h;
    App& a = *h.app;
    FakeTuner tuner;
    FakeRouting route;
    a.setInputRouting(&FakeRouting::fn, &route, &tuner);
    h.run(20);   // project recorder settings -> working copies
    a.cfg.radioFreq10 = 1011;
    h.run(20);   // -> project 0x13C (spec 02 §3.3)
    CHECK_EQ(a.cfg.radioFreq10, 1011);
    toneBlocks(h, 0, 1u << 30);
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    CHECK_EQ(route.last, 0);   // line in first
    // b0: Radio -> the HAL routes source 1 (tuner on, codec LINE input), the
    // screen sets the region band and tunes the stored frequency.
    h.tap(btn::F1);
    CHECK_EQ(a.cfg.recSource, 1);
    CHECK_EQ(route.last, 1);
    CHECK(r.sourceOk);
    CHECK_STR(a.ui.bar.label[0].bottom, "Radio");
    CHECK_NEAR(tuner.lo, 87.5, 1e-6);
    CHECK_NEAR(tuner.freq, 101.1, 1e-4);
    CHECK(tuner.tunes >= 1);
    // The tuner's audio arrives as the line input: meter and capture follow it.
    live(h, 200);
    CHECK(r.meter > 0);
    recordSome(h, 40);
    CHECK(a.capture.length() > 0);
    int peak = 0;
    for (uint32_t i = 0; i < a.capture.length(); ++i) peak = std::max(peak, std::abs(int(a.capture.buffer()[i])));
    CHECK(peak > 1000);
    // While Radio stays the source the screen re-routes it about once a second
    // (the HAL re-powers a tuner that dropped out after an I2C failure).
    const int before = route.calls;
    h.run(2100);
    CHECK(route.calls >= before + 2);
    CHECK_EQ(route.last, 1);
    // Leaving the recorder routes the line input again (tuner off).
    h.tap(btn::Pattern);
    CHECK_EQ(route.last, 0);
}

// ---- 2.x stereo sampling (2x-04 §5, revived 2026-10-19) ---------------------

namespace {
// L: 440 Hz at 10000, R: 660 Hz at 6000 (distinct lanes) in blocks [on, off).
// silentR: the right channel stays 0.
void stereoBlocks(Harness& h, uint64_t on, uint64_t off, bool silentR = false) {
    h.lineGen = [on, off, silentR](int16_t* b, uint64_t k) {
        for (int i = 0; i < 128; ++i) {
            const double t = double(k * 128 + uint64_t(i)) / 44100.0;
            const bool in = k >= on && k < off;
            b[2 * i] = in ? int16_t(10000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * t)) : 0;
            b[2 * i + 1] = in && !silentR ? int16_t(6000.0 * std::sin(2.0 * 3.14159265358979 * 660.0 * t)) : 0;
        }
    };
}
int16_t sampleAt(const std::vector<uint8_t>& w, const WavInfo& info, uint32_t idx) {
    const uint8_t* p = w.data() + info.dataOffset + 2 * idx;
    return int16_t(uint16_t(p[0] | (p[1] << 8)));
}
}  // namespace

TEST(recorder_stereo_lanes_and_wav) {
    // Line In + Config "Line in channel" = Stereo: two lanes, a 2-channel WAV
    // (frames x 4 data bytes, L/R interleaved).
    Harness h;
    App& a = *h.app;
    a.cfg.lineInChannel = 0;   // Stereo
    stereoBlocks(h, 5, 30);
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    CHECK_STR(a.ui.bar.label[0].bottom, "Line in ST");
    recordSome(h, 40);
    CHECK(a.capture.stereo());
    CHECK(a.capture.bufferR() != nullptr);
    CHECK_EQ(a.capture.bufferR(), a.capture.buffer() + a.capture.capacity());
    CHECK_EQ(r.len, 40u * 128u);
    int diff = 0;
    for (uint32_t i = 0; i < r.len; ++i) diff += a.capture.buffer()[i] != a.capture.bufferR()[i];
    CHECK(diff > 1000);   // the lanes differ
    // The lanes are the raw L and R (no mix).
    int16_t blk[256];
    h.lineGen(blk, 10);   // block 10 of the take
    bool raw = true;
    for (int i = 0; raw && i < 128; ++i)
        raw = a.capture.buffer()[10 * 128 + i] == blk[2 * i] && a.capture.bufferR()[10 * 128 + i] == blk[2 * i + 1];
    CHECK(raw);
    const uint32_t s0 = uint32_t(uint64_t(r.startM) * r.len / 65535);
    const uint32_t s1 = uint32_t(uint64_t(r.endM) * r.len / 65535);
    const uint32_t frames = s1 - s0;
    h.tap(btn::F8);   // Save -> naming keyboard
    h.tap(btn::F8);   // Save
    const auto& w = h.fs.files["Recordings/recording001.wav"];
    WavInfo info;
    CHECK(wavParseHeader(w.data(), w.size(), info));
    CHECK_EQ(info.channels, 2);
    CHECK_EQ(info.bitsPerSample, 16);
    CHECK_EQ(info.sampleRate, 44100u);
    CHECK_EQ(info.dataSize, frames * 4);
    bool order = true;
    for (uint32_t i = 0; order && i < frames; i += 97) {
        order = sampleAt(w, info, 2 * i) == a.capture.buffer()[s0 + i] &&
                sampleAt(w, info, 2 * i + 1) == a.capture.bufferR()[s0 + i];
    }
    CHECK(order);
}

TEST(recorder_stereo_save_and_load_gives_stereo_instrument) {
    Harness h;
    App& a = *h.app;
    stereoBlocks(h, 5, 30);
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    recordSome(h, 40);
    h.tap(btn::F5);   // crop to the trimmed range
    const uint32_t frames = r.len;
    std::vector<int16_t> L(a.capture.buffer() + r.off, a.capture.buffer() + r.off + frames);
    std::vector<int16_t> R(a.capture.bufferR() + r.off, a.capture.bufferR() + r.off + frames);
    h.tap(btn::F8);
    h.tap(btn::F7);   // Save & Load
    CHECK(a.instrPresent[0]);
    CHECK(a.bank.stereo(0));
    CHECK_EQ(a.bank.frames(0), frames);
    bool same = true;
    for (uint32_t i = 0; same && i < frames; ++i) same = a.bank.left(0)[i] == L[i] && a.bank.right(0)[i] == R[i];
    CHECK(same);
    CHECK(a.ui.active() == kModLoader);
}

TEST(recorder_mono_left_right_and_mic_stay_mono) {
    for (uint8_t ch : {uint8_t(1), uint8_t(2)}) {
        Harness h;
        App& a = *h.app;
        a.cfg.lineInChannel = ch;   // Mono Left / Mono Right
        stereoBlocks(h, 0, 1000);
        h.tap(btn::SampleRecorder);
        RecorderScreen& r = rec(h);
        CHECK_STR(a.ui.bar.label[0].bottom, ch == 1 ? "Line in L" : "Line in R");
        recordSome(h, 10);
        CHECK(!a.capture.stereo());
        CHECK(a.capture.bufferR() == nullptr);
        // The recorded lane is that input channel: compare with the generator.
        int16_t blk[256];
        h.lineGen(blk, 0);   // block 0 of the take
        bool same = true;
        for (int i = 0; same && i < 128; ++i) same = a.capture.buffer()[i] == blk[2 * i + (ch == 2 ? 1 : 0)];
        CHECK(same);
        (void)r;
    }
    // Mic with a Stereo config: still mono (the mic is mono).
    Harness h;
    App& a = *h.app;
    FakeRouting route;
    a.setInputRouting(&FakeRouting::fn, &route, nullptr);
    a.cfg.lineInChannel = 0;
    stereoBlocks(h, 0, 1000);
    h.tap(btn::SampleRecorder);
    h.tap(btn::F1);   // Radio (no tuner: n/a)
    h.tap(btn::F1);   // Mic LG
    CHECK_EQ(a.cfg.recSource, 2);
    CHECK_STR(a.ui.bar.label[0].bottom, "Mic LG");
    recordSome(h, 10);
    CHECK(!a.capture.stereo());
    CHECK(a.capture.bufferR() == nullptr);
    CHECK_EQ(a.capture.length(), 10u * 128u);
}

TEST(recorder_stereo_meters_side_by_side) {
    Harness h;
    App& a = *h.app;
    bool rightOn = true;
    h.lineGen = [&rightOn](int16_t* b, uint64_t) {
        for (int i = 0; i < 128; ++i) {
            b[2 * i] = int16_t(i % 2 ? 30000 : -30000);
            b[2 * i + 1] = rightOn ? int16_t(i % 2 ? 30000 : -30000) : 0;
        }
    };
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    CHECK(a.capture.stereo());   // setup page meters follow the stereo input
    live(h, 100);
    CHECK(r.meter > 85);
    CHECK(r.meterR > 85);
    rightOn = false;   // a silent R makes meter R fall, L stays
    live(h, 150);
    CHECK(r.meter > 85);
    CHECK_EQ(r.meterR, 0);
    // Mono Left: one meter, both values the recorded channel.
    a.cfg.lineInChannel = 1;
    a.selectInput(0, a.cfg.recGain[0]);
    live(h, 150);
    CHECK(!a.capture.stereo());
    CHECK_EQ(r.meterR, r.meter);
}

TEST(recorder_stereo_memory_full_halves_frames) {
    // 100000 free samples: 50000 frames per lane in stereo (Record needs at
    // least 1 s, 44100 frames, so 60000 samples would refuse to start).
    Harness h(true, 100000);
    App& a = *h.app;
    stereoBlocks(h, 0, 100000);
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    h.tap(btn::Rec);
    CHECK(a.capture.recording());
    CHECK_EQ(a.capture.capacity(), 50000u);
    h.audio.pump(500);
    CHECK(!a.capture.active());
    CHECK(a.capture.full());
    h.run(50);
    CHECK_EQ(r.len, 50000u);
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);
    CHECK(!a.ui.askOpen());
    // 60000 free samples: 60000 mono frames but only 30000 stereo ones,
    // under the 1 s minimum: Record refuses in stereo.
    Harness h2(true, 60000);
    stereoBlocks(h2, 0, 100000);
    h2.tap(btn::SampleRecorder);
    CHECK_EQ(h2.app->capture.capacityFor(true), 30000u);
    h2.tap(btn::Rec);
    CHECK(!h2.app->capture.active());
    CHECK(h2.app->ui.askOpen());   // "Not enough sample memory"
}

TEST(recorder_stereo_pads_transpose_both_lanes) {
    Harness h;
    App& a = *h.app;
    stereoBlocks(h, 0, 1000);
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    recordSome(h, 40);
    r.startM = 0;
    r.endM = 65535;
    const uint8_t pad = 20;
    const int note = a.padNote(pad);
    h.push(InputEvent::kPadDown, pad);
    CHECK(a.engine.previewBufferActive());
    CHECK_EQ(a.engine.previewBufferLeft(), a.capture.buffer());
    CHECK_EQ(a.engine.previewBufferRight(), a.capture.bufferR());
    CHECK_EQ(a.engine.previewBufferStep(), App::previewStep(note));
    // Both output channels carry their own lane (R is the 660 Hz one).
    int16_t out[256];
    bool differ = false;
    for (int b = 0; b < 4; ++b) {
        a.engine.renderLive(out);
        for (int i = 0; i < 128; ++i) differ |= out[2 * i] != out[2 * i + 1];
    }
    CHECK(differ);
    h.push(InputEvent::kPadUp, pad);
    CHECK(!a.engine.previewBufferActive());
    CHECK_EQ(App::previewStep(60), 65536u);
    CHECK_EQ(App::previewStep(72), 131072u);
    CHECK_EQ(App::previewStep(48), 32768u);
}

namespace {
// Waveform vertices (x 100..699) whose y falls in [y0, y1] in the last frame.
int waveVerts(Harness& h, int y0, int y1) {
    int n = 0;
    for (auto& l : h.disp.last) {
        int x, y;
        if (std::sscanf(l.c_str(), "VERTEX %d %d", &x, &y) == 2 && x >= 101 && x <= 698 && y >= y0 && y <= y1) ++n;
    }
    return n;
}
}  // namespace

TEST(recorder_stereo_take_draws_two_lanes) {
    // Like the Tracker+ recorder: a stereo take shows L on top and R below,
    // each lane centred in its half (L 169, R 279); a mono take keeps one
    // full-height lane centred at 225.
    for (int stereo = 1; stereo >= 0; --stereo) {
        Harness h;
        App& a = *h.app;
        a.cfg.lineInChannel = stereo ? 0 : 1;   // Stereo / Left
        stereoBlocks(h, 0, 1000);
        h.tap(btn::SampleRecorder);
        h.tap(btn::F8);   // Record
        h.lineBlocks = 0;
        h.audio.pump(40);
        h.run(40);
        CHECK(a.capture.recording());
        const int recL = waveVerts(h, 150, 188), recR = waveVerts(h, 265, 293), recMid = waveVerts(h, 210, 240);
        h.tap(btn::F8);   // Stop -> edit view
        h.run(40);
        CHECK(a.ui.lastDlWords() < 1900);
        const int edL = waveVerts(h, 150, 188), edR = waveVerts(h, 265, 293), edMid = waveVerts(h, 210, 240);
        if (stereo) {
            CHECK(recL > 50 && recR > 50 && edL > 50 && edR > 50);
            CHECK_EQ(edMid, 0);   // gap between the lanes
            CHECK_EQ(recMid, 0);
        } else {
            CHECK(edMid > 50 && recMid > 50);
            CHECK_EQ(edR, 0);
        }
    }
}

TEST(recorder_name_readable_on_every_theme) {
    // Monochrome's name box is the light title band (b9b9ba): the name is
    // drawn in its title text (black), not the light list grey. Original and
    // Anniversary keep their list text on the dark box.
    const char* want[3] = {"COLOR FFFFFF", "COLOR 000000", "COLOR C9C9CA"};
    for (int t = 0; t < 3; ++t) {
        Harness h;
        h.app->cfg.theme = uint8_t(t);
        toneBlocks(h, 0, 1000);
        h.tap(btn::SampleRecorder);
        recordSome(h, 40);
        h.tap(btn::F8);   // Save -> naming keyboard
        CHECK(h.app->ui.kb.isOpen());
        h.run(40);
        std::string colour;
        bool found = false;
        for (auto& l : h.disp.last) {
            if (l.compare(0, 6, "COLOR ") == 0) colour = l;
            if (l.compare(0, 5, "TEXT ") == 0 && l.find("\"recording001\"") != std::string::npos) {
                found = true;
                break;
            }
        }
        CHECK(found);
        CHECK_STR(colour.c_str(), want[t]);
    }
}

TEST(recorder_monitor_only_on_setup_and_while_recording) {
    // Owner request: after a take the input (radio, line) no longer plays
    // under Preview / the pads in the edit view; it monitors on the setup
    // page and while recording.
    Harness h;
    App& a = *h.app;
    a.cfg.recMonitor = 1;
    toneBlocks(h, 0, 1000);
    h.tap(btn::SampleRecorder);
    h.run(20);
    CHECK(!a.inputMonitorMuted);   // setup page
    h.tap(btn::F8);                // Record
    h.run(20);
    CHECK(!a.inputMonitorMuted);   // recording
    h.lineBlocks = 0;
    h.audio.pump(30);
    h.tap(btn::F8);                // Stop -> edit view
    h.run(20);
    CHECK_EQ(rec(h).state, 1);
    CHECK(a.inputMonitorMuted);
    h.tap(btn::F7);                // Cancel -> setup again
    h.run(20);
    if (rec(h).state == 0) CHECK(!a.inputMonitorMuted);
}

// Preview playhead (owner request): while Preview plays the selection, a
// line follows the play position across the waveform; none when it stops.
TEST(recorder_preview_shows_a_playhead) {
    Harness h;
    App& a = *h.app;
    toneBlocks(h, 0, 400);
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = rec(h);
    recordSome(h, 400);   // ~1.16 s
    CHECK_EQ(r.state, 1);
    r.startM = 0;
    r.endM = 65535;
    CHECK_EQ(r.playheadX(), -1);
    h.down(btn::F1);      // Preview (held)
    h.audio.pump(20);
    const int x0 = r.playheadX();
    h.audio.pump(150);
    const int x1 = r.playheadX();
    CHECK(x0 >= 100 && x0 < 700);
    CHECK(x1 > x0);       // it moves with the playback
    h.run(10);
    CHECK(a.ui.lastDlWords() > 0);
    h.up(btn::F1);
    h.run(10);
    CHECK_EQ(r.playheadX(), -1);
}

TEST(recorder_metronome_label_two_lines) {
    // Manual p.195-196 / spec 05 appC §2.3: F4 "Metronome / Config"
    // (MANUAL_AUDIT_2026-10-23 #29).
    Harness h;
    h.tap(btn::SampleRecorder);
    const BarLabel& l = h.app->ui.bar.label[3];
    if (l.top[0]) {
        CHECK(std::string(l.top) == "Metronome");
        CHECK(std::string(l.bottom) == "Config");
    }
}
