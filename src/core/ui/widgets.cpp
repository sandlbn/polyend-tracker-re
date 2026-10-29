// Shared stock-style UI chrome; see widgets.h.
#include "ui/widgets.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "fmath.h"

namespace ot {
namespace wg {

namespace {
inline uint32_t pick(uint32_t c, uint32_t themeC) { return c == kThemeColour ? themeC : c; }

void alphaOn(Gfx& g, uint8_t a) {
    if (a != 255) g.alpha(a);
}
void alphaOff(Gfx& g, uint8_t a) {
    if (a != 255) g.alpha(255);
}

// The four sides of the outline ring with centre lines x0..x1 / y0..y1
// moved out by d (in by -d), as non-overlapping rects of one batch.
void ringRects(Gfx& g, int x0, int y0, int x1, int y1, int d) {
    const int ax = x0 - d, bx = x1 + d, ay = y0 - d, by = y1 + d;
    if (bx < ax || by < ay) return;
    g.rectsAdd(ax, ay, bx - ax + 1, 1);
    if (by > ay) g.rectsAdd(ax, by, bx - ax + 1, 1);
    if (by - ay > 1) {
        g.rectsAdd(ax, ay + 1, 1, by - ay - 1);
        if (bx > ax) g.rectsAdd(bx, ay + 1, 1, by - ay - 1);
    }
}

void copyFit(char* out, int cap, const char* s) {
    std::snprintf(out, size_t(cap), "%s", s ? s : "");
}
}  // namespace

uint32_t mix(uint32_t bg, uint32_t fg, int a) {
    if (a <= 0) return bg & 0xFFFFFFu;
    if (a >= 255) return fg & 0xFFFFFFu;
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const int b = int((bg >> sh) & 255), f = int((fg >> sh) & 255);
        out |= uint32_t((f * a + b * (255 - a)) / 255) << sh;
    }
    return out;
}

void blendFill(Gfx& g, int x, int y, int w, int h, uint32_t c, uint8_t a) {
    if (w <= 0 || h <= 0 || a == 0) return;
    alphaOn(g, a);
    g.fill(x, y, w, h, c);
    alphaOff(g, a);
}

void hairH(Gfx& g, int x0, int x1, int y, uint32_t c) {
    if (x1 < x0) return;
    g.alpha(kHairHaloA);
    g.rectsBegin(c);
    g.rectsAdd(x0, y - 1, x1 - x0 + 1, 1);
    g.rectsAdd(x0, y + 1, x1 - x0 + 1, 1);
    g.rectsAdd(x0 - 1, y, 1, 1);
    g.rectsAdd(x1 + 1, y, 1, 1);
    g.rectsEnd();
    g.alpha(kHairCoreA);
    g.fill(x0, y, x1 - x0 + 1, 1, c);
    g.alpha(255);
}

void hairV(Gfx& g, int x, int y0, int y1, uint32_t c) {
    if (y1 < y0) return;
    g.alpha(kHairHaloA);
    g.rectsBegin(c);
    g.rectsAdd(x - 1, y0, 1, y1 - y0 + 1);
    g.rectsAdd(x + 1, y0, 1, y1 - y0 + 1);
    g.rectsAdd(x, y0 - 1, 1, 1);
    g.rectsEnd();
    g.alpha(kHairCoreA);
    g.fill(x, y0, 1, y1 - y0 + 1, c);
    g.alpha(255);
}

void strokes(Gfx& g, const int (*box)[4], int n, uint32_t c) {
    // Same-colour layers blend commutatively, so drawing every ring of one
    // level before the next gives the pixels of n separate strokes.
    static constexpr struct { uint8_t a; int d; } kLevels[3] = {{kStrokeA2, 2}, {kStrokeA1, 1}, {255, 0}};
    if (n <= 0) return;
    // Rows of neighbouring rings that touch end to end (the +1 rings of
    // adjacent bottom labels) are merged into one rect; overlapping ones
    // are not (their overlap is blended twice, as with separate strokes).
    struct Row { int y, xa, xb; };
    Row rows[4 * 8];
    for (const auto& lv : kLevels) {
        g.alpha(lv.a);
        g.rectsBegin(c);
        for (int pass = 0; pass < (lv.d ? 2 : 1); ++pass) {
            const int d = pass ? -lv.d : lv.d;
            int nr = 0;
            for (int k = 0; k < n; ++k) {
                const int ax = box[k][0] - d, bx = box[k][2] + d, ay = box[k][1] - d, by = box[k][3] + d;
                if (box[k][2] < box[k][0] || box[k][3] < box[k][1] || bx < ax || by < ay) continue;
                if (n > 8) {   // no merging: plain rings
                    ringRects(g, box[k][0], box[k][1], box[k][2], box[k][3], d);
                    continue;
                }
                rows[nr++] = {ay, ax, bx};
                if (by > ay) rows[nr++] = {by, ax, bx};
                if (by - ay > 1) {
                    g.rectsAdd(ax, ay + 1, 1, by - ay - 1);
                    if (bx > ax) g.rectsAdd(bx, ay + 1, 1, by - ay - 1);
                }
            }
            for (int i = 0; i < nr; ++i) {
                if (rows[i].xb < rows[i].xa) continue;   // merged away
                for (int j = i + 1; j < nr; ++j)
                    if (rows[j].y == rows[i].y && rows[j].xa == rows[i].xb + 1) {
                        rows[i].xb = rows[j].xb;
                        rows[j].xb = rows[j].xa - 1;
                        j = i;   // look again from the new end
                    }
                g.rectsAdd(rows[i].xa, rows[i].y, rows[i].xb - rows[i].xa + 1, 1);
            }
        }
        g.rectsEnd();
    }
    g.alpha(255);
}

void stroke(Gfx& g, int x0, int y0, int x1, int y1, uint32_t c) {
    if (x1 < x0 || y1 < y0) return;
    g.alpha(kStrokeA2);
    g.rectsBegin(c);
    ringRects(g, x0, y0, x1, y1, 2);
    ringRects(g, x0, y0, x1, y1, -2);
    g.rectsEnd();
    g.alpha(kStrokeA1);
    g.rectsBegin(c);
    ringRects(g, x0, y0, x1, y1, 1);
    ringRects(g, x0, y0, x1, y1, -1);
    g.rectsEnd();
    g.alpha(255);
    g.rectsBegin(c);
    ringRects(g, x0, y0, x1, y1, 0);
    g.rectsEnd();
}

void dot(Gfx& g, int cx, int cy, float r, uint32_t c) { g.point(cx, cy, r, c); }

// ---------------------------------------------------------------------------
// Title bar

void formatStatusTime(char* out, int cap, uint32_t seconds) {
    std::snprintf(out, size_t(cap), "%um:%02us", unsigned(seconds / 60), unsigned(seconds % 60));
}

void formatStatusPosition(char* out, int cap, int pattern, int step, bool hex) {
    if (hex) std::snprintf(out, size_t(cap), "P%02X.%02X", unsigned(pattern & 0xFF), unsigned(step & 0xFF));
    else std::snprintf(out, size_t(cap), "P%d.%03d", pattern, step);
}

StatusLayout statusLayout(uint32_t seconds) {
    // spec 20 §1.2: "%3im:%02is %2s P%i.%03i%2s" from x 280 in font 4. The
    // leading pad spaces put the ink at 300 (set 0, 10-px space) and 288
    // (set 1, 4-px space) [Part 2 C10, §10].
    char t[24];
    std::snprintf(t, sizeof(t), "%3um:%02us", unsigned(seconds / 60), unsigned(seconds % 60));
    StatusLayout l{};
    const int sp = textWidth(kFont4, " ");
    const char sym[2] = {char(24), 0};
    l.symW = customFontsBound() ? textWidth(kFont4, sym) : 12;
    if (l.symW <= 0) l.symW = 12;
    int pad = 0;
    while (t[pad] == ' ') ++pad;
    if (!customFontsBound()) {
        // ROM fallback (no stock advances): the set-0 places, time
        // right-anchored at 360.
        l.symW = 12;
        l.timeX = kStatusTimeEnd - textWidth(kFont4, t + pad);
        l.iconX = kStatusTimeEnd + 10;
        l.posX = l.iconX + 2 * l.symW + 10;
        return l;
    }
    l.timeX = kStatusX + pad * sp;   // the "%3i" pad spaces are not drawn
    l.iconX = kStatusX + textWidth(kFont4, t) + sp;
    l.posX = l.iconX + 2 * l.symW + sp;
    return l;
}

// ---------------------------------------------------------------------------
// 2.x CPU cell [I] (spec 2x-02 §2.2)

CpuCell cpuCell(uint8_t mode, bool hasData, uint32_t peak, bool overrunRecent, bool cutRecent) {
    CpuCell c;
    if (mode >= 2 || !hasData) return c;
    c.overrun = overrunRecent;
    const uint32_t pct = (peak + 5) / 10;
    c.percent = uint8_t(pct > 99 ? 99 : pct);
    // Polyend's levels on the Tracker Mini 2.0 / Tracker+ (yellow 85 %, orange
    // 93 %, red 99 %; Polyend on backstage.polyend.com "CPU almost always in
    // red"), on the peak block; the governor cut and an overrun as before.
    if (peak >= 990 || overrunRecent) c.level = CpuLevel::kCritical;
    else if (peak >= 930 || cutRecent) c.level = CpuLevel::kAlarm;
    else if (peak >= 850) c.level = CpuLevel::kWarn;
    else c.level = mode == 1 ? CpuLevel::kNormal : CpuLevel::kHidden;
    if (c.level == CpuLevel::kHidden) c = CpuCell{};
    return c;
}

uint32_t cpuCellColour(const Theme& th, CpuLevel level) {
    switch (level) {
    case CpuLevel::kWarn: return th.slot(slot::kYellow);
    case CpuLevel::kAlarm: return th.slot(slot::kOrange);
    case CpuLevel::kCritical: return th.slot(slot::kSolidRect);
    default: return th.dim;
    }
}

int cpuCellRight(uint32_t seconds) { return statusLayout(seconds).timeX - kCpuCellGap; }

// Cell width: kCpuCellW, or wider when "CPU 00%" needs it in the bound font
// set (the custom font 4 is wider than the ROM font: the label was clamped
// into 60 px and ran into the value, "CPU29%", on the device; user photo).
int cpuCellWidth() {
    const int w = textWidth(kFont4, "CPU 00%");
    return w > kCpuCellW ? w : kCpuCellW;
}

int titleClipWidth(const CpuCell* cell) {
    if (!cell || !cell->shown()) return kTitleClipW;
    const int w = cell->right - cpuCellWidth() - kCpuTitleGap - kTitleTextX;
    return w < 0 ? 0 : (w > kTitleClipW ? kTitleClipW : w);
}

namespace {

// Pen x of the "CPU" label: right-aligned one space before a two-digit
// value, so it does not move with the value.
int cpuLabelX(const CpuCell& c) {
    const int x = c.right - textWidth(kFont4, "00%") - textWidth(kFont4, " ") - textWidth(kFont4, "CPU");
    return x < c.right - cpuCellWidth() ? c.right - cpuCellWidth() : x;
}
// Glyph-run top for text centred on the title row like the status group.
int titleGlyphY() { return kTitleCentreY - fontHeight(kFont4) / 2; }

void cpuLabel(Gfx& g, const Theme& th, const CpuCell& c) {
    if (!c.shown()) return;
    g.glyphRun(cpuLabelX(c), titleGlyphY(), kFont4, "CPU", cpuCellColour(th, c.level));
}

// The dynamic part: "nn%" right-aligned at the cell edge, or "!" after the
// label. One glyph run: colour, BEGIN, <= 3 vertices, END.
void cpuValue(Gfx& g, const Theme& th, const CpuCell& c) {
    if (!c.shown()) return;
    char v[4];
    int x;
    if (c.overrun) {
        v[0] = '!';
        v[1] = 0;
        x = cpuLabelX(c) + textWidth(kFont4, "CPU");
    } else {
        const unsigned p = c.percent > 99 ? 99u : unsigned(c.percent);
        int n = 0;
        if (p >= 10) v[n++] = char('0' + p / 10);
        v[n++] = char('0' + p % 10);
        v[n++] = '%';
        v[n] = 0;
        x = c.right - textWidth(kFont4, v);
    }
    g.glyphRun(x, titleGlyphY(), kFont4, v, cpuCellColour(th, c.level));
}

void titleBand(Gfx& g, const Theme& th, const char* title, int clipW = kTitleClipW) {
    const uint32_t bg = th.slot(slot::kTitle + 1);
    const uint32_t tc = th.slot(slot::kTitle);
    g.fill(0, 0, kScreenW, kTitleBandH, bg);
    if (title && *title) {
        g.clip(kTitleTextX, 0, clipW, kHairlineY - 1);
        g.text(kTitleTextX, kTitleCentreY, kFont4, tc, title, IDisplay::kOptCenterY);
        g.noClip();
    }
}

void titleRight(Gfx& g, const Theme& th, const char* right, bool status) {
    if (!right || !*right) return;
    const uint32_t tc = th.slot(slot::kTitle);
    char buf[64];
    copyFit(buf, sizeof(buf), right);
    const int minX = status ? kRightTextMinX : kTitleTextX + kTitleClipW + 10;
    int n = int(std::strlen(buf));
    while (n > 0 && textWidth(kFont4, buf, n) > kRightTextX - minX) --n;
    buf[n] = 0;
    g.clip(minX, 0, kRightTextX - minX + 1, kHairlineY - 1);
    g.text(kRightTextX, kTitleCentreY, kFont4, tc, buf, IDisplay::kOptRightX | IDisplay::kOptCenterY);
    g.noClip();
}

}  // namespace

void titleBarStatus(Gfx& g, const Theme& th, const TitleStatus& s) {
    const TitleStatus* st = &s;
    const uint32_t sc = th.slot(slot::kStatus);
    char buf[24];
    formatStatusTime(buf, sizeof(buf), st->seconds);
    const StatusLayout l = statusLayout(st->seconds);
    g.text(l.timeX, kTitleCentreY, kFont4, sc, buf, IDisplay::kOptCenterY);
    // Transport glyph pair (2 symbol cells): two 3 x 12 bars when
    // stopped/paused, at the right edge of each cell less 6 / 1 px
    // (set 0: 376 / 381, set 1: 364 / 369); a play glyph otherwise.
    if (st->transport == Transport::kStopped) {
        g.rectsBegin(sc);
        g.rectsAdd(l.iconX + l.symW - 6, kTitleCentreY - 5, 3, 12);
        g.rectsAdd(l.iconX + l.symW - 1, kTitleCentreY - 5, 3, 12);
        g.rectsEnd();
    } else {
        // The play triangle; the loop arrow while Shift+Play loops a pattern
        // inside the song (stock photos: plain pattern play shows the triangle).
        g.icon(l.iconX + l.symW - 6, kTitleCentreY - 5, st->transport == Transport::kLoopingInSong ? kIconLoop : kIconPlay,
               sc);
    }
    formatStatusPosition(buf, sizeof(buf), st->pattern, st->step, st->hex);
    g.text(l.posX, kTitleCentreY, kFont4, sc, buf, IDisplay::kOptCenterY);
    if (st->recording) {
        const int rx = l.posX + textWidth(kFont4, buf);
        g.icon(rx + l.symW - 6, kTitleCentreY - 5, kIconRec, th.slot(slot::kSolidRect));
    }
    cpuValue(g, th, st->cpu);
}

void titleBar(Gfx& g, const Theme& th, const char* title, const char* right, const TitleStatus* st) {
    titleBand(g, th, title, titleClipWidth(st ? &st->cpu : nullptr));
    if (st) cpuLabel(g, th, st->cpu);
    if (st) titleBarStatus(g, th, *st);
    titleRight(g, th, right, st != nullptr);
}

void titleBarChrome(Gfx& g, const Theme& th, const char* title, const char* right, bool status,
                    const CpuCell* cpu) {
    if (!status) cpu = nullptr;
    titleBand(g, th, title, titleClipWidth(cpu));
    if (cpu) cpuLabel(g, th, *cpu);
    titleRight(g, th, right, status);
}

void titleHairline(Gfx& g, const Theme& th) { titleHairline(g, th, 0, kScreenW - 1); }

void titleHairline(Gfx& g, const Theme& th, int x0, int x1, uint32_t colour) {
    if (x1 < x0) return;
    const int w = x1 - x0 + 1;
    // The band lies under rows 28 / 29 too (Part 2 C3: row 29 = 1d1d1d on
    // every Original screen, i.e. the halo over 080808, not over a panel).
    g.fill(x0, kHairlineY, w, kTitleBandH - kHairlineY, th.slot(slot::kTitle + 1));
    const uint32_t c = pick(colour, th.slot(slot::kTitleHairline));
    g.alpha(kTitleHaloA);
    g.rectsBegin(c);
    g.rectsAdd(x0, kHairlineY - 1, w, 1);
    g.rectsAdd(x0, kHairlineY + 1, w, 1);
    g.rectsEnd();
    g.alpha(kHairCoreA);
    g.fill(x0, kHairlineY, w, 1, c);
    g.alpha(255);
}

// ---------------------------------------------------------------------------
// Popup headers

void popupHeader(Gfx& g, const Theme& th, const char* title, int x0, int x1, int textX) {
    if (x1 < x0) return;
    g.fill(x0, 0, x1 - x0 + 1, kHairlineY - 1, th.slot(slot::kKeyboardLabels + 1));
    if (title && *title) {
        g.clip(x0, 0, x1 - x0 + 1, kHairlineY - 1);
        g.text(textX, kTitleCentreY, kFont4, th.slot(slot::kKeyboardLabels), title, IDisplay::kOptCenterY);
        g.noClip();
    }
}

void rightPopupHeader(Gfx& g, const Theme& th, const char* title) {
    const int x0 = kRightPopupX, w = kScreenW - x0;
    popupHeader(g, th, title, x0, kScreenW - 1, x0 + 21);
    // Rows 27 / 28: 868686 / 525252 on Original (the band's edge over the
    // title band), i.e. the band colour at 52 % / 30 %.
    const uint32_t band = th.slot(slot::kKeyboardLabels + 1), tb = th.slot(slot::kTitle + 1);
    g.fill(x0, kHairlineY - 1, w, 1, mix(tb, band, 133));
    g.fill(x0, kHairlineY, w, 1, mix(tb, band, 77));
}

void rightPopupPanel(Gfx& g, const Theme& th, int y) {
    g.fill(kRightPopupX + 1, y, kScreenW - kRightPopupX - 1, kScreenH - y, th.slot(slot::kEmptyPanel + 1));
}

void fullPopupHeader(Gfx& g, const Theme& th, const char* title) {
    const uint32_t band = th.slot(slot::kKeyboardLabels + 1);
    g.fill(0, 0, kScreenW, kHairlineY, th.bg);
    popupHeader(g, th, title, 2, kScreenW - 3, 12);
    blendFill(g, 1, 0, 1, kHairlineY - 1, band, kEdgeA);
    blendFill(g, kScreenW - 2, 0, 1, kHairlineY - 1, band, kEdgeA);
    g.fill(2, kHairlineY - 1, kScreenW - 4, 1, mix(th.bg, band, 181));   // b5b5b5
}

// ---------------------------------------------------------------------------
// Bottom bar

uint32_t stripColour(const Theme& th, StripStyle style) {
    // Dark: 050505 on Original (0a0a0a halved), the strip elsewhere.
    return style == StripStyle::kDark ? th.darkStrip() : th.slot(slot::kStrip + 1);
}

void bottomStrip(Gfx& g, const Theme& th, uint8_t separators, uint32_t bg) {
    const uint32_t c = pick(bg, th.slot(slot::kStrip + 1));
    // RECTS edge: the outermost row / columns are not fully covered.
    g.alpha(230);
    g.rectsBegin(c);
    g.rectsAdd(1, kStripY - 1, kScreenW - 2, 1);
    g.rectsAdd(0, kStripY, 1, kStripH);
    g.rectsAdd(kScreenW - 1, kStripY, 1, kStripH);
    g.rectsEnd();
    g.alpha(150);
    g.rectsBegin(c);
    g.rectsAdd(0, kStripY - 1, 1, 1);
    g.rectsAdd(kScreenW - 1, kStripY - 1, 1, 1);
    g.rectsEnd();
    g.alpha(255);
    g.fill(1, kStripY, kScreenW - 2, kStripH, c);
    if (!(separators & 0xFE) || !th.barLines()) return;
    const uint32_t sc = th.slot(slot::kStrip);
    const int y1 = kStripY + kStripH - 1;
    g.alpha(kHairHaloA);
    g.rectsBegin(sc);
    for (int i = 1; i < 8; ++i) {
        if (!((separators >> i) & 1)) continue;
        const int x = 100 * i;
        g.rectsAdd(x - 1, kSeparatorTop, 1, y1 - kSeparatorTop + 1);
        g.rectsAdd(x + 1, kSeparatorTop, 1, y1 - kSeparatorTop + 1);
        g.rectsAdd(x, kSeparatorTop - 1, 1, 1);
    }
    g.rectsEnd();
    g.alpha(kHairCoreA);
    g.rectsBegin(sc);
    for (int i = 1; i < 8; ++i)
        if ((separators >> i) & 1) g.rectsAdd(100 * i, kSeparatorTop, 1, y1 - kSeparatorTop + 1);
    g.rectsEnd();
    g.alpha(255);
}

void barUnderline(Gfx& g, const Theme& th, int cx, uint8_t kind) {
    if (kind == kUnderlineNone || !th.barLines()) return;   // Anniversary: none (C6)
    const uint32_t c = th.slot(kind == kUnderlineGreen ? slot::kGreen : slot::kSolidRect);
    hairH(g, cx - kUnderlineHalf, cx + kUnderlineHalf, kUnderlineY, c);
}

void pageDots(Gfx& g, const Theme& th, int cx, int pages, int page, uint32_t off) {
    if (pages < 2) return;
    // 13-px pitch: 737 / 750 / 763 (001, 012), 744 / 757 (011) for cx 750.
    const int x0 = cx - (pages - 1) * kDotPitch / 2;
    const uint32_t oc = pick(off, th.slot(slot::kBottomLabel + 17));
    for (int p = 0; p < pages; ++p)
        dot(g, x0 + p * kDotPitch, kDotY, 3.0f, p == page ? th.slot(slot::kBottomLabel) : oc);
}

void barFocus(Gfx& g, const Theme& th, int i, bool wide) {
    focusFrame(g, th, 1 + 100 * i, kStripY - 1, 98 + 100 * i + (wide ? 100 : 0), kScreenH - 1);
}

void barFocusMask(Gfx& g, const Theme& th, uint8_t mask, uint8_t wideMask) {
    int box[8][4], n = 0;
    for (int i = 0; i < 8; ++i)
        if (mask & (1u << i)) {
            box[n][0] = 1 + 100 * i;
            box[n][1] = kStripY - 1;
            box[n][2] = 98 + 100 * i + ((wideMask >> i) & 1 ? 100 : 0);
            box[n][3] = kScreenH - 1;
            ++n;
        }
    strokes(g, box, n, th.slot(slot::kFocusFrame));
}

void barFill(Gfx& g, const Theme& th, int i, bool wide) {
    const uint32_t c = th.slot(slot::kFocusFrame);
    const int x = 3 + 100 * i, w = 95 + (wide ? 100 : 0), y = kStripY - 1, h = kScreenH - y;
    g.alpha(kEdgeA);
    g.rectsBegin(c);
    g.rectsAdd(x - 1, y, 1, h);
    g.rectsAdd(x + w, y, 1, h);
    g.rectsAdd(x, y - 1, w, 1);
    g.rectsEnd();
    g.alpha(255);
    g.fill(x, y, w, h, c);
}

void barLabel(Gfx& g, const Theme& th, int i, bool wide, const char* top, const char* bottom,
              const BarLabelStyle& st) {
    if (i < 0 || i > 7) return;
    const int cx = labelCentre(i, wide);
    if (st.fill) barFill(g, th, i, wide);
    const int set = st.enabled ? slot::kBottomLabel : slot::kBottomLabelOff;
    uint32_t tc = th.slot(set);
    const uint32_t vc = th.slot(set + 3);
    // A focused label keeps its grey title (006 "Start", 005, 011, 022, 025:
    // no white title in any stock shot); brightTitle is an explicit opt-in.
    if (st.brightTitle && st.enabled) tc = vc;
    const bool two = bottom && *bottom;
    const int y1 = two || st.pages > 1 ? kLabelY1 : (st.topAligned ? kLabelYTop : kLabelY);
    if (top && *top) g.text(cx, y1, kFont3, tc, top, IDisplay::kOptCenter);
    if (two) {
        // Set +0x44 names ("Export / Song") stay grey in font 3; a value that
        // does not fit the label in the 10-px mono uses font 3 too.
        const int maxW = wide ? 194 : 94;
        const uint8_t f = st.nameLines || textWidth(kFont2, bottom) > maxW ? kFont3 : kFont2;
        g.text(cx, kLabelY2, f, st.nameLines ? tc : vc, bottom, IDisplay::kOptCenter);
    }
    else pageDots(g, th, cx, st.pages, st.page, st.dotOff);
    barUnderline(g, th, cx, st.underline);
    if (st.focus) barFocus(g, th, i, wide);
}

// ---------------------------------------------------------------------------
// Panels

void bottomFade(Gfx& g, const Theme& th, int x, int y, int w, int h, uint32_t bg) {
    if (!th.flag(slot::kFlagBottomFade) || w <= 0 || h < kFadeH) return;
    if (g.lowDetail()) return;   // RAM_DL fallback: no gradients
    const int fy = y + h - kFadeH;
    if (!g.saveContext(7)) return;   // scissor 2 + CMD_GRADIENT 5
    g.clip(x, fy, w + 1, kFadeH + 2);
    g.gradient(x, fy, x, y + h + 2, bg, th.bg);   // stock: 000000 (Original's background)
    g.restoreContext();
}

void panel(Gfx& g, const Theme& th, int x, int y, int w, int h, uint32_t bg, bool fade) {
    const uint32_t c = pick(bg, th.slot(slot::kEmptyPanel + 1));
    g.alpha(kEdgeA);
    g.rectsBegin(c);
    g.rectsAdd(x - 1, y, 1, h + 1);
    g.rectsAdd(x + w + 1, y, 1, h + 1);
    g.rectsEnd();
    g.alpha(255);
    g.fill(x, y, w + 1, h + 1, c);
    if (fade) bottomFade(g, th, x, y, w, h, c);
}

void focusFrame(Gfx& g, const Theme& th, int x0, int y0, int x1, int y1, uint32_t fill) {
    if (fill != kThemeColour) g.fill(x0 + 1, y0 + 1, x1 - x0 - 1, y1 - y0 - 1, fill);
    stroke(g, x0, y0, x1, y1, th.slot(slot::kFocusFrame));
}

// ---------------------------------------------------------------------------
// List

int listFollow(int sel, int count, int visible, int top) {
    if (visible <= 0 || count <= visible) return 0;
    if (sel < 0) sel = 0;
    const int margin = visible > 2 ? 1 : 0;
    if (sel - margin < top) top = sel - margin;
    if (sel + margin >= top + visible) top = sel + margin - visible + 1;
    if (top > count - visible) top = count - visible;
    if (top < 0) top = 0;
    return top;
}

// --- list marquee state (POD, zero-initialised: no static constructor) ----
namespace {
struct MarqueeSlot {
    int16_t x, y;
    int32_t sel;
    uint32_t hash;
    uint32_t start;   // ms
    uint32_t frame;   // marquee frame it was last drawn in (0 = free)
};
constexpr int kMarqueeSlots = 4;
MarqueeSlot gMq[kMarqueeSlots];
uint32_t gMqNow = 0, gMqFrame = 0;
bool gMqRun = false;

uint32_t textHash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ uint8_t(*s)) * 16777619u;
    return h;
}

