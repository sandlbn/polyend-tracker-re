// App telemetry line (src/hal/telemetry.h): loop timing window, 5 s period,
// line format and the 70 % render alarm. PLAN_PHASE2 T1c.
#include "test.h"

#include <cstring>
#include <string>

#include "hal/diag_log.cpp"    // platform independent; not part of libcore
#include "hal/telemetry.cpp"

using namespace ot::diag;

TEST(telemetry_period) {
    Telemetry t;
    CHECK(!t.due(1000));      // first call starts the window
    CHECK(!t.due(5999));
    CHECK(t.due(6000));
    CHECK(!t.due(6001));
    CHECK(t.due(11000));
    // millis wrap
    Telemetry w;
    CHECK(!w.due(0xFFFFF000u));
    CHECK(!w.due(0x00000100u));
    CHECK(w.due(0x00000F00u));
}

TEST(telemetry_loop_max) {
    Telemetry t;
    t.loopMark(100);
    CHECK(t.loops() == 0);
    t.loopMark(600);
    t.loopMark(12600);
    t.loopMark(12700);
    CHECK(t.loops() == 3);
    CHECK(t.loopMaxUs() == 12000);
    t.resetWindow();
    CHECK(t.loops() == 0);
    CHECK(t.loopMaxUs() == 0);
    t.loopMark(13700);        // measured from the last mark before the reset
    CHECK(t.loopMaxUs() == 1000);
    // micros wrap
    Telemetry w;
    w.loopMark(0xFFFFFF00u);
    w.loopMark(0x00000100u);
    CHECK(w.loopMaxUs() == 0x200u);
}

TEST(telemetry_format) {
    TelemetrySample s;
    s.cycMin = 41000;
    s.cycAvg = 52224;
    s.cycMax = 104448;        // 20.0 %
    s.blocks = 1723;
    s.loopMaxUs = 12345;
    s.loops = 4000;
    s.sdramFree = 491520;
    s.i2c0Errors = 1;
    s.i2c2Errors = 2;
    char b[448];
    size_t n = formatTelemetry(b, sizeof b, s);
    CHECK(n == std::strlen(b));
    CHECK(std::string(b) ==
          "telemetry: render cyc/block min 41000 avg 52224 max 104448 (20.0% of 522240) blocks 1723 | "
          "loop max 12.345 ms (n 4000) | SDRAM free 491520 B | i2c errors i2c0 1 i2c2 2 | rx restarts 0 stale 0");
    s.stackSize = 97856;
    s.stackPeak = 41000;
    s.heap = 1856;
    s.rxRestarts = 3;
    s.rxStale = 12;
    formatTelemetry(b, sizeof b, s);
    // The heap has its own reserve below the stack guard: free = room - peak.
    CHECK(std::strstr(b, "i2c2 2 | stack peak 41000 B heap 1856 B of 97856 B (free 56856 B) | rx restarts 3 stale 12") != nullptr);
    s.stackPeak = 100000;     // clamped: never reports negative free space
    formatTelemetry(b, sizeof b, s);
    CHECK(std::strstr(b, "stack peak 97856 B heap 1856 B of 97856 B (free 0 B)") != nullptr);
    s.cycMax = 365568;        // exactly 70 %
    s.loopMaxUs = 7;
    formatTelemetry(b, sizeof b, s);
    CHECK(std::strstr(b, "(70.0% of 522240)") != nullptr);
    CHECK(std::strstr(b, "loop max 0.007 ms") != nullptr);
    CHECK(std::strstr(b, " RENDER>70%") != nullptr);
    CHECK(std::strstr(b, "din clock") == nullptr);   // no clock out: no field
    // MIDI clock out on DIN: queued -> UART, the longest in the window.
    s.cycMax = 104448;
    s.dinClocks = 240;
    s.dinClockLateMaxUs = 31250;
    formatTelemetry(b, sizeof b, s);
    CHECK(std::strstr(b, "stale 12 | din clock late max 31.250 ms (n 240)") != nullptr);
    // Small buffers truncate safely.
    char small[16];
    formatTelemetry(small, sizeof small, s);
    CHECK(std::strlen(small) == 15);
}

TEST(telemetry_stack_scan) {
    uint32_t st[16];
    for (uint32_t& w : st) w = kStackPaint;
    CHECK(untouchedStackWords(st, st + 16) == 16);   // never used
    st[10] = 0;                                      // deepest write so far
    st[13] = 0x1234;
    CHECK(untouchedStackWords(st, st + 16) == 10);
    st[2] = kStackPaint + 1;                         // a deeper write that differs by one bit
    CHECK(untouchedStackWords(st, st + 16) == 2);
    st[0] = 7;
    CHECK(untouchedStackWords(st, st + 16) == 0);    // overflowed to the bottom
    CHECK(untouchedStackWords(st, st) == 0);
}

// ---- 2.x "cpu" line (spec 2x-02 §4, tools/telemetry_check.py) -------------

#include "audio/cpu_meter.h"
#include "audio/governor.h"

