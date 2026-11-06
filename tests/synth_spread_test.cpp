// Synth Spread: a per-voice place in the stereo field at Poly 2 and up, for every
// engine that offers it, with Width on the same engines. Absent or 0 is bit
// identical to the mono output; L + R and the sends never change, so nothing is
// lost in mono; a note keeps its place; the notes of a triad differ at any root;
// the bass stays centred; Poly 1 stays mono; a live edit ramps.
#include "test.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "model/project.h"
#include "modules/module.h"
#include "modules/synth_models.h"

using namespace ot;
using namespace ot::audio;

namespace {

struct Rig {
    SynthPatch patch;
    Governor gov;
    SynthBank bank;
    SynthMixInputs in;
    int32_t engine;
    float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    std::vector<float> outL, outR, outD, outV;
    Rig(int32_t e, int poly, float spread, float width = 0.0f, bool store = true) : engine(e) {
        gov.setBudget(8);
        bank.setGovernor(&gov);
        bank.setStartsPerBlock(0);
        patchInit(e, &patch);
        if (store) {
            CHECK(synthParamSet(patch, synthSpreadParam(e), spread));
            CHECK(synthParamSet(patch, synthWidthParam(e), width));
        }
        bank.bindPatch(0, &patch);
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = 0;
        for (int m = 0; m < kSynthModelCount; ++m)
            if (synthEngineId(SynthModel(m)) == e) r.model = uint32_t(m);
        r.polyphony = uint32_t(poly);
        r.delaySend = 50.0f;   // the sends must see the same mono sum
        r.reverbSend = 50.0f;
        bank.setSlotRecord(0, r);
        bank.setSlotEngine(0, e >= 0x80 ? e : 0);
        block();
    }
    void block() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        bank.render(in, L, R, D, V, nullptr);
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
    void chord(int root, int blocks) {
        on(0, root);
        on(1, root + 4);
        on(2, root + 7);
        for (int b = 0; b < blocks; ++b) block();
        for (int t = 0; t < 3; ++t) bank.noteOff(t, NoteOffMode::Off);
        for (int b = 0; b < blocks / 2; ++b) block();
    }
};

// Every engine in this build with a Spread.
std::vector<int32_t> spreadEngines() {
    mod::ModuleHost::instance().init();
    std::vector<int32_t> out;
    for (int32_t e = 1; e < 0x100; ++e)
        if (synthEngineFor(e) && synthSpreadParam(e) >= 0) out.push_back(e);
    return out;
}

double energy(const std::vector<float>& a) {
    double x = 0.0;
    for (float v : a) x += double(v) * double(v);
    return x;
}

double correlation(const Rig& r) {
    double ll = 0.0, rr = 0.0, lr = 0.0;
    for (size_t i = 0; i < r.outL.size(); ++i) {
        ll += double(r.outL[i]) * r.outL[i];
        rr += double(r.outR[i]) * r.outR[i];
        lr += double(r.outL[i]) * r.outR[i];
    }
    return ll > 0.0 && rr > 0.0 ? lr / std::sqrt(ll * rr) : 1.0;
}

// The largest difference of L + R and the sends from `mono`, over the largest L + R.
double sumError(const Rig& mono, const Rig& wide) {
    double err = 0.0, ref = 0.0;
    for (size_t i = 0; i < mono.outL.size(); ++i) {
        const double s0 = double(mono.outL[i]) + mono.outR[i], s1 = double(wide.outL[i]) + wide.outR[i];
        err = std::max(err, std::fabs(s1 - s0));
        ref = std::max(ref, std::fabs(s0));
        err = std::max(err, std::fabs(double(wide.outD[i]) - mono.outD[i]));
        err = std::max(err, std::fabs(double(wide.outV[i]) - mono.outV[i]));
    }
    return ref > 0.0 ? err / ref : 0.0;
}

}  // namespace

TEST(spread_engines_in_this_build) {
    const std::vector<int32_t> e = spreadEngines();
    std::printf("  %d engines with Spread\n", int(e.size()));
    CHECK(!e.empty());
    for (int32_t id : e) {
        CHECK(synthWidthParam(id) >= 0);
        const PatchParamRange* r = synthParamRange(id, synthSpreadParam(id));
        CHECK(r && r->def == 0.0f && r->min == 0.0f && r->max == 1.0f && (r->flags & kParamFlagInitAbsent));
        // The Init patch leaves it out: no new bytes in a patch that does not use it.
        SynthPatch p;
        CHECK(patchInit(id, &p));
        CHECK_EQ(int(p.extCount), 0);
    }
}

TEST(spread_absent_or_0_is_bit_identical) {
    for (int32_t e : spreadEngines()) {
        Rig a(e, 4, 0.0f, 0.0f, false), b(e, 4, 0.0f, 0.0f, true);
        a.chord(60, 60);
        b.chord(60, 60);
        CHECK(a.outL == b.outL && a.outR == b.outR && a.outD == b.outD && a.outV == b.outV);
        CHECK(a.outL == a.outR);
    }
}

