// OpenTracker -- app telemetry line (telemetry.h). Platform independent.
#include "telemetry.h"

#include "app/project_io.h"
#include "audio/cpu_meter.h"
#include "audio/governor.h"
#include "diag_log.h"
#include "model/synth_patch.h"

namespace ot {
namespace diag {

void Telemetry::loopMark(uint32_t nowUs) {
    if (haveLast_) {
        const uint32_t d = nowUs - lastUs_;   // unsigned: wrap-safe
        if (d > loopMaxUs_) loopMaxUs_ = d;
        ++loops_;
    }
    lastUs_ = nowUs;
    haveLast_ = true;
}

Telemetry::ReportHook Telemetry::hook_ = nullptr;

void Telemetry::setReportHook(ReportHook fn) { hook_ = fn; }

bool Telemetry::due(uint32_t nowMs) {
    if (!started_) {
        started_ = true;
        lastReportMs_ = nowMs;
        return false;
    }
    const uint32_t window = nowMs - lastReportMs_;
    if (window < kPeriodMs) return false;
    lastReportMs_ = nowMs;
    if (hook_) hook_(window);
    return true;
}

uint32_t bytesPerSecond(uint32_t bytes, uint32_t windowMs) {
    return windowMs ? uint32_t(uint64_t(bytes) * 1000u / windowMs) : 0;
}

size_t formatLoad(char* out, size_t cap, const JobProfile& p) {
    Fmt o(out, cap);
    o.str("telemetry: load op ").dec(p.op).str(" run ").dec(p.running ? 1 : 0);
    o.str(" passes ").dec(p.passes).str(" units ").dec(p.units).str(" total ").dec(p.totalUs).str(" us");
    o.str(" max pass ").dec(p.maxPassUs).str(" us (stage ").dec(p.maxPassStage).ch('/').dec(p.maxPassPhase).ch(')');
    o.str(" max unit ").dec(p.maxUnitUs).str(" us (stage ").dec(p.maxUnitStage).ch(')');
    o.str(" | open ").dec(p.openUs).str(" us n").dec(p.opens);
    o.str(" read ").dec(p.readUs).str(" us n").dec(p.reads).ch(' ').dec(uint32_t(p.bytesRead / 1024)).str(" KB");
    o.str(" write ").dec(p.writeUs).str(" us n").dec(p.writes).ch(' ').dec(uint32_t(p.bytesWritten / 1024)).str(" KB");
    o.str(" dir ").dec(p.dirUs).str(" us n").dec(p.dirOps);
    o.str(" decode ").dec(p.decodeUs).str(" us commit ").dec(p.commitUs).str(" us");
    return o.len;
}

size_t formatIo(char* out, size_t cap, const IoSample& s) {
    Fmt o(out, cap);
    o.str("telemetry: io sd cmds ").dec(s.sdCmds);
    o.str(" read ").dec(s.sdReadBytes).str(" B (").dec(bytesPerSecond(s.sdReadBytes, s.windowMs) / 1024).str(" KB/s)");
    o.str(" write ").dec(s.sdWriteBytes).str(" B (").dec(bytesPerSecond(s.sdWriteBytes, s.windowMs) / 1024).str(" KB/s)");
    o.str(" direct ").dec(s.sdDirect).str(" bounced ").dec(s.sdBounced);
    o.str(" | cache sdram ").str(s.cacheOn ? "on" : "off").str(" (selftest ").str(s.cacheTest ? s.cacheTest : "").str(")");
    o.str(" | sd->sdram dma read ").str(s.sdDirectRead ? "direct" : "bounced");
    o.str(" write ").str(s.sdDirectWrite ? "direct" : "bounced");
    return o.len;
}

CpuSample cpuSample(const audio::CpuMeter& m, const audio::Governor& g, const audio::ProfileWindow* prof) {
    CpuSample s;
    if (prof) s.hasPeak = prof->read(s.pk, s.cycPerVoice);
    s.avg = m.avg();
    s.peak = m.peak();
    s.overruns = m.overruns();
    s.cap = g.cap();
    s.budget = g.budget();
    s.synthVoices = g.sounding();
    return s;
}

size_t formatCpu(char* out, size_t cap, const CpuSample& s) {
    Fmt o(out, cap);
    o.str("telemetry: cpu avg=").dec(s.avg / 10).ch('.').dec(s.avg % 10);
    o.str("% peak=").dec(s.peak / 10).ch('.').dec(s.peak % 10);
    o.str("% overruns=").dec(s.overruns);
    o.str(" cap=").dec(uint32_t(s.cap < 0 ? 0 : s.cap)).ch('/').dec(uint32_t(s.budget < 0 ? 0 : s.budget));
    o.str(" synth_voices=").dec(uint32_t(s.synthVoices < 0 ? 0 : s.synthVoices));
    o.str(" cyc_per_voice=").dec(s.cycPerVoice);
    if (!s.hasPeak) return o.len;
    const audio::BlockProfile& p = s.pk;
    auto k = [&o](uint32_t c) -> Fmt& { return o.dec((c + 500u) / 1000u).ch('k'); };
    auto eng = [](int32_t id) -> const char* {
        switch (id) {
            case kSynthEngineAcd: return "ACD";
            case kSynthEngineFat: return "FAT";
            case kSynthEngineVap: return "VAP";
            case kSynthEngineWtfm: return "WTFM";
            case kSynthEnginePerc: return "PERC";
            case 0: return "-";
            default: return "ENG";
        }
    };
    o.str(" | pk ");
    k(p.total).str(": seq ");
    k(p.seq).str(" on ");
    k(p.smpOn).ch('/');
    k(p.synOn).str(" ctl ");
    k(p.ctrl).str(" smp ");
    k(p.smp).str(" syn ");
    k(p.synFw);
    for (int i = 0; i < audio::BlockProfile::kSlots; ++i) {
        o.ch(' ').str(eng(p.slotEngine[i])).ch(' ');
        k(p.slot[i]);
    }
    o.str(" dly ");
    k(p.dly).str(" rev ");
    k(p.rev).str(" out ");
    k(p.out);
    o.str(" | ev on ").dec(p.nSmpOn);
    for (int i = 0; i < audio::BlockProfile::kSlots; ++i) o.ch(' ').str(eng(p.slotEngine[i])).ch(' ').dec(p.nSynOn[i]);
    o.str(" rl ").dec(p.nReload).str(" fc ").dec(p.nFullCopy).str(" mc ").dec(p.nMacro);
    o.str(" st ").dec(p.nSteal).str(" pd ").dec(p.nPendStart).str(" df ").dec(p.nDeferred);
    o.str(" vx ").dec(p.nSynVoices).ch('/').dec(p.nSmpVoices);
    // What the seq phase did (block_profile.h SeqWork): letters, pattern
    // tracks copied, MIDI messages sent.
    o.str(" sq ");
    static const char kWhy[] = "PLNERW";
    if (!p.seqWhy) o.ch('-');
    for (int b = 0; b < 6; ++b)
        if (p.seqWhy & (1u << b)) o.ch(kWhy[b]);
    o.str(" pt ").dec(p.nPatTracks).str(" mo ").dec(p.nMidiOut);
    return o.len;
}

bool CpuTelemetry::due(uint32_t nowMs) {
    if (!started_) {
        started_ = true;
        last_ = nowMs;
        return false;
    }
    if (nowMs - last_ < kPeriodMs) return false;
    last_ = nowMs;
    return true;
}

size_t CpuTelemetry::tick(uint32_t nowMs, const audio::CpuMeter& m, const audio::Governor& g, char* out,
                          size_t cap, const audio::ProfileWindow* prof) {
    if (!due(nowMs)) return 0;
    return formatCpu(out, cap, cpuSample(m, g, prof));
}

void Telemetry::resetWindow() {
    loopMaxUs_ = 0;
    loops_ = 0;
    // The iteration in progress (it includes the report itself) still counts:
    // keep lastUs_ so the next mark measures it.
}

size_t untouchedStackWords(const uint32_t* lo, const uint32_t* hi) {
    const uint32_t* p = lo;
    while (p < hi && *p == kStackPaint) ++p;
    return size_t(p - lo);
}

size_t formatTelemetry(char* out, size_t cap, const TelemetrySample& s) {
    Fmt o(out, cap);
    const uint32_t permille = uint32_t(uint64_t(s.cycMax) * 1000u / kRenderBudgetCycles);
    o.str("telemetry: render cyc/block min ").dec(s.cycMin).str(" avg ").dec(s.cycAvg).str(" max ").dec(s.cycMax);
    o.str(" (").dec(permille / 10).ch('.').dec(permille % 10).str("% of ").dec(kRenderBudgetCycles);
    o.str(") blocks ").dec(s.blocks);
    o.str(" | loop max ").dec(s.loopMaxUs / 1000).ch('.');
    const uint32_t f = s.loopMaxUs % 1000;
    o.ch(char('0' + f / 100)).ch(char('0' + f / 10 % 10)).ch(char('0' + f % 10));
    o.str(" ms (n ").dec(s.loops).str(")");
    o.str(" | SDRAM free ").dec(s.sdramFree).str(" B");
    o.str(" | i2c errors i2c0 ").dec(s.i2c0Errors).str(" i2c2 ").dec(s.i2c2Errors);
    if (s.i2c0Naks || s.i2c2Naks) o.str(" nak i2c0 ").dec(s.i2c0Naks).str(" i2c2 ").dec(s.i2c2Naks);
    if (s.stackSize) {
        // The heap has its own reserve below the stack guard (stack_guard.h):
        // the stack's room is all of S.
        const uint32_t peak = s.stackPeak < s.stackSize ? s.stackPeak : s.stackSize;
        o.str(" | stack peak ").dec(peak).str(" B heap ").dec(s.heap).str(" B of ").dec(s.stackSize);
        o.str(" B (free ").dec(s.stackSize - peak).str(" B)");
    }
    o.str(" | rx restarts ").dec(s.rxRestarts).str(" stale ").dec(s.rxStale);
    if (s.dinClocks) {
        o.str(" | din clock late max ").dec(s.dinClockLateMaxUs / 1000).ch('.');
        const uint32_t c = s.dinClockLateMaxUs % 1000;
        o.ch(char('0' + c / 100)).ch(char('0' + c / 10 % 10)).ch(char('0' + c % 10));
        o.str(" ms (n ").dec(s.dinClocks).str(")");
    }
    if (permille >= kRenderAlarmPermille) o.str(" RENDER>70%");
    return o.len;
}

StreamSample streamSample(const audio::StreamServiceStats& st, uint32_t streams, uint32_t active,
                          uint32_t& lastUnderruns, bool loading) {
    StreamSample s;
    s.loading = loading;
    s.streams = streams;
    s.active = active;
    s.chunks = st.chunks;
    s.errors = st.readErrors;
    s.passes = st.passes;
    s.readAvgUs = st.chunks ? st.sumReadUs / st.chunks : 0;
    s.readMaxUs = st.maxReadUs;
    s.leadMinMs = st.minLeadFrames == 0xFFFFFFFFu
                      ? 0xFFFFFFFFu
                      : uint32_t((uint64_t(st.minLeadFrames) * 1000u + 22059u) / 44118u);   // device rate, rounded
    s.underruns = st.underruns;
    s.underrunsDelta = st.underruns > lastUnderruns ? st.underruns - lastUnderruns : 0;
    lastUnderruns = st.underruns;
    return s;
}

size_t formatStream(char* out, size_t cap, const StreamSample& s) {
    Fmt o(out, cap);
    o.str("telemetry: stream streams ").dec(s.streams).str(" active ").dec(s.active);
    o.str(" | chunks ").dec(s.chunks);
    o.str(" read avg ").dec(s.readAvgUs / 1000).ch('.').dec((s.readAvgUs % 1000) / 100);
    o.str(" max ").dec(s.readMaxUs / 1000).ch('.').dec((s.readMaxUs % 1000) / 100).str(" ms");
    o.str(" | lead min ");
    if (s.leadMinMs == 0xFFFFFFFFu) o.ch('-');
    else o.dec(s.leadMinMs);
    o.str(" ms | underruns ").dec(s.underruns).str(" (+").dec(s.underrunsDelta).str(") errors ").dec(s.errors);
    o.str(" | passes ").dec(s.passes);
    if (s.loading) o.str(" | loading");
    return o.len;
}

}  // namespace diag
}  // namespace ot
