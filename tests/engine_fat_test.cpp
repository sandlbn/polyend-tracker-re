// FAT engine (2.1, audio/engines/fat.*, spec 21 §8.3, docs/specs/2x-07):
// the registry serves it, the Init patch plays its note, the framework never
// sees Volume applied, the oscillator stack (Fatness spread and its pair
// thresholds, Timbre, Brightness, Noise, Reset), the filter types, cutoff,
// resonance and their envelope / LFO amounts, vibrato, LFO shapes and
// retrigger modes, envelopes and voice end, glide / legato, bend, every
// parameter audible, macro-pushed values, polyphonic chords, no NaN / Inf
// over random parameter sets, and every factory FAT patch
// ($OT_VENDOR_FIXTURES/mini-patches-2.0/FAT, skipped when absent) renders
// cleanly. Cycle counts: tests/audio_bench.cpp
// (fatinit / fatworst / mixfat, make perf-gate).
#include "test.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "audio/engines/fat.h"
#include "audio/synth_engine.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"

using namespace ot;
using namespace ot::audio;
namespace fs = std::filesystem;

namespace {

constexpr int kN = fat::kParamCount;
constexpr int kBlocksPerSec = int(kSampleRate / float(kBlock)) + 1;   // 345

const PatchParamRange* range(int i) { return synthParamRange(kSynthEngineFat, i); }

// Default (Init) values.
std::vector<float> initValues() {
    std::vector<float> v(kN);
    for (int i = 0; i < kN; ++i) v[i] = range(i)->def;
    return v;
}

// One engine voice pool driven directly (no framework).
struct Rig {
    ISynthEngine* e = synthEngineFor(kSynthEngineFat);
    SynthSlotScratch scratch{};
    SynthParams p;
    SynthVoice v[kSynthMaxVoices];
    bool alive[kSynthMaxVoices] = {};
    std::vector<float> L, R;

    explicit Rig(const std::vector<float>& vals) {
        p.engine = kSynthEngineFat;
        p.count = kN;
        set(vals);
        e->slotReset(scratch);
        for (auto& x : v) {
            x = SynthVoice{};
            x.slotScratch = &scratch;
        }
    }
    void set(const std::vector<float>& vals) {
        for (int i = 0; i < kN; ++i) p.start[i] = p.end[i] = vals[i];
        ++p.serial;
    }
    void on(int k, float pitch, bool reused = false, bool overlap = false) {
        if (!reused) {
            std::memset(v[k].state, 0, sizeof(v[k].state));
            v[k].level = 0.0f;
        }
        v[k].gate = true;
        SynthNote n;
        n.pitch = pitch;
        n.note = uint8_t(std::clamp(int(pitch), 0, 127));
        n.reused = reused;
        n.overlap = overlap;
        e->noteOn(v[k], p, n);
        alive[k] = true;
    }
    void off(int k) {
        v[k].gate = false;
        e->noteOff(v[k], p);
    }
    // Render n blocks; appends to L / R. Returns the number of live voices.
    int run(int blocks, float gain = 1.0f) {
        int live = 0;
        for (int b = 0; b < blocks; ++b) {
            float l[kBlock] = {}, r[kBlock] = {};
            e->prepare(scratch, p);
            live = 0;
            for (int k = 0; k < kSynthMaxVoices; ++k) {
                if (!alive[k]) continue;
                alive[k] = e->render(v[k], p, gain, 0.0f, l, r);
                live += alive[k];
            }
            L.insert(L.end(), l, l + kBlock);
            R.insert(R.end(), r, r + kBlock);
        }
        return live;
    }
};

using Buf = std::vector<float>;

bool finite(const Buf& x) {
    for (float s : x)
        if (!std::isfinite(s)) return false;
    return true;
}
float peak(const Buf& x) {
    float m = 0.0f;
    for (float s : x) m = std::max(m, std::fabs(s));
    return m;
}
double rms(const Buf& x, size_t a = 0, size_t b = SIZE_MAX) {
    b = std::min(b, x.size());
    double s = 0.0;
    for (size_t i = a; i < b; ++i) s += double(x[i]) * x[i];
    return b > a ? std::sqrt(s / double(b - a)) : 0.0;
}
double rmsDiff(const Buf& a, const Buf& b) {
    const size_t n = std::min(a.size(), b.size());
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += double(a[i] - b[i]) * (a[i] - b[i]);
    return std::sqrt(s / double(n));
}
Buf slice(const Buf& b, double t0, double t1) {
    const size_t a = size_t(t0 * kSampleRate), e = std::min(b.size(), size_t(t1 * kSampleRate));
    return Buf(b.begin() + long(std::min(a, e)), b.begin() + long(e));
}
// Fundamental (Hz) in [a, b) by autocorrelation over lags of 60..1500 Hz;
// the first lag within 90 % of the best one (no octave-down picks).
double pitchHz(const Buf& x, size_t a, size_t b) {
    const int lo = int(kSampleRate / 1500.0f), hi = int(kSampleRate / 60.0f);
    std::vector<double> ac(size_t(hi) + 2, 0.0);
    double best = 0.0;
    for (int lag = lo; lag <= hi + 1; ++lag) {
        double s = 0.0;
        for (size_t i = a; i + size_t(lag) < b; ++i) s += double(x[i]) * x[i + size_t(lag)];
        ac[size_t(lag)] = s;
        best = std::max(best, s);
    }
    for (int lag = lo + 1; lag <= hi; ++lag)
        if (ac[size_t(lag)] >= 0.9 * best && ac[size_t(lag)] >= ac[size_t(lag) - 1] && ac[size_t(lag)] >= ac[size_t(lag) + 1]) {
            const double y0 = ac[size_t(lag) - 1], y1 = ac[size_t(lag)], y2 = ac[size_t(lag) + 1];
            const double den = y0 - 2.0 * y1 + y2;
            const double off = den != 0.0 ? 0.5 * (y0 - y2) / den : 0.0;
            return kSampleRate / (double(lag) + off);
        }
    return 0.0;
}

// High-frequency proxy: rms of the first difference over rms.
double bright(const Buf& b) {
    double s = 0;
    for (size_t i = 1; i < b.size(); ++i) s += double(b[i] - b[i - 1]) * (b[i] - b[i - 1]);
    const double r = rms(b);
    return r <= 0 ? 0.0 : std::sqrt(s / double(b.size())) / r;
}
// Goertzel magnitude (normalised, Hann window) at hz.
double tone(const Buf& b, double hz) {
    const double w = 2.0 * 3.14159265358979 * hz / kSampleRate, c = 2.0 * std::cos(w);
    const double wn = 2.0 * 3.14159265358979 / double(b.size());
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < b.size(); ++i) {
        const double s0 = b[i] * (0.5 - 0.5 * std::cos(wn * double(i))) + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / double(b.size());
}
// Relative change of a signal over `lag` frames (0 = exactly periodic).
double drift(const Buf& b, size_t lag) {
    double s = 0.0;
    for (size_t i = 0; i + lag < b.size(); ++i) s += double(b[i + lag] - b[i]) * (b[i + lag] - b[i]);
    return std::sqrt(s / double(b.size() - lag)) / rms(b);
}
// A period of exactly 200 frames (220.6 Hz).
const float kPitch200 = 69.0f + 12.0f * float(std::log2(kSampleRate / 200.0 / 440.0));

// A plain sustained voice: full sustain, open filter, no modulation.
std::vector<float> held() {
    std::vector<float> c = initValues();
    c[fat::kAmpS] = 1.0f;
    c[fat::kCutoff] = 20000.0f;
    c[fat::kResonance] = 0.0f;
    c[fat::kFilterEnv] = 0.0f;
    c[fat::kOscLfo] = 0.0f;
    return c;
}

Buf play(const std::vector<float>& vals, float pitch = 48.0f, float seconds = 0.5f) {
    Rig r(vals);
    r.on(0, pitch);
    r.run(int(seconds * kSampleRate / float(kBlock)) + 1);
    return r.L;
}

}  // namespace

