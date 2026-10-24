// Pattern editor (spec 06 §1): FX value display (03 §3.6), live-record
// transport (06 §1.4/§1.8), arrangement arrows (06 §1.5), column-view mask
// (06 §1.9), PERF marker, hold popups, Fill and focus rules.
#include <cstring>
#include <string>

#include "test.h"
#include "ui_fakes.h"

#include "app/project_fields.h"
#include "sequencer/seq_tables.h"
#include "ui/fx_text.h"
#include "ui/screens.h"

using namespace ot;

static PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }

static std::string val(FxId t, uint8_t v) {
    char b[5];
    fxValueText(uint8_t(t), v, b);
    return b;
}
static std::string cell(FxId t, uint8_t v) {
    char b[8];
    fxCellText(uint8_t(t), v, b);
    return b;
}

TEST(fx_value_text_transforms) {
    CHECK(val(FxId::Tempo, 60) == "120");
    CHECK(val(FxId::Tempo, 200) == "400");
    CHECK(val(FxId::Tempo, 4) == "STP");
    CHECK(val(FxId::Roll, 1) == "  1");
    CHECK(val(FxId::Roll, 9 * 2 + 3) == "V 3");
    CHECK(val(FxId::Roll, 53) == "R16");
    CHECK(val(FxId::Roll, 0) == " --");
    CHECK(val(FxId::MidiChord, 0) == "-");
    CHECK(val(FxId::MidiChord, 5) == "47");
    CHECK(val(FxId::MidiChord, 12) == "7E");
    CHECK(val(FxId::MidiChord, 29) == "57B");
    CHECK(val(FxId::Arp, 1) == "/ 8");
    CHECK(val(FxId::Arp, 12) == "\\ 8");
    CHECK(val(FxId::Arp, 33) == "R.8");
    CHECK(val(FxId::Panning, 0) == "-50");
    CHECK(val(FxId::Panning, 100) == "50");
    CHECK(val(FxId::MicroTune, 0) == "-99");
    CHECK(val(FxId::MicroTune, 198) == "99");
    CHECK(val(FxId::Tune, 0) == "-24");
    CHECK(val(FxId::Tune, 48) == "24");
    CHECK(val(FxId::Slice, 0) == "1");
    CHECK(val(FxId::Slice, 47) == "48");
    CHECK(val(FxId::VolumeLfo, 0) == "24");
    CHECK(val(FxId::VolumeLfo, 23) == "/64");
    CHECK(val(FxId::PanningLfo, 0) == "128");
    CHECK(val(FxId::FilterLfo, 28) == "/64");
    CHECK(val(FxId::FinetuneLfo, 20) == ".19");
    CHECK(val(FxId::BreakPattern, 1) == "BRK");
    CHECK(val(FxId::Off, 0) == "OFF");
    CHECK(val(FxId::ReversePlayback, 0) == ">>>");
    CHECK(val(FxId::ReversePlayback, 1) == "<<<");
    CHECK(val(FxId::Swing, 50) == "50");
    CHECK(val(FxId::Volume, 7) == "7");
    CHECK(val(FxId::BreakPattern, 0) == "---");
    // Cell text (spec 16 §A.8): symbol + right-aligned 3-char value.
    CHECK(cell(FxId::Volume, 50) == "V 50");
    CHECK(cell(FxId::Volume, 100) == "V100");
    CHECK(cell(FxId::Panning, 25) == "P-25");
    CHECK(cell(FxId::Tempo, 4) == "TSTP");
    CHECK(cell(FxId::Off, 0) == "!OFF");
    CHECK(cell(FxId::Roll, 9 + 2) == "Rv 2");
    CHECK(cell(FxId::Roll, 53) == "RR16");
    CHECK(cell(FxId::None, 0) == "----");
    for (int t = 1; t < kFxCount; ++t)
        for (int v = 0; v < 256; v += 17) CHECK(cell(FxId(t), uint8_t(v)).size() == 4);
}

