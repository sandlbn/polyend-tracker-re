// Sample Editor effect processing per spec 18 §2 (stock 1.9.2): one test per
// effect with impulse / sine / DC responses, output lengths and saturation.
#include <cmath>
#include <cstdlib>
#include <vector>

#include "test.h"

#include "app/sample_fx.h"

using namespace ot;

namespace {

struct Run {
    std::vector<int16_t> out;
    uint32_t len = 0;
};

Run run(int fx, const float* p, const std::vector<int16_t>& in) {
    Run r;
    const uint32_t m = sfx::outputFrames(fx, p, uint32_t(in.size()));
    r.out.assign(m + 8, int16_t(0x5a5a));
    r.len = sfx::process(fx, p, in.data(), uint32_t(in.size()), r.out.data(), m);
    r.out.resize(r.len);
    return r;
}

std::vector<int16_t> sine(uint32_t n, double amp, double hz) {
    std::vector<int16_t> s(n);
    for (uint32_t i = 0; i < n; ++i) s[i] = int16_t(amp * std::sin(2.0 * 3.14159265358979 * hz * double(i) / 44100.0));
    return s;
}

std::vector<int16_t> impulse(uint32_t n, int16_t v, uint32_t at = 0) {
    std::vector<int16_t> s(n, 0);
    s[at] = v;
    return s;
}

int peakOf(const std::vector<int16_t>& s, uint32_t from = 0, uint32_t to = 0xFFFFFFFFu) {
    int pk = 0;
    for (uint32_t i = from; i < s.size() && i < to; ++i) pk = std::abs(s[i]) > pk ? std::abs(s[i]) : pk;
    return pk;
}

void params(int fx, float* p) { sfx::defaults(fx, p); }

}  // namespace

TEST(stock_normalizer) {
    float p[9];
    params(sfx::kNormalizer, p);
    const std::vector<int16_t> in = {1000, -333, 0, 500};
    Run r = run(sfx::kNormalizer, p, in);
    CHECK_EQ(r.len, 4u);
    CHECK_EQ(r.out[0], 32767);
    CHECK_EQ(r.out[1], -10911);   // trunc(-333 * 32.767) = trunc(-10911.4)
    CHECK_EQ(r.out[3], 16383);    // trunc(16383.5)
    p[0] = 50;
    r = run(sfx::kNormalizer, p, in);
    CHECK_EQ(r.out[0], 16383);
    // P = 0 is guarded (stock would divide by zero).
    r = run(sfx::kNormalizer, p, std::vector<int16_t>(100, 0));
    CHECK_EQ(peakOf(r.out), 0);
}

TEST(stock_crop_reverse) {
    float p[9];
    params(sfx::kCrop, p);
    const auto in = sine(1000, 9000, 300);
    Run r = run(sfx::kCrop, p, in);
    CHECK(r.out == in);
    r = run(sfx::kReverse, p, in);
    CHECK_EQ(r.len, 1000u);
    for (uint32_t i = 0; i < 1000; ++i) CHECK_EQ(r.out[i], in[999 - i]);
}

TEST(stock_amplifier_trunc_and_saturation) {
    float p[9];
    params(sfx::kAmplifier, p);
    p[0] = 0.5f;
    Run r = run(sfx::kAmplifier, p, {-3, 3, 7, -7});
    CHECK_EQ(r.out[0], -1);   // trunc toward zero
    CHECK_EQ(r.out[1], 1);
    CHECK_EQ(r.out[2], 3);
    CHECK_EQ(r.out[3], -3);
    p[0] = 5.0f;
    r = run(sfx::kAmplifier, p, {10000, -10000, 100});
    CHECK_EQ(r.out[0], 32767);
    CHECK_EQ(r.out[1], -32768);
    CHECK_EQ(r.out[2], 500);
}

