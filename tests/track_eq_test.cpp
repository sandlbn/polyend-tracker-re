// Per-track 4-band EQ (audio/track_eq.h): parameter helpers, filter accuracy,
// stability, and the integration into Voice (mono and stereo) and AudioEngine.
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include "audio/engine.h"
#include "audio/track_eq.h"
#include "test.h"

using namespace ot;
using namespace ot::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = static_cast<double>(ot::audio::kSampleRate);

EqBand band(uint8_t type, uint16_t hz, int16_t gain10, uint16_t q100, bool on = true) {
    EqBand b;
    b.type = type;
    b.freqHz = hz;
    b.gainDb10 = gain10;
    b.q100 = q100;
    b.on = on;
    return b;
}

TrackEqParams one(const EqBand& b) {
    TrackEqParams p;
    p.band[0] = b;
    return p;
}

// Steady-state gain in dB of a sine at `freq` through a TrackEq (RMS over a
// whole number of cycles at the end of a long run).
double measureDb(const TrackEqParams& p, double freq) {
    TrackEq eq;
    eq.set(p);
    const int warm = 60000;
    const double cycles = std::floor(freq * 32768.0 / kFs);   // whole cycles in ~32768 samples
    const int win = static_cast<int>(std::lround(cycles * kFs / freq));
    std::vector<float> x(static_cast<size_t>(warm + win));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * freq * static_cast<double>(i) / kFs));
    eq.process(x.data(), static_cast<uint32_t>(x.size()));
    double ss = 0;
    for (int i = warm; i < warm + win; ++i) ss += static_cast<double>(x[static_cast<size_t>(i)]) * static_cast<double>(x[static_cast<size_t>(i)]);
    const double rms = std::sqrt(ss / win);
    return 20.0 * std::log10(rms / (0.25 / std::sqrt(2.0)));
}

}  // namespace

// --- stored units, defaults, steps -------------------------------------------

TEST(eq_defaults_and_sanitize) {
    TrackEqSet s;
    for (int t = 0; t < kVoices; ++t)
        for (int i = 0; i < kEqBands; ++i) {
            CHECK(!s.track[t].band[i].on);
            CHECK_EQ(s.track[t].band[i].gainDb10, 0);
        }
    CHECK_EQ(s.track[3].band[0].type, kEqLowCut);
    CHECK_EQ(s.track[3].band[1].type, kEqPeak);
    CHECK_EQ(s.track[3].band[3].type, kEqHighCut);
    CHECK_EQ(s.track[0].band[3].freqHz, 12000);
    CHECK_EQ(s.track[0].band[0].freqHz, 80);

    EqBand b = band(200, 5, 9999, 0);
    eqSanitize(b);
    CHECK_EQ(b.type, kEqPeak);
    CHECK_EQ(b.freqHz, kEqFreqMin);
    CHECK_EQ(b.gainDb10, kEqGainMax);
    CHECK_EQ(b.q100, kEqQMin);
    b = band(kEqNotch, 60000, -9999, 9999);
    eqSanitize(b);
    CHECK_EQ(b.freqHz, kEqFreqMax);
    CHECK_EQ(b.gainDb10, -kEqGainMax);
    CHECK_EQ(b.q100, kEqQMax);
    CHECK(eqUsesGain(kEqPeak) && eqUsesGain(kEqLowShelf) && eqUsesGain(kEqHighShelf));
    CHECK(!eqUsesGain(kEqLowCut) && !eqUsesGain(kEqHighCut) && !eqUsesGain(kEqNotch));
    CHECK_STR(eqTypeName(kEqLowCut), "Low Cut");
}