TEST(grid_cell_text_two_digit_instrument) {
    Harness h;
    App& a = *h.app;
    Step& s = a.pattern().tracks[0].steps[0];
    s.note = 60;
    s.instrument = 4;
    s.fx[0].type = uint8_t(FxId::Tempo);
    s.fx[0].value = 4;
    s.fx[1].type = uint8_t(FxId::Panning);
    s.fx[1].value = 25;
    Step& m = a.pattern().tracks[1].steps[0];
    m.note = 62;
    m.instrument = 48;
    a.ui.dirty();
    h.run(40);
    // One glyph run per column, each in its column colour (spec 20 §4.1).
    for (const char* t : {"\"C5\"", "\"05\"", "\"TSTP\"", "\"P-25\"", "\"D5\"", "\"M01\""})
        CHECK(h.disp.lastHasText(t));
}

TEST(rec_while_recording_keeps_playing_and_punch_in) {
    Harness h;
    App& a = *h.app;
    Sequencer& sq = a.seq;
    // Rec held + Play while idle: start live recording, edit mode off.
    h.down(btn::Rec);
    h.tap(btn::Play);
    h.up(btn::Rec);
    CHECK(sq.isRecording());
    CHECK(sq.isPlaying());
    CHECK(!a.editMode);
    h.audio.pump(200);
    // Rec while recording: recording off, transport keeps playing.
    h.tap(btn::Rec);
    CHECK(!sq.isRecording());
    CHECK(sq.isPlaying());
    CHECK(!a.editMode);
    h.audio.pump(200);
    const int before = sq.masterStep();
    CHECK(before > 0);
    // Rec held + Play while playing: punch in without restart.
    h.down(btn::Rec);
    h.tap(btn::Play);
    h.up(btn::Rec);
    CHECK(sq.isRecording());
    CHECK(sq.isPlaying());
    CHECK(!a.editMode);
    CHECK(sq.masterStep() >= before);
    // Play while playing stops.
    h.tap(btn::Play);
    CHECK(!sq.isPlaying());
}

TEST(rec_toggles_edit_mode_when_not_recording) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Rec);
    CHECK(a.editMode);
    h.tap(btn::Play);   // plain Play: play from step 0, edit mode kept
    CHECK(a.seq.isPlaying());
    CHECK(!a.seq.isRecording());
    h.tap(btn::Rec);
    CHECK(!a.editMode);
    CHECK(a.seq.isPlaying());
    h.tap(btn::Play);
    CHECK(!a.seq.isPlaying());
}

TEST(horizontal_arrangement_arrow_table) {
    Harness h;
    App& a = *h.app;
    a.cfg.arrangement = 2;
    PatternScreen& p = pe(h);
    h.tap(btn::Down);   // track + 1
    CHECK_EQ(p.track, 1);
    CHECK_EQ(p.step, 0);
    h.tap(btn::Right);  // step + 1
    CHECK_EQ(p.step, 1);
    h.tap(btn::Left);   // step - 1
    CHECK_EQ(p.step, 0);
    h.tap(btn::Left);   // wraps to len - 1
    CHECK_EQ(p.step, a.pattern().tracks[0].lastStep);
    h.tap(btn::Up);     // track - 1
    CHECK_EQ(p.track, 0);
    // Shift + Right extends the selection along the steps.
    h.tap(btn::Right);
    h.down(btn::Shift);
    h.tap(btn::Right);
    h.tap(btn::Right);
    h.up(btn::Shift);
    const SelRect r = p.target();
    CHECK_EQ(r.s0, 0);
    CHECK_EQ(r.s1, 2);
    CHECK_EQ(r.t0, 0);
    CHECK_EQ(r.t1, 0);
    // Vertical arrangements: Down = step + 1.
    a.cfg.arrangement = 1;
    h.tap(btn::Down);
    CHECK_EQ(p.track, 0);
    CHECK_EQ(p.step, 3);
}