TEST(stock_overdrive_types) {
    float p[9];
    params(sfx::kOverdrive, p);
    CHECK_NEAR(p[0], 1.0, 1e-6);
    // Type 0: y = 2s below 1/3 FS, (3 - (2 - 3a)^2)/3 up to 2/3 FS, FS above.
    Run r = run(sfx::kOverdrive, p, {8191, -8191, 16384, 30000, -30000, 0});
    CHECK_EQ(r.out[0], 16382);
    CHECK_EQ(r.out[1], -16382);
    {
        const double a = 16384.0 / 32767.0;
        CHECK_EQ(int(r.out[2]), int((3.0 - (2.0 - 3.0 * a) * (2.0 - 3.0 * a)) / 3.0 * 32767.0));
    }
    CHECK_EQ(r.out[3], 32767);
    CHECK_EQ(r.out[4], -32767);
    CHECK_EQ(r.out[5], 0);
    // Type 1: y = sign(x)(1 - e^(-4g|x|/32767)) 32767.
    p[1] = 1;
    r = run(sfx::kOverdrive, p, {32767, -16384});
    CHECK_EQ(r.out[0], int16_t((1.0 - std::exp(-4.0)) * 32767.0));
    CHECK_EQ(r.out[1], int16_t(-(1.0 - std::exp(-4.0 * 16384.0 / 32767.0)) * 32767.0));
    // Type 2: integer step trunc(10g|x|/32767).
    p[1] = 2;
    r = run(sfx::kOverdrive, p, {3276, 3277, -32767, 6554});
    CHECK_EQ(r.out[0], 0);
    CHECK_EQ(r.out[1], 16383);
    CHECK_EQ(r.out[2], -29788);   // -(1 - 1/11) * 32767
    CHECK_EQ(r.out[3], 21844);    // (1 - 1/3) * 32767
    // Gain scales the drive: g = 0.5 halves k.
    r = run(sfx::kOverdrive, p, {6553});
    CHECK_EQ(r.out[0], 16383);
    p[0] = 0.5f;
    r = run(sfx::kOverdrive, p, {6553});
    CHECK_EQ(r.out[0], 0);
}

TEST(stock_delay_multitap_and_length) {
    float p[9];
    params(sfx::kDelay, p);
    // Defaults f = 0.95, 50 ms: D = 2205, K = 90 (0.95^90 <= 0.01 < 0.95^89).
    CHECK_EQ(sfx::outputFrames(sfx::kDelay, p, 1000), 1000u + 90u * 2205u);
    p[0] = 0.5f;
    p[1] = 1.0f;   // D = 44; 0.5^7 = 0.0078 -> K = 7
    CHECK_EQ(sfx::outputFrames(sfx::kDelay, p, 100), 100u + 7u * 44u);
    Run r = run(sfx::kDelay, p, impulse(100, 10000, 0));
    CHECK_EQ(r.len, 408u);
    for (uint32_t i = 0; i < r.len; ++i) {
        if (i % 44 == 0 && i / 44 <= 7) CHECK_EQ(int(r.out[i]), int(10000.0 * std::pow(0.5, double(i / 44))));
        else CHECK_EQ(r.out[i], 0);
    }
    // The last tap of the last input sample lands on the final output sample.
    r = run(sfx::kDelay, p, impulse(100, 10000, 99));
    CHECK_EQ(int(r.out[407]), int(10000.0 * std::pow(0.5, 7.0)));
    // Not recirculating: f = 0.01 -> K = 1 (one echo of 1 %).
    p[0] = 0.01f;
    CHECK_EQ(sfx::outputFrames(sfx::kDelay, p, 100), 144u);
    // Saturation: DC 30000 with f = 0.95 sums past full scale.
    p[0] = 0.95f;
    p[1] = 1.0f;
    r = run(sfx::kDelay, p, std::vector<int16_t>(2000, 30000));
    CHECK_EQ(r.out[1999], 32767);
    CHECK_EQ(r.out[0], 30000);
    p[1] = 5000.0f;   // tail beyond the 0x1E0000 limit is reported to the screen
    CHECK(sfx::outputFrames(sfx::kDelay, p, 1000) > sfx::kMaxFrames);
}

