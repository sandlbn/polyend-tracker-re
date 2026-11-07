// [3.x] A synth preset carries the slot insert: while a slot plays preset n
// (Z), it runs preset n's insert -- the one the slot had when the preset was
// stored, or none if it had none. A preset saved before presets had inserts
// keeps the slot's own insert. Z0, Stop or an edit of the slot's patch bring
// the slot's own insert back, with its patch.
#include "test.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "audio/audio_tables.h"
#include "audio/inserts.h"
#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "model/project.h"
#include "modules/synth_models.h"
#if __has_include("ot_profile.h")
#include "ot_profile.h"
#endif

using namespace ot;
using namespace ot::audio;

#if defined(OT_SLOT_INSERTS) && OT_SLOT_INSERTS

namespace {

#if defined(OT_MOD_ACD1) && OT_MOD_ACD1
// A gain (x(1 + v/100) when on), counting the blocks it ran.
class Gain final : public IEffect {
public:
    explicit Gain(const char* id) : id_(id) {}
    const char* id() const override { return id_; }
    const char* name() const override { return "Gain"; }
    uint32_t stateBytes() const override { return sizeof(float); }
    int paramCount() const override { return 1; }
    const EffectParam& param(int) const override {
        static const EffectParam p = {"Gain", 100, 100, nullptr};
        return p;
    }
    bool process(void* state, void*, const EffectSettings& s, bool on, float* l, float* r, uint32_t n) override {
        float& g = *static_cast<float*>(state);
        const float g0 = g == 0.0f ? 1.0f : g;
        const float g1 = on ? 1.0f + float(s.v[0]) * 0.01f : 1.0f;
        if (!on && g0 == 1.0f) return false;
        for (uint32_t i = 0; i < n; ++i) {
            const float k = g0 + (g1 - g0) * float(i + 1) / float(n);
            l[i] *= k;
            r[i] *= k;
        }
        g = g1;
        ++blocks;
        return on || g1 != 1.0f;
    }
    int blocks = 0;

private:
    const char* id_;
};

EffectSettings gainSet(uint8_t v) {
    EffectSettings s{};
    s.v[0] = v;
    return s;
}

struct Rig {
    SynthPatch own;
    SynthPatch presets[kSynthPresetCount] = {};
    PresetInsert ins[kSynthPresetCount] = {};
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    int16_t lines[kSynthSlots * kEffectWorkMax / 2] = {};
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    std::vector<float> outL;
    Rig() {
        initTables();
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        bank.setInsertMemory(lines);
        patchInit(synthEngineId(SynthModel::ACD), &own);
        for (SynthPatch& p : presets) p = own;
        presets[kSynthPresetCount - 1].engine = 0;   // preset 8 empty
        bank.bindPatch(0, &own);
        bank.bindPresets(presets);
        bank.bindPresetInserts(ins);
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(SynthModel::ACD);
        r.polyphony = 4;
        bank.setSlotRecord(0, r);
        block();
    }
    void presetIns(int n, IEffect* fx, const EffectSettings& set, const char* id = nullptr, bool off = false) {
        PresetInsert& p = ins[n - 1];
        p = PresetInsert{};
        p.fx = fx;
        p.set = set;
        const char* i = id ? id : (fx ? fx->id() : nullptr);
        if (i) std::memcpy(p.id, i, 4);
        p.flags = uint8_t(kPresetInsertHas | (off ? kPresetInsertOff : 0));
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
        outL.insert(outL.end(), L, L + kBlock);
    }
    void blocks(int n) {
        for (int b = 0; b < n; ++b) block();
    }
    void on(int note = 48) {
        SynthNoteOn n;
        n.slot = 0;
        n.note = uint8_t(note);
        n.volume = -1;
        bank.noteOn(0, n);
    }
};

double energy(const std::vector<float>& x, size_t from, size_t to) {
    double e = 0.0;
    for (size_t i = from; i < to && i < x.size(); ++i) e += double(x[i]) * x[i];
    return e;
}
#endif  // OT_MOD_ACD1

}  // namespace

#if defined(OT_MOD_ACD1) && OT_MOD_ACD1

TEST(z_switches_patch_and_insert_together_and_z0_restores_own) {
    Gain a("TGNA"), b("TGNB");
    Rig r;
    r.bank.setSlotInsert(0, &a, gainSet(50));
    r.presetIns(1, &b, gainSet(100));
    r.on();
    r.blocks(4);
    CHECK(r.bank.slotInsert(0) == &a);
    r.bank.presetSelect(0, 1);
    r.blocks(6);   // the own insert fades out, then the preset's runs
    CHECK(r.bank.slotInsert(0) == &b);
    r.bank.presetSelect(0, 0);
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == &a);
}

