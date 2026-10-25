// PERC drum machine engine (2.x WP-F, audio/engines/perc.*): drum per note,
// per-drum mixer (level, pan, sends, output), decay / pitch / tone effects,
// overlapping drums inside the one voice, velocity per hit, the fade of a
// stolen voice, framework dispatch through the registry, parameter fuzzing
// (no NaN), and every factory PERC patch playing a 1-bar pattern
// ($OT_VENDOR_FIXTURES/mini-patches-2.0/PERC, skipped when absent).
#include <new>
#include "test.h"

#include <cstdlib>
#include <dirent.h>
#include <string>
#include <vector>

#include "audio/engines/perc.h"
#include "audio/synth_bank.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"

using namespace ot;
using namespace ot::audio;

namespace {

ISynthEngine* E() { return synthEngineFor(kSynthEnginePerc); }

// One slot + one voice driven directly (no framework).
struct Rig {
    SynthPatch patch;
    SynthParams p;
    SynthSlotScratch sc;
    SynthVoice v;
    bool alive = false;
    float L[kBlock], R[kBlock];
    Rig() {
        patchInit(kSynthEnginePerc, &patch);
        E()->slotReset(sc);
        v.slotScratch = &sc;
        v.slot = 0;
        sync();
    }
    void sync() {
        const uint8_t m[kSynthSeqMacros] = {};
        p.engine = kSynthEnginePerc;
        p.count = synthEffectiveParams(patch, m, 1.0f, p.end);
        for (int i = 0; i < p.count; ++i) p.start[i] = p.end[i];
    }
    void set(int i, float val) {
        synthParamSet(patch, i, val);
        sync();
    }
    void on(int note, float vel = 1.0f) {
        SynthNote n;
        n.note = uint8_t(note);
        n.pitch = float(note);
        n.velocity = vel;
        n.reused = alive;
        n.overlap = alive;
        if (!alive) std::memset(v.state, 0, sizeof(v.state));
        v.gate = true;
        lastVel = vel;
        E()->noteOn(v, p, n);
        alive = true;
    }
    float lastVel = 1.0f;
    // One block; g = framework gain (default: the last note's velocity, as
    // the framework passes it); fading: the framework's steal / cut (last
    // block, SynthVoice::fading).
    bool block(float g0 = -1.0f, float gInc = 0.0f, bool fading = false) {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = 0.0f;
        if (!alive) return false;
        if (g0 < 0.0f) g0 = lastVel;
        v.fading = fading;
        alive = E()->render(v, p, g0, gInc, L, R);
        return alive;
    }
};

struct Stats {
    double eL = 0, eR = 0, peak = 0, diff = 0;
    int zc = 0;
    bool finite = true;
    double e() const { return eL + eR; }
    double bright() const { return eL + eR > 0 ? diff / (eL + eR) : 0; }
};

// Render n blocks (from block `skip` on) and collect statistics.
Stats run(Rig& r, int n, int skip = 0) {
    Stats s;
    float prev = 0.0f;
    for (int b = 0; b < n; ++b) {
        r.block();
        if (b < skip) continue;
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float l = r.L[i], rr = r.R[i];
            if (!std::isfinite(l) || !std::isfinite(rr)) s.finite = false;
            s.eL += double(l) * l;
            s.eR += double(rr) * rr;
            const double a = std::fabs(l) > std::fabs(rr) ? std::fabs(l) : std::fabs(rr);
            if (a > s.peak) s.peak = a;
            const float m = l + rr;
            if ((m >= 0.0f) != (prev >= 0.0f)) ++s.zc;
            const float d = m - prev;
            s.diff += double(d) * d * 0.25;
            prev = m;
        }
    }
    return s;
}

Stats hit(int note, void (*setup)(Rig&) = nullptr, int blocks = 40, int skip = 0) {
    Rig r;
    if (setup) setup(r);
    r.on(note);
    return run(r, blocks, skip);
}

constexpr int kMix = perc::kMixer;
int mixIdx(perc::Drum d, int off) { return kMix + 5 * int(d) + off; }

const int kNotes[] = {60, 48, 62, 64, 69, 71};   // drum order: Kick, Tom, Snare, Hat, Cymbal, Perc

}  // namespace

TEST(perc_registry_and_state) {
    CHECK(E() != nullptr);
    CHECK(E() == percEngine());
    CHECK_EQ(E()->id(), int32_t(kSynthEnginePerc));
    CHECK(std::strcmp(E()->name(), "PERC") == 0);
    CHECK(perc::slotStateBytes() <= kSynthSlotScratchBytes);
    // Every 2.0 model has its own engine (none plays the internal test engine).
    CHECK(synthEngineFor(kSynthEngineAcd) != synthTestEngine());
    CHECK(synthEngineFor(kSynthEngineVap) != synthTestEngine());
    CHECK(synthEngineFor(kSynthEngineAcd) != E() && synthEngineFor(kSynthEngineVap) != E());
}

