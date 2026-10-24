// OpenTracker -- audio output (spec 01 §5.1-§5.3). Own implementation against the
// K66 SAI chapter and the SGTL5000 datasheet; register values come from the spec.
//
// Note: the hardware Fs is MCLK/256 = 44117.6 Hz (spec 01 §5.2), 0.04 % above the
// nominal IAudioOut::kSampleRate (44100). Pitch error ~0.7 cent; the stock firmware
// runs the same clock.
#include "audio_sgtl5000.h"
#include "diag.h"

#include <Arduino.h>
#include <DMAChannel.h>
#include <string.h>
#include "board.h"
#include "dma_mem.h"
#include "hw_math.h"
#include "i2c_bus.h"
#include "k66.h"
#include "rx_watch.h"

namespace ot {
namespace hal {

namespace {
constexpr uint32_t kFrames = board::kAudioBlockFrames;               // spec 01 §5.3
static_assert(kFrames == IAudioOut::kBlockFrames, "block size mismatch");
// SGTL5000 CHIP_CLK_CTRL = 0x0004 (SYS_FS 44.1 kHz, MCLK_FREQ 256 Fs) needs MCLK =
// 256 * Fs; BCLK = 64 Fs (32-bit slots, 2 per frame, TCR4/TCR5).
static_assert(hw::i2sBclkHz(hw::i2sMclkHz(board::kCoreHz, board::kI2sMdr), board::kI2sTcr2) ==
                  hw::i2sMclkHz(board::kCoreHz, board::kI2sMdr) / 4,
              "BCLK must be 64 Fs = MCLK / 4");
// TX ring: 2 halves x 128 stereo frames of int16 (spec 01 §5.3: 1 KB).
OT_DMA_MEM int16_t txBuf[2 * kFrames * 2];   // .dma: uncached SRAM_U (dma_mem.h)
DMAChannel txDma(false);
// RX ring: same shape (spec 01 §5.3 RX column), RDR0+2 -> buffer.
OT_DMA_MEM int16_t rxBuf[2 * kFrames * 2];
int16_t rxCopy[kFrames * 2];
DMAChannel rxDma(false);
volatile int8_t gRxHalf = -1;            // last completed RX half, -1 = none yet
// Raw RX peak for the recorder diagnostics (setRxPeak / takeRxPeak).
volatile bool gRxPeakOn = false;
volatile int32_t gRxPeakL = 0, gRxPeakR = 0;
volatile uint32_t gRxPeakBlocks = 0;
volatile uint32_t gRxBlocks = 0;
// RX liveness (spec 22 §7 row 21): a halted receiver must not keep handing the
// app the same block; serviceRx() restarts it.
RxWatch gRxWatch;
volatile bool gRxStarted = false;
uint32_t gRxLastRecoverMs = 0;
constexpr uint32_t kRcsrFef = 1u << 18;   // K66 RM SAI RCSR[FEF] (w1c)
volatile IAudioOut::FillFn gFill = nullptr;
void* volatile gCtx = nullptr;
volatile bool gRunning = false;
volatile uint32_t gBlocks = 0;
// DWT cycles spent in the fill callback per block (selftest diagnostics).
volatile uint32_t gCycMin = 0xFFFFFFFFu, gCycMax = 0, gCycCount = 0;
volatile uint64_t gCycSum = 0;
// Same, over the current telemetry window (renderCyclesWindow() resets it).
volatile uint32_t gWinMin = 0xFFFFFFFFu, gWinMax = 0, gWinCount = 0;
volatile uint64_t gWinSum = 0;

void txIsr() {
    // Fill the half the DMA is NOT reading (spec 01 §5.3 TX ISR).
    uint32_t saddr = uint32_t(txDma.TCD->SADDR);
    txDma.clearInterrupt();
    int16_t* dst = (saddr < uint32_t(txBuf) + sizeof(txBuf) / 2) ? txBuf + kFrames * 2 : txBuf;
    IAudioOut::FillFn fn = gFill;
    if (gRunning && fn) {
        uint32_t c0 = ARM_DWT_CYCCNT;
        fn(dst, kFrames, gCtx);
        uint32_t c = ARM_DWT_CYCCNT - c0;
        if (c < gCycMin) gCycMin = c;
        if (c > gCycMax) gCycMax = c;
        gCycSum = gCycSum + c;
        gCycCount = gCycCount + 1;
        if (c < gWinMin) gWinMin = c;
        if (c > gWinMax) gWinMax = c;
        gWinSum = gWinSum + c;
        gWinCount = gWinCount + 1;
    } else {
        memset(dst, 0, sizeof(txBuf) / 2);
    }
    gBlocks = gBlocks + 1;
}

void rxIsr() {
    // spec 01 §5.3 RX ISR: the half the DMA just finished. The TX callback copies
    // it (lineInBlock) well before the DMA comes back to it (one block later).
    uint32_t daddr = uint32_t(rxDma.TCD->DADDR);
    rxDma.clearInterrupt();
    gRxHalf = (daddr < uint32_t(rxBuf) + sizeof(rxBuf) / 2) ? 1 : 0;
    gRxBlocks = gRxBlocks + 1;
}

void i2sConfig() {
    // spec 01 §5.2. Clock gates: I2S + DMAMUX, DMA.
    SIM_SCGC6 |= SIM_SCGC6_I2S | SIM_SCGC6_DMAMUX;
    SIM_SCGC7 |= SIM_SCGC7_DMA;
    if (I2S0_TCSR & I2S_TCSR_TE) return;   // spec: configured only if TE/RE clear
    if (I2S0_RCSR & I2S_RCSR_RE) return;
    // MCR first, then wait for MCR.DUF (divider update in progress) to clear BEFORE
    // writing MDR, then wait again (K66 RM SAI MCR/MDR; same order as PJRC
    // output_i2s.cpp). Field result: writing MDR while DUF was set left the divider
    // at its reset ratio 1:1 (MCLK = 180 MHz, audio blocks 15.94x too fast, codec
    // silent on I2C) although MDR read back the new value.
    I2S0_MCR = board::kI2sMcr;
    for (uint32_t t = micros(); (I2S0_MCR & (1u << 31)) && micros() - t < 1000;) {}
    I2S0_MDR = board::kI2sMdr;
    for (uint32_t t = micros(); (I2S0_MCR & (1u << 31)) && micros() - t < 1000;) {}
    I2S0_TMR = 0;
    I2S0_TCR1 = 1;
    I2S0_TCR2 = board::kI2sTcr2;
    I2S0_TCR3 = board::kI2sXcr3;
    // K66 RM SAI TCR4[FCONT] (bit 28): keep transmitting after a FIFO underrun instead of
    // halting until FEF is cleared (field: TX stalled after 2 words with FEF set).
    I2S0_TCR4 = board::kI2sXcr4 | (1u << 28);
    I2S0_TCR5 = board::kI2sXcr5;
    I2S0_RMR = 0;
    I2S0_RCR1 = 1;
    I2S0_RCR2 = board::kI2sRcr2;
    I2S0_RCR3 = board::kI2sXcr3;
    I2S0_RCR4 = board::kI2sXcr4;
    I2S0_RCR5 = board::kI2sXcr5;
    pcr(board::kI2sMclk) = board::kI2sPcr;
    pcr(board::kI2sBclk) = board::kI2sPcr;
    pcr(board::kI2sTxFs) = board::kI2sPcr;
    pcr(board::kI2sTxd0) = board::kI2sPcr;
    pcr(board::kI2sRxd0) = board::kI2sPcr;
}

// spec 01 §5.3 RX column: SADDR RDR0+2, SOFF 0, 16-bit, DOFF 2, DLAST -1024.
// Used by the bring-up and by the stall recovery (same channel, no re-begin).
void rxDmaProgram() {
    rxDma.TCD->SADDR = reinterpret_cast<volatile uint8_t*>(&I2S0_RDR0) + 2;
    rxDma.TCD->SOFF = 0;
    rxDma.TCD->ATTR = DMA_TCD_ATTR_SSIZE(1) | DMA_TCD_ATTR_DSIZE(1);
    rxDma.TCD->NBYTES_MLNO = 2;
    rxDma.TCD->SLAST = 0;
    rxDma.TCD->DADDR = rxBuf;
    rxDma.TCD->DOFF = 2;
    rxDma.TCD->CITER_ELINKNO = sizeof(rxBuf) / 2;
    rxDma.TCD->DLASTSGA = -int32_t(sizeof(rxBuf));
    rxDma.TCD->BITER_ELINKNO = sizeof(rxBuf) / 2;
    rxDma.TCD->CSR = DMA_TCD_CSR_INTHALF | DMA_TCD_CSR_INTMAJOR;
}

void rxDmaStart() {
    memset(rxBuf, 0, sizeof(rxBuf));
    rxDma.begin(true);
    rxDmaProgram();
    rxDma.triggerAtHardwareEvent(DMAMUX_SOURCE_I2S0_RX);   // src 12 (spec 01 §5.3)
    rxDma.attachInterrupt(rxIsr, board::kDmaPrioAudioRx);
    rxDma.enable();
    // spec 01 §5.2 RCSR |= RE | BCE | FR | FRDE. The receiver is synchronous to
    // the transmitter (RCR2 SYNC=1), so it starts clocking when TE is set below
    // (K66 RM I2S "Synchronous mode": enable the receiver first).
    I2S0_RCSR = I2S_RCSR_RE | I2S_RCSR_BCE | I2S_RCSR_FR | I2S_RCSR_FRDE;
    gRxStarted = true;
}

// Restart a halted receiver (serviceRx): stop it, re-arm the same RX DMA
// channel as the bring-up does, reset the FIFO, clear the error flags, enable
// again. TX keeps running; the receiver is synchronous to it (RCR2 SYNC=1).
void rxRestart() {
    I2S0_RCSR = (I2S0_RCSR & ~(I2S_RCSR_RE | I2S_RCSR_FRDE)) & ~(kRcsrFef | I2S_RCSR_SEF | I2S_RCSR_WSF);
    // K66 RM SAI: RE reads 1 until the end of the current frame (22.7 us).
    for (uint32_t t = micros(); (I2S0_RCSR & I2S_RCSR_RE) && micros() - t < 1000;) {}
    rxDma.disable();
    rxDma.clearInterrupt();
    DMA_CERR = rxDma.channel;
    rxDmaProgram();
    __disable_irq();
    gRxHalf = -1;
    __enable_irq();
    rxDma.enable();
    I2S0_RCSR = I2S_RCSR_FR | kRcsrFef | I2S_RCSR_SEF | I2S_RCSR_WSF;   // FIFO reset, flags cleared (w1c)
    I2S0_RCSR = I2S_RCSR_RE | I2S_RCSR_BCE | I2S_RCSR_FRDE;
}

// Field diagnostics (first hardware boot: codec NAK, no TX DMA interrupts). Logs the
// I2S/DMA state a few times so we can tell "I2S not clocking" from "DMA ISR not
// delivered", and scans I2C0 to see whether the codec answers at 0x0A or 0x2A.
void dumpI2sDma(const char* tag) {
    const uint8_t tx = txDma.channel, rx = rxDma.channel;
    diagLogf("probe %s: MCR 0x%08lX MDR 0x%08lX TCSR 0x%08lX RCSR 0x%08lX", tag,
             static_cast<unsigned long>(I2S0_MCR), static_cast<unsigned long>(I2S0_MDR),
             static_cast<unsigned long>(I2S0_TCSR), static_cast<unsigned long>(I2S0_RCSR));
    diagLogf("probe %s: TCR2 0x%08lX TCR4 0x%08lX RCR2 0x%08lX PCR E6 0x%08lX E12 0x%08lX", tag,
             static_cast<unsigned long>(I2S0_TCR2), static_cast<unsigned long>(I2S0_TCR4),
             static_cast<unsigned long>(I2S0_RCR2), static_cast<unsigned long>(PORTE_PCR6),
             static_cast<unsigned long>(PORTE_PCR12));
    diagLogf("probe %s: dma tx ch %u CITER %u mux 0x%02X irqEn %d vec %s | rx ch %u CITER %u mux 0x%02X",
             tag, unsigned(tx), unsigned(txDma.TCD->CITER_ELINKNO),
             unsigned((&DMAMUX0_CHCFG0)[tx]), int(NVIC_IS_ENABLED(IRQ_DMA_CH0 + tx) != 0),
             _VectorsRam[16 + IRQ_DMA_CH0 + tx] == txIsr ? "txIsr" : "OTHER", unsigned(rx),
             unsigned(rxDma.TCD->CITER_ELINKNO), unsigned((&DMAMUX0_CHCFG0)[rx]));
    diagLogf("probe %s: DMA ERQ 0x%08lX ERR 0x%08lX ES 0x%08lX INT 0x%08lX VTOR 0x%08lX blocks %lu",
             tag, static_cast<unsigned long>(DMA_ERQ), static_cast<unsigned long>(DMA_ERR),
             static_cast<unsigned long>(DMA_ES), static_cast<unsigned long>(DMA_INT),
             static_cast<unsigned long>(SCB_VTOR), static_cast<unsigned long>(gBlocks));
}

void i2c0Scan(const char* tag) {
    char line[200];
    int n = snprintf(line, sizeof line, "probe %s: i2c0 ACK at", tag);
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c0().probe(a)) {
            found++;
            if (n < int(sizeof line) - 6) n += snprintf(line + n, sizeof line - size_t(n), " 0x%02X", a);
        }
    }
    if (!found) snprintf(line + n, sizeof line - size_t(n), " (none)");
    diagLog(line);
}

void audioProbe() {
    // Block-rate check: 128-frame blocks at Fs = 44117.6 Hz -> ~345 blocks/s (two
    // half-buffer interrupts per 256 frames). 15.94x that means MCLK is undivided.
    uint32_t b0 = gBlocks;
    delay(200);
    uint32_t rate = (gBlocks - b0) * 5;
    diagLogf("probe: audio blocks/s %lu (expect ~345)%s", static_cast<unsigned long>(rate),
             rate > 600 ? "  MCLK DIVIDER NOT APPLIED" : "");
    dumpI2sDma("t+5ms");
    i2c0Scan("t+5ms");
    delay(100);
    dumpI2sDma("t+105ms");
    delay(100);
    dumpI2sDma("t+205ms");
    i2c0Scan("t+205ms");
    diagLogf("probe: PCR D8 0x%08lX D9 0x%08lX A24 0x%08lX E26 0x%08lX E27 0x%08lX D6 0x%08lX",
             static_cast<unsigned long>(PORTD_PCR8), static_cast<unsigned long>(PORTD_PCR9),
             static_cast<unsigned long>(PORTA_PCR24), static_cast<unsigned long>(PORTE_PCR26),
             static_cast<unsigned long>(PORTE_PCR27), static_cast<unsigned long>(PORTD_PCR6));
    diagLogf("probe: GPIO PDOR A 0x%08lX D 0x%08lX E 0x%08lX | PDIR D 0x%08lX",
             static_cast<unsigned long>(GPIOA_PDOR), static_cast<unsigned long>(GPIOD_PDOR),
             static_cast<unsigned long>(GPIOE_PDOR), static_cast<unsigned long>(GPIOD_PDIR));
    // The codec may need longer after MCLK/power: retry for up to 1 s.
    for (int i = 0; i < 20; i++) {
        if (i2c0().probe(0x0A) || i2c0().probe(0x2A)) {
            diagLogf("probe: codec ACK after %d ms extra", i * 50);
            break;
        }
        if (i == 19) diagLog("probe: codec still silent after 1 s");
        delay(50);
    }
    uint8_t w[2] = {0, 0}, r[2] = {0, 0};
    for (uint8_t addr : {uint8_t(0x0A), uint8_t(0x2A)}) {
        bool ok = i2c0().writeRead(addr, w, 2, r, 2);
        diagLogf("probe: CHIP_ID @0x%02X %s 0x%02X%02X", unsigned(addr), ok ? "ack" : "NAK",
                 unsigned(r[0]), unsigned(r[1]));
    }
}

void dmaStart() {
    // TX first: the receiver runs synchronous to the transmitter (RCR2 SYNC=1), and
    // enabling it first starts the transmitter with an empty FIFO (field: FEF set,
    // TX DMA stuck after 2 transfers while RX DMA kept running).
    memset(txBuf, 0, sizeof(txBuf));
    txDma.begin(true);
    // spec 01 §5.3 TX column: 16-bit, SOFF 2, SLAST -1024, DADDR TDR0+2, CITER 512.
    txDma.TCD->SADDR = txBuf;
    txDma.TCD->SOFF = 2;
    txDma.TCD->ATTR = DMA_TCD_ATTR_SSIZE(1) | DMA_TCD_ATTR_DSIZE(1);
    txDma.TCD->NBYTES_MLNO = 2;
    txDma.TCD->SLAST = -int32_t(sizeof(txBuf));
    txDma.TCD->DADDR = reinterpret_cast<volatile uint8_t*>(&I2S0_TDR0) + 2;
    txDma.TCD->DOFF = 0;
    txDma.TCD->CITER_ELINKNO = sizeof(txBuf) / 2;
    txDma.TCD->DLASTSGA = 0;
    txDma.TCD->BITER_ELINKNO = sizeof(txBuf) / 2;
    txDma.TCD->CSR = DMA_TCD_CSR_INTHALF | DMA_TCD_CSR_INTMAJOR;
    txDma.triggerAtHardwareEvent(DMAMUX_SOURCE_I2S0_TX);   // src 13 on K66 (spec 01 §5.3)
    txDma.attachInterrupt(txIsr, board::kDmaPrioAudioTx);
    txDma.enable();
    // spec 01 §5.2 TCSR: TE | BCE | FRDE (+FR to flush the FIFO first).
    I2S0_TCSR = I2S_TCSR_FR;
    I2S0_TCSR = I2S_TCSR_TE | I2S_TCSR_BCE | I2S_TCSR_FRDE;
    rxDmaStart();
    // Clear any FIFO error latched during start-up (w1c, K66 RM SAI xCSR[FEF], bit 18).
    I2S0_TCSR |= (1u << 18);
    I2S0_RCSR |= (1u << 18);
}
}  // namespace

