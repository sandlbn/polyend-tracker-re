// Manual-audit fixes (docs/MANUAL_AUDIT.md): module-key hold from another
// screen, column-key popups, horizontal arrangement, Fill window.
#include <cmath>
#include <string>

#include "test.h"
#include "ui_fakes.h"

#include "app/pattern_ops.h"
#include "app/project_fields.h"
#include "ui/fx_text.h"
#include "ui/screens.h"

using namespace ot;

static PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }

// Position of the first TEXT call whose string starts with `s`.
static bool textPos(const Harness& h, const char* s, int& x, int& y) {
    const std::string q = std::string("\"") + s;
    for (auto& l : h.disp.last) {
        if (l.compare(0, 5, "TEXT ") != 0 || l.find(q) == std::string::npos) continue;
        return std::sscanf(l.c_str(), "TEXT %d %d", &x, &y) == 2;
    }
    return false;
}

TEST(hold_master_and_jog_from_pattern_sets_master_volume) {
    Harness h;
    App& a = *h.app;
    const uint8_t v0 = a.cfg.masterVolume;
    h.down(btn::Master);
    CHECK(a.ui.active() == kModMaster);
    h.enc(-5);   // Jog while Master is still held
    CHECK_EQ(int(a.cfg.masterVolume), int(v0) - 5);
    h.up(btn::Master);
    CHECK(a.ui.active() == kModMaster);
    // A plain tap still only switches: the page does not cycle on release.
    h.tap(btn::Pattern);
    MixerScreen& m = *static_cast<MixerScreen*>(a.ui.screen(kModMaster));
    m.page = 1;
    h.tap(btn::Master);
    CHECK(a.ui.active() == kModMaster);
    CHECK_EQ(m.page, 1);
    // Held past the hold time: Master main page, Jog = volume.
    h.tap(btn::Pattern);
    h.down(btn::Master);
    h.run(400);
    CHECK_EQ(m.page, 0);
    h.enc(2);
    CHECK_EQ(int(a.cfg.masterVolume), int(v0) - 3);
    h.up(btn::Master);
}

TEST(hold_song_and_jog_from_pattern_sets_tempo) {
    // spec 16 §A.7: held from another page, Song shows temporarily with the
    // tempo focused; the release returns to the previous page.
    Harness h;
    App& a = *h.app;
    const float t0 = a.project.tempo();
    h.down(btn::Song);
    CHECK(a.ui.active() == kModSong);
    h.run(400);   // hold tick
    h.enc(3);
    CHECK(std::fabs(a.project.tempo() - (t0 + 3)) < 0.01f);
    h.up(btn::Song);
    CHECK(a.ui.active() == kModPattern);
    // A short press stays on Song; Jog then moves the slot list.
    h.tap(btn::Song);
    CHECK(a.ui.active() == kModSong);
    h.enc(1);
    CHECK(std::fabs(a.project.tempo() - (t0 + 3)) < 0.01f);
}

TEST(column_key_popup_drawn_at_once) {
    Harness h;
    (void)h.app;
    h.down(btn::Instr);
    h.run(250);
    CHECK(pe(h).popup().isOpen());
    CHECK(h.disp.lastHasText("Instrument"));
    h.up(btn::Instr);
    CHECK(!pe(h).popup().isOpen());
    CHECK(!h.disp.lastHasText("Instrument"));
}

TEST(hold_note_and_pad_writes_once) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Rec);
    h.down(btn::Note);
    h.pad(36);
    h.run(700);   // past the 500 ms popup time
    CHECK(!pe(h).popup().isOpen());
    h.up(btn::Note);
    CHECK(a.pattern().tracks[0].steps[0].note == int8_t(a.padNote(36)));
    CHECK(a.pattern().tracks[0].steps[1].note == kNoteEmpty);
    CHECK_EQ(pe(h).step, 1);
    // Same for Instr + pad.
    h.down(btn::Instr);
    h.pad(3);
    h.run(300);
    CHECK(!pe(h).popup().isOpen());
    h.up(btn::Instr);
}

