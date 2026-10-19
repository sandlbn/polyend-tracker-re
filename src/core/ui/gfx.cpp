#include "ui/gfx.h"

#include <cstdio>
#include <cstring>

#include "ui/fonts/fonts.h"

namespace ot {

static_assert(int(kIconCount) == int(fontdata::kIcons.count) && kIconSize == fontdata::kIcons.w,
              "icon sheet (tools/fonts/gen_fonts.py ICONS) must follow IconId");

namespace {
uint8_t gFontSet = 0;
bool gCustomFonts = false;
RomTextMode gRomTextMode = kRomGlyphs;

// FT81x ROM font advance widths (chars 32..126) and cell heights for the
// handles the UI uses; the same values as the simulator's ROM look-alikes
// (sim/third_party/rom_fonts.inc) so layout matches on both.
struct RomMetrics {
    uint8_t handle;
    uint8_t height;
    uint8_t widths[95];
};
const RomMetrics kRomMetrics[] = {
    {18, 16, {8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8}},
    {21, 17, {4,5,6,11,8,12,10,4,5,5,6,11,4,5,4,4,8,8,8,8,8,8,8,8,8,8,4,4,11,11,11,7,13,9,9,9,10,8,7,10,10,4,4,9,7,11,10,10,8,10,9,8,8,10,9,13,9,8,9,5,4,5,11,6,6,8,8,7,8,8,5,8,8,4,4,8,4,13,8,8,8,8,5,7,5,8,8,11,8,8,7,8,4,8,11}},
    {22, 20, {5,6,7,13,10,14,12,4,6,6,8,13,5,5,5,5,10,10,10,10,10,10,10,10,10,10,5,5,13,13,13,8,15,10,10,10,12,9,9,12,11,4,4,10,8,13,11,12,9,12,10,10,9,11,10,15,10,9,10,6,5,6,13,8,8,9,10,8,10,9,5,10,10,4,4,9,4,15,10,9,10,10,6,8,6,10,9,12,9,9,8,10,5,10,13}},
    {23, 22, {5,7,8,14,11,16,13,5,7,7,8,14,5,6,5,6,11,11,11,11,11,11,11,11,11,11,6,6,14,14,14,9,17,12,12,12,13,11,10,13,13,5,5,11,9,15,13,13,10,13,12,11,10,12,12,17,12,10,12,7,6,7,14,8,8,10,11,9,11,10,6,11,11,5,5,10,5,17,11,10,11,11,7,9,7,11,10,14,10,10,9,11,6,11,14}},
    {24, 29, {7,9,10,18,14,21,17,6,9,9,11,18,7,8,7,7,14,14,14,14,14,14,14,14,14,14,7,7,18,18,18,12,22,15,15,15,17,14,13,17,17,6,6,14,12,19,16,17,13,17,15,14,13,16,15,22,15,13,15,9,7,9,18,11,11,13,14,12,14,14,8,14,14,6,6,13,6,21,14,13,14,14,9,11,9,14,13,18,13,13,12,14,7,14,18}},
    {26, 16, {4,5,6,10,8,11,9,3,5,5,6,10,4,4,4,4,8,8,8,8,8,8,8,8,8,8,4,4,10,10,10,6,12,8,8,8,9,8,7,9,9,4,4,8,7,10,9,9,7,9,8,8,7,9,8,12,8,7,8,5,4,5,10,6,6,7,8,7,8,7,4,8,8,3,3,7,3,12,8,7,8,8,5,6,5,8,7,10,7,7,6,8,4,8,10}},
    {27, 20, {5,6,7,13,10,14,12,4,6,6,8,13,5,5,5,5,10,10,10,10,10,10,10,10,10,10,5,5,13,13,13,8,15,10,10,10,12,9,9,12,11,4,4,10,8,13,11,12,9,12,10,10,9,11,10,15,10,9,10,6,5,6,13,8,8,9,10,8,10,9,5,10,10,4,4,9,4,15,10,9,10,10,6,8,6,10,9,12,9,9,8,10,5,10,13}},
    {28, 25, {6,8,9,16,12,18,15,5,7,7,10,16,6,7,6,6,12,12,12,12,12,12,12,12,12,12,6,6,16,16,16,10,19,13,13,13,15,12,11,15,14,6,6,12,11,16,14,15,11,15,13,12,12,14,13,19,13,12,13,7,6,7,16,10,10,12,12,10,12,12,7,12,12,5,5,11,5,19,12,12,12,12,8,10,7,12,11,16,11,11,10,12,6,12,16}},
    {29, 28, {7,9,10,18,14,21,17,6,9,9,11,18,7,8,7,7,14,14,14,14,14,14,14,14,14,14,7,7,18,18,18,12,22,15,15,15,17,14,13,17,17,6,6,14,12,19,16,17,13,17,15,14,13,16,15,22,15,13,15,9,7,9,18,11,11,13,14,12,14,14,8,14,14,6,6,13,6,21,14,13,14,14,9,11,9,14,13,18,13,13,12,14,7,14,18}},
};

const RomMetrics* romMetrics(uint8_t handle) {
    for (const RomMetrics& m : kRomMetrics)
        if (m.handle == handle) return &m;
    return nullptr;
}

// Custom font of a bound handle (kFontHandleBase + 0..4) in the active set.
const fontdata::FontInfo* customFont(uint8_t handle) {
    if (!gCustomFonts || handle < IDisplay::kFontHandleBase ||
        handle >= IDisplay::kFontHandleBase + IDisplay::kFontHandleCount)
        return nullptr;
    return &fontdata::info(gFontSet, handle - IDisplay::kFontHandleBase);
}

// ROM fallback role of a stock font role (gfx.h).
uint8_t fallbackRole(uint8_t role) {
    switch (role) {
        case kFont1: return kFontSmall;
        case kFont2: return kFontMono;
        case kFont3: return kFontText;
        case kFont4: return kFontLarge;
        case kFont5:
        case kFontIcons: return kFontSmall;
        default: return role;
    }
}
}  // namespace

void setFontSet(uint8_t set) { gFontSet = set < kFontSetCount ? set : 0; }
uint8_t fontSet() { return gFontSet; }

bool applyFontSet(IDisplay* d, uint8_t set) {
    setFontSet(set);
    gCustomFonts = d && d->bindFontSet(fontSet());
    return gCustomFonts;
}
bool customFontsBound() { return gCustomFonts; }
void setRomTextMode(RomTextMode m) { gRomTextMode = m; }
RomTextMode romTextMode() { return gRomTextMode; }

uint8_t romFontFor(uint8_t role, uint8_t set) {
    if (role >= kFont1 && role <= kFontIcons) {
        if (gCustomFonts)
            return role == kFontIcons ? IDisplay::kIconHandle : uint8_t(IDisplay::kFontHandleBase + role - kFont1);
        role = fallbackRole(role);
    }
    if (set != 1) return role;   // set 0: roles are the ROM handles themselves
    switch (role) {
        case kFontSmall: return 21;
        case kFontText: return 22;
        case kFontLarge: return 23;
        case kFontTitle: return 24;
        default: return role;    // kFontMono and raw handles
    }
}

uint8_t romFontFor(uint8_t role) { return romFontFor(role, gFontSet); }

void Gfx::beginFrame(uint32_t bg) {
    words_ = 0;
    appended_ = 0;
    capturesThisFrame_ = 0;
    dropped_ = 0;
    alpha_ = sentAlpha_ = 255;
    alphaDepth_ = 0;
    rectsOpen_ = false;
    cur_ = kNoColor;
    bitmapsOpen_ = false;
    translate_ = 0;
    d_ = base_;
    d_->dlStart();
    d_->clearColorRGB(uint8_t(bg >> 16), uint8_t(bg >> 8), uint8_t(bg));
    d_->clear(true, true, true);
    words_ += 2;
}

void Gfx::endFrame() {
    closePrim();
    translate(0);   // no pending COLOR_A: nothing draws with it any more
    d_->swap();
}

void Gfx::openBitmaps() {
    if (bitmapsOpen_) return;
    d_->begin(IDisplay::kBitmaps);
    ++words_;
    bitmapsOpen_ = true;
}

void Gfx::closePrim() {
    if (!bitmapsOpen_) return;
    d_->end();
    ++words_;
    bitmapsOpen_ = false;
}

void Gfx::translate(int px) {
    if (px == translate_) return;
    translate_ = px;
    d_->vertexTranslateX(int32_t(px) * 16);
    ++words_;
}

void Gfx::sync() {
    closePrim();
    translate(0);
    applyAlpha();
}

bool Gfx::admit(uint32_t n) {
    if (skipLowDetail()) return false;
    // Sticky: once something was dropped the rest of the frame is too, so
    // state (scissor, context) never gets out of step with its primitives.
    // dlTotalWords(): appended RAM_G fragments (CMD_APPEND) fill RAM_DL too.
    if (limit_ && (dropped_ || words_ + appended_ + n + 3 + kLimitReserve > limit_)) {
        ++dropped_;
        return false;
    }
    return true;
}

// COLOR_A is sent lazily, right before the next primitive that draws with
// it: "alpha(120) fill alpha(255) alpha(120) fill" costs one COLOR_A, and a
// skipped (low-detail) or dropped primitive costs none.
void Gfx::alpha(uint8_t a) { alpha_ = a; }

void Gfx::applyAlpha() {
    if (alpha_ == sentAlpha_) return;
    d_->colorA(alpha_);
    sentAlpha_ = alpha_;
    ++words_;
}

void Gfx::lineWidth(uint16_t w16) {
    if (limit_ && dropped_) return;
    sync();
    d_->lineWidth(w16);
    ++words_;
}

bool Gfx::saveContext(uint32_t groupWords) {
    if (!admit(groupWords + 2)) return false;
    sync();
    pushAlpha();
    d_->saveContext();
    ++words_;
    return true;
}

void Gfx::restoreContext() {
    // Always sent after an admitted saveContext (inside the reserve).
    sync();
    d_->restoreContext();
    popAlpha();
    ++words_;
    cur_ = kNoColor;
}

void Gfx::pushAlpha() {
    if (alphaDepth_ < kAlphaStack) alphaStack_[alphaDepth_] = sentAlpha_;
    ++alphaDepth_;
}

void Gfx::popAlpha() {
    if (alphaDepth_ > 0 && --alphaDepth_ < kAlphaStack) sentAlpha_ = alphaStack_[alphaDepth_];
}

// ---------------------------------------------------------------------------
// Cached fragments (gfx.h FragmentCache). Every path starts and ends a block
// synced (sync()) with no known colour, so the inline, captured and appended
// forms all emit the same graphics state.

bool Gfx::probeBegin(uint8_t slot) {
    sync();
    if (!cache_ || !cache_->enabled || slot >= FragmentCache::kSlots || d_ != base_ ||
        !base_->fragmentsSupported())
        return false;
    FragmentCache::Slot& s = cache_->slot[slot];
    if (s.skip) {          // hot slot: inline without hashing
        // Afterwards the probes decide again: a capture once kStableProbes
        // agree, another skip period after kHotChanges changes.
        if (--s.skip == 0) s.changes = 0;
        ++cache_->skips;
        cur_ = kNoColor;
        return false;
    }
    hasher_.reset();
    // The probe runs from the current state (words_ keeps counting, so the
    // RAM_DL guard admits exactly what the inline draw would); probeEnd puts
    // it back unless the stored fragment is appended.
    saved_ = snapshot();
    cur_ = kNoColor;
    d_ = &hasher_;
    return true;
}

Gfx::State Gfx::snapshot() const {
    State st;
    st.words = words_;
    st.dropped = dropped_;
    st.alpha = alpha_;
    st.sentAlpha = sentAlpha_;
    st.alphaDepth = alphaDepth_;
    for (int i = 0; i < kAlphaStack; ++i) st.alphaStack[i] = alphaStack_[i];
    st.rectsOpen = rectsOpen_;
    return st;
}

void Gfx::restore(const State& st) {
    words_ = st.words;
    dropped_ = st.dropped;
    alpha_ = st.alpha;
    sentAlpha_ = st.sentAlpha;
    alphaDepth_ = st.alphaDepth;
    for (int i = 0; i < kAlphaStack; ++i) alphaStack_[i] = st.alphaStack[i];
    rectsOpen_ = st.rectsOpen;
}

bool Gfx::probeEnd(uint8_t slot) {
    FragmentCache::Slot& s = cache_->slot[slot];
    d_ = base_;
    cur_ = kNoColor;
    const uint64_t h = hasher_.h;
    // fragmentWords() == 0: the display lost the slot (e.g. a reset).
    const uint32_t fw = s.has && s.stored == h ? base_->fragmentWords(slot) : 0;
    if (fw) {
        // The fragment is the probed stream: keep the probe's end state
        // (alpha, guard drops), count the append instead of the inline words.
        base_->fragmentAppend(slot);
        words_ = saved_.words + 4;     // CMD_APPEND, ptr, num (+ END on the FT81x)
        appended_ += fw;
        s.changes = 0;
        ++cache_->hits;
        return true;
    }
    ++cache_->misses;
    restore(saved_);
    return false;
}

bool Gfx::captureBegin(uint8_t slot) {
    FragmentCache::Slot& s = cache_->slot[slot];
    const uint64_t h = hasher_.h;
    // Capture only a block drawn identically in the last kStableProbes
    // probed frames (a frame redrawn for another reason does not count as
    // stable content) and that the display did not refuse before.
    if (s.seen == h) {
        if (s.stable < 255) ++s.stable;
        s.changes = 0;
    } else {
        s.seen = h;
        s.stable = 1;
        if (s.changes < 255) ++s.changes;
    }
    const bool cap = s.stable >= FragmentCache::kStableProbes && s.refused != h &&
                     capturesThisFrame_ < FragmentCache::kMaxCapturesPerFrame;
    if (!cap && s.changes >= FragmentCache::kHotChanges) s.skip = FragmentCache::kHotSkip;
    if (cap) {
        s.has = false;
        ++capturesThisFrame_;
        base_->fragmentBegin(slot);
    }
    return cap;
}

void Gfx::captureEnd(uint8_t slot, bool captured) {
    cur_ = kNoColor;
    if (!captured) return;
    FragmentCache::Slot& s = cache_->slot[slot];
    if (base_->fragmentEnd(slot)) {
        s.has = true;
        s.stored = hasher_.h;
        ++cache_->captures;
    } else {
        s.refused = hasher_.h;
    }
}

void Gfx::color(uint32_t c) {
    if (c == cur_) return;
    cur_ = c;
    d_->colorRGB(uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c));
    ++words_;
}