TEST(preset_without_insert_data_keeps_the_own_insert) {
    Gain a("TGNA");
    Rig r;
    r.bank.setSlotInsert(0, &a, gainSet(50));
    r.on();
    r.blocks(4);
    r.bank.presetSelect(0, 2);   // no insert data: saved before presets had inserts
    r.blocks(6);
    CHECK_EQ(int(r.bank.presetOf(0)), 2);
    CHECK(r.bank.slotInsert(0) == &a);
    const int ran = a.blocks;
    r.blocks(4);
    CHECK_EQ(a.blocks, ran + 4);   // never faded or parked
}

TEST(preset_with_no_insert_removes_it_while_selected) {
    Gain a("TGNA");
    Rig r;
    r.bank.setSlotInsert(0, &a, gainSet(50));
    r.presetIns(3, nullptr, EffectSettings{});   // stored with no insert
    r.on();
    r.blocks(4);
    r.bank.presetSelect(0, 3);
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == nullptr);
    r.bank.presetSelect(0, 0);
    r.blocks(2);
    CHECK(r.bank.slotInsert(0) == &a);
}

TEST(own_insert_edit_while_on_a_preset_waits_for_z0) {
    Gain a("TGNA"), b("TGNB"), c("TGNC");
    Rig r;
    r.bank.setSlotInsert(0, &a, gainSet(50));
    r.presetIns(1, &b, gainSet(100));
    r.bank.presetSelect(0, 1);
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == &b);
    r.bank.setSlotInsert(0, &c, gainSet(20));   // the slot's own insert edited
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == &b);
    r.bank.presetSelect(0, 0);
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == &c);
}

TEST(transport_stop_patch_changed_and_reset_restore_own_insert) {
    Gain a("TGNA"), b("TGNB");
    for (int k = 0; k < 3; ++k) {
        Rig r;
        r.bank.setSlotInsert(0, &a, gainSet(50));
        r.presetIns(1, &b, gainSet(100));
        r.bank.presetSelect(0, 1);
        r.blocks(6);
        CHECK(r.bank.slotInsert(0) == &b);
        if (k == 0) r.bank.transportStop();
        if (k == 1) r.bank.patchChanged(0);
        if (k == 2) r.bank.reset();
        r.blocks(6);
        CHECK_EQ(int(r.bank.presetOf(0)), 0);
        CHECK(r.bank.slotInsert(0) == &a);
    }
}

TEST(same_kind_switch_only_updates_settings_other_kind_fades_first) {
    Gain a("TGNA"), b("TGNB");
    Rig dry, wet;
    wet.bank.setSlotInsert(0, &a, gainSet(0));
    wet.presetIns(1, &a, gainSet(100));   // the same kind, other settings
    dry.on();
    wet.on();
    dry.blocks(6);
    wet.blocks(6);
    const int ran = a.blocks;
    wet.bank.presetSelect(0, 1);
    dry.bank.presetSelect(0, 1);   // (no insert table entries matter here: none bound)
    dry.blocks(1);
    wet.blocks(1);
    CHECK(wet.bank.slotInsert(0) == &a);
    CHECK_EQ(a.blocks, ran + 1);   // ran on through the switch
    dry.blocks(10);
    wet.blocks(10);
    const size_t from = 9 * kBlock, to = 17 * kBlock;
    CHECK_NEAR(energy(wet.outL, from, to) / energy(dry.outL, from, to), 4.0, 1e-2);
    // Another kind: the bound one fades out first (a block here), then the next runs.
    wet.presetIns(2, &b, gainSet(100));
    wet.bank.presetSelect(0, 2);
    const int aRan = a.blocks;
    wet.blocks(1);
    CHECK_EQ(a.blocks, aRan + 1);
    CHECK_EQ(b.blocks, 0);
    wet.blocks(6);
    CHECK(wet.bank.slotInsert(0) == &b);
    CHECK(b.blocks > 0);
}

TEST(unresolved_effect_pointer_leaves_the_slot_dry) {
    Gain a("TGNA");
    Rig r;
    r.bank.setSlotInsert(0, &a, gainSet(50));
    r.presetIns(1, nullptr, gainSet(10), "QQQQ");   // an effect this build lacks
    r.bank.presetSelect(0, 1);
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == nullptr);
    r.bank.presetSelect(0, 0);
    r.blocks(6);
    CHECK(r.bank.slotInsert(0) == &a);
}

#endif  // OT_MOD_ACD1

#else
TEST(preset_inserts_not_in_this_build) { CHECK(true); }
#endif