TEST(horizontal_arrangement_draws_tracks_as_rows) {
    Harness h;
    App& a = *h.app;
    a.pattern().tracks[0].steps[0].note = 36;   // C3
    a.pattern().tracks[1].steps[3].note = 38;   // D3
    a.cfg.arrangement = 2;
    a.ui.dirty();
    h.run(40);
    int x0, y0, x1, y1;
    CHECK(textPos(h, "C3", x0, y0));
    CHECK(textPos(h, "D3", x1, y1));
    CHECK(x1 > x0 + 2 * PatternScreen::hStepColW());   // steps run left to right
    CHECK(y1 > y0 + 40);                                // tracks run top to bottom
    // Right moves the cursor one step column to the right.
    PatternScreen& p = pe(h);
    h.tap(btn::Right);
    CHECK_EQ(p.step, 1);
    CHECK_EQ(p.track, 0);
    h.tap(btn::Down);
    CHECK_EQ(p.track, 1);
    // Vertical: the same notes are on one row band, steps top to bottom.
    a.cfg.arrangement = 0;
    p.step = 0;
    p.track = 0;
    a.ui.dirty();
    h.run(40);
    CHECK(textPos(h, "C3", x0, y0));
    CHECK(textPos(h, "D3", x1, y1));
    CHECK(y1 > y0);
    CHECK(x1 > x0);
}

TEST(fill_window_lists_and_labels) {
    Harness h;
    App& a = *h.app;
    PatternScreen& p = pe(h);
    h.tap(btn::Rec);
    h.tap(btn::F4);
    CHECK(p.fillOpen);
    CHECK(h.disp.lastHasText("Fill Notes"));
    CHECK(h.disp.lastHasText("Where?"));
    CHECK(h.disp.lastHasText("Fill Type"));
    CHECK(h.disp.lastHasText("Euclidean"));
    CHECK(h.disp.lastHasText("Chromatic"));
    CHECK(h.disp.lastHasText("From-To"));
    CHECK(std::string(a.ui.bar.label[7].top) == "Fill");
    CHECK(std::string(a.ui.bar.label[4].top) == "Note");
    CHECK(!a.ui.bar.label[1].enabled);   // Where = Note: no parameter
    CHECK(!a.ui.bar.label[5].enabled);   // constant: no To
    // Down steps down the Where list.
    h.tap(btn::Down);
    CHECK_EQ(p.fill[0].where, 1);
    p.fill[0].where = 6;
    h.tap(btn::F1);
    h.tap(btn::F2);   // F1 re-press wrapped Where to 0 -> F2 blank, ignored
    CHECK_EQ(p.fillField, 0);
    p.fill[0].where = 6;
    h.tap(btn::F2);
    CHECK_EQ(p.fillField, 1);
    CHECK(std::string(a.ui.bar.label[1].top) == "Events");
    // Left / Right move between the fields, skipping blank ones (user
    // report: they changed the value like Up / Down).
    const int where = p.fill[0].where;
    h.tap(btn::Right);
    CHECK_EQ(p.fillField, 2);
    h.tap(btn::Right);
    CHECK_EQ(p.fillField, 3);
    p.fill[0].type = 0;   // constant: To blank
    h.tap(btn::Right);
    CHECK_EQ(p.fillField, 4);
    h.tap(btn::Right);
    CHECK_EQ(p.fillField, 4);   // last shown field
    h.tap(btn::Left);
    h.tap(btn::Left);
    h.tap(btn::Left);
    CHECK_EQ(p.fillField, 1);
    CHECK_EQ(p.fill[0].where, where);   // values unchanged
    p.fill[0].where = 0;               // no Where parameter: F2 blank
    p.fillField = 3;
    h.tap(btn::Left);
    h.tap(btn::Left);
    CHECK_EQ(p.fillField, 0);
    h.tap(btn::F7);
    // FX column: index 0 = None, -1 = Random FX; From/To shown as Fx text.
    p.col = 2;
    h.tap(btn::F4);
    CHECK(h.disp.lastHasText("Fill Fx 1"));
    CHECK(h.disp.lastHasText("Random Fx"));
    CHECK(std::string(a.ui.bar.label[2].top) == "Fx Type");
    p.fill[2].scaleOrFx = 0;
    p.fillField = 2;
    h.enc(3);   // None -> Off -> Volume -> Panning
    CHECK(std::string(fxInfo(kFxMenuOrder[p.fill[2].scaleOrFx]).name) == "Panning");
    h.tap(btn::F5);
    for (int i = 0; i < 30; ++i) h.enc(-10);
    // Panning at its minimum reads -50 as in the grid (manual p.87; spec 16
    // §B.3 "a number" is ambiguous; MANUAL_AUDIT_2026-10-23 #8).
    CHECK(std::string(a.ui.bar.label[4].bottom) == "-50");
    CHECK(std::string(a.ui.bar.label[4].top) == "Fx Value");
    h.tap(btn::F7);
}

