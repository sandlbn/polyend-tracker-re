// 2.x synth framework (WP-E, docs/MINI_PLAN.md §3; audio/synth_bank.h,
// audio/synth_params.h): parameter access and the macro law (unipolar,
// bipolar, empty targets, M7), voice allocation / stealing / caps, mono
// legato, Stop resetting macro overrides, the governor cutting synth voices
// only, sequencer dispatch of instruments 64..66, the slot mixer and the
// Synth 1..3 stems, unsupported models.
#include "test.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "audio/engine.h"
#include "audio/engines/acd.h"
#include "audio/engines/perc.h"
#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "model/pattern.h"
#include "model/project.h"
#include "sequencer/sequencer.h"

using namespace ot;
using namespace ot::audio;

namespace {

// Test engine wrapper that records the calls.
struct SpyEngine : ISynthEngine {
    ISynthEngine* inner = synthTestEngine();
    std::vector<SynthNote> ons;
    int offs = 0, prepares = 0, resets = 0, renders = 0;
    float tempo = 0.0f;
    int32_t id() const override { return kSynthEngineTest; }
    void setTempo(float bpm) override { tempo = bpm; }
    const char* name() const override { return "SPY"; }
    void prepare(SynthSlotScratch& s, const SynthParams& p) override { ++prepares; inner->prepare(s, p); }
    void slotReset(SynthSlotScratch& s) override { ++resets; inner->slotReset(s); }
    void noteOn(SynthVoice& v, const SynthParams& p, const SynthNote& n) override {
        ons.push_back(n);
        inner->noteOn(v, p, n);
    }
    void noteOff(SynthVoice& v, const SynthParams& p) override {
        ++offs;
        inner->noteOff(v, p);
    }
    bool render(SynthVoice& v, const SynthParams& p, float g0, float gi, float* L, float* R) override {
        ++renders;
        return inner->render(v, p, g0, gi, L, R);
    }
};

uint32_t b(float v) { return patchFloatToBits(v); }

SynthSlotRecord rec(SynthModel m, int poly) {
    SynthSlotRecord r = SynthSlotRecord::defaults(0);
    r.model = uint32_t(m);
    r.polyphony = uint32_t(poly);
    r.gainDb = 0.0f;
    return r;
}

// A bank with three ACD slots (the test engine plays them), polyphony p.
struct BankRig {
    SynthPatch patch[kSynthSlots];
    Governor gov;
    SynthBank bank;
    SpyEngine spy;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    // starts: voice starts per block before note-ons wait a block (note-on
    // spreading); 0 = no limit, the allocation tests' setting.
    explicit BankRig(int p0 = 2, int p1 = 2, int p2 = 2, int budget = 4, int starts = 0) {
        gov.setBudget(budget);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(starts);
        bank.setEngineOverride(&spy);
        const int poly[3] = {p0, p1, p2};
        for (int s = 0; s < kSynthSlots; ++s) {
            patchInit(kSynthEngineAcd, &patch[s]);
            bank.bindPatch(s, &patch[s]);
            bank.setSlotRecord(s, rec(SynthModel::ACD, poly[s]));
        }
        block();
    }
    bool block(int16_t* const* stems = nullptr) {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        SynthMixInputs in;
        return bank.render(in, L, R, D, V, stems);
    }
    void on(int track, int slot, int note, int vol = -1, StepFx a = {}, StepFx bb = {}) {
        SynthNoteOn n;
        n.slot = uint8_t(slot);
        n.note = uint8_t(note);
        n.volume = int8_t(vol);
        n.fxA = a;
        n.fxB = bb;
        bank.noteOn(track, n);
    }
};

// Static engine rig (large buffers off the stack).
int16_t gFx[AudioEngine::kFxMemorySamples];
int16_t gPool[1u << 16];
Instrument gIns[kSampleInstrumentCount];
AudioEngine gEng;
SynthPatch gPatch[kSynthSlots];
SpyEngine gSpy;

void engineRig(int poly = 4) {
    for (auto& i : gIns) i = Instrument();
    gEng.settings() = MixerSettings();
    gEng.init(gFx, AudioEngine::kFxMemorySamples, gPool, sizeof(gPool) / 2, gIns);
    gEng.clearEffects();
    gEng.stopAll();
    gEng.synth().reset();
    gEng.synth().setStartsPerBlock(0);   // allocation / governor tests: every note starts at once
    gEng.synth().setEngineOverride(&gSpy);
    for (int s = 0; s < kSynthSlots; ++s) {
        patchInit(kSynthEngineAcd, &gPatch[s]);
        gEng.synth().bindPatch(s, &gPatch[s]);
        gEng.synth().setSlotRecord(s, rec(SynthModel::ACD, poly));
    }
}

}  // namespace

// --- parameters and the macro law ------------------------------------------------

TEST(synth_param_get_set_ranges) {
    SynthPatch p;
    CHECK(patchInit(kSynthEngineAcd, &p));
    CHECK_EQ(synthParamCount(kSynthEngineAcd), 29);
    CHECK_EQ(synthParamCount(kSynthEnginePerc), 84);
    CHECK_EQ(synthParamCount(kSynthEngineTest), 0);
    CHECK_NEAR(synthParamGet(p, 9), 400.0f, 1e-3);        // ACD cutoff default
    CHECK(synthParamSet(p, 9, 50000.0f));                  // clamped to 20000
    CHECK_NEAR(synthParamGet(p, 9), 20000.0f, 1e-3);
    CHECK(synthParamSet(p, 3, -3.0f));                     // Noise Mix -1..1
    CHECK_NEAR(synthParamGet(p, 3), -1.0f, 1e-6);
    CHECK(!synthParamSet(p, 29, 1.0f));                    // no such index
    CHECK(!synthParamSet(p, 0, NAN));
    // Absent or out-of-range stored value reads as the default.
    p.params[10].present = false;
    CHECK_NEAR(synthParamGet(p, 10), 0.5f, 1e-6);
    p.params[10] = PatchParam{b(9.0f), true};
    CHECK_NEAR(synthParamGet(p, 10), 0.5f, 1e-6);
    const PatchParamRange* r = synthParamRange(kSynthEngineVap, 27);   // LFO 1 Ratio: Hz 0..100 (correction)
    CHECK(r != nullptr);
    CHECK_NEAR(r->max, 100.0f, 1e-6);
}