TEST(eq_steps) {
    CHECK_EQ(eqStepFreq(1000, 1, false), 1029);
    CHECK_EQ(eqStepFreq(1000, -1, false), 972);
    CHECK_EQ(eqStepFreq(1000, 1, true), 1122);
    CHECK_EQ(eqStepFreq(1000, 0, true), 1000);
    // Always at least one Hz, never past the limits.
    CHECK_EQ(eqStepFreq(20, 1, false), 21);
    CHECK_EQ(eqStepFreq(20, -1, true), 20);
    CHECK_EQ(eqStepFreq(20000, 5, true), 20000);
    // One octave up in 24 fine detents, within rounding.
    uint16_t f = 440;
    for (int i = 0; i < 24; ++i) f = eqStepFreq(f, 1, false);
    CHECK(f >= 876 && f <= 884);
    // A huge spin is bounded, and monotonic.
    CHECK(eqStepFreq(100, 1000, false) > eqStepFreq(100, 10, false));

    CHECK_EQ(eqStepGain(0, 1, false), 5);
    CHECK_EQ(eqStepGain(0, -2, false), -10);
    CHECK_EQ(eqStepGain(0, 1, true), 30);
    CHECK_EQ(eqStepGain(140, 1, true), kEqGainMax);
    CHECK_EQ(eqStepGain(-140, -1, true), -kEqGainMax);

    CHECK_EQ(eqStepQ(71, 1, false), 75);
    CHECK_EQ(eqStepQ(71, 1, true), 107);
    CHECK_EQ(eqStepQ(10, -1, false), kEqQMin);
    CHECK_EQ(eqStepQ(1800, 1, true), kEqQMax);
    CHECK_EQ(eqStepQ(10, 1, false), 11);   // at least one unit

    CHECK_EQ(eqStepType(kEqPeak, -1), kEqNotch);
    CHECK_EQ(eqStepType(kEqNotch, 1), kEqPeak);
    CHECK_EQ(eqStepType(kEqPeak, 2), kEqHighShelf);
}

// --- analytic response (RBJ cookbook values) ---------------------------------

TEST(eq_response_matches_cookbook) {
    // Bell: exactly the set gain at the centre, flat far away.
    TrackEqParams p = one(band(kEqPeak, 1000, 120, 100));
    CHECK_NEAR(eqResponseDb(p, 1000.0f), 12.0, 0.02);
    CHECK_NEAR(eqResponseDb(p, 20.0f), 0.0, 0.05);
    CHECK_NEAR(eqResponseDb(p, 20000.0f), 0.0, 0.2);
    p = one(band(kEqPeak, 3000, -90, 200));
    CHECK_NEAR(eqResponseDb(p, 3000.0f), -9.0, 0.02);
    // Shelves: the set gain well inside the shelf, flat on the other side.
    p = one(band(kEqLowShelf, 200, 100, 71));
    CHECK_NEAR(eqResponseDb(p, 20.0f), 10.0, 0.1);
    CHECK_NEAR(eqResponseDb(p, 200.0f), 5.0, 0.1);   // half the gain at the corner
    CHECK_NEAR(eqResponseDb(p, 10000.0f), 0.0, 0.1);
    p = one(band(kEqHighShelf, 5000, -60, 71));
    CHECK_NEAR(eqResponseDb(p, 20000.0f), -6.0, 0.3);
    CHECK_NEAR(eqResponseDb(p, 5000.0f), -3.0, 0.1);
    CHECK_NEAR(eqResponseDb(p, 100.0f), 0.0, 0.1);
    // Cuts: -3 dB at the corner for Q = 0.707, 12 dB per octave.
    p = one(band(kEqLowCut, 1000, 0, 71));
    CHECK_NEAR(eqResponseDb(p, 1000.0f), -3.0, 0.1);
    CHECK_NEAR(eqResponseDb(p, 500.0f), -12.0, 0.8);
    CHECK_NEAR(eqResponseDb(p, 10000.0f), 0.0, 0.05);
    p = one(band(kEqHighCut, 1000, 0, 71));
    CHECK_NEAR(eqResponseDb(p, 1000.0f), -3.0, 0.1);
    CHECK_NEAR(eqResponseDb(p, 2000.0f), -12.0, 0.8);
    CHECK_NEAR(eqResponseDb(p, 50.0f), 0.0, 0.05);
    // Notch: very deep at the centre, flat away from it.
    p = one(band(kEqNotch, 1000, 0, 100));
    CHECK(eqResponseDb(p, 1000.0f) < -40.0f);
    CHECK_NEAR(eqResponseDb(p, 100.0f), 0.0, 0.2);
    // Cut and notch ignore the gain field.
    p = one(band(kEqLowCut, 1000, 150, 71));
    CHECK_NEAR(eqResponseDb(p, 1000.0f), -3.0, 0.1);
    // Disabled and neutral bands do nothing; bands add in dB.
    p = one(band(kEqPeak, 1000, 120, 100, false));
    CHECK_EQ(eqResponseDb(p, 1000.0f), 0.0f);
    p = one(band(kEqPeak, 1000, 0, 100));
    CHECK_EQ(eqResponseDb(p, 1000.0f), 0.0f);
    p.band[0] = band(kEqPeak, 1000, 60, 100);
    p.band[1] = band(kEqPeak, 1000, 60, 100);
    CHECK_NEAR(eqResponseDb(p, 1000.0f), 12.0, 0.05);
    // 20 kHz stays finite and stable (limited to 0.45 fs internally).
    float c[5];
    eqCoefficients(band(kEqHighCut, 20000, 0, 1800), c);
    for (float v : c) CHECK(std::isfinite(v));
}

