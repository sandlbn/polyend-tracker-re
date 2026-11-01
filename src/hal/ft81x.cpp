// OpenTracker -- FT81x display driver (spec 01 §4). Command encodings follow the
// public BridgeTek FT81x Series Programmer Guide.
#include "ft81x.h"
#include "diag.h"

#include <Arduino.h>
#include <DMAChannel.h>
#include <string.h>
#include "board.h"
#include "dma_mem.h"
#include "formats/crc32.h"
#include "hw_math.h"
#include "k66.h"

namespace ot {
namespace hal {

namespace {
// FT81x memory map / registers (FT81x programmer guide; addresses as in spec 01 §4.2).
constexpr uint32_t RAM_DL = 0x300000, RAM_CMD = 0x308000;
constexpr uint32_t REG_ID = 0x302000, REG_CPURESET = 0x302020, REG_HCYCLE = 0x30202C,
    REG_HOFFSET = 0x302030, REG_HSIZE = 0x302034, REG_HSYNC0 = 0x302038, REG_HSYNC1 = 0x30203C,
    REG_VCYCLE = 0x302040, REG_VOFFSET = 0x302044, REG_VSIZE = 0x302048, REG_VSYNC0 = 0x30204C,
    REG_VSYNC1 = 0x302050, REG_DLSWAP = 0x302054, REG_ROTATE = 0x302058, REG_OUTBITS = 0x30205C,
    REG_DITHER = 0x302060, REG_SWIZZLE = 0x302064, REG_CSPREAD = 0x302068, REG_PCLK_POL = 0x30206C,
    REG_PCLK = 0x302070, REG_VOL_PB = 0x302080, REG_VOL_SOUND = 0x302084, REG_SOUND = 0x302088,
    REG_GPIOX_DIR = 0x302098, REG_GPIOX = 0x30209C, REG_PWM_HZ = 0x3020D0, REG_PWM_DUTY = 0x3020D4,
    REG_CMD_READ = 0x3020F8, REG_CMD_WRITE = 0x3020FC, REG_CMD_DL = 0x302100, REG_TOUCH_RZTHRESH = 0x302118;

constexpr uint32_t CMD_DLSTART = 0xFFFFFF00, CMD_SWAP = 0xFFFFFF01, CMD_TEXT = 0xFFFFFF0C,
    CMD_NUMBER = 0xFFFFFF2E, CMD_SNAPSHOT2 = 0xFFFFFF37, CMD_LOADIMAGE = 0xFFFFFF24,
    CMD_GETPROPS = 0xFFFFFF25, CMD_MEMCPY = 0xFFFFFF1D, CMD_APPEND = 0xFFFFFF1E, CMD_GRADIENT = 0xFFFFFF0B,
    CMD_SETFONT = 0xFFFFFF2B, CMD_INFLATE = 0xFFFFFF22;
constexpr uint32_t DL_END = 0x21000000;
constexpr uint32_t OPT_NODL = 2;     // CMD_LOADIMAGE: no BITMAP_* display-list output
constexpr uint32_t kFmtL4 = 2;       // FT81x bitmap format L4 (custom fonts, icons)
constexpr uint32_t kFmtRgb565 = 7;   // FT81x bitmap format RGB565 (programmer guide BITMAP_LAYOUT)
constexpr uint32_t RAM_G_SIZE = IDisplay::kRamGSize;
static_assert(RAM_G_SIZE == 0x100000, "FT81x RAM_G is 1 MiB");
// Snapshot geometry. RGB565 (2 B/px) keeps the whole frame under 1 MB and avoids
// the ARGB8 snapshot's width convention; the panel is 18-bit (REG_OUTBITS 0x1B6),
// so only the lowest red/blue bit is lost.
constexpr uint32_t kSnapRowBytes = uint32_t(board::kLcdWidth) * 2;
static_assert(Ft81xDisplay::kSnapRamG + kSnapRowBytes * Ft81xDisplay::kSnapBandRows <= RAM_G_SIZE,
              "snapshot band must fit RAM_G");
// Verified on hardware (2026-10-16, 1.9.3): CMD_SNAPSHOT2 already returns the
// frame in UI coordinates even with REG_ROTATE = 1 (180 degrees, spec 01 §4.2);
// flipping it here turned screenshots upside down. No flip.
constexpr bool kSnapshotFlipped = false;
constexpr uint32_t DL_DISPLAY = 0x00000000;
constexpr uint32_t kIdPollMax = 100;   // spec allows 0xFFFE x 20 ms; we give up after ~2 s
// FT81x datasheet §4.1.4 / programmer guide §2.3: SPI clock < 11 MHz until the
// chip runs from its system clock; 30 MHz max afterwards.
static_assert(hw::spiSckHz(board::kBusHz, board::kSpi2CtarSlow) < 11000000, "FT81x boot SPI too fast");
static_assert(hw::spiSckHz(board::kBusHz, board::kSpi2CtarFast) <= 30000000, "FT81x SPI > 30 MHz");
// Host commands (FT81x datasheet §4.1.5, table 4-11).
constexpr uint8_t HC_ACTIVE = 0x00, HC_CLKEXT = 0x44;

// ---- SPI2 TX DMA (2.x performance workstream "SPI2 DMA") --------------------
// Same scheme as the stock driver's NES upload (spec 01 §4.3): DMAMUX source
// SPI2_TX, 8-bit beats into SPI2_PUSHR, RSER TFFF_RE | TFFF_DIRS. The CPU
// sends the 3 address bytes, the DMA all data bytes but the last, and the CPU
// the last one with EOQ, whose EOQF marks the end of the transfer before CS
// rises (RX overflow is ignored: MCR ROOE = 0, the RX FIFO is flushed after).
// A transfer is left running when flush() returns; the next SPI2 access
// (csLow) completes it first, then writes the pending REG_CMD_WRITE.
#ifndef OT_FT81X_DMA
#define OT_FT81X_DMA 1
#endif
#ifndef OT_DMA_BUF
#ifdef OT_DMA_MEM
#define OT_DMA_BUF OT_DMA_MEM
#else
#define OT_DMA_BUF __attribute__((aligned(32)))
#endif
#endif
constexpr uint32_t kDmaMinBytes = 16;     // shorter runs: CPU burst
constexpr uint32_t kMcrRun = 0x801F0000;  // spec 01 §4.1 running MCR (master, PCSIS, ROOE = 0)
constexpr uint32_t kDmaPollMax = 400000;  // >> 512 B at 30 MHz; a stuck DMA turns DMA off
OT_DMA_BUF uint8_t gStage[2][512];        // Ft81xDisplay::kStage x 2 (the .dma section, dma_mem.h)
DMAChannel gDma(false);
bool gDmaReady = false;                   // channel allocated
bool gDmaOff = OT_FT81X_DMA == 0;
bool gDmaBusy = false;                    // transfer running, CS low
bool gDmaFaulted = false;                 // set by a timeout; the driver resets the coprocessor
uint8_t gDmaLast = 0;
bool gDmaVerified = false;                // first transfer read back and compared
uint32_t gDmaAddr = 0;
const uint8_t* gDmaSrc = nullptr;
uint32_t gDmaLen = 0;
bool gPendingWp = false;
uint16_t gPendingWpVal = 0;
uint32_t gDmaFaultCount = 0;
void dmaFinish();

inline void csLow() {
    if (gDmaBusy) dmaFinish();
    digitalWriteFast(board::kTeensyFtCs, LOW);
}
inline void csHigh() { digitalWriteFast(board::kTeensyFtCs, HIGH); }

// SPI2 has a 1-entry FIFO on the K66, so transfers are strictly byte-by-byte
// (spec 01 §4.1: "polled per byte").
inline uint8_t xfer(uint8_t b) {
    SPI2_SR = SPI_SR_RFDF | SPI_SR_TCF;
    SPI2_PUSHR = b | SPI_PUSHR_CTAS(0);
    // One byte takes 1.8 us at 4.5 MHz; bounded so a mis-clocked module can
    // never hang the boot (it then reads as 0xFF/0 and REG_ID fails).
    for (uint32_t n = 0; !(SPI2_SR & SPI_SR_RFDF) && n < 20000; n++) {}
    uint8_t r = uint8_t(SPI2_POPR);
    SPI2_SR = SPI_SR_RFDF;
    return r;
}
inline void addrWrite(uint32_t a) {  // spec 01 §4.1 write framing
    xfer(uint8_t(0x80 | ((a >> 16) & 0x3F)));
    xfer(uint8_t(a >> 8));
    xfer(uint8_t(a));
}
inline void addrRead(uint32_t a) {   // spec 01 §4.1 read framing (+ dummy)
    xfer(uint8_t((a >> 16) & 0x3F));
    xfer(uint8_t(a >> 8));
    xfer(uint8_t(a));
    xfer(0);
}
void memWrite(uint32_t a, const uint8_t* p, uint32_t n) {
    csLow();
    addrWrite(a);
    while (n--) xfer(*p++);
    csHigh();
}
void memRead(uint32_t a, uint8_t* p, uint32_t n) {
    csLow();
    addrRead(a);
    while (n--) *p++ = xfer(0);
    csHigh();
}

// Burst write (Games frames, spec 14 §3.1; the command FIFO uploads of
// flush()): the next byte is queued in the 1-entry TX FIFO while the
// previous one shifts out, and every received byte is drained, so the RX
// FIFO never holds a stale byte for the next xfer(). About 0.3 us per byte
// at 30 MHz instead of a full push/wait/pop round trip. Bounded polls like
// xfer(). The boot font upload goes through this path and is read back and
// CRC-checked (uploadFonts), which verifies it on every start.
void memWriteBurst(uint32_t a, const uint8_t* p, uint32_t n) {
    csLow();
    addrWrite(a);
    if (n) {
        SPI2_SR = SPI_SR_RFDF | SPI_SR_TCF | SPI_SR_TFFF | SPI_SR_RFOF;
        SPI2_PUSHR = p[0] | SPI_PUSHR_CTAS(0);
        for (uint32_t i = 1; i < n; ++i) {
            for (uint32_t g = 0; !(SPI2_SR & SPI_SR_TFFF) && g < 20000; g++) {}
            SPI2_PUSHR = p[i] | SPI_PUSHR_CTAS(0);
            SPI2_SR = SPI_SR_TFFF;
            // RX of byte i - 1 (fully shifted out). An interrupt longer than
            // a byte time overflows the 1-entry RX FIFO (RFOF; the byte is
            // discarded, ROOE = 0): the pop then takes the next byte, which
            // keeps the loop aligned, and the end below does not wait for a
            // byte that never arrives.
            for (uint32_t g = 0; !(SPI2_SR & SPI_SR_RFDF) && g < 20000; g++) {}
            (void)SPI2_POPR;
            SPI2_SR = SPI_SR_RFDF;
        }
        // Only byte n - 1 can still be in flight: wait for its transfer
        // complete flag (cleared now that its predecessor was popped), a few
        // byte times at most (it may have completed during an interrupt),
        // then drain the RX FIFO and clear the flags.
        SPI2_SR = SPI_SR_TCF;
        for (uint32_t g = 0; !(SPI2_SR & SPI_SR_TCF) && g < 2000; g++) {}
        if (SPI2_SR & SPI_SR_RFDF) (void)SPI2_POPR;
        SPI2_SR = SPI_SR_RFDF | SPI_SR_RFOF | SPI_SR_TCF;
    }
    csHigh();
}

void spiSetup(uint32_t ctar) {
    SPI2_MCR = 0x001F4001;                 // spec 01 §4.1: halt
    SPI2_CTAR0 = ctar;
    SPI2_MCR = 0x801F0000 | SPI_MCR_CLR_TXF | SPI_MCR_CLR_RXF;  // master, running
}

bool dmaSetup() {
    if (gDmaReady) return true;
    if (gDmaOff) return false;
    SIM_SCGC6 |= SIM_SCGC6_DMAMUX;
    SIM_SCGC7 |= SIM_SCGC7_DMA;
    gDma.begin(true);
    gDma.disable();
    gDma.TCD->CSR = 0;
    gDma.triggerAtHardwareEvent(DMAMUX_SOURCE_SPI2_TX);   // spec 01 §4.3: DMAMUX 0xA7, source 39
    gDmaReady = true;
    diagLogf("ft81x: SPI2 TX DMA on channel %u", unsigned(gDma.channel));
    return true;
}

// Starts the write of p[0..n) to FT81x address a (n >= kDmaMinBytes); CS
// stays low until dmaFinish().
void dmaStart(uint32_t a, const uint8_t* p, uint32_t n) {
    csLow();
    addrWrite(a);   // polled: leaves the RX FIFO empty and the flags clear
    SPI2_SR = SPI_SR_RFDF | SPI_SR_RFOF | SPI_SR_TCF | SPI_SR_TFFF | SPI_SR_EOQF;
    gDmaLast = p[n - 1];
    gDmaAddr = a;
    gDmaSrc = p;
    gDmaLen = n;
    gDma.TCD->SADDR = p;
    gDma.TCD->SOFF = 1;
    gDma.TCD->ATTR = DMA_TCD_ATTR_SSIZE(0) | DMA_TCD_ATTR_DSIZE(0);
    gDma.TCD->NBYTES_MLNO = 1;
    gDma.TCD->SLAST = 0;
    gDma.TCD->DADDR = reinterpret_cast<volatile uint8_t*>(&SPI2_PUSHR);
    gDma.TCD->DOFF = 0;
    gDma.TCD->CITER_ELINKNO = uint16_t(n - 1);
    gDma.TCD->BITER_ELINKNO = uint16_t(n - 1);
    gDma.TCD->DLASTSGA = 0;
    gDma.TCD->CSR = DMA_TCD_CSR_DREQ;      // ERQ off at the end of the major loop
    __asm__ volatile("dsb" ::: "memory");  // staged bytes in SRAM before the DMA reads them
    gDmaBusy = true;
    SPI2_RSER = SPI_RSER_TFFF_RE | SPI_RSER_TFFF_DIRS;
    gDma.enable();
}

void dmaAbort() {
    gDma.disable();
    SPI2_RSER = 0;
    gDma.clearComplete();
    if (DMA_ERR & (1u << gDma.channel)) DMA_CERR = gDma.channel;
    spiSetup(board::kSpi2CtarFast);
    csHigh();
    gDmaBusy = false;
    gPendingWp = false;
    gDmaOff = true;
    gDmaFaulted = true;
    ++gDmaFaultCount;
}

void dmaFinish() {
    if (!gDmaBusy) return;
    uint32_t n = 0;
    while (!(gDma.TCD->CSR & DMA_TCD_CSR_DONE) && n < kDmaPollMax) {
        if (DMA_ERR & (1u << gDma.channel)) { n = kDmaPollMax; break; }
        ++n;
    }
    if (n >= kDmaPollMax) {
        dmaAbort();
        diagLog("ft81x: SPI2 DMA timeout/error, DMA off");
        return;
    }
    gDma.clearComplete();
    SPI2_RSER = 0;
    // Last DMA byte left the TX FIFO, then the last byte with EOQ.
    for (n = 0; (SPI2_SR & 0xF000u) && n < 20000; n++) {}
    SPI2_SR = SPI_SR_EOQF;
    SPI2_PUSHR = gDmaLast | SPI_PUSHR_EOQ | SPI_PUSHR_CTAS(0);
    for (n = 0; !(SPI2_SR & SPI_SR_EOQF) && n < 20000; n++) {}
    const bool eoq = (SPI2_SR & SPI_SR_EOQF) != 0;
    SPI2_MCR = kMcrRun | SPI_MCR_CLR_RXF;  // drop the RX bytes nobody read
    SPI2_SR = SPI_SR_EOQF | SPI_SR_RFDF | SPI_SR_RFOF | SPI_SR_TCF | SPI_SR_TFFF;   // EOQF clear restarts
    csHigh();
    gDmaBusy = false;
    if (!eoq) {
        dmaAbort();
        diagLog("ft81x: SPI2 DMA end of queue missing, DMA off");
        return;
    }
    if (!gDmaVerified) {
        // Once per boot, before the coprocessor sees the data: read the
        // segment back from RAM_CMD (UNVERIFIED path, spec 01 §4.3 only
        // documents the stock NES upload) and fall back on any mismatch.
        gDmaVerified = true;
        uint8_t back[64];
        const uint32_t m = gDmaLen < sizeof(back) ? gDmaLen : uint32_t(sizeof(back));
        memRead(gDmaAddr, back, m);
        if (memcmp(back, gDmaSrc, m) != 0) {
            dmaAbort();
            diagLog("ft81x: SPI2 DMA read-back mismatch, DMA off");
            return;
        }
        diagLog("ft81x: SPI2 DMA read-back ok");
    }
    if (gPendingWp) {   // spec 01 §4.3: REG_CMD_WRITE after the data
        gPendingWp = false;
        const uint8_t b[2] = {uint8_t(gPendingWpVal), uint8_t(gPendingWpVal >> 8)};
        memWrite(REG_CMD_WRITE, b, 2);
    }
}

// RAM_CMD segment write: DMA (left running) or the CPU burst.
void sendSegment(uint32_t a, const uint8_t* p, uint32_t n) {
    if (n >= kDmaMinBytes && dmaSetup()) dmaStart(a, p, n);
    else memWriteBurst(a, p, n);
}
}  // namespace

bool Ft81xDisplay::dmaActive() { return gDmaReady && !gDmaOff; }
uint32_t Ft81xDisplay::dmaFaults() { return gDmaFaultCount; }

void Ft81xDisplay::wr8(uint32_t a, uint8_t v) { memWrite(a, &v, 1); }
void Ft81xDisplay::wr16(uint32_t a, uint16_t v) {
    uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
    memWrite(a, b, 2);
}
void Ft81xDisplay::wr32(uint32_t a, uint32_t v) {
    uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    memWrite(a, b, 4);
}
uint8_t Ft81xDisplay::rd8(uint32_t a) { uint8_t v; memRead(a, &v, 1); return v; }
uint16_t Ft81xDisplay::rd16(uint32_t a) {
    uint8_t b[2];
    memRead(a, b, 2);
    return uint16_t(b[0] | (b[1] << 8));
}
uint32_t Ft81xDisplay::rd32(uint32_t a) {
    uint8_t b[4];
    memRead(a, b, 4);
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}

void Ft81xDisplay::hostCommand(uint8_t c, uint8_t p) {
    csLow();
    xfer(c);
    xfer(p);
    xfer(0);
    csHigh();
}

bool Ft81xDisplay::powerUp(uint8_t clockCmd) {
    // spec 01 §4.2 step 2: PD_N pulse.
    diagLogf("ft81x: power-up attempt, clock cmd 0x%02X; PD_N pulse", unsigned(clockCmd));
    delay(20);
    digitalWriteFast(board::kTeensyFtPd, LOW);
    delay(20);
    digitalWriteFast(board::kTeensyFtPd, HIGH);
    delay(40);
    // step 3: optional clock selection, then ACTIVE (programmer guide §2.3).
    if (clockCmd) {
        hostCommand(clockCmd, 0x00);
        delay(5);
    }
    diagLog("ft81x: host command ACTIVE sent");
    hostCommand(HC_ACTIVE, 0x00);
    // step 4: REG_ID == 0x7C, then REG_CPURESET == 0 (all engines out of reset).
    uint32_t tries = 0;
    while ((lastId_ = rd8(REG_ID)) != 0x7C) {
        if (tries < 3 || tries % 20 == 0) diagLogf("ft81x: REG_ID read %lu = 0x%02X", static_cast<unsigned long>(tries), unsigned(lastId_));
        if (++tries > kIdPollMax) { diagLog("ft81x: REG_ID timeout"); return false; }
        delay(20);
    }
    diagLogf("ft81x: REG_ID 0x7C after %lu polls", static_cast<unsigned long>(tries));
    tries = 0;
    uint8_t cpu;
    while ((cpu = rd8(REG_CPURESET)) != 0) {
        if (++tries > kIdPollMax) { diagLogf("ft81x: REG_CPURESET stuck 0x%02X", unsigned(cpu)); return false; }
        delay(20);
    }
    diagLog("ft81x: REG_CPURESET 0, engines running");
    return true;
}

bool Ft81xDisplay::init() {
    // spec 01 §4.1 / §4.2 step 1
    SIM_SCGC3 |= SIM_SCGC3_SPI2;
    digitalWriteFast(board::kTeensyFtCs, LOW);
    digitalWriteFast(board::kTeensyFtPd, LOW);
    pinMode(board::kTeensyFtCs, OUTPUT);
    pinMode(board::kTeensyFtPd, OUTPUT);
    pcr(board::kSpi2Sck) = board::kSpi2Pcr;
    pcr(board::kSpi2Sout) = board::kSpi2Pcr;
    pcr(board::kSpi2Sin) = board::kSpi2Pcr;
    csHigh();
    digitalWriteFast(board::kTeensyFtPd, HIGH);
    spiSetup(board::kSpi2CtarSlow);
    diagLog("ft81x: SPI2 slow configured");
    // Attempt 0 is the stock sequence (spec 01 §4.2: internal oscillator, no
    // CLKEXT). If REG_ID never reads 0x7C, attempt 1 power-cycles via PD_N and
    // selects the external crystal/clock (CLKEXT before ACTIVE, programmer guide
    // §2.3) in case this board revision has one. On a board without a crystal,
    // attempt 1 simply fails the same way.
    bootAttempt_ = 0;
    for (; bootAttempt_ < 2; bootAttempt_++) {
        if (powerUp(bootAttempt_ == 1 ? HC_CLKEXT : 0)) break;
    }
    if (bootAttempt_ >= 2) { ok_ = false; return false; }
    // step 5, in spec order
    diagLog("ft81x: timing registers");
    wr8(REG_PWM_DUTY, 0);
    wr16(REG_PWM_HZ, board::kLcdPwmHz);
    wr16(REG_GPIOX, uint16_t(rd16(REG_GPIOX) | 0x8000));
    wr16(REG_GPIOX_DIR, uint16_t(rd16(REG_GPIOX_DIR) | 0x8000));
    wr16(REG_HSIZE, board::kLcdWidth);
    wr16(REG_HCYCLE, 1056);
    wr16(REG_HOFFSET, 46);
    wr16(REG_HSYNC0, 0);
    wr16(REG_HSYNC1, 10);
    wr16(REG_VSIZE, board::kLcdHeight);
    wr16(REG_VCYCLE, 525);
    wr16(REG_VOFFSET, 23);
    wr16(REG_VSYNC0, 0);
    wr16(REG_VSYNC1, 10);
    wr8(REG_SWIZZLE, 0);
    wr8(REG_PCLK_POL, 1);
    wr8(REG_CSPREAD, 0);
    wr8(REG_ROTATE, board::kLcdRotate);
    wr8(REG_DITHER, 1);
    wr16(REG_OUTBITS, 0x1B6);
    wr32(RAM_DL + 0, 0x02000000);   // CLEAR_COLOR_RGB(0,0,0)
    wr32(RAM_DL + 4, 0x26000007);   // CLEAR(1,1,1)
    wr32(RAM_DL + 8, DL_DISPLAY);
    wr32(REG_DLSWAP, 2);
    wr16(REG_TOUCH_RZTHRESH, 1200);
    wr8(REG_VOL_PB, 0);
    wr8(REG_VOL_SOUND, 0);
    wr16(REG_SOUND, 0x6000);
    wr8(REG_PCLK, 2);               // last: starts scan-out
    // step 6
    diagLog("ft81x: scan-out started, SPI fast");
    spiSetup(board::kSpi2CtarFast);
    wp_ = uint16_t(rd16(REG_CMD_WRITE) & 0xFFF);
    stage_ = gStage[0];
    stageHalf_ = 0;
    staged_ = 0;
    ok_ = true;
    diagLogf("ft81x: REG_CMD_WRITE 0x%03X, REG_ID 0x%02X", unsigned(wp_), unsigned(rd8(REG_ID)));
    fontsOk_ = uploadFonts();
    return true;
}

bool Ft81xDisplay::uploadFonts() {
    // Both font sets and the icon sheet go to RAM_G once (spec 20 §2.1);
    // RAM_G survives coprocessor resets, only the handles are set up again.
#if OT_FONTS_COMPRESSED
    // 2.x flash budget: the image is stored zlib-compressed and inflated by
    // the coprocessor (programmer guide CMD_INFLATE(ptr) + zlib stream,
    // padded to 4 bytes). UNVERIFIED on hardware: any failure shows as a CRC
    // mismatch below and the UI keeps the ROM fonts.
    cmd(CMD_INFLATE);
    cmd(kFontRamG);
    cmdBytes(fontdata::kBlobZ, fontdata::kBlobZSize);
    flush();
    if (!waitIdleMs(kImageTimeoutMs)) return false;
#else
    if (!writeRamG(kFontRamG, fontdata::kBlob, fontdata::kBlobSize)) return false;
#endif
    uint8_t buf[256];
    uint32_t crc = crc32Init();
    for (uint32_t off = 0; off < fontdata::kBlobSize; off += sizeof(buf)) {
        uint32_t n = fontdata::kBlobSize - off;
        if (n > sizeof(buf)) n = sizeof(buf);
        memRead(kFontRamG + off, buf, n);
        crc = crc32Update(crc, buf, n);
    }
    crc = crc32Final(crc);
    const bool ok = crc == fontdata::kBlobCrc;
    diagLogf("ft81x: fonts %lu B at 0x%06lX, crc 0x%08lX %s", static_cast<unsigned long>(fontdata::kBlobSize),
             static_cast<unsigned long>(kFontRamG), static_cast<unsigned long>(crc),
             ok ? "ok" : "MISMATCH (ROM fonts)");
    return ok;
}

void Ft81xDisplay::fontSetup() {
    // Per frame, right after CMD_DLSTART: BITMAP_HANDLE/SOURCE/LAYOUT/SIZE of
    // the five font handles and the icon handle, then CMD_SETFONT so CMD_TEXT
    // can use the fonts (legacy metric block, pointer = cell of char 0).
    for (uint8_t i = 0; i < IDisplay::kFontHandleCount; ++i) {
        const fontdata::FontInfo& f = fontdata::info(fontSet_, i);
        const uint8_t h = uint8_t(IDisplay::kFontHandleBase + i);
        cmd((0x05u << 24) | h);
        cmd((0x01u << 24) | (fontdata::sourceAddr(f) & 0x3FFFFFu));
        cmd(0x28u << 24);   // BITMAP_LAYOUT_H 0
        cmd((0x07u << 24) | (kFmtL4 << 19) | ((uint32_t(f.stride) & 0x3FFu) << 9) | (f.h & 0x1FFu));
        cmd(0x29u << 24);   // BITMAP_SIZE_H 0
        cmd((0x08u << 24) | ((uint32_t(f.w) & 0x1FFu) << 9) | (f.h & 0x1FFu));
        cmd(CMD_SETFONT);
        cmd(h);
        cmd(kFontRamG + f.metric);
    }
    const fontdata::IconSheet& ic = fontdata::kIcons;
    cmd((0x05u << 24) | IDisplay::kIconHandle);
    cmd((0x01u << 24) | ((kFontRamG + ic.offset) & 0x3FFFFFu));
    cmd(0x28u << 24);
    cmd((0x07u << 24) | (kFmtL4 << 19) | ((uint32_t(ic.stride) & 0x3FFu) << 9) | (ic.h & 0x1FFu));
    cmd(0x29u << 24);
    cmd((0x08u << 24) | ((uint32_t(ic.w) & 0x1FFu) << 9) | (ic.h & 0x1FFu));
}

void Ft81xDisplay::setBacklight(uint8_t duty) {
    onDuty_ = duty > 128 ? 128 : duty;
    if (!ok_ || !backlightSet_) return;   // applied by the first draw (swap)
    wr8(REG_PWM_DUTY, onDuty_);
}
void Ft81xDisplay::setRotate(uint8_t rot) {
    if (ok_) wr8(REG_ROTATE, rot);
}
void Ft81xDisplay::displayOff() {
    if (!ok_) return;
    // spec 01 §4.3 "Display off"
    wr8(REG_PWM_DUTY, 0);
    wr32(RAM_DL + 0, 0x02000000);
    wr32(RAM_DL + 4, 0x26000007);
    wr32(RAM_DL + 8, DL_DISPLAY);
    wr32(REG_DLSWAP, 2);
    wr8(REG_PCLK, 10);
    wr16(REG_GPIOX, uint16_t(rd16(REG_GPIOX) & ~0x8000));
    backlightSet_ = false;
}

void Ft81xDisplay::recoverCoprocessor() {
    // spec 01 §4.3 fault recovery
    gPendingWp = false;   // the pointers are reset below
    wr8(REG_CPURESET, 1);
    wr16(REG_CMD_READ, 0);
    wr16(REG_CMD_WRITE, 0);
    wr8(REG_CPURESET, 0);
    wp_ = 0;
    staged_ = 0;
    capSlot_ = -1;
    invalidateFragments();   // a MEMCPY may not have run
    ++faults_;
}

void Ft81xDisplay::invalidateFragments() {
    for (uint16_t& b : fragBytes_) b = 0;
}

bool Ft81xDisplay::waitIdleMs(uint32_t timeoutMs) {
    // Like waitIdle, but bounded by time: a JPEG decode keeps the coprocessor
    // busy for tens of milliseconds.
    const uint32_t t0 = millis();
    for (;;) {
        const uint16_t rp = rd16(REG_CMD_READ);
        if (rp == 0xFFF) { recoverCoprocessor(); return false; }
        if (rp == wp_) return true;
        if (millis() - t0 > timeoutMs) { recoverCoprocessor(); return false; }
    }
}

bool Ft81xDisplay::waitIdle() {
    for (uint32_t polls = 0; polls < 100000; polls++) {
        ++waitPolls_;
        uint16_t rp = rd16(REG_CMD_READ);
        if (rp == 0xFFF) { recoverCoprocessor(); return false; }
        if (rp == wp_ && rd8(REG_DLSWAP) == 0) return true;
    }
    recoverCoprocessor();
    return false;
}

void Ft81xDisplay::flush() {
    // spec 01 §4.3: chunks wait for FIFO space, data at RAM_CMD + wp, then
    // REG_CMD_WRITE. The last DMA segment is still running on return; its
    // REG_CMD_WRITE follows when it completes (dmaFinish). Per chunk the
    // register traffic is 6 B (REG_CMD_READ) + 5 B (REG_CMD_WRITE) on the
    // wire, counted into frameBytes_.
    if (!staged_) return;
    const uint32_t t0 = micros();
    uint32_t off = 0;
    while (off < staged_) {
        uint32_t want = staged_ - off;
        uint32_t freeBytes = 0;
        for (uint32_t polls = 0;; polls++) {
            uint16_t rp = rd16(REG_CMD_READ);
            frameBytes_ += 6;
            if (rp == 0xFFF || polls > 100000) { recoverCoprocessor(); return; }
            freeBytes = 4092u - ((uint32_t(wp_) - rp) & 0xFFFu);
            if (freeBytes >= 4 && (freeBytes >= want || freeBytes >= 256)) break;
            ++fifoWaits_;
        }
        uint32_t n = want < freeBytes ? want : (freeBytes & ~3u);
        uint32_t first = 4096u - wp_;
        if (first > n) first = n;
        sendSegment(RAM_CMD + wp_, stage_ + off, first);
        if (n > first) sendSegment(RAM_CMD, stage_ + off + first, n - first);
        frameBytes_ += n + 3u + (n > first ? 3u : 0u);
        wp_ = uint16_t((wp_ + n) & 0xFFF);
        if (gDmaBusy) {
            gPendingWp = true;
            gPendingWpVal = wp_;
        } else {
            wr16(REG_CMD_WRITE, wp_);
        }
        frameBytes_ += 5;
        off += n;
        if (gDmaFaulted) {   // the FIFO holds a partial command: start over
            gDmaFaulted = false;
            recoverCoprocessor();
            return;
        }
    }
    staged_ = 0;
    // The DMA may still read this half: stage the next commands in the other.
    if (gDmaBusy) {
        stageHalf_ ^= 1;
        stage_ = gStage[stageHalf_];
    }
    uploadUs_ += micros() - t0;
}

void Ft81xDisplay::cmd(uint32_t w) {
    if (!ok_) return;
    if (staged_ + 4 > kStage) flush();
    stage_[staged_++] = uint8_t(w);
    stage_[staged_++] = uint8_t(w >> 8);
    stage_[staged_++] = uint8_t(w >> 16);
    stage_[staged_++] = uint8_t(w >> 24);
}

void Ft81xDisplay::cmdBytes(const void* data, uint32_t len) {
    if (!ok_) return;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t padded = (len + 3) & ~3u;
    for (uint32_t i = 0; i < padded; i++) {
        if (staged_ >= kStage) flush();
        stage_[staged_++] = i < len ? p[i] : 0;
    }
}

void Ft81xDisplay::cmdRaw(const uint8_t* p, uint32_t len) {
    if (!ok_) return;
    while (len) {
        if (staged_ >= kStage) flush();
        uint32_t n = kStage - staged_;
        if (n > len) n = len;
        memcpy(stage_ + staged_, p, n);
        staged_ += n;
        p += n;
        len -= n;
    }
}

uint16_t Ft81xDisplay::width() const { return board::kLcdWidth; }
uint16_t Ft81xDisplay::height() const { return board::kLcdHeight; }

void Ft81xDisplay::dlStart() {
    if (!ok_) return;
    flush();
    // Per-frame statistics (telemetry): start the window here so the frame
    // counts its own uploads and the polls of the wait below (one when
    // frameReady() was true just before).
    frameBytes_ = uploadUs_ = fifoWaits_ = waitPolls_ = 0;
    waitIdle();        // spec 01 §4.3 frame flow: wait for READ == WRITE
    cmd(CMD_DLSTART);
    // Every frame (the coprocessor's font table is lost on recoverCoprocessor).
    if (fontsOk_ && fontsBound_) fontSetup();
}
void Ft81xDisplay::clearColorRGB(uint8_t r, uint8_t g, uint8_t b) {
    cmd((0x02u << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b);
}
void Ft81xDisplay::clear(bool c, bool s, bool t) {
    cmd((0x26u << 24) | (c ? 4u : 0u) | (s ? 2u : 0u) | (t ? 1u : 0u));
}
void Ft81xDisplay::colorRGB(uint8_t r, uint8_t g, uint8_t b) {
    cmd((0x04u << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b);
}
void Ft81xDisplay::colorA(uint8_t a) { cmd((0x10u << 24) | a); }
void Ft81xDisplay::lineWidth(uint16_t w) { cmd((0x0Eu << 24) | (w & 0xFFFu)); }
void Ft81xDisplay::pointSize(uint16_t s) { cmd((0x0Du << 24) | (s & 0x1FFFu)); }
void Ft81xDisplay::scissorXY(uint16_t x, uint16_t y) {
    cmd((0x1Bu << 24) | ((uint32_t(x) & 0x7FFu) << 11) | (y & 0x7FFu));
}
void Ft81xDisplay::scissorSize(uint16_t w, uint16_t h) {
    cmd((0x1Cu << 24) | ((uint32_t(w) & 0xFFFu) << 12) | (h & 0xFFFu));
}
void Ft81xDisplay::begin(Prim prim) { cmd((0x1Fu << 24) | (prim & 0xFu)); }
void Ft81xDisplay::end() { cmd(0x21u << 24); }
void Ft81xDisplay::vertex2f(int32_t x, int32_t y) {
    cmd((1u << 30) | ((uint32_t(x) & 0x7FFFu) << 15) | (uint32_t(y) & 0x7FFFu));
}
void Ft81xDisplay::vertex2ii(uint16_t x, uint16_t y, uint8_t h, uint8_t c) {
    cmd((2u << 30) | ((uint32_t(x) & 0x1FFu) << 21) | ((uint32_t(y) & 0x1FFu) << 12) |
        ((uint32_t(h) & 0x1Fu) << 7) | (c & 0x7Fu));
}
void Ft81xDisplay::text(int16_t x, int16_t y, uint8_t font, uint16_t opt, const char* s) {
    cmd(CMD_TEXT);
    cmd(uint32_t(uint16_t(x)) | (uint32_t(uint16_t(y)) << 16));
    cmd(uint32_t(font) | (uint32_t(opt) << 16));
    cmdBytes(s, uint32_t(strlen(s)) + 1);   // includes the terminator
}
void Ft81xDisplay::number(int16_t x, int16_t y, uint8_t font, uint16_t opt, int32_t n) {
    cmd(CMD_NUMBER);
    cmd(uint32_t(uint16_t(x)) | (uint32_t(uint16_t(y)) << 16));
    cmd(uint32_t(font) | (uint32_t(opt) << 16));
    cmd(uint32_t(n));
}
// Visual-parity primitives (programmer guide encodings).
bool Ft81xDisplay::bindFontSet(int set) {
    // The image holds both sets: binding only selects which one dlStart sets
    // up. Without a verified upload the UI keeps the ROM font roles.
    fontSet_ = set == 1 ? 1 : 0;
    fontsBound_ = ok_ && fontsOk_;
    return fontsBound_;
}
void Ft81xDisplay::bitmapTransform(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) {
    cmd((0x15u << 24) | (uint32_t(a) & 0x1FFFFu));
    cmd((0x16u << 24) | (uint32_t(b) & 0x1FFFFu));
    cmd((0x17u << 24) | (uint32_t(c) & 0xFFFFFFu));
    cmd((0x18u << 24) | (uint32_t(d) & 0x1FFFFu));
    cmd((0x19u << 24) | (uint32_t(e) & 0x1FFFFu));
    cmd((0x1Au << 24) | (uint32_t(f) & 0xFFFFFFu));
}
void Ft81xDisplay::bitmapSize(uint8_t handle, uint16_t w, uint16_t h) {
    cmd((0x05u << 24) | (handle & 0x1Fu));
    cmd((0x08u << 24) | ((uint32_t(w) & 0x1FFu) << 9) | (h & 0x1FFu));
}
void Ft81xDisplay::gradient(int16_t x0, int16_t y0, uint32_t rgb0, int16_t x1, int16_t y1, uint32_t rgb1) {
    cmd(CMD_GRADIENT);
    cmd(uint32_t(uint16_t(x0)) | (uint32_t(uint16_t(y0)) << 16));
    cmd(rgb0 & 0xFFFFFFu);
    cmd(uint32_t(uint16_t(x1)) | (uint32_t(uint16_t(y1)) << 16));
    cmd(rgb1 & 0xFFFFFFu);
}
void Ft81xDisplay::vertexTranslateX(int32_t x16) { cmd((0x2Bu << 24) | (uint32_t(x16) & 0x1FFFFu)); }
void Ft81xDisplay::saveContext() { cmd(0x22u << 24); }
void Ft81xDisplay::restoreContext() { cmd(0x23u << 24); }
void Ft81xDisplay::swap() {
    if (!ok_) { staged_ = 0; return; }
    cmd(DL_DISPLAY);
    cmd(CMD_SWAP);
    flush();
    // Submit the frame now: flush() leaves its last DMA segment running and
    // REG_CMD_WRITE for it pending until the next SPI2 access (csLow ->
    // dmaFinish). Nothing else touches the display while the UI is not
    // dirty, so the coprocessor would never see this frame's CMD_SWAP and
    // the screen would keep the previous one (2.0.0 field bug: "Loading
    // projects list" / boot progress popups stayed up after their job).
    // The tail is at most kStage bytes (< 0.2 ms at 30 MHz).
    if (gDmaBusy) dmaFinish();
    lastFrameBytes_ = frameBytes_;
    lastUploadUs_ = uploadUs_;
    lastFifoWaits_ = fifoWaits_;
    lastWaitPolls_ = waitPolls_;
    snapBand_ = -1;          // a new frame: cached snapshot band is stale
    dlPending_ = true;       // frameReady reads its REG_CMD_DL once drained
    if (!backlightSet_) {   // spec 01 §4.2: backlight on at the first draw
        backlightSet_ = true;
        setBacklight(onDuty_ == 0xFF ? board::kLcdBacklightOn : onDuty_);
    }
}

bool Ft81xDisplay::frameReady() {
    // The stock frame flow (spec 01 §4.3) only waits for READ == WRITE; the
    // coprocessor itself holds CMD_DLSTART until the pending swap has been
    // scanned out. Polling that condition here instead of spinning in
    // dlStart() keeps the main loop (input, MIDI, jobs) running meanwhile.
    if (!ok_) return true;
    static_assert(sizeof(gStage[0]) == kStage, "staging halves");
    if (gDmaFaulted) {
        gDmaFaulted = false;
        recoverCoprocessor();
    }
    flush();
    const uint16_t rp = rd16(REG_CMD_READ);
    if (rp == 0xFFF) {
        // Coprocessor fault (e.g. a display list past the 8 KB RAM_DL, which
        // the Ui guard should make impossible): recover now and count it.
        recoverCoprocessor();
        ++frameFaults_;
        dlPending_ = false;
        busySince_ = 0;
        return true;
    }
    bool ready = rp == wp_ && rd8(REG_DLSWAP) == 0;
    if (ready && dlPending_) {
        // The swapped frame is fully built: REG_CMD_DL is the RAM_DL offset
        // it reached (CMD_DLSTART zeroes it, CMD_SWAP does not) [I].
        lastDlBytes_ = rd32(REG_CMD_DL);
        if (lastDlBytes_ > maxDlBytes_) maxDlBytes_ = lastDlBytes_;
        dlPending_ = false;
    }
    if (!ready) {
        const uint32_t now = millis();
        if (!busySince_) busySince_ = now ? now : 1;
        // Stuck: let dlStart()'s bounded wait detect it and recover.
        else if (now - busySince_ > kBusyGiveUpMs) ready = true;
    }
    if (ready) busySince_ = 0;
    return ready;
}

bool Ft81xDisplay::waitCoproc() {
    for (uint32_t polls = 0; polls < 100000; polls++) {
        const uint16_t rp = rd16(REG_CMD_READ);
        if (rp == 0xFFF) break;
        if (rp == wp_) return true;
    }
    recoverCoprocessor();
    return false;
}

void Ft81xDisplay::fragmentBegin(uint8_t slot) {
    if (!ok_ || slot >= IDisplay::kFragmentSlots || capSlot_ >= 0) return;
    flush();
    if (!waitCoproc()) return;
    capStart_ = uint16_t(rd16(REG_CMD_DL) & 0x1FFF);
    capSlot_ = int8_t(slot);
}

bool Ft81xDisplay::fragmentEnd(uint8_t slot) {
    if (!ok_ || capSlot_ != int8_t(slot)) return false;
    capSlot_ = -1;
    fragBytes_[slot] = 0;
    flush();
    if (!waitCoproc()) return false;
    const uint16_t end = uint16_t(rd16(REG_CMD_DL) & 0x1FFF);
    if (end <= capStart_ || uint32_t(end - capStart_) > kFragSlotBytes) return false;
    const uint32_t bytes = uint32_t(end - capStart_);
    // programmer guide CMD_MEMCPY(dest, src, num): runs in FIFO order, after
    // the captured commands and before anything that follows.
    cmd(CMD_MEMCPY);
    cmd(kFragRamG + uint32_t(slot) * kFragSlotBytes);
    cmd(RAM_DL + capStart_);
    cmd(bytes);
    fragBytes_[slot] = uint16_t(bytes);
    return true;
}

void Ft81xDisplay::fragmentAppend(uint8_t slot) {
    if (!ok_ || slot >= IDisplay::kFragmentSlots || !fragBytes_[slot]) return;
    // programmer guide CMD_APPEND(ptr, num); END afterwards so the
    // coprocessor never assumes a primitive the fragment may have ended.
    cmd(CMD_APPEND);
    cmd(kFragRamG + uint32_t(slot) * kFragSlotBytes);
    cmd(fragBytes_[slot]);
    cmd(DL_END);
}

uint32_t Ft81xDisplay::fragmentWords(uint8_t slot) const {
    return slot < IDisplay::kFragmentSlots ? fragBytes_[slot] / 4u : 0;
}

bool Ft81xDisplay::loadJpeg(uint32_t dest, IStorage& fs, const char* path, uint16_t maxW, uint16_t maxH,
                            uint16_t& w, uint16_t& h) {
    // spec 13 §G.6 loader: validate the header, then stream the whole file
    // behind CMD_LOADIMAGE(dest, OPT_NODL) and read the size via CMD_GETPROPS.
    w = h = 0;
    if (!ok_ || dest < kImageRamGMin) return false;
    uint16_t pw = 0, ph = 0;
    uint32_t size = 0;
    if (!probeJpeg(fs, path, dest, maxW, maxH, pw, ph, size)) {
        diagLogf("ft81x: jpeg rejected: %s", path ? path : "(null)");
        return false;
    }
    const IStorage::Handle f = fs.open(path, IStorage::kRead);
    if (f < 0) return false;
    flush();
    if (!waitIdle()) { fs.close(f); return false; }
    const uint32_t faults0 = faults_;
    cmd(CMD_LOADIMAGE);
    cmd(dest);
    cmd(OPT_NODL);
    uint8_t buf[512];
    uint32_t sent = 0;
    bool streamOk = true;
    while (sent < size) {
        uint32_t n = size - sent;
        if (n > sizeof(buf)) n = sizeof(buf);
        const int32_t r = fs.read(f, buf, n);
        if (r <= 0) { streamOk = false; break; }
        cmdRaw(buf, uint32_t(r));      // flushes in <= kStage bursts, waiting for FIFO space
        sent += uint32_t(r);
        if (faults_ != faults0) { streamOk = false; break; }
    }
    fs.close(f);
    if (!streamOk) {
        // The coprocessor is mid-image (or was reset): drop it and reset so the
        // remaining bytes are never parsed as commands. RAM_DL keeps the shown frame.
        staged_ = 0;
        recoverCoprocessor();
        diagLogf("ft81x: jpeg stream failed at %lu/%lu B", static_cast<unsigned long>(sent),
                 static_cast<unsigned long>(size));
        return false;
    }
    static const uint8_t kZero[4] = {0, 0, 0, 0};
    cmdRaw(kZero, (4u - (size & 3u)) & 3u);   // next command is 4-byte aligned
    cmd(CMD_GETPROPS);
    cmd(0);
    cmd(0);
    cmd(0);
    flush();
    if (faults_ != faults0 || !waitIdleMs(kImageTimeoutMs)) {
        diagLog("ft81x: jpeg decode fault/timeout, coprocessor reset");
        return false;
    }
    // CMD_GETPROPS results replace its parameters in RAM_CMD (spec 01 §4.3).
    const uint32_t end = rd32(RAM_CMD + ((uint32_t(wp_) - 12u) & 0xFFFu));
    const uint32_t gw = rd32(RAM_CMD + ((uint32_t(wp_) - 8u) & 0xFFFu));
    const uint32_t gh = rd32(RAM_CMD + ((uint32_t(wp_) - 4u) & 0xFFFu));
    if (gw != pw || gh != ph) {
        diagLogf("ft81x: jpeg props %lux%lu != header %ux%u", static_cast<unsigned long>(gw),
                 static_cast<unsigned long>(gh), unsigned(pw), unsigned(ph));
        return false;
    }
    // UNVERIFIED: the decoded RGB565 stride is assumed to be 2 * w (no MCU
    // padding); log when the reported end address disagrees.
    if (end != dest + uint32_t(pw) * ph * 2u)
        diagLogf("ft81x: jpeg end 0x%06lX, expected 0x%06lX", static_cast<unsigned long>(end),
                 static_cast<unsigned long>(dest + uint32_t(pw) * ph * 2u));
    w = pw;
    h = ph;
    return true;
}

void Ft81xDisplay::drawImage(int16_t x, int16_t y, uint32_t addr, uint16_t w, uint16_t h) {
    // FT81x programmer guide: BITMAP_HANDLE 0x05, BITMAP_SOURCE 0x01,
    // BITMAP_LAYOUT(_H) 0x07/0x28, BITMAP_SIZE(_H) 0x08/0x29 (NEAREST, BORDER).
    if (!ok_ || w == 0 || h == 0 || w > 2047 || h > 2047) return;
    const uint32_t stride = uint32_t(w) * 2u;
    cmd((0x05u << 24) | kImageHandle);
    cmd((0x01u << 24) | (addr & 0x3FFFFFu));
    cmd((0x28u << 24) | (((stride >> 10) & 3u) << 2) | ((uint32_t(h) >> 9) & 3u));
    cmd((0x07u << 24) | (kFmtRgb565 << 19) | ((stride & 0x3FFu) << 9) | (h & 0x1FFu));
    cmd((0x29u << 24) | (((uint32_t(w) >> 9) & 3u) << 2) | ((uint32_t(h) >> 9) & 3u));
    cmd((0x08u << 24) | ((uint32_t(w) & 0x1FFu) << 9) | (h & 0x1FFu));
    begin(kBitmaps);
    vertex2f(int32_t(x) * 16, int32_t(y) * 16);
    end();
}

bool Ft81xDisplay::writeRamG(uint32_t addr, const void* src, uint32_t len) {
    // spec 14 §3.1 frame upload. RAM_G writes bypass the command FIFO, so
    // only the staged commands are flushed first.
    if (!ok_ || !src || addr >= RAM_G_SIZE || len > RAM_G_SIZE - addr) return false;
    flush();
    const uint8_t* p = static_cast<const uint8_t*>(src);
    while (len) {   // bursts of <= 4 KB keep CS low for bounded spans
        const uint32_t n = len < 4096 ? len : 4096;
        memWriteBurst(addr, p, n);
        addr += n;
        p += n;
        len -= n;
    }
    return true;
}

void Ft81xDisplay::drawRgb332(int16_t x, int16_t y, uint32_t addr, uint16_t w, uint16_t h, uint8_t scale) {
    // spec 14 §3.2 items 3-4: SETBITMAP(addr, RGB332, w, h), BITMAP_TRANSFORM_A
    // = E = 256 / scale, BITMAP_SIZE(NEAREST, BORDER, BORDER, w * s, h * s).
    // FT81x programmer guide: SAVE_CONTEXT 0x22, RESTORE_CONTEXT 0x23,
    // BITMAP_TRANSFORM_A 0x15 / E 0x19 (signed 8.8).
    constexpr uint32_t kFmtRgb332 = 4;
    if (!ok_ || w == 0 || h == 0 || scale == 0 || w > 1023 || h > 1023) return;
    const uint32_t sw = uint32_t(w) * scale, sh = uint32_t(h) * scale;
    if (sw > 2047 || sh > 2047) return;
    const uint32_t ab = 256u / scale;
    cmd(0x22u << 24);
    cmd((0x05u << 24) | kGameHandle);
    cmd((0x01u << 24) | (addr & 0x3FFFFFu));
    cmd((0x28u << 24) | (((uint32_t(w) >> 10) & 3u) << 2) | ((uint32_t(h) >> 9) & 3u));
    cmd((0x07u << 24) | (kFmtRgb332 << 19) | ((uint32_t(w) & 0x3FFu) << 9) | (h & 0x1FFu));
    cmd((0x29u << 24) | (((sw >> 9) & 3u) << 2) | ((sh >> 9) & 3u));
    cmd((0x08u << 24) | ((sw & 0x1FFu) << 9) | (sh & 0x1FFu));
    cmd((0x15u << 24) | (ab & 0x1FFFFu));
    cmd((0x19u << 24) | (ab & 0x1FFFFu));
    begin(kBitmaps);
    vertex2f(int32_t(x) * 16, int32_t(y) * 16);
    end();
    cmd(0x23u << 24);
}

bool Ft81xDisplay::snapshotBand(int band) {
    // Render physical rows [band * kSnapBandRows, +kSnapBandRows) of the current
    // display list into RAM_G (programmer guide CMD_SNAPSHOT2). Like stock
    // (spec 05 §1.9) the scan-out clock keeps running: stopping REG_PCLK blanked
    // the panel visibly during a capture on hardware.
    flush();
    if (!waitIdle()) return false;
    const uint16_t y = uint16_t(band * kSnapBandRows);
    uint16_t h = kSnapBandRows;
    if (y + h > board::kLcdHeight) h = uint16_t(board::kLcdHeight - y);
    cmd(CMD_SNAPSHOT2);
    cmd(kFmtRgb565);
    cmd(kSnapRamG);
    cmd(uint32_t(0) | (uint32_t(y) << 16));                         // x, y
    cmd(uint32_t(board::kLcdWidth) | (uint32_t(h) << 16));          // w, h
    flush();
    const bool ok = waitIdle();
    snapBand_ = ok ? int16_t(band) : int16_t(-1);
    return ok;
}

bool Ft81xDisplay::readPixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t* rgb) {
    // Runs in the main loop with interrupts enabled: the SPI transfers are polled
    // per byte, so the audio DMA ISRs keep running. A full 800 x 480 screenshot
    // reads 768,000 bytes (~0.3 s at 30 MHz) plus two snapshots.
    if (!ok_ || !rgb) return false;
    if (w == 0 || h == 0) return true;
    if (uint32_t(x) + w > board::kLcdWidth || uint32_t(y) + h > board::kLcdHeight) return false;
    uint8_t buf[256];   // 128 px per SPI burst
    for (uint16_t row = 0; row < h; ++row) {
        const uint16_t ly = uint16_t(y + row);
        const uint16_t py = kSnapshotFlipped ? uint16_t(board::kLcdHeight - 1 - ly) : ly;
        const int band = py / kSnapBandRows;
        if (band != snapBand_ && !snapshotBand(band)) return false;
        const uint32_t rowAddr = kSnapRamG + uint32_t(py - band * kSnapBandRows) * kSnapRowBytes;
        // Physical x range covering logical [x, x + w).
        const uint16_t px0 = kSnapshotFlipped ? uint16_t(board::kLcdWidth - x - w) : x;
        uint32_t* out = rgb + uint32_t(row) * w;
        for (uint16_t done = 0; done < w;) {
            uint16_t n = uint16_t(w - done);
            if (n > sizeof(buf) / 2) n = sizeof(buf) / 2;
            memRead(rowAddr + uint32_t(px0 + done) * 2, buf, uint32_t(n) * 2);
            for (uint16_t i = 0; i < n; ++i) {
                const uint16_t c = uint16_t(buf[2 * i] | (buf[2 * i + 1] << 8));
                uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
                r = (r << 3) | (r >> 2);
                g = (g << 2) | (g >> 4);
                b = (b << 3) | (b >> 2);
                const uint16_t pi = uint16_t(done + i);              // physical offset in the run
                const uint16_t li = kSnapshotFlipped ? uint16_t(w - 1 - pi) : pi;
                out[li] = (r << 16) | (g << 8) | b;
            }
            done = uint16_t(done + n);
        }
    }
    return true;
}

}  // namespace hal
}  // namespace ot
