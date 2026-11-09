// Edit what you hear: while a synth slot plays a preset, an edit of that
// preset's entry is heard on the slot at the next block and the slot stays on
// the preset (SynthBank::presetChanged): a value ramps over one block, a new
// engine fades the voices and restarts the slot.
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
        patchInit(kSynthEngineAcd, &presets[0]);
        CHECK(synthParamSet(presets[0], 0, lo));
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

TEST(preset_changed_ramps_a_value_and_keeps_the_preset) {
    Rig r;
    CHECK(r.bank.presetSelect(0, 1));
    r.note();
    r.block();
    r.block();
    CHECK_NEAR(r.end0(), r.lo, 1e-4f);
    CHECK(synthParamSet(r.presets[0], 0, r.hi));
    r.block();
    CHECK_NEAR(r.end0(), r.lo, 1e-4f);    // not read before presetChanged
    r.bank.presetChanged(1);
    r.block();
    CHECK_EQ(int(r.bank.presetOf(0)), 1);  // still on the preset
    CHECK_NEAR(r.start0(), r.lo, 1e-4f);   // from the old value
    CHECK_NEAR(r.end0(), r.hi, 1e-4f);     // to the new one
    CHECK_EQ(r.bank.voicesOf(0), 1);       // the note keeps ringing
    r.block();
    CHECK_NEAR(r.start0(), r.hi, 1e-4f);
}

TEST(preset_changed_on_another_preset_or_the_own_patch_does_nothing) {
    Rig r;
    CHECK(synthParamSet(r.own, 0, r.hi));
    r.bank.patchChanged(0);
    r.block();
    CHECK_NEAR(r.end0(), r.hi, 1e-4f);
    CHECK(synthParamSet(r.presets[0], 0, r.lo));
    r.bank.presetChanged(1);              // the slot is on its own patch
    r.block();
    CHECK_EQ(int(r.bank.presetOf(0)), 0);
    CHECK_NEAR(r.end0(), r.hi, 1e-4f);
    patchInit(kSynthEngineAcd, &r.presets[1]);
    CHECK(r.bank.presetSelect(0, 2));
    r.block();
    r.bank.presetChanged(1);
    r.block();
    CHECK_EQ(int(r.bank.presetOf(0)), 2);
}

TEST(preset_changed_with_a_new_engine_restarts_on_it) {
    Rig r;
    CHECK(r.bank.presetSelect(0, 1));
    r.note();
    r.block();
    CHECK_EQ(r.bank.voicesOf(0), 1);
    r.presets[0].engine = kSynthEngineTest;   // as App writes an engine change
    r.bank.presetChanged(1);
    r.block();
    CHECK_EQ(int(r.bank.presetOf(0)), 1);
    CHECK_EQ(r.bank.voicesOf(0), 0);
    CHECK_EQ(int(r.bank.params(0).engine), int(kSynthEngineTest));
    r.bank.presetSelect(0, 0);   // Z0: the own patch again
    r.block();
    CHECK_EQ(int(r.bank.params(0).engine), int(kSynthEngineAcd));
}

// [OT] A preset that carries a polyphony plays at it (voice limit, mono reuse
// at 1); Z0 / Stop give the record's back; a preset without one keeps the
// record's (4 in the rig).
TEST(a_preset_polyphony_limits_the_slot_while_it_plays) {
    Rig r;
    patchSetPoly(r.presets[0], 1);
    CHECK(r.bank.presetSelect(0, 1));
    r.block();
    CHECK_EQ(r.bank.slotView(0).polyphony, 1);
    r.note(0, 60);
    r.note(1, 64);
    r.block();
    CHECK_EQ(r.bank.voicesOf(0), 1);
    // Edited in place: new notes take it.
    patchSetPoly(r.presets[0], 2);
    r.bank.presetChanged(1);
    r.block();
    CHECK_EQ(r.bank.slotView(0).polyphony, 2);
    r.note(2, 67);
    r.block();
    CHECK_EQ(r.bank.voicesOf(0), 2);
    r.bank.transportStop();   // the record's polyphony again
    r.block();
    CHECK_EQ(r.bank.slotView(0).polyphony, 4);
    r.note(3, 70);
    r.note(4, 72);
    r.block();
    CHECK(r.bank.voicesOf(0) >= 3);
}

TEST(a_preset_without_a_polyphony_keeps_the_records) {
    Rig r;
    CHECK(r.bank.presetSelect(0, 1));
    r.block();
    CHECK_EQ(r.bank.slotView(0).polyphony, 4);
    r.note(0, 60);
    r.note(1, 64);
    r.block();
    CHECK_EQ(r.bank.voicesOf(0), 2);
}
