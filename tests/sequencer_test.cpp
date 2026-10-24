// Sequencer tests (spec 03).
#include "test.h"

#include <cstring>
#include <vector>

#include "model/pattern.h"
#include "model/project.h"
#include "sequencer/seq_rng.h"
#include "sequencer/seq_tables.h"
#include "sequencer/sequencer.h"

using namespace ot;

namespace {

struct Ev {
    enum Kind { NoteOn, NoteOff, ApplyFx, ResetFx, Click, Midi } kind;
    int track;
    int a, b, c;          // NoteOn: note, instr, volume; NoteOff: mode; ApplyFx: slot,type,value; Midi: bytes
    int len;
    uint64_t tick;        // seq.tickCount() at the event
    double time;          // seconds of audio time elapsed when the event happened
    int fxAType, fxAVal, fxBType, fxBVal;
    int ports;
};

struct FakeSink : ISequencerSink {
    Sequencer* seq = nullptr;
    double time = 0;
    std::vector<Ev> ev;
    int blockUpdates = 0;
    float lastTempo = 0;

    Ev base(Ev::Kind k, int track) {
        Ev e{};
        e.kind = k;
        e.track = track;
        e.tick = seq ? seq->tickCount() : 0;
        e.time = time;
        return e;
    }
    void voiceNoteOn(uint8_t track, const VoiceNoteOn& n) override {
        Ev e = base(Ev::NoteOn, track);
        e.a = n.note; e.b = n.instrument; e.c = n.volume;
        e.fxAType = n.fxA.type; e.fxAVal = n.fxA.value; e.fxBType = n.fxB.type; e.fxBVal = n.fxB.value;
        ev.push_back(e);
    }
    void voiceNoteOff(uint8_t track, NoteOffMode mode) override {
        Ev e = base(Ev::NoteOff, track);
        e.a = int(mode);
        ev.push_back(e);
    }
    void voiceApplyFx(uint8_t track, uint8_t slot, uint8_t type, uint8_t value) override {
        Ev e = base(Ev::ApplyFx, track);
        e.a = slot; e.b = type; e.c = value;
        ev.push_back(e);
    }
    void resetAllVoiceFx() override { ev.push_back(base(Ev::ResetFx, -1)); }
    void engineBlockUpdate(float bpm) override { ++blockUpdates; lastTempo = bpm; }
    void metronomeClick(bool accent, uint8_t vel) override {
        Ev e = base(Ev::Click, -1);
        e.a = accent; e.b = vel;
        ev.push_back(e);
    }
    void midiSend(uint8_t ports, const uint8_t* b, uint8_t len) override {
        Ev e = base(Ev::Midi, -1);
        e.a = b[0]; e.b = len > 1 ? b[1] : -1; e.c = len > 2 ? b[2] : -1;
        e.len = len;
        e.ports = ports;
        ev.push_back(e);
    }

    std::vector<Ev> of(Ev::Kind k, int track = -2) const {
        std::vector<Ev> r;
        for (const auto& e : ev)
            if (e.kind == k && (track == -2 || e.track == track)) r.push_back(e);
        return r;
    }
    std::vector<Ev> midiStatus(int status) const {
        std::vector<Ev> r;
        for (const auto& e : ev)
            if (e.kind == Ev::Midi && e.a == status) r.push_back(e);
        return r;
    }
    std::vector<Ev> midiNoteOns() const {
        std::vector<Ev> r;
        for (const auto& e : ev)
            if (e.kind == Ev::Midi && (e.a & 0xF0) == 0x90) r.push_back(e);
        return r;
    }
};

struct Load { int num; bool sync; uint64_t tick; };

struct FakeHost : ISequencerHost {
    Sequencer* seq = nullptr;
    std::vector<Load> loads;
    int patternEnds = 0, patternAdvances = 0, stepChanges = 0, dirty = 0;
    bool songScreen = false;
    void loadPattern(uint8_t number, Pattern& dst, bool sync) override {
        loads.push_back({number, sync, seq ? seq->tickCount() : 0});
        dst.setDefault();
        for (auto& t : dst.tracks) t.lastStep = 3;   // 4-step patterns
        dst.tracks[0].steps[0].note = int8_t(number * 10 % 128);
        dst.tracks[0].steps[0].instrument = 0;
    }
    void onStepChanged() override { ++stepChanges; }
    std::vector<uint64_t> endTicks;
    void onPatternEnd() override {
        ++patternEnds;
        endTicks.push_back(seq ? seq->tickCount() : 0);
    }
    void onPatternAdvanced() override { ++patternAdvances; }
    void markPatternDirty() override { ++dirty; }
    bool songScreenActive() override { return songScreen; }
};

struct Rig {
    Project project;
    Pattern a, b;
    FakeSink sink;
    FakeHost host;
    Sequencer seq;
    Rig(float bpm = 120.0f) {
        a.setDefault();
        b.setDefault();
        project.setTempo(bpm);
        sink.seq = &seq;
        host.seq = &seq;
        seq.attach(&sink, &host, &project, &a, &b);
        seq.settings().midiOut = 3;
    }
    Step& step(int t, int s) { return seq.currentPattern()->tracks[t].steps[s]; }
    void setNote(int t, int s, int note, int instr = 0) {
        step(t, s).note = int8_t(note);
        step(t, s).instrument = uint8_t(instr);
    }
    // FX-A = model fx[1], FX-B = model fx[0].
    void fxA(int t, int s, int type, int v) { step(t, s).fx[1] = StepFx{uint8_t(type), uint8_t(v)}; }
    void fxB(int t, int s, int type, int v) { step(t, s).fx[0] = StepFx{uint8_t(type), uint8_t(v)}; }
    void block() {
        seq.processBlock(128);
        sink.time += 128.0 / 44122.6484;
    }
    void blocks(int n) { for (int i = 0; i < n; ++i) block(); }
    // Run blocks until `pred` holds (max n blocks).
    template <class F> bool runUntil(F pred, int n = 100000) {
        for (int i = 0; i < n; ++i) {
            if (pred()) return true;
            block();
        }
        return pred();
    }
};

constexpr double kBlockS = 128.0 / 44122.6484;

}  // namespace

// --- RNG ---------------------------------------------------------------------

TEST(rng_park_miller) {
    seq::ParkMiller r(1);
    CHECK_EQ(r.next(), 16807u);
    CHECK_EQ(r.next(), 282475249u);
    CHECK_EQ(r.next(), 1622650073u);
    seq::ParkMiller z(0);
    const uint64_t expect = (uint64_t(0x1f0cce42u) * 16807u) % 0x7fffffffu;
    CHECK_EQ(uint64_t(z.next()), expect);
    seq::ParkMiller q(5);
    CHECK_EQ(q.range(7, 7), 7);
    CHECK_EQ(q.range(9, 3), 9);
    for (int i = 0; i < 1000; ++i) {
        const int v = q.range(10, 20);
        CHECK(v >= 10 && v < 20);
    }
}

TEST(tables) {
    CHECK_EQ(seq::kTicksPerStep, 576);
    CHECK_EQ(seq::chordMask(5), uint16_t((1 << 0) | (1 << 4) | (1 << 7)));
    CHECK_EQ(seq::chordMask(12), uint16_t((1 << 0) | (1 << 7) | (1 << 2)));  // 14 mod 12 = 2
    CHECK_EQ(seq::fxMax(3), 53);
    CHECK_EQ(seq::fxMax(14), 29);
    CHECK_EQ(seq::fxMin(39), 4);
    CHECK_EQ(seq::fxMax(23), 23);
    CHECK_EQ(seq::fxMax(24), 28);
}

// --- timing -------------------------------------------------------------------

TEST(tick_period_formula) {
    Rig r(120.0f);
    CHECK_NEAR(r.seq.tickPeriodUs(), 312500.0 / 120.0 / 12.0, 1e-3);
    r.project.setTempo(800.0f);
    CHECK_NEAR(r.seq.tickPeriodUs(), 312500.0 / 800.0 / 12.0, 1e-3);
    r.project.setTempo(5000.0f);  // load sanity -> 130
    CHECK_NEAR(r.seq.effectiveTempo(), 130.0, 1e-6);
}

TEST(ticks_per_blocks_at_bpms) {
    const float bpms[] = {40.0f, 90.0f, 120.0f, 133.0f, 300.0f, 800.0f};
    for (float bpm : bpms) {
        Rig r(bpm);
        r.seq.play(0);
        const int n = 20000;
        r.blocks(n);
        const double p = 312500.0 / bpm / 12.0;
        const double expect = n * 128.0 / 44122.6484 * 1e6 / p;
        CHECK_NEAR(double(r.seq.tickCount()), expect, 2.0);
    }
}

