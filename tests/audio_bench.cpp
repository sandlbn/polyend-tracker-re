// Host benchmark for the audio engine render path (not a unit test).
//
//   make -C tests bench        host build (-O2, no sanitizers), wall-clock time
//   make -C tests bench-m4     the same code built with the device flags
//                              (cortex-m4, -O2, hard float) and run under
//                              qemu-system-arm -M mps2-an386 -icount shift=0,
//                              which counts executed M4 instructions
//                              (tests/bench_m4/: startup, linker script)
//
// Renders scenarios through AudioEngine::renderLive() -- the same entry the
// device's I2S DMA ISR calls through fillCallback() -- and reports host time
// per 128-frame block, the share of the 2901 us block period, and a rough
// device estimate (see deviceEstimate below).
//
// Scenarios:
//   idle      stopped, no voices, effect tails finished, line input of zeros
//             (the device feeds the codec input every block)
//   play8     8 looping voices, delay + reverb sends, some filters, 1 LFO
//   stereo8   play8 with planar stereo instruments (2x-04 §3.4: <= play8 + 24 %)
//   stream8   stereo8 with 3 s instruments streamed (docs/PLAN_SD_STREAMING.md
//             P1): head resident, the rest through the track rings, refilled
//             from memory between blocks (the main loop's work, not timed);
//             stream.lim gates it at <= stereo8 + 5 %
//   worst     8 voices + metronome + file preview: granular / wavetable /
//             ping-pong, filter + overdrive + bit crusher on all, 5 LFOs,
//             saturation, side-chain limiter, line-in with sends
//   synth4    2.x synth framework: 4 voices of the internal test engine
//             (sine + envelope) in Synth 1, reverb send on; WP-F engines
//             replace the test engine (docs/MINI_PLAN.md §4: <= 72k/block)
//   synth.fw  (component) framework overhead per block for those 4 voices:
//             SynthBank::render minus 4 x the engine's own render (<= 3k)
//   perc      (component) PERC engine, one slot, all six drums hit every
//             32 blocks (costly models, waveshaper + 3-band EQ on; the
//             engine's cost governor cuts drums to stay in budget):
//             OPTIMIZATION_2X item 10, <= 30k per slot per block;
//             perc.def = the Init kit (five drums ring)
//   acd.*     (component) one ACD engine voice per block: Init patch (held)
//             and worst case (OPTIMIZATION_2X item 10: <= 12k / <= 18k)
//   vapinit   VAP engine, one voice of the Init patch (prepare + render), per
//             block (OPTIMIZATION_2X item 10: <= 12k)
//   vapworst  VAP, one voice of the costliest settings: hard sync (both
//             oscillators on the general morph), noise, LP OB 24 with its
//             soft clip, six mod slots incl. Filter Env Decay (<= 18k)
//   fatinit   FAT engine, one voice of the Init patch (prepare + render), per
//             block (docs/specs/2x-07-fat-engine.md: <= 12k)
//   fatworst  FAT, one voice of the costliest settings: seven saws with
//             Timbre's second saw, Brightness, noise, LP OB 24 with its soft
//             clip, filter envelope + S&H LFO, glide 108 -> 96 (measured
//             at pitch ~108, where the polyBLEP events cost most; <= 22k)
//   mix       2.0.0 worst mix (docs/MINI_PLAN.md §5 hardware checklist): the
//             stereo8 set (8 stereo sample voices, delay + reverb) plus 4
//             ACD voices of the worst-case patch in Synth 1 and the costly
//             PERC kit in Synth 2 (all six drums hit every 32 blocks, per-drum
//             sends on), through renderLive with the real engines, at the
//             default synth voice budget 4 (the governor steals the fifth
//             voice); mix5 / mix6 at budgets 5 and 6 (all of them play).
//             Reported as % of the block budget (the plan's 70 % alarm).
//             mixfat: the mix set with 4 worst-case FAT voices (fatworst's
//             patch) in Synth 1 instead of ACD, budget 4
//   pswitch   immediate pattern switch while playing (OPTIMIZATION_2X item 2,
//             OP-1): the play8 set with the sequencer driving the engine
//             (pre-block hook, as on the device) and a pattern switch
//             requested before a block. The switch block's cost; pswitch.d =
//             switch block - steady block in estimated cycles (limit 40000).
//             The main loop assembles the new pattern into the sequencer's
//             stage buffer; the audio block only swaps pointers. The legacy
//             path (pattern copied in the audio block) is printed for reference.
//   *.max     the per-engine limits: every component line that prints
//             "limit N" is gated by make perf-gate (tools/telemetry_check.py)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#ifndef OT_BENCH_M4
#include <chrono>
#endif

#include "audio/engine.h"
#include "audio/stream_source.h"
#include "audio/engines/perc.h"
#include "audio/synth_params.h"
#include "audio/engines/acd.h"
#include "audio/engines/fat.h"
#include "audio/engines/vap.h"
#include "audio/engines/wtfm.h"
#include "formats/patch_pb.h"
#include "peak_trace.h"
#include "sequencer/sequencer.h"

using namespace ot;
using namespace ot::audio;

namespace ot {
namespace audio {
uint32_t gSdramAccesses = 0;   // counted by the engine (OT_AUDIO_SDRAM_STATS)
}
}  // namespace ot

