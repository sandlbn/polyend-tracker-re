// Pattern screen visual parity (spec 20 §4.1, docs/VISUAL_DIFF_1.9.4.md
// Part 2 §7.5 / §8): display-list budget with the ROM fallback and with the
// custom fonts bound, the cursor-step status, the stock popups.
#include "test.h"
#include "ui_fakes.h"

#include "ui/screens.h"
#include "ui/widgets.h"

#include <cmath>

using namespace ot;

static PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }

static void content(Harness& h) {
    App& a = *h.app;
    for (int s = 0; s < 16; ++s) {
        Step& st = a.pattern().tracks[s % 4].steps[s];
        st.note = int8_t(48 + s);
        st.instrument = uint8_t(s % 3);
        st.fx[0].type = uint8_t(FxId::Volume);
        st.fx[0].value = uint8_t(10 * s);
    }
    a.pattern().tracks[9].steps[2].note = 60;
    a.pattern().tracks[9].steps[2].instrument = 48;
}

static uint32_t render(Harness& h) {
    h.app->ui.dirty();
    h.run(40);
    return h.app->ui.lastDlWords();
}

static bool hasText(Harness& h, const char* s) {
    for (auto& l : h.disp.last)
        if (l.rfind("TEXT", 0) == 0 && l.find(std::string("\"") + s + "\"") != std::string::npos) return true;
    return false;
}

TEST(pattern_display_list_budget) {
    for (int fonts = 0; fonts < 2; ++fonts) {
        Harness h;
        h.disp.customFonts = fonts == 1;
        applyFontSet(&h.disp, 0);
        CHECK(customFontsBound() == (fonts == 1));
        content(h);
        h.tap(btn::Rec);
        const uint32_t grid = render(h);
        std::printf("pattern grid, %s fonts: %u DL words\n", fonts ? "custom" : "ROM", unsigned(grid));
        // A dense page (16 notes with Fx on 4 tracks): ~1180 words on the ROM
        // fallback, ~1100 with custom fonts. The 1.1 x budget against 1.9.4
        // (1050 -> 1155) is the lighter ui_snapshot_test page (1070).
        CHECK(grid < 1300);
        CHECK(grid < 2048);
        h.down(btn::Instr);
        h.run(250);
        CHECK(pe(h).popup().isOpen());
        CHECK(render(h) < 2048);
        h.up(btn::Instr);
        h.down(btn::Fx1);
        h.run(250);
        h.tap(btn::F7);   // description panel
        CHECK(pe(h).popup().descriptionOpen());
        CHECK(render(h) < 2048);
        h.tap(btn::F7);
        h.up(btn::Fx1);
        h.down(btn::Note);
        h.run(600);
        CHECK(pe(h).popup().isOpen() && pe(h).popup().kind() == 0);
        CHECK(render(h) < 2048);
        h.up(btn::Note);
        h.tap(btn::F4);   // Fill popup
        CHECK(pe(h).fillOpen);
        CHECK(render(h) < 2048);
        h.tap(btn::F7);
        h.down(btn::Shift);   // mute page
        CHECK(render(h) < 2048);
        h.up(btn::Shift);
        // MIDI tracks 9..12.
        for (int i = 0; i < 11; ++i) h.tap(btn::Right);
        CHECK(render(h) < 2048);
        h.disp.customFonts = false;
        applyFontSet(&h.disp, 0);
    }
}

TEST(pattern_status_follows_cursor_when_stopped) {
    Harness h;
    for (int i = 0; i < 13; ++i) h.tap(btn::Down);
    render(h);
    char want[16];
    std::snprintf(want, sizeof(want), "P%d.014", h.app->patternNumber());
    CHECK(hasText(h, want));
}

TEST(pattern_midi_tabs_and_note_names) {
    Harness h;
    App& a = *h.app;
    a.pattern().tracks[11].steps[0].note = 61;
    for (int i = 0; i < 11; ++i) h.tap(btn::Right);
    CHECK_EQ(pe(h).firstTrack, 8);
    render(h);
    CHECK(hasText(h, "Midi 9"));
    CHECK(hasText(h, "Midi 12"));
    CHECK(hasText(h, "C#5"));   // no separator (stock "C5", "C#5")
}

TEST(pattern_notes_popup_window_and_pads) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Rec);
    h.down(btn::Note);
    h.run(600);
    PatternScreen& p = pe(h);
    CHECK(p.popup().isOpen());
    const int win = p.noteKeysWindow();
    CHECK(win % 12 == 0);
    CHECK(p.popup().cursor() >= win && p.popup().cursor() < win + 48);
    // Top-left pad = C of the highest shown octave.
    h.pad(0);
    CHECK_EQ(p.popup().cursor(), win + 36);
    // Up = one octave higher (the row above), Down = lower.
    h.tap(btn::Down);
    CHECK_EQ(p.popup().cursor(), win + 24);
    h.up(btn::Note);
    CHECK(!p.popup().isOpen());
    CHECK_EQ(a.pattern().tracks[0].steps[0].note, win + 24);
}

TEST(pattern_off_cells_show_no_instrument) {
    // USER_REPORTS_R3 R3-2 (stock photos of live-recorded OFF steps): an
    // OFF / CUT / FADE cell leaves the Instr column empty.
    Harness h;
    App& a = *h.app;
    a.pattern().tracks[0].steps[2].note = 60;
    a.pattern().tracks[0].steps[2].instrument = 6;   // "07"
    render(h);
    CHECK(hasText(h, "07"));
    a.pattern().tracks[0].steps[2].note = kNoteOff;
    render(h);
    CHECK(!hasText(h, "07"));
    CHECK(hasText(h, "OFF"));
}

