// [I] Synth mode sweep for the hot-code lists (shared SRAM_L pool):
// make -C tests bench-m4-synth-modes PROFILE=allsynths.
//
// Every synth engine of the profile's modules plays its Init patch, a run of
// notes across the keyboard, its continuous parameters all at their minimum
// and all at their maximum, and every value of each list / on-off parameter
// (algorithm, waveform, filter type, ...) on its own. Each run is one phase:
// it prints "hotphase <k> <engine id> <name> <what>" and calls otHotPhase(),
// where tests/bench_m4/hot_plugin.c (phase=<address>) starts a new count.
// tools/synth_hot.py --modes takes each phase's hottest functions and joins
// them per module, so a mode the Init patch never plays has its code listed.
#include <cstdio>
#include <cstring>

#include "audio/synth_engine.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "modules/module.h"
#include "modules/synth_descriptor.h"

extern "C" __attribute__((noinline, used)) void otHotPhase() { __asm__ volatile("" ::: "memory"); }

namespace {

using namespace ot;
using namespace ot::audio;

constexpr int kVoices = 8;
constexpr int kMaxValues = 64;   // list values swept per parameter

SynthPatch g_init;
SynthPatch g_patch;
SynthParams g_p;
SynthSlotScratch g_sc;
SynthVoice g_v[kVoices];
bool g_alive[kVoices];
float g_l[kBlock], g_r[kBlock];
int g_phase = 0;

void block(ISynthEngine* e) {
    e->setTempo(120.0f);
    e->prepare(g_sc, g_p);
    for (int k = 0; k < kVoices; ++k)
        if (g_alive[k]) g_alive[k] = e->render(g_v[k], g_p, 0.5f, 0.0f, g_l, g_r);
}

// One phase: g_patch's notes one after the other (each on its own voice,
// held `hold` blocks), then their release.
void play(ISynthEngine* e, const mod::SynthDescriptor& d, const char* what, const int* notes, int count, int hold) {
    std::printf("hotphase %d %ld %s %s\n", g_phase++, long(d.engineId), d.name, what);
    otHotPhase();
    uint8_t m[kSynthSeqMacros];
    for (int k = 0; k < kSynthSeqMacros; ++k) m[k] = synthMacroDefault(g_patch.macros[k].bipolar);
    g_p = SynthParams{};
    g_p.engine = d.engineId;
    g_p.count = synthEffectiveParams(g_patch, m, 1.0f, g_p.end);
    std::memcpy(g_p.start, g_p.end, sizeof g_p.end);
    g_p.serial = uint32_t(g_phase);
    e->slotReset(g_sc);
    e->loadPatch(g_sc, g_patch);
    for (int k = 0; k < kVoices; ++k) {
        g_v[k] = SynthVoice{};
        g_v[k].slotScratch = &g_sc;
        g_alive[k] = false;
    }
    for (int i = 0; i < count; ++i) {
        SynthVoice& v = g_v[i % kVoices];
        v.gate = true;
        v.note = uint8_t(notes[i]);
        SynthNote n;
        n.note = uint8_t(notes[i]);
        n.pitch = float(notes[i]);
        e->noteOn(v, g_p, n);
        g_alive[i % kVoices] = true;
        for (int b = 0; b < hold; ++b) block(e);
    }
    for (int k = 0; k < kVoices; ++k)
        if (g_alive[k] && g_v[k].gate) {
            e->noteOff(g_v[k], g_p);
            g_v[k].gate = false;
        }
    for (int b = 0; b < 24; ++b) block(e);
}

bool isList(const synthui::ParamUi& u) { return u.fmt == synthui::kFmtEnum || u.fmt == synthui::kFmtOnOff; }

void sweep(const mod::SynthDescriptor& d) {
    ISynthEngine* e = synthEngineFor(d.engineId);
    if (!e || !patchInit(d.engineId, &g_init)) return;
    static const int kOne[] = {60};
    static const int kRun[] = {36, 48, 55, 60, 62, 64, 67, 69, 71, 72, 79, 84};
    g_patch = g_init;
    play(e, d, "init", kOne, 1, 96);
    play(e, d, "notes", kRun, int(sizeof kRun / sizeof kRun[0]), 6);
    for (int end = 0; end < 2; ++end) {   // continuous parameters at min, then max
        g_patch = g_init;
        for (int i = 0; i < d.paramCount; ++i)
            if (!(d.ui[i].label && isList(d.ui[i]))) synthParamSet(g_patch, i, end ? d.params[i].max : d.params[i].min);
        play(e, d, end ? "max" : "min", kOne, 1, 48);
    }
    for (int i = 0; i < d.paramCount; ++i) {
        if (!d.ui[i].label || !isList(d.ui[i])) continue;
        const int lo = int(d.params[i].min), hi = int(d.params[i].max);
        for (int v = lo; v <= hi && v - lo < kMaxValues; ++v) {
            g_patch = g_init;
            synthParamSet(g_patch, i, float(v));
            char what[48];
            std::snprintf(what, sizeof what, "%s=%d", d.ui[i].label, v);
            for (char* c = what; *c; ++c)
                if (*c == ' ') *c = '_';
            play(e, d, what, kOne, 1, 32);
        }
    }
}

}  // namespace

void synthModesBench() {
    mod::ModuleHost& h = mod::ModuleHost::instance();
    h.init();
    for (int k = 0; k < h.engineCount(); ++k)
        if (const mod::SynthDescriptor* d = h.engineAt(k)) sweep(*d);
    std::printf("hotphase %d 0 end end\n", g_phase);
    otHotPhase();
}