namespace {

constexpr float kPi = 3.14159265f;

#ifdef OT_BENCH_M4
// QEMU mps2-an386: CMSDK APB timer 0 at 25 MHz; with -icount shift=0 one
// instruction is 1 ns of virtual time, so one timer tick = 40 instructions.
// Large buffers live in the board's PSRAM (section .psram, tests/bench_m4).
#define OT_BIG __attribute__((section(".psram")))
volatile uint32_t* const kTimer = reinterpret_cast<volatile uint32_t*>(0x40000000u);
void clockStart() {
    kTimer[0] = 0;
    kTimer[2] = 0xFFFFFFFFu;   // RELOAD
    kTimer[1] = 0xFFFFFFFFu;   // VALUE
    kTimer[0] = 1;             // enable
}
double nowUnits() { return double(0xFFFFFFFFu - kTimer[1]) * 40.0; }   // instructions
constexpr const char* kUnit = "M4 insn";
constexpr int kReps = 1;
constexpr int kScale = 8;   // fewer blocks under emulation
#else
#define OT_BIG
void clockStart() {}
double nowUnits() {
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
}
constexpr const char* kUnit = "us";
constexpr int kReps = 5;
constexpr int kScale = 1;
#endif

OT_BIG int16_t gFx[AudioEngine::kFxMemorySamples];
SynthPatch gSynthPatch;
SynthPatch gPercPatch;
OT_BIG int16_t gPool[2u << 20];
OT_BIG int16_t gRingMem[size_t(kVoices) * 2 * ot::audio::kStreamRingFrames];   // stream8 rings
Instrument gIns[kSampleInstrumentCount];
AudioEngine gEng;

struct Rig {
    Instrument* ins = gIns;
    AudioEngine* eng = &gEng;
    Rig() {
        for (int i = 0; i < kSampleInstrumentCount; ++i) gIns[i] = Instrument();
        eng->settings() = MixerSettings();
        eng->setLineInput(nullptr);
        eng->init(gFx, AudioEngine::kFxMemorySamples, gPool, sizeof(gPool) / 2, gIns);
        eng->clearEffects();
        eng->stopAll();
        for (int t = 0; t < kVoices; ++t) eng->voice(t).resetFx();
    }
    void load(int i, const std::vector<int16_t>& d, PlayMode m) {
        ins[i].lengthFrames = uint32_t(d.size());
        ins[i].playMode = uint8_t(m);
        eng->bank().store(i, d.data(), uint32_t(d.size()), false);
    }
    // Planar stereo instrument: L block then R block (2.x stereo playback).
    void loadSt(int i, const std::vector<int16_t>& l, const std::vector<int16_t>& r, PlayMode m) {
        std::vector<int16_t> p(l);
        p.insert(p.end(), r.begin(), r.end());
        ins[i].lengthFrames = uint32_t(l.size());
        ins[i].playMode = uint8_t(m);
        eng->bank().store(i, p.data(), uint32_t(l.size()), true);
    }
    // Synth slots on the internal test engine, ACD-shaped patch with
    // sustain 1 (voices keep sounding through the run).
    void synth(int poly) {
        patchInit(kSynthEngineAcd, &gSynthPatch);
        gSynthPatch.params[17] = PatchParam{patchFloatToBits(1.0f), true};   // Amp Env Sustain
        SynthBank& b = eng->synth();
        b.reset();
        b.setEngineOverride(synthTestEngine());
        for (int s = 0; s < 3; ++s) b.bindPatch(s, s == 0 ? &gSynthPatch : nullptr);
        SynthSlotRecord rc = SynthSlotRecord::defaults(0);
        rc.model = uint32_t(SynthModel::ACD);
        rc.polyphony = uint32_t(poly);
        b.setSlotRecord(0, rc);
    }
    void synthNote(uint8_t track, uint8_t slot, uint8_t note) {
        SynthNoteOn n;
        n.slot = slot;
        n.note = note;
        eng->synthNoteOn(track, n);
    }
    void note(uint8_t track, uint8_t instr, uint8_t note) {
        VoiceNoteOn n;
        n.instrument = instr;
        n.note = note;
        n.volume = -1;
        eng->voiceNoteOn(track, n);
    }
};

std::vector<int16_t> tone(uint32_t len, float hz) {
    std::vector<int16_t> d(len);
    uint32_t seed = 11;
    for (uint32_t i = 0; i < len; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float n = float(int32_t(seed) >> 16) * (1.0f / 32768.0f);
        d[i] = int16_t(20000.0f * std::sin(2.0f * kPi * hz * float(i) / 44100.0f) + 2000.0f * n);
    }
    return d;
}

volatile int16_t gSink;
int16_t gLineNoise[kBlock * 2];
const int16_t* lineNoiseBuf() { return gLineNoise; }

double gPlay8 = 1.0;         // play8 cost, for the stereo8 ratio
double gStereo8 = 1.0;       // stereo8 cost, for the stream8 limit
double gAccPerBlock = 0.0;   // SDRAM accesses per block of the last timeBlocks()

// Per-block cost in kUnit (best of kReps runs of n blocks).
double timeBlocks(Rig& r, int n, const int16_t* lineIn) {
    n /= kScale;
    int16_t out[kBlock * 2];
    double best = 1e30;
    for (int rep = 0; rep < kReps; ++rep) {
        gSdramAccesses = 0;
        const double t0 = nowUnits();
        for (int b = 0; b < n; ++b) {
            if (lineIn) r.eng->setLineInput(lineIn);
            r.eng->renderLive(out);
            gSink = out[b & 255];
        }
        const double per = (nowUnits() - t0) / n;
        gAccPerBlock = double(gSdramAccesses) / n;
        if (per < best) best = per;
    }
    return best;
}

// Device estimate: M4 cycles ~= instructions + SDRAM accesses * kSdramCycles.
// Calibrated on the field data of the previous engine: stopped and silent it
// cost 292,000 cycles/block on the device, and this harness (newlib-nano,
// like the device) counts 167,000 instructions for it plus 3,712 uncached
// 16-bit SDRAM accesses per block (reverb 23 + delay 6 per sample):
// (292,000 - 167,000) / 3,712 = ~34 cycles per access. The engine now uses
// sequential 32-bit accesses, so this is on the pessimistic side.
constexpr double kSdramCycles = 34.0;
void report(const char* name, double v) {
#ifdef OT_BENCH_M4
    const double est = v + gAccPerBlock * kSdramCycles;
    std::printf("  %-6s %7.0f insn + %5.0f SDRAM acc/block -> est. %7.0f cycles (%5.1f%% of 522240)\n", name, v,
                gAccPerBlock, est, 100.0 * est / 522240.0);
#else
    std::printf("  %-6s %8.2f %s/block  (%5.2f%% of the 2901 us block period on this host), %5.0f SDRAM acc/block\n",
                name, v, kUnit, 100.0 * v / 2901.0, gAccPerBlock);
#endif
}


// One VAP voice per block (prepare + render), sustained note.
double vapVoice(bool worst) {
    static float vals[kSynthParamMax];
    int count = 0;
    const PatchParamRange* t = patchEngineParams(kSynthEngineVap, &count);
    for (int i = 0; i < count; ++i) vals[i] = t[i].def;
    if (worst) {
        using namespace vap;
        vals[kShape1] = 0.3f;
        vals[kShape2] = 0.8f;
        vals[kTune2] = 7.0f;
        vals[kOscSync] = 1.0f;
        vals[kNoise] = 0.2f;
        vals[kFilterType] = 1.0f;   // LP OB 24
        vals[kResonance] = 1.2f;
        vals[kLfo1Ratio] = 5.0f;
        vals[kLfo2Ratio] = 0.7f;
        vals[kMixEnvAmt] = 0.5f;
        const float slots[6][3] = {{3, 3, 0.2f}, {4, 8, 0.01f}, {3, 0, 0.5f}, {2, 12, 0.4f}, {4, 7, 0.3f}, {3, 10, 0.2f}};
        for (int k = 0; k < 6; ++k)
            for (int j = 0; j < 3; ++j) vals[kModBase + 3 * k + j] = slots[k][j];
    }
    static SynthParams p;
    p.engine = kSynthEngineVap;
    p.count = count;
    for (int i = 0; i < count; ++i) p.start[i] = p.end[i] = vals[i];
    p.serial = 1;
    ISynthEngine* e = vap::engine();
    static SynthSlotScratch sc;
    static SynthVoice v;
    e->slotReset(sc);
    v = SynthVoice{};
    v.slotScratch = &sc;
    v.gate = true;
    SynthNote n;
    n.pitch = 48.0f;
    e->noteOn(v, p, n);
    static float l[kBlock], r[kBlock];
    const int blocks = 800 / kScale;
    for (int b = 0; b < 20; ++b) {   // past the attack
        e->prepare(sc, p);
        e->render(v, p, 0.5f, 0.0f, l, r);
    }
    const double t0 = nowUnits();
    for (int b = 0; b < blocks; ++b) {
        e->prepare(sc, p);
        e->render(v, p, 0.5f, 0.0f, l, r);
    }
    return (nowUnits() - t0) / blocks;
}

// FAT bench patch: Init, or the costliest settings (every pair, Timbre's
// second saw, Brightness and Noise, LP OB 24 with resonance, the filter's
// envelope and a 30 Hz S&H LFO retriggered per note, 3 s glide).
void fatWorst(float* vals) {
    using namespace fat;
    const float w[][2] = {{kFatness, 1.0f}, {kTimbre, 1.0f}, {kNoise, 0.3f}, {kBrightness, -0.5f},
                          {kFilterType, kLpOb24}, {kResonance, 1.2f}, {kFilterLfo, 0.3f}, {kFilterEnv, 0.5f},
                          {kEnvS, 0.5f}, {kLfoHz, 30.0f}, {kLfoWave, kSampleHold}, {kLfoRetrig, kNote},
                          {kGlideTime, 3.0f}};
    for (auto& x : w) vals[int(x[0])] = x[1];
}

// One FAT voice per block (prepare + render), sustained note (worst: gliding
// down from pitch 108 to 96 on a reused voice: the polyBLEP event cost grows
// with pitch, so the figure is taken at the top of the range; the 3 s glide
// moves ~1.2 semitones over the emulated run, so it is measured at ~108).
double fatVoice(bool worst) {
    static float vals[kSynthParamMax];
    int count = 0;
    const PatchParamRange* t = patchEngineParams(kSynthEngineFat, &count);
    for (int i = 0; i < count; ++i) vals[i] = t[i].def;
    vals[fat::kAmpS] = 1.0f;   // keep the voice sounding for the whole run
    if (worst) fatWorst(vals);
    static SynthParams p;
    p.engine = kSynthEngineFat;
    p.count = count;
    for (int i = 0; i < count; ++i) p.start[i] = p.end[i] = vals[i];
    p.serial = 1;
    ISynthEngine* e = fat::engine();
    static SynthSlotScratch sc;
    static SynthVoice v;
    e->slotReset(sc);
    v = SynthVoice{};
    v.slotScratch = &sc;
    v.gate = true;
    SynthNote n;
    n.pitch = worst ? 108.0f : 48.0f;
    e->noteOn(v, p, n);
    static float l[kBlock], r[kBlock];
    const int blocks = 800 / kScale;
    for (int b = 0; b < 20; ++b) {   // past the attack
        e->prepare(sc, p);
        e->render(v, p, 0.5f, 0.0f, l, r);
    }
    if (worst) {
        n.pitch = 96.0f;
        n.reused = true;
        e->noteOn(v, p, n);
    }
    const double t0 = nowUnits();
    for (int b = 0; b < blocks; ++b) {
        e->prepare(sc, p);
        e->render(v, p, 0.5f, 0.0f, l, r);
    }
    return (nowUnits() - t0) / blocks;
}

// WTFM's costliest settings (spec 2x-08 §7): Oversampling, both operators
// Wilder, FM, the three feedback paths, LP OB 24 with resonance, both LFOs
// and six mod slots incl. Env 2 -> FM and Env 1 -> Flt Env Decay, glide.
void wtfmWorst(float* vals) {
    using namespace wtfm;
    const float w[][2] = {{kOversample, 1.0f}, {kChar1, kWilder}, {kChar2, kWilder}, {kShape1, 0.75f}, {kShape2, 0.75f},
                          {kFm, 3.0f}, {kFb1, 0.6f}, {kFb2, 0.6f}, {kFb21, 0.4f}, {kRatio2, 7.0f}, {kFine2, 7.0f},
                          {kFilterType, 1.0f}, {kResonance, 1.2f}, {kFilterEnvAmt, 0.5f}, {kEnv1S, 0.5f},
                          {kLfo1Ratio, 30.0f}, {kLfo1Wave, 5.0f}, {kLfo1Retrig, 2.0f}, {kLfo2Ratio, 0.7f},
                          {kGlideTime, 3.0f}};
    for (auto& x : w) vals[int(x[0])] = x[1];
    const int route[6][2] = {{kSrcLfo1, kDstShape1}, {kSrcEnv2, kDstFm}, {kSrcLfo2, kDstCutoff},
                             {kSrcAmpEnv, kDstFb21}, {kSrcEnv1, kDstEnv1Decay}, {kSrcLfo2, kDstPitch2}};
    for (int k = 0; k < 6; ++k) {
        vals[kModBase + 3 * k] = float(route[k][0]);
        vals[kModBase + 3 * k + 1] = float(route[k][1]);
        vals[kModBase + 3 * k + 2] = 0.3f;
    }
}

// One WTFM voice per block (prepare + render), sustained note (worst: a 3 s
// glide 36 -> 60 on a reused voice).
double wtfmVoice(bool worst) {
    static float vals[kSynthParamMax];
    int count = 0;
    const PatchParamRange* t = patchEngineParams(kSynthEngineWtfm, &count);
    for (int i = 0; i < count; ++i) vals[i] = t[i].def;
    vals[wtfm::kAmpS] = 1.0f;
    if (worst) wtfmWorst(vals);
    static SynthParams p;
    p.engine = kSynthEngineWtfm;
    p.count = count;
    for (int i = 0; i < count; ++i) p.start[i] = p.end[i] = vals[i];
    p.serial = 1;
    ISynthEngine* e = wtfm::engine();
    static SynthSlotScratch sc;
    static SynthVoice v;
    e->slotReset(sc);
    v = SynthVoice{};
    v.slotScratch = &sc;
    v.gate = true;
    SynthNote n;
    n.pitch = worst ? 36.0f : 48.0f;
    e->noteOn(v, p, n);
    static float l[kBlock], r[kBlock];
    const int blocks = 800 / kScale;
    for (int b = 0; b < 20; ++b) {
        e->prepare(sc, p);
        e->render(v, p, 0.5f, 0.0f, l, r);
    }
    if (worst) {
        n.pitch = 60.0f;
        n.reused = true;
        e->noteOn(v, p, n);
    }
    const double t0 = nowUnits();
    for (int b = 0; b < blocks; ++b) {
        e->prepare(sc, p);
        e->render(v, p, 0.5f, 0.0f, l, r);
    }
    return (nowUnits() - t0) / blocks;
}

void reportVoice(const char* name, double v, int limit) {
#ifdef OT_BENCH_M4
    std::printf("  %-8s %7.0f insn per voice per block (limit %d)\n", name, v, limit);
#else
    std::printf("  %-8s %8.2f %s per voice per block\n", name, v, kUnit);
    (void)limit;
#endif
}

// mix: stereo8 + 4 worst ACD voices (engine kSynthEngineFat / Wtfm: those
// engines' worst voices) + the costly PERC kit (see the header), at synth
// voice budget `budget` (Governor; the default is 4).
void mixBench(int budget, const char* name, int32_t engine = kSynthEngineAcd) {
    const bool fat = engine == kSynthEngineFat;
    Rig r;
    for (int i = 0; i < 4; ++i) {
        r.loadSt(i, tone(44100, 110.0f * float(i + 1)), tone(44100, 165.0f * float(i + 1)), PlayMode::ForwardLoop);
        r.ins[i].delaySend = uint8_t(20 + 10 * i);
        r.ins[i].reverbSend = uint8_t(30 + 5 * i);
    }
    r.ins[1].filterEnabled = 1;
    r.ins[1].cutoff = 0.5f;
    r.ins[2].env[kAutoPanning].enabled = 1;
    r.ins[2].env[kAutoPanning].loop = 1;
    r.eng->settings().delayTimeMs = 375;
    r.eng->settings().delayFeedback = 45;
    for (int t = 0; t < 8; ++t) r.note(uint8_t(t), uint8_t(t & 3), uint8_t(48 + 3 * t));
    // Synth 1: ACD, worst-case patch (every oscillator, RD3, all modulation),
    // or FAT's worst case (fatWorst).
    if (engine == kSynthEngineWtfm) {
        patchInit(kSynthEngineWtfm, &gSynthPatch);
        float vals[kSynthParamMax];
        int count = 0;
        const PatchParamRange* t = patchEngineParams(kSynthEngineWtfm, &count);
        for (int i = 0; i < count; ++i) vals[i] = t[i].def;
        wtfmWorst(vals);
        vals[wtfm::kAmpS] = 1.0f;
        for (int i = 0; i < count; ++i)
            if (vals[i] != t[i].def) synthParamSet(gSynthPatch, i, vals[i]);
    } else if (fat) {
        patchInit(kSynthEngineFat, &gSynthPatch);
        float vals[kSynthParamMax];
        int count = 0;
        const PatchParamRange* t = patchEngineParams(kSynthEngineFat, &count);
        for (int i = 0; i < count; ++i) vals[i] = t[i].def;
        fatWorst(vals);
        vals[fat::kAmpS] = 1.0f;
        for (int i = 0; i < count; ++i)
            if (vals[i] != t[i].def) synthParamSet(gSynthPatch, i, vals[i]);
    } else {
        using namespace acd;
        patchInit(kSynthEngineAcd, &gSynthPatch);
        const float w[][2] = {{kSquareMix, 1}, {kSubMix, 1}, {kSubMode, 1}, {kNoiseMix, 0.5f}, {kPwLfo, 0.3f},
                              {kPwEnv, 0.2f}, {kPitchLfo, 0.05f}, {kFilterType, kRd3}, {kResonance, 1.2f},
                              {kFilterLfo, 0.2f}, {kFilterEnv, 0.3f}, {kModS, 0.5f}, {kLfoHz, 30.0f},
                              {kLfoWave, kSampleHold}, {kLfoRetrig, kNote}, {kGlideTime, 3.0f}, {kAmpS, 1.0f}};
        for (auto& x : w) synthParamSet(gSynthPatch, int(x[0]), x[1]);
    }
    // Synth 2: PERC, the costly kit of the perc component.
    patchInit(kSynthEnginePerc, &gPercPatch);
    const float set[][2] = {{perc::kKickWave, 98}, {perc::kTomModel, 2}, {perc::kTomWave, 60}, {perc::kSnareModel, 4},
                            {perc::kSnareWave, 70}, {perc::kHatModel, 1}, {perc::kHatOpen, 3}, {perc::kCymBodyDecay, 4},
                            {perc::kPercModel, 1}, {perc::kPercDecay, 2}, {perc::kKickDecay, 2}, {perc::kTomDecay, 2},
                            {perc::kSnareDecay, 2}, {perc::kSnareSnapDecay, 2}, {perc::kWsLevel, 0.5f},
                            {perc::kWsType, 1}, {perc::kEqLow, 3}, {perc::kEqMid, -3}, {perc::kEqHigh, 3}};
    for (const auto& kv : set) synthParamSet(gPercPatch, int(kv[0]), kv[1]);
    SynthBank& b = r.eng->synth();
    b.reset();
    b.setEngineOverride(nullptr);
    r.eng->governor().setBudget(budget);
    b.bindPatch(0, &gSynthPatch);
    b.bindPatch(1, &gPercPatch);
    b.bindPatch(2, nullptr);
    SynthSlotRecord a = SynthSlotRecord::defaults(0);
    a.model = uint32_t(engine == kSynthEngineWtfm ? SynthModel::WTFM : (fat ? SynthModel::FAT : SynthModel::ACD));
    a.polyphony = 4;
    a.delaySend = 30.0f;
    a.reverbSend = 30.0f;
    b.setSlotRecord(0, a);
    SynthSlotRecord pr = SynthSlotRecord::defaults(1);
    pr.model = uint32_t(SynthModel::PERC);
    pr.polyphony = 1;
    pr.reverbSend = 20.0f;
    b.setSlotRecord(1, pr);
    for (int k = 0; k < 4; ++k) r.synthNote(uint8_t(8 + k), 0, uint8_t(36 + 7 * k));
    const uint8_t drums[6] = {60, 48, 62, 66, 69, 71};
    const int n = 2000 / kScale;
    int16_t out[kBlock * 2];
    double sum = 0.0, peak = 0.0, acc = 0.0;
    gSdramAccesses = 0;
    for (int bl = 0; bl < n; ++bl) {
        const double t0 = nowUnits();
        if ((bl & 31) == 0)
            for (int k = 0; k < 6; ++k) r.synthNote(uint8_t(12 + (k & 3)), 1, drums[k]);
        r.eng->setLineInput(lineNoiseBuf());
        r.eng->renderLive(out);
        gSink = out[bl & 255];
        const double c = nowUnits() - t0;
        sum += c;
        if (c > peak) peak = c;
    }
    acc = double(gSdramAccesses) / n;
    gAccPerBlock = acc;
    report(name, sum / n);
#ifdef OT_BENCH_M4
    const double estPk = peak + acc * kSdramCycles;
    std::printf("  (%s: budget %d, %d synth voices sounding; peak block est. %.0f cycles = %.1f%% of 522240; alarm 70%%)\n",
                name, budget, b.sounding(), estPk, 100.0 * estPk / 522240.0);
#else
    std::printf("  (%s: budget %d, %d synth voices sounding)\n", name, budget, b.sounding());
#endif
}

// pswitch: immediate pattern switch (see the header).
struct SwitchHost : ISequencerHost {
    const Pattern* store = nullptr;
    bool inBlock = false;
    // The App copies the pattern from the resident store (SDRAM -> SDRAM,
    // word copies with src/hal/fast_mem.c): counted as slow accesses when it
    // happens inside the audio block.
    void loadPattern(uint8_t, Pattern& dst, bool) override {
        std::memcpy(&dst, store, sizeof(Pattern));
        if (inBlock) gSdramAccesses += 2u * uint32_t(sizeof(Pattern) / 4);
    }
};
OT_BIG Pattern gSeqPat[4];
Project gSeqProject;
Sequencer gSeq;
SwitchHost gSwitchHost;
void seqPreBlock(void* ctx, float s) { static_cast<Sequencer*>(ctx)->processElapsed(s); }

void switchBench() {
    Rig r;
    for (int i = 0; i < 4; ++i) {
        r.load(i, tone(44100, 110.0f * float(i + 1)), PlayMode::ForwardLoop);
        r.ins[i].delaySend = uint8_t(20 + 10 * i);
        r.ins[i].reverbSend = uint8_t(30 + 5 * i);
    }
    r.ins[1].filterEnabled = 1;
    r.ins[1].cutoff = 0.5f;
    r.eng->settings().delayTimeMs = 375;
    r.eng->settings().delayFeedback = 45;
    for (int t = 0; t < 8; ++t) r.note(uint8_t(t), uint8_t(t & 3), uint8_t(48 + 3 * t));
    for (Pattern& p : gSeqPat) p.setDefault();   // empty steps: the voices keep looping
    gSwitchHost.store = &gSeqPat[3];
    gSeqProject.setTempo(120.0f);
    r.eng->setPreBlockHook(&seqPreBlock, &gSeq);
    int16_t out[kBlock * 2];
    double res[2][3] = {};   // [legacy, staged][steady, switch insn, switch SDRAM acc]
    for (int staged = 0; staged < 2; ++staged) {
        gSeq.attach(r.eng, &gSwitchHost, &gSeqProject, &gSeqPat[0], &gSeqPat[1], staged ? &gSeqPat[2] : nullptr);
        gSeq.play(0);
        for (int b = 0; b < 40; ++b) r.eng->renderLive(out);
        const int n = 64 / (kScale > 4 ? 4 : kScale);
        double steady = 0.0, sw = 0.0, acc = 0.0, steadyAcc = 0.0;
        for (int k = 0; k < n; ++k) {
            gSdramAccesses = 0;
            double t0 = nowUnits();
            r.eng->renderLive(out);
            steady += nowUnits() - t0;
            steadyAcc += gSdramAccesses;
            gSeq.switchPatternNow(uint8_t(2 + (k & 7)));   // main loop
            gSdramAccesses = 0;
            gSwitchHost.inBlock = true;
            t0 = nowUnits();
            r.eng->renderLive(out);
            sw += nowUnits() - t0;
            gSwitchHost.inBlock = false;
            acc += gSdramAccesses;
            gSink = out[k & 255];
        }
        res[staged][0] = steady / n + steadyAcc / n * kSdramCycles;
        res[staged][1] = sw / n;
        res[staged][2] = acc / n;
        gAccPerBlock = acc / n;
        if (staged) report("pswitch", sw / n);
        gSeq.stop();
    }
    r.eng->setPreBlockHook(nullptr, nullptr);
    const double dLegacy = res[0][1] + res[0][2] * kSdramCycles - res[0][0];
    const double dStaged = res[1][1] + res[1][2] * kSdramCycles - res[1][0];
#ifdef OT_BENCH_M4
    std::printf("  (pswitch legacy, pattern copied in the audio block: switch block %+.0f est. cycles)\n", dLegacy);
    std::printf("    pswitch.d  %8.1f  (switch block - steady block, est. cycles, limit 40000)\n", dStaged);
#else
    std::printf("  (pswitch: switch block - steady %+.2f us, legacy copy in the block %+.2f us)\n", dStaged, dLegacy);
#endif
}

// peak: song 5 replayed from its trace (tests/peak_trace.cpp, make -C tests
// peak-trace) through AudioEngine::fillCallback, i.e. with the CPU meter,
// the governor and the block profile (audio/block_profile.h) as on the
// device; the sequencer's events of each block are applied in the pre-block
// hook. Reports the average and the worst block, and the worst blocks'
// attribution (cost per phase, events).
OT_BIG uint8_t gTrace[6u << 20];
SynthPatch gTracePatches[3];
peak::Replay gReplay;
int gTracePos = 0, gTraceStep = 0;
bool gTraceMore = true;
void replayHook(void*, float) { gTraceMore = gReplay.applyBlock(&gTracePos, &gTraceStep); }
// Profile stamps in kUnit, integer only (no soft double in the measured code).
#ifdef OT_BENCH_M4
uint32_t benchCycles() { return (0xFFFFFFFFu - kTimer[1]) * 40u; }
#else
uint32_t benchCycles() {
    return uint32_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}
#endif
#ifdef OT_BENCH_M4
constexpr const char* kStampUnit = "M4 insn";
#else
constexpr const char* kStampUnit = "ns";
#endif

struct PeakBlock {
    double cost = 0.0, acc = 0.0;
    uint32_t block = 0;
    int pos = 0, step = 0;
    BlockProfile p;
};

void printProfile(const PeakBlock& b) {
    const BlockProfile& p = b.p;
    auto eng = [](int32_t id) -> const char* {
        switch (id) {
            case kSynthEngineAcd: return "ACD";
            case kSynthEngineFat: return "FAT";
            case kSynthEngineVap: return "VAP";
            case kSynthEnginePerc: return "PERC";
            case 0: return "-";
            default: return "ENG";
        }
    };
    std::printf("    block %5u (song pos %d, step %3d): %7.0f %s, est. %5.1f%% | seq %u on %u/%u ctl %u smp %u syn %u",
                unsigned(b.block), b.pos, b.step, b.cost, kUnit,
                100.0 * (b.cost + b.acc * kSdramCycles) / 522240.0, unsigned(p.seq), unsigned(p.smpOn),
                unsigned(p.synOn), unsigned(p.ctrl), unsigned(p.smp), unsigned(p.synFw));
    for (int s = 0; s < 3; ++s) std::printf(" %s %u", eng(p.slotEngine[s]), unsigned(p.slot[s]));
    std::printf(" dly %u rev %u out %u | on %u", unsigned(p.dly), unsigned(p.rev), unsigned(p.out), unsigned(p.nSmpOn));
    for (int s = 0; s < 3; ++s) std::printf(" %s %u", eng(p.slotEngine[s]), unsigned(p.nSynOn[s]));
    std::printf(" rl %u fc %u mc %u st %u pd %u df %u vx %u/%u vmax %u\n", unsigned(p.nReload), unsigned(p.nFullCopy),
                unsigned(p.nMacro), unsigned(p.nSteal), unsigned(p.nPendStart), unsigned(p.nDeferred),
                unsigned(p.nSynVoices), unsigned(p.nSmpVoices), unsigned(p.voiceMax));
}

void peakBench(const char* path, uint32_t maxBlocks) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::printf("  peak: no trace at %s (make -C tests peak-trace) -- SKIPPED\n", path);
        return;
    }
    const size_t n = std::fread(gTrace, 1, sizeof gTrace, f);
    std::fclose(f);
    Rig r;
    char why[96];
    if (!gReplay.load(gTrace, n, *r.eng, r.ins, gTracePatches, why, sizeof why)) {
        std::printf("  peak: trace %s unusable: %s -- SKIPPED\n", path, why);
        return;
    }
    r.eng->setPreBlockHook(&replayHook, nullptr);
    CpuMeter::setCycleSource(&benchCycles);
    r.eng->cpu().reset();
