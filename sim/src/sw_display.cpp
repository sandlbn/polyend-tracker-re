#include "sw_display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "jpeg_decode.h"
#include "ui/fonts/fonts.h"
#include "ui/gfx.h"

namespace sim {

SwDisplay::SwDisplay() : back_(kW, kH), ramg_(kRamGSize, 0), front_(size_t(kW) * kH, 0xFF000000u) { back_.resetClip(); }

void SwDisplay::warnOnce(int key, const char* msg, int v) {
    if (key < 0 || key >= 64 || warned_[key]) return;
    warned_[key] = true;
    std::fprintf(stderr, "sim display: %s (%d) -- ignored (logged once)\n", msg, v);
}

uint64_t SwDisplay::busyNowUs() const {
    const uint32_t us = busyClock_->micros();
    lastUs_ += uint32_t(us - uint32_t(lastUs_));
    return lastUs_;
}

void SwDisplay::busyTickLocked() const {
    constexpr uint64_t kVsyncUs = 18480;   // ft81xFrameMilliHz: 54.112 Hz
    if (!pending_ || busyNowUs() < (pendingAtUs_ / kVsyncUs + 1) * kVsyncUs) return;
    front_.swap(pendingFrame_);
    pending_ = false;
}

bool SwDisplay::frameReady() {
    if (!busyClock_) return true;
    std::lock_guard<std::mutex> lk(frontMu_);
    busyTickLocked();
    return !pending_;
}

void SwDisplay::dlStart() {
    if (busyClock_) {
        // Ft81xDisplay::dlStart blocks until the previous swap took effect.
        std::lock_guard<std::mutex> lk(frontMu_);
        if (pending_) {
            front_.swap(pendingFrame_);
            pending_ = false;
        }
    }
    dl_ = appends_ = 0;
    recSlot_ = -1;
    // Fresh display list: default graphics context (FT81x reset values).
    clearRgb_ = 0;
    rgb_ = 0xFFFFFF;
    a_ = 255;
    lineW_ = 16;
    pointS_ = 16;
    scX_ = scY_ = 0;
    scW_ = scH_ = 2048;
    translateX_ = 0;
    static const int32_t kIdentity[6] = {256, 0, 0, 0, 256, 0};
    std::memcpy(xform_, kIdentity, sizeof(xform_));
    ctxDepth_ = 0;
    prim_ = 0;
    nvert_ = 0;
    back_.resetClip();
    back_.clearRect(0);
    applyColor();
    if (fontsBound_) {
        setupFontHandles();   // as the FT81x driver after CMD_DLSTART
        // Its RAM_DL words: BITMAP_HANDLE/SOURCE/LAYOUT_H/LAYOUT/SIZE_H/SIZE
        // per font handle and for the icon handle (CMD_SETFONT adds none).
        dl_ += kFontSetupDlWords;
    }
}

namespace {
uint32_t rd32(const std::vector<uint8_t>& m, uint32_t a) {
    return uint32_t(m[a]) | (uint32_t(m[a + 1]) << 8) | (uint32_t(m[a + 2]) << 16) | (uint32_t(m[a + 3]) << 24);
}
}  // namespace

bool SwDisplay::bindFontSet(int set) {
    if (!fontsUploaded_) fontsUploaded_ = writeRamG(ot::fontdata::kRamGBase, ot::fontdata::kBlob, ot::fontdata::kBlobSize);
    fontSet_ = set == 1 ? 1 : 0;
    fontsBound_ = fontsUploaded_ && !romOnly_;
    return fontsBound_;
}

void SwDisplay::setupFontHandles() {
    // BITMAP_SOURCE/LAYOUT/SIZE + CMD_SETFONT per handle, read back from the
    // metric blocks in RAM_G (what the coprocessor sees on the device).
    for (int i = 0; i < kFontHandleCount; ++i) {
        const ot::fontdata::FontInfo& f = ot::fontdata::info(fontSet_, i);
        const uint32_t m = ot::fontdata::kRamGBase + f.metric;
        Handle& h = handles_[kFontHandleBase + i];
        if (rd32(ramg_, m + 128) != 2) warnOnce(8, "custom font is not L4", int(rd32(ramg_, m + 128)));
        h.valid = true;
        h.metric = m;
        h.stride = uint16_t(rd32(ramg_, m + 132));
        h.sizeW = uint16_t(rd32(ramg_, m + 136));
        h.layoutH = h.sizeH = uint16_t(rd32(ramg_, m + 140));
        h.source = rd32(ramg_, m + 144);
    }
    const ot::fontdata::IconSheet& ic = ot::fontdata::kIcons;
    Handle& h = handles_[kIconHandle];
    h.valid = true;
    h.metric = 0;
    h.source = ot::fontdata::kRamGBase + ic.offset;
    h.stride = ic.stride;
    h.sizeW = ic.w;
    h.layoutH = h.sizeH = ic.h;
}

void SwDisplay::bitmapTransform(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) {
    rec(21, a, b, c, d, e, f);
    xform_[0] = a; xform_[1] = b; xform_[2] = c;
    xform_[3] = d; xform_[4] = e; xform_[5] = f;
}

void SwDisplay::bitmapSize(uint8_t handle, uint16_t w, uint16_t h) {
    rec(22, handle, w, h);
    if (handle >= 16 || !handles_[handle].valid) {
        warnOnce(9, "bitmapSize on a handle without a bitmap", handle);
        return;
    }
    handles_[handle].sizeW = w;
    handles_[handle].sizeH = h;
}

void SwDisplay::customGlyph(int x, int y, uint8_t handle, uint8_t cell) {
    const Handle& h = handles_[handle];
    const uint32_t cellBytes = uint32_t(h.stride) * h.layoutH;
    const uint32_t addr = h.source + cell * cellBytes;
    if (addr + cellBytes > kRamGSize) return;
    back_.blitL4(x, y, ramg_.data() + addr, h.stride, h.stride * 2, h.layoutH, h.sizeW, h.sizeH, xform_);
}

void SwDisplay::applyClip() {
    back_.setClip(scX_, scY_, int(scX_) + scW_, int(scY_) + scH_);
}
void SwDisplay::applyColor() { back_.setColor(rgb_, a_); }

void SwDisplay::clearColorRGB(uint8_t r, uint8_t g, uint8_t b) { rec(1, r, g, b); clearRgb_ = (uint32_t(r) << 16) | (g << 8) | b; }

void SwDisplay::clear(bool color, bool s, bool t) {
    rec(2, color, s, t);
    if (!color) return;
    applyClip();
    back_.clearRect(clearRgb_);
}

void SwDisplay::colorRGB(uint8_t r, uint8_t g, uint8_t b) {
    rec(3, r, g, b);
    rgb_ = (uint32_t(r) << 16) | (g << 8) | b;
    applyColor();
}
void SwDisplay::colorA(uint8_t a) {
    rec(4, a);
    a_ = a;
    applyColor();
}
void SwDisplay::lineWidth(uint16_t w) { rec(5, w); lineW_ = w; }
void SwDisplay::pointSize(uint16_t s) { rec(6, s); pointS_ = s; }
void SwDisplay::scissorXY(uint16_t x, uint16_t y) { rec(7, x, y); scX_ = x; scY_ = y; }
void SwDisplay::scissorSize(uint16_t w, uint16_t h) { rec(8, w, h); scW_ = w; scH_ = h; }

void SwDisplay::begin(Prim prim) {
    rec(9, prim);
    if (prim < kBitmaps || prim > kRects) warnOnce(1, "unsupported BEGIN primitive", prim);
    prim_ = prim;
    nvert_ = 0;
}
void SwDisplay::end() {
    rec(10);
    prim_ = 0;
    nvert_ = 0;
}

void SwDisplay::vertex2f(int32_t x16, int32_t y16) {
    rec(11, x16, y16);
    vertex(float(x16 + translateX_) / 16.0f, float(y16) / 16.0f, 0, 0, false);
}
void SwDisplay::vertex2ii(uint16_t x, uint16_t y, uint8_t handle, uint8_t cell) {
    rec(12, x, y, handle, cell);
    vertex(float(x) + float(translateX_) / 16.0f, float(y), handle, cell, true);
}

void SwDisplay::saveContext() {
    rec(18);
    if (ctxDepth_ >= 4) {
        warnOnce(6, "SAVE_CONTEXT stack overflow", ctxDepth_);
        return;
    }
    Context& c = ctxStack_[ctxDepth_++];
    c = {clearRgb_, rgb_, a_, lineW_, pointS_, scX_, scY_, scW_, scH_, translateX_, {}};
    std::memcpy(c.xform, xform_, sizeof(xform_));
}

void SwDisplay::restoreContext() {
    rec(19);
    if (ctxDepth_ <= 0) {
        warnOnce(7, "RESTORE_CONTEXT without SAVE_CONTEXT", 0);
        return;
    }
    const Context& c = ctxStack_[--ctxDepth_];
    clearRgb_ = c.clearRgb;
    rgb_ = c.rgb;
    a_ = c.a;
    lineW_ = c.lineW;
    pointS_ = c.pointS;
    scX_ = c.scX;
    scY_ = c.scY;
    scW_ = c.scW;
    scH_ = c.scH;
    translateX_ = c.translateX;
    std::memcpy(xform_, c.xform, sizeof(xform_));
    applyColor();
}

void SwDisplay::gradient(int16_t x0, int16_t y0, uint32_t rgb0, int16_t x1, int16_t y1, uint32_t rgb1) {
    rec(20, x0, y0, int32_t(rgb0), x1, y1, int32_t(rgb1));
    // Colour = rgb0 + t * (rgb1 - rgb0), t = projection of the pixel centre
    // on (x0, y0) -> (x1, y1), clamped to 0..1; fills scissor ∩ screen.
    const int cx0 = std::max<int>(scX_, 0), cy0 = std::max<int>(scY_, 0);
    const int cx1 = std::min<int>(scX_ + scW_, kW), cy1 = std::min<int>(scY_ + scH_, kH);
    const float dx = float(x1 - x0), dy = float(y1 - y0);
    const float len2 = dx * dx + dy * dy;
    const int r0 = int((rgb0 >> 16) & 255), g0 = int((rgb0 >> 8) & 255), b0 = int(rgb0 & 255);
    const int r1 = int((rgb1 >> 16) & 255), g1 = int((rgb1 >> 8) & 255), b1 = int(rgb1 & 255);
    uint32_t* px = back_.pixels();
    for (int y = cy0; y < cy1; ++y) {
        for (int x = cx0; x < cx1; ++x) {
            float t = len2 > 0 ? ((float(x) - float(x0)) * dx + (float(y) - float(y0)) * dy) / len2 : 1.0f;
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            const uint32_t r = uint32_t(float(r0) + t * float(r1 - r0) + 0.5f);
            const uint32_t g = uint32_t(float(g0) + t * float(g1 - g0) + 0.5f);
            const uint32_t b = uint32_t(float(b0) + t * float(b1 - b0) + 0.5f);
            px[size_t(y) * kW + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void SwDisplay::edgeSegment(float x0, float y0, float x1, float y1) {
    // Fill from the segment to the screen edge (R: right, L: left, A: above, B: below).
    const bool vertical = prim_ == kEdgeStripR || prim_ == kEdgeStripL;
    if (vertical) {
        if (y1 < y0) { std::swap(x0, x1); std::swap(y0, y1); }
        for (int y = int(y0 + 0.5f); y < int(y1 + 0.5f); ++y) {
            const float t = y1 > y0 ? (float(y) + 0.5f - y0) / (y1 - y0) : 0.0f;
            const int x = int(x0 + t * (x1 - x0) + 0.5f);
            if (prim_ == kEdgeStripR) back_.fillRect(x, y, kW, y + 1);
            else back_.fillRect(0, y, x, y + 1);
        }
    } else {
        if (x1 < x0) { std::swap(x0, x1); std::swap(y0, y1); }
        for (int x = int(x0 + 0.5f); x < int(x1 + 0.5f); ++x) {
            const float t = x1 > x0 ? (float(x) + 0.5f - x0) / (x1 - x0) : 0.0f;
            const int y = int(y0 + t * (y1 - y0) + 0.5f);
            if (prim_ == kEdgeStripB) back_.fillRect(x, y, x + 1, kH);
            else back_.fillRect(x, 0, x + 1, y);
        }
    }
}

void SwDisplay::vertex(float x, float y, uint8_t handle, uint8_t cell, bool ii) {
    applyClip();
    const float lr = float(lineW_) / 16.0f;
    switch (prim_) {
        case kBitmaps:
            if (handle < 16 && handles_[handle].valid) customGlyph(int(x), int(y), handle, cell);
            else if (romFont(handle)) back_.glyph(int(x), int(y), handle, cell);
            else warnOnce(32 + (handle & 31), "BITMAPS with non-ROM-font handle", handle);
            (void)ii;
            break;
        case kPoints:
            back_.fillCircle(x, y, float(pointS_) / 16.0f);
            break;
        case kLines:
            if (nvert_ & 1) back_.fillCapsule(vx_, vy_, x, y, lr > 0.5f ? lr - 0.5f : 0.5f);
            break;
        case kLineStrip:
            if (nvert_) back_.fillCapsule(vx_, vy_, x, y, lr > 0.5f ? lr - 0.5f : 0.5f);
            break;
        case kEdgeStripR:
        case kEdgeStripL:
        case kEdgeStripA:
        case kEdgeStripB:
            if (nvert_) edgeSegment(vx_, vy_, x, y);
            break;
        case kRects:
            if (nvert_ & 1) {
                // FT81x: the area within LINE_WIDTH / 2 of the vertex box,
                // anti-aliased, vertices in continuous pixel coordinates
                // (pixel k spans [k, k+1)). Pixel-centre vertices (k + 0.5,
                // IDisplay::rect) give crisp edges; corner vertices half-cover
                // every edge pixel, as on the device.
                float x0 = vx_, y0 = vy_, x1 = x, y1 = y;
                if (x1 < x0) std::swap(x0, x1);
                if (y1 < y0) std::swap(y0, y1);
                if (lineW_ <= 16) back_.fillRectAA(x0 - 0.5f, y0 - 0.5f, x1 + 0.5f, y1 + 0.5f);
                else back_.fillRoundRect(x0, y0, x1, y1, lr - 0.5f);
            }
            break;
        default:
            warnOnce(2, "VERTEX outside BEGIN/END", prim_);
            break;
    }
    vx_ = x;
    vy_ = y;
    ++nvert_;
}

void SwDisplay::text(int16_t x, int16_t y, uint8_t font, uint16_t options, const char* s) {
    rec(13, x, y, font, options, 0, 0, s ? s : "");
    if (font < 16 && handles_[font].valid && handles_[font].metric) {
        // CMD_TEXT on a custom font: widths from the RAM_G metric block.
        if (!s) return;
        const Handle& h = handles_[font];
        int w = 0;
        for (const char* p = s; *p; ++p) w += ramg_[h.metric + (uint8_t(*p) & 0x7F)];
        int px = x + translateX_ / 16, py = y;
        if (options & kOptRightX) px -= w;
        else if (options & kOptCenterX) px -= w / 2;
        if (options & kOptCenterY) py -= h.layoutH / 2;
        applyClip();
        for (const char* p = s; *p; ++p) {
            const uint8_t ch = uint8_t(*p) & 0x7F;
            const uint8_t adv = ramg_[h.metric + ch];
            if (adv && ch != ' ') customGlyph(px, py, font, ch);   // no cell below `first`: width 0
            px += adv;
        }
        return;
    }
    const RomFont* f = romFont(font);
    if (!f) {
        warnOnce(3, "CMD_TEXT with non-ROM font", font);
        return;
    }
    if (!s) return;
    int px = x + translateX_ / 16, py = y;   // the coprocessor's vertices are translated too
    const int w = textWidth(font, s);
    if (options & kOptRightX) px -= w;
    else if (options & kOptCenterX) px -= w / 2;
    if (options & kOptCenterY) py -= f->height / 2;
    applyClip();
    back_.text(px, py, font, s);
}

void SwDisplay::number(int16_t x, int16_t y, uint8_t font, uint16_t options, int32_t n) {
    rec(14, x, y, font, options, n);
    char buf[24];
    const int digits = options & 0xFF;
    bool neg = false;
    uint32_t v;
    if ((options & kOptSigned) && n < 0) {
        neg = true;
        v = uint32_t(-(int64_t)n);
    } else {
        v = uint32_t(n);
    }
    char tmp[16];
    int len = std::snprintf(tmp, sizeof(tmp), "%u", v);
    int p = 0;
    if (neg) buf[p++] = '-';
    for (int i = len; i < digits && p < 20; ++i) buf[p++] = '0';
    std::memcpy(buf + p, tmp, size_t(len) + 1);
    const bool r = replaying_;
    replaying_ = true;   // the expansion is not a recorded command
    text(x, y, font, uint16_t(options & ~0xFF), buf);
    replaying_ = r;
}

void SwDisplay::swap() {
    lastDl_ = dl_ + 1;   // + DISPLAY
    lastAppends_ = appends_;
    if (recSlot_ >= 0) warnOnce(20, "swap inside a fragment capture", recSlot_);
    recSlot_ = -1;
    std::lock_guard<std::mutex> lk(frontMu_);
    if (busyClock_) {
        pendingFrame_.resize(front_.size());
        std::memcpy(pendingFrame_.data(), back_.pixels(), front_.size() * 4);
        pending_ = true;
        pendingAtUs_ = busyNowUs() + uint64_t(lastDl_) * 4u * 8u / 30u;
    } else {
        std::memcpy(front_.data(), back_.pixels(), front_.size() * 4);
    }
    ++frames_;
}

bool SwDisplay::readPixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t* rgb) {
    if (uint32_t(x) + w > uint32_t(kW) || uint32_t(y) + h > uint32_t(kH)) return false;
    std::lock_guard<std::mutex> lk(frontMu_);
    if (busyClock_) busyTickLocked();
    for (uint16_t r = 0; r < h; ++r)
        for (uint16_t c = 0; c < w; ++c) rgb[size_t(r) * w + c] = front_[size_t(y + r) * kW + x + c] & 0xFFFFFFu;
    return true;
}

bool SwDisplay::loadJpeg(uint32_t dest, ot::IStorage& fs, const char* path, uint16_t maxW, uint16_t maxH,
                         uint16_t& w, uint16_t& h) {
    w = h = 0;
    uint16_t pw = 0, ph = 0;
    uint32_t size = 0;
    if (!probeJpeg(fs, path, dest, maxW, maxH, pw, ph, size)) return false;
    std::vector<uint8_t> file(size);
    const ot::IStorage::Handle f = fs.open(path, ot::IStorage::kRead);
    if (f < 0) return false;
    uint32_t got = 0;
    while (got < size) {
        const int32_t r = fs.read(f, file.data() + got, size - got);
        if (r <= 0) break;
        got += uint32_t(r);
    }
    fs.close(f);
    std::vector<uint8_t> rgb;
    int dw = 0, dh = 0;
    if (got != size || !decodeJpegRgb(file.data(), size, rgb, dw, dh) || dw != pw || dh != ph) return false;
    uint8_t* out = ramg_.data() + dest;     // probeJpeg checked dest + 2*w*h <= RAM_G size
    for (size_t i = 0; i < size_t(dw) * size_t(dh); ++i) {
        const uint16_t c = uint16_t(((rgb[i * 3] >> 3) << 11) | ((rgb[i * 3 + 1] >> 2) << 5) | (rgb[i * 3 + 2] >> 3));
        out[i * 2] = uint8_t(c);
        out[i * 2 + 1] = uint8_t(c >> 8);
    }
    w = pw;
    h = ph;
    return true;
}

bool SwDisplay::writeRamG(uint32_t addr, const void* src, uint32_t len) {
    if (!src || addr >= kRamGSize || len > kRamGSize - addr) return false;
    std::memcpy(ramg_.data() + addr, src, len);
    return true;
}

void SwDisplay::drawRgb332(int16_t x, int16_t y, uint32_t addr, uint16_t w, uint16_t h, uint8_t scale) {
    rec(15, x, y, int32_t(addr), w, h, scale);
    if (!w || !h || !scale || addr + uint32_t(w) * h > kRamGSize) {
        warnOnce(5, "drawRgb332 outside RAM_G", int(addr));
        return;
    }
    // Clip: scissor intersected with the screen.
    const int cx0 = std::max<int>(scX_, 0), cy0 = std::max<int>(scY_, 0);
    const int cx1 = std::min<int>(scX_ + scW_, kW), cy1 = std::min<int>(scY_ + scH_, kH);
    const int x0 = std::max(int(x), cx0), y0 = std::max(int(y), cy0);
    const int x1 = std::min(int(x) + int(w) * scale, cx1), y1 = std::min(int(y) + int(h) * scale, cy1);
    const uint32_t a = a_;
    if (!a) return;
    uint32_t* px = back_.pixels();
    const uint8_t* src = ramg_.data() + addr;
    for (int py = y0; py < y1; ++py) {
        const uint8_t* row = src + size_t((py - y) / scale) * w;
        for (int qx = x0; qx < x1; ++qx) {
            const uint8_t c = row[(qx - x) / scale];
            // RGB332 expanded to 8 bits per channel, then modulated.
            uint32_t r = uint32_t(c >> 5) * 255 / 7, g = uint32_t((c >> 2) & 7) * 255 / 7, b = uint32_t(c & 3) * 255 / 3;
            r = r * ((rgb_ >> 16) & 255) / 255;
            g = g * ((rgb_ >> 8) & 255) / 255;
            b = b * (rgb_ & 255) / 255;
            uint32_t& d = px[size_t(py) * kW + qx];
            if (a < 255) {
                const uint32_t ia = 255 - a;
                r = (r * a + ((d >> 16) & 255) * ia) / 255;
                g = (g * a + ((d >> 8) & 255) * ia) / 255;
                b = (b * a + (d & 255) * ia) / 255;
            }
            d = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void SwDisplay::drawImage(int16_t x, int16_t y, uint32_t addr, uint16_t w, uint16_t h) {
    rec(16, x, y, int32_t(addr), w, h);
    if (!w || !h || (addr & 1u) || addr + uint32_t(w) * h * 2u > kRamGSize) {
        warnOnce(4, "drawImage outside RAM_G", int(addr));
        return;
    }
    applyClip();
    back_.blitRgb565(x, y, ramg_.data() + addr, w, h);
}

// ---------------------------------------------------------------------------
// Fragments: record while drawing, replay on append (CMD_APPEND semantics: the
// appended commands run in the graphics state of the append point).

void SwDisplay::rec(uint8_t k, int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5,
                    const char* s) {
    if (replaying_) return;
    // DL words this command produces (CMD_TEXT: one per glyph plus BEGIN/END).
    // 1.9.5 ops as Gfx counts them: translate / save / restore 1, CMD_GRADIENT
    // Gfx::kGradientDlWords (what the FT81x builds, not the 5 uploaded),
    // BITMAP_TRANSFORM_A..F 6, BITMAP_HANDLE + BITMAP_SIZE 2.
    uint32_t w = 1;
    switch (k) {
        case 13: w = uint32_t(std::strlen(s)) + 2; break;
        case 14: case 15: case 16: w = 8; break;
        case 20: w = ot::Gfx::kGradientDlWords; break;
        case 21: w = 6; break;
        case 22: w = 2; break;
        default: break;
    }
    dl_ += w;
    if (recSlot_ < 0) return;
    recWords_ += w;
    Op o;
    o.k = k;
    o.a[0] = a0; o.a[1] = a1; o.a[2] = a2; o.a[3] = a3; o.a[4] = a4; o.a[5] = a5;
    if (s) o.s = s;
    frag_[recSlot_].push_back(std::move(o));
}

void SwDisplay::replay(const Op& o) {
    const int32_t* a = o.a;
    switch (o.k) {
        case 1: clearColorRGB(uint8_t(a[0]), uint8_t(a[1]), uint8_t(a[2])); break;
        case 2: clear(a[0] != 0, a[1] != 0, a[2] != 0); break;
        case 3: colorRGB(uint8_t(a[0]), uint8_t(a[1]), uint8_t(a[2])); break;
        case 4: colorA(uint8_t(a[0])); break;
        case 5: lineWidth(uint16_t(a[0])); break;
        case 6: pointSize(uint16_t(a[0])); break;
        case 7: scissorXY(uint16_t(a[0]), uint16_t(a[1])); break;
        case 8: scissorSize(uint16_t(a[0]), uint16_t(a[1])); break;
        case 9: begin(Prim(a[0])); break;
        case 10: end(); break;
        case 11: vertex2f(a[0], a[1]); break;
        case 12: vertex2ii(uint16_t(a[0]), uint16_t(a[1]), uint8_t(a[2]), uint8_t(a[3])); break;
        case 13: text(int16_t(a[0]), int16_t(a[1]), uint8_t(a[2]), uint16_t(a[3]), o.s.c_str()); break;
        case 14: number(int16_t(a[0]), int16_t(a[1]), uint8_t(a[2]), uint16_t(a[3]), a[4]); break;
        case 15: drawRgb332(int16_t(a[0]), int16_t(a[1]), uint32_t(a[2]), uint16_t(a[3]), uint16_t(a[4]), uint8_t(a[5])); break;
        case 16: drawImage(int16_t(a[0]), int16_t(a[1]), uint32_t(a[2]), uint16_t(a[3]), uint16_t(a[4])); break;
        case 17: vertexTranslateX(a[0]); break;
        case 18: saveContext(); break;
        case 19: restoreContext(); break;
        case 20: gradient(int16_t(a[0]), int16_t(a[1]), uint32_t(a[2]), int16_t(a[3]), int16_t(a[4]), uint32_t(a[5])); break;
        case 21: bitmapTransform(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 22: bitmapSize(uint8_t(a[0]), uint16_t(a[1]), uint16_t(a[2])); break;
        default: break;
    }
}

void SwDisplay::fragmentBegin(uint8_t slot) {
    if (!fragOn_ || slot >= kFragmentSlots || recSlot_ >= 0) {
        warnOnce(21, "fragmentBegin rejected", slot);
        return;
    }
    frag_[slot].clear();
    fragWords_[slot] = 0;
    recSlot_ = slot;
    recWords_ = 0;
}

bool SwDisplay::fragmentEnd(uint8_t slot) {
    if (recSlot_ != int(slot)) return false;
    recSlot_ = -1;
    if (recWords_ == 0 || recWords_ > kFragSlotWords) {   // device: slot holds 4 KB
        frag_[slot].clear();
        return false;
    }
    fragWords_[slot] = recWords_;
    return true;
}

void SwDisplay::fragmentAppend(uint8_t slot) {
    if (slot >= kFragmentSlots || !fragWords_[slot]) {
        warnOnce(22, "append of an empty fragment slot", slot);
        return;
    }
    if (recSlot_ >= 0) warnOnce(23, "append inside a fragment capture", slot);
    ++appends_;
    dl_ += fragWords_[slot];
    replaying_ = true;
    for (const Op& o : frag_[slot]) replay(o);
    replaying_ = false;
}

uint32_t SwDisplay::fragmentWords(uint8_t slot) const { return slot < kFragmentSlots ? fragWords_[slot] : 0; }

void SwDisplay::setFragments(bool on) {
    fragOn_ = on;
    if (!on)
        for (int i = 0; i < kFragmentSlots; ++i) {
            frag_[i].clear();
            fragWords_[i] = 0;
        }
}

void SwDisplay::dimFrame(uint32_t* px, size_t n, uint8_t duty) {
    if (duty >= 128) return;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t p = px[i];
        const uint32_t r = ((p >> 16) & 0xFF) * duty / 128, g = ((p >> 8) & 0xFF) * duty / 128,
                       b = (p & 0xFF) * duty / 128;
        px[i] = (p & 0xFF000000u) | (r << 16) | (g << 8) | b;
    }
}

uint32_t SwDisplay::frontCopy(uint32_t* dst) const {
    std::lock_guard<std::mutex> lk(frontMu_);
    if (busyClock_) busyTickLocked();
    std::memcpy(dst, front_.data(), front_.size() * 4);
    return frames_;
}

}  // namespace sim