TEST(count_in_popup) {
    // spec 16 §B.9: "Counting in" for floor(count-in seconds), at least 1 s.
    Harness h;
    App& a = *h.app;
    a.seq.settings().preRoll = 1;
    a.seq.settings().tsNum = 3;
    a.seq.settings().tsDen = 3;
    h.down(btn::Rec);
    h.tap(btn::Play);
    h.up(btn::Rec);
    CHECK(a.seq.isCountingIn());
    CHECK(pe(h).countInShown());
    CHECK(h.disp.lastHasText("\"Counting in\""));
    const uint32_t n = a.seq.countInMs();
    CHECK(n <= 255);
    h.run(1000 * (n < 1 ? 1 : n) + 50);
    CHECK(!pe(h).countInShown());
    CHECK(!h.disp.lastHasText("Counting in"));
}

TEST(fx_pad_values_roll_lfo_reverse) {
    // spec 16 §B.7 (0x14458).
    auto pv = [](FxId t, int pad) { return int(pops::fxValueFromPad(uint8_t(t), pad)); };
    const int roll[48] = {0,  1,  2,  3,  5,  6,  7,  8,  9,  10, 11, 12, 14, 15, 16, 17,
                          18, 19, 20, 21, 23, 24, 25, 26, 27, 28, 29, 30, 32, 33, 34, 35,
                          36, 37, 38, 39, 41, 42, 43, 44, 45, 46, 47, 48, 50, 51, 52, 53};
    for (int p = 0; p < 48; ++p) CHECK_EQ(pv(FxId::Roll, p), roll[p]);
    CHECK_EQ(pv(FxId::VolumeLfo, 10), 10);
    CHECK_EQ(pv(FxId::VolumeLfo, 40), 23);
    CHECK_EQ(pv(FxId::PanningLfo, 20), 20);
    CHECK_EQ(pv(FxId::FilterLfo, 47), 28);
    CHECK_EQ(pv(FxId::ReversePlayback, 0), 0);
    CHECK_EQ(pv(FxId::ReversePlayback, 1), 1);
    CHECK_EQ(pv(FxId::ReversePlayback, 47), 1);
    CHECK_EQ(pv(FxId::Tempo, 0), 4);
    CHECK_EQ(pv(FxId::Swing, 47), 75);
    CHECK_EQ(pv(FxId::BitDepth, 5), 9);
    CHECK_EQ(pv(FxId::BitDepth, 47), 16);
    CHECK_EQ(pv(FxId::Panning, 47), 100);
    CHECK_EQ(pv(FxId::Panning, 1), 2);   // round(100/47)
}