TEST(column_view_mask_eight_tracks) {
    Harness h;
    App& a = *h.app;
    PatternScreen& p = pe(h);
    Step& s = a.pattern().tracks[7].steps[0];
    s.note = 60;
    s.fx[0].type = uint8_t(FxId::Volume);
    s.fx[0].value = 50;
    h.down(btn::Pattern);
    h.tap(btn::Fx1);
    h.tap(btn::Fx2);
    h.up(btn::Pattern);
    CHECK_EQ(p.colMask, uint8_t((1u << 2) | (1u << 3)));
    a.ui.dirty();
    h.run(40);
    // Track 8 visible; only the two FX columns.
    CHECK(h.disp.lastHasText("\"Track 8\""));
    CHECK(h.disp.lastHasText("\"V 50\""));
    CHECK(!h.disp.lastHasText("\"C5\""));
    // Cursor to track 12: first visible track 4 (0..4 with a mask).
    for (int i = 0; i < 11; ++i) h.tap(btn::Right);
    CHECK_EQ(p.firstTrack, 4);
    // Encoder edits only when the cursor column is in the mask.
    h.tap(btn::Rec);
    h.tap(btn::Note);   // re-targets the mask to the Note column
    CHECK_EQ(p.colMask, uint8_t(1u << 0));
    h.down(btn::Pattern);
    h.tap(btn::Instr);
    h.up(btn::Pattern);
    CHECK_EQ(p.colMask, uint8_t(1u << 1));
    CHECK_EQ(p.col, 0);
    h.enc(1);   // Note column not in the mask: no edit
    CHECK(a.pattern().tracks[11].steps[0].note == kNoteEmpty);
    // Note + Instr: 3+1+3 chars.
    h.down(btn::Pattern);
    h.tap(btn::Note);
    h.tap(btn::Instr);
    h.up(btn::Pattern);
    CHECK_EQ(p.colMask, uint8_t(3));
    h.enc(1);
    CHECK(a.pattern().tracks[11].steps[0].note == 60);
    // Pattern + the same column twice clears the mask.
    h.down(btn::Pattern);
    h.tap(btn::Note);
    h.tap(btn::Note);
    h.up(btn::Pattern);
    CHECK_EQ(p.colMask, 0);
}

TEST(perf_marker_in_top_bar) {
    Harness h;
    App& a = *h.app;
    a.ui.dirty();
    h.run(40);
    CHECK(!h.disp.lastHasText("PERF"));
    static PerfPatternMemory mem;   // the app may already provide one
    a.seq.setPerfPatternMemory(&mem);
    CHECK(a.seq.setTrackPattern(0, 2, true));
    {
        a.seq.setPerformanceMode(true);
        CHECK(pe(h).perfMarker());
        a.ui.dirty();
        h.run(40);
        CHECK(h.disp.lastHasText("PERF"));
    }
}

TEST(column_key_hold_popup_timings) {
    // The harness delivers a press one 10 ms tick late and the update loop
    // runs every >= 20 ms, so the checks leave that much slack.
    Harness h;
    (void)h.app;
    h.down(btn::Note);
    h.run(440);   // < 500 ms
    CHECK(!pe(h).popup().isOpen());
    h.run(100);
    CHECK(pe(h).popup().isOpen());
    h.up(btn::Note);
    CHECK(!pe(h).popup().isOpen());
    h.down(btn::Instr);
    h.run(130);   // < 180 ms
    CHECK(!pe(h).popup().isOpen());
    h.run(80);
    CHECK(pe(h).popup().isOpen());
    h.up(btn::Instr);
    h.down(btn::Fx1);
    h.run(130);
    CHECK(!pe(h).popup().isOpen());
    h.run(80);
    CHECK(pe(h).popup().isOpen());
    h.up(btn::Fx1);
}

TEST(fill_rekey_wraps_and_note_defaults) {
    Harness h;
    App& a = *h.app;
    PatternScreen& p = pe(h);
    h.tap(btn::Rec);
    h.tap(btn::F4);
    CHECK(p.fillOpen);
    CHECK_EQ(p.fill[0].from, a.rootNote() + 12);
    CHECK_EQ(p.fill[0].scaleOrFx, a.scale());
    h.tap(btn::F5);   // select From
    for (int i = 0; i < 20; ++i) h.enc(10);
    CHECK_EQ(p.fill[0].from, 127);   // encoder clamps
    h.tap(btn::F5);   // re-press: +1, wraps to the minimum
    CHECK_EQ(p.fill[0].from, 0);
    p.fill[0].where = 5;   // Each: F2 = Step
    h.tap(btn::F2);
    p.fill[0].param[5] = 128;
    h.tap(btn::F2);
    CHECK_EQ(p.fill[0].param[5], 1);
    h.tap(btn::F7);
    CHECK(!p.fillOpen);
}

