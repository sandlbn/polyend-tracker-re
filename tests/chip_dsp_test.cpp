// Shared chip-style DSP (audio/chip_dsp.h, docs/PLAN_RETRO.md §2): the shift
// registers' periods (and the bit tables against the registers), the tick
// clock's mean period at a fixed rate and at a note division of the tempo,
// chunks that land on the ticks, the stepped envelope's step count and
// timing, the CHIP tricks (arp orders, Custom notes, the stepped vibrato's
// depth and rate, the slide), the DAC quantiser's level counts and the
// ladder's bounds, its exactness at 0 and its non-monotonic step at 100.
// Runs in every profile (header-only, used by modules).
#include "test.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "audio/chip_dsp.h"
#include "audio/va_dsp.h"

using namespace ot::audio;
using namespace ot::audio::chip;

namespace {

template <class R>
uint32_t periodOf(R r, uint32_t cap) {
    const uint32_t s0 = r.s;
    for (uint32_t n = 1; n <= cap; ++n) {
        r.step();
        if (r.s == s0) return n;
    }
    return 0;
}

}  // namespace

TEST(chip_lfsr_periods) {
    CHECK_EQ(periodOf(Lfsr4{}, 100u), 15u);
    CHECK_EQ(periodOf(Lfsr5{}, 100u), 31u);
    CHECK_EQ(periodOf(Lfsr7{}, 1000u), 127u);
    CHECK_EQ(periodOf(Lfsr9{}, 1000u), 511u);
    CHECK_EQ(periodOf(Lfsr15{}, 100000u), 32767u);
    CHECK_EQ(periodOf(Lfsr17{}, 200000u), 131071u);
    CHECK_EQ(periodOf(Lfsr23{}, 9000000u), 8388607u);
    // The output is balanced (a maximal sequence has 2^(n-1) ones).
    Lfsr15 r;
    uint32_t ones = 0;
    for (uint32_t i = 0; i < Lfsr15::kPeriod; ++i) ones += r.step();
    CHECK_EQ(ones, 16384u);
    // The run-time width: 15 and 7 bits, and a switch from 15 to 7 never sticks at 0.
    uint32_t s = 0x7FFF, s0 = s;
    uint32_t n = 0;
    do {
        lfsrStep(s, 15, 1);
        ++n;
    } while (s != s0 && n < 100000u);
    CHECK_EQ(n, 32767u);
    s = 0x0100;   // masked to 7 bits: zero -> restarts
    std::set<uint32_t> seen;
    for (int i = 0; i < 300; ++i) {
        lfsrStep(s, 7, 1);
        CHECK(s != 0u);
        seen.insert(s);
    }
    CHECK_EQ(seen.size(), size_t(127));
    // k steps at once = k single steps (both widths, every k the rule allows).
    uint32_t a = 0x5A5A, b = 0x5A5A;
    for (int round = 0; round < 2000; ++round) {
        const int bits = round & 1 ? 7 : 15, k = round % (bits);   // 0 .. bits - 1
        uint32_t last = b & 1u;
        for (int j = 0; j < k; ++j) last = lfsrStep(b, bits, 1);
        const uint32_t out = lfsrSteps(a, bits, 1, k);
        if (k) {
            CHECK_EQ(a, b);
            CHECK_EQ(out, last);
        }
        b = a;
    }
}

TEST(chip_poly_tables_match_the_registers) {
    Lfsr4 a;
    Lfsr5 b;
    Lfsr9 c;
    for (uint32_t i = 0; i < 511; ++i) {
        const uint32_t bc = c.step();
        CHECK_EQ(kPoly9.bit(i), bc);
        if (i < 31) CHECK_EQ(kPoly5.bit(i), b.step());
        if (i < 15) CHECK_EQ(kPoly4.bit(i), a.step());
    }
    CHECK_EQ(sizeof(kPoly4.b) + sizeof(kPoly5.b) + sizeof(kPoly9.b), size_t(2 + 4 + 64));
}