TEST(step_timing_matches_formula) {
    const float bpms[] = {60.0f, 120.0f, 175.0f};
    for (float bpm : bpms) {
        Rig r(bpm);
        for (int s = 0; s < 32; ++s) r.setNote(0, s, 60);
        r.seq.play(0);
        r.blocks(int(4.0 * 15.0 / bpm / kBlockS * 9));  // ~9 steps
        auto on = r.sink.of(Ev::NoteOn, 0);
        CHECK(on.size() >= 8);
        if (on.size() < 8) continue;
        // First micro-step runs on the 12th tick (tick counter starts at 1, spec 03 §1.4).
        CHECK_EQ(on[0].tick, 12u);
        for (size_t i = 1; i < on.size(); ++i) CHECK_EQ(on[i].tick - on[i - 1].tick, 576u);
        // Wall time per step = 15/BPM s (16th note), quantised to blocks.
        const double per = (on[7].time - on[0].time) / 7.0;
        CHECK_NEAR(per, 15.0 / bpm, kBlockS);
    }
}

TEST(swing_ratio) {
    Rig r(60.0f);
    for (int s = 0; s < 32; ++s) r.setNote(0, s, 60);
    r.fxA(0, 0, 17, 75);  // Swing 75
    r.seq.play(0);
    r.blocks(int(12 * 0.25 / kBlockS));
    CHECK(r.seq.swingActive());
    auto on = r.sink.of(Ev::NoteOn, 0);
    CHECK(on.size() >= 8);
    if (on.size() < 8) return;
    // Steps alternate (75+50)/100 and (25+50)/100 of the base 0.25 s.
    const double d23 = on[3].time - on[2].time;  // step 2 (phase 0): long
    const double d34 = on[4].time - on[3].time;  // step 3 (phase 1): short
    CHECK_NEAR(d23, 0.25 * 1.25, 2 * kBlockS);
    CHECK_NEAR(d34, 0.25 * 0.75, 2 * kBlockS);
    CHECK_NEAR(on[5].time - on[3].time, 0.5, 2 * kBlockS);  // pair keeps its length
    // Stop clears swing.
    r.seq.stop();
    CHECK(!r.seq.swingActive());
    CHECK_NEAR(r.seq.swingValue(), 50.0, 1e-6);
}

// --- FX timing ------------------------------------------------------------------

TEST(roll_timing_and_variation) {
    Rig r(120.0f);
    r.setNote(0, 0, 60);
    r.fxA(0, 0, 3, 4);           // Roll type R, rate idx 4 -> period 12 microsteps (4 hits/step)
    r.setNote(1, 0, 60);
    r.fxB(1, 0, 3, 4 * 9 + 2);   // Roll type N (+1 per hit), rate idx 2 -> period 24
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 2; });
    auto on = r.sink.of(Ev::NoteOn, 0);
    // Step 0: hits at microsteps 1,13,25,37; continues over empty step 1.
    CHECK_EQ(on.size(), 8u);
    for (size_t i = 1; i < on.size(); ++i) CHECK_EQ(on[i].tick - on[i - 1].tick, 12u * 12u);
    // Roll gate = rate/2 microsteps -> note-off 6 microsteps after each hit.
    auto off = r.sink.of(Ev::NoteOff, 0);
    CHECK(off.size() >= 7);
    if (off.size() >= 1 && on.size() >= 2) CHECK_EQ(off[0].tick - on[1].tick, 6u * 12u);
    auto on1 = r.sink.of(Ev::NoteOn, 1);
    CHECK_EQ(on1.size(), 4u);
    for (size_t i = 0; i < on1.size(); ++i) CHECK_EQ(on1[i].a, 60 + int(i));
    for (size_t i = 1; i < on1.size(); ++i) CHECK_EQ(on1[i].tick - on1[i - 1].tick, 24u * 12u);
}

TEST(roll_volume_ramp) {
    Rig r(120.0f);
    r.setNote(0, 0, 60);
    r.setNote(0, 1, 62);         // next note one step later -> distance 1
    r.fxA(0, 0, 3, 1 * 9 + 4);   // type v (falls), 4 hits/step
    r.fxB(0, 0, 18, 80);         // Volume 80
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 1 && r.seq.microStep(0) > 2; });
    auto on = r.sink.of(Ev::NoteOn, 0);
    CHECK_EQ(on.size(), 5u);
    if (on.size() < 5) return;
    // f = sinceTrigger/48: hits at sinceTrigger 1, 13, 25, 37.
    CHECK_EQ(on[0].c, int(80 * (1 - 1.0f / 48)));
    CHECK_EQ(on[1].c, int(80 * (1 - 13.0f / 48)));
    CHECK_EQ(on[3].c, int(80 * (1 - 37.0f / 48)));
    CHECK_EQ(on[4].a, 62);  // new note clears the roll
}

TEST(arp_up_with_chord) {
    Rig r(120.0f);
    r.setNote(0, 0, 60);
    r.fxB(0, 0, 14, 5);          // chord 0,4,7
    r.fxA(0, 0, 21, 9);          // arp up, rate idx 8 -> period 12 microsteps
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 1 && r.seq.microStep(0) > 2; });
    auto on = r.sink.of(Ev::NoteOn, 0);
    CHECK(on.size() >= 5);
    if (on.size() < 5) return;
    const int expect[] = {60, 64, 67, 60, 64};
    for (int i = 0; i < 5; ++i) CHECK_EQ(on[i].a, expect[i]);
    for (int i = 1; i < 5; ++i) CHECK_EQ(on[i].tick - on[i - 1].tick, 144u);
}

TEST(arp_down_and_needs_chord) {
    Rig r(120.0f);
    r.setNote(0, 0, 60);
    r.fxB(0, 0, 14, 5);
    r.fxA(0, 0, 21, 12 + 8);     // dir 1 (down), rate idx 8
    r.setNote(1, 0, 60);
    r.fxA(1, 0, 21, 9);          // arp without chord: nothing
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 1; });
    auto on = r.sink.of(Ev::NoteOn, 0);
    CHECK(on.size() >= 4u);
    const int expect[] = {60, 67, 64, 60};
    for (size_t i = 0; i < on.size() && i < 4; ++i) CHECK_EQ(on[i].a, expect[i]);
    CHECK_EQ(r.sink.of(Ev::NoteOn, 1).size(), 1u);
}

TEST(chance_probability) {
    Rig r(800.0f);
    for (int s = 0; s < 32; ++s) {
        r.setNote(0, s, 60);
        r.fxA(0, s, 4, 49);      // ~50 %
        r.setNote(1, s, 60);
        r.fxA(1, s, 4, 100);     // always
        r.setNote(2, s, 60);
        r.fxB(2, s, 4, 0);       // ~1 %
    }
    r.seq.seedRandom(12345);
    r.seq.play(0);
    r.runUntil([&] { return r.sink.of(Ev::NoteOn, 1).size() >= 2000; });
    const double n = double(r.sink.of(Ev::NoteOn, 1).size());
    const double p0 = r.sink.of(Ev::NoteOn, 0).size() / n;
    const double p2 = r.sink.of(Ev::NoteOn, 2).size() / n;
    CHECK_NEAR(p0, 0.50, 0.05);
    CHECK_NEAR(p2, 0.01, 0.01);
}

TEST(gate_length) {
    Rig r(120.0f);
    r.setNote(0, 0, 60);
    r.fxA(0, 0, 20, 50);         // gate = floor(50*47/100) = 23 microsteps
    r.setNote(1, 0, 60);
    r.fxA(1, 0, 20, 100);        // 47
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 1; });
    auto on = r.sink.of(Ev::NoteOn, 0);
    auto off = r.sink.of(Ev::NoteOff, 0);
    CHECK_EQ(on.size(), 1u);
    CHECK_EQ(off.size(), 1u);
    if (on.size() == 1 && off.size() == 1) {
        CHECK_EQ(off[0].tick - on[0].tick, 23u * 12u);
        CHECK_EQ(off[0].a, int(NoteOffMode::Off));
    }
    auto off1 = r.sink.of(Ev::NoteOff, 1);
    CHECK_EQ(off1.size(), 1u);
    if (off1.size() == 1) CHECK_EQ(off1[0].tick - on[0].tick, 47u * 12u);
}

TEST(micro_move_delay) {
    Rig r(120.0f);
    r.setNote(0, 0, 60);
    r.fxA(0, 0, 2, 50);          // delay = 1 + floor(51*47/100) = 24 microsteps
    r.setNote(1, 0, 60);
    r.fxA(1, 0, 2, 0);           // 1 microstep
    r.setNote(2, 0, 60);         // reference
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 1; });
    auto ref = r.sink.of(Ev::NoteOn, 2);
    auto d0 = r.sink.of(Ev::NoteOn, 0);
    auto d1 = r.sink.of(Ev::NoteOn, 1);
    CHECK(ref.size() == 1 && d0.size() == 1 && d1.size() == 1);
    if (ref.size() == 1 && d0.size() == 1 && d1.size() == 1) {
        CHECK_EQ(d0[0].tick - ref[0].tick, 24u * 12u);
        CHECK_EQ(d1[0].tick - ref[0].tick, 12u);
    }
}