// --- the filter itself --------------------------------------------------------

TEST(eq_process_matches_response) {
    const EqBand cases[] = {
        band(kEqPeak, 1000, 120, 100),   band(kEqPeak, 2500, -150, 300), band(kEqLowShelf, 150, 90, 71),
        band(kEqHighShelf, 6000, -90, 71), band(kEqLowCut, 80, 0, 71),   band(kEqHighCut, 9000, 0, 71),
        band(kEqNotch, 1200, 0, 300),
    };
    const double freqs[] = {40, 100, 300, 1000, 1500, 4000, 9000, 15000};
    for (const EqBand& c : cases) {
        const TrackEqParams p = one(c);
        for (double hz : freqs) {
            const double want = static_cast<double>(eqResponseDb(p, static_cast<float>(hz)));
            if (want < -45.0) continue;   // beyond the measurable floor of the RMS estimate
            CHECK_NEAR(measureDb(p, hz), want, 0.2);
        }
    }
}

TEST(eq_cascade_adds_in_db) {
    TrackEqParams p;
    p.band[0] = band(kEqPeak, 800, 60, 100);
    p.band[1] = band(kEqPeak, 800, 60, 100);
    p.band[2] = band(kEqPeak, 800, 60, 100);
    CHECK_NEAR(measureDb(p, 800.0), 18.0, 0.2);
    // Band order does not matter (linear filters).
    TrackEqParams q;
    q.band[0] = band(kEqHighCut, 3000, 0, 71);
    q.band[3] = band(kEqLowShelf, 200, 80, 71);
    TrackEqParams r;
    r.band[3] = q.band[0];
    r.band[0] = q.band[3];
    CHECK_NEAR(measureDb(q, 500.0), measureDb(r, 500.0), 0.02);
}

TEST(eq_blocks_equal_one_call) {
    TrackEqParams p;
    p.band[0] = band(kEqLowShelf, 120, 80, 71);
    p.band[1] = band(kEqPeak, 900, -70, 150);
    p.band[3] = band(kEqHighCut, 12000, 0, 71);
    std::mt19937 rng(5);
    std::vector<float> a(128 * 50);
    for (auto& v : a) v = static_cast<float>(static_cast<int>(rng() % 20001) - 10000) / 40000.0f;
    std::vector<float> b = a;
    TrackEq e1, e2;
    e1.set(p);
    e2.set(p);
    e1.process(a.data(), static_cast<uint32_t>(a.size()));
    for (size_t i = 0; i < b.size(); i += 128) e2.process(b.data() + i, 128);
    CHECK(a == b);
}