TEST(perc_note_map) {
    int var = -1;
    CHECK_EQ(perc::drumForNote(60), perc::kKick);
    CHECK_EQ(perc::drumForNote(62), perc::kSnare);
    for (int n = 64; n <= 67; ++n) {
        CHECK_EQ(perc::drumForNote(uint8_t(n), &var), perc::kHat);
        CHECK_EQ(var, n - 64);
    }
    CHECK_EQ(perc::drumForNote(69), perc::kCymbal);
    CHECK_EQ(perc::drumForNote(71), perc::kPerc);
    for (int n : {61, 63, 68, 70}) CHECK_EQ(perc::drumForNote(uint8_t(n)), perc::kNone);
    for (int n : {0, 24, 48, 59, 72, 96, 127}) CHECK_EQ(perc::drumForNote(uint8_t(n)), perc::kTom);
}

TEST(perc_each_drum_triggers_on_its_note) {
    for (int d = 0; d < perc::kDrums; ++d) {
        const Stats s = hit(kNotes[d]);
        CHECK(s.finite);
        CHECK(s.e() > 1.0);
        CHECK(s.peak < 4.0);
        // Only that drum sounds: muting it (level 0) silences the note.
        Rig r;
        r.set(mixIdx(perc::Drum(d), 0), 0.0f);
        r.on(kNotes[d]);
        CHECK(run(r, 20).e() == 0.0);
    }
    // Every hi-hat note sounds; a gap note plays nothing and the voice ends.
    for (int n = 64; n <= 67; ++n) CHECK(hit(n).e() > 1.0);
    Rig r;
    r.on(61);
    CHECK(!r.block());
    CHECK(run(r, 4).e() == 0.0);
}

TEST(perc_voice_ends_when_silent) {
    Rig r;
    r.on(64);   // closed hat 0.16 s
    int b = 0;
    while (r.block() && b < 2000) ++b;
    CHECK(b > 10);
    CHECK(b < 200);
    CHECK(!r.alive);
}

TEST(perc_level_pan) {
    for (int d = 0; d < perc::kDrums; ++d) {
        const perc::Drum D = perc::Drum(d);
        // Level 2 doubles the amplitude (the drums are linear before FX).
        Rig a, c;
        a.set(mixIdx(D, 1), 0.0f);
        c.set(mixIdx(D, 1), 0.0f);
        a.set(mixIdx(D, 0), 1.0f);
        c.set(mixIdx(D, 0), 2.0f);
        a.on(kNotes[d]);
        c.on(kNotes[d]);
        const Stats sa = run(a, 20), sc = run(c, 20);
        CHECK_NEAR(sc.e() / sa.e(), 4.0, 0.01);
        // Pan hard left / right.
        const Stats l = hit(kNotes[d], d == 0 ? +[](Rig& r) { r.set(mixIdx(perc::kKick, 1), -1.0f); }
                                     : d == 1 ? +[](Rig& r) { r.set(mixIdx(perc::kTom, 1), -1.0f); }
                                     : d == 2 ? +[](Rig& r) { r.set(mixIdx(perc::kSnare, 1), -1.0f); }
                                     : d == 3 ? +[](Rig& r) { r.set(mixIdx(perc::kHat, 1), -1.0f); }
                                     : d == 4 ? +[](Rig& r) { r.set(mixIdx(perc::kCymbal, 1), -1.0f); }
                                              : +[](Rig& r) { r.set(mixIdx(perc::kPerc, 1), -1.0f); });
        CHECK(l.eL > 1.0);
        CHECK(l.eR == 0.0);
    }
    Rig r;
    r.set(mixIdx(perc::kSnare, 1), 1.0f);
    r.on(62);
    const Stats s = run(r, 20);
    CHECK(s.eL == 0.0);
    CHECK(s.eR > 1.0);
}

TEST(perc_sends_tap) {
    float d[kBlock], v[kBlock];
    for (int dr = 0; dr < perc::kDrums; ++dr) {
        const perc::Drum D = perc::Drum(dr);
        Rig r;
        r.set(mixIdx(D, 2), 0.0f);
        r.set(mixIdx(D, 3), 0.0f);
        perc::setSendTap(0, d, v);
        double ed = 0, ev = 0;
        r.on(kNotes[dr]);
        for (int b = 0; b < 10; ++b) {
            for (uint32_t i = 0; i < kBlock; ++i) d[i] = v[i] = 0.0f;
            r.block();
            for (uint32_t i = 0; i < kBlock; ++i) ed += double(d[i]) * d[i] + double(v[i]) * v[i];
        }
        CHECK(ed == 0.0);   // sends at 0: nothing on the tap
        Rig q;
        q.set(mixIdx(D, 2), 1.0f);
        q.set(mixIdx(D, 3), 0.5f);
        q.on(kNotes[dr]);
        ed = 0;
        for (int b = 0; b < 10; ++b) {
            for (uint32_t i = 0; i < kBlock; ++i) d[i] = v[i] = 0.0f;
            q.block();
            for (uint32_t i = 0; i < kBlock; ++i) {
                ed += double(d[i]) * d[i];
                ev += double(v[i]) * v[i];
            }
        }
        CHECK(ed > 0.1);
        CHECK_NEAR(ev / ed, 0.25, 0.01);   // reverb send 0.5 -> a quarter of the energy
        perc::setSendTap(0, nullptr, nullptr);
    }
}