TEST(random_fx_value) {
    Rig r(400.0f);
    for (int s = 0; s < 32; ++s) {
        r.setNote(0, s, 60);
        r.fxA(0, s, 18, 50);
        r.fxB(0, s, 16, 10);     // random +-10 on the Volume value
    }
    r.seq.seedRandom(7);
    r.seq.play(0);
    r.runUntil([&] { return r.sink.of(Ev::NoteOn, 0).size() >= 200; });
    int lo = 999, hi = -1;
    for (const auto& e : r.sink.of(Ev::NoteOn, 0)) {
        lo = e.c < lo ? e.c : lo;
        hi = e.c > hi ? e.c : hi;
        CHECK_EQ(e.fxAVal, e.c);   // the value replaces both columns
        CHECK_EQ(e.fxBVal, e.c);
    }
    CHECK(lo >= 40 && hi <= 60);
    CHECK(hi - lo >= 15);
}

TEST(random_note_and_volume) {
    Rig r(400.0f);
    for (int s = 0; s < 32; ++s) {
        r.setNote(0, s, 60);
        r.fxA(0, s, 5, 3);       // note 57..63
        r.setNote(1, s, 60);
        r.fxA(1, s, 7, 20);      // volume 0..20
    }
    r.seq.play(0);
    r.runUntil([&] { return r.sink.of(Ev::NoteOn, 0).size() >= 100; });
    for (const auto& e : r.sink.of(Ev::NoteOn, 0)) CHECK(e.a >= 57 && e.a <= 63);
    for (const auto& e : r.sink.of(Ev::NoteOn, 1)) CHECK(e.c >= 0 && e.c <= 20);
}

TEST(tempo_fx_override_and_stop) {
    Rig r(120.0f);
    r.setNote(0, 0, 60);
    r.fxA(0, 0, 15, 100);        // 200 BPM
    r.fxA(0, 2, 15, 4);          // STP at end of step 2
    r.seq.settings().transportOut = 1;
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 1; });
    CHECK(r.seq.tempoOverrideActive());
    CHECK_NEAR(r.seq.effectiveTempo(), 200.0, 1e-6);
    CHECK_NEAR(r.seq.tickPeriodUs(), 312500.0 / 200.0 / 12.0, 1e-3);
    r.runUntil([&] { return !r.seq.isPlaying(); }, 2000);
    CHECK(!r.seq.isPlaying());
    CHECK(!r.seq.tempoOverrideActive());
    CHECK_EQ(r.seq.step(0), 0);  // stop rewinds
    // Stopped at the last microstep of step 2: old tick 12 + (3*48-1)*12.
    CHECK_EQ(r.seq.tickCount(), uint64_t(12 + (3 * 48 - 1) * 12));
    CHECK_EQ(r.sink.midiStatus(0xFC).size(), 1u);
}

TEST(break_pattern) {
    Rig r(120.0f);
    r.setNote(0, 0, 60);
    r.fxA(5, 2, 13, 1);
    r.seq.play(0);
    r.runUntil([&] { return r.host.patternEnds == 2; });
    CHECK_EQ(r.host.endTicks.size(), 2u);
    if (r.host.endTicks.size() == 2) {
        // All tracks wrap at the end of step 2 (last microstep, old tick 12 + (3*48-1)*12).
        CHECK_EQ(r.host.endTicks[0], uint64_t(12 + (3 * 48 - 1) * 12));
        CHECK_EQ(r.host.endTicks[1], uint64_t(12 + (6 * 48 - 1) * 12));
    }
}

// --- song mode / pattern switching ------------------------------------------------

TEST(song_mode_switch_and_preload) {
    Rig r(120.0f);
    r.project.playlist[0] = 1;
    r.project.playlist[1] = 2;
    r.project.playlist[2] = 3;
    r.project.playlist[3] = 0;
    r.project.setCurrentPattern(1);
    r.seq.attach(&r.sink, &r.host, &r.project, &r.a, &r.b);
    r.host.loadPattern(1, r.a, true);
    r.host.loads.clear();
    r.seq.playSong(0);
    CHECK(r.seq.isSongMode());
    CHECK(r.host.loads.empty());  // pattern 1 is already current
    r.runUntil([&] { return r.host.patternEnds == 3; });
    // Preload requested one step early: at microstep 1 of the last step (3).
    CHECK(r.host.loads.size() >= 3);
    if (r.host.loads.size() >= 3) {
        CHECK_EQ(r.host.loads[0].num, 2);
        CHECK(!r.host.loads[0].sync);
        CHECK_EQ(r.host.loads[0].tick, uint64_t(12 + 3 * 576));
        CHECK_EQ(r.host.loads[1].num, 3);
        CHECK_EQ(r.host.loads[1].tick, uint64_t(12 + 7 * 576));
        CHECK_EQ(r.host.loads[2].num, 1);  // song loops
    }
    auto on = r.sink.of(Ev::NoteOn, 0);
    CHECK(on.size() >= 3);
    if (on.size() >= 3) {
        CHECK_EQ(on[0].a, 10);
        CHECK_EQ(on[1].a, 20);
        CHECK_EQ(on[1].tick, uint64_t(12 + 4 * 576));
        CHECK_EQ(on[2].a, 30);
    }
    CHECK_EQ(r.seq.songPosition(), 0);
    CHECK_EQ(r.seq.currentPatternNumber(), 1);
    CHECK_EQ(r.host.patternAdvances, 2);  // not called on the song wrap
}

// CPU round 3: a host that loads by track gets the preload spread over the
// blocks of the preload step (at most 1-2 tracks per block at <= 430 BPM)
// and the rest at the pattern end; the result is the whole-pattern load.
struct PartHost : FakeHost {
    std::vector<int> perBlock;   // tracks copied per block (filled by the test)
    int inBlock = 0, wholeLoads = 0;
    bool loadsPatternTracks() const override { return true; }
    static void fill(uint8_t number, Track& d, int t) {
        for (auto& st : d.steps) st = Step{};
        d.lastStep = 3;
        d.steps[t & 3].note = int8_t((number * 10 + t) % 128);
        d.steps[t & 3].instrument = uint8_t(t);
    }
    void loadPattern(uint8_t number, Pattern& dst, bool sync) override {
        FakeHost::loadPattern(number, dst, sync);
        for (int t = 0; t < Sequencer::kTracks; ++t) fill(number, dst.tracks[t], t);
        ++wholeLoads;
    }
    void loadPatternTracks(uint8_t number, Pattern& dst, int first, int count) override {
        for (int t = first; t < first + count; ++t) fill(number, dst.tracks[t], t);
        inBlock += count;
    }
};

TEST(song_mode_spread_preload) {
    for (float bpm : {120.0f, 400.0f, 1000.0f}) {
        Rig r(bpm);
        PartHost host;
        host.seq = &r.seq;
        r.project.playlist[0] = 1;
        r.project.playlist[1] = 2;
        r.project.playlist[2] = 0;
        r.project.setCurrentPattern(1);
        r.seq.attach(&r.sink, &host, &r.project, &r.a, &r.b);
        host.loadPattern(1, r.a, true);
        host.wholeLoads = 0;
        r.seq.playSong(0);
        int maxBlock = 0, total = 0;
        for (int i = 0; i < 20000 && host.patternEnds < 2; ++i) {
            host.inBlock = 0;
            r.block();
            maxBlock = host.inBlock > maxBlock ? host.inBlock : maxBlock;
            total += host.inBlock;
        }
        CHECK_EQ(host.patternEnds, 2);
        CHECK_EQ(host.wholeLoads, 0);           // no whole-pattern copy in an audio block
        CHECK_EQ(total, 2 * Sequencer::kTracks); // two preloads, every track once
        if (bpm <= 400.0f) CHECK(maxBlock <= 2);
        // Back at pattern 1 after the song wrap: the spread preload's tracks.
        CHECK_EQ(r.seq.currentPatternNumber(), 1);
        Pattern want;
        for (int t = 0; t < Sequencer::kTracks; ++t) PartHost::fill(1, want.tracks[t], t);
        CHECK(std::memcmp(r.seq.currentPattern(), &want, sizeof want) == 0);
        auto on = r.sink.of(Ev::NoteOn, 0);
        CHECK(on.size() >= 2);
        if (on.size() >= 2) CHECK_EQ(on[1].a, 20);   // pattern 2's track 0 played
    }
}

TEST(queued_pattern) {
    Rig r(120.0f);
    r.project.setCurrentPattern(4);
    r.seq.attach(&r.sink, &r.host, &r.project, &r.a, &r.b);
    r.host.loadPattern(4, r.a, true);
    r.host.loads.clear();
    r.seq.play(0);
    r.seq.queuePattern(+2);
    CHECK_EQ(r.seq.queuedPattern(), 6);
    r.runUntil([&] { return r.host.patternEnds == 1; });
    CHECK_EQ(r.host.loads.size(), 1u);
    if (!r.host.loads.empty()) CHECK_EQ(r.host.loads[0].num, 6);
    CHECK_EQ(r.seq.currentPatternNumber(), 6);
    CHECK_EQ(r.seq.queuedPattern(), 0);
    r.seq.queuePattern(-10);
    CHECK_EQ(r.seq.queuedPattern(), 1);
}