TEST(focus_blocks_note_entry_and_pattern_right) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Rec);
    h.down(btn::F6);   // Undo focus
    h.pad(36);
    h.up(btn::F6);
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
    // Pattern focus (spec 16 §A.2): Right/Up = +1, Left/Down = -1 (stopped: at once).
    h.down(btn::F1);
    h.tap(btn::Right);
    CHECK_EQ(a.patternNumber(), 2);
    h.tap(btn::Up);
    CHECK_EQ(a.patternNumber(), 3);
    h.tap(btn::Left);
    CHECK_EQ(a.patternNumber(), 2);
    h.tap(btn::Down);
    CHECK_EQ(a.patternNumber(), 1);
    h.up(btn::F1);
}

TEST(pattern_up_down_switch_immediately_while_playing) {
    // spec 16 §A.2: Pattern + Up/Down = pattern +1/-1 at once, also while
    // playing (step position kept); Pattern + Left/Right queue.
    Harness h;
    App& a = *h.app;
    h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    h.audio.pump(300);
    h.down(btn::Pattern);
    h.tap(btn::Up);
    h.audio.pump(1);
    CHECK_EQ(a.patternNumber(), 2);
    CHECK_EQ(a.seq.queuedPattern(), 0);
    h.tap(btn::Up);
    h.tap(btn::Up);     // two presses inside one block both count
    h.audio.pump(1);
    CHECK_EQ(a.patternNumber(), 4);
    const int st = a.seq.step(0);
    h.tap(btn::Down);
    h.audio.pump(1);
    CHECK_EQ(a.patternNumber(), 3);
    CHECK(a.seq.isPlaying());
    CHECK(a.seq.step(0) >= st);         // no restart from step 0
    h.tap(btn::Right);
    h.audio.pump(1);
    CHECK_EQ(a.patternNumber(), 3);     // Right only queues
    CHECK_EQ(a.seq.queuedPattern(), 4);
    h.up(btn::Pattern);
    h.tap(btn::Play);
}

TEST(fx_values_use_engine_ranges) {
    Harness h;
    App& a = *h.app;
    PatternScreen& p = pe(h);
    h.tap(btn::Rec);
    p.col = 2;
    Step& s = a.pattern().tracks[0].steps[0];
    s.fx[0].type = uint8_t(FxId::Roll);
    s.fx[0].value = 40;
    for (int i = 0; i < 3; ++i) h.enc(10);
    CHECK_EQ(s.fx[0].value, 53);   // 0..53, not tracker-lib 47
    s.fx[0].type = uint8_t(FxId::MidiChord);
    s.fx[0].value = 0;
    h.pad(47);
    CHECK_EQ(s.fx[0].value, 29);   // pad 47 = max (0..29)
    CHECK_EQ(seq::fxMax(uint8_t(FxId::BitDepth)), 16);
}

// ---------------------------------------------------------------------------
// 2.x M1 (docs/M1_PLAN.md §4, 2x-01 §2): tracks 13..16 in the pattern editor.

