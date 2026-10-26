// Acid generator: a TB-3PO style 303 line (own implementation of the idea).
// Every step of the phrase gets a pitch (scale degree + octave, root bias on
// the beat), an accent and a slide wish, and a threshold thr in [0, 1): the
// step plays while thr < the density probability (density -7..+7 ->
// (d + 8) / 16). So density can change live (Perform FX "Acid Density")
// without changing the melody: notes come and go in a fixed order. A slide
// only happens into a step that plays. The live mutation re-rolls some steps'
// threshold or pitch at each loop end (live_gen.cpp).
#include <cstdio>

#include "gen/gen_internal.h"

namespace ot {
namespace gen {

namespace {

struct AcidCfg {
    float accent, slide, gate, root;
    int octaves;
    uint8_t deg[12];
    int nd;
};

AcidCfg cfg(const int16_t* v, const Target& t) {
    static const float kGate[3] = {0.5f, 0.3f, 0.8f};
    AcidCfg c;
    c.accent = v[1] / 100.0f;
    c.slide = v[2] / 100.0f;
    c.octaves = v[3] + 1;
    c.gate = kGate[v[7] < 0 || v[7] > 2 ? 0 : v[7]];
    c.root = v[8] / 100.0f;
    c.nd = scaleDegrees(t.scaleMask, c.deg);
    return c;
}

void rollPitch(const AcidCfg& c, const Target& t, Rng& rng, MonoStep& m, bool onBeat) {
    const int d = (onBeat && rng.coin(c.root)) ? 0 : rng.below(c.nd);
    m.note = degreeNote(t.rootNote, c.deg, c.nd, d + rng.below(c.octaves) * c.nd);
}

void rollState(const AcidCfg& c, Rng& rng, MonoStep& m) {
    m.thr = rng.uni();
    m.accent = rng.coin(c.accent);
    m.slideWant = rng.coin(c.slide);
    m.gate = c.gate;
}

}  // namespace

float acidDensityProb(int d) {
    d = d < -7 ? -7 : (d > 7 ? 7 : d);
    return float(d + 8) / 16.0f;
}

void acidApplyDensity(MonoStep* ph, int n, int density, int root) {
    (void)root;
    const float p = acidDensityProb(density);
    bool any = false;
    int lowest = 0;
    for (int i = 0; i < n; ++i) {
        ph[i].active = ph[i].thr < p;
        any = any || ph[i].active;
        if (ph[i].thr < ph[lowest].thr) lowest = i;
    }
    if (!any && n > 0) ph[lowest].active = true;   // at least one note
    for (int i = 0; i < n; ++i) ph[i].slide = ph[i].active && ph[i].slideWant && ph[(i + 1) % n].active;
}

void acidPhrase(const int16_t* v, const Target& t, Rng& rng, MonoStep* ph, int n) {
    const AcidCfg c = cfg(v, t);
    for (int i = 0; i < n; ++i) {
        MonoStep& m = ph[i];
        m = MonoStep{};
        rollState(c, rng, m);
        rollPitch(c, t, rng, m, ((t.s0 + i) % t.stepsPerBeat) == 0);
    }
    acidApplyDensity(ph, n, v[0], t.rootNote);
}

void acidMutate(const int16_t* v, const Target& t, Rng& rng, MonoStep* ph, int n, float amount) {
    const AcidCfg c = cfg(v, t);
    for (int i = 0; i < n; ++i) {
        if (!rng.coin(amount)) continue;
        if (rng.coin(0.5f)) rollState(c, rng, ph[i]);
        else rollPitch(c, t, rng, ph[i], ((t.s0 + i) % t.stepsPerBeat) == 0);
    }
}

void genAcid(const int16_t* v, Target& t, Rng& rng, Result& r) {
    const int n = t.s1 - t.s0 + 1;
    MonoStep ph[kStepCount];
    acidPhrase(v, t, rng, ph, n);
    for (int tr = t.t0; tr <= t.t1; ++tr) writeMono(*t.pat, tr, t.s0, ph, n, t.instrument);
    int notes = 0;
    for (int i = 0; i < n; ++i) notes += ph[i].active;
    if (v[4] > 0) std::snprintf(r.msg, sizeof(r.msg), "Acid: %d notes, mutating %d%%", notes, v[4]);
    else std::snprintf(r.msg, sizeof(r.msg), "Acid: %d notes in %d steps", notes, n);
    r.ok = true;
}

}  // namespace gen
}  // namespace ot
