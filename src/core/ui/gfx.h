// Thin drawing helpers over IDisplay (FT81x display list, spec 05 §3).
//
// Text uses the FT81x ROM fonts (part of the controller, spec 05 §3.6 lists
// the vendor's own fonts which we do not copy). Screens pass a *logical* font
// role; Gfx::text resolves it to a ROM font handle through the active font
// set (config "Font", spec 05 §3.6: two sets, applied at boot and on change).
//
//   role         set 0 "Set 1" (L4 antialiased)   set 1 "Set 2" (L1 bitmap)
//   kFontMono    18 (8x16 fixed)                   18 (grid cell, never changes)
//   kFontSmall   26 (h16)                          21 (h17)
//   kFontText    27 (h20)                          22 (h20)
//   kFontLarge   28 (h25)                          23 (h22)
//   kFontTitle   29 (h28)                          24 (h29)
//
// Set 1 keeps the heights of set 0 within a few pixels so every screen layout
// fits unchanged (stock set "New" is the wider/taller one; README differences).
#pragma once

#include <cstdarg>
#include <cstdint>

#include "hal_api.h"
#include "ui/theme.h"

namespace ot {

constexpr uint8_t kFontMono = 18;
constexpr uint8_t kFontSmall = 26;
constexpr uint8_t kFontText = 27;
constexpr uint8_t kFontLarge = 28;
constexpr uint8_t kFontTitle = 29;
constexpr uint8_t kFontSetCount = 2;

// Stock font roles (spec 20 §2.2 / §2.5): fonts 1..5 of the active set
// (config "Display font": 0 Original, 1 New). Set-0 cell / advance:
//   kFont1  10x18, 8 px mono   list rows, step numbers, small values
//   kFont2  10x22, 10 px mono  pattern grid cells (11-px pitch for "--")
//   kFont3  10x19, 6..9 px     bottom bar labels, panel / popup titles
//   kFont4  12x22, 10 px bold  title bar, track header tabs, column headers
//   kFont5   8x16, 7 px mono   text boxes, help text
//   kFontIcons                 icon atlas (Gfx::icon)
// With custom fonts bound (applyFontSet returned true) they resolve to
// IDisplay::kFontHandleBase + 0..4 / kIconHandle. Fallback (today, ROM fonts
// only): kFont1 -> kFontSmall, kFont2 -> kFontMono, kFont3 -> kFontText,
// kFont4 -> kFontLarge, kFont5 / kFontIcons -> kFontSmall, then the set-1
// ROM substitution below. Role values 1..6 are not ROM handles (16..31).
constexpr uint8_t kFont1 = 1;
constexpr uint8_t kFont2 = 2;
constexpr uint8_t kFont3 = 3;
constexpr uint8_t kFont4 = 4;
constexpr uint8_t kFont5 = 5;
constexpr uint8_t kFontIcons = 6;

// Active font set (config "Font", 0..1; out-of-range -> 0). Global like theme().
void setFontSet(uint8_t set);
uint8_t fontSet();
// ROM font handle for a logical font role in the active set (or `set`).
// Values that are not a role are passed through unchanged.
uint8_t romFontFor(uint8_t role);
uint8_t romFontFor(uint8_t role, uint8_t set);

// Select font set `set` (setFontSet) and ask the display to bind its custom
// fonts (IDisplay::bindFontSet); `d` may be null. Returns customFontsBound().
bool applyFontSet(IDisplay* d, uint8_t set);
// True after applyFontSet bound custom fonts: kFont1..kFontIcons then name
// bitmap handles and glyphRun draws VERTEX2II glyphs.
bool customFontsBound();
// ROM-font text without custom fonts (docs/OPTIMIZATION_2X.md OP-6):
//  kRomGlyphs (default): Gfx::glyphRun and the Pattern grid cells draw the
//    ROM font bitmaps with VERTEX2II;
//  kRomText: the same runs as CMD_TEXT (per glyph at a fixed pitch), the
//    pixel reference for the simulator's romcheck;
//  kRomWholeText: as before OP-6, the Pattern grid cells as one CMD_TEXT
//    string each (tests/ui_fakes.h Harness: host tests read whole strings).
enum RomTextMode : uint8_t { kRomGlyphs = 0, kRomText = 1, kRomWholeText = 2 };
void setRomTextMode(RomTextMode m);
RomTextMode romTextMode();

// Pen advance of `s` (or its first `len` chars, len < 0 = all) in pixels for a
// font role or ROM handle. ROM fonts: the FT81x ROM advance tables (the
// simulator's look-alikes use the same widths). Unknown handles: 8 px/char.
int textWidth(uint8_t font, const char* s, int len = -1);
// Cell height in pixels of a font role or ROM handle.
int fontHeight(uint8_t font);

// Icons drawn by Gfx::icon in a kIconSize x kIconSize box (top-left x, y):
// with custom fonts bound, cell `id` of the generated icon sheet on
// kIconHandle (our own drawings, tools/fonts/gen_fonts.py); fallback: simple
// primitives (stock icon art is never copied, spec 20 §5).
enum IconId : uint8_t {
    kIconPlay,
    kIconStop,
    kIconPause,
    kIconRec,
    kIconLoop,
    kIconArrowUp,     // ▲
    kIconArrowDown,   // ▼
    kIconArrowLeft,   // ◀
    kIconArrowRight,  // ▶
    kIconBattery,
    kIconFolder,
    kIconCount
};
constexpr int kIconSize = 12;

constexpr int kMonoW = 8;     // ROM font 18 cell width
constexpr int kMonoH = 16;

constexpr int kScreenW = 800;
constexpr int kScreenH = 480;
constexpr int kTitleH = 28;          // title strip
constexpr int kBarY = 424;           // bottom labels (spec 05 §2.2)
constexpr int kBarLabelW = 94;
constexpr int kBarLabelH = 55;

// IDisplay sink that only hashes the commands it receives (FNV-1a 64 over
// opcode + arguments + strings). Gfx::cached() runs a block into it to learn
// whether the block's stored fragment is still current.
class HashDisplay final : public IDisplay {
public:
    static constexpr uint64_t kSeed = 0xCBF29CE484222325ull;
    uint64_t h = kSeed;
    void reset() { h = kSeed; }

