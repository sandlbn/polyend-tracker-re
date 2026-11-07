// [3.x] Z chase: what the song selected before a position. A Play from the
// middle of the song (or of a pattern) starts each synth slot on the preset
// the Z commands before it left it on, and while stopped, pads and MIDI
// play the slot as the Z up to the cursor row left it. Steps are read as
// written: mutes, Chance and random FX are not followed.
#include "test.h"

#include <cstring>

#include "model/fx.h"
#include "ui_fakes.h"

using namespace ot;

namespace {

void putNote(Step& s, int note, int instr) {
    s.note = int8_t(note);
    s.instrument = uint8_t(instr);
}
void putZ(Step& s, int col, int n) { s.fx[col] = StepFx{0, uint8_t(kFxPresetCmdBase + n)}; }   // col 1 = FX-A
void edited(App& a) { a.patternEdited(); }
// A stored pattern other than the current one (all 16 tracks writable).
Step& storedStep(App& a, int num, int t, int s) {
    a.markPatternDirty(num);
    return a.storedTrack(num, t).steps[s];
}
struct Chase {
    uint8_t z[kSynthSlotCount];
    uint8_t last[kTrackCount];
};
Chase chase(App& a, bool song, int pos, int lastStep) {
    Chase c;
    std::memset(c.z, 0, sizeof c.z);
    std::memset(c.last, 0xFF, sizeof c.last);
    a.synthChase(song, pos, a.patternNumber(), lastStep, c.z, c.last);
    return c;
}
// Presets 1..8 from slot 0's patch (all used).
void storeAll(App& a) {
    for (int n = 1; n <= kSynthPresetCount; ++n) a.storeSynthPreset(n, 0);
}

}  // namespace

TEST(chase_reads_the_current_pattern_up_to_the_cursor_fx_a_over_b) {
    Harness h;
    App& a = *h.app;
    storeAll(a);
    Pattern& p = a.pattern();
    putNote(p.tracks[0].steps[2], 60, 64);
    putZ(p.tracks[0].steps[2], 0, 1);   // FX-B first,
    putZ(p.tracks[0].steps[2], 1, 2);   // then FX-A: A wins
    putNote(p.tracks[5].steps[4], 62, 65);
    putZ(p.tracks[5].steps[4], 1, 3);
    edited(a);
    Chase c = chase(a, false, 0, 1);
    CHECK_EQ(int(c.z[0]), 0);
    c = chase(a, false, 0, 2);   // the bound is inclusive
    CHECK_EQ(int(c.z[0]), 2);
    CHECK_EQ(int(c.z[1]), 0);
    CHECK_EQ(int(c.last[0]), 64);
    CHECK_EQ(int(c.last[5]), 0xFF);
    c = chase(a, false, 0, 4);
    CHECK_EQ(int(c.z[1]), 3);
    CHECK_EQ(int(c.last[5]), 65);
    // A Z with no synth to act on (a sample step, nothing played before) is nothing.
    putNote(p.tracks[2].steps[0], 60, 3);
    putZ(p.tracks[2].steps[0], 1, 4);
    putZ(p.tracks[3].steps[0], 1, 5);
    edited(a);
    c = chase(a, false, 0, 0);
    CHECK_EQ(int(c.z[0]), 0);
    CHECK_EQ(int(c.z[1]), 0);
    CHECK_EQ(int(c.z[2]), 0);
}

TEST(no_note_z_uses_the_tracks_last_instrument_across_song_positions) {
    Harness h;
    App& a = *h.app;
    storeAll(a);
    for (int i = 0; i < 4; ++i) a.project.playlist[i] = uint8_t(i + 2);
    putNote(storedStep(a, 2, 3, 0), 60, 66);   // track 4 plays Synth 3
    putZ(storedStep(a, 3, 3, 1), 1, 5);        // no note: the synth track 4 played before
    putNote(storedStep(a, 3, 7, 0), 60, 64);
    putZ(storedStep(a, 3, 7, 5), 1, 6);        // no note, after the track's own note
    Chase c = chase(a, true, 2, -1);
    CHECK_EQ(int(c.z[2]), 5);
    CHECK_EQ(int(c.z[0]), 6);
    CHECK_EQ(int(c.last[3]), 66);
    CHECK_EQ(int(c.last[7]), 64);
    // Asked again (the summaries are cached now): the same answer.
    c = chase(a, true, 2, -1);
    CHECK_EQ(int(c.z[2]), 5);
    CHECK_EQ(int(c.z[0]), 6);
    // Without the note before it the no-note Z has nothing to act on.
    a.project.playlist[0] = 9;
    a.markProjectDirty();
    c = chase(a, true, 2, -1);
    CHECK_EQ(int(c.z[2]), 0);
    CHECK_EQ(int(c.z[0]), 6);
}

