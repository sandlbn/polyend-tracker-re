// Synth slot inserts: one optional effect per synth slot on the slot bus,
// after the engine's own slot effect and before Width, gain, pan and the
// slot sends. A note's own Delay / Reverb Send FX take the voice dry.
#include "test.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "audio/audio_tables.h"
#include "audio/inserts.h"
#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "model/fx.h"
#include "model/project.h"
#if __has_include("ot_profile.h")
#include "ot_profile.h"
#endif

using namespace ot;
using namespace ot::audio;

#if defined(OT_SLOT_INSERTS) && OT_SLOT_INSERTS && defined(OT_MOD_ACD1) && OT_MOD_ACD1

namespace {

// A gain (x2 when on) with a one-block ramp to 1 when off; counts its blocks in
// the first word of its work memory (it must persist between blocks).
class Gain2 final : public IEffect {
public:
    explicit Gain2(const char* id = "TGN2", uint32_t work = 16) : id_(id), work_(work) {}
    const char* id() const override { return id_; }
    const char* name() const override { return "Gain"; }
    uint32_t stateBytes() const override { return sizeof(float); }
    uint32_t workBytes() const override { return work_; }
    int paramCount() const override { return 1; }
    const EffectParam& param(int) const override {
        static const EffectParam p = {"Gain", 100, 100, nullptr};
        return p;
    }
    bool process(void* state, void* work, const EffectSettings& s, bool on, float* l, float* r, uint32_t n) override {
        float& g = *static_cast<float*>(state);
        const float g0 = g == 0.0f ? 1.0f : g;
        const float g1 = on ? 1.0f + float(s.v[0]) * 0.01f : 1.0f;
        if (!on && g0 == 1.0f) return false;
        for (uint32_t i = 0; i < n; ++i) {
            const float k = g0 + (g1 - g0) * float(i + 1) / float(n);
            l[i] *= k;
            r[i] *= k;
        }
        if (work) ++static_cast<uint16_t*>(work)[0];
        g = g1;
        ++blocks;
        return on || g1 != 1.0f;
    }
    int blocks = 0;

private:
    const char* id_;
    uint32_t work_;
};

// A tail: adds a decaying DC of the block's peak (halved per block), so a slot
// rings on after its voices end; silent after its tail.
class Tail final : public IEffect {
public:
    const char* id() const override { return "TTAI"; }
    const char* name() const override { return "Tail"; }
    uint32_t stateBytes() const override { return sizeof(float); }
    int paramCount() const override { return 1; }
    const EffectParam& param(int) const override {
        static const EffectParam p = {"Amount", 100, 50, nullptr};
        return p;
    }
    uint32_t tailFrames() const override { return 24 * kBlock; }
    bool process(void* state, void*, const EffectSettings&, bool on, float* l, float* r, uint32_t n) override {
        float& lv = *static_cast<float*>(state);
        float pk = 0.0f;
        for (uint32_t i = 0; i < n; ++i) pk = std::fmax(pk, std::fabs(l[i]));
        lv = std::fmax(pk, lv * 0.5f);
        if (lv < 1e-30f) lv = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            l[i] += lv;
            r[i] += lv;
        }
        return on || lv > 0.0f;
    }
};

struct Rig {
    SynthPatch patch;
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    int16_t lines[kSynthSlots * kEffectWorkMax / 2] = {};
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    std::vector<float> outL, outR, outD;
    explicit Rig(bool memory = true) {
        initTables();   // the send law (sendGain)
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        if (memory) bank.setInsertMemory(lines);
        patchInit(synthEngineId(SynthModel::ACD), &patch);
        bank.bindPatch(0, &patch);
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(SynthModel::ACD);
        r.polyphony = 4;
        r.delaySend = 50.0f;
        bank.setSlotRecord(0, r);
        block();
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
        outL.insert(outL.end(), L, L + kBlock);
        outR.insert(outR.end(), R, R + kBlock);
        outD.insert(outD.end(), D, D + kBlock);
    }
    void on(int track, int note, int8_t sendD = -1) {
        SynthNoteOn n;
        n.slot = 0;
        n.note = uint8_t(note);
        n.volume = -1;
        if (sendD >= 0) {
            n.fxA.type = uint8_t(FxId::DelaySend);
            n.fxA.value = uint8_t(sendD);
        }
        bank.noteOn(track, n);
    }
    void off(int track) { bank.noteOff(track, NoteOffMode::Cut); }
    void blocks(int n) {
        for (int b = 0; b < n; ++b) block();
    }
};