void Gfx::fill(int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0 || x >= kScreenW || y >= kScreenH || x + w <= 0 || y + h <= 0 || !admit(4)) return;
    sync();
    color(c);
    d_->rect(int16_t(x), int16_t(y), int16_t(x + w - 1), int16_t(y + h - 1));
    words_ += 4;
}

void Gfx::frame(int x, int y, int w, int h, uint32_t c, int lw) {
    fill(x, y, w, lw, c);
    fill(x, y + h - lw, w, lw, c);
    fill(x, y, lw, h, c);
    fill(x + w - lw, y, lw, h, c);
}

void Gfx::rectsBegin(uint32_t c) {
    rectsOpen_ = admit(3);
    if (!rectsOpen_) return;
    sync();
    color(c);
    d_->begin(IDisplay::kRects);
    ++words_;
}

void Gfx::rectsAdd(int x, int y, int w, int h) {
    // Off-screen rects (outline rings below row 479) cost words, show nothing.
    if (!rectsOpen_) return;
    if (w > 0 && h > 0 && (x >= kScreenW || y >= kScreenH || x + w <= 0 || y + h <= 0)) return;
    if (!admit(2)) return;
    // Pixel-centre vertices (IDisplay::rect): crisp edges on the FT81x.
    d_->vertex2f(int32_t(x) * 16 + 8, int32_t(y) * 16 + 8);
    d_->vertex2f(int32_t(x + w - 1) * 16 + 8, int32_t(y + h - 1) * 16 + 8);
    words_ += 2;
}

