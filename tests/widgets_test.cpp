// Shared stock chrome (W2, 1.9.5 visual parity): theme roles, title bar,
// bottom strip / labels, panels, list, value bar, popups (spec 20 §1.2,
// §1.5, §3; docs/VISUAL_DIFF_1.9.4.md §2.3). Geometry is checked on the
// recorded display list; the colours that the display blends are checked
// with wg::mix against the values measured on the stock screenshots.
#include "test.h"
#include "ui_fakes.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "ui/widgets.h"

using namespace ot;

namespace {

// Rects / points of a recorded frame with the colour and alpha in effect.
struct Prim {
    char kind;          // 'R' rect, 'P' point
    uint32_t colour;
    unsigned alpha;
    int x0, y0, x1, y1;
};

std::vector<Prim> prims(const std::vector<std::string>& dl) {
    std::vector<Prim> out;
    uint32_t c = 0;
    unsigned a = 255;
    int prim = -1;
    bool half = false;
    int vx = 0, vy = 0;
    for (const std::string& l : dl) {
        unsigned u = 0;
        int x = 0, y = 0;
        if (std::sscanf(l.c_str(), "COLOR %x", &u) == 1 && l.rfind("COLOR ", 0) == 0) c = u;
        else if (std::sscanf(l.c_str(), "COLOR_A %u", &u) == 1) a = u;
        else if (std::sscanf(l.c_str(), "BEGIN %d", &x) == 1) { prim = x; half = false; }
        else if (l == "END") prim = -1;
        else if (std::sscanf(l.c_str(), "VERTEX %d %d", &x, &y) == 2) {
            if (prim == IDisplay::kRects) {
                if (half) out.push_back({'R', c, a, vx, vy, x, y});
                half = !half;
                vx = x;
                vy = y;
            } else if (prim == IDisplay::kPoints) {
                out.push_back({'P', c, a, x, y, x, y});
            }
        }
    }
    return out;
}

bool hasRect(const std::vector<Prim>& p, uint32_t c, unsigned a, int x0, int y0, int x1, int y1) {
    for (const Prim& r : p)
        if (r.kind == 'R' && r.colour == c && r.alpha == a && r.x0 == x0 && r.y0 == y0 && r.x1 == x1 && r.y1 == y1)
            return true;
    return false;
}

bool hasPoint(const std::vector<Prim>& p, uint32_t c, int x, int y) {
    for (const Prim& r : p)
        if (r.kind == 'P' && r.colour == c && r.x0 == x && r.y0 == y) return true;
    return false;
}

bool hasLine(const std::vector<std::string>& dl, const std::string& s) {
    for (const std::string& l : dl)
        if (l == s) return true;
    return false;
}

bool hasText(const std::vector<std::string>& dl, const char* s) {
    for (const std::string& l : dl)
        if (l.rfind("TEXT", 0) == 0 && l.find(std::string("\"") + s + "\"") != std::string::npos) return true;
    return false;
}

std::string textLine(const std::vector<std::string>& dl, const char* s) {
    for (const std::string& l : dl)
        if (l.rfind("TEXT", 0) == 0 && l.find(std::string("\"") + s + "\"") != std::string::npos) return l;
    return "";
}

// Runs `fn` inside one frame on a fresh recording display.
template <class F>
std::vector<std::string> frame(F fn) {
    static RecordingDisplay d;
    Gfx g(d);
    g.beginFrame(0);
    fn(g);
    g.endFrame();
    return d.last;
}

const Theme& O() { return theme(0); }

}  // namespace

TEST(theme_roles_point_at_stock_slots) {
    const Theme& o = theme(0);
    CHECK_EQ(o.bg, o.slot(slot::kFrameClear));
    CHECK_EQ(o.text, 0xFFFFFFu);
    CHECK_EQ(o.dim, 0x777777u);
    CHECK_EQ(o.panel, 0x323132u);
    CHECK_EQ(o.panel2, 0x222222u);
    CHECK_EQ(o.panelDark, 0x0A0A0Au);
    CHECK_EQ(o.accent, 0xCD1818u);
    CHECK_EQ(o.good, 0x30CB3Fu);
    CHECK_EQ(o.yellow, 0xFEF749u);
    CHECK_EQ(o.track[1], 0xFEF749u);
    CHECK_EQ(o.trackDark[3], 0x183744u);
    CHECK_EQ(o.grid, 0x222222u);   // slot +0x440 grid line colour
    // Monochrome is the light theme: grey frame, light title band, black
    // title text, light-grey panels.
    const Theme& m = theme(1);
    CHECK_EQ(m.bg, 0x4B4B4Bu);
    CHECK_EQ(m.slot(slot::kTitle + 1), 0xB9B9BAu);
    CHECK_EQ(m.slot(slot::kTitle), 0x000000u);
    CHECK_EQ(m.panelDark, 0x3E3E3Eu);
    CHECK_EQ(m.text, 0xC9C9CAu);
    CHECK_EQ(m.accent, 0xDD2020u);
    CHECK_EQ(m.cyan, 0x00A4B3u);
    const Theme& a = theme(2);
    CHECK_EQ(a.bg, 0x000000u);
    CHECK_EQ(a.track[0], 0xD0D0D0u);
    CHECK(std::strcmp(theme(9).name, "Original") == 0);
}

// Each channel within +-3 of the stock pixel.
static bool near3(uint32_t got, uint32_t want) {
    for (int sh = 0; sh <= 16; sh += 8) {
        const int d = int((got >> sh) & 255) - int((want >> sh) & 255);
        if (d < -3 || d > 3) return false;
    }
    return true;
}