// Ticks the selected item of the list at (x, y) has been scrolling.
uint32_t marqueeTicks(int x, int y, int sel, const char* text) {
    const uint32_t h = textHash(text);
    MarqueeSlot* m = nullptr;
    for (auto& s : gMq)
        if (s.frame && s.x == x && s.y == y) m = &s;
    if (!m) {
        m = &gMq[0];
        for (auto& s : gMq)
            if (s.frame < m->frame) m = &s;   // free or least recently drawn
        m->frame = 0;
    }
    const bool fresh = !m->frame || m->frame + 1 < gMqFrame || m->sel != sel || m->hash != h;
    if (fresh) {
        m->x = int16_t(x);
        m->y = int16_t(y);
        m->sel = sel;
        m->hash = h;
        m->start = gMqNow;
    }
    m->frame = gMqFrame ? gMqFrame : 1;
    gMqRun = true;
    return (gMqNow - m->start) / kMarqueeTickMs;
}

// Draws the glyphs of `s` (pen origin x0) that overlap left .. right - 1
// as one text run; glyphs outside cost no RAM_DL words.
void textWindow(Gfx& g, uint8_t font, uint32_t c, char* s, int x0, int y, int left, int right) {
    int i = 0, pen = x0;
    while (s[i]) {
        const int w = textWidth(font, s + i, 1);
        if (pen + w > left) break;
        pen += w;
        ++i;
    }
    if (!s[i] || pen >= right) return;
    int j = i, p = pen;
    while (s[j] && p < right) p += textWidth(font, s + j++, 1);
    const char keep = s[j];
    s[j] = 0;
    g.text(pen, y, font, c, s + i, IDisplay::kOptCenterY);
    s[j] = keep;
}
}  // namespace