TEST(naming_keyboard_layout_cursor_and_labels) {
    Harness h;
    App& a = *h.app;
    Keyboard& kb = a.ui.kb;
    h.tap(btn::F8);
    h.tap(btn::F8);   // page 2
    h.tap(btn::F1);   // pattern name
    CHECK(kb.isOpen());
    h.run(20);
    CHECK(h.disp.lastHasText("\"Enter\""));
    CHECK(h.disp.lastHasText("\"Auto Name\""));
    CHECK(h.disp.lastHasText("\"Save\""));
    CHECK(h.disp.lastHasText("\"Backspace\""));
    CHECK(h.disp.lastHasText("\"Caps Lock\""));
    // Manual layout: '-' and '+' end the q-row, '@' after l, Space 43..47.
    h.pad(22);
    h.pad(23);
    h.pad(33);
    h.pad(45);
    CHECK(std::string(kb.text()) == "-+@ ");
    h.pad(11);   // Backspace (pads 10-11)
    h.pad(10);
    h.pad(10);
    h.pad(10);
    CHECK(std::string(kb.text()) == "");
    h.type("abc");
    CHECK_EQ(kb.cursor(), 3);
    // Up from the digit row highlights the name; Left/Right move the cursor.
    h.tap(btn::Up);
    h.tap(btn::Up);
    h.tap(btn::Up);
    h.tap(btn::Up);
    CHECK(kb.nameFocused());
    h.tap(btn::Left);
    h.tap(btn::Left);
    CHECK_EQ(kb.cursor(), 1);
    h.pad(uint8_t(keyPad('x')));   // typed at the cursor
    CHECK(std::string(kb.text()) == "axbc");
    CHECK_EQ(kb.cursor(), 2);
    h.tap(btn::Delete);            // deletes before the cursor
    CHECK(std::string(kb.text()) == "abc");
    // Wide keys move as one: Right from '0' lands on Backspace, then wraps.
    h.tap(btn::Down);   // leaves the name row if focused
    uint8_t lv[48];
    kb.leds(lv, 5, 30);
    CHECK_EQ(int(lv[27]), 5);   // f
    CHECK_EQ(int(lv[30]), 5);   // j
    CHECK_EQ(int(lv[0]), 30);   // Down from the name: highlight on '1'
    CHECK_EQ(int(lv[1]), 0);
    CHECK_EQ(int(lv[34]), 0);   // Caps off
    h.tap(btn::F8);
    CHECK(std::strcmp(a.names.get(1), "abc") == 0);
    // Reopened: the whole name is selected, the first key replaces it.
    h.tap(btn::F1);
    CHECK(kb.allSelected());
    CHECK(kb.nameFocused());
    CHECK_EQ(kb.highlighted(), 10);   // Backspace
    h.tap(btn::F1);                   // Enter types only in grid mode
    CHECK(std::string(kb.text()) == "abc");
    h.pad(uint8_t(keyPad('z')));
    CHECK(std::string(kb.text()) == "z");
    h.tap(btn::F7);
    // Track name (7 chars): no Auto Name, confirm is "Save".
    h.tap(btn::F2);
    CHECK(kb.isOpen());
    h.run(20);
    CHECK(!h.disp.lastHasText("\"Auto Name\""));
    CHECK(h.disp.lastHasText("\"Save\""));
    h.tap(btn::F7);
    CHECK(!kb.isOpen());
}

TEST(fx_picker_description_window) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Rec);
    h.down(btn::Fx1);
    h.run(250);
    CHECK(pe(h).popup().isOpen());
    CHECK(!pe(h).popup().descriptionOpen());
    CHECK(std::string(a.ui.bar.label[6].top) == "Description");
    h.tap(btn::F7);
    CHECK(pe(h).popup().descriptionOpen());
    CHECK(h.disp.lastHasText("No effect in this slot"));
    h.enc(3);   // None -> Off -> Volume -> Panning
    CHECK(h.disp.lastHasText("Stereo position"));
    h.tap(btn::F8);
    CHECK(!pe(h).popup().descriptionOpen());
    CHECK(!h.disp.lastHasText("Stereo position"));
    h.tap(btn::F8);
    CHECK(pe(h).popup().descriptionOpen());
    h.up(btn::Fx1);
    CHECK(!pe(h).popup().isOpen());
    CHECK(a.pattern().tracks[0].steps[0].fx[fxSlotForColumn(2)].type == uint8_t(FxId::Panning));
    for (int t = 0; t < kFxCount; ++t) CHECK(std::strlen(fxDescription(uint8_t(t))) > 10);
}