TEST(antialias_levels_reproduce_stock_pixels) {
    // Title hairline over the 080808 band: rows 27/28/29 = 1d / 8f / 1d.
    CHECK(near3(wg::mix(0x080808, 0xFFFFFF, wg::kHairCoreA), 0x8F8F8F));
    CHECK(near3(wg::mix(0x080808, 0xFFFFFF, wg::kTitleHaloA), 0x1D1D1D));
    CHECK(near3(wg::mix(0x0A0A0A, 0xFFFFFF, wg::kTitleHaloA), 0x1E1E1E));   // row 29 on a panel
    CHECK(near3(wg::mix(0x000000, 0xFFFFFF, wg::kTitleHaloA), 0x1A1A1A));   // row 29 on black
    // Separators 131313 / 464646 / 131313 on the 0a0a0a strip.
    CHECK(near3(wg::mix(0x0A0A0A, 0x777777, wg::kHairCoreA), 0x464646));
    CHECK(near3(wg::mix(0x0A0A0A, 0x777777, wg::kHairHaloA), 0x131313));
    // Underlines: green 1f7427 / halo 0d1b0e, red core (117, 18, 18).
    CHECK(near3(wg::mix(0x0A0A0A, 0x30CB3F, wg::kHairCoreA), 0x1F7427));
    CHECK(near3(wg::mix(0x0A0A0A, 0x30CB3F, wg::kHairHaloA), 0x0D1B0E));
    CHECK(near3(wg::mix(0x0A0A0A, 0xCD1818, wg::kHairCoreA), 0x751212));
    // 1.5-px outlines: white selection e3e3e3 / 252525, red frame b71616 / 200c0c.
    CHECK(near3(wg::mix(0x0A0A0A, 0xFFFFFF, wg::kStrokeA1), 0xE3E3E3));
    CHECK(near3(wg::mix(0x0A0A0A, 0xFFFFFF, wg::kStrokeA2), 0x252525));
    CHECK(near3(wg::mix(0x0A0A0A, 0xCD1818, wg::kStrokeA1), 0xB71616));
    CHECK(near3(wg::mix(0x0A0A0A, 0xCD1818, wg::kStrokeA2), 0x200C0C));
    // Soft 1-px edges: value-bar 7a7a7a on the 080808 track, red fill 641010.
    CHECK(near3(wg::mix(0x080808, 0xFFFFFF, wg::kEdgeA), 0x7A7A7A));
    CHECK(near3(wg::mix(0x0A0A0A, 0xCD1818, wg::kEdgeA), 0x641010));
    CHECK_EQ(wg::mix(0x123456, 0xABCDEF, 0), 0x123456u);
    CHECK_EQ(wg::mix(0x123456, 0xABCDEF, 255), 0xABCDEFu);
}

TEST(title_bar_geometry) {
    wg::TitleStatus st;
    st.pattern = 10;
    st.step = 1;
    auto dl = frame([&](Gfx& g) {
        wg::titleBar(g, O(), "Pattern 10", "5. Bass02(135BPM)", &st);
        wg::titleHairline(g, O());
    });
    auto p = prims(dl);
    CHECK(hasRect(p, 0x080808, 255, 0, 0, 799, 29));          // band under rows 27..29 (C3)
    CHECK(hasRect(p, 0xFFFFFF, wg::kHairCoreA, 0, 28, 799, 28));
    CHECK(hasRect(p, 0xFFFFFF, wg::kTitleHaloA, 0, 27, 799, 27));
    CHECK(hasRect(p, 0xFFFFFF, wg::kTitleHaloA, 0, 29, 799, 29));
    // Pause glyph: two 3 x 12 bars at x 376 / 381, y 8..19.
    CHECK(hasRect(p, 0xFFFFFF, 255, 376, 8, 378, 19));
    CHECK(hasRect(p, 0xFFFFFF, 255, 381, 8, 383, 19));
    // ROM fallback (no custom fonts): time right-anchored at 360.
    char want[32];
    std::snprintf(want, sizeof(want), "TEXT %d 13 ", 360 - textWidth(kFont4, "0m:00s"));
    CHECK(textLine(dl, "0m:00s").rfind(want, 0) == 0);
    CHECK(textLine(dl, "P10.001").rfind("TEXT 404 13 ", 0) == 0);
    CHECK(textLine(dl, "Pattern 10").rfind("TEXT 9 13 ", 0) == 0);
    CHECK(textLine(dl, "5. Bass02(135BPM)").rfind("TEXT 790 13 ", 0) == 0);
    CHECK(hasLine(dl, "SCISSOR_XY 9 0"));
    CHECK(hasLine(dl, "SCISSOR_SIZE 270 27"));
    // Hidden status (Sample Editor), long right text dropped at the end.
    static const char* kLong = "Jude Frankum - Chill Out Remix (Extended Version) - final master take 3";
    auto dl2 = frame([&](Gfx& g) {
        wg::titleBar(g, O(), "Sample Editor 1/2", kLong, nullptr);
    });
    CHECK(!hasText(dl2, "0m:00s"));
    bool clipped = false;
    for (const std::string& l : dl2)
        if (l.rfind("TEXT 790", 0) == 0) {
            const size_t q = l.find('"');
            const std::string s = l.substr(q + 1, l.size() - q - 2);
            clipped = s.size() < std::strlen(kLong) && std::string(kLong).rfind(s, 0) == 0 &&
                      textWidth(kFont4, s.c_str()) <= wg::kRightTextX - (wg::kTitleTextX + wg::kTitleClipW + 10);
        }
    CHECK(clipped);
    char b[16];
    wg::formatStatusTime(b, sizeof(b), 125);
    CHECK(std::strcmp(b, "2m:05s") == 0);
    wg::formatStatusPosition(b, sizeof(b), 10, 14, true);
    CHECK(std::strcmp(b, "P0A.0E") == 0);
}

