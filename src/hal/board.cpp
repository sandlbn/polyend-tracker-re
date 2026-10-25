// OpenTracker -- board bring-up (spec 01 §0, §1, §2, §10) and HAL singletons.
#include "hal.h"

#include <Arduino.h>
#include "audio_sgtl5000.h"
#include "app/input_gain.h"
#include "board.h"
#include "diag.h"
#include "ft81x.h"
#include "fw_update.h"
#include "hw_math.h"
#include "i2c_bus.h"
#include "input.h"
#include "k66.h"
#include "midi_ports.h"
#include "pad_leds.h"
#include "radio_si4703.h"
#include "sd_storage.h"
#include "sdram_cache.h"
#include "sdhc.h"
#include "telemetry.h"
#include "app/project_io.h"
#include "audio/cpu_meter.h"

static_assert(F_CPU == ot::board::kCoreHz && F_BUS == ot::board::kBusHz, "spec 01 §1 clocks");
static_assert(ot::hal::hw::sdramRefreshCount(ot::board::kFlexBusHz, 15625) ==
                  (ot::board::kSdramCtrl & 0x1FFu),
              "spec 01 §2: SDRAMC_CTRL.RC for 15625 ns at the FlexBus clock");

// Linker script symbols (opentracker.ld): the .fastrun code in SRAM_L.
extern "C" char __fastrun_start[], __fastrun_end[];
// Area boundaries inside .fastrun (opentracker.ld), in link order.
extern "C" char __fastrun_smp[], __fastrun_fx[], __fastrun_mix[], __fastrun_vap[], __fastrun_acd[],
    __fastrun_perc[], __fastrun_fat[], __fastrun_other[];

