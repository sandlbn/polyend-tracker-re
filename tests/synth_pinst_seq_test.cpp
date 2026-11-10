// [3.x] Preset instruments in the sequencer: a step with instrument 67..74
// sends a synth note that names the preset (the bank picks the slot). Its
// CC a..f go with the note; on an FX-only step after it they go to the
// track's sounding note. A Z has no slot to act on there (ignored, as the
// chase reads it). Random Instrument stays inside P1..P8. Note-offs, mutes,
// rolls and arps work as for the slot instruments; the Arp Gate is a slot
// setting, so a preset's arp notes last until the next one.
#include "test.h"

#include <cstring>
#include <vector>

#include "model/constants.h"
#include "model/fx.h"
#include "model/project.h"
#include "sequencer/sequencer.h"

using namespace ot;

namespace {

struct Ev {
    enum Kind { On, Off, Fx, Macro, Preset } kind;
    int track, a, b, c;
};

struct Sink : ISequencerSink {
    std::vector<Ev> ev;
    void voiceNoteOn(uint8_t, const VoiceNoteOn&) override {}
    void voiceNoteOff(uint8_t, NoteOffMode) override {}
    void voiceApplyFx(uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void resetAllVoiceFx() override {}
    void engineBlockUpdate(float) override {}
    void metronomeClick(bool, uint8_t) override {}
    void midiSend(uint8_t, const uint8_t*, uint8_t) override {}
    void synthNoteOn(uint8_t t, const SynthNoteOn& n) override {
        ev.push_back({Ev::On, t, n.note, n.slot, n.preset});
        last = n;
    }
    void synthNoteOff(uint8_t t, NoteOffMode m) override { ev.push_back({Ev::Off, t, int(m), 0, 0}); }
    void synthApplyFx(uint8_t t, uint8_t type, uint8_t v) override { ev.push_back({Ev::Fx, t, type, v, 0}); }
    void synthMacroFx(uint8_t slot, uint8_t m, uint8_t v) override { ev.push_back({Ev::Macro, -1, slot, m, v}); }
    bool synthPreset(uint8_t slot, uint8_t n) override {
        ev.push_back({Ev::Preset, -1, slot, n, 0});
        return true;
    }
    std::vector<Ev> of(Ev::Kind k) const {
        std::vector<Ev> r;
        for (const Ev& e : ev)
            if (e.kind == k) r.push_back(e);
        return r;
    }
    SynthNoteOn last;
};

struct Host : ISequencerHost {
    void loadPattern(uint8_t, Pattern& dst, bool) override { dst.setDefault(); }
    int gateAsks = 0;
    uint8_t synthArpGate(uint8_t) override {
        ++gateAsks;
        return 50;
    }
};

struct Rig {
    Project project;
    Pattern a, b;
    Sink sink;
    Host host;
    Sequencer seq;
    Rig() {
        a.setDefault();
        b.setDefault();
        project.setTempo(120.0f);
        seq.attach(&sink, &host, &project, &a, &b);
    }
    Step& step(int t, int s) { return seq.currentPattern()->tracks[t].steps[s]; }
    void note(int t, int s, int n, int instr) {
        step(t, s).note = int8_t(n);
        step(t, s).instrument = uint8_t(instr);
    }
    void fxA(int t, int s, int type, int v) { step(t, s).fx[1] = StepFx{uint8_t(type), uint8_t(v)}; }
    void z(int t, int s, int n) { step(t, s).fx[1] = StepFx{0, uint8_t(kFxPresetCmdBase + n)}; }
    void until(int t, int s) {
        for (int i = 0; i < 100000 && seq.step(t) != s; ++i) seq.processBlock(128);
    }
};

TEST(a_preset_step_sends_a_synth_note_naming_the_preset) {
    Rig r;
    r.note(0, 0, 60, 67);   // P1
    r.note(3, 1, 62, 74);   // P8
    r.note(9, 2, 64, 65);   // S2
    r.fxA(0, 0, 18, 70);    // Volume rides along
    r.seq.play(0);
    r.until(0, 3);
    auto on = r.sink.of(Ev::On);
    CHECK_EQ(on.size(), 3u);
    if (on.size() == 3) {
        CHECK_EQ(on[0].track, 0);
        CHECK_EQ(on[0].c, 1);
        CHECK_EQ(on[1].track, 3);
        CHECK_EQ(on[1].c, 8);
        CHECK_EQ(on[2].c, 0);   // a slot instrument: its slot
        CHECK_EQ(on[2].b, 1);
    }
    // Instruments past the presets stay silent.
    Rig q;
    q.note(0, 0, 60, 75);
    q.seq.play(0);
    q.until(0, 2);
    CHECK(q.sink.of(Ev::On).empty());
}

TEST(a_preset_note_is_released_by_the_track) {
    Rig r;
    r.note(0, 0, 60, 68);
    r.step(0, 1).note = kNoteOff;
    r.note(0, 2, 62, 69);   // P3 after P2: the bank releases (no sequencer note-off in between)
    r.seq.play(0);
    r.until(0, 3);
    auto off = r.sink.of(Ev::Off);
    CHECK_EQ(off.size(), 1u);
    CHECK_EQ(r.sink.of(Ev::On).size(), 2u);
    r.seq.stop();
    CHECK_EQ(r.sink.of(Ev::Off).size(), 2u);   // Stop releases the sounding one
}

TEST(cc_of_a_preset_note_goes_with_the_note_and_after_it_to_the_tracks_note) {
    Rig r;
    r.note(0, 0, 60, 67);
    r.fxA(0, 0, 8, 100);   // CC a on the note step
    r.fxA(0, 1, 9, 40);    // CC b on an FX-only step
    r.seq.play(0);
    r.until(0, 2);
    CHECK(r.sink.of(Ev::Macro).empty());   // no slot to name
    CHECK_EQ(int(r.sink.last.fxA.type), 8);
    CHECK_EQ(int(r.sink.last.fxA.value), 100);
    auto fx = r.sink.of(Ev::Fx);
    CHECK_EQ(fx.size(), 1u);
    if (!fx.empty()) {
        CHECK_EQ(fx[0].track, 0);
        CHECK_EQ(fx[0].a, 9);
        CHECK_EQ(fx[0].b, 40);
    }
    // A slot instrument's CC still sets the slot's macro.
    Rig s;
    s.note(0, 0, 60, 66);
    s.fxA(0, 0, 8, 100);
    s.fxA(0, 1, 9, 40);
    s.seq.play(0);
    s.until(0, 2);
    CHECK_EQ(s.sink.of(Ev::Macro).size(), 2u);
    CHECK(s.sink.of(Ev::Fx).empty());
}

TEST(a_z_after_or_on_a_preset_note_has_no_target) {
    Rig r;
    r.note(0, 0, 60, 67);
    r.z(0, 0, 3);   // on the note step
    r.z(0, 1, 4);   // FX-only step after it
    r.seq.play(0);
    const size_t before = r.sink.of(Ev::Preset).size();   // the start's chase
    r.until(0, 3);
    CHECK_EQ(r.sink.of(Ev::Preset).size(), before);
    // After a slot instrument's note the no-note Z still acts on its slot.
    Rig s;
    s.note(0, 0, 60, 65);
    s.z(0, 1, 4);
    s.seq.play(0);
    const size_t b2 = s.sink.of(Ev::Preset).size();
    s.until(0, 3);
    auto z = s.sink.of(Ev::Preset);
    CHECK_EQ(z.size(), b2 + 1);
    if (z.size() == b2 + 1) {
        CHECK_EQ(z.back().a, 1);
        CHECK_EQ(z.back().b, 4);
    }
}

TEST(random_instrument_stays_inside_the_presets) {
    Rig r;
    for (int s = 0; s < 32; ++s) {
        r.note(0, s, 60, 70);   // P4
        r.fxA(0, s, 6, 10);     // Random Instrument +-10
    }
    r.seq.play(0);
    r.until(0, 31);
    auto on = r.sink.of(Ev::On);
    CHECK(on.size() >= 30u);
    bool spread = false;
    for (const Ev& e : on) {
        CHECK(e.c >= 1 && e.c <= 8);
        spread |= e.c != 4;
    }
    CHECK(spread);
}

TEST(a_preset_arp_ignores_the_slot_arp_gate) {
    Rig r;
    r.note(0, 0, 60, 67);
    r.step(0, 0).fx[0] = StepFx{14, 5};   // Chord
    r.fxA(0, 0, 21, 9);                   // Arp
    r.seq.play(0);
    for (int i = 0; i < 100000 && r.sink.of(Ev::On).size() < 4u; ++i) r.seq.processBlock(128);
    CHECK(r.sink.of(Ev::On).size() >= 4u);
    for (const Ev& e : r.sink.of(Ev::On)) CHECK_EQ(e.c, 1);
    CHECK_EQ(r.host.gateAsks, 0);           // the Arp Gate is a slot's: until the next note
}

}  // namespace
