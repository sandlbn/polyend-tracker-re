// 1.9.5 visual-parity API freeze (W0): font roles, glyph runs, gradients,
// hairlines, round rects, icons, context stack and the 277-slot themes
// (spec 20 §1.4, §1.5, §2). Checks the ROM-font fallbacks and the display
// list the primitives emit.
#include "test.h"
#include "ui_fakes.h"

#include <string>

#include "ui/gfx.h"
#include "ui/theme.h"

using namespace ot;

static int count(const std::vector<std::string>& v, const char* prefix) {
    int n = 0;
    for (const std::string& s : v)
        if (s.rfind(prefix, 0) == 0) ++n;
    return n;
}
static bool has(const std::vector<std::string>& v, const std::string& line) {
    for (const std::string& s : v)
        if (s == line) return true;
    return false;
}

TEST(font_roles_fall_back_to_rom_fonts) {
    RecordingDisplay d;
    CHECK(!applyFontSet(&d, 0));
    CHECK(!customFontsBound());
    CHECK_EQ(int(d.fontBinds.size()), 1);
    CHECK_EQ(romFontFor(kFont1, 0), romFontFor(kFontSmall, 0));
    CHECK_EQ(romFontFor(kFont2, 0), kFontMono);
    CHECK_EQ(romFontFor(kFont3, 1), romFontFor(kFontText, 1));
    CHECK_EQ(romFontFor(kFont4, 1), romFontFor(kFontLarge, 1));
    CHECK_EQ(romFontFor(kFont5, 0), romFontFor(kFontSmall, 0));
    CHECK_EQ(textWidth(kFontMono, "ABCD"), 32);
    CHECK_EQ(textWidth(kFont2, "ABCD", 2), 16);
    CHECK_EQ(fontHeight(kFontMono), 16);
    CHECK(textWidth(kFontText, "Wi") > textWidth(kFontText, "ii"));
    applyFontSet(nullptr, 0);
}

TEST(custom_fonts_bind_to_handles) {
    RecordingDisplay d;
    d.customFonts = true;
    CHECK(applyFontSet(&d, 1));
    CHECK_EQ(fontSet(), 1);
    CHECK_EQ(romFontFor(kFont1), IDisplay::kFontHandleBase);
    CHECK_EQ(romFontFor(kFont5), IDisplay::kFontHandleBase + 4);
    CHECK_EQ(romFontFor(kFontIcons), IDisplay::kIconHandle);
    CHECK_EQ(romFontFor(kFontText, 1), 22);   // ROM roles keep the set-1 mapping
    // Glyph run: one VERTEX2II per glyph, fixed advance; a run across x 512
    // takes one VERTEX_TRANSLATE_X (400) for all its glyphs.
    Gfx g(d);
    g.beginFrame(0);
    g.glyphRun(500, 10, kFont2, "A-B", 0xFFFFFF, 11);
    g.glyphRun(10, 40, kFont2, "AB", 0xFFFFFF, 11);   // left of 400: back to 0
    g.endFrame();
    CHECK(has(d.last, "TRANSLATE_X 6400"));
    CHECK(has(d.last, "VERTEX2II 100 10 4 65"));
    CHECK(has(d.last, "VERTEX2II 111 10 4 45"));
    CHECK(has(d.last, "VERTEX2II 122 10 4 66"));
    CHECK(has(d.last, "VERTEX2II 10 40 4 65"));
    CHECK(has(d.last, "TRANSLATE_X 0"));
    CHECK_EQ(count(d.last, "TRANSLATE_X"), 2);
    CHECK_EQ(count(d.last, "VERTEX2II"), 5);
    CHECK_EQ(count(d.last, "TEXT"), 0);
    applyFontSet(nullptr, 0);
    CHECK(!customFontsBound());
}

// CMD_TEXT is charged what the FT81x builds in RAM_DL (#71, measured on a
// Tracker): 5 words for the string, 1 per glyph whose pen is inside x / y
// 0..511, 2 per glyph anywhere else.
TEST(cmd_text_counts_what_the_ft81x_builds) {
    RecordingDisplay d;
    applyFontSet(&d, 0);
    Gfx g(d);
    g.beginFrame(0);
    g.text(0, 0, kFont1, 0xFFFFFF, "x");   // the colour is set once
    auto cost = [&](int x, int y, const char* s, uint16_t opt = 0) {
        const uint32_t w0 = g.dlWords();
        g.text(x, y, kFont1, 0xFFFFFF, s, opt);
        return g.dlWords() - w0;
    };
    CHECK_EQ(cost(10, 20, "abcd"), 9u);    // inside
    CHECK_EQ(cost(600, 20, "abcd"), 13u);  // right of x 511
    CHECK_EQ(cost(10, 520, "ab"), 9u);     // below y 511
    const int wa = textWidth(kFont1, "a", 1);
    CHECK_EQ(cost(512 - wa, 20, "ab"), 8u);   // straddles: a inside, b outside
    const int wab = textWidth(kFont1, "ab");
    CHECK_EQ(cost(511, 20, "ab", IDisplay::kOptRightX), wab <= 511 ? 7u : 9u);   // alignment applied first
    g.endFrame();
}