TEST(spread_opens_a_chord_and_keeps_the_mono_sum) {
    for (int32_t e : spreadEngines()) {
        Rig mono(e, 4, 0.0f), wide(e, 4, 1.0f);
        mono.chord(60, 120);
        wide.chord(60, 120);
        if (energy(mono.outL) < 1e-3) continue;   // an Init patch silent on these notes
        const double c = correlation(wide), err = sumError(mono, wide);
        std::printf("  engine 0x%02x: L / R correlation %.3f, mono sum error %.1e\n", unsigned(e), c, err);
        CHECK(c < 0.9);
        CHECK(err < 1e-5);
    }
}

TEST(width_on_every_spread_engine_keeps_the_mono_sum) {
    for (int32_t e : spreadEngines()) {
        Rig mono(e, 4, 0.0f), wide(e, 4, 0.0f, 1.0f);
        mono.chord(60, 120);
        wide.chord(60, 120);
        if (energy(mono.outL) < 1e-3) continue;
        CHECK(correlation(wide) < 0.999);
        CHECK(sumError(mono, wide) < 1e-5);
    }
}

TEST(spread_poly_1_stays_mono) {
    for (int32_t e : spreadEngines()) {
        Rig plain(e, 1, 0.0f), r(e, 1, 1.0f);
        plain.chord(60, 60);
        r.chord(60, 60);
        CHECK(r.outL == r.outR);
        CHECK(r.outL == plain.outL);
    }
}

TEST(spread_places_triads_apart_at_any_root) {
    for (int root = 48; root < 96; ++root) {
        for (int third : {3, 4}) {
            const float a = synthSpreadPlace(uint8_t(root)), b = synthSpreadPlace(uint8_t(root + third)),
                        c = synthSpreadPlace(uint8_t(root + 7));
            CHECK(a != b && a != c && b != c);
            CHECK(std::max({a, b, c}) - std::min({a, b, c}) >= 1.5f);
        }
    }
    for (int n = 48; n < 128; ++n) {
        CHECK(synthSpreadPlace(uint8_t(n)) >= -1.0f && synthSpreadPlace(uint8_t(n)) <= 1.0f);
    }
}

TEST(spread_keeps_the_bass_centred) {
    for (int n = 0; n <= 36; ++n) CHECK(synthSpreadPlace(uint8_t(n)) == 0.0f);
    for (int n = 37; n < 48; ++n) {
        const float full = 0.5f * float(n % 5) - 1.0f;
        CHECK(std::fabs(synthSpreadPlace(uint8_t(n))) <= std::fabs(full));
    }
    const int32_t e = spreadEngines().front();
    Rig r(e, 4, 1.0f);
    r.on(0, 36);
    for (int b = 0; b < 60; ++b) r.block();
    CHECK(r.outL == r.outR);
}

TEST(spread_sets_the_side_a_note_plays_on) {
    const int32_t e = spreadEngines().front();
    const struct { int note; bool left, right; } kCases[] = {{60, true, false}, {64, false, true}};   // places -1, +1
    for (const auto& c : kCases) {
        CHECK_EQ(int(synthSpreadPlace(uint8_t(c.note)) * 2.0f), c.left ? -2 : 2);
        Rig r(e, 4, 1.0f);
        r.on(0, c.note);
        for (int b = 0; b < 60; ++b) r.block();
        // From its first frame: a new note does not sweep to its place.
        CHECK((energy(r.outL) > 0.0) == c.left);
        CHECK((energy(r.outR) > 0.0) == c.right);
    }
}

TEST(spread_live_edit_ramps) {
    // A held note at the place -1 (left): (L - R) / (L + R) is the Spread at that frame.
    const int32_t e = spreadEngines().front();
    Rig r(e, 4, 0.0f);
    r.on(0, 60);
    for (int b = 0; b < 40; ++b) r.block();
    CHECK(synthParamSet(r.patch, synthSpreadParam(e), 1.0f));
    r.bank.patchChanged(0);
    r.block();
    double first = 0.0, last = 0.0;
    int nf = 0, nl = 0;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const double sum = double(r.L[i]) + r.R[i];
        if (std::fabs(sum) < 1e-4) continue;
        const double s = (double(r.L[i]) - r.R[i]) / sum;
        if (i < 8) first += s, ++nf;
        if (i >= kBlock - 8) last += s, ++nl;
    }
    CHECK(nf > 0 && nl > 0);
    if (nf && nl) {
        CHECK(first / nf < 0.1);
        CHECK(last / nl > 0.9);
    }
}

TEST(spread_poly_change_returns_to_the_middle_without_a_jump) {
    const int32_t e = spreadEngines().front();
    Rig r(e, 4, 1.0f);
    r.on(0, 60);
    for (int b = 0; b < 40; ++b) r.block();
    SynthSlotRecord rec = r.bank.slotRecord(0);
    rec.polyphony = 1;
    r.bank.setSlotRecord(0, rec);
    for (int b = 0; b < 3; ++b) r.block();
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(r.L[i] == r.R[i]);
}