TEST(chip_tick_mean_period_and_chunks) {
    // 60 Hz: 735.3 frames mean over 100 ticks (+-1 frame).
    Tick t{};
    t.start(tickFrames(k60Hz, 120.0f));
    uint32_t frames = 0, ticks = 0, chunks = 0, blocks = 0;
    std::vector<uint32_t> at;
    while (ticks < 100) {
        uint32_t left = kBlock;
        ++blocks;
        while (left) {
            const uint32_t c = t.chunk(left);
            CHECK(c >= 1u && c <= left);
            ++chunks;
            frames += c;
            left -= c;
            if (t.advance(c)) {
                ++ticks;
                at.push_back(frames);
            }
        }
    }
    const double mean = double(at.back()) / 100.0;
    std::printf("  60 Hz tick: %.2f frames mean, %u chunks in %u blocks\n", mean, chunks, blocks);
    CHECK(std::fabs(mean - 44117.647 / 60.0) < 1.0);
    for (size_t k = 1; k < at.size(); ++k) CHECK(at[k] - at[k - 1] == 735u || at[k] - at[k - 1] == 736u);
    // 1/32 at 120 BPM = 2,756 frames; the fastest rate splits a block in at most 3 chunks.
    CHECK(std::fabs(tickFrames(kDiv32, 120.0f) - 2757.35f) < 1.5f);
    CHECK(std::fabs(tickFrames(kDiv64, 120.0f) - 1378.7f) < 1.0f);
    CHECK(std::fabs(tickFrames(kDiv48, 120.0f) / tickFrames(kDiv96, 120.0f) - 2.0f) < 1e-4f);
    CHECK(std::fabs(tickFrames(kDiv32, 10.0f) - tickFrames(kDiv32, 20.0f)) < 1e-3f);   // clamped 20..999
    CHECK(std::fabs(tickFrames(k50Hz, 1.0f) - 882.35f) < 0.1f);
    Tick f{};
    f.start(tickFrames(k120Hz, 120.0f));
    int worst = 0;
    for (int b = 0; b < 1000; ++b) {
        uint32_t left = kBlock;
        int n = 0;
        while (left) {
            const uint32_t c = f.chunk(left);
            f.advance(c);
            left -= c;
            ++n;
        }
        worst = std::max(worst, n);
    }
    CHECK(worst <= 3);
    // A shorter period caps the frames left; a rate change keeps the clock running.
    f.set(10.0f);
    CHECK(f.left <= 10.0f);
}

TEST(chip_step_envelope) {
    // 15 -> 0 in 15 steps of 100 frames: stepped, each step lands after its time.
    float v = 15.0f;
    int steps = 0;
    float last = env4(v, true, false);
    int frames = 0;
    std::vector<int> when;
    while (!stepEnvTo(v, 0.0f, 100.0f, 1.0f)) {
        ++frames;
        const float g = env4(v, true, false);
        if (g != last) {
            ++steps;
            when.push_back(frames);
            CHECK(std::fabs((last - g) - 1.0f / 15.0f) < 1e-6f);
        }
        last = g;
    }
    ++frames;
    if (env4(v, true, false) != last) {
        ++steps;
        when.push_back(frames);
    }
    CHECK_EQ(steps, 15);
    CHECK_EQ(env4(v, true, false), 0.0f);
    CHECK_EQ(when.front(), 100);
    CHECK_EQ(when.back(), 1500);
    // Rising rounds down; smooth is the level itself; per 0 jumps.
    float u = 0.0f;
    stepEnvTo(u, 15.0f, 100.0f, 150.0f);
    CHECK(std::fabs(env4(u, true, true) - 1.0f / 15.0f) < 1e-6f);
    CHECK(std::fabs(env4(u, false, true) - 1.5f / 15.0f) < 1e-6f);
    CHECK(stepEnvTo(u, 7.0f, 0.0f, 1.0f));
    CHECK_EQ(u, 7.0f);
}