double energy(const std::vector<float>& x, size_t from, size_t to) {
    double e = 0.0;
    for (size_t i = from; i < to && i < x.size(); ++i) e += double(x[i]) * x[i];
    return e;
}

}  // namespace

TEST(slot_insert_none_is_bit_identical) {
    Rig a, b;
    b.bank.setSlotInsert(0, nullptr, EffectSettings{});
    a.on(0, 48);
    b.on(0, 48);
    a.blocks(30);
    b.blocks(30);
    CHECK(a.outL == b.outL && a.outR == b.outR && a.outD == b.outD);
    CHECK(b.bank.slotInsert(0) == nullptr);
}

TEST(slot_insert_runs_before_the_slot_mix_and_sends) {
    // Gain x2 on the slot bus: the slot's output and its slot send double
    // (after the first block's ramp); a slot without an insert is unchanged.
    Gain2 fx;
    Rig dry, wet;
    wet.bank.setSlotInsert(0, &fx, effectDefaults(fx));
    dry.on(0, 48);
    wet.on(0, 48);
    dry.blocks(20);
    wet.blocks(20);
    CHECK(wet.bank.slotInsert(0) == &fx);
    const size_t from = 4 * kBlock, to = 20 * kBlock;
    CHECK_NEAR(energy(wet.outL, from, to) / energy(dry.outL, from, to), 4.0, 1e-3);
    CHECK_NEAR(energy(wet.outD, from, to) / energy(dry.outD, from, to), 4.0, 1e-3);
    // The work memory is the caller's SDRAM line: it persisted across blocks.
    CHECK(wet.lines[0] > 10);
    CHECK_EQ(int(wet.lines[0]), fx.blocks);
}

TEST(slot_insert_note_sends_stay_dry) {
    // A note with its own Delay Send FX and the slot send at 0: the delay bus
    // gets the dry voice, not the doubled one.
    Gain2 fx;
    Rig dry, wet;
    for (Rig* r : {&dry, &wet}) {
        SynthSlotRecord rec = r->bank.slotRecord(0);
        rec.delaySend = 0.0f;
        r->bank.setSlotRecord(0, rec);
    }
    wet.bank.setSlotInsert(0, &fx, effectDefaults(fx));
    dry.on(0, 48, 80);
    wet.on(0, 48, 80);
    dry.blocks(20);
    wet.blocks(20);
    const size_t from = 4 * kBlock, to = 20 * kBlock;
    CHECK(energy(dry.outD, from, to) > 0.0);
    CHECK(energy(wet.outD, from, to) == energy(dry.outD, from, to));
    CHECK_NEAR(energy(wet.outL, from, to) / energy(dry.outL, from, to), 4.0, 1e-3);
}

TEST(slot_insert_tail_keeps_the_slot_rendering) {
    Tail fx;
    Rig r;
    r.bank.setSlotInsert(0, &fx, effectDefaults(fx));
    r.on(0, 48);
    r.blocks(10);
    r.off(0);
    r.blocks(4);   // the voice is gone
    CHECK(r.bank.voicesOf(0) == 0);
    CHECK(r.bank.active());   // the insert still rings
    const size_t at = r.outL.size();
    r.blocks(2);
    CHECK(energy(r.outL, at, r.outL.size()) > 0.0);
    r.blocks(40);
    CHECK(!r.bank.active());
    const size_t quiet = r.outL.size();
    r.blocks(2);
    CHECK(energy(r.outL, quiet, r.outL.size()) == 0.0);
}