    uint16_t width() const override { return 800; }
    uint16_t height() const override { return 480; }
    void dlStart() override { w(0x01000000u); }
    void clearColorRGB(uint8_t r, uint8_t g, uint8_t b) override { w(0x02000000u | rgb(r, g, b)); }
    void clear(bool c, bool s, bool t) override { w(0x03000000u | (c ? 4u : 0u) | (s ? 2u : 0u) | (t ? 1u : 0u)); }
    void colorRGB(uint8_t r, uint8_t g, uint8_t b) override { w(0x04000000u | rgb(r, g, b)); }
    void colorA(uint8_t a) override { w(0x05000000u | a); }
    void lineWidth(uint16_t v) override { w(0x06000000u | v); }
    void pointSize(uint16_t v) override { w(0x07000000u | v); }
    void scissorXY(uint16_t x, uint16_t y) override { w(0x08000000u, xy(x, y)); }
    void scissorSize(uint16_t x, uint16_t y) override { w(0x09000000u, xy(x, y)); }
    void begin(Prim p) override { w(0x0A000000u | p); }
    void end() override { w(0x0B000000u); }
    void vertex2f(int32_t x, int32_t y) override { w(0x0C000000u ^ uint32_t(x), uint32_t(y)); }
    void vertex2ii(uint16_t x, uint16_t y, uint8_t hh, uint8_t c) override {
        w(0x0D000000u | (uint32_t(hh) << 8) | c, xy(x, y));
    }
    void text(int16_t x, int16_t y, uint8_t font, uint16_t o, const char* s) override {
        w(0x0E000000u | (uint32_t(font) << 16) | o, xy(uint16_t(x), uint16_t(y)));
        str(s);
    }
    void number(int16_t x, int16_t y, uint8_t font, uint16_t o, int32_t n) override {
        w(0x0F000000u | (uint32_t(font) << 16) | o, xy(uint16_t(x), uint16_t(y)));
        w(uint32_t(n));
    }
    void swap() override { w(0x10000000u); }
    void drawImage(int16_t x, int16_t y, uint32_t a, uint16_t ww, uint16_t hh) override {
        w(0x11000000u, xy(uint16_t(x), uint16_t(y)));
        w(a, xy(ww, hh));
    }
    void drawRgb332(int16_t x, int16_t y, uint32_t a, uint16_t ww, uint16_t hh, uint8_t s) override {
        w(0x12000000u | s, xy(uint16_t(x), uint16_t(y)));
        w(a, xy(ww, hh));
    }
    void fragmentAppend(uint8_t slot) override { w(0x13000000u | slot); }
    // 1.9.5 visual-parity primitives (must hash too: a block differing only
    // in a gradient, translate or transform must not reuse a fragment).
    void gradient(int16_t x0, int16_t y0, uint32_t c0, int16_t x1, int16_t y1, uint32_t c1) override {
        w(0x14000000u, xy(uint16_t(x0), uint16_t(y0)));
        w(c0, xy(uint16_t(x1), uint16_t(y1)));
        w(c1);
    }
    void vertexTranslateX(int32_t x16) override { w(0x15000000u, uint32_t(x16)); }
    void saveContext() override { w(0x16000000u); }
    void restoreContext() override { w(0x17000000u); }
    void bitmapTransform(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) override {
        w(0x18000000u, uint32_t(a));
        w(uint32_t(b), uint32_t(c));
        w(uint32_t(d), uint32_t(e));
        w(uint32_t(f));
    }
    void bitmapSize(uint8_t hh, uint16_t ww, uint16_t ht) override { w(0x19000000u | hh, xy(ww, ht)); }

private:
    static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return (uint32_t(r) << 16) | (uint32_t(g) << 8) | b; }
    static uint32_t xy(uint16_t x, uint16_t y) { return (uint32_t(x) << 16) | y; }
    void w(uint32_t v) {
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    void w(uint32_t a, uint32_t b) {
        w(a);
        w(b);
    }
    void str(const char* s) {
        uint32_t acc = 0, n = 0;
        for (; s && *s; ++s) {
            acc = (acc << 8) | uint8_t(*s);
            if (++n == 4) {
                w(acc);
                acc = n = 0;
            }
        }
        w(acc ^ (n << 24) ^ 0x80000000u);   // tail + terminator
    }
};

// Cached display-list fragments (2.x performance workstream; the stock
// firmware's per-control RAM_G lists, spec 05 §3.3). Owned by the Ui so it
// survives frames. Gfx::cached() keys every slot by a 64-bit hash of the
// command stream the block would emit, so a fragment is only reused when it
// is bit-for-bit what would be drawn now: no manual invalidation, and the
// output is identical to drawing inline. A block is captured only after the
// same hash was drawn inline in kStableProbes frames in a row (static chrome
// is cached from its third frame on; content that changes every frame never
// pays for a capture), and a slot whose content changed kHotChanges frames in
// a row is drawn inline without hashing for kHotSkip frames (a scrolling
// grid row).
struct FragmentCache {
    static constexpr uint8_t kSlots = IDisplay::kFragmentSlots;
    static constexpr uint8_t kMaxCapturesPerFrame = 8;   // each costs two FIFO syncs on the FT81x
    static constexpr uint8_t kStableProbes = 3;
    static constexpr uint8_t kHotChanges = 3;
    static constexpr uint8_t kHotSkip = 12;
    struct Slot {
        uint64_t stored = 0;     // hash of the fragment held by the display (valid if `has`)
        uint64_t seen = 0;       // hash drawn inline in the last probed frame
        uint8_t stable = 0;      // consecutive probes that saw `seen`
        uint64_t refused = 0;    // hash the display could not store (too large)
        bool has = false;
        uint8_t changes = 0;     // consecutive probes whose hash differed from the previous one
        uint8_t skip = 0;        // frames left drawn inline without probing
    };
    Slot slot[kSlots];
    bool enabled = true;         // false: always inline (A/B checks, diagnostics)
    // Statistics (cumulative): appended, captured, probed misses, skipped.
    uint32_t hits = 0, captures = 0, misses = 0, skips = 0;
    void reset() {
        for (Slot& s : slot) s = Slot{};
    }
};

// Fragment slots (one per cached block; a slot may serve several screens,
// the hash keeps them apart).
enum FragSlot : uint8_t {
    kFragTitle = 0,          // title strip background, title and subtitle
    kFragBar = 1,            // bottom bar (spec 06 §0.3)
    kFragPatHeaders = 2,     // Pattern grid: track header tabs
    kFragPatSeparators = 3,  // Pattern grid: track separators
    // The M0 per-row Pattern fragments (4..18) are gone: the 1.9.5 grid is
    // drawn in colour passes across all rows (screen_pattern_draw.cpp), not
    // row by row (docs/CHANGES_2X.md).
    kFragCount = 4,
};
static_assert(kFragCount <= IDisplay::kFragmentSlots, "fragment slots");

class Gfx {
public:
    explicit Gfx(IDisplay& d, FragmentCache* cache = nullptr) : d_(&d), base_(&d), cache_(cache) {}
    // Raw display access (the current sink: the display, or the hasher while
    // a cached() block is probed): closes an open glyph primitive and resets
    // VERTEX_TRANSLATE_X first (see sync()).
    IDisplay& display() {
        sync();
        return *d_;
    }