int marqueeOffset(uint32_t ticks) {
    const int t = ticks > 1000000u ? 1000000 : int(ticks);
    return t <= 10 ? t * t / 30 : (20 * t - 100) / 30;
}

void marqueeFrame(uint32_t nowMs) {
    gMqNow = nowMs;
    ++gMqFrame;
    if (!gMqFrame) gMqFrame = 1;
    gMqRun = false;
}

bool marqueeRunning() { return gMqRun; }

void list(Gfx& g, const Theme& th, const ListView& v) {
    const int s = v.set;
    if (v.background) panel(g, th, v.x, v.y, v.w, v.h, th.slot(s + 3));
    const int visible = listVisibleRows(v.h);
    const bool scroll = v.count > visible;
    const int innerW = v.w - (scroll ? 13 : 6);
    char buf[kListItemCap];
    if (v.item) {
        g.clip(v.x + 3, v.y, innerW, v.h);
        // Text from x + 13; the column ends at the clip's right edge.
        const int left = v.x + 3, colRight = v.x + 3 + innerW;
        for (int r = 0; r < visible; ++r) {
            const int idx = v.top + r;
            if (idx >= v.count) break;
            buf[0] = 0;
            v.item(v.ctx, idx, buf, sizeof(buf));
            buf[sizeof(buf) - 1] = 0;
            const bool marked = v.marked && v.marked(v.ctx, idx);
            const uint32_t tc = v.enabled ? th.slot(s + (marked ? 5 : 4)) : th.slot(slot::kBottomLabelOff);
            const int ty = v.y + 17 + kListRowH * r;
            // A tag takes the row's right end; the name column shrinks by it.
            const char* tag = v.tag ? v.tag(v.ctx, idx) : nullptr;
            int right = colRight, tagW = 0;
            if (tag && tag[0]) {
                tagW = textWidth(v.font, tag) + 8;
                right -= tagW;
                g.text(colRight - 4, ty, v.font, tc, tag, IDisplay::kOptRightX | IDisplay::kOptCenterY);
            }
            const int tw = textWidth(v.font, buf);
            if (idx == v.sel && v.selStyle != ListSel::kNone && v.x + 13 + tw > right) {
                // Marquee (spec 20 §1.6): the text and its repeat after a
                // 30-px gap, each cut to the column.
                const int period = tw + kMarqueeGap;
                const int off = marqueeOffset(marqueeTicks(v.x, v.y, idx, buf)) % period;
                textWindow(g, v.font, tc, buf, v.x + 13 - off, ty, left, right);
                textWindow(g, v.font, tc, buf, v.x + 13 - off + period, ty, left, right);
                continue;
            }
            // Glyphs that start right of the clip are invisible but still
            // cost RAM_DL words (31-char names: VERIFY_1.9.5 R4): cut them.
            const int room = innerW - 10 - tagW;
            int n = 0, pen = 0;
            while (buf[n] && pen < room) pen += textWidth(v.font, buf + n++, 1);
            buf[n] = 0;
            g.text(v.x + 13, ty, v.font, tc, buf, IDisplay::kOptCenterY);
        }
        g.noClip();
    }
    if (v.sel >= v.top && v.sel < v.top + visible && v.sel < v.count && v.selStyle != ListSel::kNone) {
        uint32_t c = th.slot(s + (v.active ? 1 : 0));
        if (v.selStyle == ListSel::kWhite) c = th.slot(slot::kList);
        else if (v.selStyle == ListSel::kRed) c = th.slot(slot::kFocusFrame);
        else if (v.selStyle == ListSel::kGreen) c = th.slot(slot::kGreen);
        if (!v.enabled) c = mix(th.slot(s + 3), c, kEdgeA);   // 7e7e7e on Monochrome [S round 3, 007]
        const int rt = v.y + 4 + kListRowH * (v.sel - v.top);
        stroke(g, v.x + 3, rt, v.x + 3 + innerW, rt + kListRowH, c);
    }
    if (scroll) {
        const int tx = v.x + v.w - 6, ty = v.y + 8, th14 = v.h - 14;
        // Track with soft 1-px side edges (5b5b5b on Monochrome, round-3
        // Snapshot_007; 050505 on Original).
        g.alpha(kEdgeA);
        g.rectsBegin(th.slot(s + 6));
        g.rectsAdd(tx - 1, ty, 1, th14);
        g.rectsAdd(tx + 4, ty, 1, th14);
        g.rectsEnd();
        g.alpha(255);
        g.fill(tx, ty, 4, th14, th.slot(s + 6));
        // Thumb travel inset 2 px at both ends of the track (s07 / s09).
        const int travel = th14 - 4;
        int len = travel * visible / v.count;
        if (len < 4) len = 4;
        const int sel = v.sel < 0 ? v.top : v.sel;
        const int pos = 2 + (travel - len) * sel / v.count;
        const uint32_t tc = th.slot(s + 2);
        g.alpha(kHairHaloA);
        g.rectsBegin(tc);
        g.rectsAdd(tx, ty + pos, 1, len);
        g.rectsAdd(tx + 3, ty + pos, 1, len);
        g.rectsEnd();
        g.alpha(kHairCoreA);
        g.fill(tx + 1, ty + pos, 2, len, tc);
        g.alpha(255);
    }
}