TEST(switch_pattern_now_while_playing_keeps_step) {
    // spec 16 §A.2 (FW:0x3d548): immediate switch, step position kept.
    Rig r(120.0f);
    struct Host16 : FakeHost {
        void loadPattern(uint8_t n, Pattern& dst, bool s) override {
            FakeHost::loadPattern(n, dst, s);
            for (auto& t : dst.tracks) t.lastStep = 15;
        }
    };
    Host16 host;
    host.seq = &r.seq;
    r.a.tracks[0].lastStep = 15;
    r.project.setCurrentPattern(4);
    r.seq.attach(&r.sink, &host, &r.project, &r.a, &r.b);
    r.seq.play(0);
    r.seq.queuePattern(+3);   // a Left/Right queue survives the switch
    r.runUntil([&] { return r.seq.step(0) == 5; });
    host.loads.clear();
    Pattern* before = r.seq.currentPattern();
    r.seq.switchPatternNow(9);
    CHECK_EQ(r.seq.pendingSwitch(), 9);
    CHECK_EQ(r.seq.currentPatternNumber(), 4);   // committed by the audio block
    r.block();
    CHECK_EQ(r.seq.pendingSwitch(), 0);
    CHECK_EQ(r.seq.currentPatternNumber(), 9);
    CHECK_EQ(int(r.project.currentPattern()), 9);
    CHECK(r.seq.currentPattern() != before);      // buffers swapped
    CHECK_EQ(host.loads.size(), 1u);
    if (!host.loads.empty()) CHECK_EQ(host.loads[0].num, 9);
    CHECK(r.seq.isPlaying());
    CHECK_EQ(r.seq.step(0), 5);                   // no restart
    CHECK_EQ(r.seq.queuedPattern(), 7);
    r.runUntil([&] { return host.patternEnds == 1; });
    CHECK_EQ(r.seq.currentPatternNumber(), 7);
}

TEST(switch_pattern_now_wraps_step_beyond_new_length) {
    Rig r(120.0f);   // FakeHost loads 4-step patterns
    r.a.tracks[0].lastStep = 15;
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 6; });
    r.seq.switchPatternNow(2);
    r.block();
    CHECK_EQ(r.seq.currentPatternNumber(), 2);
    CHECK_EQ(r.seq.step(0), 6 % 4);
    CHECK(r.seq.isPlaying());
}

TEST(switch_pattern_now_stopped_commits_at_once) {
    Rig r(120.0f);
    r.seq.switchPatternNow(12);
    CHECK_EQ(r.seq.currentPatternNumber(), 12);
    CHECK_EQ(r.seq.pendingSwitch(), 0);
    r.seq.switchPatternNow(0);     // out of range: ignored
    CHECK_EQ(r.seq.currentPatternNumber(), 12);
}

// --- OP-1: switchPatternNow with a stage buffer (no pattern copy in the audio block) ---

namespace {
// Host whose main-loop load fills the destination track by track and can run
// audio blocks in the middle of it (an audio interrupt preempting the
// main-loop preload). Loads made from inside a block are recorded apart.
struct StagedHost : FakeHost {
    Rig* rig = nullptr;
    bool inBlock = false;
    int blocksMidLoad = 0;       // audio blocks to run halfway through the next load
    std::vector<int> isrLoads;   // pattern numbers loaded from an audio block
    void loadPattern(uint8_t n, Pattern& dst, bool s) override {
        if (inBlock) {
            isrLoads.push_back(n);
            FakeHost::loadPattern(n, dst, s);
            return;
        }
        loads.push_back({n, s, 0});
        Pattern tmp;
        FakeHost tmpHost;
        tmpHost.loadPattern(n, tmp, s);
        for (auto& t : tmp.tracks) t.lastStep = 15;
        for (int t = 0; t < kTrackCount; ++t) {
            dst.tracks[t] = tmp.tracks[t];
            if (t == kTrackCount / 2)
                for (; blocksMidLoad > 0; --blocksMidLoad) block();
        }
    }
    void block() {
        inBlock = true;
        rig->block();
        inBlock = false;
    }
};
}  // namespace

TEST(switch_pattern_now_staged_swaps_pointers_in_the_audio_block) {
    Rig r(120.0f);
    Pattern stage;
    stage.setDefault();
    StagedHost host;
    host.seq = &r.seq;
    host.rig = &r;
    r.a.tracks[0].lastStep = 15;
    r.project.setCurrentPattern(4);
    r.seq.attach(&r.sink, &host, &r.project, &r.a, &r.b, &stage);
    CHECK(r.seq.stagePattern() == &stage);
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 5; });
    host.loads.clear();
    host.isrLoads.clear();
    Pattern* before = r.seq.currentPattern();
    r.seq.switchPatternNow(9);
    CHECK_EQ(host.loads.size(), 1u);            // loaded in the main loop
    CHECK_EQ(r.seq.pendingSwitch(), 9);
    CHECK_EQ(r.seq.currentPatternNumber(), 4);
    host.block();
    CHECK(host.isrLoads.empty());               // no copy in the audio block
    CHECK(r.seq.currentPattern() == &stage);    // swapped in
    CHECK(r.seq.stagePattern() == before);      // the old pattern is the next stage
    CHECK_EQ(r.seq.currentPatternNumber(), 9);
    CHECK_EQ(int(r.seq.currentPattern()->tracks[0].steps[0].note), 90);
    CHECK_EQ(r.seq.step(0), 5);                  // step kept
    // A re-attach without a stage keeps it (the Song screen re-points with two).
    r.seq.attach(&r.sink, &host, &r.project, r.seq.currentPattern(), r.seq.preloadPattern());
    CHECK(r.seq.stagePattern() == before);
    // Stopped: commits at once, still without an audio-block load.
    r.seq.stop();
    r.seq.switchPatternNow(3);
    CHECK_EQ(r.seq.currentPatternNumber(), 3);
    CHECK(r.seq.currentPattern() == before);
    CHECK(host.isrLoads.empty());
}

TEST(switch_pattern_now_audio_block_during_preload) {
    // An audio block that runs while the main loop is still assembling the
    // stage buffer must neither swap in the half-filled buffer nor commit the
    // request it replaces; the next block commits the complete pattern.
    Rig r(120.0f);
    Pattern stage;
    stage.setDefault();
    StagedHost host;
    host.seq = &r.seq;
    host.rig = &r;
    r.a.tracks[0].lastStep = 15;
    r.project.setCurrentPattern(4);
    r.seq.attach(&r.sink, &host, &r.project, &r.a, &r.b, &stage);
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 2; });
    Pattern* playing = r.seq.currentPattern();

    // 1. Blocks during the first preload: nothing pending yet.
    host.blocksMidLoad = 3;
    r.seq.switchPatternNow(9);
    CHECK(r.seq.currentPattern() == playing);
    CHECK_EQ(r.seq.currentPatternNumber(), 4);
    CHECK(host.isrLoads.empty());

    // 2. A second switch replaces the first before any block took it; blocks
    //    run halfway through its preload (the stage holds half of 11, half
    //    of 9): the first request must not commit.
    host.blocksMidLoad = 2;
    r.seq.switchPatternNow(11);
    CHECK(r.seq.currentPattern() == playing);
    CHECK_EQ(r.seq.currentPatternNumber(), 4);
    CHECK_EQ(r.seq.pendingSwitch(), 11);
    CHECK(host.isrLoads.empty());

    // 3. The next block commits 11, complete.
    host.block();
    CHECK(r.seq.currentPattern() == &stage);
    CHECK_EQ(r.seq.currentPatternNumber(), 11);
    CHECK(host.isrLoads.empty());
    const Pattern& p = *r.seq.currentPattern();
    CHECK_EQ(int(p.tracks[0].steps[0].note), 110);
    bool allTracks = true;
    for (const auto& t : p.tracks) allTracks &= t.lastStep == 15;
    CHECK(allTracks);

    // 4. Blocks during a preload that follows a committed switch: the stage
    //    is now the old pattern, the playing one stays untouched.
    host.blocksMidLoad = 2;
    r.seq.switchPatternNow(7);
    CHECK(r.seq.currentPattern() == &stage);
    CHECK_EQ(r.seq.currentPatternNumber(), 11);
    host.block();
    CHECK(r.seq.currentPattern() == playing);
    CHECK_EQ(r.seq.currentPatternNumber(), 7);
    CHECK_EQ(int(r.seq.currentPattern()->tracks[0].steps[0].note), 70);
    CHECK(host.isrLoads.empty());
    CHECK(r.seq.isPlaying());
}

// --- MIDI clock / transport ----------------------------------------------------------

