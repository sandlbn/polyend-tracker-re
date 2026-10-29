// OpenTracker -- SDHC block driver (spec 01 §6). Register model from the NXP K66
// reference manual (SDHC chapter); command flow from the SD Physical Layer
// Simplified Specification.
//
// Transfers: ADMA2 (descriptor table, CMD17/18/24/25, auto CMD12), like the stock
// SDK host (spec 01 §6). Buffers ADMA2 cannot reach safely (unaligned, crossing
// the SRAM_L/SRAM_U boundary, SDRAM until checkSdramDirect() passed) bounce
// through an SRAM buffer. The descriptors and the bounce buffer live in the
// uncached `.dma` section (dma_mem.h); SDRAM buffers get the system-cache
// maintenance of sdram_cache.h around each direct transfer. Clock: CMD6 high
// speed -> 45 MHz (180/4; stock asks the SDK for 50 MHz), else default speed
// 22.5 MHz. Errors: reset CMD/DAT lines, CMD12, retry once, then step down the
// clock (last resort: PIO). The polled PIO path (DATPORT) stays as runtime
// fallback when ADMA2 fails its init self-check.
#include "sdhc.h"

#include <Arduino.h>
#include <string.h>
#include "board.h"
#include "diag.h"
#include "dma_mem.h"
#include "sdram_cache.h"
#include "hw_math.h"
#include "k66.h"