TEST(synth_macro_law_unipolar_bipolar_empty) {
    SynthPatch p;
    patchInit(kSynthEngineAcd, &p);
    // M1 unipolar -> cutoff (9) +0.5 and resonance (10) -0.2, with an empty
    // target (-1) inside count between them.
    PatchMacro& m1 = p.macros[0];
    m1.bipolar = false;
    m1.count = 3;
    m1.targets[0] = PatchTarget{9, b(0.5f)};
    m1.targets[1] = PatchTarget{-1, b(1.0f)};
    m1.targets[2] = PatchTarget{10, b(-0.2f)};
    // M2 bipolar -> PW (4) +1; M3 also on PW (sum).
    PatchMacro& m2 = p.macros[1];
    m2.bipolar = true;
    m2.count = 1;
    m2.targets[0] = PatchTarget{4, b(1.0f)};
    PatchMacro& m3 = p.macros[2];
    m3.count = 1;
    m3.targets[0] = PatchTarget{4, b(0.25f)};
    // Targets beyond count are ignored.
    p.macros[3].count = 0;
    p.macros[3].targets[0] = PatchTarget{0, b(2.0f)};
    float out[kSynthParamMax];
    uint8_t v[kSynthSeqMacros] = {0, 63, 0, 127, 0, 0};
    CHECK_EQ(synthEffectiveParams(p, v, 1.0f, out), 29);
    CHECK_NEAR(out[9], 400.0f, 1e-3);   // unipolar 0: patch value
    CHECK_NEAR(out[4], 0.5f, 1e-6);     // bipolar centre 63: patch value
    CHECK_NEAR(out[0], 1.0f, 1e-6);     // macro 4 count 0
    v[0] = 127;   // m = 1: cutoff + 0.5 * (20000 - 20)
    synthEffectiveParams(p, v, 1.0f, out);
    CHECK_NEAR(out[9], 400.0f + 0.5f * 19980.0f, 0.05);
    CHECK_NEAR(out[10], 0.5f - 0.2f * 1.5f, 1e-5);
    v[0] = 127 / 2 + 1;   // 64/127
    synthEffectiveParams(p, v, 1.0f, out);
    CHECK_NEAR(out[9], 400.0f + 0.5f * (64.0f / 127.0f) * 19980.0f, 0.05);
    // Bipolar: (v - 63) / 64; 0 -> -63/64, 127 -> +1; clamped to [0, 1].
    v[0] = 0;
    v[1] = 79;   // +0.25 -> 0.75
    synthEffectiveParams(p, v, 1.0f, out);
    CHECK_NEAR(out[4], 0.75f, 1e-6);
    v[1] = 31;   // -0.5 -> 0.0
    synthEffectiveParams(p, v, 1.0f, out);
    CHECK_NEAR(out[4], 0.0f, 1e-6);
    v[1] = 127;
    v[2] = 127;   // 0.5 + 1 + 0.25 -> clamp 1
    synthEffectiveParams(p, v, 1.0f, out);
    CHECK_NEAR(out[4], 1.0f, 1e-6);
    CHECK_NEAR(synthMacroM(0, true), -63.0f / 64.0f, 1e-6);
    CHECK_NEAR(synthMacroM(127, true), 1.0f, 1e-6);
    CHECK_NEAR(synthMacroM(200, false), 1.0f, 1e-6);
    CHECK_EQ(synthMacroDefault(true), 63);
}

TEST(synth_macro7_velocity_law) {
    SynthPatch p;
    patchInit(kSynthEngineAcd, &p);   // M7 Velocity -> Volume (28) +1 (U15)
    CHECK_EQ(p.macros[6].count, 1);
    CHECK_EQ(p.macros[6].targets[0].param, 28);
    float out[kSynthParamMax];
    const uint8_t v[kSynthSeqMacros] = {};
    synthEffectiveParams(p, v, 1.0f, out);
    CHECK_NEAR(out[28], 1.0f, 1e-6);     // no Volume FX: the patch as saved
    synthEffectiveParams(p, v, 0.5f, out);
    CHECK_NEAR(out[28], 0.5f, 1e-6);     // linear in velocity
    synthEffectiveParams(p, v, 0.0f, out);
    CHECK_NEAR(out[28], 0.0f, 1e-6);
    // The bank's voice gain follows the same law (Volume handled by the framework).
    BankRig r(1, 1, 1);
    r.on(0, 0, 60, 50);
    r.block();
    CHECK_NEAR(r.spy.ons.back().velocity, 0.5f, 1e-6);
}

TEST(synth_bank_params_match_reference_and_ramp) {
    BankRig r;
    PatchMacro& m1 = r.patch[0].macros[0];
    m1.count = 2;
    m1.targets[0] = PatchTarget{9, b(0.25f)};
    m1.targets[1] = PatchTarget{-1, b(0.0f)};
    r.bank.patchChanged(0);
    SynthSlotRecord rc = rec(SynthModel::ACD, 2);
    rc.macros[0] = 100;
    r.bank.setSlotRecord(0, rc);
    r.block();
    r.block();
    float ref[kSynthParamMax];
    const uint8_t v[kSynthSeqMacros] = {100, 0, 0, 0, 0, 0};
    synthEffectiveParams(r.patch[0], v, 1.0f, ref);
    const SynthParams& p = r.bank.params(0);
    CHECK_EQ(p.count, 29);
    for (int i = 0; i < p.count; ++i) CHECK_NEAR(p.end[i], ref[i], 1e-3);
    CHECK(!p.moving);
    // A macro change ramps over one block: start = old, end = new.
    r.on(0, 0, 60);
    r.bank.macroFx(0, 0, 0);
    r.block();
    CHECK(p.moving);
    CHECK_NEAR(p.start[9], ref[9], 1e-3);
    CHECK_NEAR(p.end[9], 400.0f, 1e-3);
    r.block();
    CHECK_NEAR(p.start[9], 400.0f, 1e-3);
}

// A live patch edit (Edit Patch) ramps across the next block like a macro
// move, from where the last block ended; two edits before one block ramp
// from the first value to the last.
TEST(synth_patch_edit_ramps) {
    BankRig r;
    r.on(0, 0, 60);
    r.block();
    r.block();
    const SynthParams& p = r.bank.params(0);
    CHECK_NEAR(p.start[9], 400.0f, 1e-3);
    synthParamSet(r.patch[0], 9, 1000.0f);
    r.bank.patchChanged(0);
    r.block();
    CHECK(p.moving);
    CHECK_NEAR(p.start[9], 400.0f, 1e-3);
    CHECK_NEAR(p.end[9], 1000.0f, 1e-3);
    r.block();
    CHECK_NEAR(p.start[9], 1000.0f, 1e-3);
    CHECK_NEAR(p.end[9], 1000.0f, 1e-3);
    synthParamSet(r.patch[0], 9, 700.0f);
    r.bank.patchChanged(0);
    r.on(1, 0, 62);   // a note in between syncs the edit early
    synthParamSet(r.patch[0], 9, 2000.0f);
    r.bank.patchChanged(0);
    r.block();
    CHECK_NEAR(p.start[9], 1000.0f, 1e-3);
    CHECK_NEAR(p.end[9], 2000.0f, 1e-3);
    // An edit while a macro moves: both ramp from the last block's end.
    PatchMacro& m1 = r.patch[0].macros[0];
    m1.count = 1;
    m1.targets[0] = PatchTarget{10, b(0.5f)};
    r.bank.patchChanged(0);
    r.block();
    r.block();
    const float res0 = p.end[10];
    r.bank.macroFx(0, 0, 127);
    r.on(2, 0, 64);   // applies the macro between blocks
    synthParamSet(r.patch[0], 9, 500.0f);
    r.bank.patchChanged(0);
    r.block();
    CHECK_NEAR(p.start[10], res0, 1e-4);
    CHECK(p.end[10] > res0 + 0.1f);
    CHECK_NEAR(p.start[9], 2000.0f, 1e-3);
    CHECK_NEAR(p.end[9], 500.0f, 1e-3);
}