namespace ot {
namespace hal {

uint32_t SystemClock::micros() const { return ::micros(); }
uint32_t SystemClock::millis() const { return ::millis(); }

namespace {
Ft81xDisplay gDisplay;
Sgtl5000Audio gAudio;
PanelInput gInput;
PadLeds gLeds;
SdStorage gStorage;
MidiPorts gMidi;
SystemClock gClock;
BootReport gReport;
HalSet gHal = {&gDisplay, &gAudio, &gStorage, &gMidi, &gInput, &gLeds, &gClock, &service};

// Power button (spec 01 §10, spec 05 power-button handler). Button ids from
// spec 05 §2.1.1 (core ui/buttons.h): Power is the virtual slot 0x2a, Shift 0x1d.
constexpr uint8_t kBtnPower = 0x2a, kBtnShift = 0x1d;
bool gPowerArmed = false;      // seen released once since boot (a press that woke us is ignored)
// Debounced state. Starts as "down": the wake-up press is released long before
// the first service(), and only a release edge arms the button. A released
// button at the first service() is that edge; a press still held stays unarmed.
bool gPowerDown = true;
bool gPowerRaw = false;
uint32_t gPowerEdgeMs = 0;
uint32_t gPowerPressMs = 0;
bool gPowerShiftAtPress = false;
void (*gPowerOffHook)() = nullptr;

// Recorder input routing state (selectInput).
int8_t gInputSource = -1;
uint8_t gInputGain = 0;
bool gInputOk = false;

// Telemetry "io" line (telemetry.h formatIo): SD commands / bytes per window and
// the cache state, logged right before the app's render line of the same window.
sdhc::Counters gIoLast;
void ioTelemetry(uint32_t windowMs) {
    const sdhc::Counters c = sdhc::counters();
    diag::IoSample s;
    s.windowMs = windowMs;
    s.sdCmds = c.cmds - gIoLast.cmds;
    s.sdReadBytes = c.readBytes - gIoLast.readBytes;
    s.sdWriteBytes = c.writeBytes - gIoLast.writeBytes;
    s.sdDirect = c.direct - gIoLast.direct;
    s.sdBounced = c.bounced - gIoLast.bounced;
    gIoLast = c;
    if (!diagTerminalOpen()) return;
    s.cacheOn = cache::sdramCached();
    s.cacheTest = cache::testName(cache::status().test);
    s.sdDirectRead = sdhc::sdramDirectRead();
    s.sdDirectWrite = sdhc::sdramDirectWrite();
    char line[240];
    diag::formatIo(line, sizeof line, s);
    diagLog(line);
    // Background project job profile: while one runs, and once more after it ended.
    const JobProfile& jp = jobProfile();
    static uint32_t lastPasses = 0;
    if (jp.running || jp.passes != lastPasses) {
        lastPasses = jp.passes;
        diag::formatLoad(line, sizeof line, jp);
        diagLog(line);
    }
}

uint32_t dwtCycles() { return ARM_DWT_CYCCNT; }

void sdramInit() {
    // spec 01 §2 sequence
    diagLogf("sdram: SIM_CLKDIV1 0x%08lX -> OUTDIV3=1", static_cast<unsigned long>(SIM_CLKDIV1));
    SIM_CLKDIV1 |= 0x00100000;                       // 1: OUTDIV3=1 -> FlexBus 90 MHz
    delay(10);                                       // 2
    diagLog("sdram: pins");
    for (const auto& p : board::kSdramPins) pcr(p) = board::kSdramPcr;   // 3
    SIM_SOPT2 = (SIM_SOPT2 & ~0xE0u) | 0x300u;       // 4: CLKOUTSEL=0, FBSL=3
    diagLogf("sdram: SIM_SOPT2 0x%08lX, FlexBus clock gate", static_cast<unsigned long>(SIM_SOPT2));
    SIM_SCGC7 |= SIM_SCGC7_FLEXBUS;                  // 5
    diagLogf("sdram: SIM_SCGC7 0x%08lX, FB_CSPMCR 0x%08lX", static_cast<unsigned long>(SIM_SCGC7),
             static_cast<unsigned long>(OT_FB_CSPMCR));
    OT_FB_CSPMCR = (OT_FB_CSPMCR & board::kFbCspmcrMask) | board::kFbCspmcrVal;  // 6
    SIM_SCGC7 |= SIM_SCGC7_SDRAMC;
    diagLogf("sdram: SDRAMC gate on (SCGC7 0x%08lX), CTRL/AC0/CM0 = 0x%08lX/0x%08lX/0x%08lX",
             static_cast<unsigned long>(SIM_SCGC7), static_cast<unsigned long>(board::kSdramCtrl),
             static_cast<unsigned long>(board::kSdramAc0), static_cast<unsigned long>(board::kSdramCm0));
    OT_SDRAM_CTRL = board::kSdramCtrl;               // 7
    OT_SDRAM_AC0 = board::kSdramAc0;
    OT_SDRAM_CM0 = board::kSdramCm0;
    // JEDEC SDR SDRAM power-up (K66 RM SDRAMC "Initialization sequence"): the
    // clock (CLKOUT, live since step 4) must be stable with NOPs for >= 100 us
    // before the first command. The stock code gets this from the time spent in
    // the SDK driver; make it explicit.
    diagLogf("sdram: regs readback CTRL 0x%08lX AC0 0x%08lX CM0 0x%08lX", static_cast<unsigned long>(OT_SDRAM_CTRL),
             static_cast<unsigned long>(OT_SDRAM_AC0), static_cast<unsigned long>(OT_SDRAM_CM0));
    delayMicroseconds(200);
    // 8: init commands. IP / IMRS self-clear when the command has been issued.
    diagLog("sdram: precharge-all (IP) + first write to 0x70000000");
    OT_SDRAM_AC0 |= kSdramAcIp;
    *reinterpret_cast<volatile uint8_t*>(board::kSdramBase) = 0xAA;
    int ipWait = 0;
    for (; ipWait < 100000 && (OT_SDRAM_AC0 & kSdramAcIp); ipWait++) {}
    diagLogf("sdram: IP cleared after %d polls, AC0 0x%08lX; auto-refresh on", ipWait,
             static_cast<unsigned long>(OT_SDRAM_AC0));
    OT_SDRAM_AC0 |= kSdramAcRe;
    // The SDRAM needs >= 2 (many parts: 8) auto-refresh cycles before MRS. One
    // refresh every (RC+1)*16 bus clocks = 15.5 us (hw::sdramRefreshPeriodNs),
    // so 200 us covers > 8. (spec 01 §2: busy-wait 125 iterations.)
    delayMicroseconds(200);
    diagLog("sdram: mode register set (IMRS)");
    OT_SDRAM_AC0 |= kSdramAcImrs;
    // UNVERIFIED: mode word encoded in the address 0x70001800 (spec 01 §12 item 2).
    *reinterpret_cast<volatile uint8_t*>(board::kSdramMrsAddr) = 0xAA;
    int mrsWait = 0;
    for (; mrsWait < 100000 && (OT_SDRAM_AC0 & kSdramAcImrs); mrsWait++) {}
    delayMicroseconds(10);                           // tMRD + margin before the first access
    diagLogf("sdram: IMRS cleared after %d polls, AC0 0x%08lX CTRL 0x%08lX", mrsWait,
             static_cast<unsigned long>(OT_SDRAM_AC0), static_cast<unsigned long>(OT_SDRAM_CTRL));
}

void sdramPinsOff() {
    for (const auto& p : board::kSdramPins) pcr(p) = 0;   // spec 01 §2 (0xf14 on soft-off)
}

}  // namespace

void statusLed(bool on) { gpioWrite(board::kPinStatusLed, !on); }   // spec 01 §10: HIGH = off

uint32_t bootFailMask(const BootReport& r) {
    uint32_t m = 0;
    auto set = [&m](hw::BlinkCode c) { m |= 1u << (c - 1); };
    if (!r.displayOk) set(hw::kBlinkDisplay);
    if (!r.sdramOk) set(hw::kBlinkSdram);
    if (!r.codecOk) set(hw::kBlinkCodec);
    if (!r.buttonsOk) set(hw::kBlinkButtons);
    if (!r.padsOk) set(hw::kBlinkPads);
    if (!r.ledsOk) set(hw::kBlinkPadLeds);
    if (r.sdPresent && !r.sdMounted) set(hw::kBlinkSd);
    if (!r.clocksOk) set(hw::kBlinkClocks);
    return m;
}

Ft81xDisplay& display() { return gDisplay; }
Sgtl5000Audio& audio() { return gAudio; }
PanelInput& input() { return gInput; }
PadLeds& leds() { return gLeds; }
SdStorage& storage() { return gStorage; }
MidiPorts& midi() { return gMidi; }
HalSet& halSet() { return gHal; }
const BootReport& bootReport() { return gReport; }

bool powerSwitchOn() {
    return digitalReadFast(board::kTeensyPowerSwitch) == LOW;   // spec 01 §10
}

bool sdramTest(uint32_t bytes, uint32_t* errors) {
    volatile uint32_t* m = reinterpret_cast<volatile uint32_t*>(board::kSdramBase);
    uint32_t n = bytes / 4, err = 0;
    if (n > board::kSdramSize / 4) n = board::kSdramSize / 4;
    // data bus: walking ones
    for (int b = 0; b < 32; b++) {
        m[0] = 1u << b;
        if (m[0] != (1u << b)) err++;
    }
    // address bus: power-of-two offsets must not alias
    for (uint32_t off = 1; off < n; off <<= 1) m[off] = 0xAAAAAAAAu;
    m[0] = 0x55555555u;
    for (uint32_t off = 1; off < n; off <<= 1)
        if (m[off] != 0xAAAAAAAAu) err++;
    diagLogf("sdram test %lu KB: data+address bus done, %lu errors", static_cast<unsigned long>(n / 256),
             static_cast<unsigned long>(err));
    // fill + verify, then inverted (spec 01 §2: factory test uses a 16 MB pattern).
    // Progress is logged every 1 MB (diag.h).
    constexpr uint32_t kMbWords = (1u << 20) / 4;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t inv = pass ? 0xFFFFFFFFu : 0;
        for (uint32_t i = 0; i < n; i++) {
            m[i] = (i * 0x9E3779B1u) ^ inv;
            if ((i + 1) % kMbWords == 0) {
                diagLogf("sdram test pass %d fill %lu MB", pass, static_cast<unsigned long>((i + 1) / kMbWords));
                diagBeat();
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            if (m[i] != ((i * 0x9E3779B1u) ^ inv)) {
                if (err < 8)
                    diagLogf("sdram mismatch @0x%08lX: 0x%08lX want 0x%08lX",
                             static_cast<unsigned long>(board::kSdramBase + 4 * i),
                             static_cast<unsigned long>(m[i]),
                             static_cast<unsigned long>((i * 0x9E3779B1u) ^ inv));
                err++;
            }
            if ((i + 1) % kMbWords == 0) {
                diagLogf("sdram test pass %d verify %lu MB, %lu errors", pass,
                         static_cast<unsigned long>((i + 1) / kMbWords), static_cast<unsigned long>(err));
                diagBeat();
            }
        }
    }
    if (errors) *errors = err;
    return err == 0;
}

const BootReport& boardEarlyInit() {
    BootReport& r = gReport;
    static bool done = false;
    if (done) return r;
    done = true;
    // Status LED on for the whole bring-up (spec 01 §0 step 14 drives the same pin
    // as an output, HIGH idle). A hang in a driver leaves it lit.
    gpioOutput(board::kPinStatusLed, false);
    // Fault vectors in the RAM vector table (Teensy core: SCB_VTOR = _VectorsRam).
    // (Fault vectors: diagInit() in main(), diag.cpp.)
    diagInit();
    // spec 01 §1: the Teensy core already ran the clock setup; with F_BUS=90 MHz it
    // programs SIM_CLKDIV1 = 0x01060000 exactly like the stock firmware.
    r.clocksOk = SIM_CLKDIV1 == board::kSimClkdiv1;

    // §0 steps 1-2 (EEPROM power flag, soft-off gate) run later, after the
    // ot_restore check, in bootPowerGate() (called from main()).
    pinMode(board::kTeensyPowerSwitch, INPUT);                   // spec 01 §3.2 (no pull)

    // Deliberate deviation from the spec 01 §0 order: the rail and the SD card come
    // first so the SD-only restore path (fw_update.h checkRestoreTrigger) works even
    // if a later driver (I2C, SDRAM, FT81x, codec) misbehaves.
    r.stage = 4;
    diagStage("step 4: rail enable");
    // step 4: peripheral rail enable. UNVERIFIED [I] (spec 01 §10).
    gpioOutput(board::kPinRailEnable, true);
    delay(2);
    // PTE26/PTE27 (radio switch / Si4703 RST_N) stay unconfigured until after the
    // codec, like stock (spec 08 §1): the codec did not ACK in the field while we
    // drove them this early.

    r.stage = 15;
    diagStage("step 15: SD card detect + mount");
    // step 15: SD card detect + mount (all SDHC waits are bounded, sdhc.cpp).
    pinMode(board::kTeensyCardDetect, INPUT_PULLUP);
    delay(2);
    r.sdPresent = digitalReadFast(board::kTeensyCardDetect) == LOW;
    diagLogf("sd: card detect %s", r.sdPresent ? "present" : "absent");
    r.sdMounted = gStorage.mount();
    diagLogf("sd: mounted %d", int(r.sdMounted));
    return r;
}

const BootReport& boardInit() {
    BootReport& r = gReport;
    boardEarlyInit();

    r.stage = 3;
    diagStage("step 3: I2C0 bus recovery");
    // step 3: I2C0 + I2C2 (pad bus) at 400 kHz class. A slave may still hold SDA
    // from before an MCU reset, so start with the spec 01 §3.2 bus recovery
    // (recover() ends with the normal begin()).
    diagLogf("i2c: SIM_SCGC4 0x%08lX SIM_SCGC1 0x%08lX", static_cast<unsigned long>(SIM_SCGC4),
             static_cast<unsigned long>(SIM_SCGC1));
    i2c0().recover();
    diagStage("step 3: I2C2 bus recovery");
    i2c2().recover();
    diagLogf("i2c: recovered, SIM_SCGC4 0x%08lX SIM_SCGC1 0x%08lX", static_cast<unsigned long>(SIM_SCGC4),
             static_cast<unsigned long>(SIM_SCGC1));

    r.stage = 5;
    diagStage("step 5: SDRAM init");
    // step 5: SDRAM + quick test (1 MB; the selftest runs the full 16 MB).
    sdramInit();
    diagStage("step 5: SDRAM quick test 1 MB");
    r.sdramOk = sdramTest(1u << 20, &r.sdramErrors);
    diagLogf("sdram: quick test %s, %lu errors", r.sdramOk ? "OK" : "FAIL",
             static_cast<unsigned long>(r.sdramErrors));

    // step 6: DWT cycle counter.
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
    ARM_DWT_CYCCNT = 0;

    // Background job profile in DWT cycles (core app/project_io.h).
    setJobProfileCycles(&dwtCycles, board::kCoreHz / 1000000u);
    // 2.x CPU meter / governor: render cycles per block (core audio/cpu_meter.h).
    audio::CpuMeter::setCycleSource(&dwtCycles);
    {   // Hot audio code copied to SRAM_L with .data (core fastrun.h, OT_FASTRUN).
        const uint32_t n = uint32_t(__fastrun_end - __fastrun_start);
#if defined(OT_FASTRUN) && OT_FASTRUN
        // Per area (opentracker.ld): sample voices, delay / reverb / limiter,
        // master sum + synth framework, the four engines, the rest (memset,
        // veneers). docs/OPTIMIZATION_2X.md "CPU round 3" lists the functions.
        auto sz = [](const char* a, const char* b) { return static_cast<unsigned long>(b - a); };
        diagLogf("fastrun: %lu B of audio code in SRAM_L at 0x%08lX (OT_FASTRUN=1): smp %lu fx %lu mix %lu vap %lu "
                 "acd %lu perc %lu fat %lu other %lu",
                 static_cast<unsigned long>(n), static_cast<unsigned long>(reinterpret_cast<uintptr_t>(__fastrun_start)),
                 sz(__fastrun_smp, __fastrun_fx), sz(__fastrun_fx, __fastrun_mix), sz(__fastrun_mix, __fastrun_vap),
                 sz(__fastrun_vap, __fastrun_acd), sz(__fastrun_acd, __fastrun_perc), sz(__fastrun_perc, __fastrun_fat),
                 sz(__fastrun_fat, __fastrun_other), sz(__fastrun_other, __fastrun_end));
#else
        diagLogf("fastrun: off (OT_FASTRUN=0), %lu B in SRAM_L", static_cast<unsigned long>(n));
#endif
    }

    // 2.x performance workstream: system cache for the SDRAM (write-through,
    // coherency self-test, sdram_cache.h), then ADMA2 straight into SDRAM behind
    // its own check (sdhc.h). Both use SDRAM the quick test just overwrote.
    if (r.sdramOk) {
        diagStage("step 6b: SDRAM cache + coherency selftest");
        cache::initSdram(reinterpret_cast<uint8_t*>(board::kSdramCheckScratch));
        if (r.sdMounted) {
            diagStage("step 6c: SD ADMA2 direct-to-SDRAM check");
            gStorage.checkDirectDma(reinterpret_cast<uint8_t*>(board::kSdramSdScratch));
        }
    }

    r.stage = 7;
    diagStage("step 7: FT81x display");
    // step 7: display.
    r.displayOk = gDisplay.init();
    r.displayId = gDisplay.lastId();
    r.displayAttempt = gDisplay.bootAttempt();
    diagLogf("ft81x: ok %d REG_ID 0x%02X attempt %u", int(r.displayOk), unsigned(r.displayId),
             unsigned(r.displayAttempt));

    r.stage = 9;
    diagStage("step 9: SGTL5000 codec + I2S/DMA");
    // step 9: audio (mute stays LOW until IAudioOut::start()).
    r.codecOk = gAudio.init();
    r.codecId = gAudio.chipId();
    diagLogf("codec: ok %d CHIP_ID 0x%04X, i2c0 errors %lu", int(r.codecOk), unsigned(r.codecId),
             static_cast<unsigned long>(i2c0().errorCount()));

    r.stage = 11;
    diagStage("step 10: radio switch HIGH, Si4703 reset + crystal (after codec, spec 08 §1, spec 22 §2.2)");
    // Radio audio switch in the "radio off" state (PTE26 HIGH, spec 01 §5.4).
    digitalWriteFast(board::kTeensyRadioSwitch, HIGH);
    pinMode(board::kTeensyRadioSwitch, OUTPUT);
    // spec 22 §2.2: the Si4703 is reset once, here, and its crystal started;
    // RST_N then stays HIGH for the whole session (stock never drives it LOW
    // again). The radio source only enables / disables it (radio_si4703.h).
    radio().boot();
    diagStage("step 11: pad bus probe");
    // step 11-13: keypads, pad LEDs, encoder.
    r.padBusAck = padBus().probe(board::kAddrPads);
    diagLogf("pads: TCA8418 ack %d", int(r.padBusAck));
    diagStage("step 11-13: keypads + encoder");
    gInput.init();
    r.buttonsOk = gInput.buttonsOk();
    r.padsOk = gInput.padsOk();
    diagLogf("input: buttons %d pads %d", int(r.buttonsOk), int(r.padsOk));
    diagStage("step 12: pad LEDs IS31FL3731");
    r.ledsOk = gLeds.init();
    diagLogf("leds: ok %d, i2c0 errors %lu i2c2 errors %lu", int(r.ledsOk),
             static_cast<unsigned long>(i2c0().errorCount()), static_cast<unsigned long>(i2c2().errorCount()));

    gIoLast = sdhc::counters();
    diag::Telemetry::setReportHook(&ioTelemetry);

    // MIDI (spec 01 §8).
    diagStage("MIDI init");
    gMidi.init();
    r.stage = 16;
    diagStage("step 14: boardInit done");
    // step 14: status LED (UNVERIFIED [I] that PTC19 is an LED): off = boot done.
    statusLed(false);
    return r;
}

namespace {
// Busy-wait helpers on the DWT cycle counter (enabled in boardInit step 6), so
// they stay correct after the clock is lowered in softOff().
bool switchStableFor(bool pressed, uint32_t ms, uint32_t coreHz) {
    const uint32_t cycles = ms * (coreHz / 1000u);
    const uint32_t t = ARM_DWT_CYCCNT;
    while (ARM_DWT_CYCCNT - t < cycles)
        if (powerSwitchOn() != pressed) return false;
    return true;
}

// K66 RM MCG / SMC: PEE (PLL 180 MHz, HSRUN) -> PBE -> BLPE (core from the
// 16 MHz crystal, PLL off), then HSRUN -> RUN. VLPS may only be entered from
// RUN. Returns false (clocks unchanged or partly changed, still valid) if a
// step does not complete; the caller then polls at the current clock.
bool lowerClocks() {
    MCG_C1 = uint8_t((MCG_C1 & ~0xC0u) | MCG_C1_CLKS(2));        // external reference
    for (uint32_t n = 0; (MCG_S & MCG_S_CLKST_MASK) != MCG_S_CLKST(2); n++)
        if (n > 1000000) return false;
    MCG_C2 = uint8_t(MCG_C2 | MCG_C2_LP);                           // BLPE: PLL/FLL off
    SMC_PMCTRL = SMC_PMCTRL_RUNM(0);                                // HSRUN -> RUN
    for (uint32_t n = 0; SMC_PMSTAT != SMC_PMSTAT_RUN; n++)
        if (n > 1000000) return false;
    return true;
}
}  // namespace

void setPowerOffHook(void (*fn)()) { gPowerOffHook = fn; }

[[noreturn]] void powerOff() {
    // spec 01 §5.4: mute (pin 21 LOW) for the power-off.
    gAudio.stop();
    if (gDisplay.ok()) {
        gDisplay.dlStart();
        gDisplay.clearColorRGB(0, 0, 0);
        gDisplay.clear(true, true, true);
        gDisplay.colorRGB(255, 255, 255);
        gDisplay.text(400, 240, 29, IDisplay::kOptCenter, "Powering off...");
        gDisplay.swap();
    }
    if (gPowerOffHook) gPowerOffHook();
    softOff();
}

namespace {
// spec 01 §10 power flag: EEPROM physical byte 0 = FlexRAM byte 0 (fw_update.cpp
// writes the same byte to 1 before an update). 0 = switched off with the power
// button; anything else boots.
volatile uint8_t* const kPowerFlag = reinterpret_cast<volatile uint8_t*>(0x14000000u);

// Direct FlexRAM write, only valid outside HSRUN (K66: no flash programming in
// HSRUN) and with the EEPROM partition ready; bounded wait for the EEE copy.
void writePowerFlagRun(uint8_t v) {
    if (SMC_PMSTAT == SMC_PMSTAT_HSRUN || !(FTFL_FCNFG & FTFL_FCNFG_EEERDY)) return;
    if (*kPowerFlag == v) return;
    const uint8_t err = FTFL_FSTAT & 0x70;           // clear stale ACCERR/FPVIOL/RDCOLERR (eeprom.c)
    if (err) FTFL_FSTAT = err;
    *kPowerFlag = v;
    for (uint32_t n = 0; !(FTFL_FCNFG & FTFL_FCNFG_EEERDY) && n < 10000000u; n++) {}
    if (*kPowerFlag != v) {                          // one retry; the caller cannot report
        *kPowerFlag = v;
        for (uint32_t n = 0; !(FTFL_FCNFG & FTFL_FCNFG_EEERDY) && n < 10000000u; n++) {}
    }
}

// Low-power wait for a 50 ms press of the power switch (VLPS + PTC3 wake when the
// clocks could be lowered, polling otherwise), then flag := 1 and reset, as stock.
// IRQs must be off; the DWT cycle counter must run.
[[noreturn]] void sleepUntilSwitchThenReset(bool flagUsable) {
    const bool low = lowerClocks();
    // UNVERIFIED: 16 MHz crystal (spec 01 §11) as the BLPE core clock.
    const uint32_t hz = low ? 16000000u : F_CPU;
    if (flagUsable && low) writePowerFlagRun(0);
    for (;;) {
        if (low) {
            // PTC3 logic-0 interrupt (PCR IRQC 1000b) wakes the core from VLPS;
            // PRIMASK stays set, so WFI just returns (no handler runs).
            PORTC_ISFR = 1u << board::kPinPowerSwitch.bit;
            pcr(board::kPinPowerSwitch) = PORT_PCR_MUX(1) | PORT_PCR_IRQC(8) | PORT_PCR_ISF;
            NVIC_CLEAR_PENDING(IRQ_PORTC);
            NVIC_ENABLE_IRQ(IRQ_PORTC);
            SMC_PMCTRL = SMC_PMCTRL_RUNM(0) | SMC_PMCTRL_STOPM(2);   // VLPS
            (void)SMC_PMCTRL;                                         // K66 RM: read back before WFI
            SCB_SCR = SCB_SCR | SCB_SCR_SLEEPDEEP;
            __asm__ volatile("dsb\n\twfi\n\tisb" ::: "memory");
            SCB_SCR = SCB_SCR & ~uint32_t(SCB_SCR_SLEEPDEEP);
            pcr(board::kPinPowerSwitch) = PORT_PCR_MUX(1) | PORT_PCR_ISF;   // IRQ off
            NVIC_CLEAR_PENDING(IRQ_PORTC);
        }
        // Debounce: a 50 ms press restarts (spurious wake-ups sleep again).
        if (switchStableFor(true, 50, hz)) break;
        if (!low) switchStableFor(false, 1, hz);
    }
    // spec 01 §10: flag := 1, then reset (DSB, AIRCR SYSRESETREQ).
    // The wake side must always clear the flag, or every press would reboot into
    // the gate: if the clocks could not be lowered we are still in HSRUN, so drop
    // to RUN first (as the Teensy EEPROM code does) before the FlexRAM write.
    if (flagUsable) {
        if (!low) kinetis_hsrun_disable();
        writePowerFlagRun(1);
    }
    __asm__ volatile("dsb");
    SCB_AIRCR = 0x05FA0004;
    for (;;) {}
}
}  // namespace

void bootPowerGate() {
    // spec 01 §10 boot step 2: flag 0 -> stay off until the power switch. Runs
    // after the ot_restore check, so the SD escape hatch never needs the button.
    if (!eepromPartitionUsable() || *kPowerFlag != 0) return;
    diagLog("power: flag 0 (switched off), waiting for the power switch");
    ARM_DEMCR |= ARM_DEMCR_TRCENA;                   // switchStableFor needs the DWT
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
    delay(20);                                       // let the log line reach USB
    gStorage.unmount();
    __disable_irq();
    USB0_INTEN = 0;
    USB0_CTL = 0;
    gpioWrite(board::kPinRailEnable, false);
    statusLed(false);
    SYST_CSR = 0;
    for (int i = 0; i < 8; i++) {
        (&NVIC_ICER0)[i] = 0xFFFFFFFFu;
        *(reinterpret_cast<volatile uint32_t*>(0xE000E280u) + i) = 0xFFFFFFFFu;   // NVIC_ICPR
    }
    sleepUntilSwitchThenReset(true);
}

[[noreturn]] void softOff() {
    // spec 01 §10 soft-off: peripheral shutdown (codec ANA_POWER = 0, LEDs off,
    // USB off, pin 78 HIGH, pin 80 LOW, SDRAM pins disabled), power flag := 0,
    // then wait for the switch. Deviation: the wait is VLPS with a pin interrupt
    // instead of a VLPR polling loop.
    const bool flagUsable = eepromPartitionUsable();   // uses millis(): before IRQs go off
    gAudio.stop();
    gAudio.powerDown();
    gInputSource = -1;
    radio().powerOff();
    digitalWriteFast(board::kTeensyRadioSwitch, HIGH);
    gLeds.shutdown();
    gDisplay.displayOff();
    gStorage.unmount();
    __disable_irq();
    USB0_INTEN = 0;
    USB0_CTL = 0;
    I2S0_TCSR = 0;
    I2S0_RCSR = 0;
    gpioWrite(board::kPinRailEnable, false);
    statusLed(false);                                // spec 01 §10: pin 78 HIGH
    sdramPinsOff();
    SYST_CSR = 0;                                    // no SysTick wake-ups
    for (int i = 0; i < 8; i++) {                    // nothing but PORTC may wake us
        (&NVIC_ICER0)[i] = 0xFFFFFFFFu;
        *(reinterpret_cast<volatile uint32_t*>(0xE000E280u) + i) = 0xFFFFFFFFu;   // NVIC_ICPR
    }

    // The button is normally still held (1500 ms hold): wait for the release.
    while (!switchStableFor(false, 50, F_CPU)) {}

    sleepUntilSwitchThenReset(flagUsable);
}

bool selectInput(void*, uint8_t source, uint8_t gain) {
    if (source > 3) return false;
    // spec 22 §3: the recorder gain is analog, stock scale (core app/input_gain.h).
    const uint8_t level = input_gain::lineLevel(gain);
    const uint8_t micDb = input_gain::micDb(source, gain);
    // Same source again (the recorder calls this on every gain change and
    // once a second for the radio): only the gain call when the gain changed
    // (stock 0x56640), unless the radio dropped out meanwhile.
    if (int8_t(source) == gInputSource && (source != 1 || radio().powered())) {
        if (gain != gInputGain && gInputOk) {
            gInputGain = gain;
            gInputOk = source < 2 ? gAudio.lineInLevel(level) : gAudio.micGain(micDb);
        }
        return gInputOk;
    }
    gInputSource = int8_t(source);
    gInputGain = gain;
    bool ok = false;
    bool radioOn = false;
    if (source == 1) {
        // spec 01 §5.4/§5.5 radio mode: LINE input, radio on, PTE26 LOW routes
        // the tuner into the codec line input. Deviation: the DAC stays fed from
        // I2S (the stock test feeds it from the ADC) -- OpenTracker monitors the
        // input through the engine (App::inputMonitorMuted).
        radioOn = gReport.codecOk && radio().powerOn();
        ok = radioOn;
        if (ok) {
            digitalWriteFast(board::kTeensyRadioSwitch, LOW);
            ok = gAudio.selectLineIn(level);   // spec 22 §3: gainRadio * 15 / 100
        }
        if (!ok) {
            radio().powerOff();
            digitalWriteFast(board::kTeensyRadioSwitch, HIGH);
            gAudio.selectLineIn(level);
        }
    } else {
        radio().powerOff();
        digitalWriteFast(board::kTeensyRadioSwitch, HIGH);   // spec 01 §5.4: HIGH = radio off
        if (gReport.codecOk) {
            ok = source == 0 ? gAudio.selectLineIn(level) : gAudio.selectMic(micDb);
        }
    }
    gInputOk = ok;
    uint16_t ana = 0, adc = 0;
    const bool rd = gReport.codecOk && gAudio.codecRead(0x0024, ana) && gAudio.codecRead(0x0020, adc);
    diagLogf("input: select %u (%s) -> %s | codec %d radio %s switch PTE26 %s | ANA_CTRL 0x%04X (ADC %s%s) "
             "ANA_ADC_CTRL 0x%04X%s",
             unsigned(source), source == 0 ? "line" : source == 1 ? "radio" : source == 2 ? "mic LG" : "mic HG",
             ok ? "ok" : "FAILED", int(gReport.codecOk),
             source != 1 ? "off" : radioOn ? "powering" : "NO ANSWER",
             digitalReadFast(board::kTeensyRadioSwitch) ? "HIGH (radio off)" : "LOW (radio in)",
             unsigned(ana), (ana & 0x0004) ? "LINEIN" : "MIC", (ana & 0x0001) ? " MUTED" : "", unsigned(adc),
             rd ? "" : " (readback failed)");
    return ok;
}

void inputDiagEnable(bool on) { Sgtl5000Audio::setRxPeak(on); }

void inputDiagLog() {
    // One line per call: is there signal on the I2S RX (raw, before the app
    // gain), how is the codec ADC routed, and what is the tuner doing.
    int pl = 0, pr = 0;
    uint32_t blocks = 0;
    Sgtl5000Audio::takeRxPeak(pl, pr, blocks);
    uint16_t ana = 0, sss = 0, pwr = 0, adcdac = 0, adc = 0;
    bool rd = gReport.codecOk;
    rd = rd && gAudio.codecRead(0x0024, ana);
    rd = rd && gAudio.codecRead(0x000A, sss);
    rd = rd && gAudio.codecRead(0x0030, pwr);
    rd = rd && gAudio.codecRead(0x000E, adcdac);
    rd = rd && gAudio.codecRead(0x0020, adc);
    const Si4703Radio& r = radio();
    const uint16_t st = r.reg(0x0A), pcfg = r.reg(0x02), s2 = r.reg(0x05);
    diagLogf("input: src %d ok %d | rx peak L %d R %d (%lu blocks) RCSR 0x%08lX (FEF %d) restarts %lu stale %lu | codec%s ANA_CTRL 0x%04X SSS_CTRL 0x%04X "
             "ANA_POWER 0x%04X ADCDAC_CTRL 0x%04X ANA_ADC_CTRL 0x%04X | switch PTE26 %d | radio state %u "
             "pwr %d DEVID 0x%04X POWERCFG 0x%04X (DMUTE %d DSMUTE %d) VOL %u READCHAN %u %.1f MHz "
             "RSSI %u STC %u ST %u busy %d err %lu tunes ok %lu failed %lu",
             int(gInputSource), int(gInputOk), pl, pr, static_cast<unsigned long>(blocks),
             static_cast<unsigned long>(Sgtl5000Audio::rxStatus()), int((Sgtl5000Audio::rxStatus() >> 18) & 1),
             static_cast<unsigned long>(Sgtl5000Audio::rxRecoveries()),
             static_cast<unsigned long>(Sgtl5000Audio::rxStaleBlocks()), rd ? "" : " (READ FAILED)",
             unsigned(ana), unsigned(sss), unsigned(pwr), unsigned(adcdac), unsigned(adc),
             int(digitalReadFast(board::kTeensyRadioSwitch)), unsigned(r.stateCode()), int(r.powered()),
             unsigned(r.deviceId()), unsigned(pcfg), int((pcfg & 0x4000) != 0), int((pcfg & 0x8000) != 0),
             unsigned(s2 & 0x0F), unsigned(r.reg(0x0B) & 0x3FF), double(r.frequency()), unsigned(st & 0xFF),
             unsigned((st & 0x4000) != 0), unsigned((st & 0x0100) != 0), int(r.busy()),
             static_cast<unsigned long>(r.errorCount()), static_cast<unsigned long>(r.tunesDone()),
             static_cast<unsigned long>(r.tuneFailures()));
}

const int16_t* lineIn(void* ctx) { return Sgtl5000Audio::lineInBlock(ctx); }

void service() {
    gMidi.service();
    gInput.service();
    gLeds.service();
    gStorage.service();
    Sgtl5000Audio::serviceRx();
    radio().service();
    // spec 01 §10 / spec 05 power button: edges become btn::Power presses and
    // releases (Shift + Power = screenshot in the UI). Held for 1500 ms without
    // Shift held at the press -> power off. A press still held from before boot
    // (the wake-up press) is ignored until the first release.
    const uint32_t now = millis();
    const bool raw = powerSwitchOn();
    if (raw != gPowerRaw) {
        gPowerRaw = raw;
        gPowerEdgeMs = now;
    } else if (raw != gPowerDown && now - gPowerEdgeMs >= board::kPowerDebounceMs) {
        gPowerDown = raw;
        if (!raw) {
            if (gPowerArmed) gInput.injectButton(false, kBtnPower);
            gPowerArmed = true;
        } else if (gPowerArmed) {
            gPowerPressMs = now;
            gPowerShiftAtPress = gInput.buttonDown(kBtnShift);
            gInput.injectButton(true, kBtnPower);
        }
    }
    if (gPowerArmed && gPowerDown && !gPowerShiftAtPress && now - gPowerPressMs >= board::kPowerHoldOffMs)
        powerOff();
}

}  // namespace hal
}  // namespace ot
