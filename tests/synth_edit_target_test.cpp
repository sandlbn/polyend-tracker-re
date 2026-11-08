// [3.x] Edit what you hear: while a synth slot plays a preset (Z, the
// stopped chase, a P1..P8 note), the Synth pages and the Mixer show that
// preset and edits change the preset itself, heard at once and saved; the
// slot stays on it. Z0, Stop and a note of the slot's own instrument show
// the slot's own sound again. A preset carries the slot's polyphony too.
#include "test.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "app/synth_presets.h"
#include "audio/audio_tables.h"
#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/ot3.h"
#include "formats/patch_pb.h"
#include "model/fx.h"
#include "modules/module.h"
#include "modules/synth_models.h"
#include "ui/screens.h"
#include "ui_fakes.h"
#if __has_include("ot_profile.h")
#include "ot_profile.h"
#endif

using namespace ot;
using namespace ot::audio;

namespace {

#if defined(OT_MOD_ACD1) && OT_MOD_ACD1 && defined(OT_MOD_FAT1) && OT_MOD_FAT1
// --- the bank ------------------------------------------------------------------

struct Rig {
    SynthPatch own;
    SynthPatch others[2];
    SynthPatch presets[kSynthPresetCount] = {};
    PresetMix mix[kSynthPresetCount] = {};
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    Rig() {
        initTables();
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        patchInit(synthEngineId(SynthModel::ACD), &own);
        for (SynthPatch& p : presets) p = own;
        presets[kSynthPresetCount - 1].engine = 0;
        bank.bindPatch(0, &own);
        for (int s = 1; s < kSynthSlots; ++s) {
            others[s - 1] = own;
            bank.bindPatch(s, &others[s - 1]);
        }
        bank.bindPresets(presets);
        bank.bindPresetMix(mix);
        for (int s = 0; s < kSynthSlots; ++s) bank.setSlotRecord(s, record(4));
        block();
    }
    static SynthSlotRecord record(int poly) {
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(SynthModel::ACD);
        r.polyphony = uint32_t(poly);
        return r;
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
    }
    void blocks(int n) {
        for (int b = 0; b < n; ++b) block();
    }
    void on(int track, int note, int slot = 0) {
        SynthNoteOn n;
        n.slot = uint8_t(slot);
        n.note = uint8_t(note);
        n.volume = -1;
        bank.noteOn(track, n);
    }
    void pOn(int track, int k, int note) {
        SynthNoteOn n;
        n.slot = 0;
        n.preset = uint8_t(k);
        n.note = uint8_t(note);
        n.volume = -1;
        bank.noteOn(track, n);
    }
};

// A parameter of the engine with a range, and a value inside it that is not v.
int rangedParam(int32_t engine, float v, float* other) {
    int n = 0;
    const PatchParamRange* t = patchEngineParams(engine, &n);
    for (int i = 0; i < n; ++i) {
        if (t[i].flags & kParamFlagNoRange || t[i].max <= t[i].min) continue;
        const float mid = 0.5f * (t[i].min + t[i].max);
        *other = std::fabs(mid - v) > 1e-3f ? mid : t[i].min;
        return i;
    }
    return -1;
}

TEST(a_preset_edit_ramps_a_parameter_and_the_slot_stays_on_the_preset) {
    Rig r;
    r.on(0, 48);
    CHECK(r.bank.presetSelect(0, 1));
    r.blocks(2);
    const int32_t acd = synthEngineId(SynthModel::ACD);
    float nv = 0.0f;
    const int i = rangedParam(acd, 0.0f, &nv);
    CHECK(i >= 0);
    nv = 0.5f * (synthParamGet(r.presets[0], i) + nv);
    synthParamSet(r.presets[0], i, nv);
    r.bank.presetChanged(1);
    r.block();
    CHECK_EQ(int(r.bank.presetOf(0)), 1);
    CHECK_NEAR(r.bank.params(0).end[i], nv, 1e-6);
    CHECK(r.bank.voicesOf(0) == 1);   // the note plays on (a ramp, not a restart)
    // The own patch was not touched; an edit of another preset changes nothing here.
    CHECK(std::fabs(synthParamGet(r.own, i) - nv) > 1e-6f);
    synthParamSet(r.presets[1], i, synthParamGet(r.own, i));
    r.bank.presetChanged(2);
    r.block();
    CHECK_NEAR(r.bank.params(0).end[i], nv, 1e-6);
    r.bank.presetSelect(0, 0);
    r.block();
    CHECK_NEAR(r.bank.params(0).end[i], synthParamGet(r.own, i), 1e-6);
}

TEST(a_new_engine_on_the_preset_restarts_the_slot_on_it) {
    Rig r;
    r.on(0, 48);
    r.bank.presetSelect(0, 1);
    r.blocks(2);
    CHECK_EQ(int(r.bank.voicesOf(0)), 1);
    const int32_t fat = synthEngineId(SynthModel::FAT);
    patchInit(fat, &r.presets[0]);
    r.bank.presetChanged(1);
    r.block();
    CHECK_EQ(int(r.bank.presetOf(0)), 1);
    CHECK_EQ(int(r.bank.params(0).engine), int(fat));
    CHECK_EQ(int(r.bank.voicesOf(0)), 0);   // the old engine's note faded
    r.on(1, 50);
    r.block();
    CHECK_EQ(int(r.bank.voicesOf(0)), 1);
}

TEST(a_preset_carries_its_polyphony) {
    Rig r;
    r.mix[0].polyphony = 1;
    r.mix[0].flags = kPresetMixPoly;   // no mix: only the voices
    r.mix[1].flags = 0;                // saved without polyphony: the slot's 4
    r.bank.presetSelect(0, 1);
    r.block();
    CHECK_EQ(r.bank.slotView(0).polyphony, 4);   // the record stays as it is
    r.on(0, 48);
    r.on(1, 52);
    r.on(2, 55);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(0), 1);   // mono
    r.bank.stopAll();
    r.bank.presetSelect(0, 2);
    r.block();
    r.on(3, 48);
    r.on(4, 52);
    r.on(5, 55);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(0), 3);
    // An edit of the preset's polyphony applies to the next notes (as a record change).
    r.bank.stopAll();
    r.bank.presetSelect(0, 1);
    r.blocks(2);
    r.mix[0].polyphony = 2;
    r.bank.presetChanged(1);
    r.block();
    for (int t = 0; t < 4; ++t) r.on(6 + t, 60 + 2 * t);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(0), 2);
    // Z0: the record's 4 again.
    r.bank.stopAll();
    r.bank.presetSelect(0, 0);
    r.block();
    for (int t = 0; t < 4; ++t) r.on(10 + t, 40 + 2 * t);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(0), 4);
}