TEST(eq_stable_at_extremes) {
    // Lowest frequency, highest Q and full gain on every band, loud noise.
    TrackEqParams p;
    p.band[0] = band(kEqLowShelf, 20, 150, 1800);
    p.band[1] = band(kEqPeak, 20, 150, 1800);
    p.band[2] = band(kEqPeak, 19999, -150, 1800);
    p.band[3] = band(kEqHighShelf, 20000, 150, 1800);
    TrackEq eq;
    eq.set(p);
    std::mt19937 rng(11);
    std::vector<float> x(128);
    float worst = 0;
    for (int blk = 0; blk < 4000; ++blk) {
        for (auto& v : x) v = static_cast<float>(static_cast<int>(rng() % 20001) - 10000) / 10000.0f;
        eq.process(x.data(), 128);
        for (float v : x) {
            CHECK(std::isfinite(v));
            worst = std::fmax(worst, std::fabs(v));
        }
    }
    CHECK(worst < 1000.0f);
    // Cuts and notch at the same extremes.
    TrackEqParams q;
    q.band[0] = band(kEqLowCut, 20, 0, 1800);
    q.band[1] = band(kEqHighCut, 20000, 0, 1800);
    q.band[2] = band(kEqNotch, 20, 0, 1800);
    q.band[3] = band(kEqNotch, 20000, 0, 1800);
    eq.set(q);
    eq.reset();
    for (int blk = 0; blk < 4000; ++blk) {
        for (auto& v : x) v = static_cast<float>(static_cast<int>(rng() % 20001) - 10000) / 10000.0f;
        eq.process(x.data(), 128);
        for (float v : x) CHECK(std::isfinite(v) && std::fabs(v) < 1000.0f);
    }
}

TEST(eq_tail_decays_to_exact_zero) {
    TrackEq eq;
    eq.set(one(band(kEqLowShelf, 20, 150, 71)));
    std::vector<float> x(128, 0.0f);
    x[0] = 1.0f;
    eq.process(x.data(), 128);
    bool zero = false;
    for (int blk = 0; blk < 4000 && !zero; ++blk) {
        std::fill(x.begin(), x.end(), 0.0f);
        eq.process(x.data(), 128);
        zero = true;
        for (float v : x) zero = zero && v == 0.0f;
    }
    CHECK(zero);   // the denormal guard lets the tail end
}

TEST(eq_set_semantics) {
    TrackEq eq;
    CHECK(!eq.active());
    TrackEqParams p = one(band(kEqPeak, 1000, 0, 100));
    eq.set(p);
    CHECK(!eq.active());   // a 0 dB bell does nothing, costs nothing
    p.band[0].gainDb10 = 30;
    eq.set(p);
    CHECK(eq.active());
    p.band[0].on = false;
    eq.set(p);
    CHECK(!eq.active());
    // Neutral and off bands leave the signal bit-exact.
    std::vector<float> x(256), y;
    for (size_t i = 0; i < x.size(); ++i) x[i] = std::sin(static_cast<float>(i) * 0.1f);
    y = x;
    eq.process(y.data(), 256);
    CHECK(x == y);
    // Cut / notch are active even with gain 0.
    p = one(band(kEqLowCut, 100, 0, 71));
    eq.set(p);
    CHECK(eq.active());
    // Switching a band off clears its state: re-enabling starts clean.
    TrackEqParams on = one(band(kEqPeak, 500, 150, 400));
    eq.set(on);
    std::vector<float> burst(128, 0.5f);
    eq.process(burst.data(), 128);
    TrackEqParams off = on;
    off.band[0].on = false;
    eq.set(off);
    eq.set(on);
    std::vector<float> a(128, 0.0f);
    eq.process(a.data(), 128);
    for (float v : a) CHECK_EQ(v, 0.0f);
    // Out-of-range stored values are clamped instead of trusted.
    TrackEqParams bad = one(band(77, 0, 30000, 0));
    eq.set(bad);
    std::vector<float> z(128, 0.1f);
    eq.process(z.data(), 128);
    for (float v : z) CHECK(std::isfinite(v));
}

// --- integration: Voice (mono and stereo) and AudioEngine ---------------------