TEST(perc_decay) {
    struct D {
        int note, param;
        float shortT, longT;
    };
    const D cases[] = {
        {60, perc::kKickDecay, 0.1f, 1.0f},       {48, perc::kTomDecay, 0.1f, 1.0f},
        {62, perc::kSnareDecay, 0.05f, 1.0f},     {62, perc::kSnareSnapDecay, 0.05f, 1.0f},
        {64, perc::kHatClosed, 0.05f, 1.0f},      {67, perc::kHatOpen, 0.1f, 2.0f},
        {69, perc::kCymBodyDecay, 0.2f, 3.0f},    {69, perc::kCymImpactDecay, 0.05f, 2.0f},
        {71, perc::kPercDecay, 0.1f, 1.5f},
    };
    for (const D& c : cases) {
        Rig a, b;
        a.set(c.param, c.shortT);
        b.set(c.param, c.longT);
        if (c.param == perc::kCymBodyDecay) {
            a.set(perc::kCymImpactDecay, 0.05f);
            b.set(perc::kCymImpactDecay, 0.05f);
        }
        if (c.param == perc::kCymImpactDecay) {
            a.set(perc::kCymBodyDecay, 0.05f);
            b.set(perc::kCymBodyDecay, 0.05f);
            a.set(perc::kCymImpactBody, -1.0f);
            b.set(perc::kCymImpactBody, -1.0f);
        }
        if (c.param == perc::kSnareSnapDecay) {
            a.set(perc::kSnareMix, 1.0f);
            b.set(perc::kSnareMix, 1.0f);
        }
        a.on(c.note);
        b.on(c.note);
        // Tail energy 0.15 .. 0.3 s after the hit.
        const Stats ta = run(a, 100, 50), tb = run(b, 100, 50);
        CHECK(tb.e() > 10.0 * ta.e() + 1e-3);
    }
    // Hi-hat notes 64..67 decay from Closed to Open.
    double prev = -1;
    for (int n = 64; n <= 67; ++n) {
        const double e = hit(n, nullptr, 60, 20).e();
        CHECK(e > prev);
        prev = e;
    }
}

TEST(perc_pitch_and_tone) {
    // Kick / Snare Pitch and the Tom note move the zero-crossing rate.
    {
        const Stats lo = hit(60, [](Rig& r) { r.set(perc::kKickPitch, 28.0f); r.set(perc::kKickTrLevel, 0.0f); }, 30, 2);
        const Stats hi = hit(60, [](Rig& r) { r.set(perc::kKickPitch, 52.5f); r.set(perc::kKickTrLevel, 0.0f); }, 30, 2);
        CHECK(hi.zc > 3 * lo.zc);
        // Pitches are fractional: half a semitone changes the sound.
        Rig a, b;
        for (Rig* r : {&a, &b}) r->set(perc::kKickTrLevel, 0.0f);
        a.set(perc::kKickPitch, 40.0f);
        b.set(perc::kKickPitch, 40.5f);
        a.on(60);
        b.on(60);
        double d = 0;
        for (int k = 0; k < 20; ++k) {
            a.block();
            b.block();
            for (uint32_t i = 0; i < kBlock; ++i) d += std::fabs(double(a.L[i]) - b.L[i]);
        }
        CHECK(d > 1.0);
    }
    {
        auto body = [](Rig& r) { r.set(perc::kSnareMix, -1.0f); };
        Rig a, b;
        body(a);
        body(b);
        a.set(perc::kSnarePitch, 40.0f);
        b.set(perc::kSnarePitch, 64.0f);
        a.on(62);
        b.on(62);
        CHECK(run(b, 20).zc > 3 * run(a, 20).zc);
        // Tone moves the balance to the higher body oscillator.
        Rig c, d;
        body(c);
        body(d);
        c.set(perc::kSnareTone, 0.0f);
        d.set(perc::kSnareTone, 1.0f);
        for (Rig* r : {&c, &d}) {
            r->set(perc::kSnareDecay, 2.0f);
            r->on(62);
        }
        CHECK(run(d, 60).zc > run(c, 60).zc + 15);
    }
    {
        auto quiet = [](Rig& r) { r.set(perc::kTomModel, 0.0f); r.set(perc::kTomPitchMod, 0.0f); };
        const Stats lo = hit(36, quiet, 30), hi = hit(60 + 24, quiet, 30);
        CHECK(hi.zc > 3 * lo.zc);
    }
    // Hi-hat / cymbal Tone Pitch (tone only, no filters in the way).
    for (int which = 0; which < 2; ++which) {
        Rig a, b;
        for (Rig* r : {&a, &b}) {
            if (which == 0) {
                r->set(perc::kHatMix, -1.0f);
                r->set(perc::kHatHpf, 10.0f);
                r->set(perc::kHatLpf, 20000.0f);
                r->set(perc::kHatTimbre, 0.0f);
                r->set(perc::kHatOpen, 2.0f);
            } else {
                r->set(perc::kCymMix, -1.0f);
                r->set(perc::kCymImpactBody, -1.0f);
                r->set(perc::kCymImpactF, 500.0f);
                r->set(perc::kCymTimbre, 0.0f);
            }
        }
        a.set(which ? perc::kCymPitch : perc::kHatPitch, 110.0f);
        b.set(which ? perc::kCymPitch : perc::kHatPitch, 800.0f);
        a.on(which ? 69 : 67);
        b.on(which ? 69 : 67);
        CHECK(run(b, 10).zc > 2 * run(a, 10).zc);
    }
    // Timbres / models / Body Wave change the sound.
    auto diff = [](int note, int param, float x, float y) {
        Rig a, b;
        a.set(param, x);
        b.set(param, y);
        a.on(note);
        b.on(note);
        double e = 0, ea = 0;
        for (int k = 0; k < 20; ++k) {
            a.block();
            b.block();
            for (uint32_t i = 0; i < kBlock; ++i) {
                const double dd = double(a.L[i]) - b.L[i];
                e += dd * dd;
                ea += double(a.L[i]) * a.L[i];
            }
        }
        return ea > 0 ? e / ea : 0.0;
    };
    CHECK(diff(60, perc::kKickWave, 0.0f, 98.0f) > 0.05);
    CHECK(diff(60, perc::kKickModel, 0.0f, 2.0f) > 0.05);
    CHECK(diff(48, perc::kTomModel, 0.0f, 2.0f) > 0.05);
    CHECK(diff(62, perc::kSnareModel, 0.0f, 3.0f) > 0.05);
    CHECK(diff(62, perc::kSnareSnapTimbre, 0.0f, 1.0f) > 0.05);
    CHECK(diff(64, perc::kHatTimbre, 0.0f, 12.0f) > 0.05);
    CHECK(diff(64, perc::kHatModel, 0.0f, 1.0f) > 0.05);
    CHECK(diff(69, perc::kCymTimbre, 3.0f, 20.0f) > 0.05);
    CHECK(diff(69, perc::kCymModel, 0.0f, 1.0f) > 0.05);
    CHECK(diff(71, perc::kPercTimbre, 0.0f, 1.0f) > 0.05);
    for (int m = 1; m < 5; ++m) CHECK(diff(71, perc::kPercModel, 0.0f, float(m)) > 0.05);
    // Every Body Wave family sounds and is finite.
    for (int w = 0; w < 176; w += 5) {
        const Stats s = hit(60, nullptr, 1);
        Rig r;
        r.set(perc::kKickWave, float(w));
        r.on(60);
        const Stats t = run(r, 10);
        CHECK(t.finite);
        CHECK(t.e() > 0.1 * s.e());
    }
}