TEST(slot_insert_stop_all_silences_the_tail) {
    Tail fx;
    Rig r;
    r.bank.setSlotInsert(0, &fx, effectDefaults(fx));
    r.on(0, 48);
    r.blocks(10);
    r.bank.stopAll();
    CHECK(!r.bank.active());
    const size_t at = r.outL.size();
    r.blocks(2);
    CHECK(energy(r.outL, at, r.outL.size()) == 0.0);
}

TEST(slot_insert_kind_change_fades_then_switches) {
    // A -> B while sounding: A fades out (no step above the ramp), then B runs
    // within 4 blocks; settings of the same kind apply at the next block.
    Gain2 a("TGNA"), b("TGNB");
    Rig r;
    r.bank.setSlotInsert(0, &a, effectDefaults(a));
    r.on(0, 48);
    r.blocks(10);
    r.bank.setSlotInsert(0, &b, effectDefaults(b));
    const int before = b.blocks;
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == &b);
    CHECK(b.blocks > before);
    float worst = 0.0f;
    const size_t at = 10 * kBlock;
    for (size_t i = at; i < at + 6 * kBlock; ++i) worst = std::fmax(worst, std::fabs(r.outL[i] - r.outL[i - 1]));
    float steady = 0.0f;
    for (size_t i = 4 * kBlock; i < at; ++i) steady = std::fmax(steady, std::fabs(r.outL[i] - r.outL[i - 1]));
    CHECK(worst < 1.5f * steady);
    // Back to none: the last effect fades out, then nothing is bound.
    r.bank.setSlotInsert(0, nullptr, EffectSettings{});
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == nullptr);
}

// Off (the first setting's "Off"): the effect fades out as on a kind change,
// then is not run at all (no CPU); On again resumes with the same settings.
TEST(slot_insert_off_fades_out_then_stops_running) {
    Gain2 fx;
    Rig dry, wet;
    const EffectSettings set = effectDefaults(fx);
    wet.bank.setSlotInsert(0, &fx, set);
    dry.on(0, 48);
    wet.on(0, 48);
    dry.blocks(10);
    wet.blocks(10);
    wet.bank.setSlotInsert(0, &fx, set, true);
    dry.blocks(10);
    wet.blocks(10);
    const int ran = fx.blocks;
    dry.blocks(20);
    wet.blocks(20);
    CHECK_EQ(fx.blocks, ran);   // parked: not run
    CHECK(wet.bank.slotInsert(0) == &fx);
    const size_t from = 22 * kBlock, to = 40 * kBlock;
    CHECK_NEAR(energy(wet.outL, from, to) / energy(dry.outL, from, to), 1.0, 1e-6);
    // On again: x2 again (after the ramp).
    wet.bank.setSlotInsert(0, &fx, set, false);
    dry.blocks(20);
    wet.blocks(20);
    CHECK(fx.blocks > ran);
    CHECK_NEAR(energy(wet.outL, 44 * kBlock, 60 * kBlock) / energy(dry.outL, 44 * kBlock, 60 * kBlock), 4.0, 1e-3);
}

TEST(slot_insert_needs_its_memory) {
    // An effect with work memory and no SDRAM lines given: not bound, the slot
    // plays dry. A memoryless effect still binds.
    Gain2 withWork("TGNW", 16), noWork("TGN0", 0);
    Rig r(false);
    r.bank.setSlotInsert(0, &withWork, effectDefaults(withWork));
    r.blocks(2);
    CHECK(r.bank.slotInsert(0) == nullptr);
    r.bank.setSlotInsert(0, &noWork, effectDefaults(noWork));
    r.blocks(2);
    CHECK(r.bank.slotInsert(0) == &noWork);
}

#else
TEST(slot_inserts_not_in_this_build) { CHECK(true); }
#endif