TEST(stock_bitcrusher_hold_and_bits) {
    float p[9];
    params(sfx::kBitcrusher, p);
    // R = 44100: H = round(44117.647 / 44100) = 1; 4 bits. Stock keeps the
    // top bits (floor, (x >> 12) * 4096); OpenTracker deliberately rounds to
    // the nearest step on the same grid (no hiss/DC on quiet material).
    const auto in = sine(2000, 20000, 440);
    Run r = run(sfx::kBitcrusher, p, in);
    for (uint32_t i = 0; i < 2000; ++i) CHECK_EQ(r.out[i], int16_t(((in[i] + 2048) >> 12) * 4096));
    // B = 16, R = 11025: H = 4, hold only.
    p[0] = 16;
    p[1] = 11025;
    r = run(sfx::kBitcrusher, p, in);
    for (uint32_t i = 0; i < 2000; ++i) CHECK_EQ(r.out[i], in[i - i % 4]);
    // R = 500: round(88.2) clamps to H = 64.
    p[1] = 500;
    r = run(sfx::kBitcrusher, p, in);
    for (uint32_t i = 0; i < 2000; ++i) CHECK_EQ(r.out[i], in[i - i % 64]);
    // B = 1: steps of 32768, rounded to nearest (stock: sign only, 0 or
    // -32768); the top step clamps to 32767.
    p[0] = 1;
    p[1] = 44100;
    r = run(sfx::kBitcrusher, p, {100, -100, 32767, -1, -20000, 20000});
    CHECK_EQ(r.out[0], 0);
    CHECK_EQ(r.out[1], 0);
    CHECK_EQ(r.out[2], 32767);
    CHECK_EQ(r.out[3], 0);
    CHECK_EQ(r.out[4], -32768);
    CHECK_EQ(r.out[5], 32767);
}

TEST(stock_chorus_taps) {
    float p[9];
    params(sfx::kChorus, p);
    const auto in = sine(1000, 9000, 300);
    // Voices 1: pass-through.
    Run r = run(sfx::kChorus, p, in);
    CHECK(r.out == in);
    // Length 128 -> L = 64; voices 2: taps 0 and 64/1 - 1 = 63.
    p[1] = 2;
    r = run(sfx::kChorus, p, impulse(300, 1001, 10));
    CHECK_EQ(r.len, 300u);   // tail not extended
    CHECK_EQ(r.out[10], 500);
    CHECK_EQ(r.out[73], 500);
    CHECK_EQ(peakOf(r.out, 0, 10) + peakOf(r.out, 11, 73) + peakOf(r.out, 74), 0);
    // Voices 3: step 64/2 - 1 = 31 (taps 0, 31, 62); integer mean truncates.
    p[1] = 3;
    r = run(sfx::kChorus, p, impulse(300, -100, 0));
    CHECK_EQ(r.out[0], -33);
    CHECK_EQ(r.out[31], -33);
    CHECK_EQ(r.out[62], -33);
    CHECK_EQ(r.out[93], 0);
    // DC passes at unity once the line is full.
    p[0] = 4096;
    p[1] = 8;
    r = run(sfx::kChorus, p, std::vector<int16_t>(5000, 7000));
    CHECK_EQ(r.out[4999], 7000);
}

TEST(stock_flanger) {
    float p[9];
    params(sfx::kFlanger, p);   // length 128 (L = 64), offset 50 % = 32, depth 1, 1 Hz
    Run r = run(sfx::kFlanger, p, impulse(400, 10000, 0));
    CHECK_EQ(r.len, 400u);
    CHECK_EQ(r.out[0], 5000);   // (x + tap) / 2
    // The echo sits 32 +- depth samples back.
    int echo = 0;
    for (uint32_t i = 30; i <= 34; ++i) echo += r.out[i];
    CHECK(echo >= 4990 && echo <= 5000);
    CHECK_EQ(peakOf(r.out, 1, 30) + peakOf(r.out, 35), 0);
    // DC is unity.
    r = run(sfx::kFlanger, p, std::vector<int16_t>(1000, 8000));
    CHECK(std::abs(r.out[999] - 8000) <= 1);
    // Offset 100 % clamps to L - 1.
    p[1] = 100;
    p[2] = 1;
    r = run(sfx::kFlanger, p, impulse(400, 10000, 0));
    CHECK_EQ(peakOf(r.out, 1, 60), 0);
    CHECK(peakOf(r.out, 60, 66) > 0);
}

