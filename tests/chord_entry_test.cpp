// [I] Chord step entry from the pads (not in stock), as MIDI note-in step
// entry (PatternScreen::midiChordEntry, spec 17 §1.4 step 8): in edit mode,
// with the sequencer stopped and a selection over more than one track, pads
// held together fill the selected tracks left to right on the selection's
// first step. A single-cell cursor keeps plain note entry.
#include "test.h"
#include "ui_fakes.h"

#include "app/pattern_ops.h"
#include "app/project_fields.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {
PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }
void down(Harness& h, uint8_t p) { h.push(InputEvent::kPadDown, p); }
void up(Harness& h, uint8_t p) { h.push(InputEvent::kPadUp, p); }
void addWav(Harness& h, const char* path, uint32_t frames) {
    std::vector<int16_t> s(frames);
    for (uint32_t i = 0; i < frames; ++i) s[i] = int16_t((i % 100) * 300 - 15000);
    std::vector<uint8_t> out(frames * 2 + 64);
    out.resize(wavEncode(s.data(), frames, 1, out.data(), out.size()));
    h.fs.files[path] = out;
}
int note(Harness& h, int t, int s) { return h.app->pattern().tracks[t].steps[s].note; }
// A rectangle selection: tracks t0..t1, steps s0..s1 (cursor at t1 / s1).
void select(Harness& h, int t0, int t1, int s0, int s1) {
    PatternScreen& p = pe(h);
    p.selState = 2;
    p.anchorTrack = t0;
    p.anchorStep = s0;
    p.track = t1;
    p.step = s1;
}
// Edit mode on, sample instrument 1 with a sample, step jump 1.
void setUp(Harness& h) {
    App& a = *h.app;
    addWav(h, "tone.wav", 20000);
    CHECK(a.importSample("tone.wav", 0));
    a.setCurrentInstrument(0);
    a.setStepJump(1);
    h.tap(btn::Rec);
    CHECK(a.editMode);
}
const uint8_t kPads[4] = {36, 40, 43, 47};
}  // namespace

TEST(chord_entry_fast_melody_on_a_single_cursor) {
    // Overlapping presses with a single-cell cursor are a melody: each
    // note goes to the cursor, which advances (as before chord entry).
    Harness h;
    App& a = *h.app;
    setUp(h);
    int want[4];
    for (int i = 0; i < 4; ++i) want[i] = a.padNote(kPads[i]);
    down(h, kPads[0]);
    down(h, kPads[1]);
    up(h, kPads[0]);
    down(h, kPads[2]);
    down(h, kPads[3]);
    up(h, kPads[1]);
    up(h, kPads[2]);
    up(h, kPads[3]);
    for (int i = 0; i < 4; ++i) CHECK_EQ(note(h, 0, i), want[i]);
    for (int t = 1; t < 4; ++t)
        for (int s = 0; s < 4; ++s) CHECK_EQ(note(h, t, s), kNoteEmpty);
    CHECK_EQ(pe(h).step, 4);
    CHECK_EQ(pe(h).track, 0);
}

TEST(chord_entry_fills_the_selected_tracks) {
    Harness h;
    App& a = *h.app;
    setUp(h);
    int want[4];
    for (int i = 0; i < 4; ++i) want[i] = a.padNote(kPads[i]);
    select(h, 1, 3, 4, 6);   // tracks 2..4, steps 5..7
    for (int i = 0; i < 3; ++i) down(h, kPads[i]);
    // Left to right on the selection's first step only.
    for (int i = 0; i < 3; ++i) CHECK_EQ(note(h, 1 + i, 4), want[i]);
    for (int t = 1; t <= 3; ++t) {
        CHECK_EQ(note(h, t, 5), kNoteEmpty);
        CHECK_EQ(note(h, t, 6), kNoteEmpty);
    }
    CHECK_EQ(note(h, 0, 4), kNoteEmpty);
    CHECK_EQ(int(a.project.u8(pf::kLastNote)), want[2]);   // as normal entry
    CHECK_EQ(pe(h).step, 6);   // the cursor stays
    CHECK_EQ(pe(h).selState, 2);
    for (int i = 0; i < 3; ++i) up(h, kPads[i]);
    // One Undo removes the whole chord.
    CHECK(a.undoEdit());
    for (int t = 1; t <= 3; ++t) CHECK_EQ(note(h, t, 4), kNoteEmpty);
    // A new chord after all pads were released starts at the first track.
    down(h, kPads[3]);
    CHECK_EQ(note(h, 1, 4), want[3]);
    up(h, kPads[3]);
}

