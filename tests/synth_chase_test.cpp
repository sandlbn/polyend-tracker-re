// [OT] Z chase: the preset each synth slot is on at a position, read from the
// pattern data (app/synth_chase.h): at Play from a later position, and while
// stopped at the pattern-editor cursor.
#include "test.h"

#include <cstring>

#include "app/synth_chase.h"
#include "ui/screens.h"
#include "ui_fakes.h"

using namespace ot;

namespace {
void z(Step& st, int col, int n) { st.fx[col] = StepFx{0, uint8_t(kFxPresetCmdBase + n)}; }   // col 0 = FX-B, 1 = FX-A
void note(Step& st, int instr, int n = 60) {
    st.note = int8_t(n);
    st.instrument = uint8_t(instr);
}
// A stored pattern's step, for writing (marks it dirty like an edit).
Step& stored(App& a, int num, int t, int s) {
    a.markPatternDirty(num);
    return a.storedTrack(num, t).steps[s];
}
struct Chase {
    uint8_t preset[kSynthSlotCount];
    uint8_t last[kTrackCount];
};
Chase ask(App& a, bool song, int pos, int lastStep) {
    Chase c;
    std::memset(c.preset, 0, sizeof(c.preset));
    std::memset(c.last, 0xFF, sizeof(c.last));
    a.synthChase(song, pos, a.patternNumber(), lastStep, c.preset, c.last);
    return c;
}
void presets(App& a, std::initializer_list<int> ns) {
    for (int n : ns) a.storeSynthPreset(n, 0);
}
}  // namespace

TEST(chase_reads_the_current_pattern_up_to_the_cursor_fx_a_over_b) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    presets(a, {1, 2, 3});
    Pattern& p = a.pattern();
    note(p.tracks[0].steps[3], 64);
    z(p.tracks[0].steps[3], 0, 1);   // FX-B first
    z(p.tracks[0].steps[3], 1, 2);   // FX-A wins
    a.patternEdited();
    CHECK_EQ(int(ask(a, false, 0, 2).preset[0]), 0);   // before the Z
    Chase c = ask(a, false, 0, 3);
    CHECK_EQ(int(c.preset[0]), 2);
    CHECK_EQ(int(c.last[0]), 64);
    CHECK_EQ(int(c.last[1]), 0xFF);
    // A later track of the same step wins (tracks 0..15 in order).
    note(p.tracks[5].steps[3], 64);
    z(p.tracks[5].steps[3], 1, 3);
    a.patternEdited();
    CHECK_EQ(int(ask(a, false, 0, 3).preset[0]), 3);
}

TEST(no_note_z_uses_the_tracks_last_instrument_across_song_positions) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    presets(a, {4});
    note(stored(a, 2, 0, 0), 65);    // Synth 2 on track 0
    z(stored(a, 3, 0, 0), 1, 4);     // a Z without a note
    note(stored(a, 5, 0, 0), 3);     // a sample note
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 3;
    a.project.playlist[2] = 4;
    Chase c = ask(a, true, 2, -1);
    CHECK_EQ(int(c.preset[1]), 4);
    CHECK_EQ(int(c.last[0]), 65);
    // After a sample note the Z has no synth to switch.
    a.project.playlist[0] = 5;
    c = ask(a, true, 2, -1);
    CHECK_EQ(int(c.preset[1]), 0);
    CHECK_EQ(int(c.last[0]), 3);
    // Within the current pattern, the song prefix's instrument counts too.
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 6;
    z(a.pattern().tracks[0].steps[1], 1, 4);
    c = ask(a, true, 1, 1);
    CHECK_EQ(int(c.preset[1]), 4);
}

TEST(empty_preset_z_is_a_no_op_and_store_clear_invalidate_summaries) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    presets(a, {1});
    note(stored(a, 2, 0, 0), 64);
    z(stored(a, 2, 0, 0), 1, 1);
    note(stored(a, 2, 0, 4), 64);
    z(stored(a, 2, 0, 4), 1, 5);   // preset 5 is empty
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 3;
    CHECK_EQ(int(ask(a, true, 1, -1).preset[0]), 1);
    a.storeSynthPreset(5, 0);
    CHECK_EQ(int(ask(a, true, 1, -1).preset[0]), 5);
    a.clearSynthPreset(5);
    CHECK_EQ(int(ask(a, true, 1, -1).preset[0]), 1);
    a.clearSynthPreset(1);
    CHECK_EQ(int(ask(a, true, 1, -1).preset[0]), 0);
}

TEST(chase_ignores_chance_and_mutes_and_zero_playlist_entries) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    presets(a, {2, 6});
    Step& s = stored(a, 2, 4, 0);
    note(s, 66);
    s.fx[0] = StepFx{uint8_t(FxId::Chance), 0};   // never plays live: read as written
    z(s, 1, 2);
    a.setTrackMute(4, true);
    note(stored(a, 7, 0, 0), 64);
    z(stored(a, 7, 0, 0), 1, 6);
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 0;   // an empty entry: skipped
    a.project.playlist[2] = 7;
    a.project.playlist[3] = 3;
    Chase c = ask(a, true, 3, -1);
    CHECK_EQ(int(c.preset[2]), 2);
    CHECK_EQ(int(c.preset[0]), 6);
}