void Gfx::rectsEnd() {
    if (!rectsOpen_) return;
    rectsOpen_ = false;
    d_->end();
    ++words_;
}

void Gfx::hline(int x, int y, int w, uint32_t c) { fill(x, y, w, 1, c); }

void Gfx::point(int cx, int cy, float r, uint32_t c) {
    if (!admit(5)) return;
    sync();
    color(c);
    d_->pointSize(uint16_t(r * 16.0f + 0.5f));
    d_->begin(IDisplay::kPoints);
    d_->vertex2f(int32_t(cx) * 16 + 8, int32_t(cy) * 16 + 8);
    d_->end();
    d_->pointSize(16);
    words_ += 5;
}
void Gfx::vline(int x, int y, int h, uint32_t c) { fill(x, y, 1, h, c); }

void Gfx::line(int x0, int y0, int x1, int y1, uint32_t c) {
    if (!admit(4)) return;
    sync();
    color(c);
    d_->line(int16_t(x0), int16_t(y0), int16_t(x1), int16_t(y1));
    words_ += 4;
}

void Gfx::text(int x, int y, uint8_t font, uint32_t c, const char* s, uint16_t opt) {
    if (!s || !*s || !admit(4 + uint32_t(std::strlen(s)))) return;
    sync();   // CMD_TEXT draws in the current context (translate included)
    color(c);
    d_->text(int16_t(x), int16_t(y), romFontFor(font), opt, s);
    words_ += 4 + uint32_t(std::strlen(s));
}

