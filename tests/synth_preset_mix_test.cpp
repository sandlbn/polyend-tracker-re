// Z with a preset that carries a slot mix (gain, pan, reverb / delay send):
// the slot plays at the preset's mix with its patch, exactly as if its own
// record held those values. Z0, Stop, an edit of the patch or a reset bring
// the record's mix back; a preset without a mix keeps the record's. The
// Performance sends of a track add to the mix the slot plays.
#include "test.h"

#include <cmath>

#include "audio/audio_tables.h"
#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "model/fx.h"
#include "model/project.h"

using namespace ot;
using namespace ot::audio;

namespace {

struct Mix {
    float gain, pan, reverb, delay;
};
constexpr Mix kOwn{-9.0f, 0.0f, 0.0f, 0.0f};
constexpr Mix kPre{-3.0f, -40.0f, 30.0f, 60.0f};
constexpr Mix kOther{-15.0f, 70.0f, 10.0f, 5.0f};

SynthSlotRecord recOf(const Mix& m) {
    SynthSlotRecord r = SynthSlotRecord::defaults(0);
    r.model = uint32_t(SynthModel::ACD);
    r.polyphony = 4;
    r.gainDb = m.gain;
    r.pan = m.pan;
    r.reverbSend = m.reverb;
    r.delaySend = m.delay;
    return r;
}

struct Rig {
    SynthPatch own;
    SynthPatch presets[kSynthPresetCount] = {};
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    explicit Rig(const Mix& rec) {
        initTables();   // the SEND law
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        patchInit(kSynthEngineAcd, &own);
        presets[0] = own;   // the same sound: only the mix differs
        presets[1] = own;
        bank.bindPatch(0, &own);
        bank.bindPresets(presets);
        bank.setSlotRecord(0, recOf(rec));
        block();
        block();
    }
    void withMix(int n, const Mix& m) { patchSetMix(presets[n - 1], m.gain, m.pan, m.reverb, m.delay); }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
    }
    void note(int track = 0, uint8_t key = 57) {
        SynthNoteOn n;
        n.slot = 0;
        n.note = key;
        n.volume = -1;
        bank.noteOn(track, n);
    }
};

// The Z applied in its own block (a Z and a note on one step ramp the mix over
// the note's first block, as a record change does), then a note on each.
void notes(Rig& a, Rig& b, int track = 0) {
    a.block();
    b.block();
    a.note(track);
    b.note(track);
}

// Both rigs render `blocks` blocks; true when every bus matches sample for
// sample (and something was heard on each of the given buses).
bool same(Rig& a, Rig& b, int blocks, bool sends = true) {
    bool ok = true;
    double l = 0, r = 0, d = 0, v = 0;
    for (int k = 0; k < blocks; ++k) {
        a.block();
        b.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float tol = 1e-6f;
            if (std::fabs(a.L[i] - b.L[i]) > tol || std::fabs(a.R[i] - b.R[i]) > tol ||
                std::fabs(a.D[i] - b.D[i]) > tol || std::fabs(a.V[i] - b.V[i]) > tol)
                ok = false;
            l += std::fabs(a.L[i]);
            r += std::fabs(a.R[i]);
            d += std::fabs(a.D[i]);
            v += std::fabs(a.V[i]);
        }
    }
    if (l + r <= 0.0) ok = false;
    if (sends && (d <= 0.0 || v <= 0.0)) ok = false;
    return ok;
}

}  // namespace

TEST(z_on_a_mix_preset_plays_at_its_mix) {
    Rig a(kPre);
    Rig b(kOwn);
    b.withMix(1, kPre);
    CHECK(b.bank.presetSelect(0, 1));
    notes(a, b);
    CHECK(same(a, b, 20));
    // And the own record differs from it: the test hears the mix.
    Rig c(kOwn);
    c.note();
    Rig a2(kPre);
    a2.note();
    CHECK(!same(a2, c, 4, false));
}