// --- voices ----------------------------------------------------------------------------

TEST(synth_alloc_steal_cap) {
    BankRig r(2, 2, 2, 4);
    // Slot 0, polyphony 2: two notes take two voices, the third steals the oldest.
    r.on(0, 0, 60);
    r.on(1, 0, 62);
    CHECK_EQ(r.bank.voicesOf(0), 2);
    r.block();
    r.on(2, 0, 64);
    CHECK_EQ(r.bank.voicesOf(0), 2);
    CHECK_EQ(r.bank.steals(), 1u);
    CHECK_EQ(r.bank.busyRecords(), 3);   // the stolen one fades for a block
    r.block();
    CHECK_EQ(r.bank.busyRecords(), 2);
    // Same note re-struck on another track steals that note first.
    r.on(3, 0, 62);
    CHECK_EQ(r.bank.voicesOf(0), 2);
    // Global cap 4: slots 1 and 2 add voices up to 4 in total.
    r.on(4, 1, 50);
    r.on(5, 1, 52);
    CHECK_EQ(r.bank.sounding(), 4);
    r.on(6, 2, 70);   // slot 2 has room, the pool does not: global steal
    CHECK_EQ(r.bank.sounding(), 4);
    CHECK_EQ(r.bank.voicesOf(2), 1);
    for (int i = 0; i < 3; ++i) r.block();
    CHECK(r.bank.busyRecords() <= 4);
}

// A flood of notes in one tick (every record busy, notes queued on fading
// records) never sounds more voices than the cap, at any time.
TEST(synth_note_flood_never_exceeds_cap) {
    for (int budget : {4, 3, 6}) {
        BankRig r(8, 8, 8, budget);
        int worst = 0;
        for (int round = 0; round < 3; ++round) {
            for (int t = 0; t < 16; ++t) {
                r.on(t, t % 3, 40 + t + round);
                worst = std::max(worst, r.bank.sounding());
            }
            for (int k = 0; k < 6; ++k) {
                r.block();
                worst = std::max(worst, r.bank.sounding());
            }
        }
        CHECK(worst <= budget);
        CHECK(r.bank.busyRecords() <= kSynthMaxVoices);
        CHECK_EQ(r.bank.sounding(), budget);
    }
}

TEST(synth_polyphony_clamped_at_play) {
    // Stored 8 plays min(8, cap 4); stored 0 is off.
    BankRig r(8, 0, 1, 4);
    for (int t = 0; t < 8; ++t) r.on(t, 0, 40 + t);
    CHECK_EQ(r.bank.voicesOf(0), 4);
    CHECK_EQ(r.bank.slotView(0).polyphony, 8);
    CHECK_EQ(r.bank.slotView(0).playable, 4);
    CHECK(r.bank.slotView(0).limited);
    r.on(9, 1, 60);
    CHECK_EQ(r.bank.voicesOf(1), 0);
    // Release-phase voices are stolen before held ones (quietest in release).
    BankRig q(3, 1, 1, 4);
    q.on(0, 0, 60);
    q.on(1, 0, 62);
    q.on(2, 0, 64);
    for (int i = 0; i < 20; ++i) q.block();
    q.bank.noteOff(1, NoteOffMode::Off);
    for (int i = 0; i < 4; ++i) q.block();
    q.on(3, 0, 65);
    // track 1's voice (in release) went; tracks 0 and 2 still hold theirs
    CHECK_EQ(q.bank.voicesOf(0), 3);
    q.bank.noteOff(0, NoteOffMode::Off);
    q.bank.noteOff(2, NoteOffMode::Off);
    CHECK_EQ(q.spy.offs, 3);
}

TEST(synth_mono_legato_and_track_release) {
    BankRig r(1, 2, 1);
    r.on(0, 0, 60);
    r.block();
    r.on(0, 0, 67);   // same slot, gate held: legato on the same voice
    CHECK_EQ(r.spy.ons.size(), 2u);
    CHECK(r.spy.ons[1].overlap);
    CHECK(r.spy.ons[1].reused);
    CHECK_EQ(r.bank.voicesOf(0), 1);
    CHECK_EQ(r.bank.steals(), 0u);
    r.bank.noteOff(0, NoteOffMode::Off);
    r.block();
    r.on(3, 0, 62);   // still releasing: reused, envelope retriggers
    CHECK(!r.spy.ons[2].overlap);
    CHECK(r.spy.ons[2].reused);
    // Polyphony 2: a new note on the same track releases the old one.
    r.on(5, 1, 50);
    r.on(5, 1, 52);
    CHECK_EQ(r.bank.voicesOf(1), 2);   // old one releasing, new one held
    const int offs = r.spy.offs;
    r.bank.noteOff(5, NoteOffMode::Off);
    CHECK_EQ(r.spy.offs, offs + 1);
    // Cut fades the voice at once.
    r.on(6, 2, 70);
    r.bank.noteOff(6, NoteOffMode::Cut);
    CHECK_EQ(r.bank.voicesOf(2), 0);
}