TEST(midi_clock_pulses_per_step) {
    Rig r(120.0f);
    r.seq.settings().clockOut = 2;
    r.seq.settings().transportOut = 3;
    r.block();
    CHECK(r.sink.midiStatus(0xF8).empty());  // nothing while stopped
    r.seq.play(0);
    CHECK_EQ(r.sink.midiStatus(0xFA).size(), 1u);
    if (!r.sink.midiStatus(0xFA).empty()) CHECK_EQ(r.sink.midiStatus(0xFA)[0].ports, 3);
    r.runUntil([&] { return r.seq.step(0) == 4; });
    auto clk = r.sink.midiStatus(0xF8);
    CHECK_EQ(clk.size(), 24u);  // 6 per step
    for (size_t i = 1; i < clk.size(); ++i) CHECK_EQ(clk[i].tick - clk[i - 1].tick, 96u);
    if (!clk.empty()) CHECK_EQ(clk[0].ports, 2);
    r.seq.stop();
    CHECK_EQ(r.sink.midiStatus(0xFC).size(), 1u);
    const size_t n = r.sink.midiStatus(0xF8).size();
    r.blocks(100);
    CHECK_EQ(r.sink.midiStatus(0xF8).size(), n);
    // Rendering suppresses all MIDI.
    r.seq.setRendering(true);
    r.seq.play(0);
    r.blocks(100);
    CHECK_EQ(r.sink.midiStatus(0xFA).size(), 1u);
}

TEST(external_clock_sync) {
    Rig r(120.0f);
    r.seq.settings().clockIn = 2;      // MIDI jack
    r.seq.settings().transportIn = 2;
    r.setNote(0, 0, 60);
    r.setNote(0, 1, 61);
    r.seq.userPlay(0);
    CHECK(!r.seq.isPlaying());          // only arms
    r.seq.midiRealtimeIn(MidiPortId::kUsb, 0xFA);  // wrong port: ignored
    CHECK(!r.seq.isPlaying());
    r.seq.midiRealtimeIn(MidiPortId::kDin, 0xFA);
    CHECK(r.seq.isPlaying());
    // First pulse after Start only aligns.
    r.seq.midiRealtimeIn(MidiPortId::kDin, 0xF8);
    r.block();
    CHECK_EQ(r.seq.tickCount(), 0u);
    for (int i = 0; i < 6; ++i) {
        r.seq.midiRealtimeIn(MidiPortId::kDin, 0xF8);
        r.block();
        CHECK_EQ(r.seq.tickCount(), uint64_t(96 * (i + 1)));
    }
    // 6 pulses = 1 step; note 2 starts on the next pulse.
    CHECK_EQ(r.sink.of(Ev::NoteOn, 0).size(), 1u);
    r.seq.midiRealtimeIn(MidiPortId::kDin, 0xF8);
    r.seq.midiRealtimeIn(MidiPortId::kUsb, 0xF8);  // wrong port
    r.block();
    CHECK_EQ(r.seq.tickCount(), uint64_t(96 * 7));
    CHECK_EQ(r.sink.of(Ev::NoteOn, 0).size(), 2u);
    // Stop via MIDI.
    r.seq.midiRealtimeIn(MidiPortId::kDin, 0xFC);
    CHECK(!r.seq.isPlaying());
}

TEST(external_clock_correction) {
    Rig r(120.0f);
    r.seq.settings().clockIn = 1;
    r.seq.settings().transportIn = 1;
    r.seq.settings().clockSyncCorrection = 8;   // +2 pulses
    r.seq.midiRealtimeIn(MidiPortId::kUsb, 0xFA);
    r.block();
    CHECK_EQ(r.seq.tickCount(), 96u);  // jumps ahead at once: 2 + (-1 align)
    r.seq.midiRealtimeIn(MidiPortId::kUsb, 0xF8);
    r.block();
    CHECK_EQ(r.seq.tickCount(), 192u);  // 2 pulses ahead of an uncorrected start
    // Negative: hold back.
    Rig q(120.0f);
    q.seq.settings().clockIn = 1;
    q.seq.settings().transportIn = 1;
    q.seq.settings().clockSyncCorrection = 4;   // -2 pulses
    q.seq.midiRealtimeIn(MidiPortId::kUsb, 0xFA);
    for (int i = 0; i < 3; ++i) {
        q.seq.midiRealtimeIn(MidiPortId::kUsb, 0xF8);
        q.block();
    }
    CHECK_EQ(q.seq.tickCount(), 0u);  // 1 align + 2 held back
    q.seq.midiRealtimeIn(MidiPortId::kUsb, 0xF8);
    q.block();
    CHECK_EQ(q.seq.tickCount(), 96u);
}

TEST(external_tempo_estimate_and_no_clock) {
    Rig r(120.0f);
    r.seq.settings().clockIn = 2;
    r.seq.settings().transportIn = 2;
    CHECK_NEAR(r.seq.externalTempo(), 130.0, 1e-6);  // boot default
    r.seq.midiRealtimeIn(MidiPortId::kDin, 0xFA);
    const double bpm = 140.0;
    const double pulseS = 60.0 / bpm / 24.0;
    double next = 0;
    for (int i = 0; i < 2000; ++i) {
        while (r.sink.time >= next) {
            r.seq.midiRealtimeIn(MidiPortId::kDin, 0xF8);
            next += pulseS;
        }
        r.block();
    }
    CHECK_NEAR(r.seq.externalTempo(), bpm, 1.0);
    CHECK_NEAR(r.seq.effectiveTempo(), bpm, 1.0);
    CHECK_NEAR(r.sink.lastTempo, bpm, 1.0);
    // Ticks follow pulses (96 per pulse), one align pulse lost.
    const uint64_t pulses = uint64_t(next / pulseS + 0.5);
    CHECK_EQ(r.seq.tickCount(), (pulses - 1) * 96);
    // No pulse for > 1 s -> NO CLK.
    r.blocks(int(1.1 / kBlockS));
    CHECK(r.seq.externalClockLost());
}

// --- MIDI note output -------------------------------------------------------------

TEST(midi_note_output) {
    Rig r(120.0f);
    r.setNote(8, 0, 60, 48);                 // track 9, MIDI ch 1
    r.setNote(8, 1, 62, 48);
    r.fxA(8, 1, 18, 50);                     // velocity 50*127/100 = 63
    r.setNote(9, 0, 64, 50);                 // ch 3, chord 0,4,7
    r.fxB(9, 0, 14, 5);
    r.setNote(10, 0, 60, 0);                 // sample instr on track 11: dropped
    r.setNote(10, 1, kNoteOff, 0);
    r.setNote(9, 2, kNoteCut, 50);
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 3; });
    auto ons = r.sink.midiNoteOns();
    CHECK_EQ(ons.size(), 5u);
    if (ons.size() == 5) {
        CHECK_EQ(ons[0].a, 0x90); CHECK_EQ(ons[0].b, 60); CHECK_EQ(ons[0].c, 127);
        CHECK_EQ(ons[1].a, 0x92); CHECK_EQ(ons[1].b, 64);
        CHECK_EQ(ons[2].b, 68);
        CHECK_EQ(ons[3].b, 71);
        CHECK_EQ(ons[4].a, 0x90); CHECK_EQ(ons[4].b, 62); CHECK_EQ(ons[4].c, 63);
    }
    // Monophonic per track: note-off for 60 before 62; chord released by CUT.
    auto offs = r.sink.midiStatus(0x80);
    CHECK_EQ(offs.size(), 1u);
    if (!offs.empty()) { CHECK_EQ(offs[0].b, 60); CHECK_EQ(offs[0].c, 0); }
    CHECK_EQ(r.sink.midiStatus(0x82).size(), 3u);
    CHECK(r.sink.of(Ev::NoteOn, 10).empty());
    // Middle C shifts the sent note.
    Rig m(120.0f);
    m.seq.settings().middleC = 1;
    m.setNote(8, 0, 60, 48);
    m.seq.play(0);
    m.runUntil([&] { return m.seq.step(0) == 1; });
    auto mo = m.sink.midiNoteOns();
    CHECK_EQ(mo.size(), 1u);
    if (!mo.empty()) CHECK_EQ(mo[0].b, 72);
    // Stop releases held MIDI notes.
    m.seq.stop();
    CHECK_EQ(m.sink.midiStatus(0x80).size(), 1u);
}

TEST(midi_cc_and_pitch_bend) {
    Rig r(120.0f);
    r.setNote(8, 0, 60, 49);                 // ch 2
    r.fxA(8, 0, 8, 100);                     // CC A -> cc 1
    r.fxB(8, 0, 34, 99);                     // centre bend
    r.setNote(9, 0, kNoteEmpty, 48);         // empty step, MIDI instr byte
    r.fxA(9, 0, 37, 5);                      // CC F -> 128 = Program Change
    r.fxB(9, 0, 34, 198);                    // +4000
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 1; });
    auto cc = r.sink.midiStatus(0xB1);
    CHECK_EQ(cc.size(), 1u);
    if (!cc.empty()) { CHECK_EQ(cc[0].b, 1); CHECK_EQ(cc[0].c, 100); }
    auto pb = r.sink.midiStatus(0xE1);
    CHECK_EQ(pb.size(), 1u);
    if (!pb.empty()) { CHECK_EQ(pb[0].b, 0); CHECK_EQ(pb[0].c, 0x40); }
    auto pc = r.sink.midiStatus(0xC0);
    CHECK_EQ(pc.size(), 1u);
    if (!pc.empty()) { CHECK_EQ(pc[0].b, 5); CHECK_EQ(pc[0].len, 2); }
    auto pb2 = r.sink.midiStatus(0xE0);
    CHECK_EQ(pb2.size(), 1u);
    if (!pb2.empty()) CHECK_EQ(pb2[0].b | (pb2[0].c << 7), 8192 + 4000);
}