// ---------------------------------------------------------------------------
// Value bar

void valueBar(Gfx& g, const Theme& th, int x, int y, int w, int h, float value, const ValueBarStyle& st) {
    const int s = st.set;
    if (st.background) panel(g, th, x, y, w, h, th.slot(s + 4));
    const uint32_t track = th.slot(s + 3);
    const int tx = x + 10, ty = y + 11, tw = w - 19, tH = h - 24;
    if (tw <= 2 || tH <= 2) return;
    // Track with soft 1-px edges (the 090909 "well" edge on 0a0a0a, Part 2 §7.4).
    g.alpha(kEdgeA);
    g.rectsBegin(track);
    g.rectsAdd(tx - 1, ty, 1, tH);
    g.rectsAdd(tx + tw, ty, 1, tH);
    g.rectsAdd(tx, ty - 1, tw, 1);
    g.rectsAdd(tx, ty + tH, tw, 1);
    g.rectsEnd();
    g.alpha(255);
    g.fill(tx, ty, tw, tH, track);
    if (th.flag(slot::kFlagValueBarGrad)) g.gradientRect(tx, ty, 9 < tw ? 9 : tw, tH, th.bg, track, false);
    const uint32_t fc = pick(st.fill, th.slot(s));
    const int fx = x + 11, fw = w - 21, base = y + h - 15, span = h - 24;
    auto band = [&](int y0, int y1, uint32_t c) {   // solid rows y0..y1 with soft edges
        if (y1 < y0) return;
        g.alpha(kEdgeA);
        g.rectsBegin(c);
        g.rectsAdd(fx - 1, y0, 1, y1 - y0 + 1);
        g.rectsAdd(fx + fw, y0, 1, y1 - y0 + 1);
        g.rectsAdd(fx, y0 - 1, fw, 1);
        g.rectsAdd(fx, y1 + 1, fw, 1);
        g.rectsEnd();
        g.alpha(255);
        g.fill(fx, y0, fw, y1 - y0 + 1, c);
    };
    if (st.bipolar) {
        if (value < -1) value = -1;
        if (value > 1) value = 1;
        const int mid = y + 11 + (tH - 2) / 2;
        const int yv = mid - int(value * float(span) / 2.0f + (value >= 0 ? 0.5f : -0.5f));
        band(yv, yv, fc);
        return;
    }
    if (value < 0) value = 0;
    if (value > 1) value = 1;
    // Stock rounds the fill up (ceil over a span of h - 24 rows).
    const int n = int(fm::ceilD(double(span) * double(value) - 1e-4));
    if (st.ref >= 0 && n > 0) {
        const int nr = int(fm::ceilD(double(span) * double(st.ref > 1 ? 1.0f : st.ref) - 1e-4));
        const int lo = n < nr ? n : nr;
        if (lo > 0) band(base - lo + 1, base, fc);
        if (n != nr) {
            const int hi = n < nr ? nr : n;
            band(base - hi + 1, base - lo, th.slot(s + (n < nr ? 1 : 2)));
        }
        return;
    }
    // Value 0 still shows the 1.5-px line on the base row (Part 2 §7.4).
    band(base - (n > 0 ? n : 1) + 1, base, fc);
}