TEST(glyph_run_fallback_is_text) {
    RecordingDisplay d;
    setRomTextMode(kRomWholeText);
    applyFontSet(&d, 0);
    Gfx g(d);
    g.beginFrame(0);
    g.glyphRun(600, 20, kFont1, "Pattern", 0xFFFFFF);
    // Charged what the device draws (kRomGlyphs: 7 VERTEX2II + BEGIN and a
    // translate at most), not the CMD_TEXT it stands in for (5 + 2 x 7 at
    // x 600).
    const uint32_t w0 = g.dlWords();
    g.glyphRun(600, 50, kFont1, "Pattern", 0xFFFFFF);
    CHECK_EQ(g.dlWords() - w0, 9u);
    g.glyphRun(10, 30, kFont2, "--", 0x777777, 11);   // one string, the pitch ignored
    g.textRotated(5, 5, kFont3, 0xFFFFFF, "VOL");
    g.endFrame();
    CHECK(has(d.last, "TEXT 600 20 f26 o0 \"Pattern\""));
    CHECK(has(d.last, "TEXT 10 30 f18 o0 \"--\""));
    CHECK(has(d.last, "TEXT 5 5 f27 o0 \"VOL\""));
    CHECK_EQ(count(d.last, "VERTEX2II"), 0);
    setRomTextMode(kRomGlyphs);
}

// OP-6: without custom fonts glyphRun draws the ROM font bitmaps (handles
// 16-31, cell = character code) at the romMetrics advances; runs clipped at
// the left / top edge keep CMD_TEXT.
TEST(glyph_run_rom_fonts_vertex2ii) {
    RecordingDisplay d;
    setRomTextMode(kRomGlyphs);
    applyFontSet(&d, 0);
    CHECK(!customFontsBound());
    Gfx g(d);
    g.beginFrame(0);
    g.glyphRun(600, 20, kFont1, "Pa t", 0xFFFFFF);   // f26: P 7, a 7, ' ' 4
    g.glyphRun(10, 30, kFont2, "--", 0x777777, 11);  // f18, fixed pitch
    g.glyphRun(-3, 40, kFont2, "AB", 0xFFFFFF);      // clipped left: CMD_TEXT
    g.endFrame();
    CHECK(has(d.last, "BEGIN 1"));   // kBitmaps
    CHECK(has(d.last, "TRANSLATE_X 6400"));                  // x 600 -> 600 - 400
    CHECK(has(d.last, "VERTEX2II 200 20 26 80"));            // 'P'
    CHECK(has(d.last, "VERTEX2II 207 20 26 97"));            // 'a' at 600 + 7
    CHECK(has(d.last, "VERTEX2II 218 20 26 116"));           // 't' after a 4-px space
    CHECK(has(d.last, "VERTEX2II 10 30 18 45"));
    CHECK(has(d.last, "VERTEX2II 21 30 18 45"));
    CHECK_EQ(count(d.last, "VERTEX2II"), 5);
    CHECK(has(d.last, "TEXT -3 40 f18 o0 \"AB\""));
    CHECK_EQ(count(d.last, "TEXT "), 1);
    CHECK_EQ(g.textWidth(kFont1, "Pa t"), 7 + 7 + 4 + 5);
}

TEST(gradient_hairline_roundrect_icons) {
    RecordingDisplay d;
    Gfx g(d);
    g.beginFrame(0);
    g.gradientRect(10, 400, 100, 12, 0x0A0A0A, 0x000000);
    g.hairline(0, 28, 799, 28, 0xFFFFFF);
    g.roundRect(20, 20, 40, 20, 4, 0x323132);
    for (int i = 0; i < kIconCount; ++i) g.icon(100 + 14 * i, 100, IconId(i), 0xFFFFFF);
    g.endFrame();
    CHECK(has(d.last, "SAVE_CONTEXT"));
    CHECK(has(d.last, "SCISSOR_XY 10 400"));
    CHECK(has(d.last, "SCISSOR_SIZE 100 12"));
    CHECK(has(d.last, "GRADIENT 10 400 0A0A0A 10 411 000000"));
    CHECK(has(d.last, "RESTORE_CONTEXT"));
    CHECK(has(d.last, "LINE_WIDTH 8"));
    CHECK(has(d.last, "LINE_WIDTH 64"));
    CHECK(has(d.last, "VERTEX 23 23"));
    CHECK(has(d.last, "VERTEX 56 36"));
    CHECK_EQ(count(d.last, "LINE_WIDTH 16"), 2);
    CHECK(count(d.last, "BEGIN") > kIconCount);
}

TEST(theme_slots_from_spec20) {
    CHECK_EQ(kThemeSlotCount, 277);
    const Theme& o = theme(0);
    const Theme& m = theme(1);
    const Theme& a = theme(2);
    CHECK_EQ(o.slot(slot::kFrameClear), 0x000000u);
    CHECK_EQ(m.slot(slot::kFrameClear), 0x4B4B4Bu);
    CHECK_EQ(o.slot(slot::kGrid + 2), 0x00E8BEu);   // Note text
    CHECK_EQ(a.slot(slot::kGrid + 2), 0xD0D0D0u);
    CHECK_EQ(o.slot(slot::kCyan), 0x3FFFFFu);
    CHECK_EQ(m.slot(slot::kCyan), 0x00A4B3u);
    CHECK_EQ(o.slot(slot::kGridLine), 0x222222u);
    CHECK(o.flag(slot::kFlagBottomFade));
    CHECK(!m.flag(slot::kFlagBottomFade));
    CHECK(m.flag(slot::kFlagCursorRowGrad));
    CHECK(!a.flag(slot::kFlagUnknown));
    CHECK_EQ(o.slot(-1), 0u);
    CHECK_EQ(o.slot(277), 0u);
    // Named colours unchanged.
    CHECK_EQ(o.accent, 0xCD1818u);
    CHECK_EQ(o.track[0], 0x00E8BEu);
    CHECK(std::strcmp(fontSetName(kFontSetOriginal), "Original") == 0);
    CHECK(std::strcmp(fontSetName(kFontSetNew), "New") == 0);
    CHECK(std::strcmp(fontSetName(7), "Original") == 0);
}