TEST(empty_step_apply_fx_and_off) {
    Rig r(120.0f);
    r.setNote(0, 0, 60);
    r.fxA(0, 1, 27, 40);                     // LP cutoff on empty step -> slot 0
    r.fxB(0, 1, 18, 70);                     // volume -> slot 1
    r.fxB(0, 2, 1, 0);                       // Off FX -> undo slot 1
    r.fxA(0, 2, 20, 10);                     // gate: consumed, not forwarded
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 3; });
    auto fx = r.sink.of(Ev::ApplyFx, 0);
    CHECK_EQ(fx.size(), 3u);
    if (fx.size() == 3) {
        CHECK_EQ(fx[0].a, 1); CHECK_EQ(fx[0].b, 18); CHECK_EQ(fx[0].c, 70);  // FX-B first
        CHECK_EQ(fx[1].a, 0); CHECK_EQ(fx[1].b, 27); CHECK_EQ(fx[1].c, 40);
        CHECK_EQ(fx[2].a, 1); CHECK_EQ(fx[2].b, 0);
    }
}

TEST(mute_blocks_notes) {
    Rig r(120.0f);
    r.setNote(0, 0, 60);
    r.setNote(9, 0, 60, 48);
    r.project.setU8(0x12C + 0, 1);           // mute track 1
    r.project.setU8(0x742 + 1, 1);           // mute track 10
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 2; });
    CHECK(r.sink.of(Ev::NoteOn, 0).empty());
    CHECK(r.sink.midiNoteOns().empty());
    CHECK_EQ(r.seq.step(0), 2);              // muted tracks still advance
}

// --- recording --------------------------------------------------------------------

TEST(record_quantised) {
    Rig r(40.0f);
    r.seq.record();
    CHECK(r.seq.isPlaying());
    CHECK(r.seq.isRecording());
    // Early in step 2 -> step 2.
    r.runUntil([&] { return r.seq.step(0) == 2 && r.seq.microStep(0) >= 10; });
    CHECK_EQ(r.seq.liveNoteOn(60, 127, 0, 3), 0);
    CHECK_EQ(int(r.step(0, 2).note), 60);
    CHECK_EQ(int(r.step(0, 2).instrument), 3);
    CHECK_EQ(int(r.step(0, 2).fx[1].type), 0);
    // Polyphonic input moves to the next free track.
    CHECK_EQ(r.seq.liveNoteOn(64, 127, 0, 3), 1);
    CHECK_EQ(int(r.step(1, 2).note), 64);
    r.seq.liveNoteOff(60);
    CHECK_EQ(int(r.step(0, 3).note), int(kNoteOff));  // release at same step -> next step
    // USER_REPORTS_R3 R3-2: the OFF carries the played note's instrument, not 0.
    CHECK_EQ(int(r.step(0, 3).instrument), 3);
    // Late in step 5 -> step 6.
    r.runUntil([&] { return r.seq.step(0) == 5 && r.seq.microStep(0) >= 30; });
    CHECK_EQ(r.seq.liveNoteOn(67, 127, 0, 3), 0);
    CHECK_EQ(int(r.step(0, 6).note), 67);
    CHECK(r.host.dirty >= 3);
    // The live-played note is not re-triggered when the sequencer reaches step 6.
    const size_t before = r.sink.of(Ev::NoteOn, 0).size();
    r.runUntil([&] { return r.seq.step(0) == 7; });
    CHECK_EQ(r.sink.of(Ev::NoteOn, 0).size(), before);
    // Next loop it plays.
    r.runUntil([&] { return r.seq.step(0) == 7 && r.host.patternEnds == 1; });
    CHECK(r.sink.of(Ev::NoteOn, 0).size() > before);
}

TEST(record_note_just_before_the_count_in_ends_lands_on_step_0) {
    // USER_REPORTS_R3 R3-1: with Pre-roll, a note meant for the first beat
    // that comes a little early (inside the count-in's last half step) was
    // played but not recorded. It now lands on step 0 (the same rounding as
    // "late in a step -> the next step") and is not triggered again there.
    Rig r(40.0f);
    r.seq.settings().preRoll = 1;
    r.seq.record();
    CHECK(r.seq.isCountingIn());
    // Early in the count-in: played only, not recorded.
    r.runUntil([&] { return r.seq.countInTicksLeft() <= 3u * seq::kTicksPerStep; });
    CHECK_EQ(r.seq.liveNoteOn(55, 127, 0, 2), 0);
    r.seq.liveNoteOff(55);
    for (int s = 0; s < 16; ++s) CHECK_EQ(int(r.step(0, s).note), int(kNoteEmpty));
    // A quarter step before the downbeat: recorded on step 0.
    r.runUntil([&] { return r.seq.countInTicksLeft() <= seq::kTicksPerStep / 4; });
    CHECK(!r.seq.isPlaying());
    CHECK_EQ(r.seq.liveNoteOn(60, 127, 0, 2), 0);
    CHECK_EQ(int(r.step(0, 0).note), 60);
    CHECK_EQ(int(r.step(0, 0).instrument), 2);
    const size_t ons = r.sink.of(Ev::NoteOn, 0).size();
    r.runUntil([&] { return r.seq.isPlaying() && r.seq.step(0) == 2; });
    CHECK_EQ(r.sink.of(Ev::NoteOn, 0).size(), ons);   // step 0 did not re-trigger it
    r.seq.liveNoteOff(60);
    CHECK_EQ(int(r.step(0, 2).note), int(kNoteOff));
}

TEST(record_microtiming_and_velocity) {
    Rig r(40.0f);
    r.seq.settings().recordingOptions = 3;
    r.seq.record();
    r.runUntil([&] { return r.seq.step(0) == 1 && r.seq.microStep(0) >= 30; });
    const int master = r.seq.masterMicroStep();
    CHECK_EQ(r.seq.liveNoteOn(60, 100, 0, 0, 100), 0);
    CHECK_EQ(int(r.step(0, 1).note), 60);                  // always current step
    CHECK_EQ(int(r.step(0, 1).fx[1].type), 2);             // FX-A micro-move
    CHECK_EQ(int(r.step(0, 1).fx[1].value), (master - 1) * 100 / 47);
    CHECK_EQ(int(r.step(0, 1).fx[0].type), 18);            // FX-B volume
    const int vol = 100 * 100 / 127;
    CHECK_EQ(int(r.step(0, 1).fx[0].value), (vol * 100 + 63) / 127);
}

TEST(record_external_arms) {
    Rig r(120.0f);
    r.seq.settings().clockIn = 2;
    r.seq.settings().transportIn = 2;
    r.seq.record();
    CHECK(r.seq.isArmed());
    CHECK(!r.seq.isPlaying());
    r.seq.midiRealtimeIn(MidiPortId::kDin, 0xFA);
    CHECK(r.seq.isPlaying());
    CHECK(r.seq.isRecording());
}

TEST(count_in) {
    Rig r(120.0f);
    r.seq.settings().preRoll = 1;
    r.seq.settings().tsNum = 3;
    r.seq.settings().tsDen = 3;
    r.seq.record();
    CHECK(!r.seq.isPlaying());
    CHECK(r.seq.isCountingIn());
    r.runUntil([&] { return r.seq.isPlaying(); });
    auto clicks = r.sink.of(Ev::Click);
    CHECK_EQ(clicks.size(), 4u);  // 16 steps: beats every 4 steps
    if (clicks.size() == 4) {
        CHECK_EQ(clicks[0].a, 1);
        CHECK_EQ(clicks[0].b, 100);
        CHECK_EQ(clicks[1].a, 0);
        CHECK_EQ(clicks[1].b, 45);
    }
    CHECK(r.seq.isRecording());
}

// spec 07 Q10: never during plain playback, during recording with the
// metronome on.
TEST(metronome_only_when_recording) {
    Rig r(120.0f);
    r.seq.settings().metronome = 1;
    r.seq.userPlay(0);
    r.blocks(2000);
    CHECK(r.sink.of(Ev::Click).empty());
    r.seq.stop();
    Rig q(120.0f);
    q.seq.settings().metronome = 1;
    q.seq.record();
    q.blocks(2000);
    CHECK(!q.sink.of(Ev::Click).empty());
    Rig o(120.0f);   // metronome off: recording stays silent
    o.seq.record();
    o.blocks(2000);
    CHECK(o.sink.of(Ev::Click).empty());
}

