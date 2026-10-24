// Acid generator: a TB-3PO style 303 line (own implementation of the idea:
// per-step density, root bias on the beat, scale degree + octave, accent and
// slide chances; a slide into a rest becomes a plain note; at least one note),
// and its live mutation (each loop re-rolls some steps' state or pitch).
#include <cstdio>

#include "gen/gen_internal.h"

namespace ot {
namespace gen {

namespace {

struct AcidCfg {
    float density, accent, slide;
    int octaves;
    uint8_t deg[12];
    int nd;
};

AcidCfg cfg(const int16_t* v, const Target& t) {
    AcidCfg c;
    c.density = v[0] / 100.0f;
    c.accent = v[1] / 100.0f;
    c.slide = v[2] / 100.0f;
    c.octaves = v[3] + 1;
    c.nd = scaleDegrees(t.scaleMask, c.deg);
    return c;
}

void rollState(const AcidCfg& c, Rng& rng, MonoStep& m) {
    m.active = !(rng.uni() > c.density);
    m.accent = rng.coin(c.accent);
    m.slide = rng.coin(c.slide);
    m.gate = 0.5f;
}

void rollPitch(const AcidCfg& c, const Target& t, Rng& rng, MonoStep& m, bool onBeat) {
    const int d = (onBeat && rng.coin(0.35f)) ? 0 : rng.below(c.nd);
    m.note = degreeNote(t.rootNote, c.deg, c.nd, d + rng.below(c.octaves) * c.nd);
}

void cleanup(MonoStep* ph, int n, int root) {
    bool any = false;
    for (int i = 0; i < n; ++i) {
        if (ph[i].slide && !ph[(i + 1) % n].active) ph[i].slide = false;   // slide into a rest
        any = any || ph[i].active;
    }
    if (!any && n > 0) {
        ph[0].active = true;
        ph[0].note = root;
        ph[0].gate = 0.5f;
    }
}

}  // namespace

void acidPhrase(const int16_t* v, const Target& t, Rng& rng, MonoStep* ph, int n) {
    const AcidCfg c = cfg(v, t);
    for (int i = 0; i < n; ++i) {
        MonoStep& m = ph[i];
        m = MonoStep{};
        if (rng.uni() > c.density) continue;
        m.active = true;
        rollPitch(c, t, rng, m, ((t.s0 + i) % t.stepsPerBeat) == 0);
        m.gate = 0.5f;
        m.accent = rng.coin(c.accent);
        m.slide = rng.coin(c.slide);
    }
    cleanup(ph, n, t.rootNote);
}

void acidMutate(const int16_t* v, const Target& t, Rng& rng, MonoStep* ph, int n, float amount) {
    const AcidCfg c = cfg(v, t);
    for (int i = 0; i < n; ++i) {
        if (!rng.coin(amount)) continue;
        if (rng.coin(0.5f)) {
            const bool was = ph[i].active;
            rollState(c, rng, ph[i]);
            if (ph[i].active && !was) rollPitch(c, t, rng, ph[i], ((t.s0 + i) % t.stepsPerBeat) == 0);
        } else {
            rollPitch(c, t, rng, ph[i], ((t.s0 + i) % t.stepsPerBeat) == 0);
        }
    }
    cleanup(ph, n, t.rootNote);
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
