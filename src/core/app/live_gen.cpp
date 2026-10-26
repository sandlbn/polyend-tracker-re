// Live generators (live_gen.h).
#include "app/live_gen.h"

#include <cstring>

#include "app/app.h"
#include "sdram_bss.h"
#include "sequencer/seq_playmodes.h"

namespace ot {
namespace live {

namespace {

struct AcidSlot {
    bool used = false;
    uint8_t track = 0;
    uint8_t pattern = 0;
    int16_t s0 = 0, n = 0;
    int16_t lastStep = -1;
    int16_t v[gen::kParams] = {};
    uint8_t instrument = 0;
    int16_t rootNote = 36;
    uint16_t scaleMask = 0;
    uint32_t rng = 1;
    int8_t applied = 0;       // density the steps were written with
    uint16_t loops = 0;
    uint32_t hash = 0;        // of the steps as written (a hand edit ends the slot)
    gen::MonoStep ph[kStepCount];
};

struct BBSlot {
    bool used = false;
    uint8_t t0 = 0, t1 = 0, pattern = 0;
    int16_t s0 = 0, n = 0, lastStep = -1;
    int16_t v[gen::kParams] = {};
    uint8_t instrument = 0;
    int16_t rootNote = 36;
    uint16_t scaleMask = 0;
    int16_t sliceCount = 0;
    uint16_t slices[kSliceCount] = {};
    bool beatSlice = false;
    float sampleBeats = 0;
    uint32_t rng = 1;
    uint16_t loops = 0;
    uint32_t hash = 0;
};

struct State {
    AcidSlot acid[kAcidSlots];
    BBSlot bb[kBBSlots];
    int lastPos = -1;          // pattern position (B mode re-cuts per loop)
    uint16_t loops = 0;
    uint8_t tableTmp[seq::kPlayModeTableLen][3];
    int16_t bbA[10] = {}, bbB[10] = {};
    bool bbSet[10] = {};
    int8_t densOff[kTrackCount] = {};
    bool wasPlaying = true;   // builds the tables on the first stopped pass
    uint32_t tableSeed = 0x2545F491u;
};

// Main-loop data only: SDRAM on the device.
OT_SDRAM_BSS SdramLazy<State> gState;

gen::Target targetOf(App& a, const AcidSlot& s) {
    gen::Target t;
    t.pat = &a.pattern();
    t.t0 = t.t1 = s.track;
    t.s0 = s.s0;
    t.s1 = s.s0 + s.n - 1;
    t.instrument = s.instrument;
    t.rootNote = s.rootNote;
    t.scaleMask = s.scaleMask;
    return t;
}

uint32_t hashRange(const Pattern& p, int t0, int t1, int s0, int n) {
    uint32_t h = 2166136261u;
    for (int t = t0; t <= t1; ++t) {
        const Track& tr = p.tracks[t];
        for (int i = 0; i < n && s0 + i < kStepCount; ++i) {
            const Step& st = tr.steps[s0 + i];
            const uint8_t b[6] = {uint8_t(st.note), st.instrument, st.fx[0].type, st.fx[0].value, st.fx[1].type,
                                  st.fx[1].value};
            for (uint8_t x : b) h = (h ^ x) * 16777619u;
        }
    }
    return h;
}
uint32_t hashSteps(const Pattern& p, const AcidSlot& s) { return hashRange(p, s.track, s.track, s.s0, s.n); }
uint32_t hashSteps(const Pattern& p, const BBSlot& s) { return hashRange(p, s.t0, s.t1, s.s0, s.n); }

// New cut tables for the B play modes (time mixed into the seed: a new set
// every power-on too). Built aside and copied in one go per mode.
void rebuildTables(App& a, State& st) {
    for (int m = 0; m < seq::kBBModeCount; ++m) {
        st.tableSeed = st.tableSeed * 1103515245u + 12345u + a.micros();
        gen::bbPlayTable(m, st.bbSet[m] ? st.bbA[m] : -1, st.bbSet[m] ? st.bbB[m] : -1, st.tableSeed | 1u,
                         st.tableTmp);
        std::memcpy(seq::gBBPlayTables[m], st.tableTmp, sizeof(st.tableTmp));
    }
    seq::gBBPlayTablesReady = true;
}

// BBCut Mutate: the same settings, a new random draw over the slot's steps.
void recut(App& a, BBSlot& s) {
    gen::Target t;
    t.pat = &a.pattern();
    t.t0 = s.t0;
    t.t1 = s.t1;
    t.s0 = s.s0;
    t.s1 = s.s0 + s.n - 1;
    t.instrument = s.instrument;
    t.rootNote = s.rootNote;
    t.scaleMask = s.scaleMask;
    t.slices.count = s.sliceCount;
    t.slices.start = s.slices;
    t.slices.beatSlice = s.beatSlice;
    t.slices.sampleBeats = s.sampleBeats;
    s.rng = s.rng * 1664525u + 1013904223u;
    t.entropy = s.rng;
    int16_t v[gen::kParams];
    for (int i = 0; i < gen::kParams; ++i) v[i] = s.v[i];
    v[gen::kSeedParam] = 0;   // random: the entropy above
    gen::generate(gen::kBBCut, v, t);
    s.hash = hashSteps(a.pattern(), s);
}

int effectiveDensity(const State& st, const AcidSlot& s) {
    const int d = s.v[0] + st.densOff[s.track];
    return d < -7 ? -7 : (d > 7 ? 7 : d);
}

void write(App& a, AcidSlot& s) {
    gen::writeMono(a.pattern(), s.track, s.s0, s.ph, s.n, s.instrument);
    s.hash = hashSteps(a.pattern(), s);
}

}  // namespace

void service(App& a) {
    State& st = gState.get();
    const bool playing = a.seq.isPlaying();
    // B play modes (Config > BB play modes): new cuts at every stop, every N
    // loops while playing, or locked (built once).
    const uint8_t reseed = a.cfg.bbReseed;
    if (!playing && st.wasPlaying) {
        if (reseed != Config::kBBLocked || !seq::gBBPlayTablesReady) rebuildTables(a, st);
        for (AcidSlot& s : st.acid) s.lastStep = -1;
        for (BBSlot& s : st.bb) s.lastStep = -1;
        st.lastPos = -1;
        st.loops = 0;
    }
    st.wasPlaying = playing;
    if (playing) {
        const int pos = a.seq.patternPosition();
        if (st.lastPos >= 0 && pos < st.lastPos) {
            ++st.loops;
            static const uint16_t kN[5] = {0, 1, 2, 4, 8};
            if (reseed >= 1 && reseed <= 4 && st.loops % kN[reseed] == 0) rebuildTables(a, st);
        }
        st.lastPos = pos;
    }
    bool edited = false;
    static const uint16_t kEvery[4] = {1, 2, 4, 8};
    for (AcidSlot& s : st.acid) {
        if (!s.used) continue;
        if (a.patternNumber() != s.pattern) continue;
        if (hashSteps(a.pattern(), s) != s.hash) {   // edited by hand: hands off
            s.used = false;
            continue;
        }
        bool dirty = false;
        if (playing) {
            const int now = a.seq.step(s.track);
            // The loop end: the track's step went back (forward play).
            if (s.lastStep >= 0 && now < s.lastStep) {
                ++s.loops;
                const int every = kEvery[s.v[6] < 0 || s.v[6] > 3 ? 0 : s.v[6]];
                if (s.v[4] > 0 && s.loops % every == 0) {
                    gen::Rng rng(s.rng);
                    gen::Target t = targetOf(a, s);
                    gen::acidMutate(s.v, t, rng, s.ph, s.n, s.v[4] / 200.0f);
                    s.rng = rng.s;
                    s.applied = int8_t(100);   // force the rewrite below
                }
            }
            s.lastStep = int16_t(now);
        }
        const int d = effectiveDensity(st, s);
        if (d != s.applied) {
            gen::acidApplyDensity(s.ph, s.n, d, s.rootNote);
            s.applied = int8_t(d);
            dirty = true;
        }
        if (dirty) {
            write(a, s);
            edited = true;
        }
    }
    static const uint16_t kBBEvery[5] = {0, 1, 2, 4, 8};
    for (BBSlot& s : st.bb) {
        if (!s.used || a.patternNumber() != s.pattern) continue;
        if (hashSteps(a.pattern(), s) != s.hash) {   // edited by hand: hands off
            s.used = false;
            continue;
        }
        if (!playing) continue;
        const int now = a.seq.step(s.t0);
        if (s.lastStep >= 0 && now < s.lastStep) {
            ++s.loops;
            const int m = s.v[4] < 1 || s.v[4] > 4 ? 1 : s.v[4];
            if (s.loops % kBBEvery[m] == 0) {
                recut(a, s);
                edited = true;
            }
        }
        s.lastStep = int16_t(now);
    }
    if (edited) a.patternEdited();
}

void acid(App& a, const int16_t* v, const gen::Target& t, uint32_t rngSeed) {
    State& st = gState.get();
    // An existing slot for the track, else a free one, else the oldest.
    AcidSlot* slot = nullptr;
    for (AcidSlot& s : st.acid)
        if (s.used && s.track == t.t0) slot = &s;
    if (!slot)
        for (AcidSlot& s : st.acid)
            if (!s.used) {
                slot = &s;
                break;
            }
    if (!slot) {
        for (int i = 0; i + 1 < kAcidSlots; ++i) st.acid[i] = st.acid[i + 1];
        slot = &st.acid[kAcidSlots - 1];
    }
    slot->used = true;
    slot->track = uint8_t(t.t0);
    slot->pattern = uint8_t(a.patternNumber());
    slot->s0 = int16_t(t.s0);
    slot->n = int16_t(t.s1 - t.s0 + 1);
    for (int i = 0; i < gen::kParams; ++i) slot->v[i] = v[i];
    slot->instrument = t.instrument;
    slot->rootNote = int16_t(t.rootNote);
    slot->scaleMask = t.scaleMask;
    slot->lastStep = -1;
    slot->loops = 0;
    // The same phrase the generate wrote (same RNG seed, same order).
    gen::Rng rng(rngSeed);
    gen::acidPhrase(v, t, rng, slot->ph, slot->n);
    slot->rng = rng.s;
    slot->applied = int8_t(v[0] < -7 ? -7 : (v[0] > 7 ? 7 : v[0]));
    slot->hash = hashSteps(a.pattern(), *slot);
    // A density offset already on: the next service pass applies it.
}

void release(int track) {
    for (AcidSlot& s : gState.get().acid)
        if (s.used && s.track == track) s.used = false;
    for (BBSlot& s : gState.get().bb)
        if (s.used && track >= s.t0 && track <= s.t1) s.used = false;
}

bool acidActive(int track) {
    for (const AcidSlot& s : gState.get().acid)
        if (s.used && s.track == track) return true;
    return false;
}

void setAcidDensityOffset(int track, int offset) {
    if (track < 0 || track >= kTrackCount) return;
    gState.get().densOff[track] = int8_t(offset < -14 ? -14 : (offset > 14 ? 14 : offset));
}

void setBBParams(int proc, int a, int b) {
    if (proc < 0 || proc >= 10) return;
    State& st = gState.get();
    st.bbA[proc] = int16_t(a);
    st.bbB[proc] = int16_t(b);
    st.bbSet[proc] = true;
}

void bbcut(App& a, const int16_t* v, const gen::Target& t, uint32_t rngSeed) {
    State& st = gState.get();
    if (v[4] <= 0) return;   // Mutate Off: nothing live (release() already ran)
    BBSlot* slot = nullptr;
    for (BBSlot& s : st.bb)
        if (!s.used) {
            slot = &s;
            break;
        }
    if (!slot) {
        st.bb[0] = st.bb[1];
        slot = &st.bb[1];
    }
    *slot = BBSlot{};
    slot->used = true;
    slot->t0 = uint8_t(t.t0);
    slot->t1 = uint8_t(t.t1);
    slot->pattern = uint8_t(a.patternNumber());
    slot->s0 = int16_t(t.s0);
    slot->n = int16_t(t.s1 - t.s0 + 1);
    for (int i = 0; i < gen::kParams; ++i) slot->v[i] = v[i];
    slot->instrument = t.instrument;
    slot->rootNote = int16_t(t.rootNote);
    slot->scaleMask = t.scaleMask;
    slot->sliceCount = int16_t(t.slices.count > kSliceCount ? kSliceCount : t.slices.count);
    for (int i = 0; i < slot->sliceCount; ++i) slot->slices[i] = t.slices.start[i];
    slot->beatSlice = t.slices.beatSlice;
    slot->sampleBeats = t.slices.sampleBeats;
    slot->rng = rngSeed ^ a.micros();
    slot->hash = hashSteps(a.pattern(), *slot);
}

bool bbcutActive(int track) {
    for (const BBSlot& s : gState.get().bb)
        if (s.used && track >= s.t0 && track <= s.t1) return true;
    return false;
}

void stopAll() {
    for (AcidSlot& s : gState.get().acid) s.used = false;
    for (BBSlot& s : gState.get().bb) s.used = false;
}

}  // namespace live
}  // namespace ot