namespace ot {
namespace hal {
namespace sdhc {

namespace {
// PRSSTAT bits
constexpr uint32_t P_CIHB = 1u << 0, P_CDIHB = 1u << 1, P_SDSTB = 1u << 3;
// IRQSTAT bits
constexpr uint32_t I_CC = 1u << 0, I_TC = 1u << 1, I_DINT = 1u << 3, I_BWR = 1u << 4, I_BRR = 1u << 5;
constexpr uint32_t I_CMD_ERR = (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19);
constexpr uint32_t I_DATA_ERR = (1u << 20) | (1u << 21) | (1u << 22) | (1u << 24) | (1u << 28);
// XFERTYP fields
constexpr uint32_t X_RSP_NONE = 0, X_RSP_136 = 1u << 16, X_RSP_48 = 2u << 16, X_RSP_48B = 3u << 16;
constexpr uint32_t X_CCCEN = 1u << 19, X_CICEN = 1u << 20, X_DPSEL = 1u << 21;
constexpr uint32_t X_DTDSEL = 1u << 4, X_MSBSEL = 1u << 5, X_BCEN = 1u << 1, X_AC12EN = 1u << 2;
constexpr uint32_t X_DMAEN = 1u << 0;
constexpr uint32_t R1 = X_RSP_48 | X_CCCEN | X_CICEN;
constexpr uint32_t R1B = X_RSP_48B | X_CCCEN | X_CICEN;
constexpr uint32_t R2 = X_RSP_136 | X_CCCEN;
constexpr uint32_t R3 = X_RSP_48;
inline uint32_t CMD(uint32_t i) { return i << 24; }
// PROCTL: EMODE (5:4) = 10b little endian, DTW (2:1) = 01b 4-bit, DMAS (9:8) = 10b ADMA2.
constexpr uint32_t PROCTL_LE = 0x20, PROCTL_4BIT = 0x2, PROCTL_ADMA2 = 2u << 8;
constexpr uint32_t HTCAP_ADMAS = 1u << 20;   // K66 RM SDHC HTCAPBLT: ADMA support
// LMEM PCCCR (K66 RM "Local memory controller"): GO, PUSHW1, INVW1, PUSHW0, INVW0.
constexpr uint32_t LMEM_GO = 1u << 31, LMEM_CLEAR_ALL = (1u << 27) | (1u << 26) | (1u << 25) | (1u << 24);

const board::Pin kPins[6] = {{board::PE, 0}, {board::PE, 1}, {board::PE, 2},
                             {board::PE, 3}, {board::PE, 4}, {board::PE, 5}};

struct ClockStep { uint8_t fs, dvs; };
// Transfer clocks, fastest first. Index 0 only after a successful CMD6 switch.
constexpr ClockStep kClocks[] = {{board::kSdHsClkfs, board::kSdHsDvs},
                                 {board::kSdDsClkfs, board::kSdDsDvs},
                                 {board::kSdLoClkfs, board::kSdLoDvs}};
constexpr uint32_t kNumClocks = sizeof(kClocks) / sizeof(kClocks[0]);
constexpr uint32_t clockOf(uint32_t i) { return hw::sdhcClockHz(board::kCoreHz, kClocks[i].fs, kClocks[i].dvs); }

// SD Physical Layer Simplified Spec §4.2.1 / §6.6.6: identification <= 400 kHz,
// default speed <= 25 MHz, high speed <= 50 MHz. SDHC base clock = core clock.
static_assert(hw::sdhcClockHz(board::kCoreHz, board::kSdIdClkfs, board::kSdIdDvs) <= 400000,
              "SD init clock > 400 kHz");
static_assert(clockOf(0) <= 50000000, "SD high-speed clock > 50 MHz");
static_assert(clockOf(1) <= 25000000, "SD default-speed clock > 25 MHz");
static_assert(board::kSdBounceBytes % 512 == 0 && board::kSdBounceBytes >= 1024, "bounce size");
static_assert(board::kSdAdmaDescBytes % 512 == 0 && board::kSdAdmaDescBytes <= 65532, "ADMA2 length");
static_assert(board::kSdBounceBytes <= board::kSdAdmaDescs * board::kSdAdmaDescBytes, "descs");

constexpr uint32_t kBounceBlocks = board::kSdBounceBytes / 512;
constexpr uint32_t kMaxDirectBlocks = board::kSdAdmaDescs * board::kSdAdmaDescBytes / 512;

struct Adma2Desc { uint32_t attr; uint32_t addr; };
OT_DMA_MEM Adma2Desc gDesc[board::kSdAdmaDescs];
OT_DMA_MEM uint8_t gBounce[board::kSdBounceBytes];

bool gReady = false;
bool gHighCapacity = false;
uint32_t gRca = 0;
uint32_t gSectors = 0;
uint32_t gLastErr = 0;
uint32_t gClockIdx = 1;
uint32_t gProctl = PROCTL_LE;
Info gInfo;
const char* gNote = "";
Counters gCount;
// ADMA2 straight into / out of SDRAM: a property of the crossbar path, so it
// survives card re-inits (set only by checkSdramDirect()).
bool gSdramRd = false, gSdramWr = false;
DirectCheck gDirect = DirectCheck::NotRun, gDirectWr = DirectCheck::NotRun;

bool waitPrs(uint32_t mask, uint32_t timeoutMs) {
    uint32_t t = millis();
    while (SDHC_PRSSTAT & mask) {
        if (millis() - t > timeoutMs) return !(SDHC_PRSSTAT & mask);   // re-check after preemption
    }
    return true;
}

// Wait for any bit in `mask` or an error; returns the IRQSTAT snapshot (0 on timeout).
// The flag is read once more after the deadline: an ISR may have held the CPU.
uint32_t waitIrq(uint32_t mask, uint32_t errMask, uint32_t timeoutMs) {
    uint32_t t = millis();
    for (;;) {
        uint32_t s = SDHC_IRQSTAT;
        if (s & (mask | errMask)) return s;
        if (millis() - t > timeoutMs) {
            s = SDHC_IRQSTAT;
            return (s & (mask | errMask)) ? s : 0;
        }
    }
}

void resetLines() {
    SDHC_SYSCTL |= SDHC_SYSCTL_RSTC | SDHC_SYSCTL_RSTD;
    uint32_t t = millis();
    while ((SDHC_SYSCTL & (SDHC_SYSCTL_RSTC | SDHC_SYSCTL_RSTD)) && millis() - t < 10) {}
    // UNVERIFIED: K66 RM lists only CREQ/SABGREQ as cleared by RSTD; PROCTL is
    // rewritten anyway so the bus width / DMA select survive a reset.
    SDHC_PROCTL = gProctl;
    SDHC_IRQSTAT = 0xFFFFFFFFu;
}

bool command(uint32_t idx, uint32_t arg, uint32_t flags) {
    bool data = flags & X_DPSEL;
    if (!waitPrs(P_CIHB | (data || (flags & X_RSP_48B) == X_RSP_48B ? P_CDIHB : 0), 500)) {
        gLastErr = 0x80000000u;
        return false;
    }
    SDHC_IRQSTAT = 0xFFFFFFFFu;
    SDHC_CMDARG = arg;
    SDHC_XFERTYP = CMD(idx) | flags;
    gCount.cmds++;
    uint32_t s = waitIrq(I_CC, I_CMD_ERR, 500);
    if (!(s & I_CC) || (s & I_CMD_ERR)) {
        gLastErr = s ? s : 0x40000000u;
        resetLines();
        return false;
    }
    SDHC_IRQSTAT = I_CC;
    if ((flags & X_RSP_48B) == X_RSP_48B && !data) {
        // busy signalled on DAT0: wait for the transfer-complete event.
        waitIrq(I_TC, I_DATA_ERR, 1000);
        SDHC_IRQSTAT = I_TC;
    }
    return true;
}

bool appCommand(uint32_t idx, uint32_t arg, uint32_t flags) {
    return command(55, gRca << 16, R1) && command(idx, arg, flags);
}

void setClock(uint32_t sdclkfs, uint32_t dvs) {
    // spec 01 §6: SYSCTL = (SYSCTL & 0xFFF00007) | div | 0xE0008 (DTOCV=0xE, SDCLKEN).
    SDHC_SYSCTL &= ~SDHC_SYSCTL_SDCLKEN;
    SDHC_SYSCTL = (SDHC_SYSCTL & 0xFFF00007u) | SDHC_SYSCTL_SDCLKFS(sdclkfs) |
                  SDHC_SYSCTL_DVS(dvs) | SDHC_SYSCTL_DTOCV(board::kSdDtocv);
    uint32_t t = millis();
    while (!(SDHC_PRSSTAT & P_SDSTB) && millis() - t < 10) {}
    SDHC_SYSCTL |= SDHC_SYSCTL_SDCLKEN;
}

void selectClock(uint32_t idx) {
    gClockIdx = idx;
    setClock(kClocks[idx].fs, kClocks[idx].dvs);
    gInfo.clockHz = clockOf(idx);
}

void setAdma(bool on) {
    gInfo.adma = on;
    gProctl = (gProctl & ~(3u << 8)) | (on ? PROCTL_ADMA2 : 0u);
    SDHC_PROCTL = gProctl;
}

bool waitTransferState() {
    uint32_t t = millis();
    while (millis() - t < 1000) {
        if (!command(13, gRca << 16, R1)) return false;
        uint32_t st = SDHC_CMDRSP0;
        if ((st & (1u << 8)) && ((st >> 9) & 0xF) == 4) return true;   // READY_FOR_DATA, tran
    }
    return false;
}

void parseCsd() {
    // R2 response is stored without CRC: CMDRSPn bit k = CSD bit k+8.
    uint32_t r1 = SDHC_CMDRSP1, r2 = SDHC_CMDRSP2, r3 = SDHC_CMDRSP3;
    uint32_t structure = (r3 >> 22) & 3;
    if (structure == 1) {                 // CSD v2: C_SIZE = CSD[69:48]
        uint32_t cSize = (r1 >> 8) & 0x3FFFFF;
        gSectors = (cSize + 1) * 1024;
    } else if (structure == 0) {          // CSD v1
        uint32_t cSize = ((r1 >> 22) & 0x3FF) | ((r2 & 0x3) << 10);   // CSD[73:62]
        uint32_t mult = (r1 >> 7) & 0x7;                              // CSD[49:47]
        uint32_t blLen = (r2 >> 8) & 0xF;                             // CSD[83:80]
        gSectors = ((cSize + 1) << (mult + 2)) << blLen >> 9;
    } else {
        gSectors = 0;
    }
}

uint32_t timeoutMsFor(uint32_t count) { return 500 + count; }   // >= 0.5 MB/s worst case

// Starts a data command; true once the command phase completed.
bool startData(bool write, uint32_t lba, uint32_t count, uint32_t extra) {
    if (!waitPrs(P_CIHB | P_CDIHB, 500)) { gLastErr = 0x80000000u; return false; }
    SDHC_IRQSTAT = 0xFFFFFFFFu;
    SDHC_BLKATTR = (count << 16) | 512;
    uint32_t flags = R1 | X_DPSEL | extra | (write ? 0 : X_DTDSEL);
    if (count > 1) flags |= X_MSBSEL | X_BCEN | X_AC12EN;
    SDHC_CMDARG = gHighCapacity ? lba : lba * 512;
    SDHC_XFERTYP = CMD(write ? (count > 1 ? 25 : 24) : (count > 1 ? 18 : 17)) | flags;
    gCount.cmds++;
    uint32_t s = waitIrq(I_CC, I_CMD_ERR, 500);
    if (!(s & I_CC) || (s & I_CMD_ERR)) { gLastErr = s ? s : 0x40000000u; return false; }
    SDHC_IRQSTAT = I_CC;
    return true;
}

// ---- polled PIO (fallback) ---------------------------------------------------
bool pioXfer(bool write, uint32_t lba, uint8_t* buf, uint32_t count) {
    if (!startData(write, lba, count, 0)) return false;
    const uint32_t ready = write ? I_BWR : I_BRR;
    for (uint32_t b = 0; b < count; b++) {
        uint32_t s = waitIrq(ready, I_DATA_ERR, 500);
        if (!(s & ready) || (s & I_DATA_ERR)) { gLastErr = s ? s : 0x20000000u; return false; }
        if (write) {
            for (int w = 0; w < 128; w++) { uint32_t v; memcpy(&v, buf, 4); SDHC_DATPORT = v; buf += 4; }
        } else {
            for (int w = 0; w < 128; w++) { uint32_t v = SDHC_DATPORT; memcpy(buf, &v, 4); buf += 4; }
        }
        SDHC_IRQSTAT = ready;
    }
    uint32_t s = waitIrq(I_TC, I_DATA_ERR, write ? 2000 : 500);
    if (!(s & I_TC) || (s & I_DATA_ERR)) { gLastErr = s ? s : 0x20000000u; return false; }
    SDHC_IRQSTAT = I_TC;
    return write ? waitTransferState() : true;
}

// ---- ADMA2 ---------------------------------------------------------------------
// The core reaches SRAM_L through the LMEM code-bus cache (enabled by the Teensy
// startup, PCCCR = 0x85000003); the SDHC master does not. Push + invalidate the
// whole 8 KB cache around DMA that touches SRAM_L (descriptors or data).
// UNVERIFIED: K66 RM allows cache commands while the cache is enabled and
// executing; whether SRAM_L is cacheable at all depends on PCCRMR (harmless if not).
void cacheClearAll() {
    LMEM_PCCCR |= LMEM_GO | LMEM_CLEAR_ALL;
    while (LMEM_PCCCR & LMEM_GO) {}
}

bool admaXfer(bool write, uint32_t lba, uint8_t* buf, uint32_t count) {
    uint32_t bytes = count * 512, addr = reinterpret_cast<uint32_t>(buf);
    uint32_t n = 0;
    while (bytes) {
        uint32_t len = bytes < board::kSdAdmaDescBytes ? bytes : board::kSdAdmaDescBytes;
        bytes -= len;
        gDesc[n].attr = detail::adma2Attr(len, bytes == 0);
        gDesc[n].addr = addr;
        addr += len;
        n++;
    }
    const bool maint = detail::inCodeBusRam(reinterpret_cast<uint32_t>(buf)) ||
                       detail::inCodeBusRam(reinterpret_cast<uint32_t>(gDesc));
    if (maint) cacheClearAll();
    if (write) cache::cleanForDma(buf, count * 512);   // SDRAM source (no-op elsewhere)
    __asm__ volatile("dsb" ::: "memory");              // descriptors stored before the SDHC fetches them
    SDHC_ADSADDR = reinterpret_cast<uint32_t>(gDesc);
    if (!startData(write, lba, count, X_DMAEN)) return false;
    uint32_t s = waitIrq(I_TC, I_DATA_ERR, timeoutMsFor(count));
    if (!(s & I_TC) || (s & I_DATA_ERR)) {
        gLastErr = s ? s : 0x20000000u;
        if (s & (1u << 28)) gInfo.admaErr = SDHC_ADMAES;
        return false;
    }
    SDHC_IRQSTAT = I_TC | I_DINT;
    if (maint && !write) cacheClearAll();
    if (!write) cache::invalidate(buf, count * 512);   // SDRAM destination (no-op elsewhere)
    return write ? waitTransferState() : true;
}

bool xferOnce(bool write, uint32_t lba, uint8_t* buf, uint32_t count) {
    return gInfo.adma ? admaXfer(write, lba, buf, count) : pioXfer(write, lba, buf, count);
}

// After a failed transfer: reset CMD/DAT lines, stop a multi-block transfer the
// auto CMD12 may not have ended, and wait for the card to return to tran.
void recoverAfterError() {
    resetLines();
    command(12, 0, R1B);          // fails harmlessly (illegal command) when already in tran
    waitTransferState();
}

void degrade() {
    gInfo.degrades++;
    if (gClockIdx + 1 < kNumClocks) {
        selectClock(gClockIdx + 1);
        diagLogf("sd: errors, clock down to %lu Hz", static_cast<unsigned long>(gInfo.clockHz));
    } else if (gInfo.adma) {
        setAdma(false);
        diagLog("sd: errors, ADMA2 off -> PIO");
    }
}

// Retry once at the same settings, then once more after stepping down.
bool xferRobust(bool write, uint32_t lba, uint8_t* buf, uint32_t count) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (xferOnce(write, lba, buf, count)) return true;
        if (!cardPresent()) return false;
        gInfo.retries++;
        recoverAfterError();
        if (attempt == 1) degrade();
    }
    return false;
}

bool xfer(bool write, uint32_t lba, uint8_t* buf, uint32_t count) {
    if (!gReady || count == 0) return false;
    while (count) {
        uint32_t n;
        bool ok;
        const uint32_t addr = reinterpret_cast<uint32_t>(buf);
        if (!gInfo.adma) {
            n = count < 0xFFFFu ? count : 0xFFFFu;
            ok = xferRobust(write, lba, buf, n);
        } else if (const detail::Chunk c = detail::planChunk(addr, count, kMaxDirectBlocks, kBounceBlocks,
                                                               write ? gSdramWr : gSdramRd, board::kSdramBase,
                                                               board::kSdramSize);
                   n = c.blocks, c.direct) {
            gCount.direct++;
            ok = xferRobust(write, lba, buf, n);
        } else {
            gInfo.bounced++;
            gCount.bounced++;
            if (write) memcpy(gBounce, buf, n * 512);
            ok = xferRobust(write, lba, gBounce, n);
            if (ok && !write) memcpy(buf, gBounce, n * 512);
        }
        if (!ok) return false;
        if (write) gCount.writeBytes += n * 512; else gCount.readBytes += n * 512;
        lba += n;
        buf += n * 512;
        count -= n;
    }
    return true;
}

// CMD6 SWITCH_FUNC (SD spec §4.3.10): 64-byte status block read by PIO. The read
// watermark drops to 16 words for it (BRR asserts at the watermark).
bool switchFunc(uint32_t arg, uint8_t* st) {
    if (!waitPrs(P_CIHB | P_CDIHB, 500)) return false;
    SDHC_WML = (board::kSdWml & ~0xFFu) | 16u;
    SDHC_IRQSTAT = 0xFFFFFFFFu;
    SDHC_BLKATTR = (1u << 16) | 64;
    SDHC_CMDARG = arg;
    SDHC_XFERTYP = CMD(6) | R1 | X_DPSEL | X_DTDSEL;
    gCount.cmds++;
    bool ok = false;
    uint32_t s = waitIrq(I_CC, I_CMD_ERR, 500);
    if ((s & I_CC) && !(s & I_CMD_ERR)) {
        SDHC_IRQSTAT = I_CC;
        s = waitIrq(I_BRR, I_DATA_ERR, 500);
        if ((s & I_BRR) && !(s & I_DATA_ERR)) {
            for (int w = 0; w < 16; w++) { uint32_t v = SDHC_DATPORT; memcpy(st + 4 * w, &v, 4); }
            SDHC_IRQSTAT = I_BRR;
            s = waitIrq(I_TC, I_DATA_ERR, 500);
            ok = (s & I_TC) && !(s & I_DATA_ERR);
        }
    }
    SDHC_WML = board::kSdWml;
    if (!ok) { gLastErr = s ? s : 0x10000000u; resetLines(); }
    SDHC_IRQSTAT = 0xFFFFFFFFu;
    return ok;
}

// Check, then switch, function group 1 to high speed. SD 1.0 cards do not know
// CMD6: the command times out and we stay at default speed.
bool trySwitchHighSpeed() {
    uint8_t st[64];
    if (!switchFunc(0x00FFFFF1u, st)) { gNote = "CMD6 check failed"; return false; }
    if (!detail::cmd6HighSpeedSupported(st) || detail::cmd6Group1Result(st) != 1) {
        gNote = "card has no high speed";
        return false;
    }
    if (!switchFunc(0x80FFFFF1u, st) || detail::cmd6Group1Result(st) != 1) {
        gNote = "CMD6 switch failed";
        return false;
    }
    delayMicroseconds(10);   // SD spec: new timing within 8 clocks after the status block
    return true;
}
}  // namespace

bool cardPresent() { return digitalReadFast(board::kTeensyCardDetect) == LOW; }
bool ready() { return gReady; }
uint32_t sectorCount() { return gSectors; }
uint32_t lastError() { return gLastErr; }
const Info& info() { return gInfo; }
const char* initNote() { return gNote; }
const Counters& counters() { return gCount; }
bool sdramDirectRead() { return gSdramRd; }
bool sdramDirectWrite() { return gSdramWr; }
DirectCheck directCheck() { return gDirect; }
DirectCheck directWriteCheck() { return gDirectWr; }
const char* directCheckName(DirectCheck c) {
    switch (c) {
    case DirectCheck::NotRun: return "not run";
    case DirectCheck::Ok: return "ok";
    case DirectCheck::Mismatch: return "MISMATCH";
    case DirectCheck::XferError: return "transfer error";
    case DirectCheck::Skipped: return "skipped";
    }
    return "?";
}

// ADMA2 straight into SDRAM, compared with the bounce path (docs/PLAN_2X.md
// "Direct ADMA2 into SDRAM, behind an init self-check"). Reads: 64 sectors at
// `lba` through the bounce buffer (reference) and directly into a pre-cached,
// pre-filled SDRAM buffer (exercises the invalidate), plus 8 sectors to a
// word- but not line-aligned address. Only reads touch the card here; direct
// writes are verified lazily through FatFs (SdStorage, begin/endDirectWriteTrial).
// UNVERIFIED: K66 crossbar lets the SDHC master (M5) reach the SDRAM controller.
bool checkSdramDirect(uint8_t* scratch, uint32_t lba) {
    constexpr uint32_t kN = 64, kBytes = kN * 512;
    static_assert(kN <= kMaxDirectBlocks, "one ADMA2 table");
    gSdramRd = gSdramWr = false;
    gDirect = DirectCheck::Skipped;
    gDirectWr = DirectCheck::NotRun;
    if (!gReady || !gInfo.adma || !board::kSdDmaToSdram) return false;
    uint8_t* ref = scratch;
    uint8_t* chk = scratch + kBytes;
    if (!xfer(false, lba, ref, kN)) { gDirect = DirectCheck::XferError; return false; }   // bounced
    memset(chk, 0xA5, kBytes + 64);
    volatile uint32_t sink = 0;
    for (uint32_t i = 0; i < kBytes; i += 16) sink = sink + chk[i];   // pull into the cache
    (void)sink;
    gSdramRd = true;   // xfer() may now go direct
    const uint32_t before = gCount.direct;
    bool ok = xfer(false, lba, chk, kN) && gCount.direct != before;
    if (!ok) gDirect = DirectCheck::XferError;
    else if (memcmp(ref, chk, kBytes) != 0) { ok = false; gDirect = DirectCheck::Mismatch; }
    if (ok) {
        uint8_t* odd = chk + 4;
        ok = xfer(false, lba, odd, 8);
        if (!ok) gDirect = DirectCheck::XferError;
        else if (memcmp(ref, odd, 8 * 512) != 0) { ok = false; gDirect = DirectCheck::Mismatch; }
    }
    if (!ok) {
        gSdramRd = false;
        recoverAfterError();
        return false;
    }
    gDirect = DirectCheck::Ok;
    return true;
}

// Direct-write trial (sd_storage.cpp, through FatFs on a temp file): writes from
// SDRAM go direct only between begin and end; end(true) keeps them direct.
void beginDirectWriteTrial() { gSdramWr = gSdramRd; }
void endDirectWriteTrial(bool ok) {
    gSdramWr = ok && gSdramRd;
    gDirectWr = gSdramWr ? DirectCheck::Ok : (gSdramRd ? DirectCheck::Mismatch : DirectCheck::Skipped);
}
void skipDirectWriteCheck() { gSdramWr = false; gDirectWr = DirectCheck::Skipped; }

void release() {
    gReady = false;
    // Without a card init() never ran: the SDHC clock gate is off and any
    // register access is a BusFault (boot with no card, then the power gate
    // or a soft-off unmounts: HardFault at SYSCTL 0x400B102C).
    if (!(SIM_SCGC3 & SIM_SCGC3_SDHC)) return;
    SDHC_SYSCTL &= ~SDHC_SYSCTL_SDCLKEN;
}

bool init() {
    gReady = false;
    gRca = 0;
    gInfo = Info();
    gNote = "";
    pinMode(board::kTeensyCardDetect, INPUT_PULLUP);
    if (!cardPresent()) return false;

    // spec 01 §6 init
    MPU_RGDAAC0 |= 0x0C000000;             // crossbar master 5 (SDHC) read/write
    SIM_SCGC3 |= SIM_SCGC3_SDHC;
    for (const auto& p : kPins) pcr(p) = board::kSdPcrInit;
    SDHC_SYSCTL = SDHC_SYSCTL_RSTA;
    uint32_t t = millis();
    while ((SDHC_SYSCTL & SDHC_SYSCTL_RSTA) && millis() - t < 100) {}
    SDHC_VENDOR = 0;
    gProctl = PROCTL_LE;                   // little endian, 1-bit bus, no DMA
    SDHC_PROCTL = gProctl;
    SDHC_WML = board::kSdWml;
    // spec 01 §6 programs 0x117F000B for its ADMA2 driver. That value leaves
    // BWRSEN/BRRSEN (bits 4/5) clear, and K66 RM SDHC "IRQSTATEN": a cleared
    // enable bit keeps the IRQSTAT flag from ever being set -- the PIO path
    // waits on BRR/BWR, so they must be enabled (+0x30). Polled, no IRQSIGEN.
    SDHC_IRQSTATEN = board::kSdIrqStatEn | I_BWR | I_BRR;
    SDHC_IRQSIGEN = 0;
    setClock(board::kSdIdClkfs, board::kSdIdDvs);   // 375 kHz identification clock
    for (const auto& p : kPins) pcr(p) = (p.bit == 2) ? board::kSdPcrClk : board::kSdPcrData;
    SDHC_SYSCTL |= SDHC_SYSCTL_INITA;      // 80 clocks
    t = millis();
    while ((SDHC_SYSCTL & SDHC_SYSCTL_INITA) && millis() - t < 100) {}

    if (!command(0, 0, X_RSP_NONE)) return false;
    bool v2 = command(8, 0x1AA, R1) && (SDHC_CMDRSP0 & 0xFFF) == 0x1AA;
    uint32_t ocr = 0;
    t = millis();
    do {
        if (!appCommand(41, v2 ? 0x40300000 : 0x00300000, R3)) return false;
        ocr = SDHC_CMDRSP0;
        if (millis() - t > 1500) return false;
    } while (!(ocr & 0x80000000u));
    gHighCapacity = v2 && (ocr & 0x40000000u);
    if (!command(2, 0, R2)) return false;
    if (!command(3, 0, R1)) return false;
    gRca = SDHC_CMDRSP0 >> 16;
    if (!command(9, gRca << 16, R2)) return false;
    parseCsd();
    if (!command(7, gRca << 16, R1B)) return false;
    if (!appCommand(6, 2, R1)) return false;           // spec 01 §6: ACMD6 4-bit bus
    gProctl |= PROCTL_4BIT;                             // DTW = 4-bit
    SDHC_PROCTL = gProctl;
    if (!gHighCapacity && !command(16, 512, R1)) return false;
    selectClock(1);                                     // default speed 22.5 MHz
    gReady = true;

    // Reference copy of sector 0 by PIO at default speed: the known-good path.
    uint8_t* ref = gBounce;
    uint8_t* chk = gBounce + 512;
    if (!pioXfer(false, 0, ref, 1)) { gReady = false; return false; }

    // spec 01 §6: high-speed switch -> 50 MHz class. Verified by re-reading sector 0.
    if (trySwitchHighSpeed()) {
        gInfo.highSpeed = true;
        selectClock(0);
        if (!pioXfer(false, 0, chk, 1) || memcmp(ref, chk, 512) != 0) {
            recoverAfterError();
            selectClock(1);       // card stays in HS timing; valid at lower clocks
            gNote = "high-speed read check failed, 22.5 MHz";
        }
    }

    // ADMA2 (spec 01 §6 stock host mode), accepted only if it returns the same
    // sector 0 as PIO (checks descriptors, crossbar access and cache handling).
    if (!(SDHC_HTCAPBLT & HTCAP_ADMAS)) {
        gNote = "no ADMA capability, PIO";
    } else {
        setAdma(true);
        memset(chk, 0xA5, 512);
        if (!admaXfer(false, 0, chk, 1) || memcmp(ref, chk, 512) != 0) {
            recoverAfterError();
            setAdma(false);
            gNote = "ADMA2 self-check failed, PIO";
        }
    }
    return true;
}

namespace {
volatile Owner gOwner = Owner::kApp;
uint32_t gViolations = 0;
bool ownedBy(Owner o, const char* what) {
    if (gOwner == o) return true;
    if (gViolations++ < 8)
        diagLogf("ASSERT sd: %s refused, the card belongs to the %s (USB storage mode)", what,
                 gOwner == Owner::kHost ? "computer" : "app");
    return false;
}
}  // namespace

void setOwner(Owner o) { gOwner = o; }
Owner owner() { return gOwner; }
uint32_t ownershipViolations() { return gViolations; }

bool readBlocks(uint32_t lba, uint8_t* dst, uint32_t count) {
    return ownedBy(Owner::kApp, "app read") && xfer(false, lba, dst, count);
}

bool writeBlocks(uint32_t lba, const uint8_t* src, uint32_t count) {
    // xfer() only reads from `buf` on the write path (DMA source / memcpy source).
    return ownedBy(Owner::kApp, "app write") && xfer(true, lba, const_cast<uint8_t*>(src), count);
}

bool hostReadBlocks(uint32_t lba, uint8_t* dst, uint32_t count) {
    return ownedBy(Owner::kHost, "USB read") && xfer(false, lba, dst, count);
}

bool hostWriteBlocks(uint32_t lba, const uint8_t* src, uint32_t count) {
    return ownedBy(Owner::kHost, "USB write") && xfer(true, lba, const_cast<uint8_t*>(src), count);
}

uint8_t* hostBuffer(uint32_t* bytes) {
    if (gOwner != Owner::kHost) return nullptr;
    *bytes = uint32_t(sizeof gBounce);
    return gBounce;
}

uint8_t* takeBounceForFault(uint32_t* bytes) {
    // Software reset of the DAT line: aborts a running ADMA transfer. Only
    // with the SDHC clock gate on (a register access without it bus-faults).
    if (SIM_SCGC3 & SIM_SCGC3_SDHC) {
        SDHC_SYSCTL |= SDHC_SYSCTL_RSTD;
        for (int i = 0; i < 100000 && (SDHC_SYSCTL & SDHC_SYSCTL_RSTD); ++i) {}
    }
    *bytes = uint32_t(sizeof gBounce);
    return gBounce;
}

}  // namespace sdhc
}  // namespace hal
}  // namespace ot