TEST(perc_overlap_in_one_voice) {
    // Kick hard left, hi-hat hard right: the hat hits (same voice, reused)
    // leave the kick on L bit-identical to the kick alone.
    auto setup = [](Rig& r) {
        r.set(mixIdx(perc::kKick, 1), -1.0f);
        r.set(mixIdx(perc::kHat, 1), 1.0f);
        r.set(perc::kKickDecay, 2.0f);
    };
    Rig a, b;
    setup(a);
    setup(b);
    a.on(60);
    b.on(60);
    bool same = true, hatOn = false;
    for (int k = 0; k < 60; ++k) {
        if (k == 3 || k == 20) b.on(64);
        if (k == 30) b.on(66);
        a.block();
        b.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            if (a.L[i] != b.L[i]) same = false;
            if (b.R[i] != 0.0f) hatOn = true;
        }
    }
    CHECK(same);
    CHECK(hatOn);
    CHECK(a.alive && b.alive);
    // All six drums at once.
    Rig r;
    for (int n : kNotes) r.on(n);
    const Stats s = run(r, 30);
    CHECK(s.finite);
    CHECK(s.e() > 1.0);
    CHECK(r.v.level > 0.0f);
    // Closed hat chokes the open hat (one channel).
    Rig o, c;
    o.on(67);
    c.on(67);
    run(o, 5);
    run(c, 5);
    c.on(64);
    CHECK(run(c, 40, 20).e() < 0.2 * run(o, 40, 20).e());
}

TEST(perc_velocity_per_hit) {
    // A soft hit plays at its velocity, and a later soft hit of another drum
    // does not duck a drum that is still ringing (the framework passes the
    // last note's velocity as the voice gain: the engine divides it out).
    Rig a, b;
    for (Rig* r : {&a, &b}) {
        r->set(mixIdx(perc::kKick, 1), -1.0f);
        r->set(mixIdx(perc::kSnare, 1), 1.0f);
    }
    a.on(60, 1.0f);
    b.on(60, 0.5f);
    double ea = 0, eb = 0;
    for (int k = 0; k < 5; ++k) {
        a.block(1.0f);
        b.block(0.5f);
        for (uint32_t i = 0; i < kBlock; ++i) {
            ea += double(a.L[i]) * a.L[i];
            eb += double(b.L[i]) * b.L[i];
        }
    }
    CHECK_NEAR(eb / ea, 0.25, 1e-3);
    Rig c, d;
    for (Rig* r : {&c, &d}) {
        r->set(mixIdx(perc::kKick, 1), -1.0f);
        r->set(mixIdx(perc::kSnare, 1), 1.0f);
        r->on(60, 1.0f);
        r->block(1.0f);
    }
    d.on(62, 0.2f);
    c.block(1.0f);
    d.block(1.0f, (0.2f - 1.0f) / float(kBlock));   // framework ramps to the new velocity
    bool same = true;
    for (int k = 0; k < 10; ++k) {
        for (uint32_t i = 0; i < kBlock; ++i)
            if (c.L[i] != d.L[i]) same = false;
        c.block(1.0f);
        d.block(0.2f);
    }
    CHECK(same);
}

