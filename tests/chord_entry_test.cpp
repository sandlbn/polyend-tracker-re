// [I] Chord step entry (not in stock): in edit mode, pads pressed while a note
// pad is held write on the first note's step, one track further right each.
#include "test.h"
#include "ui_fakes.h"

#include "app/pattern_ops.h"
#include "ui/screens.h"

using namespace ot;

namespace {
PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }
void down(Harness& h, uint8_t p) { h.push(InputEvent::kPadDown, p); }
void up(Harness& h, uint8_t p) { h.push(InputEvent::kPadUp, p); }
int note(Harness& h, int t, int s) { return h.app->pattern().tracks[t].steps[s].note; }
}  // namespace

TEST(chord_entry_spreads_held_pads_over_tracks) {
    Harness h;
    App& a = *h.app;
    a.setStepJump(1);
    h.tap(btn::Rec);
    CHECK(a.editMode);
    const uint8_t pads[4] = {36, 40, 43, 47};
    int want[4];
    for (int i = 0; i < 4; ++i) want[i] = a.padNote(pads[i]);
    // A four-note chord: all pads down, then up.
    for (uint8_t p : pads) down(h, p);
    for (int i = 0; i < 4; ++i) CHECK_EQ(note(h, i, 0), want[i]);
    CHECK_EQ(note(h, 4, 0), kNoteEmpty);
    CHECK_EQ(pe(h).step, 1);    // advanced once, by the first note
    CHECK_EQ(pe(h).track, 0);
    for (uint8_t p : pads) up(h, p);
    // A single note afterwards: as in stock, on the cursor track only.
    h.pad(pads[1]);
    CHECK_EQ(note(h, 0, 1), want[1]);
    CHECK_EQ(note(h, 1, 1), kNoteEmpty);
    CHECK_EQ(pe(h).step, 2);
    // A new chord after all pads were released starts at the cursor again.
    down(h, pads[2]);
    down(h, pads[3]);
    up(h, pads[2]);
    up(h, pads[3]);
    CHECK_EQ(note(h, 0, 2), want[2]);
    CHECK_EQ(note(h, 1, 2), want[3]);
    CHECK_EQ(pe(h).step, 3);
    // One Undo removes the whole chord.
    CHECK(a.undoEdit());
    CHECK_EQ(note(h, 0, 2), kNoteEmpty);
    CHECK_EQ(note(h, 1, 2), kNoteEmpty);
    CHECK_EQ(note(h, 0, 1), want[1]);
}

TEST(chord_entry_stops_at_a_track_that_cannot_play_it) {
    Harness h;
    App& a = *h.app;
    a.setStepJump(1);
    a.setCurrentInstrument(0);   // a sample instrument: tracks 1..8 only
    h.tap(btn::Rec);
    pe(h).track = 6;
    const uint8_t pads[4] = {36, 40, 43, 47};
    for (uint8_t p : pads) down(h, p);
    CHECK(note(h, 6, 0) != kNoteEmpty);
    CHECK(note(h, 7, 0) != kNoteEmpty);
    CHECK_EQ(note(h, 8, 0), kNoteEmpty);   // a MIDI track: the chord ends
    CHECK_EQ(note(h, 9, 0), kNoteEmpty);
    for (uint8_t p : pads) up(h, p);
}