TEST(bottom_strip_and_labels) {
    auto dl = frame([&](Gfx& g) {
        wg::bottomStrip(g, O(), 0xBF);   // Song: no separator at x 600
        wg::BarLabelStyle st;
        st.underline = wg::kUnderlineGreen;
        wg::barLabel(g, O(), 0, false, "Pattern", "10", st);
        wg::BarLabelStyle off;
        off.enabled = false;
        off.underline = wg::kUnderlineRed;
        wg::barLabel(g, O(), 3, false, "Fill", nullptr, off);
        wg::BarLabelStyle more;
        more.pages = 3;
        wg::barLabel(g, O(), 7, false, "More", nullptr, more);
        wg::barFocus(g, O(), 1);
        wg::barFill(g, O(), 6);
    });
    auto p = prims(dl);
    CHECK(hasRect(p, 0x0A0A0A, 255, 1, 425, 798, 479));          // strip
    CHECK(hasRect(p, 0x0A0A0A, 230, 1, 424, 798, 424));          // its soft top edge
    CHECK(hasRect(p, 0x777777, wg::kHairCoreA, 100, 438, 100, 479));
    CHECK(hasRect(p, 0x777777, wg::kHairHaloA, 99, 438, 99, 479));
    CHECK(hasRect(p, 0x777777, wg::kHairHaloA, 100, 437, 100, 437));
    CHECK(hasRect(p, 0x777777, wg::kHairCoreA, 700, 438, 700, 479));
    CHECK(!hasRect(p, 0x777777, wg::kHairCoreA, 600, 438, 600, 479));
    // Two-line label: grey title at y 441, white value at y 461.
    CHECK(textLine(dl, "Pattern").rfind("TEXT 50 441 ", 0) == 0);
    CHECK(textLine(dl, "10").rfind("TEXT 50 461 ", 0) == 0);
    CHECK(textLine(dl, "Fill").rfind("TEXT 350 451 ", 0) == 0);
    CHECK(textLine(dl, "More").rfind("TEXT 750 441 ", 0) == 0);
    // Underlines x cx - 40 .. cx + 40 at y 474.
    CHECK(hasRect(p, 0x30CB3F, wg::kHairCoreA, 10, 474, 90, 474));
    CHECK(hasRect(p, 0x30CB3F, wg::kHairHaloA, 10, 473, 90, 473));
    CHECK(hasRect(p, 0xCD1818, wg::kHairCoreA, 310, 474, 390, 474));
    // Page dots 737 / 750 / 763 at y 463, current grey, others dark.
    CHECK(hasPoint(p, 0x777777, 737, 463));
    CHECK(hasPoint(p, 0x222222, 750, 463));
    CHECK(hasPoint(p, 0x222222, 763, 463));
    // Focus frame of label 1: centre lines 101 / 198, 424 / 479.
    CHECK(hasRect(p, 0xCD1818, 255, 101, 424, 198, 424));
    CHECK(hasRect(p, 0xCD1818, 255, 101, 425, 101, 478));
    CHECK(hasRect(p, 0xCD1818, wg::kStrokeA1, 100, 423, 199, 423));
    // Fill of label 6: (603, 424, 95, 56) + soft edges at 602 / 698 / row 423.
    CHECK(hasRect(p, 0xCD1818, 255, 603, 424, 697, 479));
    CHECK(hasRect(p, 0xCD1818, wg::kEdgeA, 602, 424, 602, 479));
    CHECK(hasRect(p, 0xCD1818, wg::kEdgeA, 603, 423, 697, 423));
}

// Stock two-line labels (VISUAL_DIFF_1.9.4 §"Two-line grey set"): a name
// split over two lines ("Export / Song", "New / project") draws both lines
// grey in font 3; a value stays white in font 2 unless it is too wide for
// the label ("Line in ST" in the 10-px mono), then it falls back to font 3.
static std::string fontOf(const std::string& line) {
    const size_t f = line.find(" f");
    return f == std::string::npos ? "" : line.substr(f + 1, line.find(' ', f + 1) - f - 1);
}
TEST(bar_label_names_and_long_values_use_the_label_font) {
    RecordingDisplay d;
    d.customFonts = true;
    applyFontSet(&d, 0);
    CHECK(customFontsBound());
    char f2[8], f3[8];
    std::snprintf(f2, sizeof(f2), "f%u", unsigned(romFontFor(kFont2)));
    std::snprintf(f3, sizeof(f3), "f%u", unsigned(romFontFor(kFont3)));
    Gfx g(d);
    g.beginFrame(0);
    wg::BarLabelStyle name;
    name.nameLines = true;
    wg::barLabel(g, O(), 0, false, "New", "project", name);
    wg::BarLabelStyle val;
    wg::barLabel(g, O(), 1, false, "Source", "Line in ST", val);
    wg::barLabel(g, O(), 2, false, "Start", "0.000s", val);
    g.endFrame();
    const auto& dl = d.last;
    CHECK(fontOf(textLine(dl, "project")) == f3);
    CHECK(fontOf(textLine(dl, "Line in ST")) == f3);
    CHECK(fontOf(textLine(dl, "0.000s")) == f2);
    d.customFonts = false;
    applyFontSet(&d, 0);
}