// --- registry and contract -------------------------------------------------------------

TEST(fat_registered) {
    ISynthEngine* e = synthEngineFor(kSynthEngineFat);
    CHECK(e != nullptr);
    CHECK(e != synthTestEngine());
    CHECK_STR(e->name(), "FAT");
    CHECK_EQ(e->id(), int32_t(kSynthEngineFat));
    CHECK(e == fat::engine());
    CHECK_EQ(synthParamCount(kSynthEngineFat), kN);
}

TEST(fat_init_patch_plays_the_note) {
    Rig r(initValues());   // Init: decay 0.5 s to a 0 sustain
    r.on(0, 57.0f);        // A3 = 220 Hz
    r.run(kBlocksPerSec);
    CHECK(finite(r.L));
    CHECK(peak(r.L) > 0.05f);
    CHECK(peak(r.L) < 2.0f);
    CHECK(r.L == r.R);   // mono voice
    const double hz = pitchHz(r.L, size_t(kSampleRate * 0.05), size_t(kSampleRate * 0.25));
    CHECK(hz > 219.0 && hz < 221.0);
    if (!(hz > 219.0 && hz < 221.0)) std::printf("       pitch %.1f Hz\n", hz);
    // Held: the same pitch across the range.
    for (float pitch : {38.0f, 45.0f, 69.0f}) {
        const Buf b = play(held(), pitch, 0.4f);
        const double want = synthPitchHz(pitch), got = pitchHz(b, size_t(kSampleRate * 0.1), size_t(kSampleRate * 0.35));
        CHECK(std::fabs(got / want - 1.0) < 0.005);
        if (!(std::fabs(got / want - 1.0) < 0.005)) std::printf("       pitch %.1f: %.2f Hz, want %.2f\n", pitch, got, want);
    }
}

TEST(fat_volume_is_never_applied) {
    std::vector<float> a = held(), b = held();
    b[fat::kVolume] = 0.25f;
    CHECK(play(a) == play(b));
    // The gain ramp passed to render() scales the output.
    Rig r1(a), r2(a);
    r1.on(0, 48.0f);
    r2.on(0, 48.0f);
    r1.run(60, 1.0f);
    r2.run(60, 0.5f);
    CHECK(std::fabs(rms(r2.L) / rms(r1.L) - 0.5) < 1e-4);
}

// --- envelopes -------------------------------------------------------------------------

TEST(fat_amp_envelope_stages) {
    std::vector<float> q = held();
    // Attack
    q[fat::kAmpA] = 0.0f;
    const Buf fast = play(q);
    q[fat::kAmpA] = 0.4f;
    const Buf slow = play(q);
    CHECK(rms(slice(slow, 0, 0.05)) < 0.3 * rms(slice(fast, 0, 0.05)));
    CHECK(rms(slice(slow, 0.42, 0.5)) > 0.8 * rms(slice(fast, 0.42, 0.5)));
    // Decay and sustain
    q[fat::kAmpA] = 0.0f;
    q[fat::kAmpS] = 0.0f;
    q[fat::kAmpD] = 0.1f;
    const Buf d1 = play(q);
    q[fat::kAmpD] = 1.0f;
    const Buf d2 = play(q);
    CHECK(rms(slice(d2, 0.2, 0.3)) > 3.0 * rms(slice(d1, 0.2, 0.3)));
    q[fat::kAmpS] = 0.5f;
    const Buf s5 = play(q, 48.0f, 3.0f);
    q[fat::kAmpS] = 1.0f;
    const Buf s10 = play(q, 48.0f, 3.0f);
    CHECK(std::fabs(rms(slice(s5, 2.5, 3.0)) / rms(slice(s10, 2.5, 3.0)) - 0.5) < 0.02);
    // level reports the amp envelope for voice stealing
    Rig s(held());
    s.on(0, 60.0f);
    s.run(10);
    CHECK(s.v[0].level > 0.9f);
}

TEST(fat_voice_finishes_after_release) {
    for (float rel : {0.0f, 0.05f, 0.5f, 2.0f}) {
        std::vector<float> c = held();
        c[fat::kAmpR] = rel;
        Rig r(c);
        r.on(0, 60.0f);
        r.run(30);
        r.off(0);
        const size_t at = r.L.size();
        int blocks = 0;
        while (r.run(1) && blocks < 8 * kBlocksPerSec) ++blocks;
        CHECK(blocks < int((2.5f * rel + 0.1f) * kBlocksPerSec));   // -80 dB within 2x the stage time
        CHECK(r.v[0].level < 1e-3f);
        if (rel >= 0.5f) {   // a real release: still audible half way
            const Buf tail(r.L.begin() + long(at), r.L.end());
            CHECK(rms(slice(tail, 0.4 * rel, 0.5 * rel)) > 0.02 * rms(r.L, 0, at));
        }
    }
    // Percussive: sustain 0 ends without a note off (the Init patch).
    Rig q(initValues());
    q.on(0, 60.0f);
    int blocks = 0;
    while (q.run(1) && blocks < 4 * kBlocksPerSec) ++blocks;
    CHECK(blocks < 2 * kBlocksPerSec);
    CHECK(blocks > kBlocksPerSec / 4);
}

// --- pitch -----------------------------------------------------------------------------

TEST(fat_bend_adds_to_pitch) {
    Rig r(held());
    r.on(0, 57.0f);
    r.v[0].bend = r.v[0].bendStart = 12.0f;
    r.run(kBlocksPerSec / 3);
    const double hz = pitchHz(r.L, size_t(kSampleRate * 0.05), size_t(kSampleRate * 0.3));
    CHECK(hz > 437.0 && hz < 443.0);
}

// --- oscillator stack -------------------------------------------------------------------

