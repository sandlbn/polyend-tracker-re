// [3.x] A synth preset carries the slot mix: while a slot plays preset n (Z)
// it plays with preset n's volume, pan and delay / reverb sends -- the ones
// the slot had when the preset was stored. A preset saved before presets had
// a mix keeps the slot's own. Z0, Stop or an edit of the slot's patch bring
// the slot's own mix back, with its patch. An edit of its own mix (App) is
// heard at once on the preset's patch, until the next Z. The Mixer and the
// Synth page show and edit the slot's own mix. Store, Load
// and Clear carry the mix; it is saved in project.ot3x, one chunk per preset.
#include "test.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "app/synth_presets.h"
#include "audio/audio_tables.h"
#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/crc32.h"
#include "formats/ot3.h"
#include "formats/patch_pb.h"
#include "model/fx.h"
#include "model/project.h"
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

#if defined(OT_MOD_ACD1) && OT_MOD_ACD1
struct Rig {
    SynthPatch own;
    SynthPatch presets[kSynthPresetCount] = {};
    PresetMix mix[kSynthPresetCount] = {};
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    std::vector<float> outL, outR, outD, outV;
    explicit Rig(float gainDb = -9.0f) {
        initTables();
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        patchInit(synthEngineId(SynthModel::ACD), &own);
        for (SynthPatch& p : presets) p = own;   // the same patch: only the mix differs
        presets[kSynthPresetCount - 1].engine = 0;   // preset 8 empty
        bank.bindPatch(0, &own);
        bank.bindPresets(presets);
        bank.bindPresetMix(mix);
        bank.setSlotRecord(0, record(gainDb));
        block();
    }
    static SynthSlotRecord record(float gainDb, float pan = 0.0f, float rev = 0.0f, float del = 0.0f) {
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(SynthModel::ACD);
        r.polyphony = 4;
        r.gainDb = gainDb;
        r.pan = pan;
        r.reverbSend = rev;
        r.delaySend = del;
        return r;
    }
    void presetMix(int n, float gainDb, float pan = 0.0f, float rev = 0.0f, float del = 0.0f) {
        PresetMix& m = mix[n - 1];
        m = PresetMix{};
        m.gainDb = gainDb;
        m.pan = pan;
        m.reverbSend = rev;
        m.delaySend = del;
        m.flags = kPresetMixHas;
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
        outL.insert(outL.end(), L, L + kBlock);
        outR.insert(outR.end(), R, R + kBlock);
        outD.insert(outD.end(), D, D + kBlock);
        outV.insert(outV.end(), V, V + kBlock);
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
    size_t at() const { return outL.size(); }
};

double energy(const std::vector<float>& x, size_t from, size_t to) {
    double e = 0.0;
    for (size_t i = from; i < to && i < x.size(); ++i) e += double(x[i]) * x[i];
    return e;
}
double dbRatio(double db) { return std::pow(10.0, db / 10.0); }   // energy ratio

// Two rigs play the same note; `wet` switches, `dry` stays. Energy of the
// blocks [from, from + n) of wet over dry.
double ratio(const std::vector<float>& wet, const std::vector<float>& dry, size_t from, int n) {
    return energy(wet, from, from + size_t(n) * kBlock) / energy(dry, from, from + size_t(n) * kBlock);
}

TEST(z_plays_the_presets_gain_and_z0_restores_own) {
    Rig dry, wet;
    wet.presetMix(1, -3.0f);   // 6 dB above the own -9 dB
    dry.on();
    wet.on();
    dry.blocks(6);
    wet.blocks(6);
    CHECK_NEAR(wet.bank.slotGain(0), dbToGain(-9.0f), 1e-6);
    wet.bank.presetSelect(0, 1);
    dry.bank.presetSelect(0, 2);   // no mix: the own gain
    dry.blocks(2);
    wet.blocks(2);   // the ramp
    CHECK_NEAR(wet.bank.slotGain(0), dbToGain(-3.0f), 1e-6);
    CHECK_NEAR(dry.bank.slotGain(0), dbToGain(-9.0f), 1e-6);
    size_t from = wet.at();
    dry.blocks(8);
    wet.blocks(8);
    CHECK_NEAR(ratio(wet.outL, dry.outL, from, 8), dbRatio(6.0), 2e-2);
    CHECK_NEAR(ratio(wet.outR, dry.outR, from, 8), dbRatio(6.0), 2e-2);
    wet.bank.presetSelect(0, 0);
    dry.bank.presetSelect(0, 0);
    dry.blocks(2);
    wet.blocks(2);
    from = wet.at();
    dry.blocks(8);
    wet.blocks(8);
    CHECK_NEAR(ratio(wet.outL, dry.outL, from, 8), 1.0, 1e-3);
    CHECK_NEAR(wet.bank.slotGain(0), dbToGain(-9.0f), 1e-6);
}

TEST(the_gain_ramps_over_the_z_block) {
    // No step: the first block after the Z moves from the old gain to the new.
    Rig r;
    r.presetMix(1, 12.0f);
    r.on();
    r.blocks(6);
    float before = 0.0f;
    for (uint32_t i = kBlock - 8; i < kBlock; ++i) before = std::fmax(before, std::fabs(r.L[i]));
    r.bank.presetSelect(0, 1);
    r.block();
    float head = 0.0f, tail = 0.0f;
    for (uint32_t i = 0; i < 8; ++i) head = std::fmax(head, std::fabs(r.L[i]));
    for (uint32_t i = kBlock - 8; i < kBlock; ++i) tail = std::fmax(tail, std::fabs(r.L[i]));
    CHECK(before > 0.0f);
    CHECK(head < 2.0f * before);   // 21 dB up only at the end of the block
    CHECK(tail > 4.0f * head);
}

TEST(the_presets_pan_moves_the_slot) {
    Rig r;
    r.presetMix(1, -9.0f, 100.0f);   // hard right
    r.on();
    r.blocks(4);
    const size_t a = r.at();
    r.blocks(4);
    CHECK(energy(r.outL, a, r.at()) > 0.0);
    r.bank.presetSelect(0, 1);
    r.blocks(2);
    const size_t b = r.at();
    r.blocks(6);
    CHECK(energy(r.outR, b, r.at()) > 0.0);
    CHECK(energy(r.outL, b, r.at()) < 1e-9 * energy(r.outR, b, r.at()));
    r.bank.presetSelect(0, 0);
    r.blocks(2);
    const size_t c = r.at();
    r.blocks(4);
    CHECK_NEAR(energy(r.outL, c, r.at()) / energy(r.outR, c, r.at()), 1.0, 1e-3);
}

TEST(the_presets_sends_feed_delay_and_reverb) {
    Rig r;
    r.presetMix(1, -9.0f, 0.0f, 80.0f, 40.0f);
    r.on();
    r.blocks(4);
    const size_t a = r.at();
    r.blocks(4);
    CHECK(energy(r.outD, a, r.at()) == 0.0);   // own sends 0
    CHECK(energy(r.outV, a, r.at()) == 0.0);
    r.bank.presetSelect(0, 1);
    r.blocks(2);
    const size_t b = r.at();
    r.blocks(4);
    CHECK(energy(r.outD, b, r.at()) > 0.0);
    CHECK(energy(r.outV, b, r.at()) > energy(r.outD, b, r.at()));
    r.bank.presetSelect(0, 0);
    r.blocks(2);
    const size_t c = r.at();
    r.blocks(4);
    CHECK(energy(r.outD, c, r.at()) == 0.0);
    CHECK(energy(r.outV, c, r.at()) == 0.0);
}

TEST(preset_without_mix_data_keeps_the_own_mix) {
    Rig dry, wet;
    wet.mix[1] = PresetMix{};      // preset 2: saved before presets had a mix
    wet.mix[1].gainDb = 12.0f;     // (values without the flag are not used)
    dry.on();
    wet.on();
    dry.blocks(4);
    wet.blocks(4);
    wet.bank.presetSelect(0, 2);
    wet.blocks(2);
    dry.blocks(2);
    CHECK_EQ(int(wet.bank.presetOf(0)), 2);
    const size_t from = wet.at();
    wet.blocks(6);
    dry.blocks(6);
    CHECK_NEAR(ratio(wet.outL, dry.outL, from, 6), 1.0, 1e-3);
}

TEST(own_record_change_on_a_mix_preset_waits_for_z0) {
    Rig r;
    r.presetMix(1, -3.0f);
    r.bank.presetSelect(0, 1);
    r.blocks(2);
    CHECK_NEAR(r.bank.slotGain(0), dbToGain(-3.0f), 1e-6);
    r.bank.setSlotRecord(0, Rig::record(0.0f));   // the bank's own record (the App also calls patchChanged)
    r.blocks(2);
    CHECK_NEAR(r.bank.slotGain(0), dbToGain(-3.0f), 1e-6);
    r.bank.presetSelect(0, 0);
    r.blocks(2);
    CHECK_NEAR(r.bank.slotGain(0), 1.0f, 1e-6);
}

TEST(own_mix_wins_on_the_presets_patch_until_the_next_z) {
    Rig r;
    r.presetMix(1, -3.0f);
    r.presetMix(2, 6.0f);
    r.on();
    r.bank.presetSelect(0, 1);
    r.blocks(2);
    r.bank.ownMixWins(0);
    r.bank.setSlotRecord(0, Rig::record(0.0f));
    r.blocks(2);
    CHECK_EQ(int(r.bank.presetOf(0)), 1);   // still the preset's patch
    CHECK_NEAR(r.bank.slotGain(0), 1.0f, 1e-6);
    r.bank.setSlotRecord(0, Rig::record(-1.0f));   // further edits follow
    r.blocks(1);
    CHECK_NEAR(r.bank.slotGain(0), dbToGain(-1.0f), 1e-6);
    // A repeated Z of the same preset takes the edit back.
    r.bank.presetSelect(0, 1);
    r.blocks(1);
    CHECK_NEAR(r.bank.slotGain(0), dbToGain(-3.0f), 1e-6);
    // Another Z, Z0, Stop, a patch edit and a reset too.
    for (int k = 0; k < 5; ++k) {
        r.bank.presetSelect(0, 1);
        r.blocks(1);
        r.bank.ownMixWins(0);
        r.bank.setSlotRecord(0, Rig::record(float(-20 - k)));
        r.blocks(1);
        CHECK_NEAR(r.bank.slotGain(0), dbToGain(float(-20 - k)), 1e-6);
        if (k == 0) r.bank.presetSelect(0, 2);
        if (k == 1) r.bank.presetSelect(0, 0);
        if (k == 2) r.bank.transportStop();
        if (k == 3) r.bank.patchChanged(0);
        if (k == 4) r.bank.reset();
        r.blocks(1);
        if (k == 0) CHECK_NEAR(r.bank.slotGain(0), dbToGain(6.0f), 1e-5);
        r.bank.presetSelect(0, 1);
        r.blocks(1);
        CHECK_NEAR(r.bank.slotGain(0), dbToGain(-3.0f), 1e-6);   // the flag went with it
    }
}

TEST(own_record_change_on_a_preset_without_mix_is_heard_at_once) {
    Rig r;
    r.bank.presetSelect(0, 3);   // no mix data
    r.blocks(2);
    r.bank.setSlotRecord(0, Rig::record(-20.0f));
    r.blocks(1);
    CHECK_EQ(int(r.bank.presetOf(0)), 3);
    CHECK_NEAR(r.bank.slotGain(0), dbToGain(-20.0f), 1e-6);
}

TEST(transport_stop_patch_changed_and_reset_restore_own_mix) {
    for (int k = 0; k < 3; ++k) {
        Rig r;
        r.presetMix(1, 6.0f);
        r.bank.presetSelect(0, 1);
        r.blocks(2);
        CHECK_NEAR(r.bank.slotGain(0), dbToGain(6.0f), 1e-5);
        if (k == 0) r.bank.transportStop();
        if (k == 1) r.bank.patchChanged(0);
        if (k == 2) r.bank.reset();
        r.blocks(2);
        CHECK_EQ(int(r.bank.presetOf(0)), 0);
        CHECK_NEAR(r.bank.slotGain(0), dbToGain(-9.0f), 1e-6);
    }
}

TEST(an_emptied_preset_or_no_table_plays_the_own_mix) {
    Rig r;
    r.presetMix(8, 6.0f);   // preset 8 is empty: a Z of it does nothing
    CHECK(!r.bank.presetSelect(0, 8));
    r.blocks(1);
    CHECK_NEAR(r.bank.slotGain(0), dbToGain(-9.0f), 1e-6);
    Rig n;
    n.bank.bindPresetMix(nullptr);
    n.presetMix(1, 6.0f);
    n.bank.presetSelect(0, 1);
    n.blocks(1);
    CHECK_NEAR(n.bank.slotGain(0), dbToGain(-9.0f), 1e-6);
}

TEST(performance_sends_start_from_the_presets_sends) {
    // A track's Performance send offset adds to the send the slot plays
    // with: the preset's while on it. Same D-bus level as a slot whose own
    // record has the preset's send and the same offset.
    Rig ref, wet, own;
    ref.bank.setSlotRecord(0, Rig::record(-9.0f, 0.0f, 0.0f, 50.0f));
    wet.presetMix(1, -9.0f, 0.0f, 0.0f, 50.0f);
    SynthPerf p;
    p.dsend = 10;
    for (Rig* x : {&ref, &wet, &own}) {
        x->bank.setTrackPerf(0, p);
        x->on();
    }
    wet.bank.presetSelect(0, 1);
    for (Rig* x : {&ref, &wet, &own}) x->blocks(4);
    const size_t from = wet.at();
    for (Rig* x : {&ref, &wet, &own}) x->blocks(6);
    CHECK(energy(ref.outD, from, ref.at()) > 0.0);
    CHECK_NEAR(ratio(wet.outD, ref.outD, from, 6), 1.0, 1e-3);
    CHECK(ratio(wet.outD, own.outD, from, 6) > 10.0);
    // Back on the own sends (0): the offset alone, as on a slot never on the preset.
    wet.bank.presetSelect(0, 0);
    for (Rig* x : {&ref, &wet, &own}) x->blocks(4);
    const size_t b = wet.at();
    for (Rig* x : {&ref, &wet, &own}) x->blocks(6);
    CHECK(energy(own.outD, b, own.at()) > 0.0);
    CHECK_NEAR(ratio(wet.outD, own.outD, b, 6), 1.0, 1e-3);
}
#endif  // OT_MOD_ACD1

}  // namespace
