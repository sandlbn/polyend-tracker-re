// Pattern screen visual parity (spec 20 §4.1, docs/VISUAL_DIFF_1.9.4.md
// Part 2 §7.5 / §8): display-list budget with the ROM fallback and with the
// custom fonts bound, the cursor-step status, the stock popups.
#include "test.h"
#include "ui_fakes.h"

#include "ui/screens.h"

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