int textWidth(uint8_t font, const char* s, int len) {
    if (!s) return 0;
    const uint8_t handle = romFontFor(font);
    const fontdata::FontInfo* cf = customFont(handle);
    const RomMetrics* m = cf ? nullptr : romMetrics(handle);
    int w = 0;
    for (int i = 0; s[i] && (len < 0 || i < len); ++i) {
        const uint8_t ch = uint8_t(s[i]);
        if (cf) w += fontdata::advance(*cf, ch);
        else if (gCustomFonts && handle == IDisplay::kIconHandle) w += kIconSize;
        else if (!m) w += 8;
        else if (ch >= 32 && ch < 127) w += m->widths[ch - 32];
    }
    return w;
}

int fontHeight(uint8_t font) {
    const uint8_t handle = romFontFor(font);
    if (const fontdata::FontInfo* cf = customFont(handle)) return cf->h;
    if (gCustomFonts && handle == IDisplay::kIconHandle) return kIconSize;
    const RomMetrics* m = romMetrics(handle);
    return m ? m->height : 16;
}

void Gfx::glyphRun(int x, int y, uint8_t font, const char* s, uint32_t c, int adv) {
    if (!s || !*s) return;
    const uint8_t handle = romFontFor(font);
    const fontdata::FontInfo* cf = customFont(handle);
    // Without the custom fonts the run is drawn with the FT81x ROM font
    // bitmaps (handles 16-31, cell = character code) and the romMetrics
    // advances (docs/OPTIMIZATION_2X.md OP-6): one VERTEX2II per glyph
    // instead of a CMD_TEXT (4 + n words) per run or per cell.
    const bool rom = !gCustomFonts;
    const bool romText = rom && gRomTextMode != kRomGlyphs;
    auto drawable = [cf, rom](uint8_t ch) {
        return ch < 127 && (rom ? ch > ' ' : ch != ' ' && (!cf || ch >= cf->first));
    };
    // One VERTEX_TRANSLATE_X for the whole run when its glyphs fit one
    // 512-px window (the one in effect first): a run across x 512 (Pattern
    // Fx1 cells of the third track) no longer toggles it twice per cell.
    int lo = 912, hi = -1, first = 912;
    for (int i = 0, pen = x; s[i]; ++i) {
        if (drawable(uint8_t(s[i]))) {
            if (pen < first) first = pen;
            if (pen >= 0 && pen < 912) {
                if (pen < lo) lo = pen;
                hi = pen;
            }
        }
        pen += adv > 0 ? adv : ot::textWidth(font, s + i, 1);
    }
    if (romText || (rom && (handle < 16 || handle > 31 || y < 0 || y > 511 || first < 0))) {
        // Not a ROM font handle, or a glyph clipped at the top / left edge
        // (VERTEX2II takes unsigned coordinates): the coprocessor's CMD_TEXT.
        if (adv <= 0) {
            text(x, y, font, c, s);
            return;
        }
        char one[2] = {0, 0};
        for (int i = 0; s[i]; ++i) {
            if (s[i] == ' ') continue;
            one[0] = s[i];
            text(x + i * adv, y, font, c, one);
        }
        return;
    }
    if (y < 0 || y > 511 || !admit(2)) return;   // VERTEX2II y is 9 bits
    color(c);
    applyAlpha();
    openBitmaps();                         // shared with the previous run (closePrim ends it)
    if (hi < 0) return;
    d_->textHint(int16_t(x), int16_t(y), font, s);   // [I] for the display command export
    int base = -1;
    if (lo >= translate_ && hi - translate_ <= 511) base = translate_;
    else if (hi <= 511) base = 0;
    else if (lo >= 400) base = 400;
    int pen = x;
    for (int i = 0; s[i]; ++i) {
        const uint8_t ch = uint8_t(s[i]);
        const int step = adv > 0 ? adv : ot::textWidth(font, s + i, 1);
        if (drawable(ch) && pen >= 0 && pen < 912) {
            if (!admit(2)) return;
            translate(base >= 0 ? base : (pen < 512 ? 0 : 400));   // pen - base in 0..511 up to x = 911
            d_->vertex2ii(uint16_t(pen - translate_), uint16_t(y), handle, uint8_t(ch & 0x7F));
            ++words_;
        }
        pen += step;
    }
}