void valueBars(Gfx& g, const Theme& th, const ValueBarItem* bars, int n) {
    const int s = slot::kValueBar;
    const uint32_t track = th.slot(s + 3);
    auto shown = [](const ValueBarItem& b) { return b.w - 19 > 2 && b.h - 24 > 2; };
    // Wells: soft edges, then the wells, then the inner gradients (valueBar order).
    g.alpha(kEdgeA);
    g.rectsBegin(track);
    for (int i = 0; i < n; ++i) {
        const ValueBarItem& b = bars[i];
        if (!shown(b)) continue;
        const int tx = b.x + 10, ty = b.y + 11, tw = b.w - 19, tH = b.h - 24;
        g.rectsAdd(tx - 1, ty, 1, tH);
        g.rectsAdd(tx + tw, ty, 1, tH);
        g.rectsAdd(tx, ty - 1, tw, 1);
        g.rectsAdd(tx, ty + tH, tw, 1);
    }
    g.rectsEnd();
    g.alpha(255);
    g.rectsBegin(track);
    for (int i = 0; i < n; ++i)
        if (shown(bars[i])) g.rectsAdd(bars[i].x + 10, bars[i].y + 11, bars[i].w - 19, bars[i].h - 24);
    g.rectsEnd();
    if (th.flag(slot::kFlagValueBarGrad))
        for (int i = 0; i < n; ++i) {
            const ValueBarItem& b = bars[i];
            if (shown(b)) g.gradientRect(b.x + 10, b.y + 11, 9 < b.w - 19 ? 9 : b.w - 19, b.h - 24, th.bg, track, false);
        }
    // Fills (rows y0..y1 with soft edges), one edge batch and one fill batch
    // per colour.
    auto rows = [](const ValueBarItem& b, int& y0, int& y1) {
        float v = b.value < 0 ? 0 : (b.value > 1 ? 1 : b.value);
        const int span = b.h - 24, base = b.y + b.h - 15;
        const int k = int(fm::ceilD(double(span) * double(v) - 1e-4));
        y0 = base - (k > 0 ? k : 1) + 1;
        y1 = base;
    };
    uint32_t colours[16];
    int nc = 0;
    for (int i = 0; i < n; ++i) {
        if (!shown(bars[i])) continue;
        const uint32_t c = pick(bars[i].fill, th.slot(s));
        int j = 0;
        while (j < nc && colours[j] != c) ++j;
        if (j == nc && nc < 16) colours[nc++] = c;
    }
    for (int edges = 1; edges >= 0; --edges) {
        g.alpha(edges ? kEdgeA : 255);
        for (int j = 0; j < nc; ++j) {
            g.rectsBegin(colours[j]);
            for (int i = 0; i < n; ++i) {
                const ValueBarItem& b = bars[i];
                if (!shown(b) || pick(b.fill, th.slot(s)) != colours[j]) continue;
                int y0, y1;
                rows(b, y0, y1);
                const int fx = b.x + 11, fw = b.w - 21;
                if (edges) {
                    g.rectsAdd(fx - 1, y0, 1, y1 - y0 + 1);
                    g.rectsAdd(fx + fw, y0, 1, y1 - y0 + 1);
                    g.rectsAdd(fx, y0 - 1, fw, 1);
                    g.rectsAdd(fx, y1 + 1, fw, 1);
                } else {
                    g.rectsAdd(fx, y0, fw, y1 - y0 + 1);
                }
            }
            g.rectsEnd();
        }
    }
    g.alpha(255);
}