// The 4-track window scrolls to tracks 13..16 like it reaches 9..12 (first
// shown track 0..12; 0..8 with a column mask); Right at track 16 stops.
TEST(tracks_13_16_window_and_tabs) {
    Harness h;
    App& a = *h.app;
    PatternScreen& p = pe(h);
    for (int i = 0; i < 20; ++i) h.tap(btn::Right);
    CHECK_EQ(p.track, kTrackCount - 1);
    CHECK_EQ(p.firstTrack, kTrackCount - 4);
    a.ui.dirty();
    h.run(40);
    for (const char* nm : {"\"Midi 13\"", "\"Midi 14\"", "\"Midi 15\"", "\"Midi 16\""}) CHECK(h.disp.lastHasText(nm));
    CHECK(!h.disp.lastHasText("\"Midi 12\""));
    // Back left: the window follows one track at a time, as on 1..12.
    for (int i = 0; i < 4; ++i) h.tap(btn::Left);
    CHECK_EQ(p.track, 11);
    CHECK_EQ(p.firstTrack, 11);
    // Column mask: 8 tracks, first shown track at most 8.
    for (int i = 0; i < 4; ++i) h.tap(btn::Right);
    h.down(btn::Pattern);
    h.tap(btn::Fx1);
    h.up(btn::Pattern);
    CHECK(p.colMask != 0);
    h.tap(btn::Left);
    h.tap(btn::Right);
    CHECK_EQ(p.firstTrack, kTrackCount - 8);
    a.ui.dirty();
    h.run(40);
    CHECK(h.disp.lastHasText("\"Midi 9\""));
    CHECK(h.disp.lastHasText("\"Midi 16\""));
    // Horizontal arrangement keeps its window (rows = the same tracks).
    h.down(btn::Pattern);
    h.tap(btn::Fx1);
    h.tap(btn::Fx1);
    h.up(btn::Pattern);
    CHECK_EQ(p.colMask, 0);
    a.cfg.arrangement = 2;
    h.tap(btn::Down);   // track + 1 (already the last)
    CHECK_EQ(p.track, kTrackCount - 1);
    CHECK_EQ(p.firstTrack, kTrackCount - 4);
}

// Mute (Shift) and Live-REC arm (Shift + Rec) pages: bank B (cursor on 9..16)
// puts tracks 9..16 on F1..F8, so F5..F8 reach 13..16.
TEST(tracks_13_16_mute_and_arm_pages) {
    Harness h;
    App& a = *h.app;
    PatternScreen& p = pe(h);
    for (int i = 0; i < 13; ++i) h.tap(btn::Right);
    CHECK_EQ(p.track, 13);
    h.down(btn::Shift);
    h.run(20);
    CHECK_STR(a.ui.bar.label[0].top, "Midi 9");
    CHECK_STR(a.ui.bar.label[4].top, "Midi 13");
    CHECK_STR(a.ui.bar.label[7].top, "Midi 16");
    CHECK(!a.ui.bar.label[7].hidden);
    CHECK_STR(a.ui.bar.label[7].bottom, a.trackMuted(15) ? "Unmute" : "Mute");
    h.tap(btn::Rec);   // arm page
    CHECK(p.recArmPage);
    CHECK(p.armed[12] && p.armed[15]);
    h.tap(btn::F5);
    h.tap(btn::F8);
    CHECK(!p.armed[12]);
    CHECK(!p.armed[15]);
    CHECK(p.armed[13] && p.armed[11]);
    CHECK_STR(a.ui.bar.label[4].bottom, "Unarmed");
    h.up(btn::Shift);
    // Bank A on tracks 1..8.
    for (int i = 0; i < 13; ++i) h.tap(btn::Left);
    h.down(btn::Shift);
    h.run(20);
    CHECK_STR(a.ui.bar.label[0].top, "Track 1");
    h.up(btn::Shift);
}

