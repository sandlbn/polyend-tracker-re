// Generator table, dispatch and shared helpers (gen.h).
#include "gen/gen.h"

#include <cstdio>

#include "gen/gen_internal.h"
#include "model/fx.h"

namespace ot {
namespace gen {

namespace {

const char* const kOctLabels[3] = {"1 oct", "2 oct", "3 oct"};
const char* const kAcidEvery[4] = {"1 loop", "2 loops", "4 loops", "8 loops"};
const char* const kAcidGate[3] = {"Normal", "Short", "Long"};
const char* const kCrLayout[3] = {"Call+Answer", "Answer mine", "Two tracks"};
const char* const kCrForm[6] = {"C R", "C R C R", "C C R", "C R R2 C", "C C C R", "C R C R2"};
const char* const kCrSteps[5] = {"Auto", "4", "8", "16", "32"};
const char* const kCrEnding[5] = {"Style", "Resolve", "Open", "Rise", "Fall"};
const char* const kCrOct[3] = {"Same", "+1 oct", "-1 oct"};
const char* const kCrMotif[4] = {"Auto", "2", "3", "4"};
const char* const kBBSource[6] = {"Auto", "1 beat", "2 beats", "1 bar", "2 bars", "4 bars"};
const char* const kTaPhrase[4] = {"1 bar", "2 bars", "4 bars", "8 bars"};
const char* const kTaStyle[6] = {"Rolls", "Stutter", "Drop", "Reverse", "Build", "Mix"};
const char* const kGtMode[3] = {"Markov", "Ball", "Ball+Markov"};
const char* const kStSource[4] = {"RDS name", "PI code", "Frequency", "All"};
const char* const kStMode[2] = {"Notes", "Slices"};
const char* const kStRate[3] = {"1/16", "1/8", "1/4"};
const char* const kStGroups[2] = {"Off", "Fives"};

constexpr const char* kSeedHelp = "0: a new result every time. 1-999: the same result again";
#define NONE {nullptr, 0, 0, 0, nullptr, nullptr}
#define SEED {"Seed", 0, 999, 0, nullptr, kSeedHelp}

KindDesc gKinds[kKindCount] = {
    {"Acid", "303 line: density, accent, slide (TB-3PO style)",
     {{"Density", -7, 7, 3, nullptr, "Steps that play: -7 almost none, 0 half, +7 nearly all. Live: Perform FX Acid Density"},
      {"Accent", 0, 100, 40, nullptr, "Chance of an accent (Volume 100)"},
      {"Slide", 0, 100, 25, nullptr, "Chance a note glides into the next one (Glide FX, no gate)"},
      {"Range", 0, 2, 1, kOctLabels, "Pitch range of the line"},
      {"Mutate", 0, 100, 0, nullptr, "Live: share of the steps re-rolled while it plays. 0 = locked"},
      SEED,
      {"Mut. every", 0, 3, 0, kAcidEvery, "How often Mutate changes the line"},
      {"Gate", 0, 2, 0, kAcidGate, "Length of the notes that do not slide"},
      {"Root", 0, 100, 35, nullptr, "Chance a note on the beat is the root note"},
      NONE, NONE, NONE}},
    {"Call+Resp", "Call phrase, then the answer (Nordl3 C/R Generator)",
     {{"Style", 0, kCrStyleCount - 1, 0, nullptr, "Genre profile: density, leaps, accents, slides, rests"},
      {"Answer", 0, kCrStrategyCount - 1, 0, nullptr, "How the answer is made from the call"},
      {"Density", 0, 100, 0, nullptr, "Notes per phrase. 0 = the style's own density"},
      {"Layout", 0, 2, 0, kCrLayout, "One track / answer the call you wrote / call and answer on two tracks"},
      {"Form", 0, 5, 0, kCrForm, "Order of the parts: C = call, R = answer (R2 = a second answer)"},
      SEED,
      {"Steps", 0, 4, 0, kCrSteps, "Steps per part. Auto = the selection divided by the parts"},
      {"Ending", 0, 4, 0, kCrEnding, "How the answer ends: the style's way, on the root, open, rising, falling"},
      {"Ans. oct", 0, 2, 0, kCrOct, "Register of the answer against the call"},
      {"Motif", 0, 3, 0, kCrMotif, "Length of the repeated motif in steps. Auto = from the part length"},
      NONE, NONE}},
    {"BBCut", "Rearrange a sliced break (BBCut / BBenCut procs)",
     {{"Proc", 0, kBBProcCount - 1, 0, nullptr, "BBCut cut procedure"},
      {"A", 0, 100, 50, nullptr, nullptr},
      {"B", 0, 100, 50, nullptr, nullptr},
      {"Source", 0, 5, 0, kBBSource, "Length of the break: Auto from the sample length at the song tempo"},
      NONE,
      SEED,
      NONE, NONE, NONE, NONE, NONE, NONE}},
    {"Turnaround", "Phrase-end rolls, stutters and drops on a budget",
     {{"Phrase", 0, 3, 1, kTaPhrase, "Phrase length: every phrase, half and quarter phrase end gets edits"},
      {"Budget", 1, 16, 4, nullptr, "Steps of edits at a phrase end"},
      {"Style", 0, 5, 5, kTaStyle, "Kind of edits"},
      {"Intensity", 0, 100, 60, nullptr, "How strong and how many the edits are"},
      NONE,
      SEED,
      NONE, NONE, NONE, NONE, NONE, NONE}},
    {"Genotype", "Learn the track, play a variation of it",
     {{"Mode", 0, 2, 2, kGtMode, "Markov walk over the learnt steps, a bouncing TILT ball, or both"},
      {"Identity", 0, 100, 60, nullptr, "Chance a step stays as it was"},
      {"Order", 1, 2, 1, nullptr, "Markov memory: 1 or 2 steps"},
      {"Variety", 0, 100, 40, nullptr, "How far the walk may jump"},
      NONE,
      SEED,
      NONE, NONE, NONE, NONE, NONE, NONE}},
    {"Station", "Numbers station from the radio (RDS, PI, frequency)",
     {{"Source", 0, 3, 3, kStSource, "What is read from the radio: station name, PI code, frequency, all"},
      {"Mode", 0, 1, 0, kStMode, "Notes: digits as scale degrees. Slices: digit d plays slice d (a voice counting 0-9)"},
      {"Rate", 0, 2, 1, kStRate, "Length of one digit"},
      {"Groups", 0, 1, 1, kStGroups, "Read in groups of five with a pause between"},
      {"Repeat", 1, 4, 2, nullptr, "Times the message is read"},
      SEED,
      NONE, NONE, NONE, NONE, NONE, NONE}},
};
#undef NONE
#undef SEED

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
        static const ParamDesc kNone = {nullptr, 0, 0, 0, nullptr, nullptr};
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
    const uint32_t seed = v[kSeedParam] > 0 ? uint32_t(v[kSeedParam]) * 2654435761u : (t.entropy | 1u);
    r.seed = v[kSeedParam] > 0 ? uint32_t(v[kSeedParam]) : 0;
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