TEST(perc_fade_of_a_stolen_voice) {
    Rig r;
    r.set(perc::kKickDecay, 3.0f);
    r.on(60);
    r.block();
    CHECK(r.alive);
    CHECK(!r.block(1.0f, -1.0f / float(kBlock), true));   // fade to 0 over one block
    CHECK(std::fabs(r.L[kBlock - 1]) < 1e-6f);
    CHECK(std::fabs(r.L[10]) > 0.0f);
    // The kit is free: a new voice starts clean.
    Rig q;
    q.on(61);
    CHECK(!q.block());
}

// An empty-step Volume FX (the framework gain moves, no new hit) scales the
// ringing kit; it is not a steal, however low (VERIFY 2.0.0 fix A item 2).
TEST(perc_volume_fx_scales_ringing_kit) {
    for (float vol : {0.3f, 0.0f, 0.49f}) {
        Rig a, b;
        for (Rig* r : {&a, &b}) {
            r->set(perc::kKickDecay, 3.0f);
            r->on(60, 1.0f);
            r->block(1.0f);
        }
        a.block(1.0f);
        b.block(1.0f, (vol - 1.0f) / float(kBlock));   // Volume FX: gain 1 -> vol
        double ea = 0, eb = 0;
        bool alive = true;
        for (int k = 0; k < 8; ++k) {
            a.block(1.0f);
            alive = b.block(vol) && alive;
            for (uint32_t i = 0; i < kBlock; ++i) {
                ea += double(a.L[i]) * a.L[i];
                eb += double(b.L[i]) * b.L[i];
            }
        }
        CHECK(alive);   // still ringing, not freed
        CHECK_NEAR(eb / ea, double(vol) * vol, 1e-3);
        // Back to full volume: the kit comes back up (no new hit).
        b.block(vol, (1.0f - vol) / float(kBlock));
        a.block(1.0f);
        b.block(1.0f);
        a.block(1.0f);
        CHECK_NEAR(b.L[kBlock - 1], a.L[kBlock - 1], 1e-6);
    }
    // A new hit keeps the scaled level of the drums it did not retrigger.
    Rig c;
    c.set(perc::kKickDecay, 3.0f);
    c.set(mixIdx(perc::kKick, 1), -1.0f);
    c.set(mixIdx(perc::kSnare, 1), 1.0f);
    c.on(60, 1.0f);
    c.block(1.0f);
    c.block(1.0f, -0.5f / float(kBlock));   // Volume FX 50
    const float kickBefore = std::fabs(c.L[kBlock - 1]);
    c.on(62, 1.0f);   // snare at full velocity, right channel
    c.block(0.5f, 0.5f / float(kBlock));
    CHECK(kickBefore > 0.0f);
    double l = 0, r = 0;
    for (uint32_t i = 0; i < kBlock; ++i) {
        l += double(c.L[i]) * c.L[i];
        r += double(c.R[i]) * c.R[i];
    }
    CHECK(r > l);   // the fresh snare plays at its own velocity
}

// A steal fades the kit out whatever the last hit's velocity was (a
// Volume-0 hit used to skip the fade: hard cut, kit left owned by a freed
// record) (VERIFY 2.0.0 fix A item 4).
TEST(perc_steal_after_volume0_hit_fades_and_frees_kit) {
    Rig r;
    r.set(perc::kKickDecay, 3.0f);
    r.on(60, 1.0f);
    for (int k = 0; k < 3; ++k) r.block(1.0f);
    r.on(62, 0.0f);   // silent snare hit: vLast = 0
    r.block(1.0f, -1.0f / float(kBlock));
    CHECK(std::fabs(r.L[kBlock - 1]) > 1e-4f);   // the kick still rings
    const float before = r.L[kBlock - 1];
    CHECK(!r.block(0.0f, 0.0f, true));   // stolen: fades, ends
    CHECK(std::fabs(r.L[kBlock - 1]) < 1e-6f);
    // No click at the block start: the first sample continues the kick.
    CHECK(std::fabs(r.L[0] - before) < 0.05f);
    float maxStep = 0.0f;
    for (uint32_t i = 1; i < kBlock; ++i) maxStep = std::max(maxStep, std::fabs(r.L[i] - r.L[i - 1]));
    CHECK(maxStep < 0.05f);
    // The kit has no owner left: another record of the slot starts clean.
    SynthVoice v2;
    v2.slotScratch = &r.sc;
    v2.slot = 0;
    std::memset(v2.state, 0, sizeof(v2.state));
    SynthNote n;
    n.note = 61;   // plays nothing
    n.pitch = 61.0f;
    E()->noteOn(v2, r.p, n);
    float L2[kBlock] = {}, R2[kBlock] = {};
    CHECK(!E()->render(v2, r.p, 1.0f, 0.0f, L2, R2));
}

