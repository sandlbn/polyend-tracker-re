// OpenTracker -- periodic app telemetry line for the diag log (PLAN_PHASE2 T1c,
// docs/HARDWARE_TESTING.md "Diagnostic log"). Platform independent (no
// registers, no printf) so it is host-tested (tests/telemetry_test.cpp); the
// device glue (src/app_main.cpp) feeds it from the main loop and sends the
// line through diagLog() every 5 s while a USB terminal is open.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio/block_profile.h"
#include "audio/stream_source.h"

namespace ot {
struct JobProfile;   // core app/project_io.h
namespace audio {
class CpuMeter;      // core audio/cpu_meter.h
class Governor;      // core audio/governor.h
}

namespace diag {

// Audio render budget per 128-frame block: 128 / 44117.6 Hz * 180 MHz.
constexpr uint32_t kRenderBudgetCycles = 522240;
// Hardware rounds alarm at 70 % of the budget (docs/PLAN_PHASE2.md "Rules").
constexpr uint32_t kRenderAlarmPermille = 700;

struct TelemetrySample {
    uint32_t cycMin = 0, cycAvg = 0, cycMax = 0;   // DWT cycles per render block
    uint32_t blocks = 0;                           // render blocks in the window
    uint32_t loopMaxUs = 0;                        // longest main-loop iteration
    uint32_t loops = 0;                            // iterations in the window
    uint32_t sdramFree = 0;                        // bytes not used by the SDRAM layout
    uint32_t i2c0Errors = 0, i2c2Errors = 0;       // cumulative bus faults (timeouts, arbitration)
    uint32_t i2c0Naks = 0, i2c2Naks = 0;           // cumulative NAKs (shown when non-zero)
    uint32_t stackSize = 0;                        // stack room above the guard (0: not measured, field omitted)
    uint32_t stackPeak = 0;                        // deepest stack use since boot (stack_watch.h)
    uint32_t heap = 0;                             // newlib heap in its reserve (__brkval - _ebss)
    uint32_t rxRestarts = 0, rxStale = 0;          // cumulative I2S RX restarts / input blocks lost
    uint32_t dinClocks = 0;                        // MIDI clocks sent on DIN in the window
    uint32_t dinClockLateMaxUs = 0;                // longest clock queued -> UART (shown with clocks)
};

// Stack high-water mark: the device paints the free stack at boot
// (hal/stack_watch.h); the words from the bottom up that still hold the paint
// were never used. Returns that count, scanning [lo, hi).
constexpr uint32_t kStackPaint = 0xC5A5C5A5u;
size_t untouchedStackWords(const uint32_t* lo, const uint32_t* hi);

// SD + cache state over one report window (2.x performance workstream).
struct IoSample {
    uint32_t windowMs = 0;                         // window length (0: unknown -> no rate)
    uint32_t sdCmds = 0;                           // SD commands issued in the window
    uint32_t sdReadBytes = 0, sdWriteBytes = 0;
    uint32_t sdDirect = 0, sdBounced = 0;          // ADMA2 chunks direct / through the bounce buffer
    bool cacheOn = false;                          // LMEM system cache on for the SDRAM
    const char* cacheTest = "";                    // boot coherency self-test result
    bool sdDirectRead = false, sdDirectWrite = false;
};

// "telemetry: io sd cmds N read R B (K KB/s) write W B (K KB/s) direct D bounced B |
//  cache sdram on|off (selftest X) | sd->sdram dma read direct|bounced write direct|bounced"
size_t formatIo(char* out, size_t cap, const IoSample& s);
// Bytes per second over windowMs (0 if the window is 0).
uint32_t bytesPerSecond(uint32_t bytes, uint32_t windowMs);

// "telemetry: load op O run R passes P units U total T us max pass M us (stage S/F)
//  max unit X us (stage S) | open A us nO read B us nR K KB write C us nW K KB dir D us nD
//  decode E us commit F us" -- the last (or running) background project job.
size_t formatLoad(char* out, size_t cap, const JobProfile& p);

// 2.x CPU meter / governor state (spec 2x-02 §4), permille of the block budget.
struct CpuSample {
    uint32_t avg = 0, peak = 0;        // A, P (permille)
    uint32_t overruns = 0;             // since boot
    int cap = 0, budget = 0;           // governor C / B
    int synthVoices = 0;               // sounding synth voices
    uint32_t cycPerVoice = 0;          // costliest synth voice render (cycles) in the last 1 s window
    bool hasPeak = false;              // pk holds the last window's peak block
    audio::BlockProfile pk;            // peak block attribution (audio/block_profile.h)
};
// prof: the engine's peak-block window (nullptr: no attribution).
CpuSample cpuSample(const audio::CpuMeter& m, const audio::Governor& g, const audio::ProfileWindow* prof = nullptr);
// "telemetry: cpu avg=A.A% peak=P.P% overruns=N cap=C/B synth_voices=K cyc_per_voice=X"
// and, with a peak block (cycles in k = 1000, rounded):
// " | pk T: seq S on A/B ctl C smp D syn F <ENG1> G <ENG2> H <ENG3> I dly J rev K out L
//  | ev on N <ENG1> a <ENG2> b <ENG3> c rl R fc F mc M st S pd P df D vx V/W"
// (engine slots without an engine print "-"; ev counts the block's sample
// note-ons, synth note-ons per slot (PERC: drum hits), reloadParams,
// full parameter copies, macro recomputes, steals, pending starts, deferred
// note-ons, synth and sample voices rendered).
size_t formatCpu(char* out, size_t cap, const CpuSample& s);

// 2.x SD streaming (docs/PLAN_SD_STREAMING_P1C_P2.md C11): one telemetry
// window of the stream service, logged with the 5 s lines while streaming is on.
struct StreamSample {
    bool loading = false;                  // a project job ran (reads compete with it)
    uint32_t streams = 0, active = 0;      // bound sources, rings playing a stream
    uint32_t chunks = 0, errors = 0, passes = 0;
    uint32_t readAvgUs = 0, readMaxUs = 0;
    uint32_t leadMinMs = 0xFFFFFFFFu;      // smallest lead, 0xFFFFFFFF = nothing left to read
    uint32_t underruns = 0;                // the rings' running total
    uint32_t underrunsDelta = 0;           // new in this window
};
// From one window of the service stats; lastUnderruns carries the previous
// total (updated) for the delta.
StreamSample streamSample(const audio::StreamServiceStats& st, uint32_t streams, uint32_t active,
                          uint32_t& lastUnderruns, bool loading);
// "telemetry: stream streams S active A | chunks C read avg X.Y max X.Y ms | lead min L ms
//  | underruns U (+D) errors E | passes P" (+ " | loading"; lead "-" when none)
size_t formatStream(char* out, size_t cap, const StreamSample& s);

// Once-per-second "cpu" line (kPeriodMs): due() starts the period on its
// first call like Telemetry::due().
class CpuTelemetry {
public:
    static constexpr uint32_t kPeriodMs = 1000;
    bool due(uint32_t nowMs);
    // Formats the line into out when due (returns its length), else 0.
    size_t tick(uint32_t nowMs, const audio::CpuMeter& m, const audio::Governor& g, char* out, size_t cap,
                const audio::ProfileWindow* prof = nullptr);

private:
    uint32_t last_ = 0;
    bool started_ = false;
};

// Main-loop timing over a report window.
class Telemetry {
public:
    static constexpr uint32_t kPeriodMs = 5000;
    // Called by due() each time it returns true, before the caller writes its
    // lines, so a HAL-side line (the device registers the SD/cache "io" line,
    // board.cpp) covers exactly the same window as the app's render line.
    using ReportHook = void (*)(uint32_t windowMs);
    static void setReportHook(ReportHook fn);