TEST(pattern_cursor_red_while_recording) {
    // USER_REPORTS_R3 R3-3 (stock photos): Rec + Play leaves edit mode, but
    // the cursor stays red while recording (count-in included).
    Harness h;
    CHECK(!pe(h).recordCursor());
    h.down(btn::Rec);
    h.tap(btn::Play);
    h.up(btn::Rec);
    h.run(20);
    CHECK(h.app->seq.isRecording());
    CHECK(!h.app->editMode);
    CHECK(pe(h).recordCursor());
    h.tap(btn::Play);   // stop
    h.run(20);
    CHECK(!pe(h).recordCursor());
}

TEST(fill_instruments_show_grid_codes_and_names) {
    // USER_REPORTS_R3 R3-5 (2.x): the Fill Instruments From / To values were
    // bare numbers 1..66 (stock style); they now read like the grid (07, M01,
    // S1) and the header names the From / To instruments.
    Harness h;
    App& a = *h.app;
    char v[8];
    pe(h).col = 1;
    pe(h).fillValueText(6, v);
    CHECK(std::string(v) == "07");
    pe(h).fillValueText(kFirstMidiInstrument, v);
    CHECK(std::string(v) == "M01");
    pe(h).fillValueText(kFirstMidiInstrument + 15, v);
    CHECK(std::string(v) == "M16");
    pe(h).fillValueText(kInstrumentCount, v);
    CHECK(std::string(v) == "S1");
    std::snprintf(a.instruments[6].name, sizeof(a.instruments[6].name), "Kick");
    a.instrPresent[6] = true;
    h.tap(btn::Rec);
    pe(h).col = 1;
    h.tap(btn::F4);   // Fill
    CHECK(pe(h).fillOpen);
    pe(h).fill[1].from = 6;
    render(h);
    CHECK(hasText(h, "From 07 Kick"));
}

TEST(fill_fx_values_read_like_the_grid) {
    // MANUAL_AUDIT_2026-10-23 #8 (manual p.87 / 92: Fill Fx Panning "From -50
    // ... To 50"): Fx From / To show the Fx's display value, not the raw byte.
    Harness h;
    int idx = -1;
    for (int i = 0; i < kFxCount; ++i)
        if (kFxMenuOrder[i] == uint8_t(FxId::Panning)) idx = i;
    CHECK(idx >= 0);
    pe(h).col = 2;
    pe(h).fill[2].scaleOrFx = int8_t(idx);
    char v[8];
    pe(h).fillValueText(0, v);
    CHECK(std::string(v) == "-50");
    pe(h).fillValueText(100, v);
    CHECK(std::string(v) == "50");
    pe(h).fill[2].scaleOrFx = -1;   // random type: plain number
    pe(h).fillValueText(7, v);
    CHECK(std::string(v) == "7");
}

TEST(popup_left_right_page_through_lists) {
    // MANUAL_AUDIT_2026-10-23 #19 (manual p.101 "[Left]/[Right] to scroll by
    // page of slots"; p.103 FX popup "large steps"): list popups page.
    Harness h;
    h.down(btn::Instr);
    h.run(250);
    CHECK(pe(h).popup().isOpen());
    const int c0 = pe(h).popup().cursor();
    h.tap(btn::Right);
    CHECK_EQ(pe(h).popup().cursor(), c0 + wg::listVisibleRows(394));
    h.tap(btn::Left);
    CHECK_EQ(pe(h).popup().cursor(), c0);
    h.up(btn::Instr);
}

TEST(fill_fx_bar_uses_the_engine_range) {
    // MANUAL_AUDIT_2026-10-23 #24: the Fill Fx From / To bar fraction uses
    // the engine range (seq::fxMin/fxMax), not the file-format table.
    Harness h;
    int idx = -1;
    for (int i = 0; i < kFxCount; ++i)
        if (kFxMenuOrder[i] == uint8_t(FxId::BitDepth)) idx = i;
    pe(h).col = 2;
    pe(h).fill[2].scaleOrFx = int8_t(idx);
    CHECK(std::fabs(pe(h).fillValueFrac(4) - 0.0f) < 1e-6f);    // 4 bits = the bottom
    CHECK(std::fabs(pe(h).fillValueFrac(16) - 1.0f) < 1e-6f);
}

TEST(eight_track_view_tabs_cover_the_top_row) {
    // USER_REPORTS_R3 R3-10 (owner screenshot): in the 8-track view (Pattern
    // held + Note) a name-wide tab left part of the top row's note visible
    // beside it ("Hats5"). The narrow tabs span the track width.
    Harness h;
    App& a = *h.app;
    for (int t = 0; t < 8; ++t) a.pattern().tracks[t].steps[0].note = 60;
    h.down(btn::Pattern);
    h.tap(btn::Note);
    h.up(btn::Pattern);
    a.ui.dirty();
    h.run(40);
    // Tab rects: pairs of vertices on the tab row; the widest must reach ~85 px.
    int widest = 0;
    bool inRects = false, first = true;
    int x0 = 0, y0 = 0;
    for (const std::string& l : h.disp.last) {
        if (l.rfind("BEGIN", 0) == 0) { inRects = true; first = true; continue; }
        if (l == "END") inRects = false;
        int x, y;
        if (!inRects || std::sscanf(l.c_str(), "VERTEX %d %d", &x, &y) != 2) continue;
        if (first) { x0 = x; y0 = y; }
        else if (y - y0 == 25 && x - x0 < 93 && x - x0 > widest && y0 < 80) widest = x - x0;   // tab body, one track
        first = !first;
    }
    std::printf("       widest tab-height rect in the 8-track view: %d px\n", widest);
    CHECK(widest >= 80);
}
