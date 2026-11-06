// [I] Display command recorder (core/remote/display_tap.h): records exactly
// the requested frame, passes every call and the device feedback through,
// and replays back to the same calls. The simulator's --frames uses it for
// the FT81x emulator check (tools/eve_emu).
#include <memory>
#include <string>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "remote/display_tap.h"

using namespace ot;
using namespace ot::remote;

namespace {
// RecordingDisplay plus the calls it does not log.
struct FullRecorder : RecordingDisplay {
    std::vector<std::string> extra;   // bitmap transform / size, text hints (any frame)
    void bitmapTransform(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) override {
        extra.push_back("TF " + std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c) + " " +
                        std::to_string(d) + " " + std::to_string(e) + " " + std::to_string(f));
    }
    void bitmapSize(uint8_t h, uint16_t w, uint16_t hh) override {
        extra.push_back("BS " + std::to_string(h) + " " + std::to_string(w) + " " + std::to_string(hh));
    }
    void textHint(int16_t x, int16_t y, uint8_t font, const char* s) override {
        extra.push_back("HINT " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(font) + " " + s);
    }
};

void drawSample(IDisplay& d) {
    d.dlStart();
    d.clearColorRGB(1, 2, 3);
    d.clear(true, false, true);
    d.colorRGB(200, 100, 50);
    d.colorA(77);
    d.lineWidth(24);
    d.pointSize(40);
    d.scissorXY(10, 20);
    d.scissorSize(2048, 2048);
    d.begin(IDisplay::kRects);
    d.vertex2f(-3 * 16, 5 * 16);    // negative: sign-extended back
    d.vertex2f(799 * 16, 479 * 16);
    d.end();
    d.vertexTranslateX(-400 * 16);
    d.vertexTranslateX(400 * 16);
    d.saveContext();
    d.bitmapSize(4, 13, 300);
    d.bitmapTransform(0, -256, 11 * 256, 256, 0, -5);
    d.begin(IDisplay::kBitmaps);
    d.vertex2ii(511, 300, 4, 'A');
    d.end();
    d.restoreContext();
    // Glyph runs (Gfx::glyphRun): the hint, then the glyphs (sharing one BITMAPS).
    d.vertexTranslateX(0);
    d.begin(IDisplay::kBitmaps);
    d.textHint(5, 6, 2, "Ab");   // spelled by its cells
    d.vertex2ii(5, 6, 4, 'A');
    d.vertex2ii(13, 6, 4, 'b');
    d.textHint(100, 200, 3, "a b");   // a space: sent as a string
    d.vertex2ii(100, 200, 5, 'a');
    d.vertex2ii(116, 200, 5, 'b');
    d.vertexTranslateX(400 * 16);
    d.textHint(511, 300, 2, "Z9");   // right of x 511: translated
    d.vertex2ii(111, 300, 4, 'Z');
    d.vertex2ii(119, 300, 4, '9');
    d.end();
    d.vertexTranslateX(0);
    for (const char* s : {"", "a", "ab", "abc", "abcd", "Pattern 1"}) d.text(-4, 33, 27, IDisplay::kOptCenterY, s);
    d.number(700, 40, 26, IDisplay::kOptSigned | IDisplay::kOptRightX, -12345);
    d.gradient(0, -1, 0x123456u, 800, 27, 0xABCDEFu);
    d.swap();
}
}  // namespace

TEST(display_tap_records_only_the_requested_frame) {
    FullRecorder real;
    DisplayTap tap(real);
    std::vector<uint8_t> buf(kFrameBufBytes);
    drawSample(tap);   // no buffer: passes through only
    CHECK_EQ(real.swaps, 1);
    tap.request();
    CHECK(!tap.requested());   // no buffer, no capture
    tap.setBuffer(buf.data(), uint32_t(buf.size()));
    drawSample(tap);
    CHECK(!tap.ready());
    tap.request();
    CHECK(tap.requested());
    drawSample(tap);   // requested, not armed (the cache is not off yet)
    CHECK(!tap.ready());
    tap.arm();
    CHECK(tap.busy());
    drawSample(tap);
    CHECK(tap.ready());
    CHECK(!tap.truncated());
    CHECK_EQ(tap.frameSeq(), 1u);
    CHECK_EQ(real.swaps, 4);
    // Further frames are not recorded over it.
    const uint32_t n = tap.frameBytes();
    tap.request();
    real.dlStart();
    CHECK_EQ(tap.frameBytes(), n);
    CHECK(tap.ready());
    tap.release();
    CHECK(!tap.ready() && !tap.busy());
}

