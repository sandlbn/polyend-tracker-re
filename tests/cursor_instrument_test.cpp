// Issue #52 (public repo): from the Pattern screen with Rec off, Instrument
// Params, Sample Playback and Sample Editor open the instrument of the step
// under the cursor and make it the current instrument (stock 1.9.2, checked
// on a Tracker by the reporter). An empty step or OFF / CUT / FADE (no
// instrument shown) keeps the current instrument; with Rec on nothing changes.
#include "test.h"
#include "ui_fakes.h"
#include "ui/screens.h"

using namespace ot;

namespace {
void putStep(App& a, int track, int step, int8_t note, uint8_t instr) {
    Step& s = a.pattern().tracks[track].steps[step];
    s.note = note;
    s.instrument = instr;
    a.patternEdited();
}
void cursorTo(Harness& h, int track, int step) {
    PatternScreen& p = *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern));
    p.track = track;
    p.step = step;
}
}  // namespace

TEST(module_keys_take_the_instrument_under_the_cursor) {
    const uint8_t keys[] = {btn::InstrParams, btn::SamplePlayback, btn::SampleEditor};
    for (uint8_t key : keys) {
        Harness h;
        App& a = *h.app;
        a.setCurrentInstrument(0);
        putStep(a, 1, 4, 60, 2);   // instrument 3 on track 2, step 5
        h.tap(btn::Pattern);
        CHECK(!a.editMode);
        cursorTo(h, 1, 4);
        h.tap(key);
        CHECK_EQ(int(a.currentInstrument()), 2);
    }
}

TEST(module_key_on_a_midi_step_takes_the_midi_instrument) {
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(0);
    putStep(a, 0, 0, 60, uint8_t(kFirstMidiInstrument + 2));   // M03
    h.tap(btn::Pattern);
    cursorTo(h, 0, 0);
    h.tap(btn::InstrParams);
    CHECK_EQ(int(a.currentInstrument()), kFirstMidiInstrument + 2);
}

TEST(module_key_keeps_the_current_instrument_without_a_note) {
    const int8_t notes[] = {kNoteEmpty, kNoteOff, kNoteCut, kNoteFade};
    for (int8_t n : notes) {
        Harness h;
        App& a = *h.app;
        a.setCurrentInstrument(5);
        putStep(a, 0, 3, n, 2);
        h.tap(btn::Pattern);
        cursorTo(h, 0, 3);
        h.tap(btn::SamplePlayback);
        CHECK_EQ(int(a.currentInstrument()), 5);
    }
}

TEST(module_key_with_rec_on_keeps_the_current_instrument) {
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(5);
    putStep(a, 0, 0, 60, 2);
    h.tap(btn::Pattern);
    h.tap(btn::Rec);
    CHECK(a.editMode);
    cursorTo(h, 0, 0);
    h.tap(btn::InstrParams);
    CHECK_EQ(int(a.currentInstrument()), 5);
}

TEST(moving_the_cursor_alone_keeps_the_current_instrument) {
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(5);
    putStep(a, 0, 1, 60, 2);
    h.tap(btn::Pattern);
    h.tap(btn::Down);
    CHECK_EQ(int(a.currentInstrument()), 5);
}

TEST(module_key_from_another_screen_keeps_the_current_instrument) {
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(5);
    putStep(a, 0, 0, 60, 2);
    h.tap(btn::Pattern);
    cursorTo(h, 0, 0);
    h.tap(btn::Song);
    h.tap(btn::SamplePlayback);   // not from the Pattern screen
    CHECK_EQ(int(a.currentInstrument()), 5);
}

// 2.x: a synth step opens its synth page (Instrument Params) and becomes the
// current instrument for Sample Playback too; tracks 13-16 follow the rule.
TEST(module_key_on_a_synth_step_with_rec_on_keeps_the_current_instrument) {
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(5);
    putStep(a, 0, 0, 60, uint8_t(kFirstSynthInstrument + 1));   // Synth 2
    h.tap(btn::Pattern);
    h.tap(btn::Rec);
    CHECK(a.editMode);
    cursorTo(h, 0, 0);
    h.tap(btn::InstrParams);
    CHECK_EQ(int(a.currentInstrument()), 5);
    CHECK_EQ(a.ui.active(), int(kModInstrument));
}

TEST(module_key_on_a_synth_step_without_a_note_keeps_the_current_instrument) {
    const int8_t notes[] = {kNoteEmpty, kNoteOff, kNoteCut, kNoteFade};
    for (int8_t n : notes) {
        Harness h;
        App& a = *h.app;
        a.setCurrentInstrument(5);
        putStep(a, 0, 3, n, uint8_t(kFirstSynthInstrument + 1));
        h.tap(btn::Pattern);
        cursorTo(h, 0, 3);
        h.tap(btn::InstrParams);
        CHECK_EQ(int(a.currentInstrument()), 5);
        CHECK_EQ(a.ui.active(), int(kModInstrument));
    }
}

TEST(a_synth_as_the_current_instrument_still_opens_its_page) {
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(kFirstSynthInstrument + 2);   // Synth 3
    h.tap(btn::Pattern);
    h.tap(btn::Rec);
    cursorTo(h, 0, 0);
    h.tap(btn::InstrParams);
    CHECK_EQ(int(a.currentInstrument()), kFirstSynthInstrument + 2);
    CHECK_EQ(a.ui.active(), int(kModSynth));
}

