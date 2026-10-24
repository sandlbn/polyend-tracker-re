#include <cstdio>
#include <cstring>
// OpenTracker device glue: hands the HAL drivers and the SDRAM buffers to the
// App (core stays free of hal headers; ARCHITECTURE.md rules).
#include "app/app.h"
#include "hal/audio_sgtl5000.h"
#include "hal/board.h"
#include "hal/diag.h"
#include "hal/ft81x.h"
#include "hal/fw_update.h"
#include "hal/hal.h"
#include "hal/i2c_bus.h"
#include "hal/input.h"
#include "hal/nvstore_eeprom.h"
#include "hal/pad_leds.h"
#include "hal/radio_si4703.h"
#include "hal/telemetry.h"
#include "hal/sd_storage.h"
#include "hal/usb_storage.h"

extern "C" uint8_t _ssdram_bss[], _esdram_bss[];   // src/hal/opentracker.ld

namespace {

// SDRAM layout (spec 01 §2: 16 MB at 0x70000000). Sample pool first
// (spec 02 §8.2: 0xB40000 bytes), then the engine's effect memory -- three
// int16 delay lines of 262,144 samples (spec 04 §5.1) and two int16 reverb
// rings of 16,384 samples (spec 04 §5.2) -- then the in-memory pattern set,
// undo rings, clipboard and scratch.
// 2.x (docs/M1_PLAN.md §1): the resident store keeps 12-track PatternStock
// records; tracks 13..16 live in the ext-track pool. Every working pattern
// (sequencer current/preload, undo, file undo, clipboard, performance
// scratch) is a 16-track Pattern, and the two sequencer buffers moved here
// from App (SRAM).
constexpr uint32_t kPatSize = sizeof(ot::Pattern);          // 16 tracks, 12,304 B
constexpr uint32_t kStockSize = sizeof(ot::PatternStock);   // 12 tracks, 9,228 B
constexpr uint32_t kUndoSlots = 20;      // spec 05 §5.1 (a)
constexpr uint32_t kFileUndoSlots = 10;  // spec 05 §5.1 (b)
constexpr uint32_t kScratch = 0x10000;
// Ext-track pool: 384 blocks (spec gap 9) + the pool-full write target.
constexpr int kExtBlocks = ot::ExtTrackPool::kDefaultBlocks + 1;
constexpr uint32_t kFxSamples = uint32_t(ot::audio::AudioEngine::kFxMemorySamples);
constexpr uint32_t kFxBytes = kFxSamples * sizeof(int16_t);
static_assert(kFxBytes == 3u * 262144u * 2u + 2u * 16384u * 2u, "delay 3x262144 + reverb 2x16384 int16");
static_assert(ot::kSampleBankBytes == 11796480u, "sample pool size, spec 02 §8.2");
static_assert(kPatSize == 769u * 16u && kStockSize == 769u * 12u, "packed 769-byte track records");
constexpr uint32_t align(uint32_t v) { return (v + 15u) & ~15u; }
constexpr uint32_t kOffBank = 0;
constexpr uint32_t kOffFx = align(kOffBank + ot::kSampleBankBytes);
constexpr uint32_t kOffPatterns = align(kOffFx + kFxBytes);
constexpr uint32_t kOffExt = align(kOffPatterns + kStockSize * ot::kPatternCount);
constexpr uint32_t kOffExtMeta = align(kOffExt + ot::ExtTrackPool::blockBytes(kExtBlocks));
constexpr uint32_t kOffSeqBufs = align(kOffExtMeta + ot::ExtTrackPool::metaBytes(kExtBlocks - 1));
constexpr uint32_t kOffUndo = align(kOffSeqBufs + kPatSize * 3u);   // current, preload, switch stage
constexpr uint32_t kOffFileUndo = align(kOffUndo + kPatSize * kUndoSlots);
constexpr uint32_t kOffClip = align(kOffFileUndo + kPatSize * kFileUndoSlots);
constexpr uint32_t kOffNums = align(kOffClip + kPatSize);
constexpr uint32_t kOffScratch = align(kOffNums + 64);
// Per-track pattern staging for the Performance screen (spec 12 §C.1).
constexpr uint32_t kOffPerf = align(kOffScratch + kScratch);
constexpr uint32_t kOffStems = align(kOffPerf + sizeof(ot::PerfPatternMemory));
constexpr uint32_t kEnd = kOffStems + sizeof(ot::audio::StemBlock);   // WAV export stems (was SRAM)
static_assert(ot::board::kSdramSize == 16u * 1024u * 1024u, "spec 01 §2");
static_assert(kEnd <= ot::board::kSdramSize, "SDRAM layout exceeds 16 MB");
// M1 budget (docs/M1_PLAN.md §1): at least 150 KB stay free for M2/M3.
static_assert(ot::board::kSdramSize - kEnd >= 150u * 1024u, "SDRAM: < 150 KB left after the M1 layout");
// The linker's `.sdram_bss` (core/sdram_bss.h) sits above the layout.
static_assert(kEnd <= ot::board::kSdramBssBase - ot::board::kSdramBase, "SDRAM layout overlaps .sdram_bss");
static_assert(sizeof(ot::PerfPatternMemory) == 769u * 16u * 2u + kPatSize, "perf slots [16][2] + scratch");

constexpr uint32_t kSdramFree = ot::board::kSdramBssBase - ot::board::kSdramBase - kEnd;

ot::App gApp;
ot::diag::Telemetry gTelemetry;
void (*gHalService)() = nullptr;
uint8_t gAppliedBrightness = 0xFF;   // config level last sent to the display

// Display brightness (spec 05 §1.8): the stock display manager compares the
// requested duty with the applied one every update and calls
// HAL.setBacklight() on a change. Doing the same here covers boot (config
// loaded from the card), a late card insert, the Config screen and Reset
// config without touching the core. The FT81x driver holds the level until
// its first draw (spec 01 §4.2).
void applyBrightness() {
    const uint8_t level = gApp.cfg.brightness > 2 ? 2 : gApp.cfg.brightness;
    if (level == gAppliedBrightness) return;
    gAppliedBrightness = level;
    ot::hal::halSet().display->setBacklight(ot::IDisplay::brightnessDuty(level));
}

// Periodic telemetry line in the diag log (docs/PLAN_PHASE2.md T1c): audio
// render cycles per block (DWT, measured in the I2S TX ISR), the longest
// main-loop iteration, SDRAM left over by the layout above and the I2C error
// counts. Every 5 s; sent only while a USB terminal is open (DTR), otherwise
// the window is just restarted so the next report is fresh.
// Render-cycle windows: the driver window is polled once a second and fed to
// both the 5 s telemetry window and the Config /.develop overlay (spec 06
// §7.2: CPU min/max/avg/last, Play resets it).
struct CycleWindow {
    uint32_t minC = 0xFFFFFFFFu, maxC = 0, blocks = 0;
    uint64_t sum = 0;
    void add(uint32_t mn, uint32_t avg, uint32_t mx, uint32_t n) {
        if (!n) return;
        if (mn < minC) minC = mn;
        if (mx > maxC) maxC = mx;
        sum += uint64_t(avg) * n;
        blocks += n;
    }
    uint32_t avg() const { return blocks ? uint32_t(sum / blocks) : 0; }
    uint32_t min() const { return blocks ? minC : 0; }
    void reset() { *this = CycleWindow{}; }
};
CycleWindow gTelWindow, gDevWindow;
uint32_t gDevLastCycles = 0;
uint32_t gLastCyclePoll = 0;

void pollRenderCycles(uint32_t nowMs) {
    if (nowMs - gLastCyclePoll < 1000) return;
    gLastCyclePoll = nowMs;
    uint32_t mn = 0, avg = 0, mx = 0;
    const uint32_t n = ot::hal::audio().renderCyclesWindow(mn, avg, mx);
    if (!n) return;
    gTelWindow.add(mn, avg, mx, n);
    gDevWindow.add(mn, avg, mx, n);
    gDevLastCycles = avg;
}

uint32_t permille(uint32_t cycles) {
    return uint32_t(uint64_t(cycles) * 1000u / ot::diag::kRenderBudgetCycles);
}

// First frame per screen the Ui RAM_DL guard had to cut (docs/VERIFY_1.9.5.md
// §6): logged once per screen, the frame itself never reaches the FT81x.
void dlOverflow(const char* screen, uint32_t words, uint32_t dropped, bool lowDetail) {
    ot::hal::diagLogf("ui: display list over budget on %s: cut at %lu words, %lu primitives dropped%s", screen,
                      static_cast<unsigned long>(words), static_cast<unsigned long>(dropped),
                      lowDetail ? " (low detail)" : "; low detail from the next frame");
}

// Headphone-amp level (spec 01 §5.1 volume(v)); the stock boot value is not
// in the specs, so the Config /.develop "HP level" row can change it for
// measurement (USER_REPORTS_R2 §2). Logged with the HP_CTRL step.
void logHeadphoneLevel(float v) {
    const unsigned n = unsigned(v * 129.0f + 0.5f), reg = n >= 128 ? 0u : 0x80u - n;
    const int halfDb = 0x18 - int(reg);   // 0x18 = 0 dB, 0.5 dB per step
    ot::hal::diagLogf("audio: HP level %u/100 (HP_CTRL 0x%02X, %s%d.%d dB)", unsigned(v * 100.0f + 0.5f), reg,
                      halfDb < 0 ? "-" : "+", (halfDb < 0 ? -halfDb : halfDb) / 2, (halfDb & 1) ? 5 : 0);
}
void setHeadphoneLevel(float v) {
    ot::hal::audio().setHeadphoneVolume(v);
    logHeadphoneLevel(v);
}

bool devStats(ot::DevStats& d) {
    d.cpuMin = permille(gDevWindow.min());
    d.cpuMax = permille(gDevWindow.maxC);
    d.cpuAvg = permille(gDevWindow.avg());
    d.cpuLast = permille(gDevLastCycles);
    d.memFree = kSdramFree;
    d.loopMaxUs = gTelemetry.loopMaxUs();
    return true;
}

void devStatsReset() { gDevWindow.reset(); }

// Factory test first page: the power-on checks of the hardware selftest
// (hal::bootReport(), live keypad / LED state).
int bootChecks(ot::FactoryCheck* out, int cap) {
    const ot::hal::BootReport& r = ot::hal::bootReport();
    const ot::FactoryCheck all[] = {
        {"clocks 180/90", r.clocksOk},
        {"SDRAM", r.sdramOk},
        {"display FT81x", r.displayOk},
        {"codec SGTL5000", r.codecOk},
        {"buttons TCA8418", ot::hal::input().buttonsOk()},
        {"pads TCA8418", ot::hal::input().padsOk()},
        {"pad LEDs", ot::hal::leds().ok()},
        {"SD card", r.sdMounted},
    };
    int n = 0;
    for (const ot::FactoryCheck& c : all)
        if (n < cap) out[n++] = c;
    return n;
}

// Recorder input diagnostics (docs/HARDWARE_TESTING.md "Recorder input"):
// once a second while the Sample Recorder is open (capture metering on), one
// HAL "input:" line (raw I2S RX peak, codec ADC routing, radio switch, Si4703
// status) and one app "recorder:" line (source, gain, monitor, capture state
// and the peak of the last recorded 0.1 s), so a device log shows where a
// signal stops.
void recorderInputDiag(uint32_t nowMs) {
    static bool on = false;
    static uint32_t last = 0;
    const bool open = gApp.capture.metering();
    if (open != on) {
        on = open;
        ot::hal::inputDiagEnable(open);
        last = nowMs;
        if (!open) return;
    }
    if (!open || nowMs - last < 1000) return;
    last = nowMs;
    ot::hal::inputDiagLog();
    const ot::Capture& c = gApp.capture;
    const uint32_t len = c.length();
    int peak = 0;
    if (c.buffer()) {
        const uint32_t n = len < 4410 ? len : 4410;
        for (uint32_t i = len - n; i < len; ++i) {
            const int v = c.buffer()[i] < 0 ? -int(c.buffer()[i]) : int(c.buffer()[i]);
            if (v > peak) peak = v;
        }
    }
    const uint8_t src = gApp.cfg.recSource > 3 ? 0 : gApp.cfg.recSource;
    ot::hal::diagLogf("recorder: source %u gain %u monitor %u (muted %d) line ch %u | capture state %u len %lu "
                      "full %d, last 0.1 s peak %d | monitor mix peak %d",
                      unsigned(src), unsigned(gApp.cfg.recGain[src]), unsigned(gApp.cfg.recMonitor),
                      int(gApp.inputMonitorMuted), unsigned(gApp.cfg.lineInChannel), unsigned(c.state()),
                      static_cast<unsigned long>(len), int(c.full()), peak, gApp.engine.takeLineMonitorPeak());
}

// [I] One "resample:" line per finished or cancelled take (app/resample.h),
// with the I2S RX restarts / stale input blocks counted while it recorded.
void resampleDiag() {
    static uint32_t seen = 0, rx0 = 0, stale0 = 0;
    static bool recording = false;
    const ot::Resampler::State st = gApp.resampler.state();
    if (!recording && (st == ot::Resampler::kRecording || st == ot::Resampler::kStopping)) {
        recording = true;
        rx0 = ot::hal::Sgtl5000Audio::rxRecoveries();
        stale0 = ot::hal::Sgtl5000Audio::rxStaleBlocks();
    }
    const ot::ResampleReport& r = gApp.lastResample;
    if (r.seq == seen) return;
    seen = r.seq;
    const uint32_t rx = recording ? ot::hal::Sgtl5000Audio::rxRecoveries() - rx0 : 0;
    const uint32_t stale = recording ? ot::hal::Sgtl5000Audio::rxStaleBlocks() - stale0 : 0;
    recording = false;
    char line[200];
    ot::formatResampleReport(line, sizeof line, r, rx, stale);
    ot::hal::diagLog(line);
}

void telemetryTick() {
    const ot::IClock& clock = *ot::hal::halSet().clock;
    gTelemetry.loopMark(clock.micros());
    recorderInputDiag(clock.millis());
    resampleDiag();
    pollRenderCycles(clock.millis());
    {   // 2.x "cpu" line, 1 Hz (spec 2x-02 §4, hal/telemetry.h)
        static ot::diag::CpuTelemetry cpuTel;
        char cl[320];
        if (cpuTel.tick(clock.millis(), gApp.engine.cpu(), gApp.engine.governor(), cl, sizeof cl,
                        &gApp.engine.profile()) &&
            ot::hal::diagTerminalOpen())
            ot::hal::diagLog(cl);
    }
    if (!gTelemetry.due(clock.millis())) return;
    ot::diag::TelemetrySample s;
    s.cycMin = gTelWindow.min();
    s.cycAvg = gTelWindow.avg();
    s.cycMax = gTelWindow.maxC;
    s.blocks = gTelWindow.blocks;
    gTelWindow.reset();
    s.loopMaxUs = gTelemetry.loopMaxUs();
    s.loops = gTelemetry.loops();
    gTelemetry.resetWindow();
    if (!ot::hal::diagTerminalOpen()) return;
    s.sdramFree = kSdramFree;
    s.i2c0Errors = ot::hal::i2c0().errorCount();
    s.i2c2Errors = ot::hal::i2c2().errorCount();
    s.i2c0Naks = ot::hal::i2c0().nakCount();
    s.i2c2Naks = ot::hal::i2c2().nakCount();
    char line[448];
    ot::diag::formatTelemetry(line, sizeof line, s);
    ot::hal::diagLog(line);
    // UI line (docs/HARDWARE_TESTING.md "telemetry: ui"): frames and render
    // time as before, then the last frame's SPI upload (bytes, time), the
    // frames deferred by a busy display, the longest frame-to-frame gap, the
    // dirty -> frame and input -> frame latencies, the FIFO-space waits and
    // the dlStart() polls of the last frame, and the coprocessor resets; 2.x:
    // the last frame's command-FIFO words (DL words include appended
    // fragments), cumulative fragment appends / captures and the SPI2 DMA.
    const ot::Ui::RenderStats r = gApp.ui.takeRenderStats();
    ot::hal::Ft81xDisplay& d = ot::hal::display();
    const uint32_t hwDlMax = d.takeMaxDlBytes();
    std::snprintf(line, sizeof line,
                  "telemetry: ui %lu frames (%lu.%lu fps) render avg %lu.%02lu max %lu.%02lu ms, last frame %lu DL words"
                  " %lu B spi %lu.%02lu ms | deferred %lu gap max %lu ms lat dirty %lu.%lu ms input %lu ms"
                  " | fifo waits %lu dlstart polls %lu copro faults %lu (after frame %lu)"
                  " | dl max %lu words, over %lu, low detail %lu, hw %lu B max %lu B"
                  " | upload %lu words frag hits %lu captures %lu dma %s",
                  static_cast<unsigned long>(r.frames), static_cast<unsigned long>(r.frames / 5),
                  static_cast<unsigned long>((r.frames * 2) % 10),
                  static_cast<unsigned long>(r.frames ? r.usSum / r.frames / 1000 : 0),
                  static_cast<unsigned long>(r.frames ? (r.usSum / r.frames % 1000) / 10 : 0),
                  static_cast<unsigned long>(r.usMax / 1000), static_cast<unsigned long>((r.usMax % 1000) / 10),
                  static_cast<unsigned long>(r.words), static_cast<unsigned long>(d.lastFrameBytes()),
                  static_cast<unsigned long>(d.lastUploadUs() / 1000),
                  static_cast<unsigned long>((d.lastUploadUs() % 1000) / 10),
                  static_cast<unsigned long>(r.deferred), static_cast<unsigned long>(r.gapMaxMs),
                  static_cast<unsigned long>(r.dirtyLatMaxUs / 1000),
                  static_cast<unsigned long>((r.dirtyLatMaxUs % 1000) / 100),
                  static_cast<unsigned long>(r.inputLatMaxMs), static_cast<unsigned long>(d.lastFifoWaits()),
                  static_cast<unsigned long>(d.lastWaitPolls()), static_cast<unsigned long>(d.faults()),
                  static_cast<unsigned long>(d.frameFaults()), static_cast<unsigned long>(r.wordsMax),
                  static_cast<unsigned long>(r.overBudget), static_cast<unsigned long>(r.lowDetail),
                  static_cast<unsigned long>(d.lastDlBytes()), static_cast<unsigned long>(hwDlMax),
                  static_cast<unsigned long>(gApp.ui.lastUploadWords()),
                  static_cast<unsigned long>(gApp.ui.fragments.hits),
                  static_cast<unsigned long>(gApp.ui.fragments.captures),
                  ot::hal::Ft81xDisplay::dmaActive() ? "on" : "off");
    ot::hal::diagLog(line);
}

// Called once per App::loop() iteration (HalRefs::service).
void appService() {
    if (gHalService) gHalService();
    applyBrightness();
    telemetryTick();
}

// Recorder routing hook: the tuner's de-emphasis follows the config "Radio
// region" (spec 05 config table EU/US/AU/JP), read here so the core interface
// stays unchanged; setBand() (called by the recorder right after) applies it.
bool selectInputThunk(void* ctx, uint8_t source, uint8_t gain) {
    ot::hal::radio().setRegion(gApp.cfg.radioRegion);
    return ot::hal::selectInput(ctx, source, gain);
}

// Power-off (hal::powerOff, audio already stopped): flush the workspace and
// settings to the card like the periodic autosave. lastAutosave() is 0 until
// App::boot() has run (no card / nothing loaded -> nothing to save).
void powerOffHook() {
    if (gApp.lastAutosave() == 0) return;
    if (gApp.usbStorageActive()) return;   // the computer owns the card: nothing of ours to write
    gApp.autosave();
    gApp.saveConfig();
}

}  // namespace

