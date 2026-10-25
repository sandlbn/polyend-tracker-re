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
