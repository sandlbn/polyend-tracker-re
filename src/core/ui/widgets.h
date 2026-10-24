// Shared stock-style UI chrome (spec 20 §1.2, §1.5, §3; docs/VISUAL_DIFF_1.9.4.md §2).
//
// Every widget draws its own primitives (fills, alpha fills, gradients,
// points); no stock bitmaps are used. Geometry and colours follow the
// measured stock frame:
//
//   title bar   (0, 0, 800, 30) slot kTitle+1, hairline rows 27/28/29 (the
//               band lies under all three rows, VISUAL_DIFF Part 2 C3)
//   main area   panels (1 + 100 k, 29, 97 + 100 (span - 1), 394): on screen
//               rows 30..423 (row 29 is the hairline's lower AA row) + fade
//   bottom bar  strip (0, 425, 800, 54) slot kStrip+1, separators at 100 i,
//               labels centred at 50 + 100 i, underline y 474, dots y 463
//
// The stock "0.5 px" lines show on screen as a 3-px band (core + two faint
// halo rows); the "1.5 px" outlines as a 5-px band. Both are reproduced with
// alpha-blended fills so the result is identical on the FT81x and in the
// simulator (kHair* / kStroke* below).
#pragma once

#include <cstdint>

#include "ui/gfx.h"
#include "ui/theme.h"

