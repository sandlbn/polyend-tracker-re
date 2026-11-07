// [3.x] Z chase: what the song selected before a position. A Play from the
// middle of the song (or of a pattern) starts each synth slot on the preset
// the Z commands before it left it on, and while stopped, pads and MIDI
// play the slot as the Z up to the cursor row left it. Steps are read as
// written: mutes, Chance and random FX are not followed.
#include "test.h"

#include <cstring>
#include <string>
#include <vector>

#include "formats/patch_pb.h"
#include "model/fx.h"
#include "modules/synth_models.h"
#include "ui/screens.h"
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
void cursorTo(Harness& h, int step) {
    PatternScreen& p = *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern));
    p.step = step;
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

TEST(stopped_pads_and_midi_play_the_chase_at_the_cursor_and_stop_reapplies_it) {
    Harness h;
    App& a = *h.app;
    storeAll(a);
    Pattern& p = a.pattern();
    putNote(p.tracks[0].steps[4], 60, 64);
    putZ(p.tracks[0].steps[4], 1, 2);
    putZ(p.tracks[0].steps[8], 1, 3);   // no note: track 1's Synth 1
    edited(a);
    cursorTo(h, 3);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 0);
    cursorTo(h, 4);   // the cursor row counts
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 2);
    cursorTo(h, 9);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 3);
    // Play from the top, then Stop: the slot is back on the cursor's preset.
    a.togglePlay(false);
    CHECK(a.seq.isPlaying());
    CHECK_EQ(a.synthPresetActive(0), 0);
    h.audio.pump(4);
    h.run();
    a.togglePlay(false);
    CHECK(!a.seq.isPlaying());
    cursorTo(h, 9);   // the cursor followed the playback
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 3);
    // An edit of the slot's patch is heard on that patch (no fight)...
    a.markSynthPatchDirty(0);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 0);
    // ...until the cursor moves to another preset.
    cursorTo(h, 5);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 2);
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

TEST(play_from_step_0_starts_on_own_even_with_the_cursor_after_a_z) {
    Harness h;
    App& a = *h.app;
    storeAll(a);
    Pattern& p = a.pattern();
    putNote(p.tracks[0].steps[1], 60, 64);
    putZ(p.tracks[0].steps[1], 1, 6);
    edited(a);
    cursorTo(h, 2);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 6);
    a.togglePlay(false);
    CHECK_EQ(a.synthPresetActive(0), 0);
    a.togglePlay(false);
}

TEST(warm_up_summarises_stale_patterns_in_the_background) {
    Harness h;
    App& a = *h.app;
    for (int n = 2; n <= 7; ++n) putNote(storedStep(a, n, 0, n), 60, 64);
    a.storeSynthPreset(1, 0);   // every summary is stale now
    for (int n = 1; n <= kPatternCount; ++n) CHECK(!a.synthChaseSummarised(n));
    h.run(10);
    int done = 0;
    for (int n = 2; n <= 7; ++n) done += a.synthChaseSummarised(n) ? 1 : 0;
    CHECK(done < 6);   // a few per pass, not all at once
    h.run(200);
    for (int n = 1; n <= kPatternCount; ++n) CHECK(a.synthChaseSummarised(n));
}

TEST(stopped_chase_follows_the_play_the_screen_would_start) {
    // Pattern-screen Play is the current pattern from step 0, also after a
    // song ran; the Song screen plays the song from its cursor.
    Harness h;
    App& a = *h.app;
    storeAll(a);
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 3;
    putNote(storedStep(a, 2, 0, 0), 60, 64);
    putZ(storedStep(a, 2, 0, 0), 1, 3);
    a.project.playlistPos = 1;
    a.togglePlay(true);
    h.audio.pump(2);
    h.run();
    a.togglePlay(false);
    CHECK(a.seq.isSongMode());   // left set by the song playback
    h.tap(btn::Pattern);
    cursorTo(h, 0);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 0);   // pattern mode: nothing before the cursor row
    h.tap(btn::Song);
    CHECK(a.ui.active() == kModSong);
    SongScreen& ss = *static_cast<SongScreen*>(a.ui.screen(kModSong));
    ss.cursor = 1;
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 3);   // position 0 before the Song cursor
    ss.cursor = 0;
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 0);
}