TEST(synth_pitch_fx_hooks) {
    BankRig r(1, 1, 1);
    r.on(0, 0, 60, -1, StepFx{40, 36});   // Tune +12: bend +12 (the note stays 60)
    r.block();
    CHECK_NEAR(r.spy.ons.back().pitch, 60.0f, 1e-6);
    CHECK_NEAR(r.bank.voiceOfTrack(0)->bend, 12.0f, 1e-6);
    // Glide FX from the previous note (72 incl. tune) to 64: bend starts at
    // +8 and reaches 0 after glideFxMs(20) = ~408 ms (~141 blocks).
    r.on(0, 0, 64, -1, StepFx{19, 20});
    CHECK_NEAR(r.bank.voiceOfTrack(0)->bend, 8.0f, 1e-6);
    r.block();
    const float b1 = r.bank.voiceOfTrack(0)->bend;
    CHECK(b1 < 8.0f && b1 > 7.8f);
    for (int i = 0; i < 160; ++i) r.block();
    CHECK_NEAR(r.bank.voiceOfTrack(0)->bend, 0.0f, 1e-6);
    // Slide up 16 = one semitone over a step (130 bpm default: ~40 blocks).
    r.on(1, 1, 60, -1, StepFx{41, 16});
    r.block();
    CHECK(r.bank.voiceOfTrack(1)->bend > 0.0f);
    for (int i = 0; i < 60; ++i) r.block();
    CHECK_NEAR(r.bank.voiceOfTrack(1)->bend, 1.0f, 1e-5);
    // Empty-step FX on the sounding note: slide down 2 st, Micro-tune.
    r.bank.applyFx(1, 42, 32);
    for (int i = 0; i < 60; ++i) r.block();
    CHECK_NEAR(r.bank.voiceOfTrack(1)->bend, -1.0f, 1e-5);
    r.bank.applyFx(1, 34, 149);   // +50 cents
    r.block();
    CHECK_NEAR(r.bank.voiceOfTrack(1)->bend, 0.5f, 1e-6);
}

// Mean frequency of a mono block run by rising zero crossings (interpolated).
double crossingHz(const std::vector<float>& x) {
    double first = -1, last = -1;
    int n = 0;
    for (size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double t = double(i - 1) + double(-x[i - 1]) / double(x[i] - x[i - 1]);
            if (first < 0) first = t;
            last = t;
            ++n;
        }
    const double sr = 44100.0;
    return n > 1 ? double(n - 1) * sr / (last - first) : 0.0;
}

// Patch glide (Always, 0.3 s) and a Glide FX on the same note must not add
// up: 48 -> 60 starts at 48 (not at 48 - 12 = 36) and stays between the two.
TEST(synth_glide_fx_and_patch_glide_start_pitch) {
    for (int engine : {int(kSynthEngineAcd), int(kSynthEngineVap)}) {
        BankRig r(1, 1, 1);
        r.bank.setEngineOverride(nullptr);
        const SynthModel model = engine == kSynthEngineAcd ? SynthModel::ACD : SynthModel::VAP;
        patchInit(engine, &r.patch[0]);
        if (engine == kSynthEngineAcd) {
            synthParamSet(r.patch[0], acd::kGlideMode, 0.0f);
            synthParamSet(r.patch[0], acd::kGlideTime, 0.3f);
            synthParamSet(r.patch[0], acd::kCutoff, 20000.0f);
            synthParamSet(r.patch[0], acd::kAmpS, 1.0f);
        } else {
            synthParamSet(r.patch[0], 35, 0.0f);   // Glide Mode Always
            synthParamSet(r.patch[0], 36, 0.3f);   // Glide Time
            synthParamSet(r.patch[0], 12, 20000.0f);
            synthParamSet(r.patch[0], 17, 1.0f);
        }
        r.bank.patchChanged(0);
        r.bank.setSlotRecord(0, rec(model, 1));
        r.block();
        // Reference: the steady note 48 as this measure sees it (harmonics
        // may double the crossing count; the ratio is what matters).
        auto measure = [&r]() {
            std::vector<float> x;
            for (int i = 0; i < 14; ++i) {   // ~40 ms
                r.block();
                x.insert(x.end(), r.L, r.L + kBlock);
            }
            return crossingHz(x);
        };
        r.on(0, 0, 48);
        for (int i = 0; i < 60; ++i) r.block();
        const double ref48 = measure();
        r.on(0, 0, 60, -1, StepFx{19, 20});   // Glide FX ~408 ms
        const double hz = measure();
        // Starts at 48 (the first 40 ms move ~1.2 st), never below it.
        CHECK(ref48 > 0.0);
        CHECK(hz > ref48 * 0.98 && hz < ref48 * 1.25);
    }
}

TEST(synth_stop_resets_macro_overrides) {
    BankRig r;
    PatchMacro& m1 = r.patch[1].macros[0];
    m1.count = 1;
    m1.targets[0] = PatchTarget{10, b(0.5f)};   // resonance 0.5 -> +0.75 at m = 1
    r.bank.patchChanged(1);
    SynthSlotRecord rc = rec(SynthModel::ACD, 2);
    rc.macros[0] = 0;
    r.bank.setSlotRecord(1, rc);
    r.block();
    CHECK_NEAR(r.bank.params(1).end[10], 0.5f, 1e-6);
    r.bank.macroFx(1, 0, 127);   // FX CC a on a Synth 2 step
    r.block();
    CHECK_NEAR(r.bank.params(1).end[10], 1.25f, 1e-6);
    CHECK(r.bank.slotView(1).overridden[0]);
    CHECK_EQ(r.bank.slotView(1).macro[0], 127);
    r.bank.transportStop();
    r.block();
    CHECK_NEAR(r.bank.params(1).end[10], 0.5f, 1e-6);
    CHECK(!r.bank.slotView(1).overridden[0]);
    CHECK_EQ(r.bank.slotView(1).macro[0], 0);
}

TEST(synth_unsupported_engine_silent) {
    // An engine missing from the build (WTFM hidden; every Mini model has an
    // engine since 2.2): the slot keeps its patch, plays nothing and reports
    // "not available".
    audio::synthEngineHide(kSynthEngineWtfm, true);
    BankRig r;
    patchInit(kSynthEngineWtfm, &r.patch[0]);
    r.bank.setEngineOverride(nullptr);
    r.bank.setSlotRecord(0, rec(SynthModel::WTFM, 2));
    r.block();
    CHECK(!r.bank.slotAvailable(0));
    CHECK(!r.bank.slotView(0).available);
    CHECK_STR(r.bank.slotView(0).model, "WTFM");
    r.on(0, 0, 60);
    CHECK_EQ(r.bank.voicesOf(0), 0);
    CHECK(!r.block());
    // The patch is untouched: it encodes byte-identically to a fresh Init.
    SynthPatch fresh;
    patchInit(kSynthEngineWtfm, &fresh);
    uint8_t a[kPatchMaxFileSize], c[kPatchMaxFileSize];
    size_t na = 0, nc = 0;
    CHECK(encodePatch(r.patch[0], a, sizeof a, &na) == PatchStatus::kOk);
    CHECK(encodePatch(fresh, c, sizeof c, &nc) == PatchStatus::kOk);
    CHECK(na == nc && std::memcmp(a, c, na) == 0);
    // A patch of another engine than the model: silent and flagged.
    patchInit(kSynthEngineVap, &r.patch[1]);
    r.bank.patchChanged(1);
    r.block();
    CHECK(r.bank.slotView(1).patchMismatch);
    r.on(1, 1, 60);
    CHECK_EQ(r.bank.voicesOf(1), 0);
    // ACD / VAP / PERC are available (registry).
    r.bank.setSlotRecord(2, rec(SynthModel::PERC, 1));
    patchInit(kSynthEnginePerc, &r.patch[2]);
    r.bank.patchChanged(2);
    r.block();
    CHECK(r.bank.slotAvailable(2));
    audio::synthEngineHide(kSynthEngineWtfm, false);
}