TEST(panels_lists_value_bars) {
    auto dl = frame([&](Gfx& g) { wg::panelColumn(g, O(), 2, 2); });   // (201, 29, 197, 394)
    auto p = prims(dl);
    CHECK(hasRect(p, 0x0A0A0A, 255, 201, 29, 398, 423));          // rows y .. y + h (30..423 on screen)
    CHECK(hasRect(p, 0x0A0A0A, wg::kEdgeA, 200, 29, 200, 423));
    CHECK(hasLine(dl, "SCISSOR_XY 201 413"));
    CHECK(hasLine(dl, "SCISSOR_SIZE 198 12"));
    CHECK(hasLine(dl, "GRADIENT 201 413 0A0A0A 201 425 000000"));
    // No fade in Monochrome (flag +0x444 = 0).
    auto dlm = frame([&](Gfx& g) { wg::panel(g, theme(1), 1); });
    CHECK(!hasLine(dlm, "GRADIENT 1 413 3E3E3E 1 425 000000"));

    // Focused column frame: centre lines x .. x + w, 31 .. 422.
    p = prims(frame([&](Gfx& g) { wg::panelFocus(g, O(), 1, 197); }));
    CHECK(hasRect(p, 0xCD1818, 255, 1, 31, 198, 31));
    CHECK(hasRect(p, 0xCD1818, 255, 1, 422, 198, 422));

    // List: 14 rows of 27 px, selection outline from (x + 3, y + 4 + 27 sel).
    CHECK_EQ(wg::listVisibleRows(394), 14);
    CHECK_EQ(wg::listFollow(0, 30, 14, 0), 0);
    CHECK_EQ(wg::listFollow(13, 30, 14, 0), 1);     // one row of context
    CHECK_EQ(wg::listFollow(29, 30, 14, 0), 16);
    CHECK_EQ(wg::listFollow(5, 30, 14, 10), 4);
    wg::ListView lv;
    lv.x = 601;
    lv.count = 20;
    lv.sel = 0;
    lv.item = [](void*, int i, char* o, int cap) { std::snprintf(o, size_t(cap), "Item %d", i); };
    dl = frame([&](Gfx& g) { wg::list(g, O(), lv); });
    p = prims(dl);
    CHECK(hasRect(p, 0xFFFFFF, 255, 604, 33, 788, 33));      // scrollbar: inner w - 13
    CHECK(hasRect(p, 0xFFFFFF, 255, 604, 60, 788, 60));
    CHECK(hasRect(p, 0x000000, 255, 792, 37, 795, 416));    // scroll track
    CHECK(textLine(dl, "Item 0").rfind("TEXT 614 46 ", 0) == 0);
    CHECK(textLine(dl, "Item 13").rfind("TEXT 614 397 ", 0) == 0);
    CHECK(!hasText(dl, "Item 14"));
    lv.count = 4;
    lv.x = 1;
    lv.selStyle = wg::ListSel::kRed;
    p = prims(frame([&](Gfx& g) { wg::list(g, O(), lv); }));
    CHECK(hasRect(p, 0xCD1818, 255, 4, 33, 195, 33));        // no scrollbar: inner w - 6

    // Value bar at (1, 29, 97, 394): track 11..87 x 40..409, gradient over
    // its first 9 px, 50 % fill = rows 224..408 (x 12..87).
    dl = frame([&](Gfx& g) { wg::valueBar(g, O(), 1, 29, 97, 394, 0.5f); });
    p = prims(dl);
    CHECK(hasRect(p, 0x080808, 255, 11, 40, 88, 409));
    CHECK(hasLine(dl, "GRADIENT 11 40 000000 19 40 080808"));
    CHECK(hasRect(p, 0xFFFFFF, 255, 12, 224, 87, 408));
    CHECK(hasRect(p, 0xFFFFFF, wg::kEdgeA, 12, 223, 87, 223));
    CHECK(hasRect(p, 0xFFFFFF, wg::kEdgeA, 11, 224, 11, 408));
    p = prims(frame([&](Gfx& g) { wg::valueBar(g, O(), 1, 29, 97, 394, 1.0f); }));
    CHECK(hasRect(p, 0xFFFFFF, 255, 12, 39, 87, 408));      // 100 % = y 39 (Part 2 §8.26)
    wg::ValueBarStyle bi;
    bi.bipolar = true;
    p = prims(frame([&](Gfx& g) { wg::valueBar(g, O(), 101, 29, 97, 394, 0.0f, bi); }));
    CHECK(hasRect(p, 0xFFFFFF, 255, 112, 224, 187, 224));   // centre line y 224
    wg::ValueBarStyle green;
    green.fill = 0x30CB3F;
    p = prims(frame([&](Gfx& g) { wg::valueBar(g, O(), 401, 29, 97, 394, 0.59f, green); }));
    CHECK(hasRect(p, 0x30CB3F, 255, 412, 190, 487, 408));   // ceil(0.59 x 370) = 219 rows
}

TEST(popups_and_text) {
    auto dl = frame([&](Gfx& g) { wg::progressPopup(g, O(), "Loading files list", 50); });
    auto p = prims(dl);
    CHECK(hasRect(p, 0x222222, 255, 190, 190, 609, 289));
    CHECK(hasRect(p, 0x121212, 255, 250, 250, 549, 251));   // track: x + w / 7, 5 w / 7, 2 px
    CHECK(hasRect(p, 0xFFFFFF, 255, 250, 250, 399, 251));
    CHECK(textLine(dl, "Loading files list").rfind("TEXT 400 223 ", 0) == 0);
    dl = frame([&](Gfx& g) {
        CHECK_EQ(wg::textBox(g, O(), 0, 0, 120, 200, kFontMono, "one two three four five six"), 3);
    });
    CHECK(hasText(dl, "one two"));
    CHECK(hasText(dl, "three four"));
    CHECK(hasText(dl, "five six"));
    p = prims(frame([&](Gfx& g) { wg::key(g, O(), 13, 130, 65, 65, "Q", true); }));
    CHECK(hasRect(p, 0x0A0A0A, 255, 14, 131, 76, 193));
    CHECK(hasRect(p, 0xCD1818, 255, 14, 131, 76, 131));
}

TEST(ui_chrome_on_screens) {
    Harness h;
    App& a = *h.app;
    a.ui.dirty();
    h.run(40);
    auto p = prims(h.disp.last);
    CHECK(hasText(h.disp.last, "0m:00s"));
    CHECK(!hasText(h.disp.last, "STOP"));
    CHECK(hasRect(p, 0x080808, 255, 0, 0, 799, 29));
    CHECK(hasRect(p, 0xFFFFFF, wg::kHairCoreA, 0, 28, 799, 28));
    CHECK(hasRect(p, 0x0A0A0A, 255, 1, 425, 798, 479));
    // The strip is drawn before the screen, the hairline after it.
    int strip = -1, hair = -1, n = 0;
    for (const Prim& r : p) {
        if (strip < 0 && r.colour == 0x0A0A0A && r.y0 == 425 && r.x0 == 1) strip = n;
        if (r.colour == 0xFFFFFF && r.alpha == wg::kHairCoreA && r.y0 == 28) hair = n;
        ++n;
    }
    CHECK(strip >= 0 && hair > strip);
    // Bottom labels are text only (no 323132 boxes).
    CHECK(!hasRect(p, 0x323132, 255, 3, 424, 96, 478));
    // Sample Editor: no status group.
    h.tap(btn::SampleEditor);
    h.run(40);
    CHECK(!hasText(h.disp.last, "0m:00s"));
    // Separator mask follows wide labels.
    BottomBar b;
    b.setWide(6, "Effect");
    CHECK_EQ(int(b.separatorMask()), 0x7E);
    b.clear();
    CHECK_EQ(int(b.separatorMask()), 0xFE);
}

// --- 1.9.5 W2b: Part 2 chrome (VISUAL_DIFF_1.9.4.md §7..§10) ---------------

