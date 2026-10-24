// WTFM engine (2.2, audio/engines/wtfm.*, spec 21 §8.5, docs/specs/2x-08):
// the registry serves it, the Init patch plays its note, the framework never
// sees Volume applied, the Ratio table and Finetune, the harmonic signature
// of every Character row along Shape, FM sidebands, the operator-2 skip, the
// three feedback paths, Reset, Oversampling, every parameter audible,
// macro-pushed values, polyphonic chords, no NaN / Inf over random parameter
// sets, and every factory WTFM patch ($OT_VENDOR_FIXTURES/mini-patches-2.0/WTFM,
// skipped when absent) renders cleanly. Cycle counts: tests/audio_bench.cpp
// (wtfminit / wtfmworst / mixwtfm, make perf-gate).
#include "test.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "audio/engines/wtfm.h"
#include "audio/synth_engine.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"

using namespace ot;
using namespace ot::audio;
namespace fs = std::filesystem;

namespace {

constexpr int kN = wtfm::kParamCount;
constexpr int kBlocksPerSec = int(kSampleRate / float(kBlock)) + 1;   // 345

const PatchParamRange* range(int i) { return synthParamRange(kSynthEngineWtfm, i); }

// Default (Init) values.
std::vector<float> initValues() {
    std::vector<float> v(kN);
    for (int i = 0; i < kN; ++i) v[i] = range(i)->def;
    return v;
}

// One engine voice pool driven directly (no framework).
struct Rig {
    ISynthEngine* e = synthEngineFor(kSynthEngineWtfm);
    SynthSlotScratch scratch{};
    SynthParams p;
    SynthVoice v[kSynthMaxVoices];
    bool alive[kSynthMaxVoices] = {};
    std::vector<float> L, R;