    // Once per main-loop iteration (microsecond clock, wraps).
    void loopMark(uint32_t nowUs);
    // True once per kPeriodMs (the first call only starts the window).
    bool due(uint32_t nowMs);
    uint32_t loopMaxUs() const { return loopMaxUs_; }
    uint32_t loops() const { return loops_; }
    // Starts a new loop-timing window (after a report, or a skipped one).
    void resetWindow();

private:
    uint32_t lastUs_ = 0;
    bool haveLast_ = false;
    uint32_t loopMaxUs_ = 0;
    uint32_t loops_ = 0;
    uint32_t lastReportMs_ = 0;
    bool started_ = false;
    static ReportHook hook_;
};

// "telemetry: render cyc/block min A avg B max C (P.P% of 522240) blocks N |
//  loop max M.MMM ms (n L) | SDRAM free F B | i2c errors i2c0 X i2c2 Y[ nak i2c0 A i2c2 B]
//  [| stack peak P B heap H B of S B (free F B)] | rx restarts R stale T
//  [ | din clock late max M.MMM ms (n C)]" (the last part while MIDI clock goes out on DIN)
// with " RENDER>70%" appended when the max crosses the alarm. Returns length.
size_t formatTelemetry(char* out, size_t cap, const TelemetrySample& s);

}  // namespace diag
}  // namespace ot