namespace {

std::vector<int16_t> tone(uint32_t len, float hz, float amp) {
    std::vector<int16_t> d(len);
    for (uint32_t i = 0; i < len; ++i)
        d[i] = static_cast<int16_t>(amp * 32767.0f * std::sin(2.0f * static_cast<float>(kPi) * hz * static_cast<float>(i) / 44100.0f));
    return d;
}

double rmsOf(const float* x, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    return std::sqrt(s / static_cast<double>(n));
}

Instrument plainIns(uint32_t len) {
    Instrument ins;
    ins.lengthFrames = len;
    ins.playMode = static_cast<uint8_t>(PlayMode::OneShot);
    ins.env[kAutoVolume].enabled = 0;   // flat gate
    return ins;
}

struct VoiceRun {
    std::vector<float> amp, l, r, ds;
};

// Renders `blocks` blocks of a bare voice with an optional EQ. right ==
// nullptr: mono note. fused: pass the dry-bus pointers and check the voice
// sums o.l / o.r into them.
VoiceRun runVoice(const Instrument& ins, const int16_t* left, const int16_t* right, const TrackEqParams* eq,
                  int blocks, bool fused = false) {
    initTables();
    std::unique_ptr<Voice> v(new Voice());
    v->reset();
    if (eq) v->setEq(*eq);
    v->noteOn(&ins, left, ins.lengthFrames, 60, -1, 0, 0, 0, 0, right);
    VoiceControl c;
    c.antiAliasing = false;
    VoiceRun out;
    VoiceOut vo;
    for (int b = 0; b < blocks; ++b) {
        float ds[kBlock] = {}, rs[kBlock] = {}, ml[kBlock] = {}, mr[kBlock] = {};
        v->control(c);
        v->render(c, vo, ds, rs, fused ? ml : nullptr, fused ? mr : nullptr, true);
        out.amp.insert(out.amp.end(), vo.amp, vo.amp + kBlock);
        out.l.insert(out.l.end(), vo.l, vo.l + kBlock);
        out.r.insert(out.r.end(), vo.r, vo.r + kBlock);
        out.ds.insert(out.ds.end(), ds, ds + kBlock);
        if (fused)
            for (uint32_t i = 0; i < kBlock; ++i) {
                CHECK(ml[i] == vo.l[i]);
                CHECK(mr[i] == vo.r[i]);
            }
    }
    return out;
}

}  // namespace

TEST(eq_in_voice_mono) {
    const auto data = tone(88200, 1000.0f, 0.05f);
    const Instrument ins = plainIns(static_cast<uint32_t>(data.size()));
    const auto plain = runVoice(ins, data.data(), nullptr, nullptr, 300, true);
    // A neutral band leaves the voice bit-identical.
    const TrackEqParams neutral = one(band(kEqPeak, 1000, 0, 100));
    CHECK(runVoice(ins, data.data(), nullptr, &neutral, 300, true).amp == plain.amp);
    // +12 dB bell on the tone: ~4x amplitude once settled (still summing into the dry bus).
    const TrackEqParams boost = one(band(kEqPeak, 1000, 120, 100));
    const auto eq = runVoice(ins, data.data(), nullptr, &boost, 300, true);
    CHECK_NEAR(rmsOf(eq.amp.data() + 20000, 8192) / rmsOf(plain.amp.data() + 20000, 8192), 3.98, 0.12);
    // A deep notch on the tone removes it.
    const TrackEqParams notch = one(band(kEqNotch, 1000, 0, 100));
    const auto n = runVoice(ins, data.data(), nullptr, &notch, 300, true);
    CHECK(rmsOf(n.amp.data() + 20000, 8192) < 0.02 * rmsOf(plain.amp.data() + 20000, 8192));
}