TEST(render_and_load_opens_sample_loader) {
    // spec 16 §B.8.
    Harness h;
    App& a = *h.app;
    a.pattern().tracks[0].steps[0].note = 60;
    CHECK(pe(h).renderSelection("sel", true));
    CHECK(a.ui.active() == kModLoader);
    LoaderScreen& l = *static_cast<LoaderScreen*>(a.ui.screen(kModLoader));
    CHECK_EQ(l.insCursor, int(a.currentInstrument()));
}

// File > Open Project and Import Mod list up to ProjectBrowser::kMaxEntries
// (80 like stock, spec 05 appendix E1.2; 2.x: 200). User report: the list
// stopped at 64.
TEST(file_lists_hold_80_entries) {
    Harness h;
    h.fs.dirs.insert("Projects");
    h.fs.dirs.insert("Mod");
    for (int i = 0; i < 210; ++i) {
        char p[64];
        std::snprintf(p, sizeof p, "Projects/Song %03d", i);
        h.fs.dirs.insert(p);
        std::snprintf(p, sizeof p, "Projects/Song %03d/project.mt", i);
        h.fs.files[p] = std::vector<uint8_t>(64, 0);
        std::snprintf(p, sizeof p, "Mod/tune%03d.mod", i);
        h.fs.files[p] = std::vector<uint8_t>(2048, 0);
    }
    auto& f = *static_cast<FileScreen*>(h.app->ui.screen(kModFile));
    h.tap(btn::File);
    h.tap(btn::F2);   // Open Project
    CHECK_EQ(f.state, 1);
    std::printf("  open list %d entries\n", f.count);
    CHECK_EQ(f.count, ProjectBrowser::kMaxEntries);
    f.state = 0;      // back to the File page
    f.count = 0;
    h.tap(btn::F3);   // Import Mod
    CHECK_EQ(f.state, 2);
    std::printf("  mod list %d entries (state %d)\n", f.count, f.state);
    CHECK_EQ(f.count, ProjectBrowser::kMaxEntries);
}

TEST(pattern_stop_keeps_the_cursor_on_the_stop_row) {
    // User report (stock 1.9.2): Play, then Play again to stop -- the cursor
    // stays on the row where playback stopped instead of jumping to the top.
    Harness h;
    App& a = *h.app;
    h.tap(btn::Pattern);
    auto& p = *static_cast<PatternScreen*>(a.ui.screen(kModPattern));
    CHECK(!a.editMode);
    h.tap(btn::Play);
    CHECK(a.seq.isPlaying());
    for (int i = 0; i < 300; ++i) {   // ~1.7 s at 130 BPM
        h.audio.pump(3);
        h.run(10);
    }
    const int row = a.seq.step(0);
    CHECK(row > 4);
    h.tap(btn::Play);
    CHECK(!a.seq.isPlaying());
    h.run(60);
    CHECK(p.step >= row && p.step <= row + 1);
}

TEST(pattern_tap_alone_leaves_the_column_view) {
    // Manual Quick Reference / user report: Pattern + Note shows the Note
    // column of 8 tracks; pressing Pattern alone goes back to the 4-track view.
    Harness h;
    App& a = *h.app;
    h.tap(btn::Pattern);
    auto& p = *static_cast<PatternScreen*>(a.ui.screen(kModPattern));
    CHECK_EQ(p.colMask, 0);
    h.down(btn::Pattern);
    h.tap(btn::Note);
    h.up(btn::Pattern);
    h.run(60);
    CHECK_EQ(p.colMask, 1);   // the combo keeps the view
    h.tap(btn::Pattern);
    h.run(60);
    CHECK_EQ(p.colMask, 0);   // Pattern alone: full view
    // Two columns, then Pattern alone again.
    h.down(btn::Pattern);
    h.tap(btn::Note);
    h.tap(btn::Fx1);
    h.up(btn::Pattern);
    CHECK_EQ(p.colMask, 1 | 4);
    h.tap(btn::Pattern);
    CHECK_EQ(p.colMask, 0);
}