TEST(stock_equalizer) {
    float p[9];
    params(sfx::kEqualizer, p);
    // All bands at 0 dB (linear 1.0): bypassed.
    const auto mid = sine(44100, 2000, 800);
    Run r = run(sfx::kEqualizer, p, mid);
    int dev = 0;
    for (uint32_t i = 0; i < mid.size(); ++i) dev = std::abs(r.out[i] - mid[i]) > dev ? std::abs(r.out[i] - mid[i]) : dev;
    CHECK(dev <= 1);
    // +12 dB on the mid band at 800 Hz: the 800 Hz sine gains ~3.98x.
    p[3] = 12;
    r = run(sfx::kEqualizer, p, mid);
    const int pk = peakOf(r.out, 22050);
    CHECK(std::abs(pk - 7962) < 120);
    // An 80 Hz sine is barely touched by the mid band (Q = 800/714 = 1.12).
    const auto low = sine(44100, 2000, 80);
    r = run(sfx::kEqualizer, p, low);
    CHECK(peakOf(r.out, 22050) < 2300);
    // -12 dB cut.
    p[3] = -12;
    r = run(sfx::kEqualizer, p, mid);
    CHECK(std::abs(peakOf(r.out, 22050) - 502) < 30);
    // Clipping to -1..1 before scaling.
    p[3] = 12;
    r = run(sfx::kEqualizer, p, sine(44100, 20000, 800));
    CHECK_EQ(peakOf(r.out, 22050), 32767);
}

TEST(stock_limiter) {
    float p[9];
    params(sfx::kLimiter, p);   // -6 dB, 3 ms, 300 ms
    // Below the threshold the gain stays exactly 1.
    const auto quiet = sine(22050, 10000, 440);
    Run r = run(sfx::kLimiter, p, quiet);
    CHECK(r.out == quiet);
    // A loud sine settles near the threshold (theta = 16422).
    const auto loud = sine(44100, 32000, 440);
    r = run(sfx::kLimiter, p, loud);
    const int pk = peakOf(r.out, 33075);
    CHECK(pk < 32000);
    CHECK(pk > 12000 && pk < 21000);
    // The gain only scales: sign and zero crossings are kept.
    for (uint32_t i = 0; i < loud.size(); ++i) CHECK((r.out[i] >= 0) == (loud[i] >= 0) || r.out[i] == 0);
}

TEST(stock_compressor) {
    float p[9];
    params(sfx::kCompressor, p);   // -12 dB, 4:1, 10 ms, 50 ms
    // Below the threshold: unchanged.
    const auto quiet = sine(22050, 3000, 440);
    Run r = run(sfx::kCompressor, p, quiet);
    CHECK(r.out == quiet);
    // 0 dBFS-ish sine: mean square 0.4768 -> L = -3.2 dB; gain
    // (-12 + 3.2) * 0.75 = -6.6 dB -> 0.468.
    const auto loud = sine(88200, 32000, 440);
    r = run(sfx::kCompressor, p, loud);
    const double ms = 0.5 * (32000.0 / 32768.0) * (32000.0 / 32768.0);
    const double L = 10.0 * std::log10(ms);
    const double G = std::pow(10.0, (-12.0 - L) * 0.75 / 20.0);
    CHECK(std::abs(double(peakOf(r.out, 66150)) / 32000.0 - G) < 0.03);
    // Ratio 1: no compression.
    p[1] = 1;
    r = run(sfx::kCompressor, p, loud);
    CHECK(r.out == loud);
}