    void beginFrame(uint32_t bg);
    void endFrame();

    void color(uint32_t c);
    // COLOR_A (one DL word, sent before the next primitive that uses it). In
    // low detail (setLowDetail) primitives drawn with alpha <=
    // kLowDetailMaxAlpha (AA halos, soft edges) are skipped.
    void alpha(uint8_t a);
    // LINE_WIDTH / SAVE_CONTEXT / RESTORE_CONTEXT, counted in dlWords().
    // saveContext admits the whole group that follows (groupWords) and
    // returns false (nothing sent) when it does not fit or in low detail's
    // word budget; restoreContext is always sent after an admitted save.
    void lineWidth(uint16_t w16);
    bool saveContext(uint32_t groupWords = 0);
    void restoreContext();
    void fill(int x, int y, int w, int h, uint32_t c);
    void frame(int x, int y, int w, int h, uint32_t c, int lw = 1);
    void hline(int x, int y, int w, uint32_t c);
    void vline(int x, int y, int h, uint32_t c);
    void line(int x0, int y0, int x1, int y1, uint32_t c);
    void text(int x, int y, uint8_t font, uint32_t c, const char* s, uint16_t opt = 0);
    void textf(int x, int y, uint8_t font, uint32_t c, uint16_t opt, const char* fmt, ...)
        __attribute__((format(printf, 7, 8)));
    // Glyph run (spec 20 §2.1): one VERTEX2II per glyph, top-left at (x, y),
    // pen advance from the font table or `advanceOverride` px when > 0 (the
    // grid's 11-px "--" rule, §2.3); VERTEX_TRANSLATE_X keeps the 9-bit
    // VERTEX2II x in range from x >= 512 on. Without custom fonts the same
    // path draws the ROM font bitmaps (handles 16-31) with the romMetrics
    // advances; only a run clipped at the top / left edge (or a non-ROM
    // handle) falls back to CMD_TEXT (per glyph when advanceOverride > 0).
    void glyphRun(int x, int y, uint8_t font, const char* s, uint32_t colour, int advanceOverride = 0);
    int textWidth(uint8_t font, const char* s) const { return ot::textWidth(font, s); }
    // Text turned by quarterTurns x 90 degrees counter-clockwise (1 = reads
    // bottom to top; stock style bit7 "vertical text"). (x, y) is the
    // top-left of the turned box (cell height wide for 1 / 3), adjusted by
    // kOptCenterX / kOptRightX / kOptCenterY. Custom fonts: SAVE_CONTEXT,
    // bitmapSize + BITMAP_TRANSFORM, one VERTEX2II per glyph, RESTORE_CONTEXT.
    // Fallback (ROM fonts): horizontal.
    void textRotated(int x, int y, uint8_t font, uint32_t c, const char* s, int quarterTurns = 1,
                     uint16_t opt = 0);
    // Several turned strings in one font, colour and turn inside one
    // SAVE_CONTEXT / transform (~14 words once instead of per string); the
    // glyphs land where textRotated would put them.
    struct RotatedText {
        int x, y;
        const char* s;
        uint16_t opt;
    };
    void textRotatedRun(const RotatedText* t, int n, uint8_t font, uint32_t c, int quarterTurns = 1);
    // CMD_GRADIENT from c0 at (x0, y0) to c1 at (x1, y1) over the current
    // clip (spec 20 §1.5). Fallback displays: a solid fill.
    void gradient(int x0, int y0, int x1, int y1, uint32_t c0, uint32_t c1);
    // RAM_DL words one CMD_GRADIENT expands to on the FT81x (it uploads 5).
    // Measured on a Tracker (telemetry REG_CMD_DL, the same page in the
    // default and the Monochrome theme, ~45 frames each): Track Mixer 17
    // gradients 1794 vs 1140 words, Global Mixer 20 gradients 1754 vs 1085
    // -> ~23 words per gradient. Counted as 24 so the budgets and the RAM_DL
    // guard see what the device builds (at 5, gradient-heavy pages passed
    // every host check and faulted on hardware).
    static constexpr uint32_t kGradientDlWords = 24;
    // Box x, y, w, h filled with a gradient c0 -> c1, top to bottom (vertical)
    // or left to right: SAVE_CONTEXT, scissor, CMD_GRADIENT, RESTORE_CONTEXT.
    void gradientRect(int x, int y, int w, int h, uint32_t c0, uint32_t c1, bool vertical = true);
    // 0.5-px line (LINE_WIDTH 8, spec 20 §1.5); LINE_WIDTH returns to 16.
    void hairline(int x0, int y0, int x1, int y1, uint32_t c);
    // Filled rectangle x, y, w, h with corner radius r px (RECTS with
    // LINE_WIDTH r * 16); r <= 1 is a plain fill. LINE_WIDTH returns to 16.
    void roundRect(int x, int y, int w, int h, int r, uint32_t c);
    // Filled circle of radius r px centred on pixel (cx, cy) (POINTS);
    // POINT_SIZE returns to 16.
    void point(int cx, int cy, float r, uint32_t c);
    // Icon `id` in the kIconSize box at top-left (x, y).
    void icon(int x, int y, IconId id, uint32_t colour);