bool Sgtl5000Audio::codecWrite(uint16_t reg, uint16_t val) {
    uint8_t b[4] = {uint8_t(reg >> 8), uint8_t(reg), uint8_t(val >> 8), uint8_t(val)};
    return i2c0().write(board::kAddrSgtl5000, b, 4);   // spec 01 §5.1 wire format
}

bool Sgtl5000Audio::codecRead(uint16_t reg, uint16_t& val) {
    uint8_t w[2] = {uint8_t(reg >> 8), uint8_t(reg)}, r[2] = {0, 0};
    if (!i2c0().writeRead(board::kAddrSgtl5000, w, 2, r, 2)) return false;
    val = uint16_t((r[0] << 8) | r[1]);
    return true;
}

bool Sgtl5000Audio::init() {
    // Mute first (spec 01 §5.4: pin 21 LOW at boot).
    digitalWriteFast(board::kTeensyMute, LOW);
    pinMode(board::kTeensyMute, OUTPUT);

    diagLog("codec: I2S config");
    i2sConfig();
    diagLog("codec: audio DMA start");
    dmaStart();
    diagLog("codec: DMA running");

    // spec 01 §5.1 enable sequence, after 5 ms.
    delay(5);
    audioProbe();
    struct { uint16_t reg, val; uint16_t delayMs; } const seq[] = {
        {0x0030, 0x4060, 0},   // CHIP_ANA_POWER
        {0x0026, 0x006C, 0},   // CHIP_LINREG_CTRL
        {0x0028, 0x01F3, 0},   // CHIP_REF_CTRL (SMALL_POP=1)
        {0x002C, 0x0F22, 0},   // CHIP_LINE_OUT_CTRL
        {0x003C, 0x4446, 0},   // CHIP_SHORT_CTRL
        {0x0024, 0x0137, 0},   // CHIP_ANA_CTRL
        {0x0030, 0x40FF, 0},   // CHIP_ANA_POWER
        {0x0002, 0x0073, 400}, // CHIP_DIG_POWER, then 400 ms
        {0x002E, 0x1D1D, 0},   // CHIP_LINE_OUT_VOL
        {0x0004, 0x0004, 0},   // CHIP_CLK_CTRL (44.1 kHz, 256 Fs)
        {0x0006, 0x0130, 0},   // CHIP_I2S_CTRL (UNVERIFIED [I]: 32Fs setting on a 64Fs bus)
        {0x000A, 0x0010, 0},   // CHIP_SSS_CTRL
        {0x000E, 0x0000, 0},   // CHIP_ADCDAC_CTRL
        {0x0010, 0x3C3C, 0},   // CHIP_DAC_VOL
        {0x0022, 0x7F7F, 0},   // CHIP_ANA_HP_CTRL
        {0x0024, 0x0036, 0},   // CHIP_ANA_CTRL (line out on, HP muted)
    };
    // SGTL5000 datasheet: CHIP_ID (0x0000) bits 15:8 = PARTID 0xA0. Diagnostic only:
    // the stock firmware does not read it, so a mismatch is reported, not fatal.
    chipId_ = 0;
    bool idOk = codecRead(0x0000, chipId_);
    diagLogf("codec: CHIP_ID read %s 0x%04X", idOk ? "ack" : "NAK", unsigned(chipId_));
    codecOk_ = true;
    for (const auto& s : seq) {
        if (!codecWrite(s.reg, s.val)) {
            codecOk_ = false;
            diagLogf("codec: write 0x%04X=0x%04X failed", unsigned(s.reg), unsigned(s.val));
        }
        if (s.delayMs) delay(s.delayMs);
    }
    // spec 18 §1.1: DAP into the I2S->DAC path (DAP_CONTROL = 1, SSS = 0x70),
    // 5-band GEQ, surround off, bass enhance on at minimum. Added after the
    // proven enable sequence; output is still muted (pin LOW, HP muted).
    // CHIP_DIG_POWER 0x0073 above already powers the DAP (bit 4).
    // UNVERIFIED [I]: stock runs this at engine init, slightly later than here.
    if (codecOk_) {
        dap_.attach(dapBus());
        if (!dap_.init()) diagLog("codec: DAP init write failed");
    }
    return codecOk_;
}

