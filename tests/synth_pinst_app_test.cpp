// [3.x] Preset instruments in the App: P1..P8 (67..74) are instruments the
// cursor, the step edits and the previews know. A preview (pads, MIDI keys,
// the note encoder) of a preset instrument plays its preset on a slot the
// bank picks; an empty preset is silent. The stopped chase does not take
// the slot back while the note sounds.
#include "test.h"

#include <cstring>
#include <string>

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

// --- UI ----------------------------------------------------------------------------

TEST(popups_list_the_presets_after_the_slots) {
    using namespace synthui;
    CHECK_EQ(kSynthPopupCount, 11);
    CHECK_EQ(popupInstrument(2), 66);
    CHECK_EQ(popupInstrument(3), 67);
    CHECK_EQ(popupInstrument(10), 74);
    CHECK_EQ(popupInstrument(11), 0);
    CHECK_EQ(popupInstrument(11 + 63), 63);
    for (int pos = 0; pos < kInstrumentCount + kSynthPopupCount; ++pos) CHECK_EQ(popupPosition(popupInstrument(pos)), pos);
    CHECK_EQ(kMidiPopupCount, 27);
    CHECK_EQ(popupInstrumentMidi(3), 67);
    CHECK_EQ(popupInstrumentMidi(11), 48);
    for (int pos = 0; pos < kMidiPopupCount; ++pos) CHECK_EQ(popupPositionMidi(popupInstrumentMidi(pos)), pos);
    CHECK_EQ(popupPositionMidi(5), 11);   // a sample instrument: MIDI 1
    // MIDI-track Instr pads: 17-19 = S1..S3, 20-27 = P1..P8.
    CHECK_EQ(midiTrackPadInstrument(18), 66);
    CHECK_EQ(midiTrackPadInstrument(19), 67);
    CHECK_EQ(midiTrackPadInstrument(26), 74);
    CHECK_EQ(midiTrackPadInstrument(27), kFirstMidiInstrument + (27 & 15));
    CHECK_EQ(midiTrackInstrumentPad(67), 19);
    CHECK_EQ(midiTrackInstrumentPad(74), 26);
    CHECK_EQ(kInstrumentChooserCount, 75);
}

#if defined(OT_MOD_ACD1) && OT_MOD_ACD1
TEST(preset_instruments_are_labelled_by_their_engine) {
    Harness h;
    App& a = *h.app;
    if (a.synthSlot(1).model != uint32_t(SynthModel::ACD)) a.setSynthModel(1, int(SynthModel::ACD));
    a.storeSynthPreset(2, 1);
    char t[48];
    synthui::presetLabel(a, 2, t, sizeof t);
    CHECK(std::strcmp(t, "P2 ACD") == 0);
    synthui::presetLabel(a, 3, t, sizeof t);
    CHECK(std::strcmp(t, "P3 -") == 0);
    instrumentChooserItem(&a, kFirstPresetInstrument + 1, t, sizeof t);
    CHECK(std::strcmp(t, "P2 ACD") == 0);
    CHECK(!synthui::presetNotice(a, kFirstPresetInstrument + 1, t, sizeof t));
    CHECK(synthui::presetNotice(a, kFirstPresetInstrument + 2, t, sizeof t));
    CHECK(std::strcmp(t, "P3 is empty") == 0);
    CHECK(!synthui::presetNotice(a, 66, t, sizeof t));
}

TEST(instrument_params_on_a_preset_instrument_open_its_preset_row) {
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(kFirstSynthInstrument + 2);   // the Synth page shows slot 3 first
    h.tap(btn::Pattern);
    h.tap(btn::InstrParams);
    CHECK_EQ(a.ui.active(), kModSynth);
    h.tap(btn::Pattern);
    a.setCurrentInstrument(kFirstPresetInstrument + 4);   // P5
    h.tap(btn::InstrParams);
    auto* sy = static_cast<SynthScreen*>(a.ui.screen(kModSynth));
    CHECK_EQ(a.ui.active(), kModSynth);
    CHECK(sy->view == SynthScreen::View::Presets);
    CHECK_EQ(sy->row, 4);
    CHECK_EQ(sy->slot, 2);   // the slot shown last
}

TEST(the_pattern_shows_p_codes_and_picking_an_empty_preset_says_so) {
    Harness h;
    App& a = *h.app;
    Pattern& p = a.pattern();
    p.tracks[0].steps[0].note = 60;
    p.tracks[0].steps[0].instrument = uint8_t(kFirstPresetInstrument + 6);   // P7
    a.patternEdited();
    h.tap(btn::Pattern);
    a.ui.dirty();
    h.run(40);
    bool code = false;
    for (const std::string& l : h.disp.last) code |= l.find("P7") != std::string::npos;
    CHECK(code);
    // The Instrument popup: P3 (empty) picked with the cursor on the Instr column.
    auto* ps = static_cast<PatternScreen*>(a.ui.screen(kModPattern));
    h.tap(btn::Rec);
    ps->col = 1;
    ps->step = 1;
    a.setCurrentInstrument(0);
    h.down(btn::Instr);
    h.run(300);
    CHECK_EQ(ps->popup().cursor(), synthui::kSynthPopupCount);   // sample 1 after S1..S3, P1..P8
    for (int i = 0; i < synthui::kSynthPopupCount - 5; ++i) h.enc(-1);
    h.up(btn::Instr);
    CHECK_EQ(int(a.currentInstrument()), kFirstPresetInstrument + 2);
    CHECK(std::strcmp(a.ui.notice(), "P3 is empty") == 0);
    CHECK_EQ(int(p.tracks[0].steps[1].instrument), kFirstPresetInstrument + 2);   // written as the instrument
}
#endif