void Gfx::textRotated(int x, int y, uint8_t font, uint32_t c, const char* s, int quarterTurns, uint16_t opt) {
    if (!s || !*s) return;
    const uint8_t handle = romFontFor(font);
    const fontdata::FontInfo* cf = customFont(handle);
    const int q = ((quarterTurns % 4) + 4) % 4;
    if (!cf || q == 0) {   // fallback (ROM fonts) / no turn: horizontal text
        text(x, y, font, c, s, opt);
        return;
    }
    // Box of the turned text: W x H with W along the reading direction.
    const int W = ot::textWidth(font, s), H = cf->h, cw = cf->w;
    const bool side = q != 2;              // reads vertically
    const int bw = side ? H : W, bh = side ? W : H;
    if (opt & IDisplay::kOptCenterX) x -= bw / 2;
    else if (opt & IDisplay::kOptRightX) x -= bw;
    if (opt & IDisplay::kOptCenterY) y -= bh / 2;
    // BITMAP_TRANSFORM: source (u, v) of the drawn pixel (px, py) from the
    // vertex, NEAREST sampling at pixel centres (8.8 / 1/256 px).
    //   q 1 (CCW, bottom to top): u = cw - py, v = px
    //   q 2 (upside down):        u = cw - px, v = H - py
    //   q 3 (CW, top to bottom):  u = py,      v = H - px
    int32_t A = 0, B = 0, C = 0, D = 0, E = 0, F = 0;
    if (q == 1) { B = -256; C = cw * 256; D = 256; }
    else if (q == 2) { A = -256; C = cw * 256; E = -256; F = H * 256; }
    else { B = 256; D = -256; F = H * 256; }
    if (!admit(14 + 2 * uint32_t(std::strlen(s)))) return;
    sync();   // the saved context (and the restored one) has translate 0
    color(c);
    d_->saveContext();
    d_->bitmapSize(handle, uint16_t(side ? H : cw), uint16_t(side ? cw : H));
    d_->bitmapTransform(A, B, C, D, E, F);
    d_->begin(IDisplay::kBitmaps);
    words_ += 10;
    int base = 0;
    d_->textHint(int16_t(x), int16_t(y), font, s);   // [I] display command export
    int pen = 0;
    for (int i = 0; s[i]; ++i) {
        const uint8_t ch = uint8_t(s[i]);
        const int step = fontdata::advance(*cf, ch);
        if (ch != ' ' && ch < 127 && ch >= cf->first) {
            int vx, vy;   // vertex = top-left of the turned cell
            if (q == 1) { vx = x; vy = y + W - pen - cw; }
            else if (q == 2) { vx = x + W - pen - cw; vy = y; }
            else { vx = x; vy = y + pen; }
            if (vx >= 0 && vx < 912 && vy >= 0 && vy <= 511) {
                const int want = vx < 512 ? 0 : 400;
                if (want != base) {
                    base = want;
                    d_->vertexTranslateX(int32_t(base) * 16);
                    ++words_;
                }
                d_->vertex2ii(uint16_t(vx - base), uint16_t(vy), handle, ch);
                ++words_;
            }
        }
        pen += step;
    }
    d_->end();
    d_->restoreContext();   // transform and translate
    d_->bitmapSize(handle, cw, uint16_t(H));
    words_ += 4;
    cur_ = kNoColor;
}