// Selections, copy / paste, note entry and Fill across 16 tracks; tracks
// 13..16 are stored in the ext-track pool.
TEST(tracks_13_16_selection_copy_paste_entry) {
    Harness h;
    App& a = *h.app;
    PatternScreen& p = pe(h);
    // Shift+Up at step 0 twice: all 16 tracks.
    h.down(btn::Shift);
    h.tap(btn::Up);
    h.tap(btn::Up);
    h.up(btn::Shift);
    SelRect r = p.target();
    CHECK_EQ(r.t0, 0);
    CHECK_EQ(r.t1, kTrackCount - 1);
    // Whole column at track 16, Shift+Right extends to all tracks.
    for (int i = 0; i < 16; ++i) h.tap(btn::Right);
    h.down(btn::Shift);
    h.tap(btn::Up);
    CHECK(p.wholeCol);
    h.tap(btn::Right);
    h.up(btn::Shift);
    CHECK(p.allTracks);
    // Note entry on track 13 (edit mode).
    h.tap(btn::Left);   // collapses, cursor to track 15
    for (int i = 0; i < 2; ++i) h.tap(btn::Left);
    CHECK_EQ(p.track, 12);
    h.tap(btn::Rec);
    CHECK(p.edit());
    a.setCurrentInstrument(kFirstMidiInstrument + 12);   // MIDI 13 = "M13"
    a.cfg.midiOut = 3;   // USB + jack
    const int n0 = h.midi.sent.size();
    h.pad(10);
    CHECK(int(h.midi.sent.size()) > n0);   // audition on track 13 (MIDI out)
    const Step& e = a.pattern().tracks[12].steps[0];
    CHECK(e.note >= 0);
    CHECK_EQ(int(e.instrument), kFirstMidiInstrument + 12);
    const int num = a.patternNumber();
    const Track* st = a.findStoredTrack(num, 12);
    CHECK(st != nullptr);
    if (st) CHECK_EQ(int(st->steps[0].note), int(e.note));
    a.ui.dirty();
    h.run(40);
    CHECK(h.disp.lastHasText("\"M13\""));
    // Copy tracks 13..14, step 0 and paste at track 15: the paste clamps at
    // track 16.
    p.step = 0;
    h.down(btn::Shift);
    h.tap(btn::Right);
    h.up(btn::Shift);
    r = p.target();
    CHECK_EQ(r.t0, 12);
    CHECK_EQ(r.t1, 13);
    h.tap(btn::Copy);
    CHECK(a.clipboardValid());
    h.tap(btn::Right);   // collapse, cursor to track 15
    CHECK_EQ(p.track, 14);
    p.step = 0;
    h.down(btn::Shift);
    h.tap(btn::Copy);    // paste
    h.up(btn::Shift);
    CHECK_EQ(int(a.pattern().tracks[14].steps[0].note), int(e.note));
    CHECK(a.pattern().tracks[15].steps[0].note == kNoteEmpty);
    CHECK(a.findStoredTrack(num, 14) != nullptr);
    // Fill over all 16 tracks (constant note).
    pops::FillParams f;
    f.where = 1;   // empty steps
    f.from = f.to = 62;
    SelRect all;
    all.s0 = 0;
    all.s1 = 3;
    all.t0 = 0;
    all.t1 = kTrackCount - 1;
    pops::fill(a.pattern(), all, 0, f, 0, uint8_t(kFirstMidiInstrument), nullptr, nullptr);
    for (int t = 0; t < kTrackCount; ++t) CHECK_EQ(int(a.pattern().tracks[t].steps[3].note), 62);
    // Pattern copy / paste carries all 16 tracks.
    a.patternEdited();
    p.page = 1;
    h.tap(btn::F6);     // Copy pattern
    a.pattern().tracks[15].steps[3].note = kNoteEmpty;
    h.tap(btn::F7);     // Paste pattern
    CHECK_EQ(int(a.pattern().tracks[15].steps[3].note), 62);
}

TEST(slide_description_says_16_is_a_semitone) {
    // Manual p.178-179 "A value of 16 will slide 1 Semitone" (MANUAL_AUDIT #22).
    CHECK(std::strstr(fxDescription(uint8_t(FxId::SlideUp)), "16 = 1 semitone") != nullptr);
    CHECK(std::strstr(fxDescription(uint8_t(FxId::SlideDown)), "16 = 1 semitone") != nullptr);
}

TEST(fx_help_ends_with_the_range) {
    // MANUAL_AUDIT_2026-10-23 #23 (manual p.142 / spec 16 §B.4: the FX
    // description window ends with the value range).
    const char* p = fxHelpText(uint8_t(FxId::Panning));
    CHECK(std::strstr(p, "From -50 to 50") != nullptr);
    CHECK(std::strstr(fxHelpText(uint8_t(FxId::BitDepth)), "From 4 to 16") != nullptr);
    CHECK(std::strstr(fxHelpText(uint8_t(FxId::None)), "From") == nullptr);   // no range
}