TEST(fat_fatness_spreads_the_stack) {
    // Fatness 0: the centre saw alone, periodic; the pairs' detune makes the
    // wave drift from period to period, more with more Fatness.
    std::vector<float> c = held();
    const size_t lag = 20 * 200;   // 20 periods
    double last = -1.0;
    for (float F : {0.0f, 0.1f, 0.3f, 0.6f, 1.0f}) {
        c[fat::kFatness] = F;
        const Buf b = slice(play(c, kPitch200, 0.6f), 0.1, 0.6);
        const double d = drift(b, lag);
        if (F == 0.0f) CHECK(d < 0.01);
        else CHECK(d > last + 0.05);
        if (!(F == 0.0f ? d < 0.01 : d > last + 0.05)) std::printf("       Fatness %.1f: drift %.3f (last %.3f)\n", F, d, last);
        last = d;
    }
    // Equal power: the level stays within 2 dB across the range.
    c[fat::kFatness] = 0.0f;
    const double r0 = rms(slice(play(c, 48.0f, 0.6f), 0.1, 0.6));
    for (float F : {0.3f, 0.6f, 1.0f}) {
        c[fat::kFatness] = F;
        const double r = rms(slice(play(c, 48.0f, 0.6f), 0.1, 0.6));
        CHECK(r > 0.79 * r0 && r < 1.26 * r0);
    }
}

TEST(fat_pair_thresholds_fade_in) {
    // Each pair enters at a Fatness third from level 0: a step across the
    // threshold changes the sound by almost nothing, and so does the block
    // in which the kernel gains the pair.
    for (float th : {0.0f, 1.0f / 3.0f, 2.0f / 3.0f}) {
        std::vector<float> a = held(), b = held();
        a[fat::kFatness] = std::max(0.0f, th - 1e-5f);
        b[fat::kFatness] = th + 1e-5f;
        const Buf x = play(a, 48.0f, 0.3f), y = play(b, 48.0f, 0.3f);
        const double d = rmsDiff(x, y) / rms(x);
        CHECK(d < 0.02);
        if (!(d < 0.02)) std::printf("       threshold %.3f: change %.4f\n", th, d);
        // A ramp across the threshold within one block.
        Rig r(a);
        r.on(0, 48.0f);
        r.run(20);
        r.p.end[fat::kFatness] = b[fat::kFatness];
        r.p.moving = true;
        ++r.p.serial;
        r.run(1);
        r.p.start[fat::kFatness] = b[fat::kFatness];
        r.p.moving = false;
        r.run(20);
        CHECK(rmsDiff(r.L, x) / rms(x) < 0.02);
        // And back down (b -> a) within one block: the pair fades out.
        Rig u(b);
        u.on(0, 48.0f);
        u.run(20);
        u.p.end[fat::kFatness] = a[fat::kFatness];
        u.p.moving = true;
        ++u.p.serial;
        u.run(1);
        u.p.start[fat::kFatness] = a[fat::kFatness];
        u.p.moving = false;
        u.run(20);
        const double dd = rmsDiff(u.L, y) / rms(y);
        CHECK(dd < 0.02);
        if (!(dd < 0.02)) std::printf("       threshold %.3f down: change %.4f\n", th, dd);
    }
}

TEST(fat_timbre_morphs_to_a_pulse) {
    std::vector<float> c = held();
    c[fat::kFatness] = 0.0f;
    const double f = synthPitchHz(48.0f);
    c[fat::kTimbre] = 0.0f;
    const Buf saw = slice(play(c, 48.0f, 0.5f), 0.1, 0.5);
    c[fat::kTimbre] = 1.0f;
    const Buf sq = slice(play(c, 48.0f, 0.5f), 0.1, 0.5);
    c[fat::kTimbre] = 0.5f;
    const Buf mid = slice(play(c, 48.0f, 0.5f), 0.1, 0.5);
    CHECK(tone(saw, 2 * f) > 0.4 * tone(saw, f));    // a saw: 1/n
    CHECK(tone(sq, 2 * f) < 0.02 * tone(sq, f));     // a 50 % pulse: odd harmonics only
    CHECK(tone(sq, 3 * f) > 0.25 * tone(sq, f));
    const double r2 = tone(mid, 2 * f) / tone(mid, f);
    CHECK(r2 > 0.1 && r2 < 0.4);                     // half way: even harmonics at 1/3
    // Equal power within 1.5 dB.
    CHECK(rms(sq) > 0.84 * rms(saw) && rms(sq) < 1.19 * rms(saw));
    // The whole stack morphs: no even harmonics at full Fatness either.
    c[fat::kFatness] = 1.0f;
    c[fat::kTimbre] = 1.0f;
    const Buf fat = slice(play(c, 48.0f, 0.5f), 0.1, 0.5);
    CHECK(tone(fat, 2 * f) < 0.05 * tone(fat, f));
}

TEST(fat_brightness_darkens) {
    std::vector<float> c = held();
    double last = 1e9;
    for (float B : {1.0f, 0.5f, 0.0f, -0.5f, -1.0f}) {
        c[fat::kBrightness] = B;
        const double b = bright(slice(play(c, 48.0f, 0.4f), 0.1, 0.4));
        CHECK(b < last * (B == 1.0f ? 1.0 : 0.9));
        last = b;
    }
}

TEST(fat_noise_adds_broadband_energy) {
    std::vector<float> c = held();
    c[fat::kNoise] = 0.0f;
    const Buf dry = play(c, 48.0f, 0.4f);
    c[fat::kNoise] = 1.0f;
    const Buf wet = play(c, 48.0f, 0.4f);
    c[fat::kNoise] = 0.5f;
    const Buf half = play(c, 48.0f, 0.4f);
    Buf n1(dry.size()), n5(dry.size());
    for (size_t i = 0; i < dry.size(); ++i) {
        n1[i] = wet[i] - dry[i];
        n5[i] = half[i] - dry[i];
    }
    CHECK(rms(n1) > 0.1 * rms(dry));
    CHECK(std::fabs(rms(n5) / rms(n1) - 0.5) < 0.05);
    CHECK(bright(n1) > 1.0);   // white: half its energy above fs / 4
}

// The first floats of fat.cpp's VoiceState: the stack phases.
struct PhaseProbe {
    float ph[7];
};

TEST(fat_reset_on_restarts_the_stack) {
    std::vector<float> on = held(), off = held();
    on[fat::kFatness] = off[fat::kFatness] = 1.0f;
    on[fat::kReset] = 1.0f;
    off[fat::kReset] = 0.0f;
    Rig a(on), b(off);
    a.on(0, 60.0f);
    b.on(0, 60.0f);
    // A fresh voice starts at the fixed spread either way: the same render.
    a.run(37);
    b.run(37);
    CHECK(a.L == b.L);
    float before[7];
    for (int k = 0; k < 7; ++k) before[k] = b.v[0].as<PhaseProbe>().ph[k];
    a.on(0, 60.0f, true);
    b.on(0, 60.0f, true);
    for (int k = 0; k < 7; ++k) {
        CHECK(std::fabs(a.v[0].as<PhaseProbe>().ph[k] - (float(2 * k) / 7.0f - 1.0f)) < 1e-6f);   // On: the spread
        CHECK_EQ(b.v[0].as<PhaseProbe>().ph[k], before[k]);                    // Off: free running
    }
    const size_t at = a.L.size();
    a.run(20);
    b.run(20);
    CHECK(rmsDiff(Buf(a.L.begin() + long(at), a.L.end()), Buf(b.L.begin() + long(at), b.L.end())) > 1e-3);
    // Reset On: every retrigger of the voice sounds the same.
    Rig c(on);
    c.on(0, 60.0f);
    c.run(30);
    std::vector<Buf> notes;
    for (int k = 0; k < 3; ++k) {
        c.L.clear();
        c.on(0, 60.0f, true);
        c.run(30);
        notes.push_back(c.L);
    }
    CHECK(rmsDiff(notes[0], notes[1]) < 1e-3 * rms(notes[0]));   // the filter's state carries over
    CHECK(rmsDiff(notes[1], notes[2]) < 1e-3 * rms(notes[0]));
}