TEST(telemetry_cpu_line) {
    CpuSample s;
    s.avg = 372;
    s.peak = 480;
    s.overruns = 2;
    s.cap = 3;
    s.budget = 4;
    s.synthVoices = 1;
    s.cycPerVoice = 12000;
    char line[160];
    const size_t n = formatCpu(line, sizeof line, s);
    CHECK_EQ(n, std::strlen(line));
    CHECK_STR(line, "telemetry: cpu avg=37.2% peak=48.0% overruns=2 cap=3/4 synth_voices=1 cyc_per_voice=12000");
    // With the peak block of the last window (audio/block_profile.h).
    s.hasPeak = true;
    s.pk.total = 491109;
    s.pk.seq = 5400;
    s.pk.smpOn = 8040;
    s.pk.synOn = 8520;
    s.pk.ctrl = 5360;
    s.pk.smp = 160240;
    s.pk.synFw = 14880;
    s.pk.slot[0] = 124920;
    s.pk.slot[1] = 27440;
    s.pk.slot[2] = 40560;
    s.pk.slotEngine[0] = ot::kSynthEngineVap;
    s.pk.slotEngine[1] = ot::kSynthEngineAcd;
    s.pk.slotEngine[2] = ot::kSynthEnginePerc;
    s.pk.dly = 7720;
    s.pk.rev = 39680;
    s.pk.out = 36200;
    s.pk.nSmpOn = 8;
    s.pk.nSynOn[0] = 4;
    s.pk.nSynOn[1] = 1;
    s.pk.nSynOn[2] = 2;
    s.pk.nSteal = 6;
    s.pk.nPendStart = 2;
    s.pk.nDeferred = 3;
    s.pk.nSynVoices = 10;
    s.pk.nSmpVoices = 8;
    s.pk.seqWhy = ot::audio::kSeqPreloadPart | ot::audio::kSeqPatternEnd;
    s.pk.nPatTracks = 2;
    s.pk.nMidiOut = 3;
    char big[400];
    const size_t nb = formatCpu(big, sizeof big, s);
    CHECK_EQ(nb, std::strlen(big));
    CHECK_STR(big, "telemetry: cpu avg=37.2% peak=48.0% overruns=2 cap=3/4 synth_voices=1 cyc_per_voice=12000"
                   " | pk 491k: seq 5k on 8k/9k ctl 5k smp 160k syn 15k VAP 125k ACD 27k PERC 41k dly 8k rev 40k out 36k"
                   " | ev on 8 VAP 4 ACD 1 PERC 2 rl 0 fc 0 mc 0 st 6 pd 2 df 3 vx 10/8 sq PN pt 2 mo 3");
    s.pk.seqWhy = 0;
    CHECK(formatCpu(big, sizeof big, s) > 0 && std::strstr(big, " vx 10/8 sq - pt 2 mo 3") != nullptr);
    s.hasPeak = false;
    // From the meter / governor.
    ot::audio::CpuMeter m;
    ot::audio::Governor g;
    for (int i = 0; i < 10; ++i) m.addBlock(ot::audio::CpuMeter::kBudgetCycles / 2);
    const CpuSample t = cpuSample(m, g);
    CHECK_EQ(t.peak, 500u);
    CHECK_EQ(t.cap, 4);
    CHECK_EQ(t.budget, 4);
    CHECK_EQ(t.synthVoices, 0);
    // 1 Hz period.
    CpuTelemetry c;
    CHECK_EQ(c.tick(100, m, g, line, sizeof line), 0u);   // starts the period
    CHECK_EQ(c.tick(1099, m, g, line, sizeof line), 0u);
    CHECK(c.tick(1100, m, g, line, sizeof line) > 0);
    CHECK(std::strstr(line, "peak=50.0%") != nullptr);
    CHECK_EQ(c.tick(1500, m, g, line, sizeof line), 0u);
    CHECK(c.tick(2100, m, g, line, sizeof line) > 0);
}

// SD streaming C11 (docs/PLAN_SD_STREAMING_P1C_P2.md): the 5 s "stream" line.
TEST(telemetry_stream_line) {
    StreamSample s;
    s.streams = 4;
    s.active = 3;
    s.chunks = 212;
    s.readAvgUs = 2700;
    s.readMaxUs = 3149;
    s.leadMinMs = 612;
    s.underruns = 2;
    s.underrunsDelta = 0;
    s.errors = 0;
    s.passes = 1180;
    char line[200];
    const size_t n = formatStream(line, sizeof line, s);
    CHECK_EQ(n, std::strlen(line));
    CHECK_STR(line, "telemetry: stream streams 4 active 3 | chunks 212 read avg 2.7 max 3.1 ms | lead min 612 ms"
                    " | underruns 2 (+0) errors 0 | passes 1180");
    s.leadMinMs = 0xFFFFFFFFu;   // nothing left to read: no lead
    s.underrunsDelta = 1;
    s.loading = true;
    formatStream(line, sizeof line, s);
    CHECK_STR(line, "telemetry: stream streams 4 active 3 | chunks 212 read avg 2.7 max 3.1 ms | lead min - ms"
                    " | underruns 2 (+1) errors 0 | passes 1180 | loading");
}

TEST(telemetry_stream_sample_from_stats) {
    ot::audio::StreamServiceStats st;
    st.chunks = 10;
    st.sumReadUs = 25000;
    st.maxReadUs = 4000;
    st.minLeadFrames = 44118;   // 1 s
    st.underruns = 3;
    st.passes = 50;
    uint32_t last = 1;
    const StreamSample s = streamSample(st, 2, 1, last, false);
    CHECK_EQ(s.readAvgUs, 2500u);
    CHECK_EQ(s.leadMinMs, 1000u);
    CHECK_EQ(s.underrunsDelta, 2u);
    CHECK_EQ(last, 3u);
    CHECK_EQ(s.streams, 2u);
    CHECK_EQ(s.active, 1u);
    ot::audio::StreamServiceStats idle;   // no reads, no active ring
    const StreamSample z = streamSample(idle, 0, 0, last, false);
    CHECK_EQ(z.readAvgUs, 0u);
    CHECK_EQ(z.leadMinMs, 0xFFFFFFFFu);
    CHECK_EQ(z.underrunsDelta, 0u);   // the mirror is 0 when nothing ran: never negative
}