TEST(stock_wavetable_smoother) {
    float p[9];
    params(sfx::kWavetableSmoother, p);
    // Source: 10 windows of 100 samples, window j holds 1000 j.
    std::vector<int16_t> in(1000);
    for (uint32_t i = 0; i < 1000; ++i) in[i] = int16_t(1000 * (i / 100));
    p[0] = 4;
    p[1] = 100;
    // n = min(C - 1, len/W - 1) = 3; output C W = 400.
    Run r = run(sfx::kWavetableSmoother, p, in);
    CHECK_EQ(r.len, 400u);
    // p = k * 3/4: windows (0,1) w 1, (0,1) w 1/2, (1,2) w 1, (2,3) w 1.
    CHECK_EQ(r.out[50], 0);
    CHECK_EQ(r.out[150], 500);
    CHECK_EQ(r.out[250], 1000);
    CHECK_EQ(r.out[350], 2000);
    // Short source: n = len/W - 1 limits the windows read.
    p[0] = 8;
    p[1] = 250;
    r = run(sfx::kWavetableSmoother, p, in);   // n = min(7, 3) = 3
    CHECK_EQ(r.len, 2000u);
    CHECK(peakOf(r.out) <= 9000);   // windows 0..3 (samples 0..999)
    CHECK_EQ(r.out[0], 0);
    // C = 1 gives n = C: the sample is copied unchanged.
    p[0] = 1;
    p[1] = 100;
    CHECK_EQ(sfx::outputFrames(sfx::kWavetableSmoother, p, 1000), 1000u);
    r = run(sfx::kWavetableSmoother, p, in);
    CHECK(r.out == in);
    // Defaults: 256 x 2048.
    params(sfx::kWavetableSmoother, p);
    CHECK_EQ(sfx::outputFrames(sfx::kWavetableSmoother, p, 600000), 256u * 2048u);
}

TEST(stock_timestretch) {
    float p[9];
    params(sfx::kStretchNote, p);
    // Target 0 means the current length.
    p[0] = 0;
    CHECK_EQ(sfx::outputFrames(sfx::kStretchNote, p, 12345), 12345u);
    // Stretch 0.5 s -> 1.0 s: DC stays DC, the pitch of a sine is kept.
    p[0] = 1.0f;
    p[1] = 50;
    Run r = run(sfx::kStretchNote, p, std::vector<int16_t>(22050, 9000));
    CHECK_EQ(r.len, 44100u);
    uint32_t end = r.len;
    while (end > 0 && r.out[end - 1] == 0) --end;
    int dcDev = 0;
    for (uint32_t i = 0; i < end; ++i) dcDev = std::abs(r.out[i] - 9000) > dcDev ? std::abs(r.out[i] - 9000) : dcDev;
    CHECK(dcDev <= 1);         // no gaps inside: only the end is zero-filled
    CHECK(end > r.len - 2 * 2205);
    const auto s = sine(22050, 10000, 441);
    r = run(sfx::kStretchNote, p, s);
    int crossings = 0;
    for (uint32_t i = 1; i < 40000; ++i) crossings += (r.out[i - 1] < 0) != (r.out[i] < 0);
    // 441 Hz over 40000 samples = 400 cycles = 800 crossings.
    CHECK(std::abs(crossings - 800) < 40);
    // Compress 1 s -> 0.25 s.
    p[0] = 0.25f;
    r = run(sfx::kStretchNote, p, sine(44100, 10000, 441));
    CHECK_EQ(r.len, 11025u);
    CHECK(peakOf(r.out) <= 10001);
    // Beat: 120 BPM, 16 steps = 2 s; granule from len g 0.25 / (S + mu).
    params(sfx::kStretchBeat, p);
    CHECK_EQ(sfx::outputFrames(sfx::kStretchBeat, p, 1000), 88200u);
    r = run(sfx::kStretchBeat, p, std::vector<int16_t>(44100, -5000));
    CHECK_EQ(r.len, 88200u);
    CHECK_EQ(r.out[0], -5000);
    CHECK_EQ(r.out[60000], -5000);
}

TEST(stock_fades) {
    float p[9];
    params(sfx::kFadeIn, p);
    const std::vector<int16_t> dc(1000, 10000);
    Run r = run(sfx::kFadeIn, p, dc);
    CHECK_EQ(r.out[0], 0);
    CHECK_EQ(r.out[500], 5000);
    CHECK_EQ(r.out[999], 9990);
    for (uint32_t i = 1; i < 1000; ++i) CHECK(r.out[i] >= r.out[i - 1]);
    r = run(sfx::kFadeOut, p, dc);
    CHECK_EQ(r.out[0], 10000);
    CHECK_EQ(r.out[500], 5000);
    CHECK_EQ(r.out[999], 10);
    // Negative samples truncate toward zero.
    r = run(sfx::kFadeIn, p, std::vector<int16_t>(3, -10));
    CHECK_EQ(r.out[1], -3);
}
