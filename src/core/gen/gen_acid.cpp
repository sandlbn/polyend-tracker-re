// Acid generator: a TB-3PO style 303 line (own implementation of the idea:
// per-step density, root bias on the beat, scale degree + octave, accent and
// slide chances; a slide into a rest becomes a plain note; at least one note).
#include <cstdio>

#include "gen/gen_internal.h"

namespace ot {
namespace gen {

void genAcid(const int16_t* v, Target& t, Rng& rng, Result& r) {
    const float density = v[0] / 100.0f, accent = v[1] / 100.0f, slide = v[2] / 100.0f;
    const int octaves = v[3] + 1;
    uint8_t deg[12];
    const int nd = scaleDegrees(t.scaleMask, deg);
    const int n = t.s1 - t.s0 + 1;
    MonoStep ph[kStepCount];
    bool any = false;
    for (int i = 0; i < n; ++i) {
        MonoStep& m = ph[i];
        m = MonoStep{};
        if (rng.uni() > density) continue;
        const bool onBeat = ((t.s0 + i) % t.stepsPerBeat) == 0;
        const int d = (onBeat && rng.coin(0.35f)) ? 0 : rng.below(nd);
        const int oct = rng.below(octaves);
        m.active = true;
        m.note = degreeNote(t.rootNote, deg, nd, d + oct * nd);
        m.gate = 0.5f;
        m.accent = rng.coin(accent);
        m.slide = rng.coin(slide);
        any = true;
    }
    // A slide into a rest is a plain note; the last step slides into the loop
    // start like TB-3PO's pattern wrap.
    for (int i = 0; i < n; ++i) {
        const MonoStep& nx = ph[(i + 1) % n];
        if (ph[i].slide && !nx.active) ph[i].slide = false;
    }
    if (!any) {
        ph[0].active = true;
        ph[0].note = t.rootNote;
        ph[0].gate = 0.5f;
    }
    for (int tr = t.t0; tr <= t.t1; ++tr) writeMono(*t.pat, tr, t.s0, ph, n, t.instrument);
    int notes = 0;
    for (int i = 0; i < n; ++i) notes += ph[i].active;
    std::snprintf(r.msg, sizeof(r.msg), "Acid: %d notes in %d steps", notes, n);
    r.ok = true;
}

}  // namespace gen
}  // namespace ot
