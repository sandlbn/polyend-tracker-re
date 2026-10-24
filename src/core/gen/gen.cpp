// Generator table, dispatch and shared helpers (gen.h).
#include "gen/gen.h"

#include <cstdio>

#include "gen/gen_internal.h"
#include "model/fx.h"

namespace ot {
namespace gen {

namespace {

const char* const kOctLabels[3] = {"1 oct", "2 oct", "3 oct"};
const char* const kCrLayout[3] = {"Call+Answer", "Answer mine", "Two tracks"};
const char* const kBBSource[6] = {"Auto", "1 beat", "2 beats", "1 bar", "2 bars", "4 bars"};
const char* const kTaPhrase[4] = {"1 bar", "2 bars", "4 bars", "8 bars"};
const char* const kTaStyle[6] = {"Rolls", "Stutter", "Drop", "Reverse", "Build", "Mix"};
const char* const kGtMode[3] = {"Markov", "Ball", "Ball+Markov"};
const char* const kStSource[4] = {"RDS name", "PI code", "Frequency", "All"};
const char* const kStMode[2] = {"Notes", "Slices"};
const char* const kStRate[3] = {"1/16", "1/8", "1/4"};
const char* const kStGroups[2] = {"Off", "Fives"};

KindDesc gKinds[kKindCount] = {
    {"Acid", "303 line: density, accent, slide (TB-3PO style)",
     {{"Density", 0, 100, 70, nullptr},
      {"Accent", 0, 100, 40, nullptr},
      {"Slide", 0, 100, 25, nullptr},
      {"Range", 0, 2, 1, kOctLabels},
      {"Mutate", 0, 100, 0, nullptr},
      {"Seed", 0, 999, 0, nullptr}}},
    {"Call+Resp", "Call phrase, then the answer (Nordl3 C/R Generator)",
     {{"Style", 0, kCrStyleCount - 1, 0, nullptr},
      {"Answer", 0, kCrStrategyCount - 1, 0, nullptr},
      {"Density", 0, 100, 0, nullptr},
      {"Layout", 0, 2, 0, kCrLayout},
      {nullptr, 0, 0, 0, nullptr},
      {"Seed", 0, 999, 0, nullptr}}},
    {"BBCut", "Rearrange a sliced break (BBCut / BBenCut procs)",
     {{"Proc", 0, kBBProcCount - 1, 0, nullptr},
      {"A", 0, 100, 50, nullptr},
      {"B", 0, 100, 50, nullptr},
      {"Source", 0, 5, 0, kBBSource},
      {nullptr, 0, 0, 0, nullptr},
      {"Seed", 0, 999, 0, nullptr}}},
    {"Turnaround", "Phrase-end rolls, stutters and drops on a budget",
     {{"Phrase", 0, 3, 1, kTaPhrase},
      {"Budget", 1, 16, 4, nullptr},
      {"Style", 0, 5, 5, kTaStyle},
      {"Intensity", 0, 100, 60, nullptr},
      {nullptr, 0, 0, 0, nullptr},
      {"Seed", 0, 999, 0, nullptr}}},
    {"Genotype", "Learn the track, play a variation of it",
     {{"Mode", 0, 2, 2, kGtMode},
      {"Identity", 0, 100, 60, nullptr},
      {"Order", 1, 2, 1, nullptr},
      {"Variety", 0, 100, 40, nullptr},
      {nullptr, 0, 0, 0, nullptr},
      {"Seed", 0, 999, 0, nullptr}}},
    {"Station", "Numbers station from the radio (RDS, PI, frequency)",
     {{"Source", 0, 3, 3, kStSource},
      {"Mode", 0, 1, 0, kStMode},
      {"Rate", 0, 2, 1, kStRate},
      {"Groups", 0, 1, 1, kStGroups},
      {"Repeat", 1, 4, 2, nullptr},
      {"Seed", 0, 999, 0, nullptr}}},
};

bool labelsReady = false;
const char* gCrStyleLabels[kCrStyleCount];
const char* gCrStrategyLabels[kCrStrategyCount];
const char* gBBProcLabels[kBBProcCount];

void initLabels() {
    if (labelsReady) return;
    for (int i = 0; i < kCrStyleCount; ++i) gCrStyleLabels[i] = crStyleName(i);
    for (int i = 0; i < kCrStrategyCount; ++i) gCrStrategyLabels[i] = crStrategyName(i);
    for (int i = 0; i < kBBProcCount; ++i) gBBProcLabels[i] = bbProcName(i);
    gKinds[kCallResponse].p[0].labels = gCrStyleLabels;
    gKinds[kCallResponse].p[1].labels = gCrStrategyLabels;
    gKinds[kBBCut].p[0].labels = gBBProcLabels;
    labelsReady = true;
}

}  // namespace

const KindDesc& kind(int k) {
    initLabels();
    return gKinds[k < 0 ? 0 : (k >= kKindCount ? kKindCount - 1 : k)];
}

const ParamDesc& param(int k, int i, const int16_t* values) {
    const KindDesc& d = kind(k);
    if (k == kBBCut && (i == 1 || i == 2) && values) {
        const ParamDesc* bp = bbProcParam(values[0], i - 1);
        static const ParamDesc kNone = {nullptr, 0, 0, 0, nullptr};
        return bp ? *bp : kNone;
    }
    return d.p[i];
}

int scaleDegrees(uint16_t mask, uint8_t out[12]) {
    int n = 0;
    for (int pc = 0; pc < 12; ++pc)
        if (mask & (1u << pc)) out[n++] = uint8_t(pc);
    if (n == 0) {   // no scale: natural minor
        static const uint8_t kMinor[7] = {0, 2, 3, 5, 7, 8, 10};
        for (int i = 0; i < 7; ++i) out[i] = kMinor[i];
        n = 7;
    }
    return n;
}

int degreeNote(int root, const uint8_t* deg, int n, int d) {
    int oct = d >= 0 ? d / n : -((-d + n - 1) / n);
    int idx = d - oct * n;
    int note = root + deg[idx] + 12 * oct;
    return note < 0 ? 0 : (note > 127 ? 127 : note);
}

void setFx(Step& s, int slot, uint8_t type, uint8_t value) {
    s.fx[slot].type = type;
    s.fx[slot].value = value;
}

// Up to two FX on a step; extra ones are dropped (callers pass them by priority).
void putFx(Step& s, uint8_t type, uint8_t value) {
    for (int k = 0; k < kFxPerStep; ++k)
        if (s.fx[k].type == 0) {
            setFx(s, k, type, value);
            return;
        }
}

void writeMono(Pattern& p, int t, int s0, const MonoStep* steps, int n, uint8_t instr, uint8_t glide,
               uint8_t normalVol) {
    Track& tr = p.tracks[t];
    for (int i = 0; i < n; ++i) {
        const int s = s0 + i;
        if (s >= kStepCount) break;
        Step& st = tr.steps[s];
        st.clear();
        const MonoStep& m = steps[i];
        if (!m.active) continue;
        st.note = int8_t(m.note < 0 ? 0 : (m.note > 127 ? 127 : m.note));
        st.instrument = instr;
        const MonoStep* prev = i > 0 ? &steps[i - 1] : nullptr;
        const bool glideIn = prev && prev->active && prev->slide;
        // Priority: Glide (the slide itself), accent Volume, then Gate.
        if (glideIn) putFx(st, kFxGlide, glide);
        if (m.accent) putFx(st, kFxVolume, 100);
        else if (normalVol) putFx(st, kFxVolume, normalVol);
        // A note that slides out (or ties) sustains into the next step.
        if (!m.slide && m.gate < 0.95f) {
            int g = int(m.gate * 100.0f + 0.5f);
            putFx(st, kFxGate, uint8_t(g < 5 ? 5 : (g > 100 ? 100 : g)));
        }
    }
}

Result generate(int k, const int16_t* params, Target& t) {
    initLabels();
    Result r;
    if (!t.pat) return r;
    if (t.s1 < t.s0) {
        const int x = t.s0;
        t.s0 = t.s1;
        t.s1 = x;
    }
    if (t.t1 < t.t0) {
        const int x = t.t0;
        t.t0 = t.t1;
        t.t1 = x;
    }
    int16_t v[kParams];
    for (int i = 0; i < kParams; ++i) {
        const ParamDesc& d = param(k, i, params);
        int x = params[i];
        if (d.name) x = x < d.min ? d.min : (x > d.max ? d.max : x);
        v[i] = int16_t(x);
    }
    const uint32_t seed = v[kParams - 1] > 0 ? uint32_t(v[kParams - 1]) * 2654435761u : (t.entropy | 1u);
    r.seed = v[kParams - 1] > 0 ? uint32_t(v[kParams - 1]) : 0;
    r.rngSeed = seed;
    Rng rng(seed);
    switch (k) {
        case kAcid: genAcid(v, t, rng, r); break;
        case kCallResponse: genCallResponse(v, t, rng, r); break;
        case kBBCut: genBBCut(v, t, rng, r); break;
        case kTurnaround: genTurnaround(v, t, rng, r); break;
        case kGenotype: genGenotype(v, t, rng, r); break;
        case kStation: genStation(v, t, rng, r); break;
        default: std::snprintf(r.msg, sizeof(r.msg), "Unknown generator"); break;
    }
    return r;
}

}  // namespace gen
}  // namespace ot