void otAppMain(ot::hal::HalSet& hal) {
    using namespace ot;
    // Nothing may touch 0x70000000 unless the controller init + quick test passed
    // (spec 01 §2); main.cpp already checks, this guards other callers.
    if (!hal::bootReport().sdramOk) return;
    // The codec init leaves the headphone amp muted at minimum (spec 01 §5.1 step 15/16)
    // and IAudioOut has no volume control, so open it here. Field: no sound on
    // headphones in the app while the selftest (which sets 0.5) was audible.
    hal::audio().setHeadphoneVolume(kHeadphoneBootLevel);
    // `.sdram_bss` (core/sdram_bss.h): NOLOAD, zeroed here, before any App code.
    std::memset(_ssdram_bss, 0, size_t(_esdram_bss - _ssdram_bss));
    hal::storage().enableSdramFiles();   // FatFs file slots in `.sdram_bss`
    uint8_t* sd = reinterpret_cast<uint8_t*>(board::kSdramBase);
    AppMemory mem;
    mem.sampleBank = reinterpret_cast<int16_t*>(sd + kOffBank);
    mem.sampleBankSamples = kSampleBankBytes / 2;
    mem.fxMemory = reinterpret_cast<int16_t*>(sd + kOffFx);
    mem.fxSamples = kFxSamples;
    mem.patterns = reinterpret_cast<PatternStock*>(sd + kOffPatterns);
    mem.extTracks = reinterpret_cast<Track*>(sd + kOffExt);
    mem.extTrackBlocks = kExtBlocks;
    mem.extMeta = sd + kOffExtMeta;
    mem.seqBuffers = reinterpret_cast<Pattern*>(sd + kOffSeqBufs);
    mem.seqStage = mem.seqBuffers + 2;
    mem.undo = reinterpret_cast<Pattern*>(sd + kOffUndo);
    mem.undoSlots = int(kUndoSlots);
    mem.fileUndo = reinterpret_cast<Pattern*>(sd + kOffFileUndo);
    mem.fileUndoSlots = int(kFileUndoSlots);
    mem.clipboard = reinterpret_cast<Pattern*>(sd + kOffClip);
    mem.undoNums = sd + kOffNums;
    mem.fileUndoNums = sd + kOffNums + 32;
    mem.scratch = sd + kOffScratch;
    mem.scratchBytes = kScratch;
    mem.perfPatterns = reinterpret_cast<PerfPatternMemory*>(sd + kOffPerf);
    mem.exportStems = reinterpret_cast<audio::StemBlock*>(sd + kOffStems);

    HalRefs refs;
    refs.display = hal.display;
    refs.audio = hal.audio;
    refs.storage = hal.storage;
    refs.midi = hal.midi;
    refs.input = hal.input;
    refs.leds = hal.leds;
    refs.clock = hal.clock;
    gHalService = hal.service;
    refs.service = &appService;   // HAL service + brightness + telemetry
    refs.isUpdateImageName = &hal::isUpdateImageName;
    refs.requestFirmwareUpdate = &hal::requestFirmwareUpdate;
    refs.lineIn = &hal::lineIn;            // I2S RX (spec 01 §5.3)
    refs.lineInCtx = nullptr;
    refs.selectInput = &selectInputThunk;  // codec input mux + tuner (spec 01 §5.4-§5.6)
    refs.inputCtx = nullptr;
    refs.radio = &hal::radio();
    refs.nvstore = &hal::nvStore();        // config image in the stock EEPROM area (spec 05 §5.4)
    refs.usbStorage = hal::usbStorage();   // 2.x USB Storage mode (null in an OT_USB_MSC=0 image)
    if (!gApp.init(refs, mem)) return;   // falls back to the selftest
    hal::setPowerOffHook(&powerOffHook);
    gApp.ui.platform.devStats = &devStats;         // Config /.develop overlay (spec 06 §7.2)
    gApp.ui.platform.devStatsReset = &devStatsReset;
    gApp.ui.platform.setHeadphoneLevel = &setHeadphoneLevel;   // Config /.develop HP level
    logHeadphoneLevel(kHeadphoneBootLevel);   // the boot value set above, for the diag log
    gApp.ui.platform.bootChecks = &bootChecks;     // factory test (spec 05 §2.12)
    gApp.ui.platform.dlOverflow = &dlOverflow;     // RAM_DL guard (Ui::render)
    if (hal::bootReport().sdMounted) gApp.boot();
    gApp.run();
}