TEST(empty_preset_z_is_a_no_op_and_store_clear_invalidate_summaries) {
    Harness h;
    App& a = *h.app;
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 3;
    putNote(storedStep(a, 2, 0, 0), 60, 64);
    putZ(storedStep(a, 2, 0, 0), 1, 4);
    Chase c = chase(a, true, 1, -1);
    CHECK_EQ(int(c.z[0]), 0);   // preset 4 is empty
    a.storeSynthPreset(4, 1);
    c = chase(a, true, 1, -1);
    CHECK_EQ(int(c.z[0]), 4);
    a.clearSynthPreset(4);
    c = chase(a, true, 1, -1);
    CHECK_EQ(int(c.z[0]), 0);
    // Z0 is always there.
    a.storeSynthPreset(2, 0);
    putNote(storedStep(a, 2, 0, 1), 60, 64);
    putZ(storedStep(a, 2, 0, 1), 1, 2);
    putZ(storedStep(a, 2, 0, 3), 1, 0);   // no note: track 1's Synth 1
    c = chase(a, true, 1, -1);
    CHECK_EQ(int(c.z[0]), 0);
}

TEST(chase_ignores_chance_and_mutes_and_zero_playlist_entries) {
    Harness h;
    App& a = *h.app;
    storeAll(a);
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 0;   // an empty entry before the position: skipped
    a.project.playlist[2] = 3;
    a.project.playlist[3] = 4;
    Step& s = storedStep(a, 2, 1, 0);
    putNote(s, 60, 65);
    s.fx[0] = StepFx{4, 0};   // Chance 0 in FX-B
    putZ(s, 1, 7);
    putNote(storedStep(a, 3, 2, 3), 60, 66);
    putZ(storedStep(a, 3, 2, 3), 1, 8);
    a.setTrackMute(2, true);
    Chase c = chase(a, true, 3, -1);
    CHECK_EQ(int(c.z[1]), 7);
    CHECK_EQ(int(c.z[2]), 8);
}

TEST(summary_is_rebuilt_after_a_pattern_edit_undo_and_track_copy) {
    Harness h;
    App& a = *h.app;
    storeAll(a);
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 3;
    putNote(storedStep(a, 2, 0, 0), 60, 64);
    putZ(storedStep(a, 2, 0, 0), 1, 1);
    CHECK_EQ(int(chase(a, true, 1, -1).z[0]), 1);
    CHECK(a.synthChaseSummarised(2));
    // An edit through the store.
    a.pushFileUndo(2);
    putZ(a.storedTrack(2, 0).steps[0], 1, 2);
    CHECK(!a.synthChaseSummarised(2));
    CHECK_EQ(int(chase(a, true, 1, -1).z[0]), 2);
    // Its undo.
    uint8_t num = 0;
    CHECK(a.popFileUndo(&num));
    CHECK_EQ(int(num), 2);
    CHECK_EQ(int(chase(a, true, 1, -1).z[0]), 1);
    // A whole pattern written over it.
    Pattern p;
    a.assemblePattern(2, p);
    putZ(p.tracks[0].steps[0], 1, 3);
    putNote(p.tracks[14].steps[2], 60, 65);   // an ext track
    putZ(p.tracks[14].steps[2], 1, 4);
    CHECK(a.storePattern(2, p));
    Chase c = chase(a, true, 1, -1);
    CHECK_EQ(int(c.z[0]), 3);
    CHECK_EQ(int(c.z[1]), 4);
    // The current pattern edited in the editor (it is position 0 here).
    a.selectPattern(2);
    putZ(a.pattern().tracks[0].steps[0], 1, 5);
    edited(a);
    CHECK_EQ(int(chase(a, true, 1, -1).z[0]), 5);
}

TEST(shift_play_mid_song_starts_on_the_chased_preset) {
    Harness h;
    App& a = *h.app;
    storeAll(a);
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 3;
    putNote(storedStep(a, 2, 0, 0), 60, 64);
    putZ(storedStep(a, 2, 0, 0), 1, 3);
    a.project.playlistPos = 1;
    a.togglePlay(true);
    CHECK(a.seq.isPlaying());
    CHECK(a.seq.isSongMode());
    CHECK_EQ(a.synthPresetActive(0), 3);
    a.togglePlay(false);
    // From position 0 every slot starts on its own patch.
    a.project.playlistPos = 0;
    a.togglePlay(true);
    CHECK_EQ(a.synthPresetActive(0), 0);
    a.togglePlay(false);
}