void Gfx::gradient(int x0, int y0, int x1, int y1, uint32_t c0, uint32_t c1) {
    if (lowDetail_ || !admit(5)) return;
    sync();
    d_->gradient(int16_t(x0), int16_t(y0), c0 & 0xFFFFFFu, int16_t(x1), int16_t(y1), c1 & 0xFFFFFFu);
    cur_ = kNoColor;   // fallback displays change COLOR_RGB
    words_ += 5;
}

void Gfx::gradientRect(int x, int y, int w, int h, uint32_t c0, uint32_t c1, bool vertical) {
    if (w <= 0 || h <= 0 || lowDetail_ || !admit(9)) return;
    sync();
    d_->saveContext();
    ++words_;
    clip(x, y, w, h);
    if (vertical) gradient(x, y, x, y + h - 1, c0, c1);
    else gradient(x, y, x + w - 1, y, c0, c1);
    d_->restoreContext();
    ++words_;
    cur_ = kNoColor;
}

void Gfx::hairline(int x0, int y0, int x1, int y1, uint32_t c) {
    if (!admit(6)) return;
    d_->lineWidth(8);
    line(x0, y0, x1, y1, c);
    d_->lineWidth(16);
    words_ += 2;
}

void Gfx::roundRect(int x, int y, int w, int h, int r, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 1) {
        fill(x, y, w, h, c);
        return;
    }
    if (!admit(6)) return;
    // RECTS extend LINE_WIDTH - 1 px beyond their vertices (pixel centres).
    sync();
    color(c);
    d_->lineWidth(uint16_t(r * 16));
    d_->rect(int16_t(x + r - 1), int16_t(y + r - 1), int16_t(x + w - r), int16_t(y + h - r));
    d_->lineWidth(16);
    words_ += 6;
}