TEST(step_entry) {
    Rig r(120.0f);
    r.seq.stepEntry(60, 127, 5, 9, 2);       // sample instr on MIDI track -> 48
    CHECK_EQ(int(r.step(9, 5).note), 60);
    CHECK_EQ(int(r.step(9, 5).instrument), 48);
    CHECK_EQ(r.sink.midiNoteOns().size(), 1u);
    r.seq.stepEntry(62, 127, 5, 9, 7);       // occupied: instrument kept
    CHECK_EQ(int(r.step(9, 5).instrument), 48);
}

TEST(selection_play) {
    Rig r(120.0f);
    r.seq.settings().clockOut = 1;
    for (int s = 0; s < 8; ++s) { r.setNote(0, s, 60 + s); r.setNote(1, s, 40); }
    r.seq.playSelection(2, 4, 0, 0, false);
    r.runUntil([&] { return !r.seq.isPlaying(); }, 5000);
    auto on = r.sink.of(Ev::NoteOn, 0);
    CHECK_EQ(on.size(), 3u);
    if (on.size() == 3) { CHECK_EQ(on[0].a, 62); CHECK_EQ(on[2].a, 64); }
    CHECK(r.sink.of(Ev::NoteOn, 1).empty());
    CHECK(r.sink.midiStatus(0xF8).empty());   // no clock in non-looping selection
    // Looping selection wraps to its start.
    Rig l(120.0f);
    for (int s = 0; s < 8; ++s) l.setNote(0, s, 60 + s);
    l.seq.playSelection(2, 3, 0, 0, true);
    l.runUntil([&] { return l.sink.of(Ev::NoteOn, 0).size() >= 5; });
    auto lo = l.sink.of(Ev::NoteOn, 0);
    const int expect[] = {62, 63, 62, 63, 62};
    for (int i = 0; i < 5 && i < int(lo.size()); ++i) CHECK_EQ(lo[i].a, expect[i]);
}

TEST(performance_backward_and_transpose) {
    Rig r(120.0f);
    for (int s = 0; s < 4; ++s) r.setNote(0, s, 60 + s, 48);
    for (auto& t : r.seq.currentPattern()->tracks) t.lastStep = 3;
    r.seq.setPerformanceMode(true);
    r.seq.setPerfPlayMode(0, 1);
    r.seq.setPerfPlayMode(8, 1);   // 2.x: play modes apply to all 16 tracks (M1-WP4)
    r.seq.setPerfTranspose(0, true, 12);
    r.seq.setPerfVelocityOffset(0, -50);
    r.seq.play(0);
    r.runUntil([&] { return r.sink.midiNoteOns().size() >= 5; });
    CHECK_EQ(r.seq.step(8), r.seq.step(0));   // backward like track 0 (stock: forward)
    auto on = r.sink.midiNoteOns();
    // Backward from the last step.
    const int expect[] = {75, 74, 73, 72, 75};
    for (int i = 0; i < 5 && i < int(on.size()); ++i) CHECK_EQ(on[i].b, expect[i]);
    if (!on.empty()) CHECK_EQ(on[0].c, 127 - 63);
}

TEST(stop_rewinds_and_resets) {
    Rig r(120.0f);
    r.setNote(0, 0, 60);
    r.seq.play(5);
    r.runUntil([&] { return r.seq.step(0) == 7; });
    r.seq.stop();
    CHECK(!r.seq.isPlaying());
    CHECK_EQ(r.seq.step(0), 0);
    CHECK_EQ(r.seq.microStep(0), 1);
    auto offs = r.sink.of(Ev::NoteOff);
    CHECK_EQ(offs.size(), 8u);  // note-off on all sample voices
    CHECK(r.sink.blockUpdates > 0);
}

TEST(micro_move_plus_midi_offset) {
    // spec 03 §1.5 / 13 §I.1: 1 + floor((m+1)·47/100) + (floor((v+1)·45/100) + 1).
    struct Case { int m; int v; uint32_t expect; } cases[] = {
        {50, 1, 1 + 23 + 1},      // v=1: inner floor is 0, the +1 still applies
        {0, 100, 1 + 0 + 46},
        {-1, 1, 1},               // offset alone, v=1 -> 1 µstep
        {-1, 100, 46},
        {100, 100, 94},           // 1+47+46 (< cap 96)
    };
    for (const Case& c : cases) {
        Rig r(120.0f);
        MidiInstrument mi = r.project.midiInstrument(0);
        mi.extra[0] = uint8_t(c.v);
        r.project.setMidiInstrument(0, mi);
        r.setNote(8, 0, 60, 48);
        if (c.m >= 0) r.fxA(8, 0, 2, c.m);
        r.setNote(0, 0, 60);          // reference
        r.seq.play(0);
        r.runUntil([&] { return r.seq.step(0) == 2; });
        auto ref = r.sink.of(Ev::NoteOn, 0);
        auto on = r.sink.midiNoteOns();
        CHECK(ref.size() == 1 && on.size() == 1);
        if (ref.size() == 1 && on.size() == 1) CHECK_EQ(on[0].tick - ref[0].tick, c.expect * 12u);
    }
}

TEST(set_recording_keeps_transport) {
    Rig r(40.0f);
    r.seq.settings().transportOut = 1;
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 2; });
    const uint64_t t0 = r.seq.tickCount();
    // Punch in: recording on, playback continues from where it is.
    r.seq.setRecording(true);
    CHECK(r.seq.isRecording());
    CHECK(r.seq.isPlaying());
    CHECK_EQ(r.seq.step(0), 2);
    CHECK_EQ(r.seq.tickCount(), t0);
    CHECK_EQ(r.sink.midiStatus(0xFA).size(), 1u);  // no new Start
    r.runUntil([&] { return r.seq.step(0) == 3 && r.seq.microStep(0) >= 5; });
    CHECK_EQ(r.seq.liveNoteOn(60, 127, 0, 3), 0);
    CHECK_EQ(int(r.step(0, 3).note), 60);
    // Rec while recording: stop recording, keep playing.
    r.seq.setRecording(false);
    CHECK(!r.seq.isRecording());
    CHECK(r.seq.isPlaying());
    CHECK_EQ(r.seq.step(0), 3);
    CHECK(r.sink.midiStatus(0xFC).empty());
    r.seq.liveNoteOn(62, 127, 0, 3);                 // played live, not written
    CHECK_EQ(int(r.step(0, 3).note), 60);
    CHECK_EQ(int(r.step(1, 3).note), int(kNoteEmpty));
}

TEST(selection_play_stops_after_selected_rows) {
    // Selection not starting at step 0 plays its rows exactly once (README deviation).
    Rig r(120.0f);
    for (int s = 0; s < 16; ++s) r.setNote(0, s, 60 + s);
    r.seq.playSelection(5, 7, 0, 0, false);
    r.runUntil([&] { return !r.seq.isPlaying(); }, 5000);
    auto on = r.sink.of(Ev::NoteOn, 0);
    CHECK_EQ(on.size(), 3u);
    if (on.size() == 3) { CHECK_EQ(on[0].a, 65); CHECK_EQ(on[1].a, 66); CHECK_EQ(on[2].a, 67); }
    // Single-row selection.
    Rig s1(120.0f);
    s1.setNote(0, 9, 70);
    s1.seq.playSelection(9, 9, 0, 0, false);
    s1.runUntil([&] { return !s1.seq.isPlaying(); }, 5000);
    CHECK_EQ(s1.sink.of(Ev::NoteOn, 0).size(), 1u);
}

// --- 2.x: tracks 13..16 (docs/M1_PLAN.md §3, spec 2x-01 §6, spec 21 §3.2) ---------

TEST(tracks16_sixteen_tracks) {
    CHECK_EQ(Sequencer::kTracks, 16);
    CHECK_EQ(Sequencer::kVoiceTracks, 8);
}

TEST(tracks16_track13_midi_channel_cc_bend) {
    Rig r(120.0f);
    r.setNote(12, 0, 60, 52);                // track 13, MIDI 5 -> ch 5 (instr - 47)
    r.fxA(12, 0, 8, 100);                    // CC A -> cc 1 on ch 5
    r.fxB(12, 0, 34, 198);                   // pitch bend +4000 on ch 5
    r.setNote(12, 1, 62, 52);
    r.fxA(12, 1, 18, 50);                    // velocity 63
    r.setNote(15, 0, 64, 63);                // track 16, MIDI 16 -> ch 16
    r.setNote(13, 0, 60, 0);                 // sample instr on track 14: dropped
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 2; });
    auto ch5 = r.sink.midiStatus(0x94);
    CHECK_EQ(ch5.size(), 2u);
    if (ch5.size() == 2) {
        CHECK_EQ(ch5[0].b, 60); CHECK_EQ(ch5[0].c, 127);
        CHECK_EQ(ch5[1].b, 62); CHECK_EQ(ch5[1].c, 63);
    }
    auto ch16 = r.sink.midiStatus(0x9F);
    CHECK_EQ(ch16.size(), 1u);
    if (!ch16.empty()) CHECK_EQ(ch16[0].b, 64);
    auto cc = r.sink.midiStatus(0xB4);
    CHECK_EQ(cc.size(), 1u);
    if (!cc.empty()) { CHECK_EQ(cc[0].b, 1); CHECK_EQ(cc[0].c, 100); }
    auto pb = r.sink.midiStatus(0xE4);
    CHECK_EQ(pb.size(), 1u);
    if (!pb.empty()) CHECK_EQ(pb[0].b | (pb[0].c << 7), 8192 + 4000);
    CHECK_EQ(r.sink.midiStatus(0x84).size(), 1u);   // monophonic: 60 off before 62
    CHECK(r.sink.of(Ev::NoteOn, 13).empty());
    CHECK_EQ(r.sink.midiNoteOns().size(), 3u);
}