// --- filter --------------------------------------------------------------------------------

// The open context with a single saw (pitch tests: one zero crossing per cycle).
std::vector<float> single() {
    std::vector<float> c = held();
    c[fat::kFatness] = 0.0f;
    return c;
}
// Frequency from upward zero crossings.
double freq(const Buf& b) {
    int n = 0;
    size_t first = 0, last = 0;
    for (size_t i = 1; i < b.size(); ++i)
        if (b[i - 1] < 0.0f && b[i] >= 0.0f) {
            if (!n) first = i;
            last = i;
            ++n;
        }
    return n < 2 ? 0.0 : double(n - 1) * kSampleRate / double(last - first);
}

TEST(fat_cutoff_brightness_all_types) {
    for (int t = 0; t < 3; ++t) {
        std::vector<float> c = held();
        c[fat::kFilterType] = float(t);
        c[fat::kResonance] = 0.3f;
        double last = 0;
        for (float fc : {100.0f, 400.0f, 1600.0f, 6400.0f, 20000.0f}) {
            c[fat::kCutoff] = fc;
            const double b = bright(slice(play(c, 36.0f, 0.45f), 0.05, 0.45));
            CHECK(b > last * (fc > 10000.0f ? 1.0 : 1.15));
            last = b;
        }
        c[fat::kCutoff] = 20.0f;
        CHECK(rms(slice(play(c, 60.0f, 0.45f), 0.1, 0.45)) < 0.05 * rms(slice(play(held(), 60.0f, 0.45f), 0.1, 0.45)));
    }
}

TEST(fat_filter_types_differ) {
    std::vector<float> c = single();
    c[fat::kCutoff] = 300.0f;
    c[fat::kResonance] = 0.0f;
    Buf t[3];
    for (int i = 0; i < 3; ++i) {
        c[fat::kFilterType] = float(i);
        t[i] = slice(play(c, 36.0f, 0.45f), 0.1, 0.45);
    }
    const double f = synthPitchHz(36.0f);
    // Far above the cutoff the 24 dB types are far below the 12 dB one.
    const double r12 = tone(t[2], 55 * f) / tone(t[2], f), rmg = tone(t[0], 55 * f) / tone(t[0], f),
                 rob = tone(t[1], 55 * f) / tone(t[1], f);
    CHECK(rmg < 0.25 * r12);
    CHECK(rob < 0.25 * r12);
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j) CHECK(rmsDiff(t[i], t[j]) > 0.05 * rms(t[i]));
    // OB's soft clip: a hot, resonant OB voice stays bounded, MG's is not clipped.
    c[fat::kResonance] = 1.5f;
    c[fat::kCutoff] = 2000.0f;
    c[fat::kFilterType] = float(fat::kLpOb12);
    CHECK(peak(play(c, 36.0f, 0.3f)) <= 2.0f * 0.39f + 1e-3f);   // clip +-2 x kOutTrim
}

TEST(fat_resonance_peak) {
    for (int t = 0; t < 3; ++t) {
        std::vector<float> c = single();
        c[fat::kFilterType] = float(t);
        c[fat::kCutoff] = 1000.0f;
        double last = -1;
        for (float res : {0.0f, 0.5f, 1.0f, 1.5f}) {
            c[fat::kResonance] = res;
            const Buf b = slice(play(c, 36.0f, 0.45f), 0.1, 0.45);   // 65.4 Hz saw, 15th harmonic ~ 981 Hz
            const double rel = tone(b, synthPitchHz(36.0f) * 15) / tone(b, synthPitchHz(36.0f));
            CHECK(rel > last * 1.2);
            CHECK(peak(b) < 4.0f);
            last = rel;
        }
    }
}

// Filter openness at pitch 48 (the 6th harmonic over the fundamental).
double up6(const Buf& b) {
    const double f = synthPitchHz(48.0f);
    return tone(b, 6 * f) / tone(b, f);
}

TEST(fat_filter_env_amount) {
    std::vector<float> q = held();
    q[fat::kCutoff] = 300.0f;
    q[fat::kEnvD] = 0.2f;
    q[fat::kEnvS] = 0.0f;
    q[fat::kFilterEnv] = 0.0f;
    const Buf flat = play(q);
    q[fat::kFilterEnv] = 0.3f;
    const Buf up = play(q);
    q[fat::kFilterEnv] = 0.02f;
    const Buf small = play(q);
    q[fat::kFilterEnv] = -0.3f;
    q[fat::kCutoff] = 3000.0f;
    const Buf down = play(q);
    q[fat::kFilterEnv] = 0.0f;
    const Buf downRef = play(q);
    CHECK(up6(slice(up, 0, 0.05)) > 10.0 * up6(slice(flat, 0, 0.05)));
    CHECK(up6(slice(small, 0, 0.05)) > 2.0 * up6(slice(flat, 0, 0.05)));   // small amounts matter
    CHECK(rmsDiff(slice(up, 0.45, 0.5), slice(flat, 0.45, 0.5)) < 0.05 * rms(slice(flat, 0.45, 0.5)));   // envelope over
    CHECK(up6(slice(down, 0, 0.05)) < 0.5 * up6(slice(downRef, 0, 0.05)));
}

TEST(fat_filter_envelope_stages) {
    std::vector<float> q = held();
    q[fat::kCutoff] = 200.0f;
    q[fat::kFilterEnv] = 0.5f;
    q[fat::kEnvA] = 0.0f;
    q[fat::kEnvD] = 0.1f;
    q[fat::kEnvS] = 0.0f;
    const Buf a0 = play(q);
    q[fat::kEnvA] = 0.3f;
    const Buf a3 = play(q);
    CHECK(up6(slice(a3, 0, 0.03)) < 0.2 * up6(slice(a0, 0, 0.03)));
    q[fat::kEnvA] = 0.0f;
    q[fat::kEnvD] = 1.0f;
    const Buf d1 = play(q);
    CHECK(up6(slice(d1, 0.2, 0.3)) > 5.0 * up6(slice(a0, 0.2, 0.3)));
    q[fat::kEnvD] = 0.1f;
    q[fat::kEnvS] = 0.6f;
    const Buf s6 = play(q);
    CHECK(up6(slice(s6, 0.4, 0.5)) > 5.0 * up6(slice(a0, 0.4, 0.5)));
    // Release: after the note off the brightness falls with the envelope's release.
    for (float er : {0.02f, 10.0f}) {
        q[fat::kEnvS] = 1.0f;
        q[fat::kEnvR] = er;
        q[fat::kAmpR] = 2.0f;
        Rig r(q);
        r.on(0, 48.0f);
        r.run(35);
        const Buf held0 = r.L;
        r.off(0);
        r.L.clear();
        r.run(105);
        if (er < 0.5f) CHECK(up6(slice(r.L, 0.2, 0.3)) < 0.2 * up6(slice(held0, 0.05, 0.1)));
        else CHECK(up6(slice(r.L, 0.2, 0.3)) > 0.8 * up6(slice(held0, 0.05, 0.1)));
    }
}