namespace ot {
namespace wg {

// "Take the colour from the theme" for the optional colour parameters.
constexpr uint32_t kThemeColour = 0xFF000000u;

// --- frame geometry (spec 20 §1.2, VISUAL_DIFF §2.3) -----------------------
constexpr int kHairlineY = 28;      // title hairline core row
constexpr int kMainTop = 29;
constexpr int kPanelY = 29;         // stock control y; the fill shows from kPanelFillTop
constexpr int kPanelH = 394;        // rows y .. y + h (29..423), fade 414..423
constexpr int kPanelFillTop = 30;   // first panel row on screen (C3: row 29 = hairline AA)
constexpr int kTitleBandH = 30;     // title band rows 0..29 (under the hairline's AA rows)
constexpr int kFadeH = 10;          // scissor (x, y + h - 10, w + 1, 12)
constexpr int kStripY = 425;        // strip fill 425..479, edge row 424
constexpr int kStripH = 55;
constexpr int kSeparatorTop = 438;  // separators 438..479 (cap at 437)
constexpr int kLabelY = 451;        // single-line label centre
constexpr int kLabelY1 = 441;       // two-line: title centre
constexpr int kLabelY2 = 461;       //           value centre
constexpr int kUnderlineY = 474;    // x cx - 40 .. cx + 40
constexpr int kUnderlineHalf = 40;
constexpr int kDotY = 463;          // page dots, 7 px, 14 px pitch
constexpr int kDotPitch = 13;      // Part 2 / snap 001: 737 / 750 / 763
constexpr int kTitleTextX = 9;      // clipped to 270 px
constexpr int kTitleClipW = 270;
constexpr int kTitleCentreY = 13;
constexpr int kStatusX = 280;       // status string origin (spec 20 §1.2); ink at 300 / 288 (C10)
constexpr int kStatusTimeEnd = 360; // "%3im:%02is" ends here in set 0 (starts at 280)
constexpr int kStatusIconX = 370;   // 2 x 12-px transport glyphs
constexpr int kStatusPosX = 404;    // "P10.001"
constexpr int kStatusRecX = 474;    // 2 x 12-px record glyphs
constexpr int kRightTextX = 790;    // right-aligned instrument / project text
constexpr int kRightTextMinX = 526;
constexpr int kListRowH = 27;
constexpr int kProgressX = 190;
constexpr int kProgressW = 420;
constexpr int kRightPopupX = 600;   // right-hand popups (Instruments, Fx): x 600..799
constexpr int kLabelYTop = 442;     // class-0x20 label strip (keyboard): single line, caps 437..447

// --- anti-aliasing levels (alpha 0..255, measured) -------------------------
constexpr uint8_t kHairCoreA = 140;   // 0.5-px line core row
constexpr uint8_t kHairHaloA = 22;    // its two neighbour rows / end caps
constexpr uint8_t kTitleHaloA = 23;   // title hairline halos (rows 27 / 29)
constexpr uint8_t kStrokeA1 = 227;    // 1.5-px outline: rows +-1 of the centre line
constexpr uint8_t kStrokeA2 = 29;     //                 rows +-2
constexpr uint8_t kEdgeA = 120;       // half-covered edge pixels (1-px RECTS edges)

// Colour `fg` blended over `bg` with alpha a (0..255), like the display does.
uint32_t mix(uint32_t bg, uint32_t fg, int a);

// --- primitives -------------------------------------------------------------
// Rectangle filled with colour c at alpha a (COLOR_A a ... COLOR_A 255).
void blendFill(Gfx& g, int x, int y, int w, int h, uint32_t c, uint8_t a);
// Stock 0.5-px horizontal line from x0 to x1 (inclusive) at row y: core row
// plus halo rows y - 1 / y + 1 and halo end caps at x0 - 1 / x1 + 1.
void hairH(Gfx& g, int x0, int x1, int y, uint32_t c);
// Vertical 0.5-px line at column x, rows y0..y1, halo columns x +- 1 and a
// halo cap above y0.
void hairV(Gfx& g, int x, int y0, int y1, uint32_t c);
// Stock 1.5-px outline whose centre lines are x0, x1 (columns) and y0, y1
// (rows): a 5-px band 28 / 227 / 255 / 227 / 28 (selection boxes, focus frames).
void stroke(Gfx& g, int x0, int y0, int x1, int y1, uint32_t c);
// n outlines of one colour (box[k] = x0, y0, x1, y1) in three batches.
void strokes(Gfx& g, const int (*box)[4], int n, uint32_t c);
// Filled circle of radius r (px) centred on pixel (cx, cy) (POINTS).
void dot(Gfx& g, int cx, int cy, float r, uint32_t c);

// --- title bar (spec 20 §1.2) ------------------------------------------------
enum class Transport : uint8_t { kStopped, kPlayingSong, kPlayingPattern, kLoopingInSong };

// 2.x CPU load cell [I] (docs/specs/2x-02 §2.2; stock has no CPU meter):
// "CPU nn%" (P, the 1 s peak) or "CPU!" after an overrun, 60 x 24 px,
// right-aligned at the play time's ink x - 4. The "CPU" label is drawn with
// the title chrome (cached fragment), the value with the status group (<= 6
// display-list words). While shown the title clip narrows to end 10 px before
// the cell (217 px with font set 0). Font 4 like the status group; colours:
// normal = theme dim text, warn = slot kYellow, alarm = slot kOrange,
// critical = slot kSolidRect (red).
enum class CpuLevel : uint8_t { kHidden, kNormal, kWarn, kAlarm, kCritical };
struct CpuCell {
    CpuLevel level = CpuLevel::kHidden;
    uint8_t percent = 0;      // P rounded to whole percent, 0..99
    bool overrun = false;     // "CPU!" (an overrun in the last 2 s)
    int16_t right = 0;        // right edge x (cpuCellRight)
    bool shown() const { return level != CpuLevel::kHidden; }
    bool operator==(const CpuCell& o) const {
        return level == o.level && percent == o.percent && overrun == o.overrun && right == o.right;
    }
    bool operator!=(const CpuCell& o) const { return !(*this == o); }
};
constexpr int kCpuCellW = 60;
constexpr int kCpuCellH = 24;
constexpr int kCpuCellGap = 4;      // cell right edge = time ink x - 4
constexpr int kCpuTitleGap = 10;    // title clip ends this far before the cell
// Display state from the meter (mode: Config "CPU meter" 0 Auto, 1 Always,
// 2 Off). Auto: hidden below 60 % unless an overrun / governor cut in the
// last 2 s; Off or no data: hidden. right is left 0 (the caller sets it).
CpuCell cpuCell(uint8_t mode, bool hasData, uint32_t peakPermille, bool overrunRecent, bool cutRecent);
uint32_t cpuCellColour(const Theme& th, CpuLevel level);
// Right edge of the cell for the play time `seconds` (statusLayout(seconds).timeX - 4).
int cpuCellRight(uint32_t seconds);
// Title clip width with the cell shown (or kTitleClipW when cell is null / hidden).
int titleClipWidth(const CpuCell* cell);
// Width of the CPU cell in the bound font set (>= kCpuCellW).
int cpuCellWidth();

struct TitleStatus {
    uint32_t seconds = 0;          // play time
    Transport transport = Transport::kStopped;
    bool recording = false;        // record glyphs after the position
    int pattern = 1;               // "P%i.%03i" (hex: "P%02X.%02X")
    int step = 1;
    bool hex = false;
    CpuCell cpu;                   // 2.x CPU cell (hidden by default)
};
// "%im:%02is" (the stock field is %3i, right-anchored at kStatusTimeEnd).
void formatStatusTime(char* out, int cap, uint32_t seconds);
// "P10.001" / "P0A.01".
void formatStatusPosition(char* out, int cap, int pattern, int step, bool hex);
// Band (0, 0, 800, 30), bold title at x 9 clipped to 270 px, the status group
// (status may be null = hidden, Sample Editor, right popups) and the
// right-aligned text (may be null), clipped to x 526..790 (the end is dropped
// when too long). The status is the stock string "%3im:%02is", the transport
// glyph pair and "P%i.%03i" laid out with the active font set's advances from
// x 280: ink at 300 with set 0 and at 288 with set 1 ("New", C10).
void titleBar(Gfx& g, const Theme& th, const char* title, const char* right, const TitleStatus* status);
// titleBar() in two parts (2.x: the static part goes through a cached RAM_G
// fragment, the status changes every step): band, title and right text laid
// out for a status group present or not; then the status group. The status
// and the right text never overlap, so the pixels match titleBar().
// cpu: the 2.x CPU cell (null or hidden: stock chrome) -- narrows the title
// clip and draws the cell's "CPU" label.
void titleBarChrome(Gfx& g, const Theme& th, const char* title, const char* right, bool status,
                    const CpuCell* cpu = nullptr);
void titleBarStatus(Gfx& g, const Theme& th, const TitleStatus& status);
// x of the status parts for the active font set (host tests): time string,
// transport glyph pair (2 symbol cells), position string.
struct StatusLayout { int timeX, iconX, symW, posX; };   // timeX: after the pad spaces
StatusLayout statusLayout(uint32_t seconds);
// The hairline under the band (rows 27 / 28 / 29), x0..x1. Drawn after the
// screen: rows 28 / 29 are repainted in the band colour first (stock draws
// the title last, over the panels' first row). colour kThemeColour = slot
// kTitleHairline.
void titleHairline(Gfx& g, const Theme& th);
void titleHairline(Gfx& g, const Theme& th, int x0, int x1, uint32_t colour = kThemeColour);

// --- popup headers (VISUAL_DIFF Part 2 §7.2) --------------------------------
// White band (slot kKeyboardLabels+1) over title rows 0..26 from x0 to x1
// with the black bold title (slot kKeyboardLabels, font 4, cap y 8..19) at
// textX.
void popupHeader(Gfx& g, const Theme& th, const char* title, int x0, int x1, int textX);
// Right-hand popup (Instruments, Fx; 015 / 018): header x 600..799, text ink
// from x 621, rows 27 / 28 868686 / 525252. Ui::openPopupHeader draws it over
// the title bar after the hairline; screens draw rightPopupPanel under their
// list.
void rightPopupHeader(Gfx& g, const Theme& th, const char* title);
// Panel of a right-hand popup: x 600..799, rows y..479 (covers the bottom bar
// there; the x 600 separator stays), list at (600, 30).
void rightPopupPanel(Gfx& g, const Theme& th, int y = kPanelFillTop);
// Full-width popup header (Notes, 016): band x 2..797 (AA columns 1 / 798),
// row 27 b5b5b5, text ink from x 12.
void fullPopupHeader(Gfx& g, const Theme& th, const char* title);

// --- bottom bar (spec 20 §1.2 parts 1..5) ------------------------------------
// Underlines are a *state*, independent of the label's enabled colour
// (VISUAL_DIFF Part 2 C7): green = available, red = "edit (Rec) mode only".
// In Rec mode Fill / Undo / Redo are enabled (777777) with the red line;
// outside it they are disabled (333333) and keep the red line.
enum Underline : uint8_t {
    kUnderlineNone = 0,
    kUnderlineGreen = 1,
    kUnderlineRed = 2,
    kUnderlineAvailable = kUnderlineGreen,
    kUnderlineEditMode = kUnderlineRed,
};
// Strip variants (VISUAL_DIFF Part 2 C5): strip 0a0a0a + separators 464646
// on screen (Pattern, Config list, File, Keyboard, Performance, Mixer /
// Master, Export, Import Mod) and strip 050505 + separators 434343 (Instrument
// Params, Playback, Fill popup, Games, Config pickers). Both use the same
// separator slot; only the fill differs. Other themes: kDark = the theme's
// own strip unless it is 0a0a0a.
enum class StripStyle : uint8_t { kNormal, kDark };
uint32_t stripColour(const Theme& th, StripStyle style);
// Strip fill (0a0a0a) with its 1-px edge, and a separator at x = 100 i for
// every set bit i (1..7) of `separators` (stock value 0xff; Song 0xbf).
// bg = kThemeColour uses slot kStrip+1. No separators when !th.barLines().
void bottomStrip(Gfx& g, const Theme& th, uint8_t separators = 0xFE, uint32_t bg = kThemeColour);
inline void bottomStrip(Gfx& g, const Theme& th, uint8_t separators, StripStyle style) {
    bottomStrip(g, th, separators, stripColour(th, style));
}
// Centre x of bottom label i (wide: spans i and i + 1).
inline int labelCentre(int i, bool wide = false) { return wide ? 100 + 100 * i : 50 + 100 * i; }
struct BarLabelStyle {
    bool enabled = true;     // false: colour set +0x34 (333333 / 575757)
    bool focus = false;      // red 1.5-px focus frame around the label box
    bool brightTitle = false;  // title in the value colour (opt-in; focus keeps it grey, 006)
    bool fill = false;       // active toggle: solid red box (Record armed)
    uint8_t underline = kUnderlineNone;
    uint8_t pages = 0;       // > 1: page dots under the title ("More")
    uint8_t page = 0;
    bool topAligned = false; // class-0x20 label strip (keyboard): single line at kLabelYTop
    bool nameLines = false;  // a name on two lines (set +0x44): both lines grey, font 3
    uint32_t dotOff = kThemeColour;   // colour of the other page dots (pageDots `off`)
};
// One label: title grey (font 3), value white (font 2; font 3 when too wide
// for the label) on two lines, a two-line name (both grey, font 3), or one
// centred line; plus underline (not in themes without bar lines), dots,
// frame / fill.
void barLabel(Gfx& g, const Theme& th, int i, bool wide, const char* top, const char* bottom,
              const BarLabelStyle& st);
void barUnderline(Gfx& g, const Theme& th, int cx, uint8_t kind);
// Page dots: current in the label colour, the others in `off` (kThemeColour
// = slot kBottomLabel+17, 222222: Pattern 001 / 012; Instrument Parameters
// 011 shows 323132 = slot kBottomLabel+1).
void pageDots(Gfx& g, const Theme& th, int cx, int pages, int page, uint32_t off = kThemeColour);
// Red focus frame around bottom label i: centre lines (1 + 100 i, 424) ..
// (98 + 100 i (+ 100 wide), 479).
void barFocus(Gfx& g, const Theme& th, int i, bool wide = false);
// Focus frames of every label in `mask` (bit i = label i) in one batch
// (Live-REC arm page: eight frames).
void barFocusMask(Gfx& g, const Theme& th, uint8_t mask, uint8_t wideMask = 0);
// Red fill of label i: (3 + 100 i, 424, 95 (+ 100 wide), 56) with soft edges.
void barFill(Gfx& g, const Theme& th, int i, bool wide = false);

// --- panels (spec 20 §1.3, §1.5, §3.3) -----------------------------------------
inline int columnX(int col) { return 1 + 100 * col; }
inline int columnW(int span = 1) { return 97 + 100 * (span - 1); }
// Bottom fade of a control box (style bit21, theme flag +0x444): vertical
// gradient bg -> 000000 over (x, y + h - 10, w + 1, 12).
void bottomFade(Gfx& g, const Theme& th, int x, int y, int w, int h, uint32_t bg);
// Panel: solid x .. x + w, rows y .. y + h (stock RECTS extent: a panel at
// (x, 29, w, 394) covers 29..423, row 29 then lies under the title band),
// soft 1-px side edges, bottom fade over rows y + h - 9 .. y + h (theme
// flag). bg = kThemeColour uses slot kEmptyPanel+1 (0a0a0a).
void panel(Gfx& g, const Theme& th, int x, int y = kPanelY, int w = 97, int h = kPanelH,
           uint32_t bg = kThemeColour, bool fade = true);
// Column panel k (x 1 + 100 k) spanning `span` columns.
inline void panelColumn(Gfx& g, const Theme& th, int col, int span = 1) {
    panel(g, th, columnX(col), kPanelY, columnW(span), kPanelH);
}
// Focus frame (class 0x02): 1.5-px outline on centre lines x0..x1, y0..y1 in
// slot kFocusFrame, optional inner fill (kThemeColour = none).
void focusFrame(Gfx& g, const Theme& th, int x0, int y0, int x1, int y1, uint32_t fill = kThemeColour);
// Red frame around a focused column panel (x, w as given to panel()):
// centre lines x .. x + w, 31 .. 422.
inline void panelFocus(Gfx& g, const Theme& th, int x, int w) { focusFrame(g, th, x, 31, x + w, 422); }

// --- scrolling list (class 0x08, spec 20 §3.5) ---------------------------------
enum class ListSel : uint8_t { kFromSet, kWhite, kRed, kGreen, kNone };
struct ListView {
    int x = 1, y = kPanelY, w = 197, h = kPanelH;
    int count = 0;
    int top = 0;               // first visible item (see listFollow)
    int sel = -1;              // selected item (-1 none)
    bool active = false;       // selection in set colour[1] instead of [0]
    bool background = true;    // panel bg + fade (style bit13 / bit21)
    int set = slot::kList;     // 7-colour set: sel, sel active, thumb, bg, text, marked, track
    ListSel selStyle = ListSel::kFromSet;
    uint8_t font = kFont1;
    // false: a disabled list (Master Delay Rate while Sync is off, round-3
    // Snapshot_007): items in the disabled label colour (slot
    // kBottomLabelOff), the selection outline at half strength over the
    // list background; the scroll bar is unchanged.
    bool enabled = true;
    // Item text; `marked` (may be null) selects text colour[5].
    void (*item)(void* ctx, int index, char* out, int cap) = nullptr;
    bool (*marked)(void* ctx, int index) = nullptr;
    void* ctx = nullptr;
};
// Rows that fit: (h - 4) / 27 (14 for a 394-px list).
inline int listVisibleRows(int h) { return h > 4 ? (h - 4) / kListRowH : 0; }
// Keeps `sel` visible with one row of context at either edge; returns the
// new first visible item.
int listFollow(int sel, int count, int visible, int top);
void list(Gfx& g, const Theme& th, const ListView& v);

// --- value bar (class 0x00, spec 20 §3.4) --------------------------------------
struct ValueBarStyle {
    bool bipolar = false;          // -1..1: a white line, centre = 0
    bool background = true;        // column bg + fade
    float ref = -1.0f;             // unipolar reference 0..1 (< 0: none)
    uint32_t fill = kThemeColour;  // fill / line colour (level meter: green)
    int set = slot::kValueBar;     // fill, below-ref, above-ref, track, bg
};
// Box (x, y, w, h) (typically a column panel). value: 0..1, or -1..1 bipolar.
// Track x + 10 .. x + w - 10, rows y + 11 .. y + h - 14 (inclusive, soft
// 1-px edges: the "well" x + 9 .. x + 88 of Part 2 §7.4) with the 9-px left
// inner gradient; fill x + 11 .. x + w - 11 (soft 1-px edges) growing up from
// row y + h - 15 by round((h - 24) v) rows (39..408 full for a column at 29);
// value 0 = the 1.5-px line on row y + h - 15 alone. Bipolar: a 1-row line at
// the track middle.
void valueBar(Gfx& g, const Theme& th, int x, int y, int w, int h, float value,
              const ValueBarStyle& st = ValueBarStyle());
// Many unipolar value bars without background (valueBar with background =
// false, the default set) drawn in layers: all well edges in one RECTS
// batch, all wells in one, the inner gradients, then the fills batched per
// colour. The bars must not overlap; the pixels are then the same as one
// valueBar call per bar, in far fewer display-list words (FT81x RAM_DL).
struct ValueBarItem {
    int x, y, w, h;
    float value;
    uint32_t fill = kThemeColour;
};
void valueBars(Gfx& g, const Theme& th, const ValueBarItem* bars, int n);

// --- popups and text ----------------------------------------------------------
// Popup box (class 0x06 colours): fill colour[0], 1-px outline colour[1].
void popupBox(Gfx& g, const Theme& th, int x, int y, int w, int h);
// Progress popup (spec 20 §3.7): box (190, y, 420, h), title centred at
// y + h / 3, 2-px bar at (x + w / 7, y + 3 h / 5) of length 5 w / 7.
void progressPopup(Gfx& g, const Theme& th, const char* title, int percent, int y = 190, int h = 100);
// Boot splash (Snapshot_035): frame-clear background, no popup box, title
// (font 1, centred on x 400, caps from y 303), 2-px bar in slot kProgress+2
// at x 250, rows 331..332 (5 w / 7 = 300 px at 100 %, 1-px AA ring), no
// track; `version` (may be null) in font 1 at (11, 461..471). OpenTracker
// draws no logo (README: no Polyend branding).
void bootSplash(Gfx& g, const Theme& th, const char* title, int percent, const char* version);
// Message popup in the same style: title and one text line.
void messagePopup(Gfx& g, const Theme& th, const char* title, const char* text, int y = 170, int h = 120);
// Text box (class 0x16): word-wrapped at w - 5 from (x + 5, y + 5), line
// pitch fontHeight + extraPitch; optional background. Returns lines drawn.
int textBox(Gfx& g, const Theme& th, int x, int y, int w, int h, uint8_t font, const char* text, int extraPitch = 0,
            uint32_t colour = kThemeColour, bool background = false);
// Name field (spec 20 §3.8, Snapshot_020): box x .. x + w - 1, y .. y + h - 1
// (keyboard: (1, 31, 798, 93)); outer fill 0a0a0a, inner box inset 10 px in
// slot kTitle+1 (080808, soft 1-px edge 090909), the text (font 2) centred
// on x + w / 2, a 1.5-px text cursor (25 rows, centred) after `cursor`
// characters, selection highlight when selAll; 1.5-px red outline on the
// box's edge lines when focused (cursor red too). cellW is not used any more
// (proportional text).
void nameField(Gfx& g, const Theme& th, int x, int y, int w, int h, const char* text, int cursor, bool selAll,
               bool focused, int cellW = 15);
// On-screen keyboard key: fill colour[0] (0a0a0a), centred label colour[1]
// (or textColour), 1.5-px outline colour[2] (red) when selected (or selColour).
void key(Gfx& g, const Theme& th, int x, int y, int w, int h, const char* label, bool selected,
         uint32_t textColour = kThemeColour, uint32_t selColour = kThemeColour);
// Stock 42-key keyboard key (Snapshot_020): solid box x .. x + w - 1,
// y .. y + h - 1 (61 x 61; wide keys 65 n - 4) with a soft 1-px edge, label
// in font 2 centred; `home` (f / j) = white key with a black label; red
// 1.5-px outline on the box edge when selected. textColour as for key().
void keyboardKey(Gfx& g, const Theme& th, int x, int y, int w, int h, const char* label, bool selected,
                 bool home = false, uint32_t textColour = kThemeColour);
// Value-picker cell: dark cell, red outline when selected.
void pickerCell(Gfx& g, const Theme& th, int x, int y, int w, int h, bool selected);

}  // namespace wg
}  // namespace ot