// ---------------------------------------------------------------------------
// Popups and text

void popupBox(Gfx& g, const Theme& th, int x, int y, int w, int h) {
    g.fill(x, y, w, h, th.slot(slot::kProgress + 1));
    g.fill(x + 1, y + 1, w - 2, h - 2, th.slot(slot::kProgress));
}

void progressPopup(Gfx& g, const Theme& th, const char* title, int percent, int y, int h) {
    const int x = kProgressX, w = kProgressW;
    popupBox(g, th, x, y, w, h);
    if (title && *title) g.text(x + w / 2, y + h / 3, kFont3, th.slot(slot::kProgress + 2), title, IDisplay::kOptCenter);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const int bx = x + w / 7, by = y + 3 * h / 5, bw = 5 * w / 7;
    g.fill(bx, by, bw, 2, th.slot(slot::kProgress + 3));
    g.fill(bx, by, bw * percent / 100, 2, th.slot(slot::kProgress + 2));
}

void bootSplash(Gfx& g, const Theme& th, const char* title, int percent, const char* version) {
    const uint32_t tc = th.slot(slot::kEmptyPanel);   // ffffff in all three themes
    g.fill(0, 0, kScreenW, kScreenH, th.bg);
    // Font 1 cell centred on 308: caps 303..313 (set 0).
    if (title && *title) g.text(kScreenW / 2, 308, kFont1, tc, title, IDisplay::kOptCenter);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const int bx = kProgressX + kProgressW / 7, bw = 5 * kProgressW / 7, len = bw * percent / 100;
    if (len > 0) {
        // 2-px bar, core x bx + 1 .. bx + len, rows 331 / 332, AA ring (5d5d5d
        // on 4b4b4b for c9c9ca: alpha 36).
        const uint32_t bc = th.slot(slot::kProgress + 2);
        g.alpha(36);
        g.rectsBegin(bc);
        g.rectsAdd(bx + 1, 330, len, 1);
        g.rectsAdd(bx + 1, 333, len, 1);
        g.rectsAdd(bx, 331, 1, 2);
        g.rectsAdd(bx + len + 1, 331, 1, 2);
        g.rectsEnd();
        g.alpha(255);
        g.fill(bx + 1, 331, len, 2, bc);
    }
    if (version && *version) g.text(11, 466, kFont1, tc, version, IDisplay::kOptCenterY);
}