// --- LFO -----------------------------------------------------------------------------------

TEST(fat_filter_lfo_amount) {
    std::vector<float> q = held();
    q[fat::kCutoff] = 600.0f;
    q[fat::kLfoWave] = float(fat::kSquare);
    q[fat::kLfoHz] = 2.0f;
    q[fat::kLfoRetrig] = float(fat::kNote);
    q[fat::kFilterLfo] = 0.0f;
    const Buf ref = play(q, 36.0f, 0.5f);
    q[fat::kFilterLfo] = 0.2f;
    const Buf m = play(q, 36.0f, 0.5f);
    q[fat::kFilterLfo] = -0.2f;
    const Buf n = play(q, 36.0f, 0.5f);
    // First half cycle (LFO +1) brighter, second (-1) darker; a negative amount the other way.
    CHECK(bright(slice(m, 0.05, 0.24)) > 1.5 * bright(slice(ref, 0.05, 0.24)));
    CHECK(bright(slice(m, 0.3, 0.49)) < 0.8 * bright(slice(ref, 0.3, 0.49)));
    CHECK(bright(slice(n, 0.05, 0.24)) < 0.8 * bright(slice(ref, 0.05, 0.24)));
}

TEST(fat_osc_lfo_vibrato) {
    // Square LFO at 0.5 Hz starts high: + 12 A^2 semitones for its first second.
    std::vector<float> q = single();
    q[fat::kLfoWave] = float(fat::kSquare);
    q[fat::kLfoHz] = 0.5f;
    q[fat::kLfoRetrig] = float(fat::kNote);
    q[fat::kOscLfo] = 1.0f;
    CHECK(std::fabs(freq(slice(play(q, 57.0f, 0.4f), 0.05, 0.35)) - 440.0) < 4.0);
    q[fat::kOscLfo] = 0.5f;   // 3 semitones
    CHECK(std::fabs(freq(slice(play(q, 57.0f, 0.4f), 0.05, 0.35)) - synthPitchHz(60.0f)) < 2.5);
    q[fat::kOscLfo] = 0.0f;
    CHECK(std::fabs(freq(slice(play(q, 57.0f, 0.4f), 0.05, 0.35)) - 220.0) < 2.0);
}

TEST(fat_lfo_frequency_and_shapes) {
    std::vector<float> q = held();
    q[fat::kCutoff] = 500.0f;
    q[fat::kFilterLfo] = 0.3f;
    q[fat::kLfoRetrig] = float(fat::kNote);
    Buf w[6];
    for (int i = 0; i < 6; ++i) {
        q[fat::kLfoWave] = float(i);
        w[i] = play(q, 36.0f, 1.0f);
    }
    for (int i = 0; i < 6; ++i)
        for (int j = i + 1; j < 6; ++j) CHECK(rmsDiff(w[i], w[j]) > 0.05 * rms(w[i]));
    q[fat::kLfoWave] = float(fat::kSine);
    Buf f[3];
    const float hzs[3] = {0.1f, 4.0f, 100.0f};
    for (int i = 0; i < 3; ++i) {
        q[fat::kLfoHz] = hzs[i];
        f[i] = play(q, 36.0f, 1.0f);
    }
    CHECK(rmsDiff(f[0], f[1]) > 0.1 * rms(f[0]));
    CHECK(rmsDiff(f[1], f[2]) > 0.1 * rms(f[1]));
}

TEST(fat_lfo_retrigger_modes) {
    std::vector<float> q = single();
    q[fat::kOscLfo] = 0.3f;
    q[fat::kLfoWave] = float(fat::kTri);
    q[fat::kLfoHz] = 3.3f;
    // Note: every note starts the LFO at phase 0 -> identical pitch tracks.
    q[fat::kLfoRetrig] = float(fat::kNote);
    Rig r(q);
    r.on(0, 48.0f);
    r.run(45);
    const Buf a = r.L;
    r.off(0);
    r.L.clear();
    r.on(0, 48.0f, true, false);
    r.run(45);
    CHECK(std::fabs(freq(slice(a, 0.01, 0.12)) - freq(slice(r.L, 0.01, 0.12))) < 1.0);
    // Free running: the slot's phase keeps going -> the second note differs.
    std::vector<float> fr = q;
    fr[fat::kLfoRetrig] = float(fat::kFree);
    Rig rf(fr);
    rf.on(0, 48.0f);
    rf.run(45);
    const Buf fa = rf.L;
    rf.L.clear();
    rf.on(0, 48.0f);
    rf.run(45);
    CHECK(std::fabs(freq(slice(fa, 0.01, 0.12)) - freq(slice(rf.L, 0.01, 0.12))) > 2.0);
    // One shot: one cycle, then the pitch holds (at the cycle's end, 0).
    std::vector<float> os = q;
    os[fat::kLfoRetrig] = float(fat::kOneShot);
    const Buf o = play(os, 57.0f, 1.0f), n = play(q, 57.0f, 1.0f);
    CHECK(std::fabs(freq(slice(o, 0.5, 0.9)) - 220.0) < 1.5);
    CHECK(rmsDiff(slice(o, 0.5, 0.9), slice(n, 0.5, 0.9)) > 0.1 * rms(slice(n, 0.5, 0.9)));
}

// --- glide ---------------------------------------------------------------------------------