TEST(chord_entry_note_past_the_selection_falls_through) {
    Harness h;
    App& a = *h.app;
    setUp(h);
    select(h, 2, 3, 0, 0);   // tracks 3..4, one row
    for (uint8_t p : kPads) down(h, p);
    CHECK(note(h, 2, 0) != kNoteEmpty);
    CHECK(note(h, 3, 0) != kNoteEmpty);
    CHECK_EQ(note(h, 4, 0), kNoteEmpty);   // nothing written past the selection
    CHECK_EQ(note(h, 1, 0), kNoteEmpty);
    // ... but the notes still sound (not swallowed).
    CHECK(a.auditionTrack(kPads[2]) >= 0);
    CHECK(a.auditionTrack(kPads[3]) >= 0);
    for (uint8_t p : kPads) up(h, p);
}

TEST(chord_entry_stops_at_a_track_that_cannot_play_it) {
    // A sample instrument plays on tracks 1..8 only: a selection over
    // tracks 8..10 takes the first note; the next track (a MIDI track)
    // would change the instrument, so that note only sounds.
    Harness h;
    App& a = *h.app;
    setUp(h);
    select(h, 7, 9, 0, 0);
    down(h, kPads[0]);
    down(h, kPads[1]);
    CHECK(note(h, 7, 0) != kNoteEmpty);
    CHECK_EQ(note(h, 8, 0), kNoteEmpty);
    CHECK_EQ(note(h, 9, 0), kNoteEmpty);
    CHECK(a.auditionTrack(kPads[1]) >= 0);
    up(h, kPads[0]);
    up(h, kPads[1]);
}

TEST(chord_entry_not_while_playing) {
    // Playing: the pads keep their normal behaviour (a single-row selection
    // only auditions), no chord is written.
    Harness h;
    App& a = *h.app;
    setUp(h);
    select(h, 0, 2, 0, 0);
    h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    CHECK(a.editMode);
    for (int i = 0; i < 3; ++i) down(h, kPads[i]);
    for (int t = 0; t < 3; ++t) CHECK_EQ(note(h, t, 0), kNoteEmpty);
    for (int i = 0; i < 3; ++i) up(h, kPads[i]);
    h.tap(btn::Play);
    CHECK(!a.seq.isPlaying());
}

TEST(chord_entry_resets_on_pattern_change) {
    // A pad still held from the old pattern does not continue its chord
    // in the new one: the next pad starts on the first selected track.
    Harness h;
    App& a = *h.app;
    setUp(h);
    select(h, 0, 2, 0, 0);
    down(h, kPads[0]);
    CHECK(note(h, 0, 0) != kNoteEmpty);
    a.selectPattern(2);
    h.run(20);
    CHECK_EQ(note(h, 0, 0), kNoteEmpty);
    select(h, 0, 2, 0, 0);
    down(h, kPads[1]);
    CHECK_EQ(note(h, 0, 0), a.padNote(kPads[1]));
    CHECK_EQ(note(h, 1, 0), kNoteEmpty);
    up(h, kPads[0]);
    up(h, kPads[1]);
}

TEST(chord_entry_resets_when_edit_mode_or_play_starts) {
    Harness h;
    App& a = *h.app;
    setUp(h);
    select(h, 0, 2, 0, 0);
    down(h, kPads[0]);
    // Edit mode off and on again with the pad held.
    h.tap(btn::Rec);
    CHECK(!a.editMode);
    h.tap(btn::Rec);
    CHECK(a.editMode);
    select(h, 0, 2, 0, 0);
    down(h, kPads[1]);
    CHECK_EQ(note(h, 0, 0), a.padNote(kPads[1]));
    CHECK_EQ(note(h, 1, 0), kNoteEmpty);
    // Play started and stopped with both pads held.
    h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    h.tap(btn::Play);
    CHECK(!a.seq.isPlaying());
    select(h, 0, 2, 0, 0);
    down(h, kPads[2]);
    CHECK_EQ(note(h, 0, 0), a.padNote(kPads[2]));
    CHECK_EQ(note(h, 1, 0), kNoteEmpty);
    for (int i = 0; i < 3; ++i) up(h, kPads[i]);
}