TEST(synth_wtfm_available_and_sounding) {
    // WTFM has an engine since 2.2: available, its voices start and sound.
    BankRig r;
    patchInit(kSynthEngineWtfm, &r.patch[0]);
    r.bank.setEngineOverride(nullptr);
    r.bank.patchChanged(0);
    r.bank.setSlotRecord(0, rec(SynthModel::WTFM, 2));
    r.block();
    CHECK(r.bank.slotAvailable(0));
    CHECK(r.bank.slotView(0).available);
    CHECK(!r.bank.slotView(0).patchMismatch);
    CHECK_STR(r.bank.slotView(0).model, "WTFM");
    r.on(0, 0, 60);
    r.on(1, 0, 67);
    CHECK_EQ(r.bank.voicesOf(0), 2);
    double e = 0.0;
    for (int b = 0; b < 20; ++b) {
        CHECK(r.block());
        for (uint32_t i = 0; i < kBlock; ++i) e += double(r.L[i]) * r.L[i];
    }
    CHECK(e > 1.0);
}

TEST(synth_fat_available_and_sounding) {
    // FAT has an engine since 2.1: available, its voices start and sound.
    BankRig r;
    patchInit(kSynthEngineFat, &r.patch[0]);
    r.bank.setEngineOverride(nullptr);
    r.bank.patchChanged(0);
    r.bank.setSlotRecord(0, rec(SynthModel::FAT, 2));
    r.block();
    CHECK(r.bank.slotAvailable(0));
    CHECK(r.bank.slotView(0).available);
    CHECK(!r.bank.slotView(0).patchMismatch);
    CHECK_STR(r.bank.slotView(0).model, "FAT");
    r.on(0, 0, 60);
    r.on(1, 0, 67);
    CHECK_EQ(r.bank.voicesOf(0), 2);
    double e = 0.0;
    for (int b = 0; b < 20; ++b) {
        CHECK(r.block());
        for (uint32_t i = 0; i < kBlock; ++i) e += double(r.L[i]) * r.L[i];
    }
    CHECK(e > 1.0);
}

TEST(synth_voice_memory_budget) {
    // Voice state in SRAM: the pool is at most 1.5 KB.
    CHECK(sizeof(SynthVoice) * kSynthMaxVoices <= 1536);
    CHECK_EQ(kSynthDefaultVoices, 4);
    CHECK_EQ(Governor::kDefaultBudget, 4);
}

// --- engine integration: governor, mixer, stems ------------------------------------------

TEST(synth_governor_cuts_synth_voices_only) {
    engineRig(4);
    // One sample voice on track 0.
    std::vector<int16_t> tone(44100);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = int16_t(8000.0 * std::sin(i * 0.05));
    gIns[0].lengthFrames = uint32_t(tone.size());
    gIns[0].playMode = uint8_t(PlayMode::ForwardLoop);
    gEng.bank().store(0, tone.data(), uint32_t(tone.size()), false);
    VoiceNoteOn vn;
    vn.instrument = 0;
    vn.note = 60;
    gEng.voiceNoteOn(0, vn);
    int16_t out[kBlock * 2];
    gEng.renderLive(out);
    for (int t = 0; t < 4; ++t) {
        SynthNoteOn n;
        n.slot = 0;
        n.note = uint8_t(60 + t);
        gEng.synthNoteOn(uint8_t(8 + t), n);
    }
    gEng.renderLive(out);
    CHECK_EQ(gEng.governor().sounding(), 4);
    gEng.governor().evaluate(1000, 400, false);  // overrun cut: C := 3
    CHECK_EQ(gEng.governor().cap(), 3);
    CHECK_EQ(gEng.synth().sounding(), 3);
    CHECK(gEng.voice(0).active());               // the sample voice is untouched
    // New notes respect the cap.
    SynthNoteOn n;
    n.slot = 1;
    n.note = 72;
    gEng.synthNoteOn(14, n);
    CHECK_EQ(gEng.synth().sounding(), 3);
    gEng.stopAll();
    CHECK_EQ(gEng.synth().sounding(), 0);
    CHECK_EQ(gEng.governor().cap(), 4);
}

TEST(synth_mixer_and_stems) {
    engineRig(2);
    SynthSlotRecord r1 = rec(SynthModel::ACD, 2);
    r1.pan = -100.0f;          // hard left (balance)
    r1.gainDb = 0.0f;
    r1.reverbSend = 100.0f;
    gEng.synth().setSlotRecord(1, r1);
    gEng.beginOffline();
    std::unique_ptr<StemBlock> st(new StemBlock);
    int16_t master[kBlock * 2];
    gEng.renderOfflineBlock(master, st.get());
    SynthNoteOn n;
    n.slot = 1;
    n.note = 69;
    gEng.synthNoteOn(12, n);   // Synth 2 on track 13
    double l = 0, rr = 0, s0 = 0, s2 = 0, m = 0;
    for (int bl = 0; bl < 20; ++bl) {
        gEng.renderOfflineBlock(master, st.get());
        for (uint32_t i = 0; i < kBlock; ++i) {
            l += std::abs(st->data[12][2 * i]);
            rr += std::abs(st->data[12][2 * i + 1]);
            s0 += std::abs(st->data[11][2 * i]) + std::abs(st->data[11][2 * i + 1]);
            s2 += std::abs(st->data[13][2 * i]) + std::abs(st->data[13][2 * i + 1]);
            m += std::abs(master[2 * i]);
        }
    }
    CHECK(l > 1000.0);
    CHECK_EQ(rr, 0.0);
    CHECK_EQ(s0, 0.0);
    CHECK_EQ(s2, 0.0);
    CHECK(m > 0.0);
    double rev = 0;
    for (int bl = 0; bl < 40; ++bl) {
        gEng.renderOfflineBlock(master, st.get());
        for (uint32_t i = 0; i < kBlock; ++i) rev += std::abs(st->data[8][2 * i]);
    }
    CHECK(rev > 0.0);   // the reverb send carries the slot
    gEng.endOffline();
    CHECK_EQ(StemBlock::kStems, 14);
}

// --- engine hooks and the M7 law with the real engines --------------------------------------

