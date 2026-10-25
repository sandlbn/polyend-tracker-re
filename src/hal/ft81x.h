// OpenTracker -- FT81x (EVE2) display on SPI2 implementing IDisplay (spec 01 §4).
#pragma once

#include <stdint.h>
#include "hal_api.h"
#include "ui/fonts/fonts.h"

namespace ot {
namespace hal {

class Ft81xDisplay final : public IDisplay {
public:
    bool init();                  // spec 01 §4.2 power-up sequence; true if REG_ID == 0x7C
    bool ok() const { return ok_; }
    uint8_t lastId() const { return lastId_; }         // last REG_ID read (0x7C = alive)
    uint8_t bootAttempt() const { return bootAttempt_; }  // 0 = internal osc, 1 = CLKEXT, 2 = failed

    // IDisplay::setBacklight: REG_PWM_DUTY 0..128 (spec 01 §4.3, spec 05 §1.8).
    // Before the first draw it only records the level the first draw uses
    // (spec 01 §4.2: duty 0 during init, backlight on at the first draw).
    void setBacklight(uint8_t duty) override;
    void setRotate(uint8_t rot);       // REG_ROTATE (spec 01 §4.3)
    void displayOff();                 // spec 01 §4.3 "Display off"

    // IDisplay
    uint16_t width() const override;
    uint16_t height() const override;
    void dlStart() override;
    void clearColorRGB(uint8_t r, uint8_t g, uint8_t b) override;
    void clear(bool color, bool stencil, bool tag) override;
    void colorRGB(uint8_t r, uint8_t g, uint8_t b) override;
    void colorA(uint8_t a) override;
    void lineWidth(uint16_t width16) override;
    void pointSize(uint16_t size16) override;
    void scissorXY(uint16_t x, uint16_t y) override;
    void scissorSize(uint16_t w, uint16_t h) override;
    void begin(Prim prim) override;
    void end() override;
    void vertex2f(int32_t x16, int32_t y16) override;
    void vertex2ii(uint16_t x, uint16_t y, uint8_t handle, uint8_t cell) override;
    void text(int16_t x, int16_t y, uint8_t font, uint16_t options, const char* s) override;
    void number(int16_t x, int16_t y, uint8_t font, uint16_t options, int32_t n) override;
    void swap() override;
    // Non-blocking swap (2.x M0 / 1.9.5): FIFO consumed and REG_DLSWAP == 0.
    // A display that stays busy for kBusyGiveUpMs reports ready so
    // dlStart()'s bounded wait runs the spec 01 §4.3 fault recovery.
    bool frameReady() override;
    // Cached fragments (spec 05 §3.3 scheme, see hal_api.h): captured with
    // REG_CMD_DL before/after + CMD_MEMCPY(RAM_DL -> RAM_G), drawn with
    // CMD_APPEND. A fragment holds at most kFragSlotBytes.
    bool fragmentsSupported() const override { return ok_; }
    void fragmentBegin(uint8_t slot) override;
    bool fragmentEnd(uint8_t slot) override;
    void fragmentAppend(uint8_t slot) override;
    uint32_t fragmentWords(uint8_t slot) const override;
    // SPI2 TX DMA for the command FIFO uploads (build flag OT_FT81X_DMA,
    // default on; turned off for good after a DMA timeout). Diagnostics.
    static bool dmaActive();
    static uint32_t dmaFaults();
    // Visual-parity primitives: CMD_GRADIENT, VERTEX_TRANSLATE_X (0x2B),
    // SAVE_CONTEXT (0x22) / RESTORE_CONTEXT (0x23), BITMAP_TRANSFORM_A..F
    // (0x15..0x1A), BITMAP_HANDLE + BITMAP_SIZE.
    // bindFontSet: true once init() uploaded and verified the font image
    // (fontdata::kBlob, CRC-32 read back); the set's five fonts and the icon
    // sheet are then set up on kFontHandleBase.. / kIconHandle at every
    // dlStart (BITMAP_* + CMD_SETFONT), which also restores them after a
    // coprocessor reset (recoverCoprocessor).
    bool bindFontSet(int set) override;
    void bitmapTransform(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) override;
    void bitmapSize(uint8_t handle, uint16_t w, uint16_t h) override;
    bool fontsLoaded() const { return fontsOk_; }
    void gradient(int16_t x0, int16_t y0, uint32_t rgb0, int16_t x1, int16_t y1, uint32_t rgb1) override;
    void vertexTranslateX(int32_t x16) override;
    void saveContext() override;
    void restoreContext() override;
    // Screenshot readback (spec 05 §3.4): CMD_SNAPSHOT2 of the shown frame into
    // RAM_G in horizontal bands, then read over SPI. Main loop only.
    bool readPixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t* rgb) override;