TEST(eq_in_voice_stereo_channels_independent) {
    // Left 1 kHz, right 3 kHz; a narrow notch at 1 kHz removes only the left tone.
    const auto left = tone(30000, 1000.0f, 0.05f);
    const auto right = tone(30000, 3000.0f, 0.05f);
    std::vector<int16_t> planar(left);
    planar.insert(planar.end(), right.begin(), right.end());
    const Instrument ins = plainIns(30000);
    const auto plain = runVoice(ins, planar.data(), planar.data() + 30000, nullptr, 200, true);
    const TrackEqParams notch = one(band(kEqNotch, 1000, 0, 400));
    const auto eq = runVoice(ins, planar.data(), planar.data() + 30000, &notch, 200, true);
    CHECK(rmsOf(plain.l.data() + 8000, 8192) > 0.0);
    CHECK(rmsOf(eq.l.data() + 8000, 8192) < 0.03 * rmsOf(plain.l.data() + 8000, 8192));
    CHECK_NEAR(rmsOf(eq.r.data() + 8000, 8192) / rmsOf(plain.r.data() + 8000, 8192), 1.0, 0.02);
}

TEST(eq_in_voice_stereo_matches_mono_per_channel) {
    // At centre pan each stereo channel equals the mono rendering of that
    // channel alone, with the EQ too (two filter states, same settings).
    std::vector<int16_t> left(20000), right(20000);
    std::mt19937 rng(3);
    for (auto& s : left) s = static_cast<int16_t>(static_cast<int>(rng() % 12001) - 6000);
    for (auto& s : right) s = static_cast<int16_t>(static_cast<int>(rng() % 12001) - 6000);
    std::vector<int16_t> planar(left);
    planar.insert(planar.end(), right.begin(), right.end());
    const Instrument ins = plainIns(20000);
    TrackEqParams p;
    p.band[0] = band(kEqLowShelf, 150, 90, 71);
    p.band[1] = band(kEqPeak, 1200, -80, 200);
    p.band[3] = band(kEqHighCut, 9000, 0, 71);
    const auto st = runVoice(ins, planar.data(), planar.data() + 20000, &p, 120, true);
    const auto ml = runVoice(ins, left.data(), nullptr, &p, 120, false);
    const auto mr = runVoice(ins, right.data(), nullptr, &p, 120, false);
    CHECK(st.l == ml.l);
    CHECK(st.r == mr.r);
}

TEST(eq_before_the_sends) {
    // The delay send carries the EQ'd signal (insert effect, not post-send).
    const auto data = tone(88200, 1000.0f, 0.05f);
    Instrument ins = plainIns(static_cast<uint32_t>(data.size()));
    ins.delaySend = 100;
    const auto plain = runVoice(ins, data.data(), nullptr, nullptr, 300);
    const TrackEqParams boost = one(band(kEqPeak, 1000, 120, 100));
    const auto eq = runVoice(ins, data.data(), nullptr, &boost, 300);
    CHECK(rmsOf(plain.ds.data() + 20000, 8192) > 0.0);
    CHECK_NEAR(rmsOf(eq.ds.data() + 20000, 8192) / rmsOf(plain.ds.data() + 20000, 8192), 3.98, 0.12);
    // And the same for a stereo note (sends take the channel mean).
    std::vector<int16_t> planar(data);
    planar.insert(planar.end(), data.begin(), data.end());
    const auto sp = runVoice(ins, planar.data(), planar.data() + data.size(), nullptr, 300);
    const auto se = runVoice(ins, planar.data(), planar.data() + data.size(), &boost, 300);
    CHECK(rmsOf(sp.ds.data() + 20000, 8192) > 0.0);
    CHECK_NEAR(rmsOf(se.ds.data() + 20000, 8192) / rmsOf(sp.ds.data() + 20000, 8192), 3.98, 0.12);
}