namespace {
// One slot of a real engine (no override) in a bare bank.
struct RealRig {
    SynthPatch patch;
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    RealRig(SynthModel m, int32_t engine) {
        gov.setBudget(4);
        bank.setGovernor(&gov);
        patchInit(engine, &patch);
        bank.bindPatch(0, &patch);
        bank.setSlotRecord(0, rec(m, 1));
        block();
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
    }
    void on(int note, int vol = -1) {
        SynthNoteOn n;
        n.slot = 0;
        n.note = uint8_t(note);
        n.volume = int8_t(vol);
        bank.noteOn(0, n);
    }
    // Energy of the main bus (L) and the two send buses over n blocks.
    void run(int n, double& l, double& d, double& v) {
        l = d = v = 0.0;
        for (int k = 0; k < n; ++k) {
            block();
            for (uint32_t i = 0; i < kBlock; ++i) {
                l += double(L[i]) * L[i];
                d += double(D[i]) * D[i];
                v += double(V[i]) * V[i];
            }
        }
    }
};
}  // namespace

// User report (2.1.0): step FX Reverb / Delay Send left synth tracks dry
// (only the slot's send from the synth page worked). They now set the
// note's own send, on top of / instead of the slot's.
TEST(synth_step_fx_sends_reach_the_send_buses) {
    RealRig r(SynthModel::ACD, kSynthEngineAcd);   // slot sends 0 (-inf)
    double l, d, v;
    r.on(48);
    r.run(4, l, d, v);
    CHECK(l > 0.0);
    CHECK(d == 0.0 && v == 0.0);   // dry: the slot sends nothing
    r.bank.applyFx(0, 32, 100);     // Reverb Send 100
    r.run(8, l, d, v);
    CHECK(v > 0.01 * l);
    CHECK(d == 0.0);
    r.bank.applyFx(0, 30, 50);      // Delay Send 50
    r.run(8, l, d, v);
    CHECK(d > 0.0 && v > 0.0);
    // A Reverb Send FX on the note step itself (the reported case).
    r.bank.noteOff(0, NoteOffMode::Cut);
    r.run(4, l, d, v);
    {
        SynthNoteOn n;
        n.slot = 0;
        n.note = 52;
        n.fxA.type = 32;
        n.fxA.value = 80;
        r.bank.noteOn(0, n);
    }
    r.run(8, l, d, v);
    CHECK(l > 0.0);
    CHECK(v > 0.01 * l);
    CHECK(d == 0.0);
    // The next note starts with the slot's sends again.
    r.bank.noteOff(0, NoteOffMode::Cut);
    r.run(4, l, d, v);
    r.on(50);
    r.run(4, l, d, v);
    for (int k = 0; k < 4; ++k) r.block();
    r.run(4, l, d, v);
    CHECK(l > 0.0);
    CHECK(v == 0.0 && d == 0.0);
}

TEST(synth_step_fx_panning) {
    // Panning FX on synth notes (0 left, 50 centre, 100 right), on a later
    // step and on the note step itself; the next plain note is centred.
    RealRig r(SynthModel::ACD, kSynthEngineAcd);
    auto lr = [&](double& el, double& er) {
        el = er = 0.0;
        for (int k = 0; k < 8; ++k) {
            r.block();
            for (uint32_t i = 0; i < kBlock; ++i) {
                el += double(r.L[i]) * r.L[i];
                er += double(r.R[i]) * r.R[i];
            }
        }
    };
    double el, er;
    r.on(48);
    lr(el, er);
    CHECK(el > 0.0 && std::fabs(el - er) < 0.01 * el);   // centred
    r.bank.applyFx(0, 31, 0);                            // hard left
    for (int k = 0; k < 2; ++k) r.block();
    lr(el, er);
    CHECK(el > 0.0 && er < 1e-9 * el);
    r.bank.noteOff(0, NoteOffMode::Cut);
    for (int k = 0; k < 4; ++k) r.block();
    SynthNoteOn n;
    n.slot = 0;
    n.note = 50;
    n.fxB.type = 31;
    n.fxB.value = 100;                                   // hard right on the note step
    r.bank.noteOn(0, n);
    for (int k = 0; k < 2; ++k) r.block();
    lr(el, er);
    CHECK(er > 0.0 && el < 1e-9 * er);
    r.bank.noteOff(0, NoteOffMode::Cut);
    for (int k = 0; k < 4; ++k) r.block();
    r.on(52);
    for (int k = 0; k < 2; ++k) r.block();
    lr(el, er);
    CHECK(el > 0.0 && std::fabs(el - er) < 0.01 * el);
}

namespace {
// RMS of an ACD Init note (sustain held) over blocks 8..39 after the note,
// with the Volume FX vol (-1: none), M7 -> Volume amount a, and an optional
// Volume FX step at block 8 (fxVol >= 0).
double acdRms(int vol, float a, int fxVol = -1) {
    RealRig r(SynthModel::ACD, kSynthEngineAcd);
    synthParamSet(r.patch, acd::kAmpS, 1.0f);
    r.patch.macros[kPatchMacroVelocity].targets[0].amountBits = patchFloatToBits(a);
    r.bank.patchChanged(0);
    r.block();
    r.on(48, vol);
    for (int k = 0; k < 8; ++k) r.block();
    if (fxVol >= 0) r.bank.applyFx(0, 18, uint8_t(fxVol));   // Volume FX on the playing note
    r.block();   // the gain ramps over one block (same window either way)
    double e = 0.0;
    for (int k = 0; k < 32; ++k) {
        r.block();
        for (uint32_t i = 0; i < kBlock; ++i) e += double(r.L[i]) * r.L[i];
    }
    return std::sqrt(e / (32.0 * kBlock));
}
}  // namespace

// M7 law (synth_params.h): p = p0 * (1 + sum(a * (v - 1))), applied to the
// ACD Volume by the framework's voice gain. Output level follows it exactly.
TEST(synth_m7_volume_step_scales_acd_output) {
    const double full = acdRms(-1, 1.0f);
    CHECK(full > 1e-3);
    CHECK_NEAR(acdRms(100, 1.0f) / full, 1.0, 1e-4);   // Volume 100 = no change
    CHECK_NEAR(acdRms(50, 1.0f) / full, 0.5, 1e-3);    // 1 * (1 + 1 * (0.5 - 1))
    CHECK_NEAR(acdRms(25, 1.0f) / full, 0.25, 1e-3);
    CHECK_NEAR(acdRms(50, 0.5f) / full, 0.75, 1e-3);   // 1 * (1 + 0.5 * (0.5 - 1))
    CHECK_NEAR(acdRms(0, 0.5f) / full, 0.5, 1e-3);     // 1 * (1 + 0.5 * (0 - 1))
    // A Volume step on the sounding note (Volume FX 18) moves its level.
    CHECK_NEAR(acdRms(-1, 1.0f, 50) / full, 0.5, 1e-3);
    CHECK_NEAR(acdRms(50, 1.0f, 100) / full, 1.0, 1e-3);
}