TEST(encoder_on_list_hint_for_the_simulator_wheel) {
    // The simulator flips its mouse wheel on lists (Ui::encoderOnList):
    // lists, options and rows are lists; value bars are not.
    Harness h;
    App& a = *h.app;
    h.tap(btn::Config);
    CHECK(a.ui.encoderOnList());   // categories / options
    h.tap(btn::Pattern);
    CHECK(!a.editMode);
    CHECK(a.ui.encoderOnList());   // rows while stopped, not editing
    h.tap(btn::Rec);
    CHECK(!a.ui.encoderOnList());  // edit mode: the encoder changes values
    h.tap(btn::Rec);
    a.setCurrentInstrument(0);
    h.tap(btn::InstrParams);
    auto& s = *static_cast<InstrumentScreen*>(a.ui.screen(kModInstrument));
    s.mode = 0;
    s.effects = false;
    s.column = 0;
    CHECK(!a.ui.encoderOnList());  // Volume bar
    s.column = 4;
    CHECK(a.ui.encoderOnList());   // filter type list
    h.tap(btn::SampleLoader);
    CHECK(a.ui.encoderOnList());
}

TEST(pattern_preview_plays_the_row_once) {
    // Stock 1.9.2 (owner check): Preview on a cell plays that row once and
    // stops; the cursor is free again. It looped before and held the cursor.
    Harness h;
    App& a = *h.app;
    h.tap(btn::Pattern);
    auto& p = *static_cast<PatternScreen*>(a.ui.screen(kModPattern));
    p.step = 8;
    h.tap(btn::F5);   // Preview
    CHECK(a.seq.isPlaying());
    for (int i = 0; i < 100 && a.seq.isPlaying(); ++i) {
        h.audio.pump(3);
        h.run(10);
    }
    CHECK(!a.seq.isPlaying());
    const int row = p.step;
    h.tap(btn::Down);
    CHECK_EQ(p.step, row + 1);
}

namespace {
// y of the song progress line: a 2-row RECTS box starting at x 5 in the
// grid's first row band (92..118), or -1.
int songProgressY(Harness& h) {
    int last = -1;
    for (size_t i = 0; i + 1 < h.disp.last.size(); ++i) {
        int x0, y0, x1, y1;
        if (std::sscanf(h.disp.last[i].c_str(), "VERTEX %d %d", &x0, &y0) == 2 &&
            std::sscanf(h.disp.last[i + 1].c_str(), "VERTEX %d %d", &x1, &y1) == 2 && x0 == 5 && x1 > 400 &&
            y1 - y0 == 1 && y0 >= 92 && y0 <= 118)
            last = y0;
    }
    return last;
}
}  // namespace

TEST(song_progress_line_follows_the_playing_pattern) {
    // User report (Tracker+ / stock photo): during song playback a
    // horizontal line moves down through the playing slot's grid row.
    Harness h;
    App& a = *h.app;
    h.tap(btn::Song);
    h.tap(btn::F2);   // Play Song
    CHECK(a.seq.isPlaying());
    auto playTo = [&](int step) {
        for (int i = 0; i < 2000 && a.seq.step(0) < step; ++i) {
            h.audio.pump(1);
            h.run(2);
        }
        a.ui.dirty();
        h.run(30);
    };
    playTo(4);
    const int y1 = songProgressY(h);
    playTo(24);
    const int y2 = songProgressY(h);
    CHECK(y1 >= 92);
    CHECK(y2 > y1 + 10);
    // The line stays inside the 12-column grid (device photo: on 2.x it ran
    // through the Pattern list), i.e. it ends before the list at x 491.
    for (size_t i = 0; i + 1 < h.disp.last.size(); ++i) {
        int x0, yA, x1, yB;
        if (std::sscanf(h.disp.last[i].c_str(), "VERTEX %d %d", &x0, &yA) == 2 &&
            std::sscanf(h.disp.last[i + 1].c_str(), "VERTEX %d %d", &x1, &yB) == 2 && x0 == 5 && yB - yA == 1 &&
            yA == y2)
            CHECK(x1 < 491);
    }
    h.tap(btn::F2);   // Stop
    h.run(60);
    a.ui.dirty();
    h.run(30);
    CHECK_EQ(songProgressY(h), -1);
}