TEST(status_layout_follows_font_set) {
    // Part 2 C10 / §10: status ink at x 300 with set 0, 288 with set 1;
    // pause bars 376 / 381 (set 0) and 364 / 369 (set 1).
    static RecordingDisplay d;
    d.customFonts = true;
    CHECK(applyFontSet(&d, 0));
    wg::StatusLayout l = wg::statusLayout(0);
    CHECK_EQ(l.timeX, 300);
    CHECK_EQ(l.iconX, 370);
    CHECK_EQ(l.symW, 12);
    CHECK_EQ(l.posX, 404);
    CHECK(applyFontSet(&d, 1));
    l = wg::statusLayout(0);
    CHECK_EQ(l.timeX, 288);
    CHECK_EQ(l.symW, 18);
    CHECK_EQ(l.iconX, 352);
    CHECK_EQ(l.iconX + l.symW - 6, 364);
    CHECK_EQ(l.iconX + l.symW - 1, 369);
    CHECK_EQ(l.posX, 392);
    wg::TitleStatus st;
    auto dl = frame([&](Gfx& g) { wg::titleBar(g, O(), "Config", nullptr, &st); });
    CHECK(textLine(dl, "0m:00s").rfind("TEXT 288 13 ", 0) == 0);
    CHECK(textLine(dl, "P1.001").rfind("TEXT 392 13 ", 0) == 0);
    // 10 minutes and more: one pad space less.
    CHECK_EQ(wg::statusLayout(600).timeX, 284);
    d.customFonts = false;
    applyFontSet(nullptr, 0);
}

TEST(theme_flags_part2) {
    const Theme &o = theme(0), &m = theme(1), &a = theme(2);
    CHECK(o.barLines() && m.barLines() && !a.barLines());
    CHECK(o.panelFade() && !m.panelFade() && !a.panelFade());
    CHECK(o.cursorRowGradient() && m.cursorRowGradient() && !a.cursorRowGradient());
    CHECK_EQ(o.cursorRowShade(), 0x000000u);
    CHECK_EQ(m.cursorRowShade(), 0x303030u);
    CHECK_EQ(a.cursorRowShade(), 0x222222u);
    CHECK_EQ(o.cursorCellText(), 0xFFFFFFu);
    CHECK_EQ(m.cursorCellText(), 0x000000u);
    CHECK_EQ(a.cursorCellText(), 0x000000u);
    // Measured on-screen values (Part 2 §9): Monochrome underlines 3c9144 /
    // 9a3333 and separators 606060 on the 4b4b4b strip.
    CHECK(near3(wg::mix(m.slot(slot::kStrip + 1), m.slot(slot::kGreen), wg::kHairCoreA), 0x3C9144));
    CHECK(near3(wg::mix(m.slot(slot::kStrip + 1), m.slot(slot::kSolidRect), wg::kHairCoreA), 0x9A3333));
    CHECK(near3(wg::mix(m.slot(slot::kStrip + 1), m.slot(slot::kStrip), wg::kHairCoreA), 0x606060));
    // Anniversary title hairline 111111 / 6e6e6e on black.
    CHECK(near3(wg::mix(0, a.slot(slot::kTitleHairline), wg::kHairCoreA), 0x6E6E6E));
    CHECK(near3(wg::mix(0, a.slot(slot::kTitleHairline), wg::kTitleHaloA), 0x111111));
}

TEST(strip_variants_and_bar_lines) {
    CHECK_EQ(wg::stripColour(O(), wg::StripStyle::kNormal), 0x0A0A0Au);
    CHECK_EQ(wg::stripColour(O(), wg::StripStyle::kDark), 0x050505u);
    CHECK_EQ(wg::stripColour(theme(1), wg::StripStyle::kDark), 0x4B4B4Bu);
    // 050505 strip, separators 434343 on screen (Part 2 C5).
    CHECK(near3(wg::mix(0x050505, 0x777777, wg::kHairCoreA), 0x434343));
    BottomBar b;
    b.setStripStyle(wg::StripStyle::kDark);
    b.set(0, "Where?");
    auto p = prims(frame([&](Gfx& g) { b.drawStrip(g, O()); }));
    CHECK(hasRect(p, 0x050505, 255, 1, 425, 798, 479));
    b.clear();
    CHECK(b.stripStyle == wg::StripStyle::kNormal);
    // Anniversary: no separators, no underlines (C6).
    auto dl = frame([&](Gfx& g) {
        wg::bottomStrip(g, theme(2), 0xFE);
        wg::BarLabelStyle st;
        st.underline = wg::kUnderlineGreen;
        wg::barLabel(g, theme(2), 0, false, "Pattern", "1", st);
    });
    p = prims(dl);
    for (const Prim& r : p) CHECK(!(r.y0 == 474 || (r.x0 == 100 && r.y0 == 438)));
    // Red underline is a state: an enabled label keeps it (C7).
    wg::BarLabelStyle rec;
    rec.underline = wg::kUnderlineEditMode;
    dl = frame([&](Gfx& g) { wg::barLabel(g, O(), 4, false, "Fill", nullptr, rec); });
    p = prims(dl);
    CHECK(hasRect(p, 0xCD1818, wg::kHairCoreA, 410, 474, 490, 474));
    CHECK(textLine(dl, "Fill").find(" 777777") != std::string::npos || hasText(dl, "Fill"));
    // Class-0x20 label strip: single-line labels top-aligned (caps 437..447).
    BottomBar k;
    k.labelStrip = true;
    k.set(0, "Enter");
    k.set(7, "Render", "Selection");
    dl = frame([&](Gfx& g) { k.drawLabels(g, O()); });
    CHECK(textLine(dl, "Enter").rfind("TEXT 50 442 ", 0) == 0);
    CHECK(textLine(dl, "Render").rfind("TEXT 750 441 ", 0) == 0);
}