TEST(synth_tempo_reaches_engines) {
    BankRig r(1, 1, 1);
    r.bank.setTempo(87.5f);
    r.on(0, 0, 60);
    r.block();
    CHECK_NEAR(r.spy.tempo, 87.5f, 1e-6);
    r.bank.setTempo(140.0f);
    r.block();
    CHECK_NEAR(r.spy.tempo, 140.0f, 1e-6);
    // Through the engine: the sequencer's tempo reaches the slot each block.
    engineRig(1);
    gEng.engineBlockUpdate(101.0f);
    SynthNoteOn n;
    n.slot = 0;
    n.note = 60;
    gEng.synthNoteOn(9, n);
    int16_t out[kBlock * 2];
    gEng.renderLive(out);
    CHECK_NEAR(gSpy.tempo, 101.0f, 1e-6);
}

TEST(synth_perc_drum_sends_feed_slot_buses) {
    const int snareD = perc::kMixer + 5 * perc::kSnare + 2, snareR = snareD + 1;
    const int kickD = perc::kMixer + 5 * perc::kKick + 2, kickR = kickD + 1;
    auto rig = [&](float sd, float sr) {
        std::unique_ptr<RealRig> r(new RealRig(SynthModel::PERC, kSynthEnginePerc));
        SynthSlotRecord rc = rec(SynthModel::PERC, 1);   // slot sends 0: only the drum sends
        rc.delaySend = 0.0f;
        rc.reverbSend = 0.0f;
        r->bank.setSlotRecord(0, rc);
        synthParamSet(r->patch, snareD, sd);
        synthParamSet(r->patch, snareR, sr);
        synthParamSet(r->patch, kickD, 0.0f);
        synthParamSet(r->patch, kickR, 0.0f);
        r->bank.patchChanged(0);
        r->block();
        return r;
    };
    double l, d, v;
    {   // snare: delay 1, reverb 0.5 -> a quarter of the delay energy on reverb
        auto r = rig(1.0f, 0.5f);
        r->on(62);
        r->run(20, l, d, v);
        CHECK(l > 0.1);
        CHECK(d > 0.01);
        CHECK_NEAR(v / d, 0.25, 0.01);
        // Mono sum (L + R) / 2 at the drum's send amount: the main bus carries
        // the panned drum, the delay send the centred one.
        CHECK(d < 2.0 * l);
    }
    {   // the kick sends nothing (its sends are 0)
        auto r = rig(1.0f, 1.0f);
        r->on(60);
        r->run(20, l, d, v);
        CHECK(l > 0.1);
        CHECK_EQ(d, 0.0);
        CHECK_EQ(v, 0.0);
    }
    {   // Global Mixer delay muted, slot gain off: nothing on the send buses
        auto r = rig(1.0f, 1.0f);
        r->in.delayLevel = 0.0f;
        r->on(62);
        r->run(20, l, d, v);
        CHECK_EQ(d, 0.0);
        CHECK(v > 0.01);
        SynthSlotRecord rc = rec(SynthModel::PERC, 1);
        rc.delaySend = rc.reverbSend = 0.0f;
        rc.gainDb = -96.0f;
        r->bank.setSlotRecord(0, rc);
        r->on(62);
        r->run(20, l, d, v);
        CHECK_EQ(v, 0.0);
    }
    {   // slot gain scales the drum sends like the slot output (-6 dB)
        auto a = rig(1.0f, 0.0f);
        a->on(62);
        double l0, d0, v0;
        a->run(20, l0, d0, v0);
        auto b6 = rig(1.0f, 0.0f);
        SynthSlotRecord rc = rec(SynthModel::PERC, 1);
        rc.delaySend = rc.reverbSend = 0.0f;
        rc.gainDb = -6.0f;
        b6->bank.setSlotRecord(0, rc);
        b6->block();
        b6->on(62);
        b6->run(20, l, d, v);
        CHECK_NEAR(d / d0, l / l0, 1e-3);
    }
}

// --- sequencer dispatch -------------------------------------------------------------------

namespace {
struct SeqSink : ISequencerSink {
    struct E { char k; int track, a, b, c; };
    std::vector<E> ev;
    void voiceNoteOn(uint8_t t, const VoiceNoteOn& n) override { ev.push_back({'n', t, n.note, n.instrument, 0}); }
    void voiceNoteOff(uint8_t t, NoteOffMode m) override { ev.push_back({'o', t, int(m), 0, 0}); }
    void voiceApplyFx(uint8_t t, uint8_t, uint8_t type, uint8_t v) override { ev.push_back({'f', t, type, v, 0}); }
    void resetAllVoiceFx() override {}
    void engineBlockUpdate(float) override {}
    void metronomeClick(bool, uint8_t) override {}
    void midiSend(uint8_t, const uint8_t*, uint8_t) override { ev.push_back({'m', -1, 0, 0, 0}); }
    void synthNoteOn(uint8_t t, const SynthNoteOn& n) override { ev.push_back({'S', t, n.slot, n.note, n.volume}); }
    void synthNoteOff(uint8_t t, NoteOffMode m) override { ev.push_back({'O', t, int(m), 0, 0}); }
    void synthApplyFx(uint8_t t, uint8_t type, uint8_t v) override { ev.push_back({'F', t, type, v, 0}); }
    void synthMacroFx(uint8_t s, uint8_t m, uint8_t v) override { ev.push_back({'M', -1, s, m, v}); }
    void synthTransportStop() override { ev.push_back({'X', -1, 0, 0, 0}); }
    int count(char k) const {
        int n = 0;
        for (const E& e : ev) n += e.k == k;
        return n;
    }
    const E* first(char k) const {
        for (const E& e : ev)
            if (e.k == k) return &e;
        return nullptr;
    }
};
}  // namespace