    void clip(int x, int y, int w, int h);
    void noClip();
    // Horizontal/vertical value bar with frame; frac 0..1. bipolar: fill from the middle.
    void vbar(int x, int y, int w, int h, float frac, uint32_t fillC, uint32_t frameC, bool bipolar = false);
    void hbar(int x, int y, int w, int h, float frac, uint32_t fillC, uint32_t frameC);

    // Batched rectangles: one BEGIN/END for many rects (2 vertices each).
    void rectsBegin(uint32_t c);
    void rectsAdd(int x, int y, int w, int h);
    void rectsEnd();

    // Draws the block `draw` (a callable that draws through this Gfx and has
    // no side effects; it may run up to twice per frame) through fragment
    // slot `slot`: CMD_APPEND of the stored copy when its hash matches, else
    // inline (capturing it when it was stable). Without a cache or a display
    // that supports fragments it simply calls draw().
    // Every form starts and ends synced (no open BEGIN(BITMAPS), translate
    // 0, no known colour).
    template <class F>
    void cached(uint8_t slot, F&& draw) {
        if (!probeBegin(slot)) {    // syncs the display first
            draw();
            sync();
            cur_ = kNoColor;
            return;
        }
        draw();
        sync();                        // into the hasher
        if (probeEnd(slot)) return;    // appended
        const bool cap = captureBegin(slot);
        draw();
        sync();                        // the END belongs to the fragment
        captureEnd(slot, cap);
    }