sgtl_dap::Bus Sgtl5000Audio::dapBus() {
    sgtl_dap::Bus b;
    b.read = [](void* c, uint16_t reg, uint16_t& v) { return static_cast<Sgtl5000Audio*>(c)->codecRead(reg, v); };
    b.write = [](void* c, uint16_t reg, uint16_t v) { return static_cast<Sgtl5000Audio*>(c)->codecWrite(reg, v); };
    b.ctx = this;
    return b;
}

void Sgtl5000Audio::applyMasterDsp(const float eq[5], uint8_t bassBoost, uint8_t space) {
    if (codecOk_) dap_.apply(eq, bassBoost, space);
}

void Sgtl5000Audio::setHeadphoneVolume(float v) {
    // spec 01 §5.1 volume(): n = round(v*129); 0 -> HP_CTRL 0x7F7F + mute.
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    uint32_t n = uint32_t(v * 129.0f + 0.5f);
    if (n > 128) n = 128;
    uint16_t ana = 0;
    if (!codecRead(0x0024, ana)) return;
    if (n == 0) {
        codecWrite(0x0022, 0x7F7F);
        codecWrite(0x0024, uint16_t(ana | 0x0010));
    } else {
        codecWrite(0x0022, uint16_t((0x80 - n) * 0x0101));
        codecWrite(0x0024, uint16_t(ana & ~0x0010));
    }
}

