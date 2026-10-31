// Frame exports (.otf: core/remote/display_tap.h, from the remote "frame"
// command or opentracker-sim --frames) -> the coprocessor word stream the
// FT81x driver (src/hal/ft81x.cpp) sends for the same frame, for replay.cpp:
// the private hint records dropped, the driver's per-frame font setup
// (Ft81xDisplay::fontSetup) after CMD_DLSTART, drawImage / drawRgb332 as the
// driver's DL words (the pictures themselves are not needed for the count),
// CMD_SWAP dropped (the replay reads REG_CMD_DL once DISPLAY is processed).
// Also writes the font image for RAM_G 0x06CC00. Keep the setup words in
// step with ft81x.cpp.
//
//   otf2cmd OUT_DIR FONTS.bin IN.otf [IN.otf ...]   -> OUT_DIR/<name>.cmd
//
// Prints "NAME FONT_SET WORDS" per frame (font set -1: ROM fonts, no setup).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "hal_api.h"
#include "ui/fonts/fonts.h"

using namespace ot;

namespace {

constexpr uint32_t kFmtL4 = 2, kFmtRgb565 = 7, kFmtRgb332 = 4;   // ft81x.cpp
constexpr uint8_t kImageHandle = 1, kGameHandle = 2;              // ft81x.h
constexpr uint32_t kBegin = 0x1Fu << 24, kBitmaps = 1, kEnd = 0x21u << 24;

std::vector<uint32_t> out;

// high: the _H words carry the size's high bits (drawImage); the font and
// icon setup writes them as 0.
void bitmap(uint8_t h, uint32_t src, uint32_t fmt, uint32_t stride, uint32_t w, uint32_t hgt, bool high) {
    out.push_back((0x05u << 24) | h);
    out.push_back((0x01u << 24) | (src & 0x3FFFFFu));
    out.push_back((0x28u << 24) | (high ? (((stride >> 10) & 3u) << 2) | ((hgt >> 9) & 3u) : 0u));
    out.push_back((0x07u << 24) | (fmt << 19) | ((stride & 0x3FFu) << 9) | (hgt & 0x1FFu));
    out.push_back((0x29u << 24) | (high ? (((w >> 9) & 3u) << 2) | ((hgt >> 9) & 3u) : 0u));
    out.push_back((0x08u << 24) | ((w & 0x1FFu) << 9) | (hgt & 0x1FFu));
}

void vertexAt(uint32_t xy) {
    const int32_t x = int16_t(xy & 0xFFFF), y = int16_t(xy >> 16);
    out.push_back(kBegin | kBitmaps);
    out.push_back(1u << 30 | (uint32_t(x * 16) & 0x7FFFu) << 15 | (uint32_t(y * 16) & 0x7FFFu));
    out.push_back(kEnd);
}

void fontSetup(int set) {
    for (uint8_t i = 0; i < IDisplay::kFontHandleCount; ++i) {
        const fontdata::FontInfo& f = fontdata::info(set, i);
        const uint8_t h = uint8_t(IDisplay::kFontHandleBase + i);
        bitmap(h, fontdata::sourceAddr(f), kFmtL4, f.stride, f.w, f.h, false);
        out.push_back(0xFFFFFF2Bu);   // CMD_SETFONT
        out.push_back(h);
        out.push_back(fontdata::kRamGBase + f.metric);
    }
    const fontdata::IconSheet& ic = fontdata::kIcons;
    bitmap(IDisplay::kIconHandle, fontdata::kRamGBase + ic.offset, kFmtL4, ic.stride, ic.w, ic.h, false);
}

// One frame; returns false on records it cannot translate.
bool convert(const std::vector<uint32_t>& w, int& fragments, int& set) {
    set = -1;
    bool ok = true;
    for (size_t i = 0; i < w.size(); ++i) {
        const uint32_t x = w[i];
        const uint32_t top = x >> 24;
        if (top == 0xFE) continue;                                          // kHintGlyphs
        if (top == 0xFD) { i += (((x >> 8) & 255) + 3) / 4; continue; }    // kHintString
        if (x == 0xFFFFFFE0u) { set = int32_t(w[++i]); continue; }          // kTagFontSet
        if (x == 0xFFFFFFE2u) {                                             // kTagImage xy addr wh
            const uint32_t xy = w[i + 1], addr = w[i + 2], wh = w[i + 3];
            const uint32_t iw = wh & 0xFFFF, ih = wh >> 16;
            i += 3;
            if (!iw || !ih || iw > 2047 || ih > 2047) continue;
            bitmap(kImageHandle, addr, kFmtRgb565, iw * 2u, iw, ih, true);
            vertexAt(xy);
            continue;
        }
        if (x == 0xFFFFFFE3u) {                                             // kTagRgb332 xy addr wh scale
            const uint32_t xy = w[i + 1], addr = w[i + 2], wh = w[i + 3], s = w[i + 4];
            const uint32_t iw = wh & 0xFFFF, ih = wh >> 16;
            i += 4;
            if (!iw || !ih || !s || iw > 1023 || ih > 1023 || iw * s > 2047 || ih * s > 2047) continue;
            const uint32_t sw = iw * s, sh = ih * s, ab = 256u / s;
            out.push_back(0x22u << 24);   // SAVE_CONTEXT
            out.push_back((0x05u << 24) | kGameHandle);
            out.push_back((0x01u << 24) | (addr & 0x3FFFFFu));
            out.push_back((0x28u << 24) | (((iw >> 10) & 3u) << 2) | ((ih >> 9) & 3u));
            out.push_back((0x07u << 24) | (kFmtRgb332 << 19) | ((iw & 0x3FFu) << 9) | (ih & 0x1FFu));
            out.push_back((0x29u << 24) | (((sw >> 9) & 3u) << 2) | ((sh >> 9) & 3u));
            out.push_back((0x08u << 24) | ((sw & 0x1FFu) << 9) | (sh & 0x1FFu));
            out.push_back((0x15u << 24) | (ab & 0x1FFFFu));
            out.push_back((0x19u << 24) | (ab & 0x1FFFFu));
            vertexAt(xy);
            out.push_back(0x23u << 24);   // RESTORE_CONTEXT
            continue;
        }
        if (x == 0xFFFFFFE4u) { ++i; ++fragments; continue; }               // kTagFragment: contents unknown
        if (x == 0xFFFFFF01u) continue;                                     // CMD_SWAP
        out.push_back(x);
        if (x == 0xFFFFFF00u) { if (set >= 0) fontSetup(set); continue; }   // CMD_DLSTART
        if (x == 0xFFFFFF0Cu) {                                             // CMD_TEXT xy, font|opt, string
            out.push_back(w[++i]);
            out.push_back(w[++i]);
            for (;;) {
                const uint32_t s = w[++i];
                out.push_back(s);
                if (!(s & 0xFF) || !(s & 0xFF00) || !(s & 0xFF0000) || !(s & 0xFF000000)) break;
            }
            continue;
        }
        if (x == 0xFFFFFF2Eu) { for (int k = 0; k < 3; ++k) out.push_back(w[++i]); continue; }   // CMD_NUMBER
        if (x == 0xFFFFFF0Bu) { for (int k = 0; k < 4; ++k) out.push_back(w[++i]); continue; }   // CMD_GRADIENT
        if (x >= 0xFFFFFF00u) {
            std::fprintf(stderr, "unknown command 0x%08X\n", unsigned(x));
            ok = false;
        }
    }
    return ok;
}

std::string stem(const std::string& path) {
    const size_t s = path.find_last_of('/');
    std::string n = s == std::string::npos ? path : path.substr(s + 1);
    const size_t d = n.rfind('.');
    return d == std::string::npos ? n : n.substr(0, d);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: otf2cmd OUT_DIR FONTS.bin IN.otf [IN.otf ...]\n");
        return 2;
    }
    const std::string dir = argv[1];
    if (FILE* fb = std::fopen(argv[2], "wb")) {
        std::fwrite(fontdata::kBlob, 1, fontdata::kBlobSize, fb);
        std::fclose(fb);
    } else {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 2;
    }
    int bad = 0;
    for (int a = 3; a < argc; ++a) {
        FILE* f = std::fopen(argv[a], "rb");
        if (!f) {
            std::fprintf(stderr, "cannot read %s\n", argv[a]);
            ++bad;
            continue;
        }
        std::vector<uint32_t> w;
        uint32_t v;
        while (std::fread(&v, 4, 1, f) == 1) w.push_back(v);
        std::fclose(f);
        out.clear();
        int fragments = 0, set = -1;
        const bool ok = convert(w, fragments, set);
        if (fragments) std::fprintf(stderr, "%s: %d cached fragment(s): record frames with the cache off\n", argv[a], fragments);
        const std::string path = dir + "/" + stem(argv[a]) + ".cmd";
        FILE* o = std::fopen(path.c_str(), "wb");
        if (!o) {
            std::fprintf(stderr, "cannot write %s\n", path.c_str());
            ++bad;
            continue;
        }
        std::fwrite(out.data(), 4, out.size(), o);
        std::fclose(o);
        std::printf("%s %d %zu\n", stem(argv[a]).c_str(), set, out.size());   // name, font set, words
        if (!ok || fragments) ++bad;
    }
    return bad ? 1 : 0;
}
