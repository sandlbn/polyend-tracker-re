// OpenTracker -- device boot diagnostics over USB CDC (see diag.h).
#include "diag.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include "board.h"
#include "diag_log.h"
#include "k66.h"
#include "sdhc.h"
#include "usb_dev.h"
#include "usb_mem.h"

namespace ot {
namespace hal {

namespace {
diag::LogBuf gLog;
static_assert(sizeof(diag::LogBuf) <= 8192, "gLog SRAM budget (docs/OPTIMIZATION_2X.md OP-5)");
volatile uint32_t gStage = 0;
const char* volatile gStageName = "reset";
volatile uint32_t gBeat = 0;
uint32_t gWatchStage = 0, gWatchBeat = 0, gWatchMs = 0;
volatile uint32_t gLastDrainMs = 0;
void (*gOrigSystick)() = nullptr;
bool gInit = false;
volatile bool gWatchOn = true;
// Fault report text (registers + last 40 log lines): written into the SD
// bounce buffer, which nothing uses after a fault (CPU round 3: 4 KB of SRAM).
char* gFaultText = nullptr;
uint32_t gFaultCap = 0;

constexpr uint32_t kSramLo = 0x1FFF0000u, kSramHi = 0x20030000u;   // MK66 SRAM_L + SRAM_U
constexpr uint32_t kTxQueueMax = 4;       // of our CDC packets in flight (Teensy limit 8)
constexpr uint8_t kDrainPriority = 112;   // == USB ISR: neither preempts the other

inline uint32_t lock() {
    uint32_t p;
    __asm__ volatile("mrs %0, primask" : "=r"(p));
    __disable_irq();
    return p;
}
inline void unlock(uint32_t p) {
    if (!(p & 1)) __enable_irq();
}

void appendLine(const char* text) {
    uint32_t p = lock();
    diag::line(gLog, systick_millis_count, text);
    unlock(p);
}

// Non-blocking: hands at most a few 64-byte packets to the USB stack. Never
// called with our lock held (usb_* re-enable interrupts unconditionally).
void drainCore() {
    if (!usb_configuration) return;
    // Host command: any received byte replays the boot log.
    for (usb_packet_t* rx; (rx = usb_rx(CDC_RX_ENDPOINT)) != nullptr;) {
        bool any = rx->len > 0;
        usb_free(rx);
        if (any) {
            uint32_t p = lock();
            diag::requestReplay(gLog);
            unlock(p);
        }
    }
    if (!(usb_cdc_line_rtsdtr & USB_SERIAL_DTR)) return;
    for (int i = 0; i < 8; i++) {
        uint32_t p = lock();
        bool pend = diag::pending(gLog);
        unlock(p);
        if (!pend) return;
        if (usb_tx_packet_count(CDC_TX_ENDPOINT) >= kTxQueueMax) return;
        usb_packet_t* pk = usb_malloc();
        if (!pk) return;
        p = lock();
        size_t n = diag::take(gLog, reinterpret_cast<char*>(pk->buf), CDC_TX_SIZE);
        unlock(p);
        if (!n) {
            usb_free(pk);
            return;
        }
        pk->len = uint16_t(n);
        pk->index = 0;
        usb_tx(CDC_TX_ENDPOINT, pk);
    }
}

void drainIsr() {
    gLastDrainMs = systick_millis_count;
    drainCore();
}

bool frameOk(const uint32_t* frame) {
    uint32_t a = reinterpret_cast<uint32_t>(frame);
    return (a & 3) == 0 && a >= kSramLo && a + 32 <= kSramHi;
}

void pendDrain() {
    if (gInit) NVIC_SET_PENDING(IRQ_SOFTWARE);
}
}  // namespace

// SysTick: Teensy millis first, then (every 10 ms) the stuck watchdog and the
// drain request. `frame` is the interrupted context's exception frame.
extern "C" __attribute__((used)) void diagSystickC(uint32_t* frame, uint32_t excReturn) {
    gOrigSystick();
    const uint32_t now = systick_millis_count;
    if (now % 10) return;
    const uint32_t st = gStage, beat = gBeat;
    if (st != gWatchStage || beat != gWatchBeat) {
        gWatchStage = st;
        gWatchBeat = beat;
        gWatchMs = now;
    } else if (gWatchOn && now - gWatchMs >= 10000) {
        gWatchMs = now;
        char b[200];
        diag::Fmt f(b, sizeof b);
        f.str("STUCK at stage ").dec(st).str(" '").str(gStageName).str("' for 10 s: ");
        if (frameOk(frame))
            f.str("PC ").hex(frame[6]).str(" LR ").hex(frame[5]).str(" xPSR ").hex(frame[7]);
        else
            f.str("frame unreadable");
        f.str(" (").str((excReturn & 4) ? "PSP" : "MSP").str(" ").hex(reinterpret_cast<uint32_t>(frame))
            .str(") ICSR ").hex(SCB_ICSR);
        appendLine(b);
    }
    // Emergency path: the drain IRQ has not run for 3 s (something at USB
    // priority or above is hogging the CPU) -- push from here instead.
    if (now - gLastDrainMs > 3000) {
        gLastDrainMs = now;
        drainCore();
    } else {
        NVIC_SET_PENDING(IRQ_SOFTWARE);
    }
}

extern "C" __attribute__((naked)) void diagSystickEntry() {
    __asm__ volatile(
        "tst lr, #4\n\t"
        "ite eq\n\t"
        "mrseq r0, msp\n\t"
        "mrsne r0, psp\n\t"
        "mov r1, lr\n\t"
        "b diagSystickC\n\t");
}

extern "C" __attribute__((used, noreturn)) void diagFaultC(uint32_t* frame, uint32_t excReturn) {
    __disable_irq();
    diag::FaultInfo f = {};
    __asm__ volatile("mrs %0, ipsr" : "=r"(f.ipsr));
    f.excReturn = excReturn;
    f.sp = reinterpret_cast<uint32_t>(frame);
    f.frameValid = frameOk(frame);
    if (f.frameValid) {
        f.r0 = frame[0]; f.r1 = frame[1]; f.r2 = frame[2]; f.r3 = frame[3];
        f.r12 = frame[4]; f.lr = frame[5]; f.pc = frame[6]; f.xpsr = frame[7];
    }
    f.cfsr = *reinterpret_cast<volatile uint32_t*>(0xE000ED28u);
    f.hfsr = *reinterpret_cast<volatile uint32_t*>(0xE000ED2Cu);
    f.mmfar = *reinterpret_cast<volatile uint32_t*>(0xE000ED34u);
    f.bfar = *reinterpret_cast<volatile uint32_t*>(0xE000ED38u);
    f.stage = gStage;
    f.stageName = gStageName;
    f.ms = systick_millis_count;

    // One line into the log stream, then the full report text.
    char l[160];
    diag::Fmt lf(l, sizeof l);
    lf.str("FAULT ").str(diag::exceptionName(f.ipsr)).str(" PC ").hex(f.pc).str(" CFSR ").hex(f.cfsr)
        .str(" BFAR ").hex(f.bfar).str(" at stage ").dec(f.stage);
    // [I] A SYSMPU denial (e.g. the stack guard, stack_guard.h): CESR SPERR
    // bit 31 - n flags slave port n; its error address / detail registers
    // sit at 0x4000D010 + 8 n.
    const uint32_t cesr = MPU_CESR;
    for (int port = 0; port < 5; ++port)
        if (cesr & (1u << (31 - port))) {
            const volatile uint32_t* er = reinterpret_cast<volatile uint32_t*>(0x4000D010u + 8u * uint32_t(port));
            lf.str(" MPU port ").dec(uint32_t(port)).str(" EAR ").hex(er[0]).str(" EDR ").hex(er[1]);
            break;
        }
    diag::line(gLog, f.ms, l);
    gFaultText = reinterpret_cast<char*>(sdhc::takeBounceForFault(&gFaultCap));
    if (gFaultCap > 4096) gFaultCap = 4096;
    size_t n = diag::formatFault(gFaultText, gFaultCap, f);
    diag::Fmt t(gFaultText + n, gFaultCap - n);
    t.str("--- last 40 log lines:\r\n");
    n += t.len;
    n += diag::copyFrom(gLog, diag::tailStart(gLog, 40), gFaultText + n, gFaultCap - n - 96);
    diag::Fmt e(gFaultText + n, gFaultCap - n);
    e.str("--- end of fault report (repeats every 1 s; send any byte for the boot log)\r\n\r\n");
    n += e.len;

    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
    gpioOutput(board::kPinStatusLed, false);
    uint32_t lastBlink = ARM_DWT_CYCCNT, lastReport = lastBlink - F_CPU;
    for (;;) {
        if (SIM_SCGC4 & SIM_SCGC4_USBOTG) usb_isr();
        const uint32_t c = ARM_DWT_CYCCNT;
        if (c - lastBlink >= F_CPU / 10) {   // 5 Hz blink
            lastBlink = c;
            gpio(board::kPinStatusLed).PTOR = 1u << board::kPinStatusLed.bit;
        }
        if (c - lastReport >= F_CPU && (!gLog.extra || gLog.extraPos >= gLog.extraLen)) {
            lastReport = c;
            gLog.extra = gFaultText;
            gLog.extraLen = uint32_t(n);
            gLog.extraPos = 0;
        }
        drainCore();
        __disable_irq();   // usb_* re-enable; keep the handler masked
    }
}

extern "C" __attribute__((naked)) void diagFaultEntry() {
    __asm__ volatile(
        "tst lr, #4\n\t"
        "ite eq\n\t"
        "mrseq r0, msp\n\t"
        "mrsne r0, psp\n\t"
        "mov r1, lr\n\t"
        "b diagFaultC\n\t");
}

void diagInit() {
    if (gInit) return;
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
    __disable_irq();
    for (int v = 3; v <= 6; v++) _VectorsRam[v] = diagFaultEntry;
    gOrigSystick = _VectorsRam[15];
    _VectorsRam[15] = diagSystickEntry;
    _VectorsRam[IRQ_SOFTWARE + 16] = drainIsr;
    gLastDrainMs = gWatchMs = systick_millis_count;
    gInit = true;
    __enable_irq();
    NVIC_SET_PRIORITY(IRQ_SOFTWARE, kDrainPriority);
    NVIC_ENABLE_IRQ(IRQ_SOFTWARE);
    diagLogf("OpenTracker diag log %s, built %s %s. Send any byte to replay the boot log.", OT_VERSION,
             __DATE__, __TIME__);
    diagLogf("RCM_SRS0 0x%02X RCM_SRS1 0x%02X SIM_CLKDIV1 0x%08lX SCB_SHCSR 0x%08lX", unsigned(RCM_SRS0),
             unsigned(RCM_SRS1), static_cast<unsigned long>(SIM_CLKDIV1),
             static_cast<unsigned long>(SCB_SHCSR));
}

void diagLog(const char* text) {
    appendLine(text);
    pendDrain();
}

void diagLogf(const char* fmt, ...) {
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    diagLog(b);
}

void diagStage(const char* name) {
    gStageName = name;
    gStage = gStage + 1;
    diagLogf("== stage %lu: %s", static_cast<unsigned long>(gStage), name);
}

uint32_t diagStageNumber() { return gStage; }
void diagBeat() { gBeat = gBeat + 1; }
void diagWatchdog(bool on) {
    gWatchOn = on;
    diagBeat();
}

bool diagTerminalOpen() { return usb_configuration && (usb_cdc_line_rtsdtr & USB_SERIAL_DTR); }

void diagFreezeBoot() {
    uint32_t p = lock();
    gLog.bootFrozen = true;
    unlock(p);
}

}  // namespace hal
}  // namespace ot