TEST(a_preset_instrument_plays_with_the_presets_polyphony) {
    Rig r;
    r.mix[2].polyphony = 1;
    r.mix[2].flags = kPresetMixPoly;
    r.pOn(0, 3, 48);
    r.pOn(1, 3, 52);
    r.blocks(2);
    const int s = r.bank.slotOnPreset(3);
    CHECK(s >= 0);
    CHECK_EQ(int(r.bank.presetOf(s)), 3);
    CHECK_EQ(r.bank.slotOnPreset(2), -1);
    CHECK_EQ(r.bank.voicesOf(s), 1);
    r.bank.transportStop();   // Stop: the record's polyphony again
    r.bank.stopAll();
    r.block();
    for (int t = 0; t < 3; ++t) r.on(4 + t, 60 + 2 * t, s);
    r.blocks(2);
    CHECK_EQ(r.bank.voicesOf(s), 3);
}

TEST(a_preset_mix_edit_ramps_the_gain) {
    Rig r;
    r.mix[0].gainDb = -9.0f;
    r.mix[0].flags = kPresetMixHas;
    r.on(0, 48);
    r.bank.presetSelect(0, 1);
    r.blocks(2);
    CHECK_NEAR(r.bank.slotGain(0), dbToGain(-9.0f), 1e-6);
    r.mix[0].gainDb = 0.0f;
    r.bank.presetChanged(1);
    r.block();
    CHECK_NEAR(r.bank.slotGain(0), 1.0f, 1e-6);
    CHECK_EQ(int(r.bank.presetOf(0)), 1);
}

#endif

}  // namespace