TEST(popup_headers) {
    auto dl = frame([&](Gfx& g) { wg::rightPopupHeader(g, O(), "Instruments"); });
    auto p = prims(dl);
    CHECK(hasRect(p, 0xFFFFFF, 255, 600, 0, 799, 26));
    CHECK(textLine(dl, "Instruments").rfind("TEXT 621 13 ", 0) == 0);
    bool r27 = false, r28 = false;
    for (const Prim& r : p) {
        if (r.y0 == 27 && r.x0 == 600 && r.x1 == 799 && near3(r.colour, 0x868686)) r27 = true;
        if (r.y0 == 28 && r.x0 == 600 && r.x1 == 799 && near3(r.colour, 0x525252)) r28 = true;
    }
    CHECK(r27 && r28);
    p = prims(frame([&](Gfx& g) { wg::rightPopupPanel(g, O()); }));
    CHECK(hasRect(p, 0x0A0A0A, 255, 601, 30, 799, 479));
    dl = frame([&](Gfx& g) { wg::fullPopupHeader(g, O(), "Notes"); });
    p = prims(dl);
    CHECK(hasRect(p, 0xFFFFFF, 255, 2, 0, 797, 26));
    CHECK(textLine(dl, "Notes").rfind("TEXT 12 13 ", 0) == 0);

    // Ui: the status group goes, F7 / F8 are hidden, the header is drawn.
    Harness h;
    App& a = *h.app;
    a.ui.bar.set(6, "Cancel");
    a.ui.openPopupHeader("Instruments");
    CHECK(a.ui.statusHidden());
    h.run(40);
    CHECK(!hasText(h.disp.last, "0m:00s"));
    CHECK(hasText(h.disp.last, "Instruments"));
    CHECK(hasRect(prims(h.disp.last), 0xFFFFFF, 255, 600, 0, 799, 26));
    a.ui.closePopupHeader();
    a.ui.dirty();
    h.run(40);
    CHECK(hasText(h.disp.last, "0m:00s"));
    a.ui.hideStatus(true);
    h.run(40);
    CHECK(!hasText(h.disp.last, "0m:00s"));
    a.ui.openPopupHeader("Fx");
    h.tap(btn::Config);
    h.run(40);
    CHECK(a.ui.popupHeader() == Ui::PopupHeader::kNone);   // reset on a screen switch
    CHECK(!a.ui.statusHidden());
}

TEST(keyboard_and_name_field_020) {
    auto p = prims(frame([&](Gfx& g) { wg::keyboardKey(g, O(), 13, 143, 61, 61, "1", false); }));
    CHECK(hasRect(p, 0x0A0A0A, 255, 13, 143, 73, 203));
    p = prims(frame([&](Gfx& g) { wg::keyboardKey(g, O(), 208, 273, 61, 61, "f", false, true); }));
    CHECK(hasRect(p, 0xFFFFFF, 255, 208, 273, 268, 333));     // home key
    p = prims(frame([&](Gfx& g) { wg::nameField(g, O(), 1, 31, 798, 93, "", 0, false, true); }));
    CHECK(hasRect(p, 0x0A0A0A, 255, 1, 31, 798, 123));
    CHECK(hasRect(p, 0x080808, 255, 12, 42, 787, 112));       // inner box
    CHECK(hasRect(p, 0x080808, 128, 11, 41, 788, 113));       // its soft edge (090909)
    CHECK(hasRect(p, 0xCD1818, 255, 1, 31, 798, 31));         // focus outline core rows
    CHECK(hasRect(p, 0xCD1818, 255, 400, 65, 400, 89));       // empty text: cursor at 400

    Harness h;
    App& a = *h.app;
    a.ui.kb.open("Edit Pattern 2 Name", "", 31, "Save", nullptr, nullptr);
    a.ui.dirty();
    h.run(40);
    auto dl = h.disp.last;
    p = prims(dl);
    CHECK(hasText(dl, "Edit Pattern 2 Name"));
    CHECK(hasText(dl, "0m:00s"));                                // screen's status stays
    CHECK(hasRect(p, 0x0A0A0A, 255, 663, 143, 788, 203));      // Backspace (2 cells)
    CHECK(hasRect(p, 0x0A0A0A, 255, 468, 338, 788, 398));      // Space (5 cells)
    CHECK(hasRect(p, 0xFFFFFF, 255, 403, 273, 463, 333));      // j
    CHECK(textLine(dl, "Enter").rfind("TEXT 50 442 ", 0) == 0);
    CHECK(textLine(dl, "Save").rfind("TEXT 750 442 ", 0) == 0);
    a.ui.kb.close();
}

TEST(value_bar_zero_line_and_boot_splash) {
    // Value 0 = the 1.5-px line on the base row (Part 2 §7.4, 042 Attack).
    auto p = prims(frame([&](Gfx& g) { wg::valueBar(g, O(), 301, 29, 97, 394, 0.0f); }));
    CHECK(hasRect(p, 0xFFFFFF, 255, 312, 408, 387, 408));
    CHECK(hasRect(p, 0xFFFFFF, wg::kEdgeA, 312, 407, 387, 407));
    CHECK(hasRect(p, 0xFFFFFF, wg::kEdgeA, 312, 409, 387, 409));
    // Boot splash (035): bar core from x 251, rows 331..332; 64 % = 192 px.
    auto dl = frame([&](Gfx& g) { wg::bootSplash(g, theme(1), "Opening last project", 64, "v1.9.5"); });
    p = prims(dl);
    CHECK(hasRect(p, 0x4B4B4B, 255, 0, 0, 799, 479));
    CHECK(hasRect(p, 0xC9C9CA, 255, 251, 331, 442, 332));
    CHECK(textLine(dl, "Opening last project").rfind("TEXT 400 308 ", 0) == 0);
    CHECK(textLine(dl, "v1.9.5").rfind("TEXT 11 466 ", 0) == 0);
}

TEST(status_cursor_step_and_fill_rounding) {
    Harness h;
    App& a = *h.app;
    a.ui.setStatusCursorStep(13);
    a.ui.dirty();
    h.run(40);
    char want[16];
    wg::formatStatusPosition(want, sizeof(want), a.seq.currentPatternNumber(), 13 + (a.cfg.numbering == 0 ? 0 : 1),
                             a.cfg.numbering == 2);
    CHECK(hasText(h.disp.last, want));
    h.tap(btn::Config);
    CHECK_EQ(a.ui.statusCursorStep(), -1);   // reset on a screen switch
    // Fill rounds up over h - 24 rows: 1 % of 370 = 3.7 -> 4 rows.
    auto p = prims(frame([&](Gfx& g) { wg::valueBar(g, O(), 1, 29, 97, 394, 0.01f); }));
    CHECK(hasRect(p, 0xFFFFFF, 255, 12, 405, 87, 408));
}