TEST(summary_is_rebuilt_after_a_pattern_edit_undo_and_track_copy) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    presets(a, {3, 7});
    note(stored(a, 2, 1, 0), 64);
    z(stored(a, 2, 1, 0), 1, 3);
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 3;
    CHECK_EQ(int(ask(a, true, 1, -1).preset[0]), 3);
    // An edit of pattern 2 in the editor (store copy through patternEdited).
    a.selectPattern(2);
    a.pushUndo();
    z(a.pattern().tracks[1].steps[0], 1, 7);
    a.patternEdited();
    a.selectPattern(3);
    CHECK_EQ(int(ask(a, true, 1, -1).preset[0]), 7);
    a.selectPattern(2);
    CHECK(a.undoEdit());
    a.selectPattern(3);
    CHECK_EQ(int(ask(a, true, 1, -1).preset[0]), 3);
    // A track copied into the store (Song screen paths write storedTrack).
    Track t = *a.findStoredTrack(2, 1);
    z(t.steps[5], 1, 7);
    t.steps[5].note = 62;
    t.steps[5].instrument = 64;
    a.storedTrack(2, 9) = t;
    CHECK_EQ(int(ask(a, true, 1, -1).preset[0]), 7);
    // A pattern back to the default.
    a.defaultStored(2);
    CHECK_EQ(int(ask(a, true, 1, -1).preset[0]), 0);
}

TEST(shift_play_mid_song_starts_on_the_chased_preset) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    presets(a, {3});
    note(stored(a, 2, 0, 0), 64);
    z(stored(a, 2, 0, 0), 1, 3);
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 4;
    a.project.playlistPos = 1;
    a.togglePlay(true);
    CHECK(a.seq.isPlaying());
    CHECK_EQ(int(a.patternNumber()), 4);
    CHECK_EQ(a.synthPresetActive(0), 3);
    a.togglePlay(false);
    // From position 0 nothing comes before: the own patch.
    a.project.playlistPos = 0;
    a.engine.synth().presetSelect(0, 3);
    a.togglePlay(true);
    CHECK_EQ(a.synthPresetActive(0), 0);
    a.togglePlay(false);
}

TEST(play_from_step_0_starts_on_own_even_with_the_cursor_after_a_z) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    presets(a, {2});
    note(a.pattern().tracks[0].steps[0], 64);
    z(a.pattern().tracks[0].steps[0], 1, 2);
    a.patternEdited();
    a.engine.synth().presetSelect(0, 2);
    a.togglePlay(false);
    CHECK(a.seq.isPlaying());
    CHECK_EQ(a.synthPresetActive(0), 0);   // the step-0 Z plays with the first block
    h.audio.pump(1);
    CHECK_EQ(a.synthPresetActive(0), 2);
    a.togglePlay(false);
}

// --- stopped: the slots follow the chase at the pattern-editor cursor

namespace {
void cursor(App& a, int step) {
    static_cast<PatternScreen*>(a.ui.screen(kModPattern))->step = step;
}
}  // namespace

TEST(stopped_pads_and_midi_play_the_chase_at_the_cursor_and_stop_reapplies_it) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    presets(a, {2, 5});
    note(a.pattern().tracks[0].steps[4], 64);
    z(a.pattern().tracks[0].steps[4], 1, 2);
    a.patternEdited();
    cursor(a, 3);
    h.run();
    CHECK_EQ(a.ui.patternCursorStep(), 3);
    CHECK_EQ(a.synthPresetActive(0), 0);
    cursor(a, 4);   // the cursor row counts
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 2);
    // A patch edit is heard on the patch until the next change of position.
    a.markSynthPatchDirty(0);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 0);
    cursor(a, 6);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 2);
    // Song mode: the positions before the playlist position come first.
    note(stored(a, 2, 3, 0), 65);
    z(stored(a, 2, 3, 0), 1, 5);
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 1;
    a.project.playlistPos = 1;
    a.seq.setSongMode(true);
    h.run();
    CHECK_EQ(a.synthPresetActive(1), 5);
    CHECK_EQ(a.synthPresetActive(0), 2);
    // A pad on Synth 1 plays the chased preset: the slot is on it while stopped.
    a.setCurrentInstrument(64);
    h.push(InputEvent::kPadDown, 10);
    CHECK_EQ(a.synthPresetActive(0), 2);
    h.push(InputEvent::kPadUp, 10);
    // Stop zeroes the slots' presets; the next pass puts the chase back.
    a.togglePlay(true);   // Shift+Play from position 1
    CHECK(a.seq.isPlaying());
    CHECK_EQ(a.synthPresetActive(1), 5);
    h.run();
    a.togglePlay(false);
    CHECK(!a.seq.isPlaying());
    CHECK_EQ(a.synthPresetActive(1), 0);
    h.run();
    CHECK_EQ(a.synthPresetActive(1), 5);
    a.seq.setSongMode(false);
    h.run();
    CHECK_EQ(a.synthPresetActive(1), 0);
}

TEST(warm_up_summarises_stale_patterns_in_the_background) {
    Harness h;
    App& a = *h.app;
    CHECK(a.newProject());
    for (int n = 2; n <= 5; ++n) note(stored(a, n, 0, 0), 64);
    for (int n = 2; n <= 5; ++n) CHECK(!(chase::mem().pat[n - 1].flags & chase::kValid));
    h.run(100);   // a few idle passes
    for (int n = 2; n <= 5; ++n) CHECK(chase::mem().pat[n - 1].flags & chase::kValid);
    CHECK_EQ(int(chase::mem().pat[1].last[0]), 64);
    // Not while playing.
    note(stored(a, 3, 0, 1), 65);
    a.togglePlay(false);
    h.run(100);
    CHECK(!(chase::mem().pat[2].flags & chase::kValid));
    a.togglePlay(false);
    h.run(100);
    CHECK(chase::mem().pat[2].flags & chase::kValid);
    CHECK_EQ(int(chase::mem().pat[2].last[0]), 65);
}
