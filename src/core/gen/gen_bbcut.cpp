// BBCut generator: the cut procedures of Nick Collins' BBCut (SuperCollider),
// as ported to TypeScript by bencodec/BBenCut, rewritten here without heap
// for the Tracker. BBenCut rearranges an audio clip into new clips; on the
// Tracker the same blocks and cuts become steps of a sliced instrument:
//
//   * a block's source offset picks the slice (closest slice start at or
//     before the offset in the source loop);
//   * a cut of a step or longer is a hit on its step (Micro-move when it
//     starts between steps, Gate when shorter than its IOI, Volume from the
//     cut amplitude, an OFF for a stop);
//   * runs of cuts shorter than a step become Roll FX, step by step, with
//     the nearest Tracker roll rate (1, 2, 3, 4, 6, 8, 12, 16 per step) --
//     BBenCut pre-renders those to WAV for the same reason (clips have a
//     minimum length, steps have a grid).
//
// Nine of the ten procedures BBenCut exposes are here with the same defaults
// (CageCut, Timeline, Permute and ThrashCut with OT changes, see each), and
// "Jungle" (OT) in the place of ChooseBlock: classic amen-style edits of the
// break in eighths (double snares, kick shifts, repeats, a snare fill every
// N bars). Options for every procedure: how sub-step runs are written (Roll
// FX styles, a pitch slide on the roll, stutters, single hits), a random
// pitch per hit (+-1..3 semitones), and whether Micro-move / Volume FX are
// written at all.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <new>

#include "gen/gen_internal.h"
#include "sdram_bss.h"

