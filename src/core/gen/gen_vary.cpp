// Turnaround (phrase-end edits on a budget, after the TURNAROUND plug-in)
// and Genotype (learn the track, play a variation: GENOTYPE / TILT idea).
#include <cmath>
#include <cstdio>

#include "gen/gen_internal.h"

namespace ot {
namespace gen {

namespace {

// Sets FX `type` on a step: replaces the same type, else the first free
// slot, else slot 1 (slot 0 often carries the slice / volume).
void forceFx(Step& s, uint8_t type, uint8_t value) {
    for (int k = 0; k < kFxPerStep; ++k)
        if (s.fx[k].type == type) {
            s.fx[k].value = value;
            return;
        }
    for (int k = 0; k < kFxPerStep; ++k)
        if (s.fx[k].type == 0) {
            setFx(s, k, type, value);
            return;
        }
    setFx(s, 1, type, value);
}

bool hasNote(const Step& s) { return s.note >= 0; }

enum TaStyle { kRolls = 0, kStutter, kDrop, kReverseStyle, kBuild, kMixStyle };

struct TaCfg {
    float inten;
    float rollMax;     // hits per step at the strongest point (FX level: 2 / 4 / 8)
    int volFloor;      // Build: the volume ramp starts here (70 / 50 / 30)
    bool tidy;         // drop Micro-move inside the edited tails
};

// Copies the hit of `src` (note, instrument, a Slice FX), not its other FX.
void copyHit(Step& dst, const Step& src) {
    dst.clear();
    dst.note = src.note;
    dst.instrument = src.instrument;
    for (const StepFx& f : src.fx)
        if (f.type == kFxSliceId) putFx(dst, f.type, f.value);
}

bool rollOn(const Step& s) {
    for (const StepFx& f : s.fx)
        if (f.type == kFxRoll && f.value) return true;
    return false;
}

void dropFx(Step& s, uint8_t type) {
    for (StepFx& f : s.fx)
        if (f.type == type) f = StepFx{};
}

void editTail(Track& tr, int a, int e, int style, const TaCfg& c, Rng& r, int trackEnd) {
    if (a < 0) a = 0;
    const int len = e - a + 1;
    if (len <= 0) return;
    const float inten = c.inten;
    // The last hit at or before the tail start (stutter / roll source).
    int src = -1;
    for (int i = a; i >= 0; --i)
        if (hasNote(tr.steps[i])) {
            src = i;
            break;
        }
    if (src < 0)
        for (int i = a; i <= e; ++i)
            if (hasNote(tr.steps[i])) {
                src = i;
                break;
            }
    const Step srcStep = src >= 0 ? tr.steps[src] : Step{};
    bool reversed = false;
    for (int i = a; i <= e; ++i) {
        Step& st = tr.steps[i];
        if (c.tidy) dropFx(st, kFxMicroMove);
        const float ramp = len > 1 ? float(i - a) / float(len - 1) : 1.0f;   // 0 .. 1 over the tail
        switch (style) {
            case kRolls:
                // Rolls on the hits, faster towards the end, up to rollMax.
                if (hasNote(st) && r.coin(0.3f + 0.6f * inten)) {
                    const float hits = 2.0f + (c.rollMax - 2.0f) * ramp * inten;
                    forceFx(st, kFxRoll, rollValue(hits < 2.0f ? 2.0f : hits));
                }
                break;
            case kStutter:
                // The source hit repeated (clean copies), a roll on the last steps.
                if (src >= 0 && r.coin(0.35f + 0.55f * inten)) {
                    copyHit(st, srcStep);
                    if (ramp > 0.7f && c.rollMax >= 4) forceFx(st, kFxRoll, rollValue(2.0f));
                }
                break;
            case kDrop:
                // A short gap into the downbeat: the last steps cut, the rest stays.
                if (i >= e - (len > 2 ? (inten > 0.6f ? 2 : 1) : 0)) {
                    st.clear();
                    if (i == e - (len > 2 ? (inten > 0.6f ? 2 : 1) : 0)) st.note = kNoteOff;
                }
                break;
            case kReverseStyle:
                // One reversed hit (two when strong), not the whole tail.
                if (hasNote(st) && !reversed && r.coin(0.3f + 0.5f * ramp)) {
                    forceFx(st, kFxReverse, 1);
                    reversed = inten < 0.7f;
                }
                break;
            case kBuild:
                if (src >= 0 && !hasNote(st) && r.coin(inten * 0.8f)) copyHit(st, srcStep);
                if (hasNote(st)) {
                    const float hits = 1.0f + (c.rollMax - 1.0f) * ramp * ramp * (0.4f + 0.6f * inten);
                    if (hits >= 1.5f) forceFx(st, kFxRoll, rollValue(hits, 0));
                    int vol = int(float(c.volFloor) + float(100 - c.volFloor) * ramp);
                    forceFx(st, kFxVolume, uint8_t(vol > 100 ? 100 : vol));
                }
                break;
            default: break;
        }
    }
    // A roll goes on until the next note: end it at the downbeat after the tail.
    if (rollOn(tr.steps[e])) {
        const int nx = e + 1 <= trackEnd ? e + 1 : -1;
        if (nx >= 0 && !hasNote(tr.steps[nx]) && !rollOn(tr.steps[nx])) forceFx(tr.steps[nx], kFxRoll, 0);
    }
}

}  // namespace

void genTurnaround(const int16_t* v, Target& t, Rng& rng, Result& r) {
    static const int kBars[4] = {1, 2, 4, 8};
    static const float kRollMax[3] = {2.0f, 4.0f, 8.0f};
    static const int kVolFloor[3] = {70, 50, 30};
    const int bar = 4 * (t.stepsPerBeat > 0 ? t.stepsPerBeat : 4);
    const int phrase = kBars[v[0]] * bar;
    const int budget = v[1];
    const int styleSel = v[2];
    TaCfg c;
    c.inten = v[3] / 100.0f;
    const int lvl = v[4] < 0 || v[4] > 2 ? 0 : v[4];
    c.rollMax = kRollMax[lvl];
    c.volFloor = kVolFloor[lvl];
    c.tidy = v[6] == 0;
    int edits = 0;
    for (int tr = t.t0; tr <= t.t1; ++tr) {
        Track& track = t.pat->tracks[tr];
        // Phrase ends inside the range, at three levels: P (full budget),
        // P/2 (half), P/4 (quarter).
        for (int e = t.s0; e <= t.s1; ++e) {
            const int pos = e - t.s0 + 1;   // steps from the range start
            int tail = 0;
            if (pos % phrase == 0) tail = budget;
            else if (phrase >= 2 && pos % (phrase / 2) == 0) tail = budget / 2;
            else if (phrase >= 4 && pos % (phrase / 4) == 0) tail = budget / 4;
            if (pos == t.s1 - t.s0 + 1 && tail < budget / 2) tail = budget / 2 > 0 ? budget / 2 : 1;   // the range end turns
            if (tail <= 0) continue;
            const int style = styleSel == kMixStyle ? rng.below(5) : styleSel;
            int a = e - tail + 1;
            if (a < t.s0) a = t.s0;
            editTail(track, a, e, style, c, rng, t.s1);
            ++edits;
        }
    }
    std::snprintf(r.msg, sizeof(r.msg), "Turnaround: %d phrase ends", edits);
    r.ok = true;
}

// --- Genotype ---------------------------------------------------------------

namespace {

int successor(const Step* seq, int n, int order, const Step& prev1, const Step& prev2, bool havePrev2, Rng& r) {
    int cand[kStepCount], nc = 0;
    for (int i = 0; i + 1 < n + 1; ++i) {
        const int nx = (i + 1) % n;   // the loop wraps: last -> first
        if (!(seq[i] == prev1)) continue;
        if (order >= 2 && havePrev2) {
            const int pv = (i - 1 + n) % n;
            if (!(seq[pv] == prev2)) continue;
        }
        cand[nc++] = nx;
    }
    if (nc == 0) return -1;
    return cand[r.below(nc)];
}

}  // namespace

void genGenotype(const int16_t* v, Target& t, Rng& rng, Result& r) {
    const int mode = v[0];
    const float identity = v[1] / 100.0f;
    const int order = v[2];
    const float variety = v[3] / 100.0f;
    const int n = t.s1 - t.s0 + 1;
    if (n < 2) {
        std::snprintf(r.msg, sizeof(r.msg), "Select 2 steps or more");
        return;
    }
    int changed = 0;
    for (int tr = t.t0; tr <= t.t1; ++tr) {
        Track& track = t.pat->tracks[tr];
        Step seq[kStepCount];
        bool any = false;
        for (int i = 0; i < n; ++i) {
            seq[i] = track.steps[t.s0 + i];
            any = any || hasNote(seq[i]);
        }
        if (!any) continue;
        // Ball (TILT): position and velocity in steps; bumpers kick it.
        float x = 0, vel = 1.0f;
        Step out[kStepCount];
        for (int i = 0; i < n; ++i) {
            int pick = i;
            if (!rng.coin(identity)) {
                if (mode == 0) {   // Markov walk from what was just played
                    const int s = i > 0 ? successor(seq, n, order, out[i - 1], i > 1 ? out[i - 2] : out[0], i > 1, rng) : -1;
                    pick = s >= 0 ? s : i;
                } else {
                    int bp = int(std::floor(x + 0.5f));
                    bp = ((bp % n) + n) % n;
                    pick = bp;
                    if (mode == 2 && i > 0 && rng.coin(0.5f)) {
                        const int s = successor(seq, n, order, out[i - 1], i > 1 ? out[i - 2] : out[0], i > 1, rng);
                        if (s >= 0) pick = s;
                    }
                }
                if (rng.coin(variety * 0.5f)) pick = rng.below(n);   // free jump
            }
            out[i] = seq[pick];
            changed += !(out[i] == seq[i]);
            // Ball motion: drift + bumper kicks, bounce on the ends.
            if (rng.coin(0.15f + 0.5f * variety)) vel = (rng.coin(0.5f) ? 1.0f : -1.0f) * (0.5f + rng.uni() * (1.0f + 2.0f * variety));
            x += vel;
            if (x < 0) {
                x = -x;
                vel = -vel;
            } else if (x > float(n - 1)) {
                x = 2.0f * float(n - 1) - x;
                vel = -vel;
            }
        }
        for (int i = 0; i < n; ++i) track.steps[t.s0 + i] = out[i];
    }
    if (changed == 0 && identity < 1.0f) {
        std::snprintf(r.msg, sizeof(r.msg), "Genotype: nothing to vary");
        r.ok = true;
        return;
    }
    std::snprintf(r.msg, sizeof(r.msg), "Genotype: %d steps changed", changed);
    r.ok = true;
}

}  // namespace gen
}  // namespace ot