TEST(synth_sequencer_dispatch_64_66) {
    Project project;
    Pattern a, bb;
    a.setDefault();
    bb.setDefault();
    project.setTempo(120.0f);
    SeqSink sink;
    ISequencerHost host;
    Sequencer seq;
    seq.attach(&sink, &host, &project, &a, &bb);
    Pattern& p = *seq.currentPattern();
    for (auto& t : p.tracks) t.lastStep = 7;
    // Track 13 (index 12): Synth 2 note with Volume 50 and FX CC b = 90.
    p.tracks[12].steps[0].note = 60;
    p.tracks[12].steps[0].instrument = 65;
    p.tracks[12].steps[0].fx[0] = StepFx{18, 50};
    p.tracks[12].steps[0].fx[1] = StepFx{9, 90};
    // Track 1: Synth 3 note, then a synth note on the same track (legato,
    // no note-off between), then an FX-only step, then OFF.
    p.tracks[0].steps[0].note = 48;
    p.tracks[0].steps[0].instrument = 66;
    p.tracks[0].steps[1].note = 50;
    p.tracks[0].steps[1].instrument = 66;
    p.tracks[0].steps[2].fx[0] = StepFx{41, 16};   // slide on the sounding synth
    p.tracks[0].steps[2].fx[1] = StepFx{10, 33};   // FX CC c with no note: the track's synth
    p.tracks[0].steps[3].note = kNoteOff;
    // Instrument 64 on track 16: Synth 1.
    p.tracks[15].steps[1].note = 72;
    p.tracks[15].steps[1].instrument = 64;
    seq.play();
    const int blocksPerStep = 24;   // 120 bpm: a step is ~43 blocks; 4 steps in 200
    for (int i = 0; i < 200; ++i) seq.processBlock(128);
    (void)blocksPerStep;
    const SeqSink::E* e = sink.first('S');
    CHECK(e != nullptr);
    int t12 = 0, t0 = 0, t15 = 0;
    for (const auto& x : sink.ev) {
        if (x.k != 'S') continue;
        if (x.track == 12) {
            ++t12;
            CHECK_EQ(x.a, 1);
            CHECK_EQ(x.b, 60);
            CHECK_EQ(x.c, 50);
        }
        if (x.track == 0) {
            ++t0;
            CHECK_EQ(x.a, 2);
        }
        if (x.track == 15) {
            ++t15;
            CHECK_EQ(x.a, 0);
            CHECK_EQ(x.b, 72);
        }
    }
    CHECK_EQ(t12, 1);
    CHECK_EQ(t0, 2);
    CHECK_EQ(t15, 1);
    // No note-off between the two synth notes of track 0; the OFF step sends one.
    int idx2 = -1, offIdx = -1;
    int seen = 0;
    for (size_t i = 0; i < sink.ev.size(); ++i) {
        const auto& x = sink.ev[i];
        if (x.k == 'S' && x.track == 0 && ++seen == 2) idx2 = int(i);
        if (x.k == 'O' && x.track == 0 && offIdx < 0) offIdx = int(i);
    }
    CHECK(idx2 > 0);
    CHECK(offIdx > idx2);
    // Macros: b (=1) for slot 1 = 90; c (=2) for slot 2 from the FX-only step.
    bool mb = false, mc = false, fx = false;
    for (const auto& x : sink.ev) {
        if (x.k == 'M' && x.a == 1 && x.b == 1 && x.c == 90) mb = true;
        if (x.k == 'M' && x.a == 2 && x.b == 2 && x.c == 33) mc = true;
        if (x.k == 'F' && x.track == 0 && x.a == 41 && x.b == 16) fx = true;
    }
    CHECK(mb);
    CHECK(mc);
    CHECK(fx);
    CHECK_EQ(sink.count('n'), 0);   // no sample voice note-ons for synth steps
    CHECK_EQ(sink.count('m'), 0);   // no MIDI out for synths (only clock off)
    seq.stop();
    CHECK_EQ(sink.count('X'), 1);
}

TEST(synth_sequencer_mute_releases) {
    Project project;
    Pattern a, bb;
    a.setDefault();
    bb.setDefault();
    SeqSink sink;
    ISequencerHost host;
    Sequencer seq;
    seq.attach(&sink, &host, &project, &a, &bb);
    Pattern& p = *seq.currentPattern();
    p.tracks[9].steps[0].note = 60;
    p.tracks[9].steps[0].instrument = 64;
    seq.play();
    for (int i = 0; i < 5; ++i) seq.processBlock(128);
    CHECK_EQ(sink.count('S'), 1);
    project.setTrackMuted(9, true);
    for (int i = 0; i < 50; ++i) seq.processBlock(128);
    CHECK(sink.count('O') >= 1);
}

// A steal victim that started in the same block (no frame rendered yet) ends
// at once: no fade record, no render of it (docs/OPTIMIZATION_2X.md "Peak
// attribution"); one that has sounded still fades for a block.
TEST(synth_same_block_steal_frees_at_once) {
    BankRig r(2, 2, 2, 4);
    r.on(0, 0, 60);
    r.on(1, 0, 62);
    const int renders0 = r.spy.renders;
    r.on(2, 0, 64);   // steals the note of track 0, started in this block
    CHECK_EQ(r.bank.steals(), 1u);
    CHECK_EQ(r.bank.voicesOf(0), 2);
    CHECK_EQ(r.bank.busyRecords(), 2);
    CHECK(r.bank.voiceOfTrack(0) == nullptr);
    r.block();
    CHECK_EQ(r.spy.renders - renders0, 2);
    // After a rendered block the victim fades as before.
    r.on(3, 0, 65);
    CHECK_EQ(r.bank.busyRecords(), 3);
}

// Note-on spreading: past the per-block start cap a note waits exactly one
// block, keeps its order, and takes its note-off / FX of that block along;
// polyphony-1 reuse never waits.
TEST(synth_note_on_spreading) {
    BankRig r(8, 1, 2, 8, 3);
    CHECK_EQ(SynthBank::kStartsPerBlock, 3);
    r.block();
    for (int t = 0; t < 5; ++t) r.on(t, 0, 50 + t);
    CHECK_EQ(r.bank.voicesOf(0), 3);
    CHECK_EQ(r.bank.queuedNotes(), 2);
    CHECK(r.bank.voiceOfTrack(3) == nullptr);
    r.bank.noteOff(4, NoteOffMode::Off);   // follows the queued note of track 4
    r.block();                             // block 1: the three render
    CHECK_EQ(r.bank.voicesOf(0), 3);
    // Block 2 starts: the queued notes go first, before this block's notes.
    r.on(5, 0, 70);
    CHECK_EQ(r.bank.queuedNotes(), 0);
    CHECK(r.bank.voiceOfTrack(3) != nullptr);
    CHECK_EQ(int(r.bank.voiceOfTrack(3)->note), 53);
    CHECK(r.bank.voiceOfTrack(4) == nullptr);   // started, then released by its note-off
    CHECK_EQ(r.bank.voicesOf(0), 6);
    // The cap counts the flushed starts: a fourth start in block 2 waits.
    r.on(6, 0, 71);
    CHECK_EQ(r.bank.queuedNotes(), 1);
    // A mono reuse never waits.
    r.on(7, 1, 40);
    r.block();
    r.on(8, 0, 72);
    r.on(9, 0, 73);
    r.on(10, 0, 74);
    r.on(7, 1, 41);   // slot 1 (polyphony 1) has a voice: reused now
    CHECK_EQ(int(r.bank.voiceOfTrack(7)->note), 41);
    // Without a note-on the next render flushes the queue itself.
    CHECK(r.bank.queuedNotes() > 0);
    r.block();
    r.block();
    CHECK_EQ(r.bank.queuedNotes(), 0);
    CHECK(r.bank.voiceOfTrack(6) != nullptr);
}