namespace ot {
namespace gen {

namespace {

#include "gen/sqdata.inc"

enum Proc : uint8_t {
    kClassic = 0,   // BBCutProc11
    kChooseCut,
    kJungle,
    kWarpCut,
    kSQPusher1,
    kSQPusher2,
    kCageCut,
    kTimeline,
    kPermute,
    kThrash,
};

const char* const kProcNames[kBBProcCount] = {"Classic", "ChooseCut", "Jungle", "WarpCut", "SQPusher1",
                                              "SQPusher2", "CageCut", "Timeline", "Permute", "ThrashCut"};

const char* const kSubdiv5[5] = {"4", "6", "8", "12", "16"};
const char* const kRollArea[4] = {"1/2 beat", "1 beat", "2 beats", "4 beats"};
const char* const kFillEvery[4] = {"1 bar", "2 bars", "4 bars", "8 bars"};
const char* const kQuant[3] = {"Off", "1/16", "1/8"};
const char* const kTimelines[5] = {"3+3+2", "3+2+3", "2+3+3", "5+3", "2+2+3+1"};
const char* const kSubdiv3[3] = {"4", "8", "16"};
const char* const kFillEvery3[3] = {"1 bar", "2 bars", "4 bars"};

// Parameters A / B per procedure (nullptr name = none).
const ParamDesc kProcParams[kBBProcCount][2] = {
    {{"Repeats", 0, 100, 20, nullptr}, {"Subdiv", 0, 4, 2, kSubdiv5}},
    {{"Roll", 0, 100, 10, nullptr}, {"Roll area", 0, 3, 2, kRollArea}},
    {{"Edits", 0, 100, 50, nullptr, "Jungle: how many amen-style edits per bar"},
     {"Fill every", 0, 2, 1, kFillEvery3, "Jungle: a snare fill at the end of every N bars"}},
    {{"Straight", 0, 100, 50, nullptr}, {"Accel", 50, 99, 90, nullptr}},
    {{"Activity", 0, 100, 10, nullptr}, {"Fill every", 0, 3, 2, kFillEvery}},
    {{"Scramble", 0, 100, 0, nullptr}, {"Quantise", 0, 2, 0, kQuant}},
    {{"Shuffle", 0, 100, 50, nullptr, "CageCut: chance a block plays another part of the break"},
     {nullptr, 0, 0, 0, nullptr}},
    {{"Timeline", 0, 4, 0, kTimelines}, {"Shuffle", 0, 100, 40, nullptr, "Timeline: chance a block plays another part, and a random rotation"}},
    {{"Subdiv", 0, 2, 1, kSubdiv3}, {"Shuffle", 0, 100, 50, nullptr}},
    {{"Stop", 0, 100, 13, nullptr}, {"Snare at", 0, 100, 13, nullptr}},
};

constexpr int kMaxCuts = 64;        // per block
constexpr int kMaxSeqCuts = 384;    // per phrase (sequence procedures)
constexpr int kMaxSeqBlocks = 96;

struct Cut {
    float ioi, dur;
    float off;   // < 0: the block offset; else a proportion 0..1 of the source loop
    float amp;
};

struct Block {
    float len = 0;
    int n = 0;
    Cut c[kMaxCuts];
    float offBeats = -1;   // source offset in beats (chooseoffset), -1 none
    float offProp = -1;    // source offset as a loop proportion (setoffset), -1 none
    bool roll = false;
};

// Procedure state. Lives in a static scratch (SDRAM on the device).
struct State {
    Rng* r;
    int type;
    float loopBeats;
    int16_t a, b;
    // BBCutProc base accounting
    int phrase, block;
    float phrasepos, cpl, bpsd, totalbeats;
    // Classic
    int subdiv;
    int unitsdone, totalunits;
    // Permute
    int stepIndex;
    // phrase sequences (SQPusher1/2, Cage, Timeline, Thrash)
    int seqBlocks;
    int seqStart[kMaxSeqBlocks + 1];
    float seqLen[kMaxSeqBlocks];
    Cut seq[kMaxSeqCuts];
    int seqCuts;
    // Thrash
    int whichRiff;
    bool fill;
    int didStop;
    // Output
    Block out;
};

OT_SDRAM_BSS alignas(8) unsigned char gScratch[sizeof(State)];

void newPhrase(State& s, float cpl) {
    s.cpl = cpl;
    s.phrasepos = 0;
    s.phrase += 1;
    s.block = 0;
}

void endBlock(State& s) {
    s.phrasepos += s.out.len;
    s.totalbeats += s.out.len;
    s.block += 1;
}

void setCuts(Block& b, int n, float size, float amp = 1.0f) {
    if (n > kMaxCuts) n = kMaxCuts;
    if (n < 1) n = 1;
    b.n = n;
    for (int i = 0; i < n; ++i) b.c[i] = Cut{size, size, -1, amp};
}

void seqBegin(State& s) {
    s.seqBlocks = 0;
    s.seqCuts = 0;
    s.seqStart[0] = 0;
}
// Adds a block of cuts to the phrase sequence.
void seqAdd(State& s, const Cut* cuts, int n) {
    if (s.seqBlocks >= kMaxSeqBlocks) return;
    float len = 0;
    for (int i = 0; i < n && s.seqCuts < kMaxSeqCuts; ++i) {
        s.seq[s.seqCuts++] = cuts[i];
        len += cuts[i].ioi;
    }
    s.seqLen[s.seqBlocks] = len;
    s.seqStart[++s.seqBlocks] = s.seqCuts;
}

// Takes block s.block of the sequence, trimmed to the beats left (the
// "cutoffIndex" trim shared by SQPusher1/2, CageCut, TimelineCut).
void seqTake(State& s, bool chooseOffset = true) {
    Block& b = s.out;
    const float left = s.cpl - s.phrasepos;
    b.n = 0;
    if (s.block < s.seqBlocks) {
        for (int i = s.seqStart[s.block]; i < s.seqStart[s.block + 1] && b.n < kMaxCuts; ++i) b.c[b.n++] = s.seq[i];
    }
    if (b.n == 0) {
        b.c[0] = Cut{left, left, -1, 1};
        b.n = 1;
    }
    float len = 0;
    for (int i = 0; i < b.n; ++i) len += b.c[i].ioi;
    if (len > left) {
        float acc = 0;
        int cut = 0;
        for (int i = 0; i < b.n; ++i) {
            acc += b.c[i].ioi;
            if (acc > left - 0.001f) {
                cut = i;
                break;
            }
        }
        b.n = cut + 1;
        const float trimmed = b.c[cut].ioi - (acc - left);
        b.c[cut].ioi = trimmed;
        if (b.c[cut].dur > trimmed) b.c[cut].dur = trimmed;
        len = left;
    }
    b.len = len;
    if (chooseOffset) b.offBeats = s.phrasepos;
}

float wchoose3(Rng& r, const float* vals, const float* w) {
    float tot = w[0] + w[1] + w[2], x = r.uni() * tot;
    for (int i = 0; i < 3; ++i) {
        x -= w[i];
        if (x <= 0) return vals[i];
    }
    return vals[2];
}

// --- the procedures ----------------------------------------------------------

void classic(State& s) {
    Rng& r = *s.r;
    Block& b = s.out;
    static const int kSd[5] = {4, 6, 8, 12, 16};
    const float repChance = s.a / 100.0f;
    const int repSpeed = 2;
    const float repArea = 0.5f;
    if (s.unitsdone >= s.totalunits) {
        const float barLen = 4.0f;
        int bars = int(s.loopBeats / barLen + 0.5f);
        if (bars < 1) bars = 1;
        s.subdiv = kSd[s.b];
        s.totalunits = bars * s.subdiv;
        newPhrase(s, barLen * float(bars));
        s.bpsd = barLen / float(s.subdiv);
        s.unitsdone = 0;
    }
    const int left = s.totalunits - s.unitsdone;
    int unit, repeats;
    if (r.coin(repChance) && float(left) < repArea * float(s.subdiv)) {
        repeats = left * repSpeed;
        b.len = s.cpl - s.phrasepos;
        s.unitsdone = s.totalunits;
        b.roll = true;
        b.offBeats = s.phrasepos;
        setCuts(b, repeats, s.bpsd / float(repSpeed));
    } else {
        b.roll = false;
        int oddmax = s.subdiv / 2;
        oddmax = oddmax % 2 == 0 ? (oddmax - 2) / 2 : (oddmax - 1) / 2;
        if (oddmax < 0) oddmax = 0;
        unit = 2 * r.range(0, oddmax) + 1;
        while (unit > left) unit -= 2;
        if (unit < 1) unit = 1;
        repeats = r.range(1, 2);
        int proj = repeats * unit + s.unitsdone;
        while (proj > s.totalunits) {
            repeats -= 1;
            if (repeats <= 1) {
                repeats = 1;
                unit = left;
            }
            proj = repeats * unit + s.unitsdone;
        }
        b.len = float(repeats * unit) * s.bpsd;
        if (proj == s.totalunits) b.len = s.cpl - s.phrasepos;
        s.unitsdone += repeats * unit;
        b.offBeats = s.phrasepos;
        setCuts(b, repeats, float(unit) * s.bpsd);
    }
}

void chooseCut(State& s) {
    Rng& r = *s.r;
    Block& b = s.out;
    static const float kArea[4] = {0.5f, 1, 2, 4};
    if (s.phrasepos >= s.cpl) newPhrase(s, s.loopBeats);
    const float left = s.cpl - s.phrasepos;
    if (r.coin(s.a / 100.0f) && left < kArea[s.b]) {
        b.roll = true;
        b.len = left;
        int reps = int(left * 4 + 0.5f);
        if (reps < 1) reps = 1;
        setCuts(b, reps, left / float(reps));
    } else {
        b.roll = false;
        float size = r.coin(0.5f) ? 1.0f : 1.5f;
        if (size > left) size = left;
        int repeats = r.range(1, 2);
        float proj = float(repeats) * size + s.phrasepos;
        while (proj > s.cpl + 1e-4f) {
            repeats -= 1;
            if (repeats <= 1) {
                repeats = 1;
                size = left;
            }
            proj = float(repeats) * size + s.phrasepos;
        }
        setCuts(b, repeats, size);
        b.len = float(repeats) * size;
    }
    b.offBeats = s.phrasepos;
}

// Jungle (OT): the bar in eighths of the source bar, with amen-style edits
// (A = how many): a double snare, a double kick, the kick again on 3-and, the
// first half repeated, a snare pair at the end, two late eighths swapped; a
// snare fill (16ths, then 32nds) at the end of every B bars. Snares are taken
// from beats 2 and 4 of the source bar.
void jungle(State& s) {
    Rng& r = *s.r;
    static const int kEvery[3] = {1, 2, 4};
    if (s.phrasepos >= s.cpl - 0.001f) {
        newPhrase(s, 4.0f);
        const float amt = s.a / 100.0f;
        const int bars = s.loopBeats >= 4 ? int(s.loopBeats / 4.0f + 0.5f) : 1;
        const float barOff = s.loopBeats >= 4 ? float((s.phrase - 1) % bars) * 4.0f : 0;
        int src[8];
        for (int k = 0; k < 8; ++k) src[k] = k;
        if (r.coin(amt * 0.5f)) src[3] = 2;     // double snare
        if (r.coin(amt * 0.4f)) src[1] = 0;     // double kick
        if (r.coin(amt * 0.35f)) src[5] = 4;    // kick again on 3-and
        if (r.coin(amt * 0.3f))
            for (int k = 4; k < 8; ++k) src[k] = src[k - 4];   // the first half again
        if (r.coin(amt * 0.25f)) {               // a snare pair at the end
            src[6] = 2;
            src[7] = 3;
        }
        if (r.coin(amt * 0.2f)) {
            const int a = 4 + r.below(4), b = 4 + r.below(4), t = src[a];
            src[a] = src[b];
            src[b] = t;
        }
        const bool fill = (s.phrase % kEvery[s.b < 0 || s.b > 2 ? 1 : s.b]) == 0;
        const float snare = barOff + (r.coin(0.5f) ? 1.0f : 3.0f);
        seqBegin(s);
        for (int k = 0; k < 8; ++k) {
            Cut cuts[4];
            int n = 0;
            if (fill && k >= 6) {
                const int hits = k == 6 ? 2 : 4;
                const float sp = std::fmod(snare, s.loopBeats) / s.loopBeats;
                for (int h = 0; h < hits; ++h) cuts[n++] = Cut{0.5f / float(hits), 0.5f / float(hits), sp, 1};
            } else {
                const float off = std::fmod(barOff + float(src[k]) * 0.5f, s.loopBeats);
                cuts[n++] = Cut{0.5f, 0.5f, off / s.loopBeats, 1};
            }
            seqAdd(s, cuts, n);
        }
    }
    seqTake(s, false);
    s.out.roll = false;
}

void warpCut(State& s) {
    Rng& r = *s.r;
    Block& b = s.out;
    if (s.phrasepos >= s.cpl - 0.0001f) newPhrase(s, s.loopBeats);
    const float left = s.cpl - s.phrasepos;
    static const float kVals[3] = {0.5f, 1, 2}, kW[3] = {0.5f, 0.4f, 0.1f};
    b.len = wchoose3(r, kVals, kW);
    if (b.len > left) b.len = left;
    if (r.coin(s.a / 100.0f)) {
        b.roll = false;
        setCuts(b, 1, b.len);
    } else {
        b.roll = true;
        static const int kShort[3] = {4, 8, 16}, kLong[3] = {8, 16, 32};
        const int reps = b.len < 1.0f ? r.pick(kShort) : r.pick(kLong);
        if (r.coin(0.7f)) {
            setCuts(b, reps, b.len / float(reps));
        } else {
            const float acc = s.b / 100.0f;
            const float temp = b.len * (1 - acc) / (1 - std::pow(acc, float(reps)));
            b.n = reps > kMaxCuts ? kMaxCuts : reps;
            float x = temp;
            for (int i = 0; i < b.n; ++i, x *= acc) b.c[i] = Cut{x, x, -1, 1};
            if (r.coin(0.6f))
                for (int i = 0; i < b.n / 2; ++i) {
                    const Cut t = b.c[i];
                    b.c[i] = b.c[b.n - 1 - i];
                    b.c[b.n - 1 - i] = t;
                }
        }
    }
    b.offBeats = s.phrasepos;
}

void sqPusher1(State& s) {
    Rng& r = *s.r;
    static const uint16_t kFills[13][6][6] = {
        // each fill: up to 6 blocks of up to 6 cuts, 0 = end
        {{750, 750, 750, 750}, {1000}},
        {{500, 1000}, {1000}, {1000, 500}},
        {{500}, {1000, 1000, 1000}, {500}},
        {{571}, {571, 571}, {571, 571}, {571}, {286, 286}},
        {{1000, 500}, {1000, 500}, {500, 500}},
        {{500, 500}, {660, 670, 670}, {1010}},
        {{340}, {330, 330}, {340, 330}, {2330}},
        {{1400}, {400, 400}, {600, 200}, {1000}},
        {{167, 167, 166}, {1000, 1000, 1000}, {500}},
        {{1500, 500, 1000}, {250, 250, 250, 250}},
        {{200, 200}, {400, 400}, {400, 400}, {2000}},
        {{750, 750, 1000}, {250, 250, 250, 250, 250, 250}},
        {{500, 1000}, {500}, {125, 125, 125, 125}, {1000}, {167, 167, 166}},
    };
    static const float kSqW[8] = {0.0f, 0.3f, 0.0f, 0.5f, 0.7f, 0.8f, 0.9f, 0.6f};
    static const int kEvery[4] = {1, 2, 4, 8};
    if (s.phrasepos >= s.cpl - 0.001f) {
        newPhrase(s, 4.0f);
        seqBegin(s);
        Cut cuts[8];
        if (s.phrase % kEvery[s.b] == 0) {
            const int f = r.below(13);
            for (int bl = 0; bl < 6 && kFills[f][bl][0] > 0; ++bl) {
                int n = 0;
                for (int k = 0; k < 6 && kFills[f][bl][k] > 0; ++k) {
                    const float d = float(kFills[f][bl][k]) * 0.001f;   // beats
                    cuts[n++] = Cut{d, d, -1, 1};
                }
                seqAdd(s, cuts, n);
            }
        } else {
            float done = 0;
            while (done < 4.0f) {
                const float beatpos = std::fmod(std::floor(done / 0.25f + 0.5f) * 0.25f, 1.0f);
                const int quaver = int(std::floor(done / 0.5f + 0.5f) * 2) % 8;
                const float chance = kSqW[quaver] * (s.a / 100.0f);
                const int group = beatpos == 0.5f ? 1 : r.range(1, 2);
                int n = 0;
                if (r.coin(chance))
                    for (int k = 0; k < group * 2; ++k) cuts[n++] = Cut{0.25f, 0.25f, -1, 1};
                else
                    for (int k = 0; k < group; ++k) cuts[n++] = Cut{0.5f, 0.5f, -1, 1};
                seqAdd(s, cuts, n);
                for (int k = 0; k < n; ++k) done += cuts[k].ioi;
            }
        }
    }
    seqTake(s);
    s.out.roll = false;
}

void sqPusher2(State& s) {
    Rng& r = *s.r;
    if (s.phrasepos >= s.cpl - 0.001f) {
        newPhrase(s, 4.0f);
        static const float kQ[3] = {0, 0.25f, 0.5f};
        const float q = kQ[s.b];
        // Pick a bar of the data.
        const int bar = r.below(kSqBars);
        int at = 0;
        for (int i = 0; i < bar; ++i) at += 1 + kSqData[at];
        const int n = kSqData[at];
        float on[16] = {};
        for (int i = 0; i < n && i < 16; ++i) {
            float t = float(kSqData[at + 1 + i]) / 64.0f;
            if (q > 0) t = std::floor(t / q + 0.5f) * q;
            on[i] = t;
        }
        Cut iois[17];
        int ni = 0;
        for (int i = 0; i < n; ++i) {
            const float nx = i + 1 < n ? on[i + 1] : 4.0f;
            const float d = nx - on[i];
            if (d > 0.00001f) iois[ni++] = Cut{d, d, -1, 1};
        }
        // curdle(0.5): group adjacent cuts into blocks (as index runs over iois).
        // BBenCut fills the bar start before the first onset with the source
        // (continuous audio) rather than silence; do the same.
        Cut lead = Cut{on[0], on[0], -1, 1};
        const bool hasLead = n > 0 && on[0] > 0.025f;
        int gStart[18], gLen[18], ng = 0;
        if (ni > 0) {
            gStart[0] = 0;
            gLen[0] = 0;
            for (int i = 0; i < ni; ++i) {
                ++gLen[ng];
                if (r.coin(0.5f) && i + 1 < ni) {
                    ++ng;
                    gStart[ng] = i + 1;
                    gLen[ng] = 0;
                }
            }
            ++ng;
        }
        // Block list: the leading silence (as BBenCut), then the groups.
        int order[19], no = 0;
        if (hasLead) order[no++] = -1;
        for (int i = 0; i < ng; ++i) order[no++] = i;
        if (r.coin(s.a / 100.0f))
            for (int i = no - 1; i > 0; --i) {
                const int j = r.below(i + 1), t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        seqBegin(s);
        for (int i = 0; i < no; ++i) {
            if (order[i] < 0) seqAdd(s, &lead, 1);
            else seqAdd(s, &iois[gStart[order[i]]], gLen[order[i]]);
        }
    }
    seqTake(s);
    s.out.roll = false;
}

// CageCut: three blocks of three cuts in one of four proportions (OT: the
// form and its inner form are drawn per phrase, and A = Shuffle: chance a cut
// plays another eighth of the break instead of its own place).
void cageCut(State& s) {
    Rng& r = *s.r;
    if (s.phrasepos >= s.cpl - 0.001f) {
        newPhrase(s, s.loopBeats);
        static const float kForms[4][3] = {
            {0.5f, 0.25f, 0.25f}, {0.25f, 0.5f, 0.25f}, {0.25f, 0.25f, 0.5f}, {1 / 3.0f, 1 / 3.0f, 1 / 3.0f}};
        const float* f = kForms[r.below(4)];
        const float* g = kForms[r.below(4)];
        const int eighths = int(s.cpl * 2.0f + 0.5f) > 0 ? int(s.cpl * 2.0f + 0.5f) : 1;
        seqBegin(s);
        float pos = 0;
        for (int bl = 0; bl < 3; ++bl) {
            Cut cuts[3];
            for (int c = 0; c < 3; ++c) {
                const float d = s.cpl * f[bl] * g[c];
                float src = pos;
                if (r.coin(s.a / 100.0f)) src = float(r.below(eighths)) * 0.5f;
                cuts[c] = Cut{d, d, std::fmod(src, s.loopBeats) / s.loopBeats, 1};
                pos += d;
            }
            seqAdd(s, cuts, 3);
        }
    }
    seqTake(s, false);
    s.out.roll = false;
}

// Timeline: the phrase cut in the proportions of a timeline (3+3+2 ...).
// OT: B = Shuffle: chance of a random rotation of the timeline per phrase and
// chance a block plays another eighth of the break.
void timeline(State& s) {
    Rng& r = *s.r;
    static const float kT[5][5] = {{3, 3, 2}, {3, 2, 3}, {2, 3, 3}, {5, 3}, {2, 2, 3, 1}};
    if (s.phrasepos >= s.cpl - 0.001f) {
        newPhrase(s, s.loopBeats);
        const float* t = kT[s.a < 0 || s.a > 4 ? 0 : s.a];
        float sum = 0;
        int n = 0;
        while (n < 5 && t[n] > 0) sum += t[n++];
        const float shuffle = s.b / 100.0f;
        const int rot = r.coin(shuffle) ? r.below(n) : 0;
        const int eighths = int(s.cpl * 2.0f + 0.5f) > 0 ? int(s.cpl * 2.0f + 0.5f) : 1;
        seqBegin(s);
        float pos = 0;
        for (int i = 0; i < n; ++i) {
            const float d = s.cpl * t[(i + rot) % n] / sum;
            float src = pos;
            if (r.coin(shuffle)) src = float(r.below(eighths)) * 0.5f;
            const Cut c{d, d, std::fmod(src, s.loopBeats) / s.loopBeats, 1};
            seqAdd(s, &c, 1);
            pos += d;
        }
    }
    seqTake(s, false);
    s.out.roll = false;
}

// Permute: the phrase in A subdivisions played in a permuted order. OT: the
// permutation is one of eight musical ones in eighth groups (identity, half
// repeats, swapped halves, pairs reversed, ...), drawn per phrase with
// chance B (Shuffle); with Shuffle high some slots also swap inside a pair.
void permute(State& s) {
    Rng& r = *s.r;
    Block& b = s.out;
    static const int kSd[3] = {4, 8, 16};
    static const uint8_t kPerm[8][8] = {
        {0, 1, 2, 3, 4, 5, 6, 7}, {0, 1, 2, 3, 0, 1, 6, 7}, {0, 1, 2, 3, 4, 5, 2, 3}, {0, 3, 2, 1, 4, 7, 6, 5},
        {0, 1, 0, 1, 2, 3, 6, 7}, {4, 5, 6, 7, 0, 1, 2, 3}, {0, 1, 6, 7, 4, 5, 2, 3}, {0, 1, 2, 2, 4, 5, 6, 6}};
    if (s.phrasepos >= s.cpl - 0.001f) {
        newPhrase(s, s.loopBeats);
        s.subdiv = kSd[s.a < 0 || s.a > 2 ? 1 : s.a];
        s.bpsd = s.cpl / float(s.subdiv);
        s.stepIndex = 0;
        s.whichRiff = r.coin(s.b / 100.0f) ? 1 + r.below(7) : 0;
    }
    const float left = s.cpl - s.phrasepos;
    b.len = s.bpsd;
    if (b.len > left - 0.001f) b.len = left;
    setCuts(b, 1, b.len);
    const int k = s.stepIndex % s.subdiv;
    int idx;
    if (s.subdiv >= 8) {
        const int unit = s.subdiv / 8;
        idx = int(kPerm[s.whichRiff][(k / unit) % 8]) * unit + k % unit;
    } else {
        idx = int(kPerm[s.whichRiff][(k * 2) % 8]) / 2;
    }
    if (r.coin(s.b / 400.0f)) idx ^= 1;   // a swap inside the pair
    b.offProp = float(idx % s.subdiv) / float(s.subdiv);
    b.roll = false;
    s.stepIndex += 1;
}

void thrash(State& s) {
    Rng& r = *s.r;
    // Riff database: {blockLen, type, extra...}; types 0 single kick, 1 single
    // snare, 2 roll kick, 3 roll snare, 5 prescribed (count, type, duty, amp).
    struct E {
        float len;
        int type;
        float x[4];
        int nx;
    };
    static const E kDb[4][8] = {
        {{1, 2, {}, 0}, {2, 1, {1}, 1}, {1, 2, {}, 0}, {4, 5, {4, 0, 1, 1}, 4}},
        {{0.5f, 2, {}, 0}, {1, 1, {0.25f}, 1}, {0.5f, 2, {}, 0}, {0.5f, 2, {}, 0}, {0.5f, 2, {}, 0},
         {0.25f, 1, {1, 0}, 2}, {0.75f, 1, {}, 0}},
        {{0.5f, 2, {}, 0}, {0.5f, 2, {}, 0}, {0.5f, 2, {}, 0}, {0.5f, 2, {}, 0}, {0.5f, 1, {}, 0}, {0.5f, 2, {}, 0},
         {0.5f, 1, {}, 0}, {0.5f, 2, {}, 0}},
        {{0.5f, 2, {}, 0}, {1, 1, {}, 0}, {0.5f, 2, {}, 0}, {0.5f, 1, {}, 0}, {0.5f, 2, {}, 0}, {1, 1, {}, 0}},
    };
    static const int kDbLen[4] = {4, 7, 8, 6};
    const float stop = s.a / 100.0f, snare = s.b / 100.0f, kick = 0.0f;
    if (s.phrasepos >= s.cpl - 0.001f) {
        newPhrase(s, s.loopBeats);
        s.fill = r.coin(0.125f);
        s.didStop = 0;
        s.whichRiff = 0;   // BBenCut default chooseriff = 0
        E riff[8];
        const int rl = kDbLen[s.whichRiff];
        for (int i = 0; i < rl; ++i) riff[i] = kDb[s.whichRiff][i];
        {   // shuffles = 1
            const int a = r.below(rl), b2 = r.below(rl);
            const E t = riff[a];
            riff[a] = riff[b2];
            riff[b2] = t;
        }
        seqBegin(s);
        float done = 0;
        int idx = 0;
        while (done < s.cpl - 0.001f && s.seqBlocks < kMaxSeqBlocks) {
            const float left = s.cpl - done;
            E e = riff[idx++ % rl];
            int type = e.type;
            if (s.fill) {
                if (type == 0) type = 1;
                else if (type == 1) type = 0;
                else if (type == 2) type = 3;
                else if (type == 3) type = 2;
                else if (type == 5) e.x[1] = e.x[1] == 0 ? 1 : (e.x[1] == 1 ? 0 : e.x[1]);
            }
            float amp = 0.8f, duty = 1.0f, rawOff;
            if (type < 5) {
                if (e.nx == 2) amp = e.x[1];
                if (e.nx >= 1) duty = e.x[0];
                rawOff = float(type % 2);
            } else {
                if (e.nx == 4) amp = e.x[3];
                if (e.nx > 2) duty = e.x[2];
                rawOff = e.x[1];
            }
            if (s.didStop == 0 && r.coin(stop)) {
                s.didStop = 1;
                amp = 0.0f;
            }
            const float off = rawOff < 0.5f ? kick : snare;
            amp = amp <= 0.001f ? 0.0f : 1.0f;   // OT: no accents / volume steps, only the stop
            Cut cuts[8];
            int n = 0;
            float bl = e.len;
            if (type > 1) {
                if (type == 5) {
                    const int count = int(e.x[0]);
                    for (int i = 0; i < count && n < 8; ++i) cuts[n++] = Cut{bl / count, bl / count * duty, off, amp};
                } else {
                    static const float kDiv[3] = {0.34f, 0.33f, 0.33f};
                    for (int i = 0; i < 3; ++i) cuts[n++] = Cut{bl * kDiv[i], bl * kDiv[i] * duty, off, amp};
                }
            } else {
                cuts[n++] = Cut{bl, bl * duty, off, amp};
            }
            if (bl > left) {
                bl = left;
                n = 1;
                cuts[0] = Cut{left, left, 0, 0.0f};
            }
            seqAdd(s, cuts, n);
            done += bl;
        }
    }
    seqTake(s, false);
    s.out.roll = s.out.n > 1;
}

void chooseblockFor(State& s) {
    s.out.offBeats = -1;
    s.out.offProp = -1;
    s.out.roll = false;
    switch (s.type) {
        case kClassic: classic(s); break;
        case kChooseCut: chooseCut(s); break;
        case kJungle: jungle(s); break;
        case kWarpCut: warpCut(s); break;
        case kSQPusher1: sqPusher1(s); break;
        case kSQPusher2: sqPusher2(s); break;
        case kCageCut: cageCut(s); break;
        case kTimeline: timeline(s); break;
        case kPermute: permute(s); break;
        default: thrash(s); break;
    }
    if (s.out.len <= 0.0001f) {   // safety: always advance
        float left = s.cpl - s.phrasepos;
        if (left <= 0.0001f) left = 0.25f;
        s.out.len = left;
        setCuts(s.out, 1, left);
    }
    endBlock(s);
}

const float kRates[8] = {1, 2, 3, 4, 6, 8, 12, 16};

}  // namespace

uint8_t rollValue(float hits, int type) {
    int best = 0;
    float bd = 1e9f;
    for (int i = 0; i < 8; ++i) {
        const float d = std::fabs(std::log(hits / kRates[i]));
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return uint8_t(type * 9 + best + 1);
}

const char* bbProcName(int i) { return kProcNames[i < 0 ? 0 : (i >= kBBProcCount ? kBBProcCount - 1 : i)]; }

const ParamDesc* bbProcParam(int proc, int i) {
    if (proc < 0 || proc >= kBBProcCount || i < 0 || i > 1) return nullptr;
    return kProcParams[proc][i].name ? &kProcParams[proc][i] : nullptr;
}

int sliceForOffset(const Slices& s, float prop) {
    if (s.count <= 0) return 0;
    prop -= std::floor(prop);
    const int32_t target = int32_t(prop * 65535.0f + 0.5f) + 64;   // a little tolerance
    // The slice with the largest start at or before the offset (slices need
    // not be sorted); none before it: the earliest slice.
    int best = -1, first = 0;
    int32_t bestStart = -1;
    for (int i = 0; i < s.count; ++i) {
        const int32_t st = s.start[i];
        if (st < int32_t(s.start[first])) first = i;
        if (st <= target && st > bestStart) {
            bestStart = st;
            best = i;
        }
    }
    return best >= 0 ? best : first;
}

void writeSliceHit(Step& st, const Slices& s, int slice, uint8_t instr) {
    st.instrument = instr;
    if (s.beatSlice) {
        st.note = int8_t(24 + slice > 127 ? 127 : 24 + slice);
    } else {
        st.note = int8_t(60);   // kUnityNote: the slice plays at its own pitch
        putFx(st, kFxSliceId, uint8_t(slice));
    }
}

void genBBCut(const int16_t* v, Target& t, Rng& rng, Result& r) {
    if (t.slices.count <= 0) {
        std::snprintf(r.msg, sizeof(r.msg), "Needs a sliced instrument");
        return;
    }
    State& s = *new (gScratch) State();
    s.r = &rng;
    s.type = v[0];
    {
        const ParamDesc* pa = bbProcParam(s.type, 0);
        const ParamDesc* pb = bbProcParam(s.type, 1);
        s.a = pa ? int16_t(v[1] < pa->min ? pa->min : (v[1] > pa->max ? pa->max : v[1])) : 0;
        s.b = pb ? int16_t(v[2] < pb->min ? pb->min : (v[2] > pb->max ? pb->max : v[2])) : 0;
    }
    static const float kSrc[6] = {0, 1, 2, 4, 8, 16};
    float loop = kSrc[v[3]];
    if (loop <= 0) {   // Auto: the sample length at the song tempo, as 1/2/4/8/16 beats
        loop = 4;
        if (t.slices.sampleBeats > 0.1f) {
            float best = 1, bd = 1e9f;
            for (float c : {1.0f, 2.0f, 4.0f, 8.0f, 16.0f}) {
                const float d = std::fabs(std::log(t.slices.sampleBeats / c));
                if (d < bd) {
                    bd = d;
                    best = c;
                }
            }
            loop = best;
        }
    }
    s.loopBeats = loop;
    s.cpl = 0;
    s.phrasepos = 0;
    s.bpsd = 0.5f;
    s.unitsdone = s.totalunits = 0;
    s.phrase = 0;

    // Options (page 2): how runs of short cuts are written, a random pitch
    // per hit, and whether Micro-move / Volume FX are written.
    enum RollStyle { kRollClean, kRollFadeOut, kRollFadeIn, kRollUp, kRollDown, kRollStutter, kRollSingle };
    const int rollStyle = v[6] < 0 || v[6] > 6 ? 0 : v[6];
    const int pitchRange = v[7] < 0 || v[7] > 3 ? 0 : v[7];
    const bool timing = v[8] == 0 && s.type != kThrash;
    const bool dynamics = v[9] == 0;
    Rng prng(rng.s ^ 0x7F4A7C15u);   // pitch draws: the cuts stay the same with or without them
    auto pitchHit = [&](Step& sp) {
        if (pitchRange <= 0 || sp.note < 0) return;
        const int d = prng.range(-pitchRange, pitchRange);
        if (d == 0) return;
        if (t.slices.beatSlice) putFx(sp, kFxTune, uint8_t(24 + d));    // the note picks the slice
        else sp.note = int8_t(sp.note + d);                              // Slice mode: unity note +- d
    };

    const int spb = t.stepsPerBeat > 0 ? t.stepsPerBeat : 4;
    const int nSteps = t.s1 - t.s0 + 1;
    const float total = float(nSteps) / float(spb);
    Pattern& p = *t.pat;
    // Clear the target first.
    for (int tr = t.t0; tr <= t.t1; ++tr)
        for (int i = 0; i < nSteps; ++i) p.tracks[tr].steps[t.s0 + i].clear();

    float pos = 0;   // beats into the selection
    int hits = 0, rolls = 0;
    int guard = 0;
    while (pos < total - 0.00001f && ++guard < 4096) {
        chooseblockFor(s);
        const Block& b = s.out;
        float cpos = pos;
        for (int i = 0; i < b.n; ++i) {
            const Cut& c = b.c[i];
            if (cpos >= total - 0.00001f) break;
            float prop;
            if (c.off >= 0) prop = c.off;
            else if (b.offProp >= 0) prop = b.offProp;
            else if (b.offBeats >= 0) prop = b.offBeats / s.loopBeats;
            else prop = 0;
            const int slice = sliceForOffset(t.slices, prop);
            const float startSteps = cpos * float(spb);
            const float ioiSteps = c.ioi * float(spb);
            if (c.dur < 0.001f) {
                cpos += c.ioi;
                continue;
            }
            if (ioiSteps >= 0.75f) {
                const int st = int(std::floor(startSteps + 0.02f));
                if (st >= nSteps) break;
                const float frac = startSteps - float(st);
                // A cut plays on from its offset like the audio would: each
                // step it covers triggers the slice under the advancing
                // source position (a slice only plays its own length), only
                // when that slice changes.
                const float durSteps = c.dur * float(spb);
                int span = int(std::floor(durSteps + 0.25f));
                if (span < 1) span = 1;
                int lastSlice = -1;
                for (int d = 0; d < span && st + d < nSteps; ++d) {
                    const int sl = d == 0 ? slice
                                          : sliceForOffset(t.slices, prop + float(d) / (float(spb) * s.loopBeats));
                    if (d > 0 && sl == lastSlice) continue;
                    lastSlice = sl;
                    for (int tr = t.t0; tr <= t.t1; ++tr) {
                        Step& sp = p.tracks[tr].steps[t.s0 + st + d];
                        sp.clear();
                        if (c.amp <= 0.001f) {
                            if (d == 0) sp.note = kNoteOff;
                            continue;
                        }
                        writeSliceHit(sp, t.slices, sl, t.instrument);
                        if (d == 0 && timing && frac >= 0.1f) putFx(sp, kFxMicroMove, uint8_t(frac * 100.0f + 0.5f));
                        if (dynamics && (c.amp < 0.95f || c.amp > 1.05f)) {
                            int vol = int(c.amp * 100.0f / 1.1f + 0.5f);
                            putFx(sp, kFxVolume, uint8_t(vol > 100 ? 100 : vol));
                        }
                        if (d == span - 1 && durSteps < ioiSteps * 0.9f && durSteps - float(d) < 1.0f)
                            putFx(sp, kFxGate, uint8_t(5.0f + (durSteps - float(d)) * 95.0f));
                        if (d == 0) pitchHit(sp);
                    }
                    if (c.amp <= 0.001f) break;
                }
                ++hits;
                cpos += c.ioi;
            } else {
                // Run of short cuts: Roll FX step by step until the cuts are
                // a step or longer again (or the block ends).
                int j = i;
                float runEnd = cpos;
                while (j < b.n && b.c[j].ioi * float(spb) < 0.75f) runEnd += b.c[j++].ioi;
                const int st0 = int(std::floor(startSteps + 0.02f));
                int st1 = int(std::floor(runEnd * float(spb) + 0.02f));
                if (st1 <= st0) st1 = st0 + 1;
                float q = cpos;
                int k = i;
                for (int st = st0; st < st1 && st < nSteps; ++st) {
                    // cuts inside this step -> its local rate
                    const float stepEnd = float(st + 1) / float(spb);
                    int cnt = 0;
                    while (k < j && q < stepEnd - 0.0001f) {
                        q += b.c[k++].ioi;
                        ++cnt;
                    }
                    if (cnt == 0) cnt = 1;
                    for (int tr = t.t0; tr <= t.t1; ++tr) {
                        Step& sp = p.tracks[tr].steps[t.s0 + st];
                        sp.clear();
                        if (rollStyle == kRollSingle) {
                            if (st == st0) {
                                writeSliceHit(sp, t.slices, slice, t.instrument);
                                pitchHit(sp);
                            }
                            continue;
                        }
                        if (rollStyle == kRollStutter) {   // a fresh hit on every step, no Roll FX
                            writeSliceHit(sp, t.slices, slice, t.instrument);
                            if (st != st0) putFx(sp, kFxGate, 50);
                            continue;
                        }
                        if (st == st0) writeSliceHit(sp, t.slices, slice, t.instrument);
                        const int type = rollStyle == kRollFadeOut ? 1 : (rollStyle == kRollFadeIn ? 2 : 0);
                        putFx(sp, kFxRoll, rollValue(float(cnt), type));
                        if (st == st0 && rollStyle == kRollUp) putFx(sp, kFxSlideUp, 24);
                        if (st == st0 && rollStyle == kRollDown) putFx(sp, kFxSlideDown, 24);
                    }
                    ++rolls;
                }
                // A roll keeps going until the next note: end it where the run
                // ends (a hit written there later replaces this).
                if (rollStyle != kRollSingle && rollStyle != kRollStutter && st1 < nSteps)
                    for (int tr = t.t0; tr <= t.t1; ++tr) {
                        Step& sp = p.tracks[tr].steps[t.s0 + st1];
                        if (sp.note < 0 && sp.fx[0].type == 0 && sp.fx[1].type == 0) putFx(sp, kFxRoll, 0);
                    }
                cpos = runEnd;
                i = j - 1;
            }
        }
        pos += b.len;
    }
    s.~State();
    std::snprintf(r.msg, sizeof(r.msg), "%s: %d hits, %d roll steps", bbProcName(v[0]), hits, rolls);
    r.ok = true;
}

void bbPlayTable(int proc, int a, int b, uint32_t seed, uint8_t out[128][3]) {
    Rng rng(seed);
    State& s = *new (gScratch) State();
    s.r = &rng;
    s.type = proc < 0 ? 0 : (proc >= kBBProcCount ? kBBProcCount - 1 : proc);
    const ParamDesc* pa = bbProcParam(s.type, 0);
    const ParamDesc* pb = bbProcParam(s.type, 1);
    s.a = pa ? int16_t(a < pa->min || a > pa->max ? pa->def : a) : 0;
    s.b = pb ? int16_t(b < pb->min || b > pb->max ? pb->def : b) : 0;
    s.loopBeats = 4;
    s.bpsd = 0.5f;
    constexpr int kSpb = 4;
    for (int k = 0; k < 128; ++k) {   // default: straight
        out[k][0] = 0;
        out[k][1] = uint8_t(k);
        out[k][2] = 0;
    }
    float pos = 0;
    int guard = 0;
    while (pos < 32.0f - 0.0001f && ++guard < 2048) {
        chooseblockFor(s);
        const Block& b = s.out;
        float cpos = pos;
        for (int i = 0; i < b.n; ++i) {
            const Cut& c = b.c[i];
            float prop;
            if (c.off >= 0) prop = c.off;
            else if (b.offProp >= 0) prop = b.offProp;
            else if (b.offBeats >= 0) prop = b.offBeats / s.loopBeats;
            else prop = 0;
            prop -= std::floor(prop);
            const int st0 = int(std::floor(cpos * kSpb + 0.02f));
            const int st1 = int(std::floor((cpos + c.ioi) * kSpb + 0.02f));
            const bool sub = c.ioi * kSpb < 0.75f;
            const float hits = sub ? 1.0f / (c.ioi * kSpb) : 1.0f;
            // 16 steps = the loop: an offset prop is prop * 128 of the track.
            const uint8_t off = uint8_t(int(prop * 128.0f + 0.5f) & 127);
            for (int st = st0; st < (st1 > st0 ? st1 : st0 + 1) && st < 128; ++st) {
                out[st][0] = off;
                out[st][1] = uint8_t(sub ? 0 : (st - st0 > 127 ? 127 : st - st0));
                out[st][2] = uint8_t(sub ? (hits >= 2.5f ? 3 : 9) : 0);   // 100 % rolls: 4 hits / 2 hits
            }
            cpos += c.ioi;
        }
        pos += b.len;
    }
    s.~State();
}

}  // namespace gen
}  // namespace ot