bool Sgtl5000Audio::lineInLevel(uint8_t level) {
    // lineInLevel(l, r): CHIP_ANA_ADC_CTRL = (l << 4) | r, each <= 15 (spec 01 §5.1).
    if (level > 15) level = 15;
    return codecWrite(0x0020, uint16_t((level << 4) | level));
}

bool Sgtl5000Audio::micGain(uint8_t gainDb) {
    // micGain(dB) (spec 01 §5.1): MIC_CTRL 0x173/0x172/0x171/0x170 for >= 40/30/20/<20
    // dB (bias resistor 2k, bias 3.0 V, preamp 40/30/20/0 dB), the remainder in
    // 1.5 dB ADC steps: g = min(2n/3, 15), CHIP_ANA_ADC_CTRL = g | g << 4.
    uint16_t mic = 0x170;
    uint32_t n = gainDb;
    if (n >= 40) { mic = 0x173; n -= 40; }
    else if (n >= 30) { mic = 0x172; n -= 30; }
    else if (n >= 20) { mic = 0x171; n -= 20; }
    uint32_t g = 2 * n / 3;
    if (g > 15) g = 15;
    bool ok = codecWrite(0x002A, mic);
    return codecWrite(0x0020, uint16_t(g | (g << 4))) && ok;
}