    // Display-list words uploaded through the command FIFO this frame
    // (approximate; an appended fragment counts 4).
    uint32_t dlWords() const { return words_; }
    // Display-list words of the whole frame including appended fragments
    // (approximate; FT81x RAM_DL is 2048 words).
    uint32_t dlTotalWords() const { return words_ + appended_; }

    // RAM_DL guard (docs/VERIFY_1.9.5.md §6). With a limit set, a primitive
    // that would take the frame (dlTotalWords(): appended fragments included)
    // past `limit` words (minus a small reserve
    // for closing the open primitive, the translate reset and DISPLAY) is
    // dropped instead of sent, so an oversized frame can never fault the
    // FT81x coprocessor; dropped() counts them. 0 = no limit.
    void setWordLimit(uint32_t limit) { limit_ = limit; }
    uint32_t dropped() const { return dropped_; }
    // Low-detail rendering (second-level fallback): AA halos / soft edges
    // (alpha <= kLowDetailMaxAlpha) and gradients are skipped.
    void setLowDetail(bool on) { lowDetail_ = on; }
    bool lowDetail() const { return lowDetail_; }
    static constexpr uint8_t kLowDetailMaxAlpha = 120;   // wg::kEdgeA and below
    static constexpr uint32_t kLimitReserve = 8;

private:
    // Glyph runs and icons leave their BEGIN(BITMAPS) open and the
    // VERTEX_TRANSLATE_X in effect, so consecutive runs share them (a dense
    // Pattern page saves ~450 words: BEGIN/END and two translates per cell).
    // Anything else that draws goes through these first: closePrim() ends
    // the open primitive, translate(px) sets VERTEX_TRANSLATE_X only when it
    // changes, sync() does both with translate 0 (VERTEX2F, coprocessor
    // commands and SAVE/RESTORE_CONTEXT need the default context). Cached
    // blocks (cached()) start and end synced, so the inline, captured and
    // appended forms leave the same state.
    void openBitmaps();
    void closePrim();
    void translate(int px);
    void sync();
    // Room for `n` more words (plus a colour / translate / END change)?
    // False: the primitive is dropped (word limit) or skipped (low detail).
    bool admit(uint32_t n);
    bool skipLowDetail() const { return lowDetail_ && alpha_ <= kLowDetailMaxAlpha; }
    void applyAlpha();   // send COLOR_A if the requested alpha is not in effect
    void pushAlpha();    // SAVE_CONTEXT / RESTORE_CONTEXT also save / restore COLOR_A
    void popAlpha();
    static constexpr int kAlphaStack = 4;
    uint8_t alphaStack_[kAlphaStack] = {};
    int alphaDepth_ = 0;