TEST(tracks16_stop_sends_note_off_on_tracks_13_16) {
    Rig r(120.0f);
    r.setNote(12, 0, 60, 48);
    r.setNote(15, 0, 67, 50);
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 1; });
    CHECK_EQ(r.sink.midiNoteOns().size(), 2u);
    CHECK(r.sink.midiStatus(0x80).empty());
    r.seq.stop();
    auto o0 = r.sink.midiStatus(0x80);
    auto o2 = r.sink.midiStatus(0x82);
    CHECK_EQ(o0.size(), 1u);
    CHECK_EQ(o2.size(), 1u);
    if (!o0.empty()) CHECK_EQ(o0[0].b, 60);
    if (!o2.empty()) CHECK_EQ(o2[0].b, 67);
}

TEST(mute_tracks_13_16_from_project_mt) {
    // Tracker Mini: mutes of tracks 13..16 are project.mt 0x742[4..7]
    // (MINI_PLAN U13), read through Project::trackMuted.
    Rig r(120.0f);
    r.setNote(12, 0, 60, 48);                // held (no note-off in the pattern)
    r.setNote(12, 2, 62, 48);
    r.setNote(14, 0, 64, 49);
    r.project.setTrackMuted(14, true);       // track 15 muted from the start
    CHECK_EQ(r.project.u8(0x742 + 6), 1);
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 1; });
    CHECK_EQ(r.sink.midiNoteOns().size(), 1u);
    CHECK(r.sink.midiStatus(0x91).empty());
    // Muting a track with a held note releases it on the next microstep.
    r.project.setU8(0x742 + 4, 1);
    r.block();
    auto off = r.sink.midiStatus(0x80);
    CHECK_EQ(off.size(), 1u);
    if (!off.empty()) CHECK_EQ(off[0].b, 60);
    r.runUntil([&] { return r.seq.step(0) == 3; });
    CHECK_EQ(r.sink.midiNoteOns().size(), 1u);  // step 2 is blocked
    CHECK_EQ(r.seq.step(12), 3);                // muted tracks still advance
    // Unmuted again: tracks 13 and 15 play.
    r.project.setTrackMuted(12, false);
    r.project.setTrackMuted(14, false);
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 1; });
    CHECK_EQ(r.sink.midiStatus(0x90).size(), 2u);
    CHECK_EQ(r.sink.midiStatus(0x91).size(), 1u);
}

TEST(tracks16_perf_transpose_velocity_track13) {
    Rig r(120.0f);
    r.setNote(12, 0, 60, 48);
    r.seq.setPerformanceMode(true);
    r.seq.setPerfTranspose(12, true, 7);
    r.seq.setPerfVelocityOffset(12, -50);
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 1; });
    auto on = r.sink.midiNoteOns();
    CHECK_EQ(on.size(), 1u);
    if (!on.empty()) { CHECK_EQ(on[0].b, 67); CHECK_EQ(on[0].c, 127 - 63); }
    // Leaving performance mode clears them.
    r.seq.setPerformanceMode(false);
    r.seq.stop();
    r.seq.play(0);
    r.runUntil([&] { return r.seq.step(0) == 1; });
    on = r.sink.midiNoteOns();
    CHECK_EQ(on.size(), 2u);
    if (on.size() == 2) { CHECK_EQ(on[1].b, 60); CHECK_EQ(on[1].c, 127); }
}

TEST(tracks16_live_record_tracks_13_16) {
    Rig r(40.0f);
    r.seq.record();
    r.runUntil([&] { return r.seq.step(0) == 2 && r.seq.microStep(0) >= 10; });
    r.seq.setRecordEnabled(13, false);
    CHECK_EQ(r.seq.liveNoteOn(60, 127, 12, 3), 12);   // sample instr -> MIDI 1 on 9..16
    CHECK_EQ(int(r.step(12, 2).note), 60);
    CHECK_EQ(int(r.step(12, 2).instrument), 48);
    CHECK_EQ(r.seq.liveNoteOn(62, 127, 12, 50), 14);  // 13 busy, 14 disarmed
    CHECK_EQ(r.seq.liveNoteOn(64, 127, 15, 50), 15);
    CHECK_EQ(r.seq.liveNoteOn(65, 127, 15, 50), -1);  // nothing beyond track 16
    CHECK_EQ(r.seq.liveNoteOn(66, 127, 16, 50), -1);
    r.seq.liveNoteOff(64);
    CHECK_EQ(int(r.step(15, 3).note), int(kNoteOff));
    // Selection play covers track 16.
    Rig s(120.0f);
    s.setNote(15, 0, 60, 48);
    s.setNote(0, 0, 60, 0);
    s.seq.playSelection(0, 0, 15, 15, false);
    s.runUntil([&] { return !s.seq.isPlaying(); }, 5000);
    CHECK_EQ(s.sink.midiNoteOns().size(), 1u);
    CHECK(s.sink.of(Ev::NoteOn, 0).empty());
}

// The pattern end follows a master position at the base length: a perf
// length / play mode on track 16 (the last processed track) cannot move the
// song advance (docs/M1_PLAN.md §3 risk).
TEST(tracks16_track16_perf_length_keeps_song_advance) {
    // mode 0: reference; 1: song mode, track 16 backward + 2-step length set
    // after the start; 2: pattern mode, track 16 7 steps long.
    auto run = [](int mode, bool perf, std::vector<uint64_t>& ends, std::vector<Load>& loads, int& maxStep15) {
        Rig r(120.0f);
        for (int i = 0; i < 4; ++i) r.project.playlist[i] = uint8_t(i + 1);
        r.project.playlist[4] = 0;
        r.project.setCurrentPattern(1);
        r.seq.attach(&r.sink, &r.host, &r.project, &r.a, &r.b);
        r.host.loadPattern(1, r.a, true);          // 4-step patterns
        r.host.loads.clear();
        if (perf) {
            r.seq.setPerformanceMode(true);
            if (mode == 1) r.seq.setPerfPlayMode(15, seq::kPlayBack);
        }
        if (mode == 1) r.seq.playSong(0);
        else r.seq.play(0);
        if (perf) r.seq.setPerfTrackLength(15, mode == 1 ? 1 : 6);
        maxStep15 = 0;
        r.runUntil([&] {
            if (r.seq.step(15) > maxStep15) maxStep15 = r.seq.step(15);
            return r.host.patternEnds >= 5;
        });
        ends = r.host.endTicks;
        loads = r.host.loads;
    };
    for (int mode : {1, 2}) {
        std::vector<uint64_t> e0, e1;
        std::vector<Load> l0, l1;
        int m0 = 0, m1 = 0;
        run(mode, false, e0, l0, m0);
        run(mode, true, e1, l1, m1);
        CHECK_EQ(e0.size(), 5u);
        CHECK(e0 == e1);
        CHECK_EQ(l0.size(), l1.size());
        for (size_t i = 0; i < l0.size() && i < l1.size(); ++i) {
            CHECK_EQ(l0[i].num, l1[i].num);
            CHECK_EQ(l0[i].tick, l1[i].tick);
            CHECK_EQ(l0[i].sync, l1[i].sync);
        }
        CHECK_EQ(m0, 3);
        if (mode == 2) CHECK_EQ(m1, 6);           // track 16 really played its own length
        if (mode == 1) CHECK_EQ(l1.size(), 5u);   // one preload per song step
    }
    // Pattern end tick with a 2-step track 16: still after 4 steps.
    Rig r(120.0f);
    for (auto& t : r.seq.currentPattern()->tracks) t.lastStep = 3;
    r.seq.setPerformanceMode(true);
    r.seq.setPerfTrackLength(15, 1);
    r.seq.play(0);
    r.runUntil([&] { return r.host.patternEnds == 1; });
    CHECK_EQ(r.host.endTicks.size(), 1u);
    if (!r.host.endTicks.empty()) CHECK_EQ(r.host.endTicks[0], uint64_t(12 + (4 * 48 - 1) * 12));
    CHECK_EQ(r.seq.patternPosition(), 0);
}