#ifndef OT_BENCH_M4
    // Host stamps are ns, not cycles: keep the governor out of the replay.
    r.eng->governor().setEnabled(false);
#endif
    gTraceMore = true;
    constexpr int kTop = 6;
    PeakBlock top[kTop];
    double sum = 0.0, accSum = 0.0;
    uint32_t blocks = 0, over = 0;
    int16_t out[kBlock * 2];
    const uint32_t limit = maxBlocks < gReplay.blocks() ? maxBlocks : gReplay.blocks();
    std::vector<float> costs;
    uint32_t percMax = 0;   // PERC slot, worst block (hits included)
    for (; blocks < limit && gTraceMore; ++blocks) {
        gSdramAccesses = 0;
        const double t0 = nowUnits();
        AudioEngine::fillCallback(out, kBlock, r.eng);
        const double c = nowUnits() - t0;
        gSink = out[blocks & 255];
        const double acc = gSdramAccesses;
        sum += c;
        accSum += acc;
        costs.push_back(float(c));
        const BlockProfile& bp = r.eng->lastBlockProfile();
        for (int k = 0; k < 3; ++k)
            if (bp.slotEngine[k] == kSynthEnginePerc && bp.slot[k] > percMax) percMax = bp.slot[k];
        if (c + acc * kSdramCycles > 0.7 * 522240.0) ++over;
        int lo = 0;
        for (int k = 1; k < kTop; ++k)
            if (top[k].cost < top[lo].cost) lo = k;
        if (c > top[lo].cost) {
            top[lo].cost = c;
            top[lo].acc = acc;
            top[lo].block = blocks;
            top[lo].pos = gTracePos;
            top[lo].step = gTraceStep;
            top[lo].p = r.eng->lastBlockProfile();
        }
    }
    CpuMeter::setCycleSource(nullptr);
    r.eng->setPreBlockHook(nullptr, nullptr);
    r.eng->governor().setEnabled(true);
    if (!blocks) return;
    std::sort(top, top + kTop, [](const PeakBlock& a, const PeakBlock& b) { return a.cost > b.cost; });
    std::sort(costs.begin(), costs.end());
    gAccPerBlock = accSum / blocks;
    report("peakavg", sum / blocks);
    gAccPerBlock = top[0].acc;
    report("peak", top[0].cost);
    const double p99 = costs[size_t(double(costs.size() - 1) * 0.99)];
    const double p999 = costs[size_t(double(costs.size() - 1) * 0.999)];
    std::printf("  (peak: song 5 replay, %u blocks, %u synth steals, governor cuts %u, cap %d/%d at the end; "
                "p99 %.0f, p99.9 %.0f %s; blocks over 70%% est.: %u)\n",
                unsigned(blocks), unsigned(r.eng->synth().steals()), unsigned(r.eng->governor().cuts()),
                r.eng->governor().cap(), r.eng->governor().budget(), p99, p999, kUnit, unsigned(over));