bool Sgtl5000Audio::selectLineIn(uint8_t level) {
    // spec 22 §3: inputSelect(0), then the gain call (lineInLevel). MIC_CTRL
    // is left as it is, as stock.
    uint16_t ana = 0;
    if (!codecRead(0x0024, ana)) return false;
    // inputSelect(0): CHIP_ANA_CTRL bit 2 SELECT_ADC = 1 (LINEIN).
    bool ok = codecWrite(0x0024, uint16_t(ana | 0x0004));
    return lineInLevel(level) && ok;
}

bool Sgtl5000Audio::selectMic(uint8_t gainDb) {
    uint16_t ana = 0;
    if (!codecRead(0x0024, ana)) return false;
    // inputSelect(1): SELECT_ADC = 0 (MIC), then the gain call (micGain).
    bool ok = codecWrite(0x0024, uint16_t(ana & ~0x0004));
    return micGain(gainDb) && ok;
}

const int16_t* Sgtl5000Audio::lineInBlock(void*) {
    // A halted receiver (FEF, or no new RX block for a few blocks) gives no
    // block at all instead of the last one again (rx_watch.h).
    const int16_t* src = gRxWatch.pick(gRxBlocks, (I2S0_RCSR & kRcsrFef) != 0, gRxHalf, rxBuf, kFrames * 2);
    if (!src) return nullptr;
    memcpy(rxCopy, src, sizeof(rxCopy));
    if (gRxPeakOn) {
        int32_t pl = gRxPeakL, pr = gRxPeakR;
        for (uint32_t i = 0; i < kFrames; ++i) {
            const int32_t l = rxCopy[2 * i] < 0 ? -int32_t(rxCopy[2 * i]) : rxCopy[2 * i];
            const int32_t r = rxCopy[2 * i + 1] < 0 ? -int32_t(rxCopy[2 * i + 1]) : rxCopy[2 * i + 1];
            if (l > pl) pl = l;
            if (r > pr) pr = r;
        }
        gRxPeakL = pl;
        gRxPeakR = pr;
        gRxPeakBlocks = gRxPeakBlocks + 1;
    }
    return rxCopy;
}

