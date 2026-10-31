#include "remote/display_tap.h"

#include <cstring>

namespace ot {
namespace remote {

void DisplayTap::dlStart() {
    d_.dlStart();
    if (state_ == kArmed) {
        state_ = kCapturing;
        len_ = 0;
        cut_ = false;
        put(kTagFontSet);
        put(uint32_t(fontSet_));
    }
    put(kCmdDlStart);
}

void DisplayTap::swap() {
    d_.swap();
    if (state_ != kCapturing) return;
    put(0);   // DISPLAY
    put(kCmdSwap);
    ++seq_;
    state_ = kReady;
}

void DisplayTap::putString(const char* s) {
    const size_t n = std::strlen(s) + 1;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (size_t k = 0; k < 4 && i + k < n; ++k) w |= uint32_t(uint8_t(s[i + k])) << (8 * k);
        put(w);
    }
}

void DisplayTap::text(int16_t x, int16_t y, uint8_t font, uint16_t options, const char* s) {
    d_.text(x, y, font, options, s);
    put(kCmdText);
    put(xy(x, y));
    put(uint32_t(font) | uint32_t(options) << 16);
    putString(s);
}

void DisplayTap::number(int16_t x, int16_t y, uint8_t font, uint16_t options, int32_t n) {
    d_.number(x, y, font, options, n);
    put(kCmdNumber);
    put(xy(x, y));
    put(uint32_t(font) | uint32_t(options) << 16);
    put(uint32_t(n));
}

void DisplayTap::gradient(int16_t x0, int16_t y0, uint32_t rgb0, int16_t x1, int16_t y1, uint32_t rgb1) {
    d_.gradient(x0, y0, rgb0, x1, y1, rgb1);
    put(kCmdGradient);
    put(xy(x0, y0));
    put(rgb0 & 0xFFFFFFu);
    put(xy(x1, y1));
    put(rgb1 & 0xFFFFFFu);
}

void DisplayTap::bitmapTransform(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) {
    d_.bitmapTransform(a, b, c, d, e, f);
    put(0x15u << 24 | (uint32_t(a) & 0x1FFFFu));
    put(0x16u << 24 | (uint32_t(b) & 0x1FFFFu));
    put(0x17u << 24 | (uint32_t(c) & 0xFFFFFFu));
    put(0x18u << 24 | (uint32_t(d) & 0x1FFFFu));
    put(0x19u << 24 | (uint32_t(e) & 0x1FFFFu));
    put(0x1Au << 24 | (uint32_t(f) & 0xFFFFFFu));
}

void DisplayTap::drawImage(int16_t x, int16_t y, uint32_t ramgAddr, uint16_t w, uint16_t h) {
    d_.drawImage(x, y, ramgAddr, w, h);
    put(kTagImage);
    put(xy(x, y));
    put(ramgAddr);
    put(xy(w, h));
}

void DisplayTap::drawRgb332(int16_t x, int16_t y, uint32_t ramgAddr, uint16_t w, uint16_t h, uint8_t scale) {
    d_.drawRgb332(x, y, ramgAddr, w, h, scale);
    put(kTagRgb332);
    put(xy(x, y));
    put(ramgAddr);
    put(xy(w, h));
    put(scale);
}

void DisplayTap::textHint(int16_t x, int16_t y, uint8_t font, const char* s) {
    d_.textHint(x, y, font, s);
    if (state_ != kCapturing) return;
    size_t n = 0;
    bool glyphs = true;   // every character is a glyph cell (cell = character code)
    for (; s[n]; ++n) glyphs = glyphs && uint8_t(s[n]) > ' ' && uint8_t(s[n]) < 127;
    if (glyphs) {
        put(kHintGlyphs | uint32_t(font) << 16);
        return;
    }
    if (n > 255) n = 255;
    put(kHintString | uint32_t(font) << 16 | uint32_t(n) << 8);
    for (size_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (size_t k = 0; k < 4 && i + k < n; ++k) w |= uint32_t(uint8_t(s[i + k])) << (8 * k);
        put(w);
    }
}

// ---------------------------------------------------------------------------
// Replay

namespace {
struct Reader {
    const uint8_t* p;
    size_t n, i = 0;
    bool more() const { return i + 4 <= n; }
    bool word(uint32_t& w) {
        if (!more()) return false;
        w = uint32_t(p[i]) | uint32_t(p[i + 1]) << 8 | uint32_t(p[i + 2]) << 16 | uint32_t(p[i + 3]) << 24;
        i += 4;
        return true;
    }
    // NUL-terminated string padded to a word, in place (the stream outlives the call).
    bool string(char* out, size_t cap) {
        size_t k = 0;
        while (i < n) {
            const char c = char(p[i++]);
            if (k + 1 < cap) out[k++] = c;
            if (!c) {
                i = (i + 3) & ~size_t(3);
                out[k < cap ? k : cap - 1] = 0;
                return i <= n;
            }
        }
        return false;
    }
};
int32_t sext(uint32_t v, int bits) {
    const uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1u;
    return int32_t((v ^ m) - m);
}
int16_t lo16(uint32_t w) { return int16_t(uint16_t(w)); }
int16_t hi16(uint32_t w) { return int16_t(uint16_t(w >> 16)); }

void placeholder(IDisplay& out, int x, int y, int w, int h) {
    out.saveContext();
    out.colorA(255);
    out.colorRGB(96, 96, 96);
    out.rect(int16_t(x), int16_t(y), int16_t(x + w - 1), int16_t(y + h - 1));
    out.colorRGB(160, 160, 160);
    out.lineWidth(16);
    out.line(int16_t(x), int16_t(y), int16_t(x + w - 1), int16_t(y + h - 1));
    out.line(int16_t(x), int16_t(y + h - 1), int16_t(x + w - 1), int16_t(y));
    out.restoreContext();
}
}  // namespace

namespace {
// A glyph-run hint (kHintGlyphs / kHintString) -> textHint, positioned at the
// run's first glyph; the words read stay unread (they are drawn next).
bool hint(Reader& r, uint32_t w, int32_t translate, char* s, size_t cap, IDisplay& out) {
    size_t n = 0;
    if ((w >> 24) == (kHintString >> 24)) {
        const size_t len = (w >> 8) & 0xFF;
        for (size_t i = 0; i < len; i += 4) {
            uint32_t c = 0;
            if (!r.word(c)) return false;
            for (size_t k = 0; k < 4 && i + k < len; ++k)
                if (n + 1 < cap) s[n++] = char(c >> (8 * k));
        }
    }
    int x = 0, y = 0;
    bool first = true;
    for (size_t i = r.i; i + 4 <= r.n; i += 4) {
        const uint32_t g = uint32_t(r.p[i]) | uint32_t(r.p[i + 1]) << 8 | uint32_t(r.p[i + 2]) << 16 |
                           uint32_t(r.p[i + 3]) << 24;
        if ((g >> 24) == 0x2B) {
            translate = sext(g, 17);
            continue;
        }
        if ((g >> 30) != 2) break;
        if (first) {
            x = int((g >> 21) & 0x1FF) + translate / 16;
            y = int((g >> 12) & 0x1FF);
            first = false;
        }
        if ((w >> 24) != (kHintGlyphs >> 24)) break;
        if (n + 1 < cap) s[n++] = char(g & 0x7F);
    }
    s[n] = 0;
    out.textHint(int16_t(x), int16_t(y), uint8_t(w >> 16), s);
    return true;
}

bool replayWords(const uint8_t* data, size_t bytes, IDisplay& out, bool& started) {
    Reader r{data, bytes};
    char s[256];
    int32_t translate = 0;   // VERTEX_TRANSLATE_X in effect (the hints' positions)
    // Six BITMAP_TRANSFORM words arrive one by one (always all six, in order).
    int32_t tf[6] = {256, 0, 0, 0, 256, 0};
    uint8_t handle = 0;
    for (uint32_t w; r.word(w);) {
        if (w >= 0xFFFFFF00u) {
            uint32_t a = 0, b = 0, c = 0, d = 0;
            switch (w) {
                case kTagFontSet:
                    if (!r.word(a)) return false;
                    if (!started) out.bindFontSet(int32_t(a));
                    break;
                case kCmdDlStart:
                    out.dlStart();
                    started = true;
                    break;
                case kCmdSwap: out.swap(); return true;
                case kCmdText:
                    if (!r.word(a) || !r.word(b) || !r.string(s, sizeof s)) return false;
                    out.text(lo16(a), hi16(a), uint8_t(b), uint16_t(b >> 16), s);
                    break;
                case kCmdNumber:
                    if (!r.word(a) || !r.word(b) || !r.word(c)) return false;
                    out.number(lo16(a), hi16(a), uint8_t(b), uint16_t(b >> 16), int32_t(c));
                    break;
                case kCmdGradient: {
                    uint32_t e = 0;
                    if (!r.word(a) || !r.word(b) || !r.word(c) || !r.word(e)) return false;
                    out.gradient(lo16(a), hi16(a), b, lo16(c), hi16(c), e);
                    break;
                }
                case kTagImage:
                    if (!r.word(a) || !r.word(b) || !r.word(c)) return false;
                    placeholder(out, lo16(a), hi16(a), lo16(c), hi16(c));
                    break;
                case kTagRgb332:
                    if (!r.word(a) || !r.word(b) || !r.word(c) || !r.word(d)) return false;
                    placeholder(out, lo16(a), hi16(a), lo16(c) * int(d ? d : 1), hi16(c) * int(d ? d : 1));
                    break;
                case kTagFragment:
                    if (!r.word(a)) return false;
                    break;
                default: return false;
            }
            continue;
        }
        if ((w >> 24) == (kHintGlyphs >> 24) || (w >> 24) == (kHintString >> 24)) {
            if (!hint(r, w, translate, s, sizeof s, out)) return false;
            continue;
        }
        if (!started) return false;
        switch (w >> 30) {
            case 1: out.vertex2f(sext(w >> 15, 15), sext(w, 15)); continue;
            case 2:
                out.vertex2ii(uint16_t((w >> 21) & 0x1FF), uint16_t((w >> 12) & 0x1FF), uint8_t((w >> 7) & 0x1F),
                              uint8_t(w & 0x7F));
                continue;
            default: break;
        }
        const uint32_t v = w & 0xFFFFFFu;
        switch (w >> 24) {
            case 0x00: break;   // DISPLAY
            case 0x02: out.clearColorRGB(uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)); break;
            case 0x26: out.clear(v & 4, v & 2, v & 1); break;
            case 0x04: out.colorRGB(uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)); break;
            case 0x10: out.colorA(uint8_t(v)); break;
            case 0x0E: out.lineWidth(uint16_t(v & 0xFFF)); break;
            case 0x0D: out.pointSize(uint16_t(v & 0x1FFF)); break;
            case 0x1B: out.scissorXY(uint16_t((v >> 11) & 0x7FF), uint16_t(v & 0x7FF)); break;
            case 0x1C: out.scissorSize(uint16_t((v >> 12) & 0xFFF), uint16_t(v & 0xFFF)); break;
            case 0x1F: out.begin(IDisplay::Prim(v & 0xF)); break;
            case 0x21: out.end(); break;
            case 0x2B:
                translate = sext(v, 17);
                out.vertexTranslateX(translate);
                break;
            case 0x22: out.saveContext(); break;
            case 0x23: out.restoreContext(); break;
            case 0x15: tf[0] = sext(v, 17); break;
            case 0x16: tf[1] = sext(v, 17); break;
            case 0x17: tf[2] = sext(v, 24); break;
            case 0x18: tf[3] = sext(v, 17); break;
            case 0x19: tf[4] = sext(v, 17); break;
            case 0x1A:
                tf[5] = sext(v, 24);
                out.bitmapTransform(tf[0], tf[1], tf[2], tf[3], tf[4], tf[5]);
                break;
            case 0x05: handle = uint8_t(v & 0x1F); break;
            case 0x08: out.bitmapSize(handle, uint16_t((v >> 9) & 0x1FF), uint16_t(v & 0x1FF)); break;
            default: return false;
        }
    }
    return false;   // no CMD_SWAP: cut
}
}  // namespace

bool replayFrame(const uint8_t* data, size_t bytes, IDisplay& out) {
    bool started = false;
    const bool ok = replayWords(data, bytes, out, started);
    if (!ok && started) out.swap();   // a cut or damaged frame: what was there
    return ok;
}

}  // namespace remote
}  // namespace ot