TEST(page_dots_focus_title_dot_words) {
    auto p = prims(frame([&](Gfx& g) { wg::pageDots(g, O(), 750, 2, 0, 0x323132); }));   // 011
    CHECK(hasPoint(p, 0x777777, 744, 463));
    CHECK(hasPoint(p, 0x323132, 757, 463));
    // A focused label keeps the grey title (006 "Start").
    wg::BarLabelStyle f;
    f.focus = true;
    auto dl = frame([&](Gfx& g) { wg::barLabel(g, O(), 0, false, "Start", "0.000s", f); });
    bool grey = false;
    uint32_t c = 0;
    for (const std::string& l : dl) {
        unsigned u;
        if (l.rfind("COLOR ", 0) == 0 && std::sscanf(l.c_str(), "COLOR %x", &u) == 1) c = u;
        if (l.rfind("TEXT", 0) == 0 && l.find("\"Start\"") != std::string::npos) grey = c == 0x777777;
    }
    CHECK(grey);
    // wg::dot counts its display-list words.
    static RecordingDisplay d;
    Gfx g(d);
    g.beginFrame(0);
    const uint32_t w0 = g.dlWords();
    wg::dot(g, 10, 10, 3.0f, 0xFFFFFF);
    CHECK(g.dlWords() >= w0 + 5);
    g.endFrame();
}

// ---- 2.x CPU cell [I] (spec 2x-02 §2.2) -------------------------------------

TEST(cpu_cell_levels) {
    using wg::CpuLevel;
    // Off / no data: hidden.
    CHECK(!wg::cpuCell(2, true, 990, true, true).shown());
    CHECK(!wg::cpuCell(1, false, 500, false, false).shown());
    // Polyend's levels (Tracker Mini 2.0 / Tracker+, Polyend on backstage.polyend.com
    // "CPU almost always in red"): yellow 85 %, orange 93 %, red 99 % of the
    // peak block. Auto: hidden below 85 %, Always: dim.
    CHECK(!wg::cpuCell(0, true, 849, false, false).shown());
    CHECK(!wg::cpuCell(0, true, 700, false, false).shown());
    CHECK(wg::cpuCell(1, true, 849, false, false).level == CpuLevel::kNormal);
    CHECK_EQ(wg::cpuCell(1, true, 594, false, false).percent, 59);
    CHECK_EQ(wg::cpuCell(1, true, 595, false, false).percent, 60);
    CHECK(wg::cpuCell(0, true, 850, false, false).level == CpuLevel::kWarn);
    CHECK(wg::cpuCell(0, true, 929, false, false).level == CpuLevel::kWarn);
    CHECK(wg::cpuCell(0, true, 930, false, false).level == CpuLevel::kAlarm);
    CHECK(wg::cpuCell(0, true, 300, false, true).level == CpuLevel::kAlarm);    // governor cut
    CHECK(wg::cpuCell(0, true, 989, false, false).level == CpuLevel::kAlarm);
    CHECK(wg::cpuCell(0, true, 990, false, false).level == CpuLevel::kCritical);
    wg::CpuCell o = wg::cpuCell(0, true, 200, true, false);                    // overrun
    CHECK(o.level == CpuLevel::kCritical && o.overrun);
    CHECK_EQ(wg::cpuCell(0, true, 1400, true, false).percent, 99);
    // Colours: stock slots (dim, yellow, orange, red).
    CHECK_EQ(wg::cpuCellColour(O(), CpuLevel::kNormal), O().dim);
    CHECK_EQ(wg::cpuCellColour(O(), CpuLevel::kWarn), O().slot(slot::kYellow));
    CHECK_EQ(wg::cpuCellColour(O(), CpuLevel::kAlarm), O().slot(slot::kOrange));
    CHECK_EQ(wg::cpuCellColour(O(), CpuLevel::kCritical), O().slot(slot::kSolidRect));
}

