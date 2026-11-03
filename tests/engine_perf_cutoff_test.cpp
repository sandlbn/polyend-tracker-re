// Issue #23: the Performance filter offset (SynthVoice::cutoffOct, octaves)
// moves each synth engine's own cutoff. A patch at 1600 Hz shifted by -2
// octaves sounds like the same patch at 400 Hz; PERC has no cutoff and is
// unchanged.
#include "test.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "audio/engines/acd.h"
#include "audio/engines/fat.h"
#include "audio/engines/vap.h"
#include "audio/engines/wtfm.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"

using namespace ot;
using namespace ot::audio;

namespace {

// Renders one note (pitch 48) for 40 blocks: patch defaults, cutoff index
// `ci` set to `hz` (ci < 0: untouched), the voice's cutoffOct = `oct`.
std::vector<float> play(int32_t engine, int ci, float hz, float oct) {
    ISynthEngine* e = synthEngineFor(engine);
    SynthPatch patch;
    patchInit(engine, &patch);
    if (ci >= 0) synthParamSet(patch, ci, hz);
    SynthParams p;
    const uint8_t m[kSynthSeqMacros] = {};
    p.engine = engine;
    p.count = synthEffectiveParams(patch, m, 1.0f, p.end);
    std::memcpy(p.start, p.end, sizeof(p.end));
    p.serial = 1;
    static SynthSlotScratch scratch;
    static SynthVoice v;
    scratch = SynthSlotScratch{};
    e->slotReset(scratch);
    v = SynthVoice{};
    v.slotScratch = &scratch;
    v.cutoffOct = oct;
    SynthNote n;
    n.pitch = 48.0f;
    n.note = 48;
    v.note = 48;
    v.gate = true;
    e->noteOn(v, p, n);
    std::vector<float> out;
    float L[kBlock], R[kBlock];
    for (int b = 0; b < 40; ++b) {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = 0.0f;
        e->prepare(scratch, p);
        e->render(v, p, 1.0f, 0.0f, L, R);
        out.insert(out.end(), L, L + kBlock);
    }
    return out;
}

double energy(const std::vector<float>& x) {
    double s = 0;
    for (float f : x) s += double(f) * f;
    return s;
}

// Relative difference of two renders (0 = identical).
double relDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double d = 0;
    for (size_t i = 0; i < a.size(); ++i) d += double(a[i] - b[i]) * (a[i] - b[i]);
    return std::sqrt(d / (energy(a) + 1e-30));
}

void checkEngine(int32_t engine, int ci) {
    const std::vector<float> ref = play(engine, ci, 400.0f, 0.0f);
    CHECK(energy(ref) > 1e-6);
    // The same cutoff reached by the offset: close to the 400 Hz render.
    CHECK(relDiff(ref, play(engine, ci, 1600.0f, -2.0f)) < 0.05);
    CHECK(relDiff(play(engine, ci, 6400.0f, 0.0f), play(engine, ci, 400.0f, 4.0f)) < 0.05);
    // The offset changes the sound, and 0 is the plain patch.
    CHECK(relDiff(ref, play(engine, ci, 400.0f, -3.0f)) > 0.1);
    CHECK(relDiff(ref, play(engine, ci, 400.0f, 0.0f)) == 0.0);
    // The full range stays finite (the engine's own clamps).
    for (float o : {-9.54f, 9.54f}) {
        const std::vector<float> x = play(engine, ci, o > 0 ? 20000.0f : 20.0f, o);
        bool finite = true;
        for (float f : x) finite = finite && std::isfinite(f);
        CHECK(finite);
    }
}

}  // namespace

TEST(acd_perf_cutoff_offset_moves_the_cutoff) { checkEngine(kSynthEngineAcd, acd::kCutoff); }
TEST(fat_perf_cutoff_offset_moves_the_cutoff) { checkEngine(kSynthEngineFat, fat::kCutoff); }
TEST(vap_perf_cutoff_offset_moves_the_cutoff) { checkEngine(kSynthEngineVap, vap::kCutoff); }
TEST(wtfm_perf_cutoff_offset_moves_the_cutoff) { checkEngine(kSynthEngineWtfm, wtfm::kCutoff); }

TEST(perc_ignores_the_perf_cutoff_offset) {
    const std::vector<float> a = play(kSynthEnginePerc, -1, 0.0f, 0.0f);
    CHECK(relDiff(a, play(kSynthEnginePerc, -1, 0.0f, -4.0f)) == 0.0);
}