TEST(eq_state_clears_when_the_voice_goes_silent) {
    // A resonant EQ must not carry its tail into the next note.
    std::vector<int16_t> burst(2000, 12000);
    const Instrument ins = plainIns(2000);
    TrackEqParams p = one(band(kEqPeak, 200, 150, 1800));
    initTables();
    std::unique_ptr<Voice> v(new Voice());
    v->reset();
    v->setEq(p);
    VoiceControl c;
    c.antiAliasing = false;
    VoiceOut vo;
    float ds[kBlock] = {}, rs[kBlock] = {};
    v->noteOn(&ins, burst.data(), 2000, 60, -1, 0, 0, 0, 0);
    bool silent = false;
    for (int b = 0; b < 4000 && !silent; ++b) {
        v->control(c);
        silent = !v->render(c, vo, ds, rs);
    }
    CHECK(silent);
    // The next note starts from a clean filter: its first block equals a fresh voice's.
    v->noteOn(&ins, burst.data(), 2000, 60, -1, 0, 0, 0, 0);
    v->control(c);
    CHECK(v->render(c, vo, ds, rs));
    std::vector<float> again(vo.amp, vo.amp + kBlock);
    const auto fresh = runVoice(ins, burst.data(), nullptr, &p, 1);
    CHECK(std::equal(again.begin(), again.end(), fresh.amp.begin()));
}

TEST(eq_in_engine) {
    const auto data = tone(88200, 1000.0f, 0.05f);
    auto render = [&](const TrackEqParams* eq, int track) {
        std::vector<int16_t> fx(AudioEngine::kFxMemorySamples);
        std::vector<int16_t> pool(4u << 20);
        std::unique_ptr<Instrument[]> ins(new Instrument[kSampleInstrumentCount]);
        std::unique_ptr<AudioEngine> e(new AudioEngine());
        CHECK(e->init(fx.data(), fx.size(), pool.data(), pool.size(), ins.get()));
        ins[0].lengthFrames = static_cast<uint32_t>(data.size());
        ins[0].playMode = static_cast<uint8_t>(PlayMode::OneShot);
        ins[0].env[kAutoVolume].enabled = 0;
        CHECK(e->bank().store(0, data.data(), static_cast<uint32_t>(data.size()), false));
        if (eq) e->settings().trackEq.track[track] = *eq;
        VoiceNoteOn n;
        n.instrument = 0;
        n.note = 60;
        n.volume = -1;
        e->voiceNoteOn(0, n);
        std::vector<int16_t> out;
        int16_t blk[kBlock * 2];
        for (int b = 0; b < 300; ++b) {
            e->renderLive(blk);
            out.insert(out.end(), blk, blk + kBlock * 2);
        }
        return out;
    };
    auto rmsI = [](const std::vector<int16_t>& v, size_t from, size_t n) {
        double s = 0;
        for (size_t i = from; i < from + n; ++i) s += static_cast<double>(v[i]) * static_cast<double>(v[i]);
        return std::sqrt(s / static_cast<double>(n));
    };
    const auto plain = render(nullptr, 0);
    // The EQ of track 1 (voice 0) boosts it; the EQ of another track does not.
    const TrackEqParams boost = one(band(kEqPeak, 1000, 120, 100));
    const auto on0 = render(&boost, 0);
    const auto on5 = render(&boost, 5);
    CHECK(on5 == plain);
    CHECK_NEAR(rmsI(on0, 2 * 20000, 2 * 8192) / rmsI(plain, 2 * 20000, 2 * 8192), 3.98, 0.15);
    // A neutral EQ is bit-identical to none (the fused fast paths stay in use).
    const TrackEqParams neutral = one(band(kEqPeak, 1000, 0, 100));
    CHECK(render(&neutral, 0) == plain);
}

// --- 48 dB/oct cuts on bands 1 and 4 -------------------------------------------