void messagePopup(Gfx& g, const Theme& th, const char* title, const char* text, int y, int h) {
    const int x = kProgressX, w = kProgressW;
    popupBox(g, th, x, y, w, h);
    const uint32_t c = th.slot(slot::kProgress + 2);
    if (title && *title) g.text(x + w / 2, y + h / 3, kFont4, c, title, IDisplay::kOptCenter);
    if (text && *text) g.text(x + w / 2, y + 2 * h / 3, kFont3, c, text, IDisplay::kOptCenter);
}

int textBox(Gfx& g, const Theme& th, int x, int y, int w, int h, uint8_t font, const char* text, int extraPitch,
            uint32_t colour, bool background) {
    const int set = slot::kTextBox;
    if (background) g.fill(x, y, w, h, th.slot(set + 1));
    if (!text) return 0;
    const uint32_t c = pick(colour, th.slot(set));
    const int pitch = fontHeight(font) + extraPitch, maxW = w - 10;
    int ly = y + 5, lines = 0;
    char line[128];
    const char* t = text;
    while (*t && ly + pitch <= y + h + extraPitch) {
        // Longest prefix ending at a word boundary (or a forced cut) that fits.
        int n = 0, lastSpace = -1;
        while (t[n] && t[n] != '\n' && n < int(sizeof(line)) - 1) {
            if (textWidth(font, t, n + 1) > maxW) break;
            if (t[n] == ' ') lastSpace = n;
            ++n;
        }
        int take = n;
        if (t[n] && t[n] != '\n' && lastSpace > 0) take = lastSpace;
        if (take == 0) take = 1;
        std::memcpy(line, t, size_t(take));
        line[take] = 0;
        g.text(x + 5, ly, font, c, line);
        ++lines;
        ly += pitch;
        t += take;
        if (*t == '\n') ++t;
        while (*t == ' ') ++t;
    }
    return lines;
}