TEST(fat_glide_modes_and_time) {
    auto glideRun = [](int mode, float time, bool overlap, float& mid) {
        std::vector<float> q = single();
        q[fat::kGlideMode] = float(mode);
        q[fat::kGlideTime] = time;
        Rig r(q);
        r.on(0, 45.0f);   // 110 Hz
        r.run(35);
        if (!overlap) r.off(0);
        r.L.clear();
        r.on(0, 57.0f, true, overlap);   // 220 Hz
        r.run(277);
        mid = float(freq(slice(r.L, 0.08, 0.12)));
        return freq(slice(r.L, 0.7, 0.8));
    };
    float mid;
    // No glide time: straight to the new pitch.
    CHECK(std::fabs(glideRun(fat::kAlways, 0.0f, true, mid) - 220.0) < 1.5);
    CHECK(std::fabs(mid - 220.0f) < 3.0f);
    // Glide 0.2 s: half way (in semitones) after 0.1 s, arrived later.
    CHECK(std::fabs(glideRun(fat::kAlways, 0.2f, true, mid) - 220.0) < 1.5);
    CHECK(mid > 140.0f && mid < 175.0f);
    float mid2;
    glideRun(fat::kAlways, 0.6f, true, mid2);
    CHECK(mid2 < mid - 10.0f);   // longer time: slower
    // Always and Legato glide without overlap; the Overlap modes do not.
    glideRun(fat::kAlways, 0.2f, false, mid);
    CHECK(mid > 140.0f && mid < 175.0f);
    glideRun(fat::kLegato, 0.2f, false, mid);
    CHECK(mid > 140.0f && mid < 175.0f);
    glideRun(fat::kOverlap, 0.2f, false, mid);
    CHECK(std::fabs(mid - 220.0f) < 3.0f);
    glideRun(fat::kLegatoOverlap, 0.2f, false, mid);
    CHECK(std::fabs(mid - 220.0f) < 3.0f);
    glideRun(fat::kOverlap, 0.2f, true, mid);
    CHECK(mid > 140.0f && mid < 175.0f);
    glideRun(fat::kLegatoOverlap, 0.2f, true, mid);
    CHECK(mid > 140.0f && mid < 175.0f);
    // A fresh voice (polyphony > 1) glides from the slot's previous note in Always.
    {
        std::vector<float> q = single();
        q[fat::kGlideTime] = 0.2f;
        Rig r(q);
        r.on(0, 45.0f);
        r.run(5);
        r.alive[0] = false;
        r.L.clear();
        r.on(1, 57.0f);
        r.run(40);
        const double f = freq(slice(r.L, 0.08, 0.115));   // ~0.1 s after the note
        CHECK(f > 140.0 && f < 175.0);
    }
    // A Glide FX owns the start pitch: no patch glide on top.
    {
        std::vector<float> q = single();
        q[fat::kGlideTime] = 0.2f;
        Rig r(q);
        r.on(0, 45.0f);
        r.run(35);
        r.L.clear();
        SynthNote n;
        n.pitch = 57.0f;
        n.note = 57;
        n.reused = true;
        n.overlap = true;
        n.fxGlide = true;
        r.e->noteOn(r.v[0], r.p, n);
        r.run(40);
        CHECK(std::fabs(freq(slice(r.L, 0.08, 0.115)) - 220.0) < 3.0);
    }
}

TEST(fat_legato_skips_retrigger) {
    std::vector<float> q = held();
    q[fat::kAmpA] = 0.0f;
    q[fat::kAmpD] = 0.05f;
    q[fat::kAmpS] = 0.3f;
    for (int mode = 0; mode < 4; ++mode) {
        q[fat::kGlideMode] = float(mode);
        Rig r(q);
        r.on(0, 48.0f);
        r.run(170);
        CHECK(std::fabs(r.v[0].level - 0.3f) < 0.01f);
        r.on(0, 50.0f, true, true);
        r.run(1);
        const bool legato = mode == fat::kLegato || mode == fat::kLegatoOverlap;
        if (legato) CHECK(std::fabs(r.v[0].level - 0.3f) < 0.01f);
        else CHECK(r.v[0].level > 0.6f);
    }
}

// --- every parameter, macros ------------------------------------------------------------------

// The context every parameter is heard in: the stack part detuned and part
// pulse, envelopes that move, filter envelope and LFO routed, and a note
// sequence exercising glide / legato / release: note A, B legato over A,
// release, C on the released voice.
std::vector<float> richContext() {
    std::vector<float> c = initValues();
    c[fat::kFatness] = 0.5f;
    c[fat::kBrightness] = 0.6f;
    c[fat::kOscLfo] = 0.2f;
    c[fat::kTimbre] = 0.3f;
    c[fat::kNoise] = 0.05f;
    c[fat::kReset] = 0.0f;
    c[fat::kCutoff] = 1500.0f;
    c[fat::kResonance] = 0.6f;
    c[fat::kFilterLfo] = 0.1f;
    c[fat::kFilterEnv] = 0.3f;
    c[fat::kAmpD] = 0.3f;
    c[fat::kAmpS] = 0.5f;
    c[fat::kAmpR] = 0.2f;
    c[fat::kEnvA] = 0.05f;
    c[fat::kEnvS] = 0.3f;
    c[fat::kLfoHz] = 7.0f;
    c[fat::kGlideTime] = 0.1f;
    return c;
}

Buf renderSequence(const std::vector<float>& vals) {
    Rig r(vals);
    r.run(20);   // a free-running LFO moves before the first note
    r.on(0, 57.0f);
    r.run(kBlocksPerSec / 4);
    r.on(0, 64.0f, true, true);   // legato over the held note
    r.run(kBlocksPerSec / 4);
    r.off(0);
    r.run(kBlocksPerSec / 8);
    r.on(0, 52.0f, true, false);  // the voice in its release
    r.run(kBlocksPerSec / 4);
    r.off(0);
    r.run(kBlocksPerSec / 4);
    return r.L;
}

// Values of parameter i to compare: every enum value, else min / mid / max.
std::vector<float> probeValues(int i) {
    const PatchParamRange* g = range(i);
    const bool isEnum = i == fat::kFilterType || i == fat::kLfoWave || i == fat::kLfoRetrig || i == fat::kGlideMode ||
                        i == fat::kReset;
    std::vector<float> out;
    if (isEnum) {
        for (int k = int(g->min); k <= int(g->max); ++k) out.push_back(float(k));
    } else {
        out = {g->min, 0.5f * (g->min + g->max), g->max};
    }
    return out;
}

TEST(fat_every_parameter_has_an_effect) {
    const std::vector<float> ctx = richContext();
    for (int i = 0; i < kN; ++i) {
        if (i == fat::kVolume) continue;   // the framework's job
        const std::vector<float> vals = probeValues(i);
        std::vector<Buf> outs;
        for (float x : vals) {
            std::vector<float> c = ctx;
            c[i] = x;
            outs.push_back(renderSequence(c));
            CHECK(finite(outs.back()));
        }
        for (size_t k = 1; k < outs.size(); ++k) {
            const double d = rmsDiff(outs[k - 1], outs[k]);
            const bool ok = d > 1.0e-4;
            CHECK(ok);
            if (!ok) std::printf("       param %d: %g vs %g differ by %g only\n", i, vals[k - 1], vals[k], d);
        }
    }
}

