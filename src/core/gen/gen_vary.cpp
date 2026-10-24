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

void editTail(Track& tr, int a, int e, int style, float inten, Rng& r) {
    if (a < 0) a = 0;
    const int len = e - a + 1;
    if (len <= 0) return;
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
    for (int i = a; i <= e; ++i) {
        Step& st = tr.steps[i];
        const float ramp = len > 1 ? float(i - a) / float(len - 1) : 1.0f;   // 0 .. 1 over the tail
        switch (style) {
            case kRolls:
                if (hasNote(st) && r.coin(0.35f + 0.65f * inten)) forceFx(st, kFxRoll, rollValue(2.0f + 14.0f * ramp * inten));
                break;
            case kStutter:
                if (src >= 0 && r.coin(0.4f + 0.6f * inten)) {
                    st = tr.steps[src];
                    if (ramp > 0.5f) forceFx(st, kFxRoll, rollValue(ramp > 0.85f ? 4.0f : 2.0f));
                }
                break;
            case kDrop:
                // Silence into the downbeat; the tail's first step cuts the sound.
                if (i == a) {
                    st.clear();
                    st.note = kNoteOff;
                } else if (r.coin(0.5f + 0.5f * inten)) {
                    st.clear();
                }
                break;
            case kReverseStyle:
                if (hasNote(st) && r.coin(0.4f + 0.6f * inten)) forceFx(st, kFxReverse, 1);
                break;
            case kBuild:
                if (src >= 0 && !hasNote(st) && r.coin(inten)) {
                    const Step s0 = tr.steps[src];
                    st.note = s0.note;
                    st.instrument = s0.instrument;
                }
                if (hasNote(st)) {
                    forceFx(st, kFxRoll, rollValue(1.0f + 15.0f * ramp * ramp * (0.4f + 0.6f * inten), 0));
                    int vol = int(35.0f + 65.0f * ramp);
                    forceFx(st, kFxVolume, uint8_t(vol > 100 ? 100 : vol));
                }
                break;
            default: break;
        }
    }
}

}  // namespace

void genTurnaround(const int16_t* v, Target& t, Rng& rng, Result& r) {
    static const int kBars[4] = {1, 2, 4, 8};
    const int bar = 4 * (t.stepsPerBeat > 0 ? t.stepsPerBeat : 4);
    const int phrase = kBars[v[0]] * bar;
    const int budget = v[1];
    const int styleSel = v[2];
    const float inten = v[3] / 100.0f;
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
            editTail(track, a, e, style, inten, rng);
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