    // JPEG into RAM_G (spec 13 §G.6/§I.3): CMD_LOADIMAGE(dest, OPT_NODL), file
    // streamed through the FIFO in <= 1 KB SPI bursts, then CMD_GETPROPS.
    // Waits up to kImageTimeoutMs for the coprocessor; on a fault or timeout
    // it resets the coprocessor (spec 01 §4.3) and returns false. Main loop
    // only, outside dlStart()..swap().
    bool loadJpeg(uint32_t ramgAddr, IStorage& fs, const char* path, uint16_t maxW, uint16_t maxH,
                  uint16_t& w, uint16_t& h) override;
    void drawImage(int16_t x, int16_t y, uint32_t ramgAddr, uint16_t w, uint16_t h) override;
    // Games (spec 14 §3): RAM_G burst write (TX-pipelined SPI) and the
    // magnified RGB332 frame draw on its own bitmap handle.
    bool writeRamG(uint32_t ramgAddr, const void* src, uint32_t len) override;
    void drawRgb332(int16_t x, int16_t y, uint32_t ramgAddr, uint16_t w, uint16_t h, uint8_t scale) override;

    // RAM_G map (1 MiB, 0x000000..0x0FFFFF):
    //   0x000000 .. 0x05DBFF  snapshot band (readPixels), 800 x 240 RGB565 = 384,000 B
    //   0x05DC00 .. 0x06CBFF  Games frame (IDisplay::kRamGGameFrame = 384,000):
    //                         256 x 240 RGB332 = 61,440 B (spec 14 §3.1)
    //   0x06CC00 .. 0x08C4DF  custom UI fonts, both sets + icon sheet
    //                         (fontdata::kRamGBase + kBlobSize, uploaded once by init())
    //   0x08C800 .. 0x09C7FF  cached display-list fragments: 16 slots x 4 KB
    //                         (kFragRamG, IDisplay::kFragmentSlots, spec 05 §3.3)
    //   0x09C800 .. 0x0A392F  free
    //   0x0A3930 .. 0x0F1A2F  image (IDisplay::kRamGCover = 670,000): RGB565,
    //                         <= 400 x 400 x 2 = 320,000 B for the cover
    //   0x0F1A30 .. 0x0FFFFF  free (images may extend up to kRamGSize)
    // Images must start at or above kImageRamGMin so a screenshot never
    // overwrites a loaded image (and vice versa).
    static constexpr uint32_t kSnapRamG = 0x000000;
    static constexpr uint16_t kSnapBandRows = 240;   // 800 x 240 x 2 B = 384,000 B
    static constexpr uint32_t kImageRamGMin = kSnapRamG + 800u * kSnapBandRows * 2u;
    static_assert(IDisplay::kRamGCover >= kImageRamGMin, "cover image overlaps the snapshot band");
    static_assert(IDisplay::kRamGCover + 2u * IDisplay::kCoverMaxSide * IDisplay::kCoverMaxSide <=
                      IDisplay::kRamGSize, "cover image exceeds RAM_G");
    static constexpr uint8_t kImageHandle = 1;       // bitmap handle for drawImage (0..14 are user handles)
    static constexpr uint8_t kGameHandle = 2;        // bitmap handle for drawRgb332
    static_assert(IDisplay::kRamGGameFrame >= kImageRamGMin &&
                      IDisplay::kRamGGameFrame + IDisplay::kGameFrameBytes <= IDisplay::kRamGCover,
                  "Games frame must sit between the snapshot band and the cover image");
    static constexpr uint32_t kImageTimeoutMs = 1000;
    static constexpr uint32_t kBusyGiveUpMs = 100;
    static constexpr uint32_t kFontRamG = fontdata::kRamGBase;
    static_assert(kFontRamG == 0x06CC00, "font image starts the free RAM_G band");
    static_assert(kFontRamG >= kImageRamGMin, "font image overlaps the snapshot band");
    static_assert(kFontRamG >= IDisplay::kRamGGameFrame + IDisplay::kGameFrameBytes,
                  "font image overlaps the Games frame");
    static_assert(fontdata::kRamGEnd <= IDisplay::kRamGCover && fontdata::kRamGEnd <= 0x0A3930u,
                  "font image overlaps the cover image");
    static_assert(IDisplay::kFontHandleBase + IDisplay::kFontHandleCount <= IDisplay::kIconHandle &&
                      IDisplay::kFontHandleBase > kGameHandle && IDisplay::kIconHandle < 15,
                  "font handles collide with the image / Games / scratch handles");
    // Fragments after the font image (1 KB aligned), before the cover image.
    static constexpr uint32_t kFragRamG = (fontdata::kRamGEnd + 0x3FFu) & ~0x3FFu;
    static constexpr uint32_t kFragSlotBytes = 4096;
    static constexpr uint32_t kFragRamGEnd = kFragRamG + kFragSlotBytes * IDisplay::kFragmentSlots;
    static_assert(kFragRamG == 0x08C800 && kFragRamGEnd == 0x09C800, "RAM_G map comment");
    static_assert(kFragRamG >= fontdata::kRamGEnd, "fragment slots overlap the font image");
    static_assert(kFragRamG >= IDisplay::kRamGGameFrame + IDisplay::kGameFrameBytes && kFragRamG >= kImageRamGMin,
                  "fragment slots overlap the Games frame / snapshot band");
    static_assert(kFragRamG % 4 == 0 && kFragRamGEnd <= IDisplay::kRamGCover,
                  "fragment slots must end before the cover image");