TEST(fat_macro_pushed_values) {
    // Macros push stored values off the enum grid: enums round, Reset is On
    // from 0.5.
    std::vector<float> a = richContext(), b = richContext();
    a[fat::kFilterType] = 1.0f;
    b[fat::kFilterType] = 0.6f;
    CHECK(renderSequence(a) == renderSequence(b));
    a[fat::kLfoWave] = 3.0f;
    b[fat::kLfoWave] = 3.4f;
    CHECK(renderSequence(a) == renderSequence(b));
    a[fat::kReset] = 1.0f;
    b[fat::kReset] = 0.5f;
    CHECK(renderSequence(a) == renderSequence(b));
    // Per-block movement of every parameter (a macro sweeping all of them)
    // stays finite and click-free enough: no frame-to-frame jump beyond the
    // static renders' largest.
    const std::vector<float> lo = richContext();
    std::vector<float> hi = lo;
    for (int i = 0; i < kN; ++i) hi[i] = range(i)->min + 0.8f * (range(i)->max - range(i)->min);
    for (int t : {fat::kAmpA, fat::kAmpD, fat::kAmpR, fat::kEnvA, fat::kEnvD, fat::kEnvR}) hi[t] = lo[t];
    Rig r(lo);
    r.on(0, 48.0f);
    r.run(20);
    const int steps = 100;
    for (int s = 1; s <= steps; ++s) {
        for (int i = 0; i < kN; ++i) {
            r.p.start[i] = r.p.end[i];
            r.p.end[i] = lo[i] + (hi[i] - lo[i]) * float(s) / float(steps);
        }
        r.p.moving = true;
        ++r.p.serial;
        r.run(1);
    }
    CHECK(finite(r.L));
    CHECK(peak(r.L) < 4.0f);
    // The static renders' largest frame-to-frame step (same note, same
    // length), at either end of the sweep.
    auto maxJump = [](const Buf& x) {
        double m = 0.0;
        for (size_t i = 1; i < x.size(); ++i) m = std::max(m, double(std::fabs(x[i] - x[i - 1])));
        return m;
    };
    double ref = 0.0;
    for (int k = 0; k < 2; ++k) {
        Rig s(k ? hi : lo);
        s.on(0, 48.0f);
        s.run(20 + steps);
        ref = std::max(ref, maxJump(s.L));
    }
    const double j = maxJump(r.L);
    CHECK(j <= 1.5 * ref);
    if (!(j <= 1.5 * ref)) std::printf("       sweep jump %.4f vs static %.4f\n", j, ref);
}

// --- numerics ------------------------------------------------------------------------------

TEST(fat_release_tail_is_clean) {
    // A resonant voice released to its end: no subnormal samples on the way
    // (the filter states are flushed to 0), finite state, exact silence after.
    for (int t = 0; t < 3; ++t) {
        std::vector<float> q = held();
        q[fat::kFilterType] = float(t);
        q[fat::kCutoff] = 2000.0f;
        q[fat::kResonance] = 1.2f;
        q[fat::kAmpR] = 1.0f;
        Rig r(q);
        r.on(0, 48.0f);
        r.run(70);
        r.off(0);
        int blocks = 0;
        while (r.run(1) && blocks < 4 * kBlocksPerSec) ++blocks;
        CHECK(blocks < 4 * kBlocksPerSec);
        bool subnormal = false;
        for (float x : r.L)
            if (std::fpclassify(x) == FP_SUBNORMAL) subnormal = true;
        CHECK(!subnormal);
        for (float z : r.v[0].as<PhaseProbe>().ph) CHECK(std::isfinite(z));
        r.L.clear();
        r.run(10);
        CHECK(peak(r.L) == 0.0f);
    }
}

// --- polyphony and numerics --------------------------------------------------------------

TEST(fat_chord_is_the_sum_of_its_voices) {
    const std::vector<float> c = held();
    const float notes[4] = {48.0f, 55.0f, 60.0f, 64.0f};
    Rig chord(c);
    for (int k = 0; k < 4; ++k) chord.on(k, notes[k]);
    CHECK_EQ(chord.run(100), 4);
    Buf sum(chord.L.size(), 0.0f);
    for (int k = 0; k < 4; ++k) {
        Rig one(c);
        // the same slot history (random sources) as in the chord
        for (int j = 0; j < k; ++j) one.on(7, 30.0f);
        one.alive[7] = false;
        one.on(k, notes[k]);
        one.run(100);
        for (size_t i = 0; i < sum.size(); ++i) sum[i] += one.L[i];
    }
    CHECK(rmsDiff(sum, chord.L) < 1.0e-5);
    CHECK(rms(chord.L) > 0.05);
    // Every pool voice at once stays finite.
    Rig eight(c);
    for (int k = 0; k < kSynthMaxVoices; ++k) eight.on(k, 40.0f + 5.0f * float(k));
    CHECK_EQ(eight.run(50), kSynthMaxVoices);
    CHECK(finite(eight.L));
}

TEST(fat_random_parameter_sets_stay_finite) {
    uint32_t seed = 12345;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) * (1.0f / 16777216.0f);
    };
    int bad = 0;
    float worst = 0.0f;
    for (int set = 0; set < 1000; ++set) {
        std::vector<float> a(kN), b(kN);
        for (int i = 0; i < kN; ++i) {
            const PatchParamRange* g = range(i);
            a[i] = g->min + rnd() * (g->max - g->min);
            b[i] = rnd() < 0.5f ? a[i] : g->min + rnd() * (g->max - g->min);
            if (rnd() < 0.15f) a[i] = rnd() < 0.5f ? g->min : g->max;   // extremes
        }
        // short times keep the run short and busy
        for (int t : {fat::kAmpA, fat::kAmpD, fat::kAmpR, fat::kEnvA, fat::kEnvD, fat::kEnvR})
            a[t] *= 0.05f, b[t] *= 0.05f;
        Rig r(a);
        r.on(0, float(int(rnd() * 127.0f)));
        r.on(1, float(int(rnd() * 127.0f)));
        r.v[1].bend = r.v[1].bendStart = (rnd() - 0.5f) * 48.0f;
        r.run(8);
        // a ramp: start = a, end = b
        for (int i = 0; i < kN; ++i) r.p.end[i] = b[i];
        ++r.p.serial;
        r.p.moving = true;
        r.run(1);
        for (int i = 0; i < kN; ++i) r.p.start[i] = b[i];
        r.p.moving = false;
        r.on(0, float(int(rnd() * 127.0f)), true, rnd() < 0.5f);
        r.run(8);
        r.off(0);
        r.off(1);
        r.run(8);
        const float pk = peak(r.L);
        if (!finite(r.L) || pk > 8.0f) {
            ++bad;
            std::printf("       set %d peak %g:", set, pk);
            for (int i = 0; i < kN; ++i) std::printf(" %d=%g/%g", i, a[i], b[i]);
            std::printf("\n");
        }
        if (std::isfinite(pk)) worst = std::max(worst, pk);
    }
    CHECK_EQ(bad, 0);
    std::printf("  random sets: worst peak %.2f\n", worst);
}

// --- factory patches ---------------------------------------------------------------