#ifdef OT_BENCH_M4
    std::printf("    perc.song5 %7u  (PERC slot, worst block of the replay incl. its hits, limit 30000 M4 insn)\n",
                unsigned(percMax));
#endif
    std::printf("  (peak: worst blocks, block cost in %s, phases in %s)\n", kUnit, kStampUnit);
    for (const PeakBlock& b : top)
        if (b.cost > 0.0) printProfile(b);
}

// Per-component cost (one block each, averaged), loud input.
void components() {
    std::printf("  components (per block):\n");
    float in[kBlock], l[kBlock], r[kBlock];
    for (uint32_t i = 0; i < kBlock; ++i) in[i] = 0.3f * std::sin(float(i) * 0.05f);
    const int n = 400 / kScale;
    {
        static Delay d;
        d.init(gFx);
        d.setTimeSamples(16000);
        d.setFeedbackK(128);
        const double t0 = nowUnits();
        for (int b = 0; b < n; ++b) d.process(in, l, r);
        std::printf("    delay    %10.1f\n", (nowUnits() - t0) / n);
    }
    {
        static Reverb v;
        v.init(gFx + Delay::kMemorySamples);
        const double t0 = nowUnits();
        for (int b = 0; b < n; ++b) v.process(in, l, r);
        std::printf("    reverb   %10.1f\n", (nowUnits() - t0) / n);
    }
    {
        static Limiter lim;
        lim.reset();
        lim.setThreshold(0.5f);
        lim.setCoefs(0.01f, 0.001f);
        const double t0 = nowUnits();
        for (int b = 0; b < n; ++b) lim.process(in, nullptr, l);
        std::printf("    limiter  %10.1f\n", (nowUnits() - t0) / n);
    }
    {
        Rig r;
        r.load(0, tone(44100, 220.0f), PlayMode::ForwardLoop);
        r.ins[0].delaySend = 30;
        r.note(0, 0, 60);
        static VoiceOut o;
        float ds[kBlock] = {}, rs[kBlock] = {};
        VoiceControl c;
        Voice& v = r.eng->voice(0);
        double t0 = nowUnits();
        for (int b = 0; b < n; ++b) v.control(c);
        std::printf("    v.ctrl   %10.1f\n", (nowUnits() - t0) / n);
        t0 = nowUnits();
        for (int b = 0; b < n; ++b) v.render(c, o, ds, rs);
        std::printf("    v.render %10.1f  (plain loop voice)\n", (nowUnits() - t0) / n);
        r.ins[0].filterEnabled = 1;
        r.ins[0].cutoff = 0.5f;
        v.control(c);
        t0 = nowUnits();
        for (int b = 0; b < n; ++b) v.render(c, o, ds, rs);
        std::printf("    v.render %10.1f  (+ filter)\n", (nowUnits() - t0) / n);
    }
    {   // the same voice with a stereo instrument
        Rig r;
        r.loadSt(0, tone(44100, 220.0f), tone(44100, 330.0f), PlayMode::ForwardLoop);
        r.ins[0].delaySend = 30;
        r.note(0, 0, 60);
        static VoiceOut o;
        float ds[kBlock] = {}, rs[kBlock] = {};
        static float ml[kBlock], mr[kBlock];
        VoiceControl c;
        Voice& v = r.eng->voice(0);
        v.control(c);
        double t0 = nowUnits();
        for (int b = 0; b < n; ++b) v.render(c, o, ds, rs);
        std::printf("    v.render %10.1f  (stereo loop voice)\n", (nowUnits() - t0) / n);
        t0 = nowUnits();
        for (int b = 0; b < n; ++b) v.render(c, o, ds, rs, ml, mr, false);
        std::printf("    v.render %10.1f  (stereo, fused)\n", (nowUnits() - t0) / n);
        r.ins[0].filterEnabled = 1;
        r.ins[0].cutoff = 0.5f;
        v.control(c);
        t0 = nowUnits();
        for (int b = 0; b < n; ++b) v.render(c, o, ds, rs);
        std::printf("    v.render %10.1f  (stereo + filter)\n", (nowUnits() - t0) / n);
    }
    {   // synth framework overhead: bank render (4 voices) - 4 x engine render
        Rig r;
        r.synth(4);
        SynthSlotRecord rc = SynthSlotRecord::defaults(0);
        rc.model = uint32_t(SynthModel::ACD);
        rc.polyphony = 4;
        rc.reverbSend = 40.0f;
        r.eng->synth().setSlotRecord(0, rc);
        for (int k = 0; k < 4; ++k) r.synthNote(uint8_t(8 + k), 0, uint8_t(48 + 7 * k));
        static float ml[kBlock], mr[kBlock], ds[kBlock], rs[kBlock];
        SynthMixInputs in;
        SynthBank& bank = r.eng->synth();
        double t0 = nowUnits();
        for (int b = 0; b < n; ++b) bank.render(in, ml, mr, ds, rs, nullptr);
        const double all = (nowUnits() - t0) / n;
        ISynthEngine* e = synthTestEngine();
        static SynthVoice v[4];
        SynthNote sn;
        for (int k = 0; k < 4; ++k) {
            v[k] = SynthVoice{};
            sn.pitch = float(48 + 7 * k);
            e->noteOn(v[k], bank.params(0), sn);
        }
        t0 = nowUnits();
        for (int b = 0; b < n; ++b)
            for (int k = 0; k < 4; ++k) e->render(v[k], bank.params(0), 0.5f, 0.0f, ml, mr);
        const double eng = (nowUnits() - t0) / n;
        std::printf("    synth    %10.1f  (4 test voices, engine only)\n", eng);
        std::printf("    synth.fw %10.1f  (framework overhead per block, limit 3000 M4 insn)\n", all - eng);
    }
    {   // PERC: six drums ringing in one slot, retriggered every 32 blocks
        static SynthPatch pp;
        static SynthParams sp;
        static SynthSlotScratch sc;
        static SynthVoice v;
        static float ml[kBlock], mr[kBlock], td[kBlock], tr[kBlock];
        ISynthEngine* e = percEngine();
        double peak = 0.0;
        // 0 = Init kit, 1 = costly models + FX, 2 = Init kit with the send
        // tap the framework supplies (per-drum delay / reverb sends on),
        // 3 = Init kit with every drum send at 0 (five drums ring)
        for (int mode : {1, 0, 2, 3}) {
            const bool worst = mode == 1;
            patchInit(kSynthEnginePerc, &pp);
            if (worst) {
                const float set[][2] = {{perc::kKickWave, 98}, {perc::kTomModel, 2}, {perc::kTomWave, 60},
                                        {perc::kSnareModel, 4}, {perc::kSnareWave, 70}, {perc::kHatModel, 1},
                                        {perc::kHatOpen, 3}, {perc::kCymBodyDecay, 4}, {perc::kPercModel, 1},
                                        {perc::kPercDecay, 2}, {perc::kKickDecay, 2}, {perc::kTomDecay, 2},
                                        {perc::kSnareDecay, 2}, {perc::kSnareSnapDecay, 2}, {perc::kWsLevel, 0.5f},
                                        {perc::kWsType, 1}, {perc::kEqLow, 3}, {perc::kEqMid, -3}, {perc::kEqHigh, 3}};
                for (const auto& kv : set) synthParamSet(pp, int(kv[0]), kv[1]);
            }
            if (mode == 3)
                for (int d = 0; d < 6; ++d) synthParamSet(pp, perc::kMixer + 5 * d + 2, 0.0f);
            if (mode == 2) {
                for (int d = 0; d < 6; ++d) synthParamSet(pp, perc::kMixer + 5 * d + 3, 0.3f);   // reverb sends too
                perc::setSendTap(0, td, tr, 0.5f, 0.5f);
            }
            const uint8_t mv[kSynthSeqMacros] = {};
            sp.engine = kSynthEnginePerc;
            sp.count = synthEffectiveParams(pp, mv, 1.0f, sp.end);
            std::memcpy(sp.start, sp.end, sizeof(sp.end));
            e->slotReset(sc);
            v = SynthVoice{};
            v.slotScratch = &sc;
            const uint8_t notes[6] = {60, 48, 62, 66, 69, 71};
            SynthNote sn;
            double sum = 0.0, pk = 0.0;
            for (int b = 0; b < n; ++b) {
                const double t0 = nowUnits();
                if ((b & 31) == 0)
                    for (int k = 0; k < 6; ++k) {
                        sn.note = notes[k];
                        sn.pitch = float(notes[k]);
                        sn.reused = k > 0 || b > 0;
                        e->noteOn(v, sp, sn);
                    }
                e->render(v, sp, 1.0f, 0.0f, ml, mr);
                const double c = nowUnits() - t0;
                sum += c;
                if (c > pk) pk = c;
            }
            perc::setSendTap(0, nullptr, nullptr);
            if (pk > peak) peak = pk;
            static const char* const kName[4] = {"perc.def", "perc    ", "perc.tap", "perc.dry"};
            static const char* const kWhat[4] = {", Init kit", ", costly models + FX", ", Init kit + send tap",
                                                 ", Init kit, no sends"};
            std::printf("    %s %10.1f  (6 drums hit%s, limit 30000 M4 insn)\n", kName[mode], sum / n, kWhat[mode]);
        }
        std::printf("    perc.pk  %10.1f  (PERC peak block incl. the hits, limit 30000 M4 insn)\n", peak);
    }
    {   // ACD engine, one voice: Init patch (sustain held) and the worst case
        using namespace acd;
        ISynthEngine* e = synthEngineFor(kSynthEngineAcd);
        static SynthSlotScratch sc;
        static SynthVoice v;
        static SynthParams p;
        static float ml[kBlock], mr[kBlock];
        for (int worst = 0; worst < 2; ++worst) {
            patchInit(kSynthEngineAcd, &gSynthPatch);
            const uint8_t m[kSynthSeqMacros] = {};
            p.engine = kSynthEngineAcd;
            p.count = synthEffectiveParams(gSynthPatch, m, 1.0f, p.end);
            p.end[kAmpS] = 1.0f;   // keep the voice sounding for the whole run
            if (worst) {
                // Every oscillator, pre-filter noise, the RD3 ladder, all
                // modulation routes, S&H LFO retriggered per note, glide.
                const float w[][2] = {{kSquareMix, 1}, {kSubMix, 1}, {kSubMode, 1}, {kNoiseMix, 0.5f}, {kPwLfo, 0.3f},
                                      {kPwEnv, 0.2f}, {kPitchLfo, 0.05f}, {kFilterType, kRd3}, {kResonance, 1.2f},
                                      {kFilterLfo, 0.2f}, {kFilterEnv, 0.3f}, {kModS, 0.5f}, {kLfoHz, 30.0f},
                                      {kLfoWave, kSampleHold}, {kLfoRetrig, kNote}, {kGlideTime, 3.0f}};
                for (auto& x : w) p.end[int(x[0])] = x[1];
            }
            std::memcpy(p.start, p.end, sizeof(p.end));
            p.serial = uint32_t(worst + 1);
            e->slotReset(sc);
            v = SynthVoice{};
            v.slotScratch = &sc;
            SynthNote sn;
            sn.pitch = 36.0f;
            e->noteOn(v, p, sn);
            sn.pitch = 60.0f;
            sn.reused = true;
            e->noteOn(v, p, sn);   // glide 36 -> 60 (worst case)
            const double t0 = nowUnits();
            for (int b = 0; b < n; ++b) {
                e->prepare(sc, p);
                e->render(v, p, 0.5f, 0.0f, ml, mr);
            }
            std::printf("    acd.%s %8.1f  (one ACD voice, limit %d M4 insn)\n", worst ? "max " : "init",
                        (nowUnits() - t0) / n, worst ? 18000 : 12000);
        }
    }
    {   // mono fused for comparison
        Rig r;
        r.load(0, tone(44100, 220.0f), PlayMode::ForwardLoop);
        r.ins[0].delaySend = 30;
        r.note(0, 0, 60);
        static VoiceOut o;
        float ds[kBlock] = {}, rs[kBlock] = {};
        static float ml[kBlock], mr[kBlock];
        VoiceControl c;
        Voice& v = r.eng->voice(0);
        v.control(c);
        const double t0 = nowUnits();
        for (int b = 0; b < n; ++b) v.render(c, o, ds, rs, ml, mr, false);
        std::printf("    v.render %10.1f  (mono, fused)\n", (nowUnits() - t0) / n);
    }
}