// Through the framework: an empty-step Volume FX below 50 keeps the kit.
TEST(perc_volume_fx_through_the_framework) {
    static SynthPatch patch[kSynthSlots];
    static Governor gov;
    static SynthBank bank;
    gov.setBudget(4);
    bank.setGovernor(&gov);
    for (int s = 0; s < kSynthSlots; ++s) {
        patchInit(kSynthEnginePerc, &patch[s]);
        synthParamSet(patch[s], perc::kKickDecay, 3.0f);
        bank.bindPatch(s, &patch[s]);
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(SynthModel::PERC);
        r.polyphony = 1;
        r.gainDb = 0.0f;
        bank.setSlotRecord(s, r);
    }
    static float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    auto block = [&]() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        SynthMixInputs in;
        bank.render(in, L, R, D, V, nullptr);
        double e = 0;
        for (uint32_t i = 0; i < kBlock; ++i) e += double(L[i]) * L[i] + double(R[i]) * R[i];
        return e;
    };
    block();
    SynthNoteOn n;
    n.slot = 1;   // takes record 0, so slot 0's kit lives on record 1
    n.note = 61;
    bank.noteOn(2, n);
    n.slot = 0;
    n.note = 60;
    bank.noteOn(0, n);
    for (int k = 0; k < 4; ++k) block();
    bank.applyFx(0, 18, 20);   // Volume 20 on an empty step
    double e = 0;
    for (int k = 0; k < 10; ++k) e += block();
    CHECK_EQ(bank.voicesOf(0), 1);
    CHECK(e > 1e-3);
    // Stop (stopAll frees every record at once): the kit and its owner go
    // with them, a later silent note does not bring the old kick back.
    bank.stopAll();
    n.note = 61;
    bank.noteOn(1, n);
    CHECK(block() == 0.0);
    CHECK_EQ(bank.voicesOf(0), 0);
}

TEST(perc_waveshaper_eq_output_routing) {
    auto energyDiff = [](void (*f)(Rig&)) {
        Rig a, b;
        f(b);
        for (Rig* r : {&a, &b}) r->on(62);
        double e = 0;
        for (int k = 0; k < 20; ++k) {
            a.block();
            b.block();
            for (uint32_t i = 0; i < kBlock; ++i) e += std::fabs(double(a.L[i]) - b.L[i]);
        }
        return e;
    };
    for (int t = 0; t < 5; ++t) {
        static int type;
        type = t;
        CHECK(energyDiff([](Rig& r) { r.set(perc::kWsLevel, 1.0f); r.set(perc::kWsType, float(type)); }) > 1.0);
    }
    CHECK(energyDiff([](Rig& r) { r.set(perc::kEqLow, 12.0f); }) > 1.0);
    CHECK(energyDiff([](Rig& r) { r.set(perc::kEqMid, -12.0f); }) > 1.0);
    CHECK(energyDiff([](Rig& r) { r.set(perc::kEqHigh, 12.0f); }) > 1.0);
    // A "Post EQ" drum bypasses both FX.
    CHECK(energyDiff([](Rig& r) {
              r.set(mixIdx(perc::kSnare, 4), 2.0f);
              r.set(perc::kWsLevel, 1.0f);
              r.set(perc::kEqHigh, 12.0f);
          }) == 0.0);
    // "Post Overdrive" skips the waveshaper only.
    CHECK(energyDiff([](Rig& r) {
              r.set(mixIdx(perc::kSnare, 4), 1.0f);
              r.set(perc::kWsLevel, 1.0f);
          }) == 0.0);
    CHECK(energyDiff([](Rig& r) {
              r.set(mixIdx(perc::kSnare, 4), 1.0f);
              r.set(perc::kEqHigh, 12.0f);
          }) > 1.0);
}