    // Last frame's upload statistics (telemetry, docs/HARDWARE_TESTING.md):
    // bytes on the SPI wire for the command FIFO (data + the REG_CMD_READ /
    // REG_CMD_WRITE traffic per chunk), time spent in flush() in us (SPI
    // transfer, or starting the DMA, plus any wait for FIFO space), chunks
    // that had to wait for FIFO space, REG_CMD_READ polls of the dlStart()
    // wait (1 = the frame started without waiting), and the cumulative
    // coprocessor resets.
    uint32_t lastFrameBytes() const { return lastFrameBytes_; }
    uint32_t lastUploadUs() const { return lastUploadUs_; }
    uint32_t lastFifoWaits() const { return lastFifoWaits_; }
    uint32_t lastWaitPolls() const { return lastWaitPolls_; }
    uint32_t faults() const { return faults_; }
    // Faults seen by frameReady() after a frame (REG_CMD_READ = 0xFFF), and
    // the RAM_DL bytes the last drained frame used (REG_CMD_DL; limit 8192)
    // with the window maximum (takeMaxDlBytes resets it).
    uint32_t frameFaults() const { return frameFaults_; }
    uint32_t frameFaultCount() const override { return frameFaults_; }
    uint32_t measuredDlBytes() const override { return lastDlBytes_; }
    bool framePending() const override { return ok_ && dlPending_; }
    uint32_t lastDlBytes() const { return lastDlBytes_; }
    uint32_t takeMaxDlBytes() {
        const uint32_t m = maxDlBytes_;
        maxDlBytes_ = 0;
        return m;
    }

    // Low-level memory access (public for diagnostics).
    void wr8(uint32_t addr, uint8_t v);
    void wr16(uint32_t addr, uint16_t v);
    void wr32(uint32_t addr, uint32_t v);
    uint8_t rd8(uint32_t addr);
    uint16_t rd16(uint32_t addr);
    uint32_t rd32(uint32_t addr);

private:
    void cmd(uint32_t word);
    void cmdBytes(const void* data, uint32_t len);   // padded to 4 bytes
    void flush();
    bool waitIdle();
    void recoverCoprocessor();
    void hostCommand(uint8_t c, uint8_t p);
    bool powerUp(uint8_t clockCmd);    // spec 01 §4.2 steps 2-4
    bool snapshotBand(int band);
    void cmdRaw(const uint8_t* p, uint32_t len);   // unpadded FIFO data
    bool waitIdleMs(uint32_t timeoutMs);
    bool uploadFonts();
    void fontSetup();

    bool waitCoproc();                 // REG_CMD_READ == wp_ (bounded), no DLSWAP condition
    void invalidateFragments();

    // Staging: two halves of a DMA buffer in the .dma section (ft81x.cpp):
    // one is sent by the SPI2 TX DMA while the next commands fill the other.
    static constexpr uint32_t kStage = 512;           // spec 01 §4.3: uploads chunked <= 1000 B
    uint8_t* stage_ = nullptr;
    uint8_t stageHalf_ = 0;
    uint32_t staged_ = 0;
    int8_t capSlot_ = -1;              // fragment being captured
    uint16_t capStart_ = 0;            // REG_CMD_DL at fragmentBegin
    uint16_t fragBytes_[IDisplay::kFragmentSlots] = {};
    uint32_t busySince_ = 0;           // frameReady() false since (ms), 0 = ready
    uint32_t frameBytes_ = 0, uploadUs_ = 0, fifoWaits_ = 0, waitPolls_ = 0;   // current frame
    uint32_t lastFrameBytes_ = 0, lastUploadUs_ = 0, lastFifoWaits_ = 0, lastWaitPolls_ = 0;
    uint16_t wp_ = 0;
    bool ok_ = false;
    bool backlightSet_ = false;
    uint8_t onDuty_ = 0xFF;            // requested duty, 0xFF = board::kLcdBacklightOn
    uint8_t lastId_ = 0;
    uint8_t bootAttempt_ = 0;
    int16_t snapBand_ = -1;            // band held in RAM_G, -1 = none / stale
    uint32_t faults_ = 0;              // coprocessor resets (recoverCoprocessor)
    uint32_t frameFaults_ = 0;         // of those: found by frameReady after a frame
    uint32_t lastDlBytes_ = 0, maxDlBytes_ = 0;
    bool dlPending_ = false;           // a swapped frame's REG_CMD_DL is not read yet
    bool fontsOk_ = false;             // font image in RAM_G, CRC verified
    bool fontsBound_ = false;          // bindFontSet called: set up handles per frame
    uint8_t fontSet_ = 0;
};

}  // namespace hal
}  // namespace ot