#ifdef OT_BENCH_M4
// The device's memcpy / memmove / memset (src/hal/fast_mem.c, linked here
// too) against byte loops, on the ARM code paths (LDM/STM blocks, unaligned
// word loads, backward moves). A failure stops the bench before the
// scenario lines, so make perf-gate fails.
bool memCheck() {
    static uint8_t a[1024], b[1024];
    volatile uint32_t seed = 1;
    int bad = 0;
    auto fill = [&](uint8_t* p, uint32_t k) {
        uint32_t x = seed + k;
        for (int i = 0; i < 1024; ++i) {
            x = x * 1664525u + 1013904223u;
            p[i] = uint8_t(x >> 24);
        }
    };
    const uint32_t lens[] = {0, 1, 3, 4, 7, 8, 15, 16, 31, 32, 33, 64, 100, 255, 300};
    for (uint32_t n : lens)
        for (uint32_t da = 0; da < 4; ++da)
            for (uint32_t sa = 0; sa < 4; ++sa) {
                fill(a, n + da * 4 + sa);
                for (int i = 0; i < 1024; ++i) b[i] = a[i];
                std::memcpy(a + 8 + da, a + 512 + sa, n);
                for (uint32_t i = 0; i < n; ++i) b[8 + da + i] = b[512 + sa + i];
                for (int sh : {-9, -4, -1, 1, 3, 4, 40}) {
                    std::memmove(a + 200 + da + sh, a + 200 + sa, n);
                    uint8_t t[300];
                    for (uint32_t i = 0; i < n; ++i) t[i] = b[200 + sa + i];
                    for (uint32_t i = 0; i < n; ++i) b[200 + da + sh + i] = t[i];
                }
                std::memset(a + 700 + da, int(n + sa), n);
                for (uint32_t i = 0; i < n; ++i) b[700 + da + i] = uint8_t(n + sa);
                for (int i = 0; i < 1024; ++i) bad += a[i] != b[i];
            }
    if (bad) std::printf("  mem* check FAILED (%d bytes differ): src/hal/fast_mem.c\n", bad);
    return bad == 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    static int16_t zeros[kBlock * 2] = {};
    static int16_t lineNoise[kBlock * 2];
    uint32_t s = 3;
    for (auto& v : lineNoise) {
        s = s * 1103515245u + 12345u;
        v = int16_t(int32_t(s >> 16) % 2000);
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    clockStart();
#ifdef OT_BENCH_PEAK_ONLY
    // make -C tests bench-m4-hot: the song 5 replay alone, profiled per
    // translation block by tests/bench_m4/hot_plugin.c.
    peakBench("../build/bench/song5.trace", 3000);
    return 0;
#endif
#ifdef OT_BENCH_M4
    std::printf("audio_bench (cortex-m4 -O2 under QEMU, instruction counts):\n");
    if (!memCheck()) return 1;
    if (!constTablesMatch()) {   // ARM const tables vs this toolchain's libm (audio/audio_tables.h)
        std::printf("  const audio tables differ from computeConstTables(): run make -C tests audio-tables\n");
        return 1;
    }
#else
    std::printf("audio_bench (host, -O2):\n");
#endif

#ifndef OT_BENCH_PROFILE
    {   // idle: fresh engine, then also after a played-out tail
        Rig r;
        report("idle", timeBlocks(r, 4000, zeros));
        r.load(0, tone(30000, 220.0f), PlayMode::OneShot);
        r.ins[0].delaySend = 60;
        r.ins[0].reverbSend = 60;
        r.note(0, 0, 60);
        timeBlocks(r, 1, zeros);
        int16_t out[kBlock * 2];
        int tail = 0;
        for (; tail < 6000; ++tail) {   // until the output is exactly silent for 1 s
            r.eng->setLineInput(zeros);
            r.eng->renderLive(out);
            static int quiet = 0;
            bool z = true;
            for (int16_t v : out) z &= v == 0;
            quiet = z ? quiet + 1 : 0;
            if (quiet > 345) break;
        }
        std::printf("  (tail of a delay/reverb note silent after %d blocks)\n", tail);
        // keep rendering silence a while so effect bypass can engage
        for (int b = 0; b < 3000 / kScale; ++b) {
            r.eng->setLineInput(zeros);
            r.eng->renderLive(out);
        }
        report("idle2", timeBlocks(r, 4000, zeros));
    }
#endif
    {   // typical 8-voice playback with delay + reverb
        Rig r;
        for (int i = 0; i < 4; ++i) {
            r.load(i, tone(44100, 110.0f * float(i + 1)), PlayMode::ForwardLoop);
            r.ins[i].delaySend = uint8_t(20 + 10 * i);
            r.ins[i].reverbSend = uint8_t(30 + 5 * i);
        }
        r.ins[1].filterEnabled = 1;
        r.ins[1].cutoff = 0.5f;
        r.ins[2].env[kAutoPanning].enabled = 1;
        r.ins[2].env[kAutoPanning].loop = 1;
        r.eng->settings().delayTimeMs = 375;
        r.eng->settings().delayFeedback = 45;
        for (int t = 0; t < 8; ++t) r.note(uint8_t(t), uint8_t(t & 3), uint8_t(48 + 3 * t));
        gPlay8 = timeBlocks(r, 2000, lineNoise);
        report("play8", gPlay8);
#ifdef OT_BENCH_PROFILE
        return 0;
#endif
    }
    {   // stereo8: the play8 set with stereo instruments (right = other tone)
        Rig r;
        for (int i = 0; i < 4; ++i) {
            r.loadSt(i, tone(44100, 110.0f * float(i + 1)), tone(44100, 165.0f * float(i + 1)), PlayMode::ForwardLoop);
            r.ins[i].delaySend = uint8_t(20 + 10 * i);
            r.ins[i].reverbSend = uint8_t(30 + 5 * i);
        }
        r.ins[1].filterEnabled = 1;
        r.ins[1].cutoff = 0.5f;
        r.ins[2].env[kAutoPanning].enabled = 1;
        r.ins[2].env[kAutoPanning].loop = 1;
        r.eng->settings().delayTimeMs = 375;
        r.eng->settings().delayFeedback = 45;
        for (int t = 0; t < 8; ++t) r.note(uint8_t(t), uint8_t(t & 3), uint8_t(48 + 3 * t));
        const double st = timeBlocks(r, 2000, lineNoise);
        gStereo8 = st;
        report("stereo8", st);
        std::printf("  (stereo8 = play8 %+.1f%%; limit +24%%, docs/MINI_PLAN.md §2)\n", 100.0 * (st / gPlay8 - 1.0));
    }
    {   // stream8: stereo8 with streamed 3 s instruments
        Rig r;
        constexpr uint32_t kSec3 = 3 * 44100;
        static StreamSource src[4];
        for (int i = 0; i < 4; ++i) {
            r.loadSt(i, tone(kSec3, 110.0f * float(i + 1)), tone(kSec3, 165.0f * float(i + 1)), PlayMode::ForwardLoop);
            r.ins[i].delaySend = uint8_t(20 + 10 * i);
            r.ins[i].reverbSend = uint8_t(30 + 5 * i);
            src[i] = StreamSource{};
            src[i].headL = r.eng->bank().left(i);
            src[i].headR = r.eng->bank().right(i);
            src[i].headFrames = kStreamHeadFrames;
            src[i].frames = kSec3;
            src[i].instrument = uint8_t(i);
        }
        r.ins[1].filterEnabled = 1;
        r.ins[1].cutoff = 0.5f;
        r.ins[2].env[kAutoPanning].enabled = 1;
        r.ins[2].env[kAutoPanning].loop = 1;
        r.eng->settings().delayTimeMs = 375;
        r.eng->settings().delayFeedback = 45;
        static StreamRing rings[kVoices];
        for (int v = 0; v < kVoices; ++v) {
            rings[v] = StreamRing{};
            rings[v].bufL = gRingMem + size_t(2 * v) * kStreamRingFrames;
            rings[v].bufR = rings[v].bufL + kStreamRingFrames;
        }
        r.eng->setStreamRings(rings);
        for (int i = 0; i < 4; ++i) r.eng->setStreamSource(i, &src[i]);
        // The card, here: the full instrument is in the bank.
        struct BankReader : IStreamReader {
            AudioEngine* eng = nullptr;
            bool read(const StreamSource& s, uint32_t f, uint32_t n, int16_t* l, int16_t* rr) override {
                std::memcpy(l, eng->bank().left(s.instrument) + f, n * 2);
                if (rr) std::memcpy(rr, eng->bank().right(s.instrument) + f, n * 2);
                return true;
            }
        } reader;
        reader.eng = r.eng;
        for (int t = 0; t < 8; ++t) r.note(uint8_t(t), uint8_t(t & 3), uint8_t(48 + 3 * t));
        // Run well into the ring (past the head) before timing.
        int16_t out[kBlock * 2];
        for (int b = 0; b < 400; ++b) {
            streamService(rings, kVoices, reader, 4);
            r.eng->setLineInput(lineNoise);
            r.eng->renderLive(out);
        }
        double best = 1e30;
        const int n = 2000 / kScale;
        uint32_t under0 = 0;
        for (int v = 0; v < kVoices; ++v) under0 += rings[v].underruns;
        for (int rep = 0; rep < kReps; ++rep) {
            double sum = 0.0;
            for (int b = 0; b < n; ++b) {
                streamService(rings, kVoices, reader, 4);   // main loop: not timed
                r.eng->setLineInput(lineNoise);
                const double t0 = nowUnits();
                r.eng->renderLive(out);
                sum += nowUnits() - t0;
                gSink = out[b & 255];
            }
            if (sum / n < best) best = sum / n;
        }
        uint32_t under = 0;
        for (int v = 0; v < kVoices; ++v) under += rings[v].underruns;
        report("stream8", best);
        std::printf("  (stream8 = stereo8 %+.1f%%, underruns %u; limit +5%%, docs/PLAN_SD_STREAMING.md P1)\n",
                    100.0 * (best / gStereo8 - 1.0), unsigned(under - under0));
#ifdef OT_BENCH_M4
        std::printf("    stream.lim %10.1f  (stream8 per block, limit %.0f M4 insn = stereo8 + 5 %%)\n", best,
                    gStereo8 * 1.05);
#endif
        if (under != under0) std::printf("    stream.und %u  (underruns during the run, limit 0)\n", unsigned(under - under0));
        r.eng->setStreamRings(nullptr);
        for (int i = 0; i < 4; ++i) r.eng->setStreamSource(i, nullptr);
    }
    {   // worst case
        Rig r;
        auto noise = tone(88200, 330.0f);
        r.load(0, noise, PlayMode::Granular);
        r.load(1, noise, PlayMode::PingPongLoop);
        std::vector<int16_t> wt(2048 * 8);
        for (size_t i = 0; i < wt.size(); ++i)
            wt[i] = int16_t(20000.0f * std::sin(2.0f * kPi * float(i % 2048) / 2048.0f * float(1 + i / 2048)));
        r.load(2, wt, PlayMode::Wavetable);
        r.ins[2].wavetableWindowSize = 2048;
        r.ins[2].wavetableWindowCount = 8;
        for (int i = 0; i < 3; ++i) {
            r.ins[i].filterEnabled = 1;
            r.ins[i].filterType = uint8_t(i % 3);
            r.ins[i].resonance = 1.0f;
            r.ins[i].overdrive = 40;
            r.ins[i].bitDepth = 12;
            r.ins[i].delaySend = 50;
            r.ins[i].reverbSend = 50;
            for (int k = 1; k < kAutomationCount; ++k) {
                r.ins[i].env[k].enabled = 1;
                r.ins[i].env[k].loop = 1;
            }
        }
        MixerSettings& m = r.eng->settings();
        m.sidechain = 1;
        m.saturationMode = 3;
        m.delayPingPong = true;
        m.lineInDelaySend = 50;
        m.lineInReverbSend = 50;
        for (int t = 0; t < 8; ++t) r.note(uint8_t(t), uint8_t(t % 3), uint8_t(50 + t));
        r.eng->metronomeClick(true, 100);
        r.eng->previewBuffer(noise.data(), uint32_t(noise.size()), false);
        report("worst", timeBlocks(r, 2000, lineNoise));
    }
    {   // synth4: 4 test-engine voices in Synth 1 (sustained), reverb send
        Rig r;
        r.synth(4);
        SynthSlotRecord rc = SynthSlotRecord::defaults(0);
        rc.model = uint32_t(SynthModel::ACD);
        rc.polyphony = 4;
        rc.reverbSend = 40.0f;
        r.eng->synth().setSlotRecord(0, rc);
        for (int k = 0; k < 4; ++k) r.synthNote(uint8_t(8 + k), 0, uint8_t(48 + 7 * k));
        report("synth4", timeBlocks(r, 2000, lineNoise));
        std::printf("  (synth voices sounding after the run: %d)\n", r.eng->synth().sounding());
    }
    for (int i = 0; i < kBlock * 2; ++i) gLineNoise[i] = lineNoise[i];
    mixBench(4, "mix");   // Governor::kDefaultBudget
    mixBench(5, "mix5");
    mixBench(6, "mix6");
    mixBench(4, "mixfat", kSynthEngineFat);
    mixBench(4, "mixwtfm", kSynthEngineWtfm);
    switchBench();
#ifdef OT_BENCH_M4
    peakBench("../build/bench/song5.trace", 8100);   // song positions 0-2 (intro, groove, sixteen-track chords)
#else
    peakBench(argc > 1 ? argv[1] : "../build/bench/song5.trace", 0xFFFFFFFFu);
#endif
    reportVoice("vapinit", vapVoice(false), 12000);
    reportVoice("vapworst", vapVoice(true), 18000);
    reportVoice("fatinit", fatVoice(false), 12000);
    reportVoice("fatworst", fatVoice(true), 22000);
    reportVoice("wtfminit", wtfmVoice(false), 12000);
    reportVoice("wtfmworst", wtfmVoice(true), 32000);
    components();
    return 0;
}