TEST(perc_cost_governor) {
    // The Init kit hit all at once without sends, kick click or tom noise:
    // five drums ring (the manual's limit), the newest hit always plays, the
    // estimate stays in the budget. The Init kit as it is (delay sends: the
    // send tap ~1.4k; the kick click and tom noise ~1.5k each while they
    // last) keeps four.
    for (int sends = 0; sends < 2; ++sends) {
        Rig r;
        if (!sends) {
            for (int d = 0; d < perc::kDrums; ++d) r.set(mixIdx(perc::Drum(d), 2), 0.0f);
            r.set(perc::kKickTrLevel, 0.0f);
            r.set(perc::kTomModel, 0.0f);
        }
        for (int n : {60, 48, 62, 66, 69, 71}) {
            r.on(n);
            int ringing = 0;
            CHECK(perc::kitCost(r.sc, r.p, &ringing) <= perc::kKitBudget - 1000);
            CHECK(ringing <= 5);
        }
        int ringing = 0;
        perc::kitCost(r.sc, r.p, &ringing);
        CHECK_EQ(ringing, sends ? 4 : 5);
    }
    // Costly models + both FX: a new hit cuts the quietest ringing drum.
    Rig c;
    c.set(perc::kWsLevel, 0.5f);
    c.set(perc::kEqLow, 3.0f);
    c.set(perc::kEqMid, -3.0f);
    c.set(perc::kEqHigh, 3.0f);
    c.set(perc::kSnareModel, 4.0f);
    c.set(perc::kTomModel, 2.0f);
    c.set(perc::kCymBodyDecay, 8.0f);
    c.set(perc::kCymImpactDecay, 8.0f);
    c.set(mixIdx(perc::kCymbal, 1), 1.0f);   // cymbal right, the rest left
    c.set(mixIdx(perc::kSnare, 1), -1.0f);
    c.set(mixIdx(perc::kTom, 1), -1.0f);
    c.on(69);
    CHECK(run(c, 40).eR > 0.1);
    c.on(62);
    c.on(48);
    CHECK(perc::kitCost(c.sc, c.p) <= perc::kKitBudget);
    int rc = 0;
    perc::kitCost(c.sc, c.p, &rc);
    CHECK_EQ(rc, 2);
    const Stats s = run(c, 10);
    CHECK(s.eR < 1e-4 * s.eL);   // the decayed cymbal was cut (EQ state tails only)
    CHECK(s.eL > 1.0);    // snare and tom play
}

TEST(perc_fuzz_no_nan) {
    uint32_t s = 12345;
    auto rnd = [&s]() {
        s = s * 1664525u + 1013904223u;
        return float(s >> 8) / 16777216.0f;
    };
    for (int it = 0; it < 60; ++it) {
        Rig r;
        for (int i = 0; i < perc::kParamCount; ++i) {
            const PatchParamRange* pr = synthParamRange(kSynthEnginePerc, i);
            float x = pr->min + rnd() * (pr->max - pr->min);
            const float u = rnd();
            if (u < 0.15f) x = pr->min;
            else if (u < 0.3f) x = pr->max;
            r.set(i, x);
        }
        bool ok = true;
        double peak = 0;
        for (int k = 0; k < 120; ++k) {
            if (k % 7 == 0) r.on(int(rnd() * 128.0f), rnd());
            r.block();
            for (uint32_t i = 0; i < kBlock; ++i) {
                if (!std::isfinite(r.L[i]) || !std::isfinite(r.R[i])) ok = false;
                if (std::fabs(r.L[i]) > peak) peak = std::fabs(r.L[i]);
            }
        }
        CHECK(ok);
        CHECK(peak < 64.0);
    }
}

TEST(perc_through_the_framework) {
    static SynthPatch patch[kSynthSlots];
    static Governor gov;
    static SynthBank bank;
    gov.setBudget(4);
    bank.setGovernor(&gov);
    for (int s = 0; s < kSynthSlots; ++s) {
        patchInit(kSynthEnginePerc, &patch[s]);
        bank.bindPatch(s, &patch[s]);
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(SynthModel::PERC);
        r.polyphony = s == 0 ? 1u : 2u;
        r.gainDb = 0.0f;
        bank.setSlotRecord(s, r);
    }
    static float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
    auto block = [&]() {
        for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
        SynthMixInputs in;
        bank.render(in, L, R, D, V, nullptr);
        double e = 0;
        for (uint32_t i = 0; i < kBlock; ++i) e += double(L[i]) * L[i] + double(R[i]) * R[i];
        return e;
    };
    block();
    CHECK(bank.slotView(0).available);
    for (int slot = 0; slot < 2; ++slot) {
        SynthNoteOn n;
        n.slot = uint8_t(slot);
        n.note = 60;
        bank.noteOn(0, n);
        block();
        n.note = 64;
        bank.noteOn(1, n);   // another track, same slot: one kit
        n.note = 62;
        bank.noteOn(2, n);
        double e = 0;
        for (int k = 0; k < 10; ++k) e += block();
        CHECK(e > 1.0);
        CHECK_EQ(bank.voicesOf(slot), 1);
        int k = 0;
        while (bank.voicesOf(slot) > 0 && k < 4000) {
            block();
            ++k;
        }
        CHECK(k < 4000);   // the voice ends once the kit is silent
    }
}

