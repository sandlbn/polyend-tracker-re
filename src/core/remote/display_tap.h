// [I] Display command export (remote drive mode, core/remote/remote.h): an
// IDisplay that passes every call on to the real display and, for a frame a
// host asked for, also records the frame's commands into a buffer. The
// bridge streams that buffer to the host ("@RC F ..."), where the simulator
// renders it (opentracker-sim --frame / --mirror) and tools/tracker_remote.py
// reads its text (text, expect-text).
//
// Frame format: little-endian 32-bit words, the coprocessor command stream
// the FT81x driver (src/hal/ft81x.cpp) writes for the same calls: DL words
// (COLOR_RGB, VERTEX2F, ...; the FT81x field widths, so out-of-range values
// wrap like on the device) and CMD_DLSTART / CMD_TEXT / CMD_NUMBER /
// CMD_GRADIENT / CMD_SWAP with their arguments (strings NUL-terminated,
// padded to 4 bytes), DISPLAY before CMD_SWAP. Private records the FT81x
// does not have (first word kTag* or kHint*, never a DL word or a
// coprocessor command):
//   kTagFontSet set                      bindFontSet in effect (-1: ROM fonts),
//                                        the first word of every frame
//   kHintGlyphs | font << 16             a glyph run drawn as VERTEX2II
//                                        (Gfx::glyphRun) whose text is the cells
//                                        of the VERTEX2II words that follow (up to
//                                        the next word that is neither VERTEX2II
//                                        nor VERTEX_TRANSLATE_X): no spaces
//   kHintString | font << 16 | n << 8,   any other glyph run: its n characters,
//     n bytes padded to words            4 a word (n <= 255)
//                                        The hints are for readers (the text on
//                                        screen); the position is the next glyph's.
//   kTagImage xy addr wh                 drawImage (RAM_G picture: not sent)
//   kTagRgb332 xy addr wh scale          drawRgb332 (Games frame: not sent)
//   kTagFragment slot                    fragmentAppend (contents not sent)
//   (xy = x | y << 16, wh = w | h << 16, both 16-bit fields)
//
// The UI's cached fragments (CMD_APPEND of RAM_G copies) are not in the
// stream, so the App draws a captured frame with the fragment cache off
// (App::serviceRemote): request() -> the App sees requested(), turns the
// cache off and calls arm() -> the next dlStart..swap is recorded -> ready().
// A frame larger than the buffer is cut (truncated()); its words stay valid.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hal_api.h"

namespace ot {
namespace remote {

// FT81x coprocessor commands in the stream (programmer guide values).
constexpr uint32_t kCmdDlStart = 0xFFFFFF00u;
constexpr uint32_t kCmdSwap = 0xFFFFFF01u;
constexpr uint32_t kCmdGradient = 0xFFFFFF0Bu;
constexpr uint32_t kCmdText = 0xFFFFFF0Cu;
constexpr uint32_t kCmdNumber = 0xFFFFFF2Eu;
// Private records (an unused command range).
constexpr uint32_t kTagFontSet = 0xFFFFFFE0u;
constexpr uint32_t kHintGlyphs = 0xFE000000u;   // top byte: no DL word has it
constexpr uint32_t kHintString = 0xFD000000u;
constexpr uint32_t kTagImage = 0xFFFFFFE2u;
constexpr uint32_t kTagRgb332 = 0xFFFFFFE3u;
constexpr uint32_t kTagFragment = 0xFFFFFFE4u;

// 9 KB on the device: the frame's RAM_DL is at most 8 KB (Ui's guard keeps
// it below ~7.6 KB), the stream is about as large plus a word per glyph run
// (display_budget_test: worst case ~8.5 KB, the dense Pattern grid).
constexpr uint32_t kFrameBufBytes = 9216;

class DisplayTap final : public IDisplay {
public:
    explicit DisplayTap(IDisplay& inner) : d_(inner) {}
    void setBuffer(uint8_t* buf, uint32_t cap) {
        buf_ = buf;
        cap_ = cap & ~3u;
        state_ = kIdle;
    }

    // Capture protocol (see the header comment).
    void request() {
        if (buf_ && state_ == kIdle) state_ = kRequested;
    }
    bool requested() const { return state_ == kRequested; }
    void arm() {
        if (state_ == kRequested) state_ = kArmed;
    }
    bool busy() const { return state_ == kArmed || state_ == kCapturing; }   // cache stays off
    bool ready() const { return state_ == kReady; }
    const uint8_t* frame() const { return buf_; }
    uint32_t frameBytes() const { return len_; }
    bool truncated() const { return cut_; }
    uint32_t frameSeq() const { return seq_; }   // frames recorded since boot
    // Done with the frame (sent, or the host left): the next request may start.
    void release() { state_ = kIdle; }