TEST(fat_factory_patches_render_cleanly) {
    const char* root = std::getenv("OT_VENDOR_FIXTURES");
    fs::path dir = fs::path(root ? root : "") / "mini-patches-2.0" / "FAT";
    std::error_code ec;
    if (!root || !fs::is_directory(dir, ec)) {
        std::printf("  SKIP: no vendor FAT patches (OT_VENDOR_FIXTURES=%s)\n", root ? root : "unset");
        return;
    }
    std::vector<fs::path> files;
    for (auto& de : fs::directory_iterator(dir, ec))
        if (de.path().extension() == ".patch") files.push_back(de.path());
    std::sort(files.begin(), files.end());
    CHECK(!files.empty());
    int n = 0;
    float worst = 0.0f;
    for (auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        static SynthPatch patch;
        if (decodePatch(bytes.data(), bytes.size(), &patch) != PatchStatus::kOk || patch.engine != kSynthEngineFat) {
            CHECK(false);
            continue;
        }
        uint8_t mv[kSynthSeqMacros];
        for (int m = 0; m < kSynthSeqMacros; ++m) mv[m] = synthMacroDefault(patch.macros[m].bipolar);
        std::vector<float> vals(kN);
        CHECK_EQ(synthEffectiveParams(patch, mv, 1.0f, vals.data()), kN);
        Rig r(vals);
        r.on(0, 48.0f);
        r.on(1, 55.0f);
        r.run(kBlocksPerSec * 7 / 10);
        r.off(0);
        r.off(1);
        r.run(kBlocksPerSec * 3 / 10);
        const float pk = peak(r.L);
        const bool ok = finite(r.L) && pk < 8.0f && rms(r.L) > 1.0e-4;
        CHECK(ok);
        if (!ok) std::printf("       %s: peak %g rms %g\n", f.filename().string().c_str(), pk, rms(r.L));
        worst = std::max(worst, pk);
        ++n;
    }
    std::printf("  %d factory FAT patches rendered, loudest peak %.2f\n", n, worst);
}

// --- [OT] Stereo (stereo S4) -------------------------------------------------------
// Stereo s puts each pair's up-detuned saw toward L and its down-detuned saw
// toward R: L = M + s D, R = M - s D with M the mono stack and D = sum of
// (up - down) per pair, each side through its own filter. s = 0 (absent in
// every existing patch) is today's engine, bit for bit.

namespace {
std::vector<float> withStereo(std::vector<float> v, float s) {
    v.resize(size_t(fat::kStereoParam) + 1, 0.0f);
    v[size_t(fat::kStereoParam)] = s;
    return v;
}
struct StereoRig : Rig {
    explicit StereoRig(const std::vector<float>& vals) : Rig(initValues()) {
        p.count = int(vals.size());
        for (int i = 0; i < p.count; ++i) p.start[i] = p.end[i] = vals[size_t(i)];
        ++p.serial;
    }
};
}  // namespace

TEST(fat_stereo_index_is_the_patch_extension) {
    CHECK_EQ(fat::kStereoParam, synthFatStereoParam());
}

TEST(fat_stereo_zero_is_bit_identical) {
    uint32_t seed = 777;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) * (1.0f / 16777216.0f);
    };
    for (int set = 0; set < 100; ++set) {
        std::vector<float> a(kN);
        for (int i = 0; i < kN; ++i) a[i] = range(i)->min + rnd() * (range(i)->max - range(i)->min);
        for (int t : {fat::kAmpA, fat::kAmpD, fat::kAmpR, fat::kEnvA, fat::kEnvD, fat::kEnvR}) a[t] *= 0.05f;
        Rig m(a);
        StereoRig s(withStereo(a, 0.0f));
        for (Rig* r : {static_cast<Rig*>(&m), static_cast<Rig*>(&s)}) {
            r->on(0, 60.0f);
            r->on(1, 67.0f);
            r->run(12);
            r->off(0);
            r->off(1);
            r->run(12);
        }
        CHECK(m.L == s.L && m.R == s.R);
        CHECK(m.L == m.R);
    }
}

TEST(fat_stereo_splits_the_pairs_and_keeps_the_mono_sum) {
    std::vector<float> a = initValues();
    a[fat::kFatness] = 1.0f;
    a[fat::kFilterType] = float(fat::kLpMg24);   // linear stages: the sum is exact
    Rig m(a);
    StereoRig s(withStereo(a, 1.0f));
    for (Rig* r : {static_cast<Rig*>(&m), static_cast<Rig*>(&s)}) {
        r->on(0, 57.0f);
        r->run(60);
    }
    CHECK(rmsDiff(s.L, s.R) > 0.05 * rms(s.L));
    Buf sum(s.L.size());
    for (size_t i = 0; i < sum.size(); ++i) sum[i] = 0.5f * (s.L[i] + s.R[i]);
    CHECK(rmsDiff(sum, m.L) < 1e-4 * rms(m.L));
    CHECK(finite(s.L) && finite(s.R));
}

TEST(fat_stereo_without_pairs_is_mono) {
    std::vector<float> a = initValues();
    a[fat::kFatness] = 0.0f;   // the centre saw only: nothing to spread
    StereoRig s(withStereo(a, 1.0f));
    s.on(0, 60.0f);
    s.run(30);
    CHECK(s.L == s.R);
}

TEST(fat_stereo_edit_ramps_without_a_click) {
    std::vector<float> a = initValues();
    a[fat::kFatness] = 1.0f;
    StereoRig s(withStereo(a, 0.0f));
    s.on(0, 60.0f);
    s.run(40);
    const size_t at = s.L.size();
    s.p.end[fat::kStereoParam] = 1.0f;   // a live edit: start 0 -> end 1
    ++s.p.serial;
    s.p.moving = true;
    s.run(1);
    s.p.start[fat::kStereoParam] = 1.0f;
    s.p.moving = false;
    s.run(1);
    float d0 = 0.0f, d1 = 0.0f;
    for (size_t i = at; i < at + 8; ++i) d0 = std::max(d0, std::fabs(s.L[i] - s.R[i]));
    for (size_t i = at + kBlock; i < at + 2 * kBlock; ++i) d1 = std::max(d1, std::fabs(s.L[i] - s.R[i]));
    CHECK(d1 > 0.0f);
    CHECK(d0 < 0.25f * d1);
}

TEST(fat_stereo_random_sets_stay_finite) {
    uint32_t seed = 4242;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) * (1.0f / 16777216.0f);
    };
    int bad = 0;
    for (int set = 0; set < 300; ++set) {
        std::vector<float> a(kN);
        for (int i = 0; i < kN; ++i) a[i] = range(i)->min + rnd() * (range(i)->max - range(i)->min);
        for (int t : {fat::kAmpA, fat::kAmpD, fat::kAmpR, fat::kEnvA, fat::kEnvD, fat::kEnvR}) a[t] *= 0.05f;
        StereoRig s(withStereo(a, rnd()));
        s.on(0, float(int(rnd() * 127.0f)));
        s.on(1, float(int(rnd() * 127.0f)));
        s.run(10);
        s.off(0);
        s.off(1);
        s.run(10);
        if (!finite(s.L) || !finite(s.R) || peak(s.L) > 8.0f || peak(s.R) > 8.0f) ++bad;
    }
    CHECK_EQ(bad, 0);
}