TEST(z_between_two_mix_presets_ramps_to_the_second) {
    Rig a(kPre);
    Rig b(kOwn);
    b.withMix(1, kPre);
    b.withMix(2, kOther);
    b.bank.presetSelect(0, 1);
    notes(a, b);
    CHECK(same(a, b, 4));
    b.bank.presetSelect(0, 2);
    a.bank.setSlotRecord(0, recOf(kOther));
    a.bank.patchChanged(0);   // the same rebind in both
    CHECK(same(a, b, 10));
}

TEST(preset_without_a_mix_keeps_the_records) {
    Rig a(kOther);
    Rig b(kOther);
    b.bank.presetSelect(0, 1);   // no mix stored
    notes(a, b);
    CHECK(same(a, b, 10));
    // A partial mix (an entry without a value) is none either.
    Rig c(kOther);
    c.withMix(1, kPre);
    c.presets[0].mix[kPatchMixPan].present = false;
    c.bank.presetSelect(0, 1);
    Rig d(kOther);
    notes(c, d);
    CHECK(same(c, d, 10));
    // So is one with a value that is not a number.
    Rig e(kOther);
    e.withMix(1, Mix{-3.0f, NAN, 30.0f, 60.0f});
    e.bank.presetSelect(0, 1);
    Rig f(kOther);
    notes(e, f);
    CHECK(same(e, f, 10));
}

TEST(z0_stop_and_a_patch_edit_restore_the_records_mix) {
    for (int way = 0; way < 3; ++way) {
        Rig a(kOwn);
        Rig b(kOwn);
        b.withMix(1, kPre);
        b.bank.presetSelect(0, 1);
        notes(a, b);
        CHECK(!same(a, b, 3, false));
        if (way == 0) b.bank.presetSelect(0, 0);
        if (way == 1) b.bank.transportStop();
        if (way == 2) b.bank.patchChanged(0);
        a.bank.patchChanged(0);
        b.block();   // the ramp block
        a.block();
        CHECK_EQ(int(b.bank.presetOf(0)), 0);
        CHECK(same(a, b, 6, false));
    }
}

TEST(reset_restores_the_records_mix) {
    Rig a(kOwn);
    Rig b(kOwn);
    b.withMix(1, kPre);
    b.bank.presetSelect(0, 1);
    b.note();
    b.block();
    b.bank.reset();
    a.bank.reset();
    b.bank.setSlotRecord(0, recOf(kOwn));
    a.bank.setSlotRecord(0, recOf(kOwn));
    a.block();
    b.block();
    notes(a, b);
    CHECK(same(a, b, 6, false));
}

TEST(record_edit_on_a_mix_preset_waits_for_z0) {
    Rig a(kPre);
    Rig b(kOwn);
    b.withMix(1, kPre);
    b.bank.presetSelect(0, 1);
    notes(a, b);
    CHECK(same(a, b, 3));
    b.bank.setSlotRecord(0, recOf(kOther));   // the bank alone: the preset's mix plays on
    CHECK(same(a, b, 4));
    b.bank.presetSelect(0, 0);
    a.bank.setSlotRecord(0, recOf(kOther));
    a.bank.patchChanged(0);
    CHECK(same(a, b, 6));
}

TEST(performance_sends_add_to_the_presets_sends) {
    SynthPerf pf;
    pf.dsend = 10;
    pf.rsend = -10;
    Rig a(kPre);
    Rig b(kOwn);
    b.withMix(1, kPre);
    b.bank.presetSelect(0, 1);
    a.bank.setTrackPerf(0, pf);
    b.bank.setTrackPerf(0, pf);
    notes(a, b, 0);
    CHECK(same(a, b, 10));
}

TEST(a_mix_value_out_of_range_is_held_at_the_limit) {
    Rig a(Mix{12.0f, 100.0f, 100.0f, 0.0f});
    Rig b(kOwn);
    b.withMix(1, Mix{40.0f, 300.0f, 1000.0f, -5.0f});
    b.bank.presetSelect(0, 1);
    notes(a, b);
    CHECK(same(a, b, 6, false));
    SynthPerf pf;
    pf.dsend = 20;
    a.bank.setTrackPerf(0, pf);
    b.bank.setTrackPerf(0, pf);
    a.note(0, 64);
    b.note(0, 64);
    CHECK(same(a, b, 6, false));
}
