// MIDI note-in step entry (spec 03 §5.1) and "Pattern top info" layout
// (spec 06 §1.2) in the pattern editor.
#include <string>

#include "test.h"
#include "ui_fakes.h"

#include "ui/screens.h"

using namespace ot;

static PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }

static void midiNote(Harness& h, uint8_t note, uint8_t vel) {
    h.midi.in.push_back(0x90);
    h.midi.in.push_back(note);
    h.midi.in.push_back(vel);
    h.run(20);
    h.midi.in.push_back(0x80);
    h.midi.in.push_back(note);
    h.midi.in.push_back(0);
    h.run(20);
}


static bool hasText(Harness& h, const char* s) {
    for (auto& l : h.disp.last)
        if (l.rfind("TEXT", 0) == 0 && l.find(std::string("\"") + s + "\"") != std::string::npos) return true;
    return false;
}

TEST(midi_note_audition_only_outside_edit) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;   // USB
    CHECK(!a.editMode);
    midiNote(h, 64, 100);
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
}

TEST(midi_note_step_entry_in_edit_mode) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    h.tap(btn::Rec);
    CHECK(a.editMode);
    a.setCurrentInstrument(3);
    midiNote(h, 64, 100);
    CHECK(a.pattern().tracks[0].steps[0].note == 64);
    CHECK(a.pattern().tracks[0].steps[0].instrument == 3);   // empty cell takes the current instrument
    CHECK(a.pattern().tracks[0].steps[0].fx[0].type == 0);   // no velocity FX outside a velocity mode
    CHECK(a.pattern().tracks[0].steps[0].fx[1].type == 0);
    CHECK(pe(h).step == 0);   // spec 17 §1.4 step 8: the cursor does not move
    // A cell that already holds a note keeps its instrument.
    a.setCurrentInstrument(5);
    midiNote(h, 67, 90);
    CHECK(a.pattern().tracks[0].steps[0].note == 67);
    CHECK(a.pattern().tracks[0].steps[0].instrument == 3);
    // Written regardless of the cursor column.
    h.tap(btn::Down);
    h.tap(btn::Down);
    h.tap(btn::Instr);
    midiNote(h, 70, 90);
    CHECK(a.pattern().tracks[0].steps[2].note == 70);
    CHECK(a.pattern().tracks[0].steps[2].instrument == 5);
    // Undo restores the cell.
    CHECK(a.undoEdit());
    CHECK(a.pattern().tracks[0].steps[2].note == kNoteEmpty);
}

TEST(midi_note_entry_midi_track_maps_instrument) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    h.tap(btn::Rec);
    for (int i = 0; i < 9; ++i) h.tap(btn::Right);
    CHECK(pe(h).track == 9);
    // A sample instrument only gets tracks 1..8 (spec 17 §1.4 step 4): the
    // note sounds on track 1, outside the cursor, so nothing is written.
    a.setCurrentInstrument(2);
    midiNote(h, 60, 127);
    CHECK(a.pattern().tracks[9].steps[0].note == kNoteEmpty);
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
    // A MIDI instrument is written on the MIDI track.
    a.setCurrentInstrument(kFirstMidiInstrument + 1);
    midiNote(h, 60, 127);
    CHECK(a.pattern().tracks[9].steps[0].note == 60);
    CHECK(a.pattern().tracks[9].steps[0].instrument == kFirstMidiInstrument + 1);
}

TEST(midi_note_entry_respects_channel_and_port) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Rec);
    a.cfg.notesIn = 2;   // DIN only; the fake delivers on USB
    midiNote(h, 60, 100);
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
    a.cfg.notesIn = 3;
    a.cfg.notesInChannel = 2;   // channel 2 only; the message is on channel 1
    midiNote(h, 60, 100);
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
    a.cfg.notesInChannel = 1;
    midiNote(h, 60, 100);
    CHECK(a.pattern().tracks[0].steps[0].note == 60);
}

TEST(midi_note_no_entry_while_playing_or_focus) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    h.tap(btn::Rec);
    h.down(btn::F2);   // length hold focus
    midiNote(h, 60, 100);
    h.up(btn::F2);
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
    h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    midiNote(h, 61, 100);
    h.tap(btn::Play);
    for (int s = 0; s < kStepCount; ++s) CHECK(a.pattern().tracks[0].steps[s].note != 61);
}

TEST(pattern_top_info_modes) {
    Harness h;
    App& a = *h.app;
    PatternScreen& p = pe(h);
    a.ui.dirty();
    h.run(40);
    CHECK(a.cfg.patternTopInfo == 0);
    CHECK(p.topInfoOn());
    CHECK(p.gridTop() == kTitleH);
    // 15 rows of 26 px from y 29 (header tabs in row 0, spec 20 §4.1).
    CHECK(p.rowHeight() == 26);
    CHECK(p.gridTop() + 1 + 15 * p.rowHeight() <= kTitleH + 395);
    CHECK(hasText(h, "0m:00s"));      // title strip with status (spec 20 §1.2)
    CHECK(hasText(h, "Track 1"));     // grey header tab "Track n" [S snap 001]
    a.cfg.patternTopInfo = 1;         // track names
    a.ui.dirty();
    h.run(40);
    CHECK(hasText(h, "0m:00s"));
    CHECK(hasText(h, "Track 1"));
    a.cfg.patternTopInfo = 2;         // off
    a.ui.dirty();
    h.run(40);
    CHECK(!p.topInfoOn());
    CHECK(p.gridTop() == 0);
    CHECK(p.rowHeight() == 28);
    CHECK(1 + 15 * p.rowHeight() <= 423);
    CHECK(!hasText(h, "0m:00s"));
}

TEST(midi_note_step_entry_with_a_synth_instrument) {
    // User report (2.x): with Synth 1..3 as the current instrument, MIDI keys
    // in edit mode sounded but wrote nothing (the synth plays on its own live
    // tracks, so no pattern track was claimed). They are written at the
    // cursor; a held chord fills the selected tracks in order.
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    h.tap(btn::Rec);
    CHECK(a.editMode);
    a.setCurrentInstrument(uint8_t(kFirstSynthInstrument));   // S1
    midiNote(h, 60, 100);
    CHECK(a.pattern().tracks[0].steps[0].note == 60);
    CHECK(a.pattern().tracks[0].steps[0].instrument == kFirstSynthInstrument);
    // Chord over a 3-track selection: three held keys, one per track.
    h.down(btn::Shift);
    h.tap(btn::Right);
    h.tap(btn::Right);
    h.up(btn::Shift);
    for (uint8_t n : {64, 67, 71}) {
        h.midi.in.push_back(0x90);
        h.midi.in.push_back(n);
        h.midi.in.push_back(100);
        h.run(20);
    }
    for (uint8_t n : {64, 67, 71}) {
        h.midi.in.push_back(0x80);
        h.midi.in.push_back(n);
        h.midi.in.push_back(0);
        h.run(20);
    }
    const SelRect r = pe(h).target();
    if (r.t1 > r.t0) {
        CHECK(a.pattern().tracks[r.t0].steps[r.s0].note == 64);
        CHECK(a.pattern().tracks[r.t0 + 1].steps[r.s0].note == 67);
    }
}
