// User report (MIDI step entry stopped after a jog turn): the pattern editor's note-encoder audition (kAuditionEncoder) keeps
// the cursor track busy, so the next MIDI key lands on another track and
// edit-mode step entry writes nothing; a pad press on the track frees it.
#include <cstdio>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

static PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }

static void addWav(Harness& h, const char* path, uint32_t frames) {
    std::vector<int16_t> s(frames);
    for (uint32_t i = 0; i < frames; ++i) s[i] = int16_t((i % 100) * 300 - 15000);
    std::vector<uint8_t> out(frames * 2 + 64);
    size_t n = wavEncode(s.data(), frames, 1, out.data(), out.size());
    out.resize(n);
    h.fs.files[path] = out;
}
static void send(Harness& h, std::initializer_list<uint8_t> b) {
    h.midi.in.insert(h.midi.in.end(), b);
    h.run(5);
}
static void noteOn(Harness& h, uint8_t n, uint8_t v = 100) { send(h, {0x90, n, v}); }
static void noteOff(Harness& h, uint8_t n) { send(h, {0x80, n, 0}); }

TEST(encoder_audition_does_not_block_midi_step_entry) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    addWav(h, "tone.wav", 20000);
    CHECK(a.importSample("tone.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::Rec);
    CHECK(a.editMode);
    PatternScreen& p = pe(h);
    // Put a note in the cursor cell with the keyboard, as any user would.
    noteOn(h, 60);
    noteOff(h, 60);
    CHECK_EQ(int(a.pattern().tracks[0].steps[0].note), 60);
    // Nudge the note encoder once (edit mode, Note column): audition on the
    // cursor track with voice kAuditionEncoder ... which is never released.
    h.enc(1);
    CHECK_EQ(int(a.pattern().tracks[0].steps[0].note), 61);
    const int encTrack = a.auditionTrack(App::kAuditionEncoder);
    std::printf("encoder audition track after one encoder tick: %d\n", encTrack);
    CHECK_EQ(encTrack, 0);
    h.run(2000);   // two seconds later it is still there
    CHECK_EQ(a.auditionTrack(App::kAuditionEncoder), 0);
    // Cursor to the next row; a MIDI key now writes nothing: previewOn skips
    // the busy track 0, claims track 1, and 1 is outside the cursor cell.
    h.tap(btn::Down);
    CHECK_EQ(p.step, 1);
    noteOn(h, 64);
    std::printf("midi note 64 sounded on track %d\n", a.auditionTrack(uint16_t(App::kAuditionMidiIn + 64)));
    noteOff(h, 64);
    std::printf("cell (step 1, track 0) note = %d  (expected 64)\n", int(a.pattern().tracks[0].steps[1].note));
    CHECK_EQ(int(a.pattern().tracks[0].steps[1].note), 64); 
    // A pad press on the track (edit mode, Note column -> forceTrack) frees it.
    h.pad(0);
    CHECK(a.auditionTrack(App::kAuditionEncoder) < 0);
    h.tap(btn::Up);
    h.tap(btn::Up);   // back to step 1 (pad advanced the cursor)
    p.step = 1;
    noteOn(h, 65);
    noteOff(h, 65);
    CHECK_EQ(int(a.pattern().tracks[0].steps[1].note), 65);
}

TEST(encoder_audition_does_not_shift_a_chord_selection) {
    // Chord across a 2-track selection after an encoder nudge on track 0:
    // the first key goes to track 1, the second to track 2 (outside).
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    addWav(h, "tone.wav", 20000);
    CHECK(a.importSample("tone.wav", 0));
    a.setCurrentInstrument(0);
    h.tap(btn::Rec);
    PatternScreen& p = pe(h);
    noteOn(h, 60);
    noteOff(h, 60);
    h.enc(1);
    CHECK_EQ(a.auditionTrack(App::kAuditionEncoder), 0);
    h.tap(btn::Down);
    h.down(btn::Shift);
    h.tap(btn::Right);
    h.up(btn::Shift);
    const SelRect r = p.target();
    std::printf("selection s%d..%d t%d..%d\n", r.s0, r.s1, r.t0, r.t1);
    noteOn(h, 64);
    noteOn(h, 67);
    std::printf("64 on track %d, 67 on track %d\n", a.auditionTrack(uint16_t(App::kAuditionMidiIn + 64)),
                a.auditionTrack(uint16_t(App::kAuditionMidiIn + 67)));
    noteOff(h, 64);
    noteOff(h, 67);
    std::printf("t0 s1 = %d, t1 s1 = %d (expected 64, 67)\n", int(a.pattern().tracks[0].steps[1].note),
                int(a.pattern().tracks[1].steps[1].note));
    CHECK_EQ(int(a.pattern().tracks[0].steps[1].note), 64); 
    CHECK_EQ(int(a.pattern().tracks[1].steps[1].note), 67); 
}

TEST(muted_cursor_track_takes_no_midi_entry_spec_17_step_4) {
    // Documented stock behaviour (spec 17 §1.4 steps 4 and 8): a muted cursor
    // track is skipped by the voice search, so the key is not written there.
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    a.setCurrentInstrument(0);
    h.tap(btn::Rec);
    a.setTrackMute(0, true);
    noteOn(h, 60);
    std::printf("muted track 0: note sounded on track %d\n", a.auditionTrack(uint16_t(App::kAuditionMidiIn + 60)));
    noteOff(h, 60);
    std::printf("muted track 0: cell note = %d\n", int(a.pattern().tracks[0].steps[0].note));
}