TEST(stopped_chase_waits_during_a_count_in) {
    Harness h;
    App& a = *h.app;
    storeAll(a);
    Pattern& p = a.pattern();
    putNote(p.tracks[0].steps[1], 60, 64);
    putZ(p.tracks[0].steps[1], 1, 4);
    edited(a);
    a.cfg.preRoll = 1;
    a.applySequencerSettings();
    a.seq.record();
    CHECK(a.seq.isCountingIn());
    cursorTo(h, 2);
    h.run();
    CHECK(a.seq.isCountingIn() || a.seq.isPlaying());
    CHECK_EQ(a.synthPresetActive(0), 0);
    a.togglePlay(false);
}

namespace {
// The slot's patch as a library patch Patches/<model>/<name>.patch.
void libraryPatch(Harness& h, int s, const char* name) {
    const SynthPatch& p = h.app->synthPatch(s);
    std::vector<uint8_t> buf(kPatchMaxFileSize);
    size_t n = 0;
    CHECK(encodePatch(p, buf.data(), buf.size(), &n) == PatchStatus::kOk);
    buf.resize(n);
    const std::string dir = std::string("Patches/") + synthEngineFolder(p.engine);
    h.fs.dirs.insert("Patches");
    h.fs.dirs.insert(dir);
    h.fs.files[dir + "/" + name + ".patch"] = buf;
}
}  // namespace

TEST(stopped_chase_sets_the_slots_again_after_a_midi_stop_while_armed) {
    Harness h;
    App& a = *h.app;
    storeAll(a);
    Pattern& p = a.pattern();
    putNote(p.tracks[0].steps[4], 60, 64);
    putZ(p.tracks[0].steps[4], 1, 3);
    edited(a);
    cursorTo(h, 6);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 3);
    a.seq.settings().clockIn = 1;       // USB clock: Play waits for a Start
    a.seq.settings().transportIn = 1;
    a.togglePlay(false);
    CHECK(a.seq.isArmed());
    CHECK(!a.seq.isPlaying());
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 3);
    a.seq.midiRealtimeIn(MidiPortId::kUsb, 0xFC);   // Stop: every slot to its own patch
    CHECK(!a.seq.isArmed());
    h.audio.pump(2);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 3);
    // And a MIDI Stop while stopped.
    a.seq.midiRealtimeIn(MidiPortId::kUsb, 0xFC);
    h.audio.pump(2);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 3);
}

TEST(stopped_chase_sets_a_slot_again_after_its_patch_preview_ends) {
    Harness h;
    App& a = *h.app;
    storeAll(a);
    for (int n = 1; n <= kSynthPresetCount; ++n) CHECK(a.synthPresetUsed(n));
    libraryPatch(h, 0, "Prev");
    Pattern& p = a.pattern();
    putNote(p.tracks[0].steps[4], 60, 64);
    putZ(p.tracks[0].steps[4], 1, 2);
    edited(a);
    cursorTo(h, 6);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 2);
    CHECK(a.previewSynthLibraryPatch(0, "Prev"));
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 0);   // the preview is heard
    a.endSynthPreview();
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 2);
    // Synth 2 on preset 4 too; a preview on it ends slot 0's: only the
    // slot that preview left is set again.
    putNote(p.tracks[1].steps[5], 60, 65);
    putZ(p.tracks[1].steps[5], 1, 4);
    edited(a);
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 2);
    CHECK_EQ(a.synthPresetActive(1), 4);
    libraryPatch(h, 1, "Prev2");
    CHECK(a.previewSynthLibraryPatch(0, "Prev"));
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 0);
    CHECK_EQ(a.synthPresetActive(1), 4);
    CHECK(a.previewSynthLibraryPatch(1, "Prev2"));
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 2);
    CHECK_EQ(a.synthPresetActive(1), 0);   // its preview is heard
    a.endSynthPreview();
    h.run();
    CHECK_EQ(a.synthPresetActive(0), 2);
    CHECK_EQ(a.synthPresetActive(1), 4);
}
