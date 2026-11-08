// [3.x] Preset instruments in the App: P1..P8 (67..74) are instruments the
// cursor, the step edits and the previews know. A preview (pads, MIDI keys,
// the note encoder) of a preset instrument plays its preset on a slot the
// bank picks; an empty preset is silent. The stopped chase does not take
// the slot back while the note sounds.
#include "test.h"

#include <cstring>

#include "app/pattern_ops.h"
#include "audio/synth_bank.h"
#include "modules/synth_models.h"
#include "ui/screens.h"
#include "ui_fakes.h"

using namespace ot;

namespace {

int slotsOn(App& a, int k) {
    int n = 0;
    for (int s = 0; s < kSynthSlotCount; ++s) n += a.engine.synth().presetOf(s) == k;
    return n;
}

}  // namespace

TEST(the_current_instrument_reaches_p8) {
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(kLastPresetInstrument);
    CHECK_EQ(int(a.currentInstrument()), 74);
    a.setCurrentInstrument(90);
    CHECK_EQ(int(a.currentInstrument()), 74);
    a.setCurrentInstrument(kFirstPresetInstrument);
    CHECK_EQ(int(a.currentInstrument()), 67);
}

TEST(step_edits_know_the_preset_instruments) {
    Pattern p;
    p.setDefault();
    SelRect r;
    r.s0 = r.s1 = 0;
    r.t0 = r.t1 = 0;
    p.tracks[0].steps[0].note = 60;
    p.tracks[0].steps[0].instrument = 66;
    pops::stepInstrument(p, r, 1);
    CHECK_EQ(int(p.tracks[0].steps[0].instrument), 67);
    pops::stepInstrument(p, r, 20);
    CHECK_EQ(int(p.tracks[0].steps[0].instrument), 74);
    // An empty step takes a note with a preset instrument (it plays), on a MIDI track too.
    r.t0 = r.t1 = 9;
    pops::writeInstrument(p, r, 70);
    CHECK_EQ(int(p.tracks[9].steps[0].instrument), 70);
    CHECK_EQ(int(p.tracks[9].steps[0].note), 60);
    CHECK_EQ(int(pops::mapInstrument(9, 72)), 72);
    CHECK_EQ(int(pops::mapInstrument(2, 72)), 72);
    CHECK_EQ(int(pops::mapInstrument(2, 75)), 0);   // past P8: not an instrument
}

#if defined(OT_MOD_ACD1) && OT_MOD_ACD1
TEST(a_pad_plays_a_preset_instrument_and_the_stopped_chase_keeps_it) {
    Harness h;
    App& a = *h.app;
    if (a.synthSlot(1).model != uint32_t(SynthModel::ACD)) a.setSynthModel(1, int(SynthModel::ACD));
    a.storeSynthPreset(1, 1);
    h.run(40);
    a.previewOn(3, uint8_t(kFirstPresetInstrument), 60, 127);   // pad 4, P1
    h.audio.pump(2);
    CHECK_EQ(slotsOn(a, 1), 1);
    CHECK(a.engine.synth().sounding() >= 1);
    h.run(60);   // the stopped chase runs: the slot stays on P1
    CHECK_EQ(slotsOn(a, 1), 1);
    a.previewOff(3);
    h.audio.pump(2);
    // The encoder's preview (the audition track) plays it too.
    a.previewOn(App::kAuditionEncoder, uint8_t(kFirstPresetInstrument), 62, 127);
    h.audio.pump(2);
    const audio::SynthVoice* v = a.engine.synth().voiceOfTrack(audio::kSynthAuditionTrack);
    CHECK(v && v->gate && v->note == 62 && a.engine.synth().presetOf(v->slot) == 1);
    a.previewOff(App::kAuditionEncoder);
    h.audio.pump(2);
    CHECK(!a.engine.synth().voiceOfTrack(audio::kSynthAuditionTrack) ||
          !a.engine.synth().voiceOfTrack(audio::kSynthAuditionTrack)->gate);
}

TEST(an_empty_preset_instrument_is_silent) {
    Harness h;
    App& a = *h.app;
    a.previewOn(5, uint8_t(kFirstPresetInstrument + 2), 60, 127);   // P3: nothing stored
    h.audio.pump(2);
    CHECK_EQ(a.engine.synth().sounding(), 0);
    CHECK_EQ(slotsOn(a, 3), 0);
    a.previewOff(5);
}
#endif