// The UI draws through the recorder: the driver's fault count,
// measured RAM_DL size and pending frame must reach it.
TEST(display_tap_forwards_device_feedback) {
    FullRecorder real;
    DisplayTap tap(real);
    real.faultCount = 3;
    real.measuredBytes = 4752;
    CHECK_EQ(tap.frameFaultCount(), 3u);
    CHECK_EQ(tap.measuredDlBytes(), 4752u);
    CHECK(!tap.framePending());
    drawSample(tap);
    CHECK(tap.framePending());   // swapped, outcome not polled yet
    CHECK(tap.frameReady());
    CHECK(!tap.framePending());
}

TEST(display_tap_replay_round_trip) {
    FullRecorder real, again;
    DisplayTap tap(real);
    std::vector<uint8_t> buf(kFrameBufBytes);
    tap.setBuffer(buf.data(), uint32_t(buf.size()));
    tap.bindFontSet(1);   // the recorder has no custom fonts: the frame says ROM (-1)
    tap.request();
    tap.arm();
    drawSample(tap);
    CHECK(tap.ready());
    // Every word is 4 bytes: the frame is whole words.
    CHECK_EQ(tap.frameBytes() % 4, 0u);
    CHECK(replayFrame(tap.frame(), tap.frameBytes(), again));
    CHECK_EQ(again.last.size(), real.last.size());
    for (size_t i = 0; i < real.last.size() && i < again.last.size(); ++i) CHECK_STR(again.last[i].c_str(), real.last[i].c_str());
    CHECK_EQ(again.extra.size(), real.extra.size());
    for (size_t i = 0; i < real.extra.size() && i < again.extra.size(); ++i)
        CHECK_STR(again.extra[i].c_str(), real.extra[i].c_str());
    CHECK(again.fontBinds.size() == 1u && again.fontBinds[0] == -1);
    // Cut anywhere: replay stops there (false) but still shows what it drew.
    for (uint32_t cut = 12; cut < tap.frameBytes(); cut += 4) {   // from after CMD_DLSTART
        FullRecorder part;
        const int swaps = part.swaps;
        CHECK(!replayFrame(tap.frame(), cut, part));
        CHECK_EQ(part.swaps, swaps + 1);
    }
}

TEST(display_tap_pictures_and_overflow) {
    FullRecorder real, again;
    DisplayTap tap(real);
    std::vector<uint8_t> buf(64);
    tap.setBuffer(buf.data(), uint32_t(buf.size()));
    tap.request();
    tap.arm();
    tap.dlStart();
    tap.drawImage(10, 20, 670000, 100, 50);
    tap.drawRgb332(0, 0, 384000, 256, 240, 2);
    tap.fragmentAppend(3);
    tap.swap();
    CHECK(tap.ready());
    CHECK(!tap.truncated());
    CHECK(replayFrame(tap.frame(), tap.frameBytes(), again));
    // Pictures become grey boxes (their pixels are not sent).
    bool box = false;
    for (auto& l : again.last) box |= l == "VERTEX 109 69";
    CHECK(box);
    // Too small a buffer: cut, the words kept are whole.
    tap.release();
    tap.request();
    tap.arm();
    drawSample(tap);
    CHECK(tap.ready());
    CHECK(tap.truncated());
    CHECK_EQ(tap.frameBytes(), 64u);
    FullRecorder part;
    CHECK(!replayFrame(tap.frame(), tap.frameBytes(), part));
    CHECK_EQ(part.swaps, 1);
}
