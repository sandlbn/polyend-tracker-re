// Live generators (live_gen.h).
#include "app/live_gen.h"

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
    gen::MonoStep ph[kStepCount];
};

struct State {
    AcidSlot acid[kAcidSlots];
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

}  // namespace

void service(App& a) {
    State& st = gState.get();
    const bool playing = a.seq.isPlaying();
    if (!playing && st.wasPlaying) {
        // Stopped: new BBCut cut sets for the B play modes. The sequencer does
        // not read the tables while it is stopped.
        for (int m = 0; m < seq::kBBModeCount; ++m) {
            st.tableSeed = st.tableSeed * 1103515245u + 12345u;
            gen::bbPlayTable(m, st.tableSeed | 1u, seq::gBBPlayTables[m]);
        }
        seq::gBBPlayTablesReady = true;
        for (AcidSlot& s : st.acid) s.lastStep = -1;
    }
    st.wasPlaying = playing;
    if (!playing) return;
    bool edited = false;
    for (AcidSlot& s : st.acid) {
        if (!s.used || a.patternNumber() != s.pattern) continue;
        const int now = a.seq.step(s.track);
        // The loop end: the track's step went back (forward play).
        if (s.lastStep >= 0 && now < s.lastStep) {
            gen::Rng rng(s.rng);
            gen::Target t = targetOf(a, s);
            gen::acidMutate(s.v, t, rng, s.ph, s.n, s.v[4] / 200.0f);
            s.rng = rng.s;
            gen::writeMono(a.pattern(), s.track, s.s0, s.ph, s.n, s.instrument);
            edited = true;
        }
        s.lastStep = int16_t(now);
    }
    if (edited) a.patternEdited();
}

void acid(App& a, const int16_t* v, const gen::Target& t, uint32_t rngSeed) {
    State& st = gState.get();
    // An existing slot for the track, else a free one, else the oldest (slot 0).
    AcidSlot* slot = nullptr;
    for (AcidSlot& s : st.acid)
        if (s.used && s.track == t.t0) slot = &s;
    if (v[4] <= 0) {   // Mutate 0: lock
        if (slot) slot->used = false;
        return;
    }
    if (!slot)
        for (AcidSlot& s : st.acid)
            if (!s.used) {
                slot = &s;
                break;
            }
    if (!slot) {
        st.acid[0] = st.acid[1];
        slot = &st.acid[1];
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
    // The same phrase the generate wrote (same RNG seed, same order).
    gen::Rng rng(rngSeed);
    gen::acidPhrase(v, t, rng, slot->ph, slot->n);
    slot->rng = rng.s;
}

bool acidActive(int track) {
    for (const AcidSlot& s : gState.get().acid)
        if (s.used && s.track == track) return true;
    return false;
}

void stopAll() {
    for (AcidSlot& s : gState.get().acid) s.used = false;
}

}  // namespace live
}  // namespace ot