    explicit Rig(const std::vector<float>& vals) {
        p.engine = kSynthEngineWtfm;
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
// A plain sustained voice: full sustain, open filter (LP SVF 12 at 20 kHz),
// no modulation.
std::vector<float> held() {
    std::vector<float> c = initValues();
    c[wtfm::kAmpS] = 1.0f;
    c[wtfm::kFilterType] = 4.0f;
    c[wtfm::kCutoff] = 20000.0f;
    c[wtfm::kResonance] = 0.0f;
    c[wtfm::kFilterEnvAmt] = 0.0f;
    c[wtfm::kNoteTrack] = 0.0f;
    return c;
}

Buf play(const std::vector<float>& vals, float pitch = 48.0f, float seconds = 0.5f) {
    Rig r(vals);
    r.on(0, pitch);
    r.run(int(seconds * kSampleRate / float(kBlock)) + 1);
    return r.L;
}

double hz(float pitch) { return 440.0 * std::pow(2.0, (double(pitch) - 69.0) / 12.0); }

// Harmonic k of a held note 48 (steady part), relative to the fundamental.
struct Harm {
    double h[6];
};
Harm harmonics(const std::vector<float>& vals, float pitch = 48.0f) {
    const Buf x = slice(play(vals, pitch, 0.6f), 0.2, 0.6);
    Harm r{};
    const double f = hz(pitch);
    const double h1 = tone(x, f);
    for (int k = 1; k <= 5; ++k) r.h[k] = h1 > 0 ? tone(x, f * k) / h1 : 0.0;
    r.h[0] = h1;
    return r;
}

std::vector<float> rowAt(int ch, float shape) {
    std::vector<float> c = held();
    c[wtfm::kChar1] = float(ch);
    c[wtfm::kShape1] = shape;
    return c;
}

}  // namespace

// --- registry and contract -------------------------------------------------------------

TEST(wtfm_registered) {
    ISynthEngine* e = synthEngineFor(kSynthEngineWtfm);
    CHECK(e != nullptr);
    CHECK_EQ(e->id(), int32_t(kSynthEngineWtfm));
    CHECK_STR(e->name(), "WTFM");
    CHECK_EQ(kN, 63);
}

TEST(wtfm_init_patch_plays_the_note) {
    const Buf x = play(initValues(), 60.0f, 0.6f);
    CHECK(finite(x));
    CHECK(rms(x) > 0.01);
    const double f = pitchHz(x, size_t(0.2 * kSampleRate), size_t(0.6 * kSampleRate));
    CHECK_NEAR(f, hz(60.0f), hz(60.0f) * 0.01);
}

TEST(wtfm_volume_is_never_applied) {
    std::vector<float> a = initValues(), b = initValues();
    a[wtfm::kVolume] = 0.0f;
    b[wtfm::kVolume] = 2.0f;
    CHECK(play(a) == play(b));
}

// --- operators ---------------------------------------------------------------------------

TEST(wtfm_ratio_table_and_finetune) {
    CHECK_NEAR(wtfm::ratioOf(0), 0.25f, 1e-6);
    CHECK_NEAR(wtfm::ratioOf(3), 1.0f, 1e-6);
    CHECK_NEAR(wtfm::ratioOf(7), 2.0f, 1e-6);
    CHECK_NEAR(wtfm::ratioOf(47), 12.0f, 1e-6);
    CHECK_NEAR(wtfm::ratioOf(99), 12.0f, 1e-6);
    CHECK_EQ(int(range(wtfm::kRatio1)->def), 3);
    const size_t a = size_t(0.2 * kSampleRate), b = size_t(0.5 * kSampleRate);
    std::vector<float> c = held();
    const double f0 = pitchHz(play(c, 48.0f), a, b);
    CHECK_NEAR(f0, hz(48.0f), hz(48.0f) * 0.01);
    c[wtfm::kRatio1] = 7.0f;   // 2.00: an octave up
    CHECK_NEAR(pitchHz(play(c, 48.0f), a, b), 2.0 * hz(48.0f), 2.0 * hz(48.0f) * 0.01);
    c[wtfm::kRatio1] = 2.4f;   // macro-pushed: rounds to 2 = 0.75
    CHECK_NEAR(pitchHz(play(c, 60.0f), a, b), 0.75 * hz(60.0f), 0.75 * hz(60.0f) * 0.01);
    c[wtfm::kRatio1] = 3.0f;
    c[wtfm::kFine1] = 100.0f;  // +1 semitone
    CHECK_NEAR(pitchHz(play(c, 48.0f), a, b), hz(49.0f), hz(49.0f) * 0.01);
}

TEST(wtfm_morph_rows_triangle_saw_square) {
    // Smoother at Shape 0: a sine; 0.5: a rounded saw (even harmonics);
    // 1: a rounded square (odd harmonics only).
    Harm s0 = harmonics(rowAt(wtfm::kSmoother, 0.0f));
    CHECK(s0.h[2] < 0.01 && s0.h[3] < 0.01);
    Harm s5 = harmonics(rowAt(wtfm::kSmoother, 0.5f));
    CHECK(s5.h[2] > 0.15 && s5.h[3] > 0.05);
    Harm s1 = harmonics(rowAt(wtfm::kSmoother, 1.0f));
    CHECK(s1.h[2] < 0.03 && s1.h[3] > 0.15);
    // Sharper / Wilder at Shape 0: a true triangle (3rd harmonic at 1/9).
    for (int ch : {int(wtfm::kSharper), int(wtfm::kWilder)}) {
        Harm t = harmonics(rowAt(ch, 0.0f));
        CHECK(t.h[2] < 0.01);
        if (ch == wtfm::kSharper) CHECK(t.h[3] > 0.08 && t.h[3] < 0.14);
    }
    // Brightness grows Smoother -> Sharper -> Wilder at the same Shape.
    const double bs = bright(slice(play(rowAt(wtfm::kSmoother, 0.5f)), 0.2, 0.5));
    const double bh = bright(slice(play(rowAt(wtfm::kSharper, 0.5f)), 0.2, 0.5));
    const double bw = bright(slice(play(rowAt(wtfm::kWilder, 0.5f)), 0.2, 0.5));
    CHECK(bh > bs * 1.1);
    CHECK(bw > bh * 1.05);
    // The square end keeps the even harmonics out on every morph row.
    for (int ch = wtfm::kSmoother; ch <= wtfm::kWilder; ++ch) CHECK(harmonics(rowAt(ch, 1.0f)).h[2] < 0.03);
}

TEST(wtfm_add_rows_partial_series) {
    // Shape 0: a sine on every Add row.
    for (int ch = wtfm::kAdd1; ch <= wtfm::kAdd11; ++ch) {
        Harm z = harmonics(rowAt(ch, 0.0f));
        CHECK(z.h[2] < 0.01 && z.h[3] < 0.01);
    }
    // Shape 1: the partials |1 + kN| (k = 0, +-1, ...): Add 2 odd harmonics
    // only, Add 3 every harmonic but the multiples of 3, Add 5 = 1, 4, 6, 9, ...
    Harm a1 = harmonics(rowAt(wtfm::kAdd1, 1.0f));
    CHECK(a1.h[2] > 0.1 && a1.h[3] > 0.05);
    Harm a2 = harmonics(rowAt(wtfm::kAdd2, 1.0f));
    CHECK(a2.h[2] < 0.01 && a2.h[4] < 0.01 && a2.h[3] > 0.1);
    Harm a3 = harmonics(rowAt(wtfm::kAdd3, 1.0f));
    CHECK(a3.h[3] < 0.01 && a3.h[2] > 0.05 && a3.h[4] > 0.05);
    Harm a5 = harmonics(rowAt(wtfm::kAdd5, 1.0f));
    CHECK(a5.h[2] < 0.01 && a5.h[3] < 0.01 && a5.h[5] < 0.01 && a5.h[4] > 0.05);
}

TEST(wtfm_band_limited_at_high_notes) {
    // A bright row high up: little energy away from the harmonics.
    for (int ch : {int(wtfm::kSharper), int(wtfm::kAdd1), int(wtfm::kAdd11)}) {
        const float pitch = 100.0f;
        const Buf x = slice(play(rowAt(ch, 0.6f), pitch, 0.5f), 0.2, 0.5);
        double harm = 0.0;
        for (int k = 1; k * hz(pitch) < 0.5 * kSampleRate; ++k) harm += std::pow(tone(x, k * hz(pitch)), 2.0);
        // Power of the signal vs its harmonics' (a sine of amplitude A reads
        // A / 4 through the Hann-windowed Goertzel: power A^2 / 2 = 8 tone^2).
        double total = 0.0;
        for (float s : x) total += double(s) * s;
        total /= double(x.size());
        const double alias = std::max(0.0, total - 8.0 * harm);
        CHECK(alias < 0.05 * total);
        std::printf("  row %d at pitch %.0f: inharmonic %.4f of %.4f\n", ch, double(pitch), alias, total);
    }
}

TEST(wtfm_fm_adds_sidebands) {
    std::vector<float> c = held();
    c[wtfm::kRatio2] = 7.0f;   // 2:1
    Harm plain = harmonics(c);
    CHECK(plain.h[3] < 0.01);
    c[wtfm::kFm] = 1.0f;       // 100 % = 1 rad
    Harm fm = harmonics(c);
    CHECK(fm.h[3] > 0.1);       // f1 + f2
    c[wtfm::kFm] = 3.0f;
    Harm more = harmonics(c);
    CHECK(more.h[5] > fm.h[5]);
    // Phase modulation: the timbre does not depend on the note.
    c[wtfm::kFm] = 1.0f;
    Harm hi = harmonics(c, 72.0f);
    CHECK_NEAR(hi.h[3], fm.h[3], 0.03);
}

TEST(wtfm_op2_silent_without_fm) {
    // Nothing listens to operator 2 while FM and Feedback 2>1 are 0: its
    // ratio, shape and own feedback do not change the output at all.
    std::vector<float> a = held(), b = held();
    b[wtfm::kRatio2] = 20.0f;
    b[wtfm::kChar2] = 5.0f;
    b[wtfm::kShape2] = 0.7f;
    b[wtfm::kFb2] = 1.0f;
    CHECK(play(a) == play(b));
}

TEST(wtfm_feedback_paths) {
    std::vector<float> c = held();
    const Buf base = play(c);
    c[wtfm::kFb1] = 0.5f;   // the carrier on itself: brighter
    const Buf f1 = play(c);
    CHECK(bright(slice(f1, 0.2, 0.5)) > 1.2 * bright(slice(base, 0.2, 0.5)));
    c[wtfm::kFb1] = 0.0f;
    c[wtfm::kFb21] = 0.6f;  // operator 2 into the carrier's feedback node, audible without FM
    CHECK(rmsDiff(play(c), base) > 0.01);
    c[wtfm::kFb21] = 0.0f;
    c[wtfm::kFm] = 1.0f;    // with FM on, Feedback 2 changes the modulator
    const Buf fm = play(c);
    c[wtfm::kFb2] = 0.8f;
    CHECK(rmsDiff(play(c), fm) > 0.01);
    // Full feedback stays bounded.
    c[wtfm::kFb1] = c[wtfm::kFb2] = c[wtfm::kFb21] = 1.0f;
    c[wtfm::kFm] = 10.0f;
    const Buf x = play(c, 30.0f);
    CHECK(finite(x));
    CHECK(peak(x) < 2.0f);
}

TEST(wtfm_reset_on_restarts_the_operators) {
    for (int reset = 0; reset < 2; ++reset) {
        std::vector<float> c = held();
        c[wtfm::kAmpA] = 0.0f;
        c[wtfm::kRatio2] = 4.0f;   // 1.25: the operators drift against each other
        c[wtfm::kFm] = 2.0f;
        c[wtfm::kFb1] = 0.3f;
        c[wtfm::kReset1] = c[wtfm::kReset2] = float(reset);
        Rig r(c);
        r.on(0, 48.0f);
        r.run(10);
        // From the second block on (a fresh voice's first block ramps its
        // amplitude envelope up from 0).
        const Buf first(r.L.begin() + kBlock, r.L.begin() + 10 * kBlock);
        r.run(7);
        const size_t at = r.L.size();
        r.on(0, 48.0f, true, false);   // the same voice again, not legato
        r.run(10);
        const Buf second(r.L.begin() + long(at) + kBlock, r.L.begin() + long(at) + 10 * kBlock);
        const double d = rmsDiff(first, second) / rms(first);
        std::printf("  reset %d: repeat differs by %.4f\n", reset, d);
        if (reset) CHECK(d < 0.02);
        else CHECK(d > 0.05);
    }
}

TEST(wtfm_oversampling) {
    // A mild patch: same pitch, same level within 1 dB.
    std::vector<float> c = held();
    const Buf off = play(c, 60.0f);
    c[wtfm::kOversample] = 1.0f;
    const Buf on = play(c, 60.0f);
    const double ratio = rms(slice(on, 0.2, 0.5)) / rms(slice(off, 0.2, 0.5));
    CHECK(ratio > 0.89 && ratio < 1.12);
    CHECK_NEAR(pitchHz(on, size_t(0.2 * kSampleRate), size_t(0.5 * kSampleRate)), hz(60.0f), hz(60.0f) * 0.01);
    // A harsh patch high up: it changes the sound (less aliasing).
    std::vector<float> h = held();
    h[wtfm::kChar1] = float(wtfm::kWilder);
    h[wtfm::kShape1] = 0.6f;
    h[wtfm::kFm] = 4.0f;
    h[wtfm::kFb1] = 0.7f;
    h[wtfm::kRatio2] = 11.0f;
    const Buf a = play(h, 90.0f);
    h[wtfm::kOversample] = 1.0f;
    const Buf b = play(h, 90.0f);
    CHECK(finite(b));
    CHECK(rmsDiff(a, b) > 0.005);
}

// --- every parameter, macros ------------------------------------------------------------------

namespace {
std::vector<float> richContext() {
    std::vector<float> c = initValues();
    c[wtfm::kFm] = 2.0f;
    c[wtfm::kFb1] = 0.3f;
    c[wtfm::kFb2] = 0.3f;
    c[wtfm::kFb21] = 0.3f;
    c[wtfm::kShape1] = 0.4f;
    c[wtfm::kShape2] = 0.3f;
    c[wtfm::kChar1] = float(wtfm::kSharper);
    c[wtfm::kChar2] = float(wtfm::kAdd1);
    c[wtfm::kRatio2] = 7.0f;
    c[wtfm::kCutoff] = 1500.0f;
    c[wtfm::kResonance] = 0.6f;
    c[wtfm::kFilterEnvAmt] = 0.3f;
    c[wtfm::kAmpD] = 0.3f;
    c[wtfm::kAmpS] = 0.5f;
    c[wtfm::kAmpR] = 0.2f;
    c[wtfm::kEnv1A] = 0.05f;
    c[wtfm::kEnv1S] = 0.3f;
    c[wtfm::kEnv2A] = 0.05f;
    c[wtfm::kEnv2D] = 0.3f;
    c[wtfm::kEnv2S] = 0.2f;
    c[wtfm::kEnv2R] = 0.2f;
    c[wtfm::kLfo1Ratio] = 7.0f;
    c[wtfm::kLfo2Ratio] = 3.0f;
    c[wtfm::kGlideTime] = 0.1f;
    // Mod slots: every source and a spread of destinations.
    const int route[6][2] = {{3, wtfm::kDstShape1}, {2, wtfm::kDstFm}, {4, wtfm::kDstCutoff},
                             {0, wtfm::kDstFb21}, {1, wtfm::kDstPitch2}, {3, wtfm::kDstVolume}};
    for (int k = 0; k < 6; ++k) {
        c[wtfm::kModBase + 3 * k] = float(route[k][0]);
        c[wtfm::kModBase + 3 * k + 1] = float(route[k][1]);
        c[wtfm::kModBase + 3 * k + 2] = 0.3f;
    }
    return c;
}

Buf renderSequence(const std::vector<float>& vals) {
    Rig r(vals);
    r.run(20);
    r.on(0, 57.0f);
    r.run(kBlocksPerSec / 4);
    r.on(0, 64.0f, true, true);
    r.run(kBlocksPerSec / 4);
    r.off(0);
    r.run(kBlocksPerSec / 8);
    r.on(0, 52.0f, true, false);
    r.run(kBlocksPerSec / 4);
    r.off(0);
    r.run(kBlocksPerSec / 4);
    return r.L;
}

bool isEnum(int i) {
    switch (i) {
        case wtfm::kOversample: case wtfm::kChar1: case wtfm::kChar2: case wtfm::kFilterType:
        case wtfm::kLfo1Wave: case wtfm::kLfo1Retrig: case wtfm::kLfo1Sync:
        case wtfm::kLfo2Wave: case wtfm::kLfo2Retrig: case wtfm::kLfo2Sync:
        case wtfm::kGlideMode: case wtfm::kReset1: case wtfm::kReset2: return true;
        default: break;
    }
    if (i >= wtfm::kModBase && i < wtfm::kModBase + 18) return (i - wtfm::kModBase) % 3 != 2;
    return false;
}

std::vector<float> probeValues(int i) {
    const PatchParamRange* g = range(i);
    std::vector<float> out;
    if (isEnum(i)) {
        for (int k = int(g->min); k <= int(g->max); ++k) out.push_back(float(k));
    } else {
        out = {g->min, 0.5f * (g->min + g->max), g->max};
    }
    return out;
}
}  // namespace

TEST(wtfm_every_parameter_has_an_effect) {
    const std::vector<float> ctx = richContext();
    for (int i = 0; i < kN; ++i) {
        if (i == wtfm::kVolume || i == 6 || i == 7) continue;   // the framework's job / not in the UI
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

TEST(wtfm_macro_pushed_values) {
    std::vector<float> a = richContext(), b = richContext();
    a[wtfm::kChar1] = 2.0f;
    b[wtfm::kChar1] = 1.6f;
    CHECK(renderSequence(a) == renderSequence(b));
    a[wtfm::kRatio2] = 7.0f;
    b[wtfm::kRatio2] = 7.4f;
    CHECK(renderSequence(a) == renderSequence(b));
    a[wtfm::kOversample] = 1.0f;
    b[wtfm::kOversample] = 0.5f;
    CHECK(renderSequence(a) == renderSequence(b));
    // A macro sweeping every parameter stays finite and bounded.
    const std::vector<float> lo = richContext();
    std::vector<float> hi = lo;
    for (int i = 0; i < kN; ++i) hi[i] = range(i)->min + 0.8f * (range(i)->max - range(i)->min);
    for (int t : {wtfm::kAmpA, wtfm::kAmpD, wtfm::kAmpR}) hi[t] = lo[t];
    Rig r(lo);
    r.on(0, 48.0f);
    r.run(20);
    for (int s = 1; s <= 100; ++s) {
        for (int i = 0; i < kN; ++i) {
            r.p.start[i] = r.p.end[i];
            r.p.end[i] = lo[i] + (hi[i] - lo[i]) * float(s) / 100.0f;
        }
        r.p.moving = true;
        ++r.p.serial;
        r.run(1);
    }
    CHECK(finite(r.L));
    CHECK(peak(r.L) < 4.0f);
}

TEST(wtfm_release_tail_is_clean) {
    std::vector<float> q = held();
    q[wtfm::kFilterType] = 1.0f;
    q[wtfm::kCutoff] = 2000.0f;
    q[wtfm::kResonance] = 1.2f;
    q[wtfm::kAmpR] = 1.0f;
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
    r.L.clear();
    r.run(10);
    CHECK(peak(r.L) == 0.0f);
}

TEST(wtfm_chord_is_the_sum_of_its_voices) {
    std::vector<float> c = held();
    c[wtfm::kFm] = 1.5f;
    c[wtfm::kRatio2] = 7.0f;
    const float notes[4] = {48.0f, 55.0f, 60.0f, 64.0f};
    Rig chord(c);
    for (int k = 0; k < 4; ++k) chord.on(k, notes[k]);
    CHECK_EQ(chord.run(100), 4);
    Buf sum(chord.L.size(), 0.0f);
    for (int k = 0; k < 4; ++k) {
        Rig one(c);
        one.on(k, notes[k]);
        one.run(100);
        for (size_t i = 0; i < sum.size(); ++i) sum[i] += one.L[i];
    }
    CHECK(rmsDiff(sum, chord.L) < 1.0e-5);
    Rig eight(c);
    for (int k = 0; k < kSynthMaxVoices; ++k) eight.on(k, 40.0f + 5.0f * float(k));
    CHECK_EQ(eight.run(50), kSynthMaxVoices);
    CHECK(finite(eight.L));
}

TEST(wtfm_random_parameter_sets_stay_finite) {
    uint32_t seed = 54321;
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
            if (rnd() < 0.15f) a[i] = rnd() < 0.5f ? g->min : g->max;
        }
        for (int t : {wtfm::kAmpA, wtfm::kAmpD, wtfm::kAmpR, wtfm::kEnv1A, wtfm::kEnv1D, wtfm::kEnv1R})
            a[t] *= 0.05f, b[t] *= 0.05f;
        Rig r(a);
        r.on(0, float(int(rnd() * 127.0f)));
        r.on(1, float(int(rnd() * 127.0f)));
        r.v[1].bend = r.v[1].bendStart = (rnd() - 0.5f) * 48.0f;
        r.run(8);
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
            std::printf("       set %d peak %g\n", set, pk);
        }
        if (std::isfinite(pk)) worst = std::max(worst, pk);
    }
    CHECK_EQ(bad, 0);
    std::printf("  random sets: worst peak %.2f\n", worst);
}

TEST(wtfm_factory_patches_render_cleanly) {
    const char* root = std::getenv("OT_VENDOR_FIXTURES");
    fs::path dir = fs::path(root ? root : "") / "mini-patches-2.0" / "WTFM";
    std::error_code ec;
    if (!root || !fs::is_directory(dir, ec)) {
        std::printf("  SKIP: no vendor WTFM patches (OT_VENDOR_FIXTURES=%s)\n", root ? root : "unset");
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
        if (decodePatch(bytes.data(), bytes.size(), &patch) != PatchStatus::kOk || patch.engine != kSynthEngineWtfm) {
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
    std::printf("  %d factory WTFM patches rendered, loudest peak %.2f\n", n, worst);
}