void Sgtl5000Audio::setRxPeak(bool on) { gRxPeakOn = on; }

void Sgtl5000Audio::takeRxPeak(int& peakL, int& peakR, uint32_t& blocks) {
    __disable_irq();
    peakL = int(gRxPeakL);
    peakR = int(gRxPeakR);
    blocks = gRxPeakBlocks;
    gRxPeakL = gRxPeakR = 0;
    gRxPeakBlocks = 0;
    __enable_irq();
}

uint32_t Sgtl5000Audio::rxBlocks() const { return gRxBlocks; }

uint32_t Sgtl5000Audio::rxRecoveries() { return gRxWatch.recoveries(); }
uint32_t Sgtl5000Audio::rxStaleBlocks() { return gRxWatch.staleBlocks(); }
uint32_t Sgtl5000Audio::rxStatus() { return I2S0_RCSR; }

void Sgtl5000Audio::serviceRx() {
    if (!gRxStarted || !gRxWatch.needsRecovery()) return;
    const uint32_t now = millis();
    if (now - gRxLastRecoverMs < 100) return;   // at most 10 restarts per second
    gRxLastRecoverMs = now;
    const uint32_t rcsr = I2S0_RCSR, blocks = gRxBlocks;
    const bool fef = gRxWatch.stallWasFifoError();
    rxRestart();
    __disable_irq();
    gRxWatch.recovered(gRxBlocks);
    __enable_irq();
    const uint32_t n = gRxWatch.recoveries();
    if (n <= 10 || n % 100 == 0)
        diagLogf("audio: I2S RX halted (%s, RCSR 0x%08lX, rx blocks %lu, stale blocks %lu) -> receiver "
                 "restarted, RCSR 0x%08lX (restart #%lu)",
                 fef ? "FIFO error" : "no new blocks", static_cast<unsigned long>(rcsr),
                 static_cast<unsigned long>(blocks), static_cast<unsigned long>(gRxWatch.staleBlocks()),
                 static_cast<unsigned long>(I2S0_RCSR), static_cast<unsigned long>(n));
}