TEST(song_loop_flag_ends_with_its_play_run) {
    // Stock photos: the title shows the loop arrow only while Shift+Play loops
    // a pattern in the song; a later plain pattern play shows the triangle,
    // even when the loop was stopped from another screen.
    Harness h;
    App& a = *h.app;
    h.tap(btn::Song);
    auto& s = *static_cast<SongScreen*>(a.ui.screen(kModSong));
    h.tap(btn::F2);   // play song
    h.audio.pump(20);
    h.run(20);
    h.down(btn::Shift);
    h.tap(btn::Play);   // Loop Pattern (deferred to the next step)
    h.up(btn::Shift);
    for (int i = 0; i < 200 && !s.loopingInSong(); ++i) {
        h.audio.pump(1);
        h.run(5);
    }
    CHECK(s.loopingInSong());
    h.tap(btn::Pattern);
    h.tap(btn::Play);   // stop from the Pattern screen
    CHECK(!a.seq.isPlaying());
    h.run(20);
    h.tap(btn::Play);   // plain pattern play
    CHECK(a.seq.isPlaying());
    CHECK(!s.loopingInSong());
    h.tap(btn::Play);
}

TEST(song_headers_show_track_names) {
    // User report / stock photo: a renamed track shows its name on the Song
    // page (it kept "Track X").
    Harness h;
    App& a = *h.app;
    a.project.setTrackName(1, "Bass");
    h.tap(btn::Song);
    a.ui.dirty();
    h.run(40);
    bool bass = false, track2 = false;
    for (auto& l : h.disp.last) {
        if (l.find("\"Bass\"") != std::string::npos) bass = true;
        if (l.find("\"Track\"") != std::string::npos) track2 = true;   // the other tracks keep "Track N"
    }
    CHECK(bass);
    CHECK(track2);
}

TEST(track_mixer_meters_restart_empty) {
    // User report: peak-hold marks from an earlier visit stayed on the Track
    // Mixer after the song was stopped elsewhere; the page opens empty.
    Harness h;
    App& a = *h.app;
    h.tap(btn::Master);
    h.tap(btn::Master);   // Track Mixer 2/3
    auto& m = *static_cast<MixerScreen*>(a.ui.screen(kModMaster));
    m.meter[0] = 60;
    m.meterHold[0] = 70;
    h.tap(btn::Pattern);
    h.tap(btn::Master);
    h.tap(btn::Master);
    CHECK_EQ(m.meter[0], 0);
    CHECK_EQ(m.meterHold[0], 0);
}

TEST(hold_master_and_up_down_from_pattern_sets_master_volume) {
    // MANUAL_AUDIT_2026-10-23 #3 (manual p.16: "Alternatively the [Up] and
    // [Down] buttons can be used instead of the Jog wheel"): held from another
    // page, Up / Down act on the Master hold state (main page, volume) even
    // before the hold tick, like the Jog; the Master screen was last left on
    // the Track Mixer here.
    Harness h;
    App& a = *h.app;
    MixerScreen& m = *static_cast<MixerScreen*>(a.ui.screen(kModMaster));
    h.tap(btn::Master);
    m.page = 1;
    h.tap(btn::Pattern);
    const uint8_t v0 = a.cfg.masterVolume;
    h.down(btn::Master);
    h.tap(btn::Down);
    CHECK_EQ(int(a.cfg.masterVolume), int(v0) - 1);
    h.tap(btn::Up);
    h.tap(btn::Up);
    CHECK_EQ(int(a.cfg.masterVolume), int(v0) + 1);
    h.up(btn::Master);
}

TEST(hold_song_with_another_key_still_returns_on_release) {
    // MANUAL_AUDIT_2026-10-23 #21 (manual p.225 "reverting back ... when
    // [Song] is released"; spec 16 §A.7): Song held from Pattern, Down pressed
    // inside the 333 ms window, still held past the hold tick: the release
    // returns to Pattern.
    Harness h;
    App& a = *h.app;
    CHECK(a.ui.active() == kModPattern);
    h.down(btn::Song);
    h.tap(btn::Down);
    h.run(400);
    h.up(btn::Song);
    CHECK(a.ui.active() == kModPattern);
}