TEST(eq_slope_48_response) {
    EqBand lc = band(kEqLowCut, 1000, 0, 71);
    lc.slope = 1;
    TrackEqParams p = one(lc);   // band index 0: allowed
    // 8th-order Butterworth: -3 dB at the corner, about -48 dB an octave down, flat above.
    CHECK_NEAR(eqResponseDb(p, 1000.0f), -3.0, 0.1);
    CHECK_NEAR(eqResponseDb(p, 500.0f), -48.2, 0.8);
    CHECK_NEAR(eqResponseDb(p, 666.7f), -28.2, 0.8);   // 10 log10(1 + 1.5^16)
    CHECK_NEAR(eqResponseDb(p, 10000.0f), 0.0, 0.05);
    // 12 dB/oct for comparison: only about -12 dB an octave down.
    lc.slope = 0;
    CHECK_NEAR(eqResponseDb(one(lc), 500.0f), -12.0, 0.8);
    // High cut on band index 3.
    EqBand hc = band(kEqHighCut, 2000, 0, 71);
    hc.slope = 1;
    TrackEqParams q;
    q.band[3] = hc;
    CHECK_NEAR(eqResponseDb(q, 2000.0f), -3.0, 0.1);
    CHECK_NEAR(eqResponseDb(q, 4000.0f), -48.2, 2.0);   // a little steeper: bilinear warping near Nyquist
    CHECK_NEAR(eqResponseDb(q, 100.0f), 0.0, 0.05);
    // The option does not exist on bands 2 and 3 (index 1, 2) or on other types.
    EqBand lc2 = band(kEqLowCut, 1000, 0, 71);
    lc2.slope = 1;
    TrackEqParams r;
    r.band[1] = lc2;
    CHECK_NEAR(eqResponseDb(r, 500.0f), -12.0, 0.8);
    EqBand bell = band(kEqPeak, 1000, 60, 100);
    bell.slope = 1;
    eqSanitize(bell);
    CHECK_EQ(bell.slope, 0);
    // Q still shapes it: a higher Q adds a resonant bump near the corner.
    EqBand res = band(kEqLowCut, 1000, 0, 300);
    res.slope = 1;
    CHECK(eqResponseDb(one(res), 1100.0f) > 0.5f);
    // Stored units stay in range.
    CHECK_EQ(eqStageCount(lc), 1);
    lc.slope = 1;
    CHECK_EQ(eqStageCount(lc), 4);
    CHECK_EQ(eqStageCount(bell), 1);
}

TEST(eq_slope_48_process_matches_response) {
    EqBand lc = band(kEqLowCut, 800, 0, 71);
    lc.slope = 1;
    EqBand hc = band(kEqHighCut, 6000, 0, 71);
    hc.slope = 1;
    TrackEqParams p;
    p.band[0] = lc;
    p.band[3] = hc;
    for (double hz : {300.0, 530.0, 800.0, 1500.0, 3000.0, 6000.0, 9000.0}) {
        const double want = static_cast<double>(eqResponseDb(p, static_cast<float>(hz)));
        if (want < -45.0) continue;
        CHECK_NEAR(measureDb(p, hz), want, 0.25);
    }
    // Four stages are live per slope band; switching back to 12 dB frees them and clears state.
    TrackEq eq;
    eq.set(p);
    CHECK(eq.active());
    TrackEqParams off;
    off.band[0] = lc;
    off.band[0].slope = 0;
    off.band[0].on = false;
    off.band[3] = hc;
    off.band[3].on = false;
    eq.set(off);
    CHECK(!eq.active());
}

TEST(eq_slope_48_stable_and_flushes) {
    EqBand lc = band(kEqLowCut, 20, 0, 1800);
    lc.slope = 1;
    EqBand hc = band(kEqHighCut, 20000, 0, 1800);
    hc.slope = 1;
    TrackEqParams p;
    p.band[0] = lc;
    p.band[3] = hc;
    TrackEq eq;
    eq.set(p);
    std::mt19937 rng(9);
    std::vector<float> x(128);
    for (int blk = 0; blk < 3000; ++blk) {
        for (auto& v : x) v = static_cast<float>(static_cast<int>(rng() % 20001) - 10000) / 10000.0f;
        eq.process(x.data(), 128);
        for (float v : x) CHECK(std::isfinite(v) && std::fabs(v) < 1000.0f);
    }
    // A very low Q (0.10) at a low corner is still stable.
    lc.q100 = 10;
    lc.freqHz = 20;
    p.band[0] = lc;
    eq.set(p);
    for (int blk = 0; blk < 2000; ++blk) {
        for (auto& v : x) v = static_cast<float>(static_cast<int>(rng() % 20001) - 10000) / 10000.0f;
        eq.process(x.data(), 128);
        for (float v : x) CHECK(std::isfinite(v) && std::fabs(v) < 1000.0f);
    }
}