    // --- IDisplay: forwarded, recorded while capturing ----------------------
    uint16_t width() const override { return d_.width(); }
    uint16_t height() const override { return d_.height(); }
    void dlStart() override;
    void clearColorRGB(uint8_t r, uint8_t g, uint8_t b) override {
        d_.clearColorRGB(r, g, b);
        put(0x02u << 24 | uint32_t(r) << 16 | uint32_t(g) << 8 | b);
    }
    void clear(bool color, bool stencil, bool tag) override {
        d_.clear(color, stencil, tag);
        put(0x26u << 24 | (color ? 4u : 0u) | (stencil ? 2u : 0u) | (tag ? 1u : 0u));
    }
    void colorRGB(uint8_t r, uint8_t g, uint8_t b) override {
        d_.colorRGB(r, g, b);
        put(0x04u << 24 | uint32_t(r) << 16 | uint32_t(g) << 8 | b);
    }
    void colorA(uint8_t a) override {
        d_.colorA(a);
        put(0x10u << 24 | a);
    }
    void lineWidth(uint16_t w) override {
        d_.lineWidth(w);
        put(0x0Eu << 24 | (w & 0xFFFu));
    }
    void pointSize(uint16_t s) override {
        d_.pointSize(s);
        put(0x0Du << 24 | (s & 0x1FFFu));
    }
    void scissorXY(uint16_t x, uint16_t y) override {
        d_.scissorXY(x, y);
        put(0x1Bu << 24 | (uint32_t(x) & 0x7FFu) << 11 | (y & 0x7FFu));
    }
    void scissorSize(uint16_t w, uint16_t h) override {
        d_.scissorSize(w, h);
        put(0x1Cu << 24 | (uint32_t(w) & 0xFFFu) << 12 | (h & 0xFFFu));
    }
    void begin(Prim prim) override {
        d_.begin(prim);
        put(0x1Fu << 24 | (prim & 0xFu));
    }
    void end() override {
        d_.end();
        put(0x21u << 24);
    }
    void vertex2f(int32_t x16, int32_t y16) override {
        d_.vertex2f(x16, y16);
        put(1u << 30 | (uint32_t(x16) & 0x7FFFu) << 15 | (uint32_t(y16) & 0x7FFFu));
    }
    void vertex2ii(uint16_t x, uint16_t y, uint8_t handle, uint8_t cell) override {
        d_.vertex2ii(x, y, handle, cell);
        put(2u << 30 | (uint32_t(x) & 0x1FFu) << 21 | (uint32_t(y) & 0x1FFu) << 12 | (uint32_t(handle) & 0x1Fu) << 7 |
            (cell & 0x7Fu));
    }
    void text(int16_t x, int16_t y, uint8_t font, uint16_t options, const char* s) override;
    void number(int16_t x, int16_t y, uint8_t font, uint16_t options, int32_t n) override;
    void swap() override;
    bool readPixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t* rgb) override {
        return d_.readPixels(x, y, w, h, rgb);
    }
    void setBacklight(uint8_t level) override { d_.setBacklight(level); }
    bool loadJpeg(uint32_t ramgAddr, IStorage& fs, const char* path, uint16_t maxW, uint16_t maxH, uint16_t& w,
                  uint16_t& h) override {
        return d_.loadJpeg(ramgAddr, fs, path, maxW, maxH, w, h);
    }
    void drawImage(int16_t x, int16_t y, uint32_t ramgAddr, uint16_t w, uint16_t h) override;
    bool writeRamG(uint32_t ramgAddr, const void* src, uint32_t len) override {
        return d_.writeRamG(ramgAddr, src, len);
    }
    void drawRgb332(int16_t x, int16_t y, uint32_t ramgAddr, uint16_t w, uint16_t h, uint8_t scale) override;
    bool frameReady() override { return d_.frameReady(); }
    bool fragmentsSupported() const override { return d_.fragmentsSupported(); }
    void fragmentBegin(uint8_t slot) override { d_.fragmentBegin(slot); }
    bool fragmentEnd(uint8_t slot) override { return d_.fragmentEnd(slot); }
    void fragmentAppend(uint8_t slot) override {
        d_.fragmentAppend(slot);
        put(kTagFragment);
        put(slot);
    }
    uint32_t fragmentWords(uint8_t slot) const override { return d_.fragmentWords(slot); }
    bool bindFontSet(int set) override {
        const bool ok = d_.bindFontSet(set);
        fontSet_ = ok ? set : -1;
        return ok;
    }
    void gradient(int16_t x0, int16_t y0, uint32_t rgb0, int16_t x1, int16_t y1, uint32_t rgb1) override;
    void vertexTranslateX(int32_t x16) override {
        d_.vertexTranslateX(x16);
        put(0x2Bu << 24 | (uint32_t(x16) & 0x1FFFFu));
    }
    void saveContext() override {
        d_.saveContext();
        put(0x22u << 24);
    }
    void restoreContext() override {
        d_.restoreContext();
        put(0x23u << 24);
    }
    void bitmapTransform(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) override;
    void bitmapSize(uint8_t handle, uint16_t w, uint16_t h) override {
        d_.bitmapSize(handle, w, h);
        put(0x05u << 24 | (handle & 0x1Fu));
        put(0x08u << 24 | (uint32_t(w) & 0x1FFu) << 9 | (h & 0x1FFu));
    }
    void textHint(int16_t x, int16_t y, uint8_t font, const char* s) override;

private:
    enum State : uint8_t { kIdle, kRequested, kArmed, kCapturing, kReady };
    void put(uint32_t w) {
        if (state_ != kCapturing) return;
        if (len_ + 4 > cap_) {
            cut_ = true;
            return;
        }
        buf_[len_] = uint8_t(w);
        buf_[len_ + 1] = uint8_t(w >> 8);
        buf_[len_ + 2] = uint8_t(w >> 16);
        buf_[len_ + 3] = uint8_t(w >> 24);
        len_ += 4;
    }
    static uint32_t xy(int x, int y) { return uint32_t(uint16_t(x)) | uint32_t(uint16_t(y)) << 16; }
    void putString(const char* s);   // NUL-terminated, padded to a word

    IDisplay& d_;
    uint8_t* buf_ = nullptr;
    uint32_t cap_ = 0, len_ = 0, seq_ = 0;
    bool cut_ = false;
    int fontSet_ = -1;
    State state_ = kIdle;
};

// Host side: draws a recorded frame on `out` (dlStart .. swap, after
// bindFontSet of the frame's font set). Pictures and Games frames, whose
// pixels are not in the stream, become grey boxes; an appended fragment is
// skipped. False for a malformed stream (drawing stops there).
bool replayFrame(const uint8_t* data, size_t bytes, IDisplay& out);

}  // namespace remote
}  // namespace ot
