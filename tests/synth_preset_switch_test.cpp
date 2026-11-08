// Z: a synth slot plays one of the project's presets instead of its own patch,
// through the same rebind as a live edit: the same engine ramps over one block
// and keeps its voices, another engine fades them and restarts. A repeated Z does
// nothing, an empty preset is ignored, and Z0, Stop or an edit of the patch go back.
#include "test.h"

#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "model/fx.h"
#include "model/project.h"

using namespace ot;
using namespace ot::audio;

namespace {

struct Rig {
    SynthPatch own;
    SynthPatch presets[kSynthPresetCount] = {};
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    float lo, hi;
    Rig() {
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        const PatchParamRange* r = synthParamRange(kSynthEngineAcd, 0);
        lo = r->min;
        hi = r->max;
        patchInit(kSynthEngineAcd, &own);
        CHECK(synthParamSet(own, 0, lo));
        patchInit(kSynthEngineAcd, &presets[0]);
        CHECK(synthParamSet(presets[0], 0, hi));
        bank.bindPatch(0, &own);
        bank.bindPresets(presets);
        SynthSlotRecord rec = SynthSlotRecord::defaults(0);
        rec.model = uint32_t(SynthModel::ACD);
        rec.polyphony = 4;
        bank.setSlotRecord(0, rec);
        block();
        block();
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
    }
    void note(int track = 0, uint8_t key = 60) {
        SynthNoteOn n;
        n.slot = 0;
        n.note = key;
        n.volume = -1;
        bank.noteOn(track, n);
    }
    float end0() const { return bank.params(0).end[0]; }
    float start0() const { return bank.params(0).start[0]; }
};

}  // namespace

TEST(preset_select_binds_the_preset_before_the_note) {
    Rig r;
    CHECK_NEAR(r.end0(), r.lo, 1e-4f);
    r.bank.presetSelect(0, 1);
    CHECK_EQ(int(r.bank.presetOf(0)), 1);
    r.note();                              // Z and note on one step: no block in between
    CHECK_NEAR(r.end0(), r.hi, 1e-4f);
}

TEST(same_engine_switch_ramps_over_one_block) {
    Rig r;
    r.note();
    r.block();
    r.block();
    CHECK_EQ(r.bank.voicesOf(0), 1);
    r.bank.presetSelect(0, 1);
    r.block();
    CHECK_NEAR(r.start0(), r.lo, 1e-4f);   // from the old value
    CHECK_NEAR(r.end0(), r.hi, 1e-4f);     // to the new one
    CHECK_EQ(r.bank.voicesOf(0), 1);       // the note keeps ringing
    r.block();
    CHECK_NEAR(r.start0(), r.hi, 1e-4f);
}

TEST(cross_engine_switch_fades_voices_and_restarts) {
    Rig r;
    patchInit(kSynthEngineAcd, &r.presets[1]);
    r.presets[1].engine = kSynthEngineTest;
    r.note();
    r.block();
    CHECK_EQ(r.bank.voicesOf(0), 1);
    r.bank.presetSelect(0, 2);
    r.block();
    CHECK_EQ(r.bank.voicesOf(0), 0);
    CHECK_EQ(int(r.bank.params(0).engine), int(kSynthEngineTest));
    r.bank.presetSelect(0, 0);
    r.block();
    CHECK_EQ(int(r.bank.params(0).engine), int(kSynthEngineAcd));
    CHECK_NEAR(r.end0(), r.lo, 1e-4f);
}

TEST(empty_preset_is_ignored) {
    Rig r;
    r.bank.presetSelect(0, 1);
    r.block();
    r.bank.presetSelect(0, 2);   // empty: engine 0
    r.block();
    CHECK_EQ(int(r.bank.presetOf(0)), 1);
    CHECK_NEAR(r.end0(), r.hi, 1e-4f);
    r.bank.presetSelect(0, 9);   // out of range
    r.bank.presetSelect(5, 1);
    CHECK_EQ(int(r.bank.presetOf(0)), 1);
}

TEST(repeated_same_preset_does_not_reload) {
    Rig r;
    r.bank.presetSelect(0, 1);
    r.block();
    r.block();
    CHECK(synthParamSet(r.presets[0], 0, r.lo));   // only a reload would see this
    r.bank.presetSelect(0, 1);
    r.block();
    CHECK_NEAR(r.end0(), r.hi, 1e-4f);
}

TEST(z0_transport_stop_and_patch_changed_restore_own) {
    {
        Rig r;
        r.bank.presetSelect(0, 1);
        r.block();
        r.bank.presetSelect(0, 0);
        r.block();
        CHECK_EQ(int(r.bank.presetOf(0)), 0);
        CHECK_NEAR(r.end0(), r.lo, 1e-4f);
    }
    {
        Rig r;
        r.bank.presetSelect(0, 1);
        r.block();
        r.bank.transportStop();
        r.block();
        CHECK_EQ(int(r.bank.presetOf(0)), 0);
        CHECK_NEAR(r.end0(), r.lo, 1e-4f);
    }
    {
        Rig r;
        r.bank.presetSelect(0, 1);
        r.block();
        r.bank.patchChanged(0);
        r.block();
        CHECK_EQ(int(r.bank.presetOf(0)), 0);
        CHECK_NEAR(r.end0(), r.lo, 1e-4f);
    }
}

TEST(macro_override_applies_to_the_preset_targets) {
    Rig r;
    // Only the preset's macro 1 moves parameter 0.
    CHECK(synthParamSet(r.presets[0], 0, r.lo));
    PatchMacro& m = r.presets[0].macros[0];
    m.count = 1;
    m.targets[0].param = 0;
    m.targets[0].amountBits = 0x3F800000u;   // 1.0f
    r.bank.macroFx(0, 0, 127);
    r.block();
    r.block();
    CHECK_NEAR(r.end0(), r.lo, 1e-4f);     // the own patch has no such target
    r.bank.presetSelect(0, 1);
    r.block();
    CHECK(r.end0() > r.lo + 0.5f * (r.hi - r.lo));
}

TEST(missing_engine_preset_is_silent_until_z0) {
    Rig r;
    patchInit(kSynthEngineAcd, &r.presets[2]);
    r.presets[2].engine = 0x7F;   // not in this build
    r.bank.presetSelect(0, 3);
    r.block();
    CHECK_EQ(int(r.bank.presetOf(0)), 3);
    r.note();
    r.block();
    CHECK_EQ(r.bank.voicesOf(0), 0);
    r.bank.presetSelect(0, 0);
    r.block();
    r.note(1);
    r.block();
    CHECK_EQ(r.bank.voicesOf(0), 1);
}

TEST(reset_clears_presets) {
    Rig r;
    r.bank.presetSelect(0, 1);
    r.block();
    r.bank.reset();
    CHECK_EQ(int(r.bank.presetOf(0)), 0);
}

TEST(no_preset_table_ignores_the_command) {
    SynthBank b;
    b.presetSelect(0, 1);
    CHECK_EQ(int(b.presetOf(0)), 0);
}

TEST(an_emptied_entry_plays_the_own_patch) {
    // Clear racing a Z: the Z was accepted, the entry went empty before the bind.
    Rig r;
    r.bank.presetSelect(0, 1);
    r.presets[0].engine = 0;
    r.block();
    CHECK_EQ(int(r.bank.params(0).engine), int(kSynthEngineAcd));
    CHECK_NEAR(r.end0(), r.lo, 1e-4f);
    r.note();
    r.block();
    CHECK_EQ(r.bank.voicesOf(0), 1);   // not silent
}