void Gfx::icon(int x, int y, IconId id, uint32_t c) {
    constexpr int S = kIconSize;
    if (gCustomFonts && id < kIconCount && id < fontdata::kIcons.count) {
        // Icon sheet cell `id` on kIconHandle (our own drawings, spec 20 §5).
        if (x < 0 || x >= 912 || y < 0 || y > 511 || !admit(2)) return;
        color(c);
        applyAlpha();
        openBitmaps();
        if (x < translate_ || x - translate_ > 511) translate(x < 512 ? 0 : 400);
        d_->vertex2ii(uint16_t(x - translate_), uint16_t(y), IDisplay::kIconHandle, uint8_t(id));
        ++words_;
        return;
    }
    switch (id) {
        case kIconPlay:
        case kIconArrowRight:
        case kIconArrowLeft: {
            // Horizontal triangle: play fills the box, arrows are 8 px tall.
            const int h = id == kIconPlay ? S : 8, oy = y + (S - h) / 2, half = h / 2;
            rectsBegin(c);
            for (int i = 0; i < h; ++i) {
                const int w = (i < half ? i : h - 1 - i) * (id == kIconPlay ? 2 : 1) + 1;
                rectsAdd(id == kIconArrowLeft ? x + S - 2 - w : (id == kIconPlay ? x : x + 2), oy + i, w, 1);
            }
            rectsEnd();
            break;
        }
        case kIconArrowUp:
        case kIconArrowDown: {
            rectsBegin(c);
            for (int i = 0; i < 4; ++i) {
                const int w = 2 * i + 2, row = id == kIconArrowUp ? y + 4 + i : y + 7 - i;
                rectsAdd(x + S / 2 - 1 - i, row, w, 1);
            }
            rectsEnd();
            break;
        }
        case kIconStop: fill(x + 1, y + 1, S - 2, S - 2, c); break;
        case kIconPause:
            fill(x + 2, y + 1, 3, S - 2, c);
            fill(x + S - 5, y + 1, 3, S - 2, c);
            break;
        case kIconRec:
            if (!admit(5)) break;
            sync();
            color(c);
            d_->pointSize(uint16_t(5 * 16));
            d_->begin(IDisplay::kPoints);
            d_->vertex2f(int32_t(x) * 16 + S * 8, int32_t(y) * 16 + S * 8);
            d_->end();
            d_->pointSize(16);
            words_ += 5;
            break;
        case kIconLoop:
            frame(x, y + 3, S, 7, c);
            fill(x + 6, y + 1, 1, 5, c);   // arrow head on the top edge
            fill(x + 7, y + 2, 1, 3, c);
            break;
        case kIconBattery:
            frame(x, y + 3, S - 2, 7, c);
            fill(x + S - 2, y + 5, 2, 3, c);
            break;
        case kIconFolder:
            fill(x, y + 2, 5, 2, c);
            fill(x, y + 4, S, 7, c);
            break;
        default: break;
    }
}