TEST(module_key_on_a_preset_step_follows_the_same_rule) {
    {   // Rec on: the current instrument stays.
        Harness h;
        App& a = *h.app;
        a.setCurrentInstrument(5);
        putStep(a, 0, 0, 60, uint8_t(kFirstPresetInstrument + 1));   // P2
        h.tap(btn::Pattern);
        h.tap(btn::Rec);
        cursorTo(h, 0, 0);
        h.tap(btn::InstrParams);
        CHECK_EQ(int(a.currentInstrument()), 5);
        CHECK_EQ(a.ui.active(), int(kModInstrument));
    }
    {   // An OFF that still carries P2: the current instrument stays.
        Harness h;
        App& a = *h.app;
        a.setCurrentInstrument(5);
        putStep(a, 0, 0, kNoteOff, uint8_t(kFirstPresetInstrument + 1));
        h.tap(btn::Pattern);
        cursorTo(h, 0, 0);
        h.tap(btn::InstrParams);
        CHECK_EQ(int(a.currentInstrument()), 5);
        CHECK_EQ(a.ui.active(), int(kModInstrument));
    }
    {   // Rec off on a note: the preset becomes current, its page opens.
        Harness h;
        App& a = *h.app;
        a.setCurrentInstrument(5);
        putStep(a, 0, 0, 60, uint8_t(kFirstPresetInstrument + 1));
        h.tap(btn::Pattern);
        cursorTo(h, 0, 0);
        h.tap(btn::InstrParams);
        CHECK_EQ(int(a.currentInstrument()), kFirstPresetInstrument + 1);
        CHECK_EQ(a.ui.active(), int(kModSynth));
    }
}

TEST(module_keys_on_synth_steps_and_tracks_13_16) {
    {
        Harness h;
        App& a = *h.app;
        a.setCurrentInstrument(0);
        putStep(a, 2, 0, 60, uint8_t(kFirstSynthInstrument + 1));   // Synth 2
        h.tap(btn::Pattern);
        cursorTo(h, 2, 0);
        h.tap(btn::InstrParams);
        CHECK_EQ(int(a.currentInstrument()), kFirstSynthInstrument + 1);
        CHECK_EQ(a.ui.active(), int(kModSynth));
    }
    {
        Harness h;
        App& a = *h.app;
        a.setCurrentInstrument(0);
        putStep(a, 13, 2, 60, uint8_t(kFirstMidiInstrument + 4));   // track 14, M05
        h.tap(btn::Pattern);
        cursorTo(h, 13, 2);
        h.tap(btn::InstrParams);
        CHECK_EQ(int(a.currentInstrument()), kFirstMidiInstrument + 4);
    }
}

// An instrument picked in the Instruments list (Rec off) is the one
// Instrument Params opens while the cursor stays on that step, a synth or a
// sample; once the cursor moves, the step under it decides again.
namespace {
int pickFromList(Harness& h, int8_t detents) {
    h.down(btn::Instr);
    h.run(400);
    h.enc(detents);
    h.run(200);
    h.up(btn::Instr);
    h.run(200);
    return int(h.app->currentInstrument());
}
}  // namespace

TEST(instrument_picked_in_the_list_wins_over_the_step) {
    {   // A synth step, a sample picked.
        Harness h;
        App& a = *h.app;
        a.setCurrentInstrument(kFirstSynthInstrument);
        putStep(a, 0, 0, 60, uint8_t(kFirstSynthInstrument));   // S1
        h.tap(btn::Pattern);
        CHECK(!a.editMode);
        cursorTo(h, 0, 0);
        const int ins = pickFromList(h, 14);
        CHECK(ins < kInstrumentCount);
        h.tap(btn::InstrParams);
        CHECK_EQ(int(a.currentInstrument()), ins);
        CHECK_EQ(a.ui.active(), int(kModInstrument));
    }
    {   // A sample step, a synth picked.
        Harness h;
        App& a = *h.app;
        a.setCurrentInstrument(0);
        putStep(a, 0, 0, 60, 0);
        h.tap(btn::Pattern);
        cursorTo(h, 0, 0);
        const int ins = pickFromList(h, -64);   // the list's top: Synth 1
        CHECK_EQ(ins, kFirstSynthInstrument);
        h.tap(btn::InstrParams);
        CHECK_EQ(int(a.currentInstrument()), kFirstSynthInstrument);
        CHECK_EQ(a.ui.active(), int(kModSynth));
    }
}

TEST(instrument_pick_ends_when_the_cursor_moves) {
    Harness h;
    App& a = *h.app;
    a.setCurrentInstrument(kFirstSynthInstrument);
    putStep(a, 0, 0, 60, uint8_t(kFirstSynthInstrument));
    putStep(a, 0, 1, 60, uint8_t(kFirstSynthInstrument));
    h.tap(btn::Pattern);
    cursorTo(h, 0, 0);
    CHECK(pickFromList(h, 14) < kInstrumentCount);
    cursorTo(h, 0, 1);
    h.tap(btn::InstrParams);
    CHECK_EQ(int(a.currentInstrument()), kFirstSynthInstrument);
    CHECK_EQ(a.ui.active(), int(kModSynth));
}