TEST(cpu_cell_title_geometry_and_words) {
    static RecordingDisplay d;
    d.customFonts = true;
    CHECK(applyFontSet(&d, 0));
    wg::TitleStatus st;
    // Hidden: the stock frame, byte for byte.
    auto ref = frame([&](Gfx& g) { wg::titleBar(g, O(), "Pattern 10", "5. Bass02", &st); });
    wg::TitleStatus hid;
    hid.cpu = wg::cpuCell(2, true, 900, false, false);
    hid.cpu.right = int16_t(wg::cpuCellRight(0));
    CHECK(frame([&](Gfx& g) { wg::titleBar(g, O(), "Pattern 10", "5. Bass02", &hid); }) == ref);
    CHECK(hasLine(ref, "SCISSOR_SIZE 270 27"));
    // Shown: 60 x 24 cell right-aligned at the time ink (300) - 4; the title
    // clip ends 10 px before it (217 px from x 9).
    st.cpu = wg::cpuCell(1, true, 452, false, false);
    st.cpu.right = int16_t(wg::cpuCellRight(st.seconds));
    CHECK_EQ(st.cpu.right, 296);
    // The cell is as wide as "CPU 00%" (>= 60 px): the clip ends 10 px before it.
    const int clipW = 296 - wg::cpuCellWidth() - 10 - 9;
    CHECK_EQ(wg::titleClipWidth(&st.cpu), clipW);
    char clipLine[32];
    std::snprintf(clipLine, sizeof(clipLine), "SCISSOR_SIZE %d 27", clipW);
    auto dl = frame([&](Gfx& g) { wg::titleBar(g, O(), "Pattern 10", "5. Bass02", &st); });
    CHECK(hasLine(dl, clipLine));
    // Glyphs: "CPU" then "45%", all inside x 236..296.
    int glyphs = 0, minX = 9999, maxX = 0;
    const int lo = 296 - wg::cpuCellWidth();
    for (const std::string& l : dl) {
        unsigned x, y, h, c;
        if (std::sscanf(l.c_str(), "VERTEX2II %u %u %u %u", &x, &y, &h, &c) == 4 && int(x) >= lo && int(x) < 296) {
            ++glyphs;
            minX = std::min(minX, int(x));
            maxX = std::max(maxX, int(x) + textWidth(kFont4, std::string(1, char(c)).c_str()));
        }
    }
    CHECK_EQ(glyphs, 6);
    CHECK(minX >= lo);
    CHECK_EQ(maxX, 296);
    // Display-list words of the dynamic part (status group): <= 6 more.
    auto statusWords = [&](const wg::TitleStatus& s) {
        static RecordingDisplay d2;
        Gfx g(d2);
        g.beginFrame(0);
        const uint32_t w0 = g.dlWords();
        wg::titleBarStatus(g, O(), s);
        g.endFrame();
        return g.dlWords() - w0;
    };
    wg::TitleStatus none;
    const uint32_t base = statusWords(none);
    CHECK(statusWords(st) <= base + 6);
    wg::TitleStatus ov = st;
    ov.cpu = wg::cpuCell(0, true, 1000, true, false);
    ov.cpu.right = st.cpu.right;
    CHECK(statusWords(ov) <= base + 6);
    // Overrun: "CPU!" in red.
    auto dlo = frame([&](Gfx& g) { wg::titleBar(g, O(), "Pattern 10", nullptr, &ov); });
    bool bang = false;
    for (const std::string& l : dlo) bang |= l.find(" 33") != std::string::npos && l.rfind("VERTEX2II", 0) == 0;
    CHECK(bang);
    CHECK(hasLine(dlo, "COLOR " + [&] {
        char b[8];
        std::snprintf(b, sizeof b, "%06X", unsigned(O().slot(slot::kSolidRect)));
        return std::string(b);
    }()));
    // The cached-chrome split draws the same pixels as titleBar().
    auto split = frame([&](Gfx& g) {
        wg::titleBarChrome(g, O(), "Pattern 10", "5. Bass02", true, &st.cpu);
        wg::titleBarStatus(g, O(), st);
    });
    CHECK(hasLine(split, clipLine));
    d.customFonts = false;
    applyFontSet(nullptr, 0);
}


// 2.x theme files: the Original-theme detectors (slot 58 == 0a0a0a) became
// Theme::darkStrip(): half the strip colour on a fading theme, else the strip.
TEST(dark_strip_matches_stock) {
    CHECK_EQ(theme(0).darkStrip(), 0x050505u);
    CHECK_EQ(theme(1).darkStrip(), theme(1).slot(slot::kStrip + 1));
    CHECK_EQ(theme(2).darkStrip(), theme(2).slot(slot::kStrip + 1));
    CHECK_EQ(wg::stripColour(theme(0), wg::StripStyle::kDark), 0x050505u);
    CHECK_EQ(wg::stripColour(theme(1), wg::StripStyle::kDark), 0x4B4B4Bu);
    CHECK_EQ(wg::stripColour(theme(2), wg::StripStyle::kDark), 0x000000u);
    CHECK_EQ(wg::stripColour(theme(0), wg::StripStyle::kNormal), 0x0A0A0Au);
}

// 2.x: an optional right-aligned tag per row (the Sample Loader's '~' for a
// file that will stream from the card). The name column ends before it.
TEST(list_tag_is_right_aligned_and_shrinks_the_name_column) {
    // The cut is counted in glyphs of one width ('a'). Column (scroll
    // bar): x 4..288, text from x 14, glyphs cut at innerW - 10 = 274 px.
    static const int gw = textWidth(kFont1, "a", 1);
    static const int fitN = (288 - 14) / gw - 1;   // fits the full column, not the tagged one
    auto run = [](bool tagged) {
        wg::ListView lv;
        lv.x = 1;
        lv.y = 61;
        lv.w = 297;
        lv.h = 362;
        lv.count = 20;
        lv.sel = 0;
        lv.background = false;
        lv.item = [](void*, int i, char* o, int cap) {
            std::snprintf(o, size_t(cap), "%s", i == 0 ? std::string(size_t(fitN), 'a').c_str()
                                                : i == 1 ? std::string(60, 'a').c_str()
                                                         : "short");
        };
        if (tagged) lv.tag = [](void*, int i) -> const char* { return i <= 1 ? "~" : (i == 2 ? "" : nullptr); };
        return frame([&](Gfx& g) { wg::list(g, O(), lv); });
    };
    auto count = [](const std::string& l, char c) { return int(std::count(l.begin(), l.end(), c)); };
    auto rowText = [](const std::vector<std::string>& dl, int y) {
        const std::string pre = "TEXT 14 " + std::to_string(y) + " ";
        for (const std::string& l : dl)
            if (l.rfind(pre, 0) == 0) return l;
        return std::string();
    };
    const std::vector<std::string> plain = run(false), tag = run(true);
    CHECK(!hasText(plain, "~"));
    // The tag at the column's right edge - 4, right-aligned, centred on the row.
    const std::string t = textLine(tag, "~");
    CHECK(t.rfind("TEXT 284 78 ", 0) == 0);
    CHECK(t.find(" o3072 ") != std::string::npos);
    int tags = 0;
    for (const std::string& l : tag) tags += l.rfind("TEXT", 0) == 0 && l.find("\"~\"") != std::string::npos;
    CHECK_EQ(tags, 2);   // rows 0 and 1; "" and null draw nothing
    // A long unselected name is cut textWidth("~") + 8 px earlier.
    const int shrink = (textWidth(kFont1, "~") + 8 + gw - 1) / gw;
    CHECK(count(rowText(plain, 105), 'a') < 60);
    CHECK_EQ(count(rowText(plain, 105), 'a') - count(rowText(tag, 105), 'a'), shrink);
    // A selected name that fits the full column scrolls inside the narrower one.
    CHECK_EQ(count(rowText(plain, 78), 'a'), fitN);
    CHECK(count(rowText(tag, 78), 'a') < fitN);
    CHECK(textLine(tag, "short") == textLine(plain, "short"));
}