    static constexpr uint32_t kNoColor = 0xFFFFFFFF;
    bool probeBegin(uint8_t slot);
    bool probeEnd(uint8_t slot);
    bool captureBegin(uint8_t slot);
    void captureEnd(uint8_t slot, bool captured);
    // Gfx state a probe (drawing into the hasher) changes.
    struct State {
        uint32_t words = 0, dropped = 0;
        uint8_t alpha = 255, sentAlpha = 255;
        int alphaDepth = 0;
        uint8_t alphaStack[kAlphaStack] = {};
        bool rectsOpen = false;
    };
    State snapshot() const;
    void restore(const State& st);

    IDisplay* d_;                // current sink (the display, or the hasher while probing)
    IDisplay* base_;             // the display
    FragmentCache* cache_;
    uint32_t limit_ = 0;
    uint32_t dropped_ = 0;
    bool lowDetail_ = false;
    bool rectsOpen_ = false;   // rectsBegin admitted (rectsAdd / rectsEnd follow it)
    uint8_t alpha_ = 255;
    uint8_t sentAlpha_ = 255;
    uint32_t cur_ = kNoColor;
    uint32_t words_ = 0;
    uint32_t appended_ = 0;
    bool bitmapsOpen_ = false;
    int translate_ = 0;          // VERTEX_TRANSLATE_X in effect, px
    // Probe state.
    HashDisplay hasher_;
    State saved_;
    uint8_t capturesThisFrame_ = 0;
};

}  // namespace ot