void Gfx::textf(int x, int y, uint8_t font, uint32_t c, uint16_t opt, const char* fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    text(x, y, font, c, buf, opt);
}

void Gfx::clip(int x, int y, int w, int h) {
    if (limit_ && dropped_) return;   // nothing follows it any more
    closePrim();   // the scissor is state, but keep primitives inside one clip
    d_->scissorXY(uint16_t(x < 0 ? 0 : x), uint16_t(y < 0 ? 0 : y));
    d_->scissorSize(uint16_t(w), uint16_t(h));
    words_ += 2;
}

void Gfx::noClip() { clip(0, 0, 2048, 2048); }

void Gfx::vbar(int x, int y, int w, int h, float frac, uint32_t fillC, uint32_t frameC, bool bipolar) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    frame(x, y, w, h, frameC);
    const int inner = h - 4;
    if (bipolar) {
        const int mid = y + 2 + inner / 2;
        const int v = int((frac - 0.5f) * float(inner));
        if (v > 0) fill(x + 2, mid - v, w - 4, v, fillC);
        else if (v < 0) fill(x + 2, mid, w - 4, -v, fillC);
        hline(x + 2, mid, w - 4, frameC);
    } else {
        const int v = int(frac * float(inner));
        fill(x + 2, y + 2 + inner - v, w - 4, v, fillC);
    }
}

void Gfx::hbar(int x, int y, int w, int h, float frac, uint32_t fillC, uint32_t frameC) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    frame(x, y, w, h, frameC);
    fill(x + 2, y + 2, int(frac * float(w - 4)), h - 4, fillC);
}

}  // namespace ot
