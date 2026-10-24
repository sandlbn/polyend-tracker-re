// Pattern generators (OpenTracker semi/v0.1 fork, not in the stock firmware).
//
// Each generator writes ordinary steps (notes + standard step FX) into the
// working pattern, so a generated pattern plays the same on stock 1.9.x and
// on the Tracker Mini. Generators are pure functions over a Pattern: no
// allocation, no I/O; the Pattern screen owns undo and the popup.
//
//   Acid        303-style bassline (density / accent / slide), TB-3PO style
//   Call&Resp   call phrase + answer (styles and answer strategies from
//               Nordl3's Call / Response Generator, MIT)
//   BBCut       Nick Collins' BBCut cut procedures (via bencodec/BBenCut)
//               rewritten as step edits on a sliced instrument
//   Turnaround  phrase-end rolls / stutters / drops with an edit budget
//   Genotype    learns a Markov chain from the track and plays a variation
//               (Markov walk, or a bouncing "ball" over the learnt steps)
//   Station     numbers-station line from the radio: RDS name, PI code or
//               frequency read as digits (notes, or the slices of a sliced
//               counting voice), in groups of five
#pragma once

#include <cstdint>

#include "model/pattern.h"

namespace ot {
namespace gen {

// xorshift32: deterministic per seed, so the same seed rebuilds the same pattern.
struct Rng {
    uint32_t s = 0x9E3779B9u;
    explicit Rng(uint32_t seed = 1) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        uint32_t x = s;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return s = x;
    }
    float uni() { return float(next() >> 8) * (1.0f / 16777216.0f); }   // [0, 1)
    int below(int n) { return n <= 1 ? 0 : int(next() % uint32_t(n)); }
    int range(int lo, int hi) { return lo + below(hi - lo + 1); }       // inclusive
    bool coin(float p) { return uni() < p; }
    template <class T, int N>
    T pick(const T (&a)[N]) { return a[below(N)]; }
};

enum Kind : uint8_t {
    kAcid = 0,
    kCallResponse,
    kBBCut,
    kTurnaround,
    kGenotype,
    kStation,
    kKindCount
};

constexpr int kParams = 6;   // F2..F7 of the popup; the last one is always Seed

struct ParamDesc {
    const char* name;
    int16_t min, max, def;
    const char* const* labels;   // nullptr = numeric
};

struct KindDesc {
    const char* name;
    const char* help;            // one line under the title
    ParamDesc p[kParams];        // name == nullptr: unused
};

const KindDesc& kind(int k);
// Generator-dependent parameter (BBCut's A / B change with the procedure).
const ParamDesc& param(int k, int i, const int16_t* values);

// Slices of the target instrument (BBCut, Turnaround stutter).
struct Slices {
    int count = 0;                 // 0: not a sliced instrument
    const uint16_t* start = nullptr;   // u16 fractions of the sample length
    bool beatSlice = false;        // Beat Slice: note 24 + n plays slice n
    float sampleBeats = 0;         // sample length in beats at the song tempo (0 = unknown)
};

struct Target {
    Pattern* pat = nullptr;
    int t0 = 0, t1 = 0;            // tracks (inclusive)
    int s0 = 0, s1 = 0;            // steps (inclusive)
    uint8_t instrument = 0;
    int rootNote = 36;             // base note (C3 in Tracker naming = 36)
    uint16_t scaleMask = 0x0AD5;   // bit n = pitch class (root-relative) n in the scale
    int stepsPerBeat = 4;
    Slices slices;
    uint32_t entropy = 1;          // seed when the Seed parameter is 0 (random)
    // Station: the radio text (RDS name), PI code and frequency x 10.
    const char* text = nullptr;
    uint16_t pi = 0;
    uint16_t freq10 = 0;
};

// Result line for the popup / notify ("Generated 32 steps", "Needs a sliced instrument").
struct Result {
    bool ok = false;
    char msg[40] = {};
    uint32_t seed = 0;             // the Seed parameter (0 = random)
    uint32_t rngSeed = 0;          // the generator's RNG seed (rebuilds the same result)
};

Result generate(int kind, const int16_t* params, Target& t);

// --- Shared helpers (also used by the tests) ---------------------------------

// Scale degrees of a 12-bit mask in ascending order. Returns the count (>= 1).
int scaleDegrees(uint16_t mask, uint8_t out[12]);
// Note of degree d (may exceed the scale length: next octaves).
int degreeNote(int root, const uint8_t* deg, int n, int d);

// One monophonic phrase step (Acid, Call & Response).
struct MonoStep {
    bool active = false;
    int note = 60;
    bool accent = false;
    bool slide = false;            // glide into the next note (legato)
    float gate = 0.5f;             // fraction of a step; >= 1 sustains
};
// Writes steps s0.. of track t: note, instrument and the FX (Glide on the
// note slid into, Volume 100 on accents, Gate when shorter than a step).
// Inactive steps are cleared. normalVol > 0 writes Volume on normal notes too.
void writeMono(Pattern& p, int t, int s0, const MonoStep* steps, int n, uint8_t instr, uint8_t glide = 30,
               uint8_t normalVol = 0);

// Acid phrase (genAcid without writing) and TB-3PO style mutation: each step
// has `amount` (0..1) chance to re-roll its state or its pitch. v = the Acid
// parameters (Density, Accent, Slide, Range, Mutate, Seed).
// BBCut play table for the sequencer's B modes: 128 steps of cuts from
// procedure `proc` (default parameters), entry = {offset 0..127 (1/128 of the
// track at the cut start), steps since the cut start, roll selector}.
void bbPlayTable(int proc, uint32_t seed, uint8_t out[128][3]);

void acidPhrase(const int16_t* v, const Target& t, Rng& rng, MonoStep* out, int n);
void acidMutate(const int16_t* v, const Target& t, Rng& rng, MonoStep* ph, int n, float amount);

}  // namespace gen
}  // namespace ot
