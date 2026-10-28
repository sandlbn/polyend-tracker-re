// Colour themes (spec 05 §3.5, spec 20 §1.4). Colours are 0x00RRGGBB.
#pragma once

#include <cstdint>

namespace ot {

// spec 20 §1.4: every theme is a table of 277 words. Index i is the colour
// "slot" i (byte offset 4 * i in the stock table); controls take a contiguous
// block starting at their colour-set offset. Slots 273..276 are flags (0/1).
constexpr int kThemeSlotCount = 277;

// Named slot indices (spec 20 §1.4 roles). A block's later colours follow its
// first slot, e.g. kSlotGrid + 2 is the Note text colour.
namespace slot {
constexpr int kFrameClear = 0;      // frame clear colour
constexpr int kTitle = 1;           // set +0x04: title text, bg, frame, line2
constexpr int kStatus = 5;          // set +0x14: status text
constexpr int kBottomLabel = 9;     // set +0x24: bottom label normal (text, bg, frame, line2)
constexpr int kBottomLabelOff = 13; // set +0x34: bottom label disabled
constexpr int kBlink = 37;          // set +0x94: blink (all red)
constexpr int kEmptyPanel = 49;     // set +0xc4
constexpr int kStrip = 57;          // class 0x01 strip: separator, bg, frame
constexpr int kGrey = 66;          // +0x108 grey 777777 (role inferred)
constexpr int kIconText = 67;       // class 0x1e icon + text (6)
constexpr int kValueBar = 73;       // fill, below-ref, above-ref, track, bg
constexpr int kValueBarAlt = 78;
constexpr int kList = 83;           // sel frame, sel frame active, thumb, bg, text, marked, track
constexpr int kListRed = 97;        // set +0x184 (Loader / browser)
constexpr int kListGreen = 104;     // set +0x1a0 (green marked text)
constexpr int kFocusFrame = 130;    // class 0x02: frame, optional fill
constexpr int kKeyboardLabels = 137;
constexpr int kKeyboardList = 141;
constexpr int kNameHeader = 148;
constexpr int kTextBox = 152;
constexpr int kKeyboardKeys = 156;  // key fill, key text, selection frame
constexpr int kLabelStrip = 159;    // class 0x20
constexpr int kProgress = 162;      // bg, border, bar fill, bar track
constexpr int kTabs = 166;
constexpr int kRomMessage = 172;
constexpr int kGrid = 174;          // pattern grid block [0..20] (spec 20 §1.4)
constexpr int kOverlay = 195;
constexpr int kSongList = 199;      // class 0x12 (10)
constexpr int kConfigHelp = 209;
constexpr int kConfigDesc = 216;
constexpr int kPerfVertical = 230;
constexpr int kPerfPanels = 234;
constexpr int kBitmap = 242;
constexpr int kWave = 244;          // wave, centre line
constexpr int kWaveOverlay = 246;
constexpr int kPlaybackMarkers = 252;
constexpr int kMarkers = 256;       // normal, selected, alt
constexpr int kDefaultLabel = 262;  // text, bg, frame, line2
constexpr int kTitleHairline = 266;
constexpr int kSolidRect = 267;     // also red underline
constexpr int kOrange = 268;
constexpr int kYellow = 269;
constexpr int kGreen = 270;
constexpr int kCyan = 271;
constexpr int kGridLine = 272;
constexpr int kFlagBottomFade = 273;     // style bit21 fades
constexpr int kFlagValueBarGrad = 274;   // value-bar inner left gradient
constexpr int kFlagCursorRowGrad = 275;  // pattern cursor-row shading
constexpr int kFlagUnknown = 276;
// Flag 276 (spec 20 §1.4 "role unknown": 1 / 1 / 0) is the only flag that
// separates Anniversary from the other two, and Anniversary is the only theme
// that draws no bar underlines and no strip separators (VISUAL_DIFF Part 2
// C6, §8.23): OpenTracker reads it as "bar lines drawn".
constexpr int kFlagBarLines = kFlagUnknown;
// Pattern grid block entries (kGrid + n) the theme helpers below name.
constexpr int kGridCursorRow = kGrid + 13;   // cursor-row shading (000000 / 303030 / 222222)
constexpr int kGridCursorText = kGrid + 14;  // text on the cursor cell (ffffff / 000000 / 000000)
}  // namespace slot

// Named roles: views of the slot table (theme.cpp makeTheme), kept for the
// screens that predate the slot API.
struct Theme {
    const char* name;
    uint32_t bg;          // screen background (frame clear, slot 0)
    uint32_t text;        // primary text
    uint32_t dim;         // secondary / disabled text
    uint32_t panel;       // label / control background
    uint32_t panel2;      // darker panel
    uint32_t panelDark;   // darkest panel (grid rows)
    uint32_t accent;      // selection / cursor
    uint32_t alert;       // warnings
    uint32_t good;        // positive / meters
    uint32_t yellow;
    uint32_t orange;
    uint32_t cyan;
    uint32_t track[4];    // pattern column colours: note, instr, fx1, fx2
    uint32_t trackDark[4];
    uint32_t grid;        // grid lines
    const uint32_t* slots;   // kThemeSlotCount entries (spec 20 §1.4)

    // Colour slot i (spec 20 §1.4); out-of-range indices return 0.
    uint32_t slot(int i) const { return (i >= 0 && i < kThemeSlotCount) ? slots[i] : 0u; }
    bool flag(int i) const { return slot(i) != 0; }

    // Theme behaviour (VISUAL_DIFF Part 2 §9). Original / Monochrome / Anniversary:
    // Panels and lists fade to black over their last 10 rows: yes / no / no.
    bool panelFade() const { return flag(slot::kFlagBottomFade); }
    // Value-bar track has its 9-px left inner gradient: yes / no / no.
    bool valueBarGradient() const { return flag(slot::kFlagValueBarGrad); }
    // Pattern cursor row: 10-px gradients (+ 1-px line) around the row in
    // cursorRowShade() (yes / yes), or one solid band in that colour (no,
    // Anniversary: 222222 over the full width incl. the step columns).
    bool cursorRowGradient() const { return flag(slot::kFlagCursorRowGrad); }
    uint32_t cursorRowShade() const { return slot(slot::kGridCursorRow); }
    // Text / dashes on the (green or red) cursor cell: ffffff / 000000 / 000000.
    uint32_t cursorCellText() const { return slot(slot::kGridCursorText); }
    // Bottom-bar underlines and strip separators are drawn: yes / yes / no.
    bool barLines() const { return flag(slot::kFlagBarLines); }
    // Dark strip variant (Part 2 C5): half the strip colour on a theme whose
    // panels fade (Original: 0a0a0a -> 050505), else the strip colour itself.
    uint32_t darkStrip() const;
};

// 0 = Original, 1 = Monochrome, 2 = Anniversary (config "Display theme").
constexpr uint8_t kThemeCount = 3;
const Theme& theme(uint8_t index);
// The named-role view of any slot table (2.x theme files, ui/theme_file.h).
// The fields are copied: rebuild the view after the slots change.
Theme themeView(const char* name, const uint32_t* slots);

// Display font sets (config "Display font", spec 20 §2.1): 0 Original
// (default), 1 New. The active set lives in gfx (setFontSet / fontSet).
constexpr uint8_t kFontSetOriginal = 0;
constexpr uint8_t kFontSetNew = 1;
// "Original" / "New"; out-of-range -> "Original".
const char* fontSetName(uint8_t set);

}  // namespace ot