void nameField(Gfx& g, const Theme& th, int x, int y, int w, int h, const char* text, int cursor, bool selAll,
               bool focused, int cellW) {
    (void)cellW;
    const uint32_t outer = th.slot(slot::kEmptyPanel + 1);
    const uint32_t inner = th.slot(slot::kTitle + 1);
    const uint32_t red = th.slot(slot::kFocusFrame);
    // List text on a dark box; on Monochrome's light title-band box the
    // light list grey vanishes, so the name takes the title text (black).
    const uint32_t lum = ((inner >> 16 & 0xFF) * 3 + (inner >> 8 & 0xFF) * 6 + (inner & 0xFF)) / 10;
    const uint32_t tc = lum > 128 ? th.slot(slot::kTitle) : th.slot(slot::kList + 4);
    g.fill(x, y, w, h, outer);
    // Inner box inset 10 px; its RECTS edge blends with the outer fill
    // (090909 between 0a0a0a and 080808).
    const int ix = x + 10, iy = y + 10, iw = w - 20, ih = h - 20;
    blendFill(g, ix, iy, iw, ih, inner, 128);
    g.fill(ix + 1, iy + 1, iw - 2, ih - 2, inner);
    if (focused) stroke(g, x, y, x + w - 1, y + h - 1, red);
    const int cx = x + w / 2, cy = y + (h - 1) / 2;
    const int len = text ? int(std::strlen(text)) : 0;
    if (cursor < 0) cursor = 0;
    if (cursor > len) cursor = len;
    const int tw = len ? textWidth(kFont2, text) : 0;
    const int tx = cx - tw / 2;
    if (selAll && len) g.fill(tx - 2, cy - 13, tw + 4, 27, th.slot(slot::kBottomLabel + 1));
    if (len) g.text(tx, cy, kFont2, tc, text, IDisplay::kOptCenterY);
    // 1.5-px text cursor: core column + two soft columns, rows cy - 12 .. cy + 12.
    const int kx = tx + (cursor ? textWidth(kFont2, text, cursor) : 0);
    const uint32_t kc = focused ? red : tc;
    blendFill(g, kx - 1, cy - 12, 1, 25, kc, kEdgeA);
    blendFill(g, kx + 1, cy - 12, 1, 25, kc, kEdgeA);
    g.fill(kx, cy - 12, 1, 25, kc);
}

void key(Gfx& g, const Theme& th, int x, int y, int w, int h, const char* label, bool selected, uint32_t textColour,
         uint32_t selColour) {
    const int s = slot::kKeyboardKeys;
    g.fill(x + 1, y + 1, w - 2, h - 2, th.slot(s));
    if (label && *label) g.text(x + w / 2, y + h / 2, kFont3, pick(textColour, th.slot(s + 1)), label,
                                IDisplay::kOptCenter);
    if (selected) stroke(g, x + 1, y + 1, x + w - 2, y + h - 2, pick(selColour, th.slot(s + 2)));
}

void keyboardKey(Gfx& g, const Theme& th, int x, int y, int w, int h, const char* label, bool selected, bool home,
                 uint32_t textColour) {
    const int s = slot::kKeyboardKeys;
    const uint32_t fc = home ? th.slot(s + 1) : th.slot(s);
    // Soft 1-px edge outside the box (090909 on black; e2e2e2 for a white key).
    g.alpha(230);
    g.rectsBegin(fc);
    g.rectsAdd(x - 1, y, 1, h);
    g.rectsAdd(x + w, y, 1, h);
    g.rectsAdd(x, y - 1, w, 1);
    g.rectsAdd(x, y + h, w, 1);
    g.rectsEnd();
    g.alpha(255);
    g.fill(x, y, w, h, fc);
    const uint32_t lc = home ? th.slot(slot::kNameHeader + 3) : pick(textColour, th.slot(s + 1));
    if (label && *label) g.text(x + (w - 1) / 2, y + (h - 1) / 2, kFont2, lc, label, IDisplay::kOptCenter);
    if (selected) stroke(g, x, y, x + w - 1, y + h - 1, th.slot(s + 2));
}

void pickerCell(Gfx& g, const Theme& th, int x, int y, int w, int h, bool selected) {
    g.fill(x + 1, y + 1, w - 2, h - 2, th.slot(slot::kKeyboardKeys));
    if (selected) stroke(g, x + 2, y + 2, x + w - 3, y + h - 3, th.slot(slot::kFocusFrame));
}

}  // namespace wg
}  // namespace ot