TEST(perc_factory_patches_one_bar) {
    const char* root = std::getenv("OT_VENDOR_FIXTURES");
    const std::string dir = std::string(root ? root : "") + "/mini-patches-2.0/PERC";
    DIR* dp = root ? opendir(dir.c_str()) : nullptr;
    if (!dp) {
        std::printf("  SKIP: no vendor PERC patches (%s)\n", dir.c_str());
        return;
    }
    std::vector<std::string> files;
    while (dirent* e = readdir(dp)) {
        const std::string n = e->d_name;
        if (n.size() > 6 && n.compare(n.size() - 6, 6, ".patch") == 0) files.push_back(n);
    }
    closedir(dp);
    CHECK(files.size() >= 40);
    // 1 bar of 16 steps at 120 BPM (43 blocks per step).
    struct Hit {
        int step, note;
        float vel;
    };
    const Hit pat[] = {{0, 60, 1}, {0, 69, 0.8f}, {0, 64, 0.7f}, {2, 64, 0.5f}, {4, 60, 1}, {4, 62, 1},
                       {4, 64, 0.7f}, {6, 48, 0.9f}, {6, 64, 0.5f}, {7, 52, 0.9f}, {8, 60, 1}, {8, 64, 0.7f},
                       {10, 71, 0.8f}, {10, 64, 0.5f}, {12, 60, 1}, {12, 62, 1}, {12, 65, 0.6f}, {14, 67, 0.8f},
                       {15, 36, 1}};
    int played = 0;
    for (const std::string& f : files) {
        FILE* fp = std::fopen((dir + "/" + f).c_str(), "rb");
        if (!fp) continue;
        static uint8_t buf[kPatchMaxFileSize];
        const size_t len = std::fread(buf, 1, sizeof(buf), fp);
        std::fclose(fp);
        Rig r;
        if (decodePatch(buf, len, &r.patch) != PatchStatus::kOk || r.patch.engine != kSynthEnginePerc) {
            CHECK(false);
            continue;
        }
        r.sync();
        bool ok = true;
        double peak = 0, e = 0, dc = 0;
        int blocks = 0;
        for (int step = 0; step < 16; ++step) {
            for (const Hit& h : pat)
                if (h.step == step) r.on(h.note, h.vel);
            for (int k = 0; k < 43; ++k) {
                r.block(1.0f);
                ++blocks;
                for (uint32_t i = 0; i < kBlock; ++i) {
                    if (!std::isfinite(r.L[i]) || !std::isfinite(r.R[i])) ok = false;
                    const double a = std::fabs(r.L[i]) > std::fabs(r.R[i]) ? std::fabs(r.L[i]) : std::fabs(r.R[i]);
                    if (a > peak) peak = a;
                    e += double(r.L[i]) * r.L[i] + double(r.R[i]) * r.R[i];
                    dc += r.L[i] + r.R[i];
                }
            }
        }
        // The tail dies out (the longest factory decay is 4.7 s).
        int tail = 0;
        while (r.alive && tail < 4000) {
            r.block(1.0f);
            for (uint32_t i = 0; i < kBlock; ++i)
                if (!std::isfinite(r.L[i])) ok = false;
            ++tail;
        }
        const double rms = std::sqrt(e / (2.0 * blocks * kBlock));
        if (std::getenv("PERC_VERBOSE") || !ok || peak > 8.0 || rms < 1e-3 || r.alive ||
            std::fabs(dc / (2.0 * blocks * kBlock)) > 0.05)
            std::printf("  patch %s: ok=%d peak=%.2f rms=%.4f tail=%d dc=%.4f\n", f.c_str(), ok, peak, rms, tail,
                        dc / (2.0 * blocks * kBlock));
        CHECK(ok);
        CHECK(peak < 8.0);
        CHECK(rms > 1e-3);
        CHECK(!r.alive);
        CHECK(std::fabs(dc / (2.0 * blocks * kBlock)) < 0.05);
        ++played;
    }
    std::printf("  %d factory PERC patches played\n", played);
}

// A PERC kit must not depend on what was in memory before its slot scratch:
// the noise seed was read from never-initialised scratch, so on the host the
// snare / hats differed with the heap's leftovers (found by the render-hash
// golden; on the device the bank is in zeroed static memory).
TEST(perc_kit_independent_of_prior_memory) {
    auto render = [](uint8_t fill) {
        alignas(64) static unsigned char mem[sizeof(SynthBank)];
        std::memset(mem, fill, sizeof mem);
        SynthBank* bank = new (mem) SynthBank();
        static SynthPatch patch;
        static Governor gov;
        gov.setBudget(4);
        bank->setGovernor(&gov);
        patchInit(kSynthEnginePerc, &patch);
        bank->bindPatch(0, &patch);
        SynthSlotRecord r = SynthSlotRecord::defaults(0);
        r.model = uint32_t(SynthModel::PERC);
        r.polyphony = 1;
        bank->setSlotRecord(0, r);
        SynthNoteOn n;
        n.slot = 0;
        n.note = 62;   // snare: noise
        bank->noteOn(0, n);
        std::vector<float> out;
        static float L[kBlock], R[kBlock], D[kBlock], V[kBlock];
        for (int k = 0; k < 20; ++k) {
            for (uint32_t i = 0; i < kBlock; ++i) L[i] = R[i] = D[i] = V[i] = 0.0f;
            SynthMixInputs in;
            bank->render(in, L, R, D, V, nullptr);
            out.insert(out.end(), L, L + kBlock);
        }
        bank->~SynthBank();
        return out;
    };
    const std::vector<float> a = render(0x00), b = render(0xAB);
    CHECK(a == b);
}