void Sgtl5000Audio::powerDown() {
    digitalWriteFast(board::kTeensyMute, LOW);
    codecWrite(0x0030, 0x0000);
}

uint32_t Sgtl5000Audio::blocksRendered() const { return gBlocks; }

void Sgtl5000Audio::renderCycles(uint32_t& minC, uint32_t& avgC, uint32_t& maxC) const {
    __disable_irq();
    uint32_t n = gCycCount;
    minC = n ? gCycMin : 0;
    maxC = gCycMax;
    avgC = n ? uint32_t(gCycSum / n) : 0;
    __enable_irq();
}

uint32_t Sgtl5000Audio::renderCyclesWindow(uint32_t& minC, uint32_t& avgC, uint32_t& maxC) {
    __disable_irq();
    const uint32_t n = gWinCount;
    minC = n ? gWinMin : 0;
    maxC = gWinMax;
    avgC = n ? uint32_t(gWinSum / n) : 0;
    gWinMin = 0xFFFFFFFFu;
    gWinMax = 0;
    gWinSum = 0;
    gWinCount = 0;
    __enable_irq();
    return n;
}

void Sgtl5000Audio::setCallback(FillFn fn, void* ctx) {
    __disable_irq();
    gFill = fn;
    gCtx = ctx;
    __enable_irq();
}

bool Sgtl5000Audio::start() {
    // The mute line stays LOW: the app unmutes after the start-up load
    // (setOutputEnabled; unmuting here gave a crack at power-on, R3-4).
    gRunning = true;
    return codecOk_;
}

void Sgtl5000Audio::setOutputEnabled(bool on) {
    digitalWriteFast(board::kTeensyMute, on ? HIGH : LOW);   // spec 01 §5.4
}

void Sgtl5000Audio::stop() {
    digitalWriteFast(board::kTeensyMute, LOW);
    gRunning = false;
}

}  // namespace hal
}  // namespace ot