namespace {
// The arp's notes over n ticks.
std::vector<int> arpRun(int mode, int n2, int n3, int ticks) {
    TricksCfg c{};
    setTricks(c, mode, n2, n3, k60Hz, 5.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    Tricks t{};
    tricksStart(t, c, 120.0f);
    std::vector<int> out;
    for (int k = 0; k < ticks; ++k) {
        out.push_back(int(tricksOffset(t, c, 0.0f)));
        tricksAdvance(t, c, 735, true);
    }
    return out;
}
}  // namespace

TEST(chip_tricks_arp_vibrato_slide) {
    CHECK(arpRun(kArpOct, 0, 0, 4) == (std::vector<int>{0, 12, 0, 12}));
    CHECK(arpRun(kArpMaj, 0, 0, 6) == (std::vector<int>{0, 4, 7, 0, 4, 7}));
    CHECK(arpRun(kArpMin, 0, 0, 3) == (std::vector<int>{0, 3, 7}));
    CHECK(arpRun(kArpDom7, 0, 0, 5) == (std::vector<int>{0, 4, 7, 10, 0}));
    CHECK(arpRun(kArpPow5, 0, 0, 3) == (std::vector<int>{0, 7, 12}));
    CHECK(arpRun(kArpCustom, 5, 9, 4) == (std::vector<int>{0, 5, 9, 0}));
    CHECK(arpRun(kArpCustom, 5, 0, 4) == (std::vector<int>{0, 5, 0, 5}));   // Note 3 = 0: two notes
    CHECK(arpRun(kArpOff, 5, 9, 3) == (std::vector<int>{0, 0, 0}));
    TricksCfg c{};
    setTricks(c, kArpOff, 4, 7, k60Hz, 5.0f, 0.5f, 0.0f, 0.0f, 0.0f);
    CHECK(tricksTick(c));
    // Stepped vibrato: +-0.5 st peak, 1/16 steps, changes only at the ticks, 5 Hz.
    Tricks t{};
    tricksStart(t, c, 120.0f);
    float lo = 0, hi = 0;
    int crossings = 0;
    float prev = 0.0f;
    for (int k = 0; k < 600; ++k) {   // 10 s of 60 Hz ticks
        const float before = tricksOffset(t, c, 0.0f);
        tricksAdvance(t, c, 300, false);
        CHECK_EQ(tricksOffset(t, c, 0.0f), before);   // no tick: no change
        tricksAdvance(t, c, 435, true);
        const float o = tricksOffset(t, c, 0.0f);
        CHECK(std::fabs(o * 16.0f - std::round(o * 16.0f)) < 1e-4f);
        lo = std::min(lo, o);
        hi = std::max(hi, o);
        if (prev < 0.0f && o >= 0.0f) ++crossings;
        prev = o;
    }
    std::printf("  vibrato: %.3f .. %.3f st, %d cycles in 10 s\n", lo, hi, crossings);
    CHECK(std::fabs(hi - 0.5f) < 0.07f);
    CHECK(std::fabs(lo + 0.5f) < 0.07f);
    CHECK(crossings >= 48 && crossings <= 52);
    // Vib Delay: no vibrato before it.
    setTricks(c, kArpOff, 4, 7, k60Hz, 5.0f, 1.0f, 0.5f, 0.0f, 0.0f);
    tricksStart(t, c, 120.0f);
    float early = 0.0f;
    for (int k = 0; k < 25; ++k) {   // 0.42 s
        tricksAdvance(t, c, 735, true);
        early = std::max(early, std::fabs(tricksOffset(t, c, 0.0f)));
    }
    CHECK_EQ(early, 0.0f);
    // Slide: +24 -> 0 in Slide Time (60 ms) +-10 %, linear.
    setTricks(c, kArpOff, 4, 7, k60Hz, 5.0f, 0.0f, 0.0f, 24.0f, 0.06f);
    CHECK(!tricksTick(c));
    tricksStart(t, c, 120.0f);
    CHECK_EQ(tricksOffset(t, c, 0.0f), 24.0f);
    CHECK(std::fabs(tricksOffset(t, c, 0.03f) - 12.0f) < 0.01f);
    int frames = 0;
    while (tricksOffset(t, c, 0.0f) > 0.0f && frames < 44100) {
        tricksAdvance(t, c, 1, false);
        ++frames;
    }
    std::printf("  slide: +24 st -> 0 in %.1f ms\n", 1000.0 * frames / 44117.647);
    CHECK(std::fabs(frames / 44117.647 - 0.06) < 0.006);
    // Depth is in 1/16 st steps.
    setTricks(c, kArpOff, 0, 0, k60Hz, 5.0f, 0.1f, 0.0f, 0.0f, 0.0f);
    CHECK_EQ(c.vibDepth, 0.125f);
}

TEST(chip_dac_quantise_and_ladder) {
    // Level counts: bits b -> 2^b distinct values over a full sweep.
    for (int bits : {4, 6, 8, 9, 12}) {
        std::set<float> lv;
        for (int i = 0; i <= 100000; ++i) lv.insert(quantBits(-1.0f + 2.0f * float(i) / 100000.0f, bits));
        CHECK_EQ(lv.size(), size_t(1u << bits));
    }
    CHECK_EQ(quantBits(0.0f, 4), 0.0f);
    CHECK_EQ(quantBits(0.123456f, 16), 0.123456f);
    CHECK_EQ(quantQ(0.3f, 8.0f, 0.125f), quantBits(0.3f, 4));
    // Ladder 0 = the quantiser; at 100 within 2 LSB of it and non-monotonic.
    float worst = 0.0f;
    bool down = false;
    float prev = -2.0f;
    for (int i = 0; i <= 20000; ++i) {
        const float x = -1.0f + 2.0f * float(i) / 20000.0f;
        CHECK_EQ(ladder(x, 9, 0.0f), quantBits(x, 9));
        const float y = ladder(x, 9, 1.0f);
        worst = std::max(worst, std::fabs(y - quantBits(x, 9)) * 256.0f);
        if (y < prev - 1e-6f) down = true;
        prev = y;
    }
    std::printf("  ladder (9 bits, 100): |error| <= %.2f LSB, non-monotonic %d\n", worst, int(down));
    CHECK(worst <= 2.0f);
    CHECK(worst > 0.5f);
    CHECK(down);
    // 4 and 5 bits: the top bits of the six carry the errors; 0 still exact.
    for (int bits : {4, 5}) {
        float w = 0.0f;
        std::set<float> lv;
        for (int i = 0; i <= 4000; ++i) {
            const float x = -1.0f + 2.0f * float(i) / 4000.0f;
            CHECK_EQ(ladder(x, bits, 0.0f), quantBits(x, bits));
            const float y = ladder(x, bits, 1.0f);
            w = std::max(w, std::fabs(y - quantBits(x, bits)) * float(1 << (bits - 1)));
            lv.insert(y);
        }
        CHECK(w <= 2.0f && w > 0.5f);
        CHECK_EQ(lv.size(), size_t(1u << bits));
    }
    // Hold period: 8 kHz = 5.51 frames.
    CHECK(std::fabs(holdRate(44117.647f, 8000.0f) - 5.5147f) < 0.001f);
    CHECK_EQ(holdRate(44117.647f, 100000.0f), 1.0f);
}

TEST(chip_pulse_frame_and_log2) {
    // The same frames as va::vcoFrame for a duty that moves slowly; bounded where the duty jumps.
    float p0 = 0.0f, z0 = 0.0f, p1 = 0.0f, z1 = 0.0f;
    bool h0 = true, h1 = true;
    double diff = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const float d = 0.003f + 0.002f * float(i % 7), duty = 0.3f + 0.1f * std::sin(float(i) * 0.0003f);
        diff = std::max(diff, double(std::fabs(va::vcoFrame<va::kShPulse>(p0, z0, h0, d, duty) - pulseFrame(p1, z1, h1, d, duty))));
    }
    CHECK(diff < 1e-6);
    float worst = 0.0f;
    for (float d : {0.0007f, 0.002f, 0.01f}) {
        float p = 0.0f, z = 0.0f;
        bool h = true;
        for (int i = 0; i < 20000; ++i) {
            const float duty = (i / 300) % 2 ? 0.02f : 0.5f;   // jumps every 300 frames
            worst = std::max(worst, std::fabs(pulseFrame(p, z, h, d, duty)));
        }
    }
    std::printf("  pulseFrame with duty jumps: |peak| %.3f\n", worst);
    CHECK(worst < 2.5f);
    for (float x : {0.001f, 0.37f, 1.0f, 3.0f, 1234.5f})
        CHECK(std::fabs(fastLog2(x) - std::log2(x)) < 0.005f);
    CHECK_EQ(quantBits(0.3f, 0), quantBits(0.3f, 1));
}

TEST(chip_lfsr_steps_wide_tap) {
    // Tap 5 (the 23-bit register) with up to 18 steps at once = single steps.
    uint32_t a = 0x7FFFFF, b = 0x7FFFFF;
    for (int round = 0; round < 3000; ++round) {
        const int k = 1 + round % 18;
        uint32_t last = 0;
        for (int j = 0; j < k; ++j) last = lfsrStep(b, 23, 5);
        CHECK_EQ(lfsrSteps(a, 23, 5, k), last);
        CHECK_EQ(a, b);
    }
}
