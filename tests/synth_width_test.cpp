// Synth Width (2.x stereo S2): a per-slot widener on the slot bus, after the
// voices and before gain / pan. Width 0 (absent in every existing patch) is
// skipped entirely: bit-identical output. Width > 0 adds a side signal
// S = w * (allpass(M) - M) / 2 of the mid M = (L + R) / 2 as L + S, R - S:
// L + R (mono, the delay / reverb sends) is unchanged, the bass stays
// centred (S = 0 at DC) and the stereo image opens up.
#include "test.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "model/project.h"

using namespace ot;
using namespace ot::audio;

namespace {

struct Rig {
    SynthPatch patch;
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    int16_t stemBuf[2 * kBlock];
    int16_t* stems[kSynthSlots] = {};
    std::vector<float> outL, outR, outD, outV;
    Rig(SynthModel m, float width, bool setWidth = true) {
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        const int32_t e = synthEngineId(m);
        patchInit(e, &patch);
        if (setWidth && synthWidthParam(e) >= 0) CHECK(synthParamSet(patch, synthWidthParam(e), width));
        bank.bindPatch(0, &patch);
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(m);
        r.polyphony = 4;
        r.delaySend = 0.5f;   // the sends must see the same mono sum
        r.reverbSend = 0.5f;
        bank.setSlotRecord(0, r);
        block();
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, stems);
        outL.insert(outL.end(), L, L + kBlock);
        outR.insert(outR.end(), R, R + kBlock);
        outD.insert(outD.end(), D, D + kBlock);
        outV.insert(outV.end(), V, V + kBlock);
    }
    void on(int track, int note) {
        SynthNoteOn n;
        n.slot = 0;
        n.note = uint8_t(note);
        n.volume = -1;
        bank.noteOn(track, n);
    }
    void play(int blocks) {
        on(0, 48);
        on(1, 60);
        for (int b = 0; b < blocks; ++b) block();
        bank.noteOff(0, NoteOffMode::Off);
        bank.noteOff(1, NoteOffMode::Off);
        for (int b = 0; b < blocks; ++b) block();
    }
};

const SynthModel kModels[] = {SynthModel::ACD, SynthModel::FAT, SynthModel::VAP, SynthModel::WTFM};

double sideEnergy(const Rig& r) {
    double e = 0.0;
    for (size_t i = 0; i < r.outL.size(); ++i) e += double(r.outL[i] - r.outR[i]) * double(r.outL[i] - r.outR[i]);
    return e;
}

}  // namespace

TEST(width_zero_is_bit_identical) {
    // Width absent (every existing patch) and Width stored as 0: the same
    // samples as today, bit for bit, mono (L == R) on a centred slot.
    for (SynthModel m : kModels) {
        Rig a(m, 0.0f, false), b(m, 0.0f, true);
        a.play(60);
        b.play(60);
        CHECK(a.outL == b.outL && a.outR == b.outR && a.outD == b.outD && a.outV == b.outV);
        CHECK(a.outL == a.outR);
    }
}

TEST(width_makes_l_differ_from_r_and_keeps_the_mono_sum) {
    for (SynthModel m : kModels) {
        Rig mono(m, 0.0f), wide(m, 1.0f);
        mono.play(60);
        wide.play(60);
        CHECK(sideEnergy(mono) == 0.0);
        CHECK(sideEnergy(wide) > 0.0);
        // L + R and the sends are the mono ones (to float rounding).
        double err = 0.0, ref = 0.0;
        for (size_t i = 0; i < mono.outL.size(); ++i) {
            const double s0 = double(mono.outL[i]) + mono.outR[i], s1 = double(wide.outL[i]) + wide.outR[i];
            err = std::max(err, std::fabs(s1 - s0));
            ref = std::max(ref, std::fabs(s0));
            err = std::max(err, std::fabs(double(wide.outD[i]) - mono.outD[i]));
            err = std::max(err, std::fabs(double(wide.outV[i]) - mono.outV[i]));
        }
        CHECK(ref > 0.01);
        CHECK(err < ref * 1e-5);
    }
}

TEST(width_keeps_the_bass_centred) {
    // A DC offset on the bus (the side signal's lowest band) is not widened:
    // S = 0 at DC. Checked on the side of a slow, low note: much less side
    // energy than on a bright high one.
    Rig low(SynthModel::ACD, 1.0f), high(SynthModel::ACD, 1.0f);
    low.on(0, 24);
    high.on(0, 84);
    for (int b = 0; b < 80; ++b) {
        low.block();
        high.block();
    }
    double sl = 0, ml = 0, sh = 0, mh = 0;
    for (size_t i = 0; i < low.outL.size(); ++i) {
        sl += std::pow(low.outL[i] - low.outR[i], 2.0);
        ml += std::pow(low.outL[i] + low.outR[i], 2.0);
        sh += std::pow(high.outL[i] - high.outR[i], 2.0);
        mh += std::pow(high.outL[i] + high.outR[i], 2.0);
    }
    CHECK(ml > 0 && mh > 0);
    CHECK(sl / ml < sh / mh);
}

TEST(width_change_ramps_without_a_step) {
    // Turning Width up on a sounding note: the side signal grows over the
    // block, no jump at the block start.
    Rig r(SynthModel::VAP, 0.0f);
    r.on(0, 60);
    for (int b = 0; b < 40; ++b) r.block();
    const size_t at = r.outL.size();
    CHECK(synthParamSet(r.patch, synthWidthParam(r.patch.engine), 1.0f));
    r.bank.patchChanged(0);
    r.block();
    r.block();
    // First frames of the edit block: still (almost) mono.
    float d0 = 0.0f, d1 = 0.0f;
    for (size_t i = at; i < at + 8; ++i) d0 = std::max(d0, std::fabs(r.outL[i] - r.outR[i]));
    for (size_t i = at + kBlock; i < at + 2 * kBlock; ++i) d1 = std::max(d1, std::fabs(r.outL[i] - r.outR[i]));
    CHECK(d1 > 0.0f);
    CHECK(d0 < d1 * 0.25f);
}

TEST(stems_are_post_width) {
    Rig r(SynthModel::FAT, 1.0f);
    r.stems[0] = r.stemBuf;
    r.on(0, 60);
    bool differ = false;
    for (int b = 0; b < 40; ++b) {
        r.block();
        for (uint32_t i = 0; i < kBlock; ++i) differ |= r.stemBuf[2 * i] != r.stemBuf[2 * i + 1];
    }
    CHECK(differ);
}

TEST(fat_stereo_reaches_the_engine) {
    // The bank hands the extension parameters to the engine: FAT Stereo on a
    // fat patch opens L / R even with Width 0.
    Rig mono(SynthModel::FAT, 0.0f), st(SynthModel::FAT, 0.0f);
    CHECK(synthParamSet(mono.patch, 0, 1.0f));   // Fatness 1
    CHECK(synthParamSet(st.patch, 0, 1.0f));
    CHECK(synthParamSet(st.patch, synthFatStereoParam(), 1.0f));
    mono.bank.patchChanged(0);
    st.bank.patchChanged(0);
    mono.play(40);
    st.play(40);
    CHECK(sideEnergy(mono) == 0.0);
    CHECK(sideEnergy(st) > 0.0);
}
