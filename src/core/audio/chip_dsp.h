// Shared chip-style DSP (header-only, inline only, module-facing): the
// building blocks of the retro synth voices (opentracker-synths modules/eight,
// hand, btype, arc) and the LoFi master insert (opentracker-fx modules/lofi;
// docs/PLAN_RETRO.md §2). Our own code from general knowledge of how such
// sound chips behave (shift-register noise, divider clocks, 4-bit stepped
// envelopes, small DACs); nothing here emulates a particular chip. Nothing is
// compiled into an image unless a module uses it.
//
//  - pulseFrame: the band-limited pulse with its edge times clamped (duty jumps
//    stay bounded); fastLog2;
//  - shift registers: Lfsr<bits, tap> (a Fibonacci shift register for the
//    trinomial x^bits + x^(bits - tap) + 1, maximal length: period
//    2^bits - 1), the run-time width step (lfsrStep), and the short ones
//    (4, 5, 9 bits) as constant bit tables for O(1) indexing (PolyTable);
//  - Tick: a clock in frames (50 / 60 / 100 / 120 Hz or a note division of
//    the song tempo) with a fractional period; a render loop splits its block
//    at the ticks (chunk / advance), so ticks are sample-accurate;
//  - stepped envelopes: a 4-bit level that moves one level per step time
//    (stepEnvTo), shown stepped or smooth (env4);
//  - Tricks: the CHIP section of the retro engines (arpeggio at the tick,
//    stepped vibrato, a pitch slide at the note start);
//  - DAC: quantise to n bits (quantBits), a non-monotonic ladder error on the
//    top 6 bits (ladder), the fractional sample-and-hold period (holdRate).
#pragma once

#include <cstdint>

#include "audio/audio_tables.h"

namespace ot {
namespace audio {
namespace chip {

// --- shift registers --------------------------------------------------------------

// Fibonacci shift register for x^kBits + x^(kBits - kTap) + 1 shifting right:
// the new top bit is bit 0 xor bit kTap; the output is bit 0. Never all zero.
template <int kBits, int kTap>
struct Lfsr {
    static constexpr uint32_t kMask = (1u << kBits) - 1u;
    static constexpr uint32_t kPeriod = kMask;
    uint32_t s = kMask;
    inline uint32_t step() {
        const uint32_t fb = (s ^ (s >> kTap)) & 1u;
        s = (s >> 1) | (fb << (kBits - 1));
        return s & 1u;
    }
    inline void reset() { s = kMask; }
};
using Lfsr4 = Lfsr<4, 1>;     // x^4 + x^3 + 1:     15
using Lfsr5 = Lfsr<5, 2>;     // x^5 + x^3 + 1:     31
using Lfsr7 = Lfsr<7, 1>;     // x^7 + x^6 + 1:     127
using Lfsr9 = Lfsr<9, 4>;     // x^9 + x^5 + 1:     511
using Lfsr15 = Lfsr<15, 1>;   // x^15 + x^14 + 1:   32,767
using Lfsr17 = Lfsr<17, 3>;   // x^17 + x^14 + 1:   131,071
using Lfsr23 = Lfsr<23, 5>;   // x^23 + x^18 + 1:   8,388,607

// One step of a register whose width is chosen at run time (bits <= 31; the
// same rule as Lfsr): the handheld noise's 15 / 7-bit switch. A zero state
// (after a width change masked it away) restarts all ones.
inline uint32_t lfsrStep(uint32_t& s, int bits, int tap) {
    const uint32_t mask = (1u << bits) - 1u;
    uint32_t x = s & mask;
    if (!x) x = mask;
    const uint32_t fb = (x ^ (x >> tap)) & 1u;
    s = (x >> 1) | (fb << (bits - 1));
    return s & 1u;
}

// k steps at once (0 <= k <= bits - tap): the feedback bits of the next k
// steps come from the bits still unshifted (bit j xor bit j + tap), so they
// go in together. Returns the output bit after the last step (bit 0).
inline uint32_t lfsrSteps(uint32_t& s, int bits, int tap, int k) {
    const uint32_t mask = (1u << bits) - 1u;
    uint32_t x = s & mask;
    if (!x) x = mask;
    if (k > 0) {
        const uint32_t fb = (x ^ (x >> tap)) & ((1u << k) - 1u);
        x = (x >> k) | (fb << (bits - k));
    }
    s = x;
    return x & 1u;
}

// The output bits of a short register over one period, packed (bit i of the
// sequence = bit i % 8 of byte i / 8), built at compile time.
template <int kBits, int kTap>
struct PolyTable {
    static constexpr uint32_t kPeriod = (1u << kBits) - 1u;
    uint8_t b[(kPeriod + 7) / 8];
    constexpr PolyTable() : b() {
        uint32_t s = (1u << kBits) - 1u;
        for (uint32_t i = 0; i < kPeriod; ++i) {
            const uint32_t fb = (s ^ (s >> kTap)) & 1u;
            s = (s >> 1) | (fb << (kBits - 1));
            if (s & 1u) b[i >> 3] = uint8_t(b[i >> 3] | (1u << (i & 7u)));
        }
    }
    constexpr uint32_t bit(uint32_t i) const { return (b[i >> 3] >> (i & 7u)) & 1u; }
};
inline constexpr PolyTable<4, 1> kPoly4{};   // 2 B
inline constexpr PolyTable<5, 2> kPoly5{};   // 4 B
inline constexpr PolyTable<9, 4> kPoly9{};   // 64 B

// --- band-limited pulse ----------------------------------------------------------

// va::vcoFrame<kShPulse> (the one-frame-late polyBLEP pulse, +1 below the
// duty, -1 above, less its mean) with the edge times clamped into the frame:
// a duty that jumps past the phase (a PW step, a macro) gives one bounded
// edge instead of a residual of (jump / d)^2. The same output for any duty
// that moves less than a frame's phase step.
inline float pulseFrame(float& p, float& z, bool& hi, float d, float duty) {
    p += d;
    const bool wrap = p >= 1.0f;
    if (wrap) p -= 1.0f;
    float out = z;
    const bool h = p < duty;
    float cur = (h ? 1.0f : -1.0f) - (duty + duty - 1.0f);
    if (wrap) {
        float x = p / d;
        x = x > 1.0f ? 1.0f : x;
        const float y = 1.0f - x;
        cur -= y * y;
        out += x * x;
    }
    if (!h && (hi || wrap)) {
        float x = (p - duty) / d;
        x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        const float y = 1.0f - x;
        cur += y * y;
        out -= x * x;
    }
    hi = h;
    z = cur;
    return out;
}

// log2(x) for x > 0 (the exponent bits and the atanh series on the mantissa;
// error < 0.0002): block-rate use without the library's log2f.
inline float fastLog2(float x) {
    uint32_t b;
    __builtin_memcpy(&b, &x, 4);
    const float e = float(int((b >> 23) & 0xFFu) - 127);
    b = (b & 0x007FFFFFu) | 0x3F800000u;
    float m;
    __builtin_memcpy(&m, &b, 4);   // 1..2
    const float u = (m - 1.0f) / (m + 1.0f), u2 = u * u;   // <= 1/3: 2 atanh(u) / ln 2
    return e + 2.8853901f * u * (1.0f + u2 * (1.0f / 3.0f + u2 * 0.2f));
}

// --- tick clock -------------------------------------------------------------------

// The CHIP section's rates: four fixed clocks and five note divisions of the
// song tempo (a 1/32 note is a 32nd of a 4/4 bar).
enum Rate : uint8_t { k50Hz = 0, k60Hz, k100Hz, k120Hz, kDiv32, kDiv48, kDiv64, kDiv96, kDiv128, kRateCount };

// Frames per tick of a rate at the tempo (BPM, clamped 20..999).
inline float tickFrames(int rate, float bpm) {
    static const float kHz[4] = {50.0f, 60.0f, 100.0f, 120.0f};
    static const float kDiv[5] = {32.0f, 48.0f, 64.0f, 96.0f, 128.0f};
    if (rate < 0) rate = 0;
    if (rate >= kRateCount) rate = kRateCount - 1;
    if (rate < kDiv32) return kSampleRate / kHz[rate];
    bpm = bpm < 20.0f ? 20.0f : (bpm > 999.0f ? 999.0f : bpm);
    return kSampleRate * (240.0f / bpm) / kDiv[rate - kDiv32];
}

// A clock with a fractional period in frames. A tick falls on the first frame
// at or after its exact time, so the mean period is exact. Render loops ask
// chunk(n) for the frames to render before the next tick (1..n), then
// advance(c): true when the tick fell at the end of that chunk.
struct Tick {
    float period;   // frames per tick (>= 1)
    float left;     // frames until the next tick
    inline void set(float frames) {
        period = frames < 1.0f ? 1.0f : frames;
        if (left > period) left = period;
    }
    inline void start(float frames) {
        period = frames < 1.0f ? 1.0f : frames;
        left = period;
    }
    inline uint32_t chunk(uint32_t n) const {
        if (left >= float(n)) return n;
        uint32_t c = uint32_t(left);
        if (float(c) < left) ++c;
        return c < 1u ? 1u : c;
    }
    inline bool advance(uint32_t c) {
        left -= float(c);
        if (left > 0.0f) return false;
        left += period;
        if (left <= 0.0f) left = period;
        return true;
    }
};

// --- stepped envelopes -----------------------------------------------------------

// A 4-bit envelope level 0..15 (float: the position between steps) moved
// toward `target` by `frames` frames at one level per `per` frames (per <= 0:
// at once). Returns true once it is at the target.
inline bool stepEnvTo(float& v, float target, float per, float frames) {
    if (per <= 0.0f) {
        v = target;
        return true;
    }
    const float d = frames / per;
    if (v < target) {
        v += d;
        if (v >= target) v = target;
    } else if (v > target) {
        v -= d;
        if (v <= target) v = target;
    }
    return v == target;
}
// The envelope's gain 0..1: stepped (the level the envelope has reached: a
// rising one rounds down, a falling one up, so each step lands after its
// step time) or smooth.
inline float env4(float v, bool stepped, bool rising) {
    if (!stepped) return v * (1.0f / 15.0f);
    const int i = int(v);
    const float l = rising || float(i) == v ? float(i) : float(i + 1);
    return l * (1.0f / 15.0f);
}

// --- CHIP tricks ------------------------------------------------------------------

enum ArpMode : uint8_t {
    kArpOff = 0, kArpOct, kArpMaj, kArpMin, kArpSus4, kArpDom7, kArpMaj7, kArpMin7, kArpDim, kArpAug, kArpPow5, kArpCustom,
    kArpModeCount
};

// The settings of a slot's CHIP section (block rate, from the patch).
struct TricksCfg {
    int8_t notes[4];    // the arp's semitone offsets (notes[0] = 0)
    uint8_t count;      // 1 (no arp) .. 4
    uint8_t rate;       // Rate
    float vibDepth;     // semitones (a multiple of 1/16)
    float vibRate;      // Hz
    float vibDelay;     // s
    float slideStart;   // semitones at the note start
    float slideTime;    // s (0: no slide)
};
// mode, the Custom notes (0..24), the vibrato and the slide (patch units).
inline void setTricks(TricksCfg& c, int mode, int n2, int n3, int rate, float vibRate, float vibDepth, float vibDelay,
                      float slideStart, float slideTime) {
    static const int8_t kTab[kArpModeCount][4] = {
        {0, 0, 0, 0},  {0, 12, 0, 0}, {0, 4, 7, 0},  {0, 3, 7, 0},  {0, 5, 7, 0},  {0, 4, 7, 10},
        {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 3, 6, 0},  {0, 4, 8, 0},  {0, 7, 12, 0}, {0, 0, 0, 0},
    };
    static const uint8_t kCount[kArpModeCount] = {1, 2, 3, 3, 3, 4, 4, 4, 3, 3, 3, 3};
    if (mode < 0 || mode >= kArpModeCount) mode = kArpOff;
    for (int k = 0; k < 4; ++k) c.notes[k] = kTab[mode][k];
    c.count = kCount[mode];
    if (mode == kArpCustom) {
        n2 = n2 < 0 ? 0 : (n2 > 24 ? 24 : n2);
        n3 = n3 < 0 ? 0 : (n3 > 24 ? 24 : n3);
        c.notes[1] = int8_t(n2);
        c.notes[2] = int8_t(n3);
        c.count = n3 == 0 ? (n2 == 0 ? 1 : 2) : 3;
    }
    c.rate = uint8_t(rate < 0 ? 0 : (rate >= kRateCount ? kRateCount - 1 : rate));
    const float d = vibDepth < 0.0f ? 0.0f : (vibDepth > 2.0f ? 2.0f : vibDepth);
    c.vibDepth = float(int(d * 16.0f + 0.5f)) * (1.0f / 16.0f);
    c.vibRate = vibRate < 0.5f ? 0.5f : (vibRate > 15.0f ? 15.0f : vibRate);
    c.vibDelay = vibDelay < 0.0f ? 0.0f : (vibDelay > 1.0f ? 1.0f : vibDelay);
    c.slideStart = slideStart < -24.0f ? -24.0f : (slideStart > 24.0f ? 24.0f : slideStart);
    c.slideTime = slideTime <= 0.0005f ? 0.0f : (slideTime > 1.0f ? 1.0f : slideTime);
}

// The CHIP section's enum names for the engines' descriptors (one copy in an
// image however many engines show them).
inline constexpr const char* kArpNames[kArpModeCount] = {"Off",  "Oct",  "Maj",  "Min", "Sus4", "Dom7",
                                                         "Maj7", "Min7", "Dim",  "Aug", "Pow5", "Custom"};
inline constexpr const char* kRateNames[kRateCount] = {"50 Hz", "60 Hz", "100 Hz", "120 Hz", "1/32",
                                                       "1/48",  "1/64",  "1/96",   "1/128"};

// One voice's CHIP state: the tick clock, the arp step, the vibrato (stepped:
// its value changes only at the ticks) and the age of the note.
struct Tricks {
    Tick tick;
    float vibPh;    // 0..1
    float vib;      // the held vibrato offset, semitones
    float age;      // seconds since the note start
    uint8_t idx;    // the arp step
    uint8_t pad[3];
};
// The clock needs to run: an arp or a vibrato.
inline bool tricksTick(const TricksCfg& c) { return c.count > 1 || c.vibDepth > 0.0f; }
// A note starts (a reused, gated voice keeps its arp phase only when legato).
inline void tricksStart(Tricks& t, const TricksCfg& c, float bpm) {
    t.tick.start(tickFrames(c.rate, bpm));
    t.vibPh = 0.0f;
    t.vib = 0.0f;
    t.age = 0.0f;
    t.idx = 0;
}
// The tempo or the rate moved (once per block).
inline void tricksRate(Tricks& t, const TricksCfg& c, float bpm) { t.tick.set(tickFrames(c.rate, bpm)); }
// The semitone offset now (the arp step, the held vibrato, the slide at the
// note's age plus `dt` seconds).
inline float tricksOffset(const Tricks& t, const TricksCfg& c, float dt) {
    float o = float(c.notes[t.idx < c.count ? t.idx : 0]) + t.vib;
    if (c.slideTime > 0.0f && c.slideStart != 0.0f) {
        const float a = t.age + dt;
        if (a < c.slideTime) o += c.slideStart * (1.0f - a / c.slideTime);
    }
    return o;
}
// After rendering `frames` frames: the age, and at a tick the next arp step
// and a new vibrato value (a triangle at Vib Rate, after Vib Delay, in
// 1/16 semitone steps).
inline void tricksAdvance(Tricks& t, const TricksCfg& c, uint32_t frames, bool tick) {
    const float s = float(frames) * (1.0f / kSampleRate);
    t.age += s;
    t.vibPh += s * c.vibRate;
    if (t.vibPh >= 1.0f) t.vibPh -= float(int(t.vibPh));
    if (!tick) return;
    if (c.count > 1) {
        ++t.idx;
        if (t.idx >= c.count) t.idx = 0;
    }
    if (c.vibDepth > 0.0f && t.age >= c.vibDelay) {
        const float p = t.vibPh;
        const float tri = p < 0.25f ? 4.0f * p : (p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f);
        t.vib = float(int(tri * c.vibDepth * 16.0f + (tri >= 0.0f ? 0.5f : -0.5f))) * (1.0f / 16.0f);
    } else {
        t.vib = 0.0f;
    }
}

// --- DAC --------------------------------------------------------------------------

// x (-1..1) on a `bits`-bit DAC (2^bits levels, mid-tread: 0 is a level),
// clamped to the code range. bits >= 16: unchanged.
inline float quantBits(float x, int bits) {
    if (bits >= 16) return x;
    if (bits < 1) bits = 1;
    const float q = float(1 << (bits - 1)), inv = 1.0f / q;
    float y = x * q;
    y = y < -q ? -q : (y > q - 1.0f ? q - 1.0f : y);
    const int c = int(y + (y >= 0.0f ? 0.5f : -0.5f));
    return float(c) * inv;
}
// The same with the scale precomputed (q = 2^(bits - 1)).
inline float quantQ(float x, float q, float inv) {
    float y = x * q;
    y = y < -q ? -q : (y > q - 1.0f ? q - 1.0f : y);
    const int c = int(y + (y >= 0.0f ? 0.5f : -0.5f));
    return float(c) * inv;
}

// The ladder: the top 6 bits of the offset-binary code carry weight errors
// (in LSB at amount 1): the most significant one short, the others long, so
// the step where the top bit turns on goes down (a non-monotonic DAC). Sum of
// |errors| 1.5 LSB.
inline constexpr float kLadderErr[6] = {0.05f, 0.1f, 0.15f, 0.2f, 0.3f, -0.7f};   // bit 0 (lowest of the six) .. bit 5
struct LadderTable {
    float e[64];
    constexpr LadderTable() : e() {
        for (int i = 0; i < 64; ++i) {
            float s = 0.0f;
            for (int k = 0; k < 6; ++k)
                if (i & (1 << k)) s += kLadderErr[k];
            e[i] = s;
        }
    }
};
inline constexpr LadderTable kLadder{};
// x on a `bits`-bit DAC (4..15; 16 as 15) with the ladder error x amount
// (0..1; 0 = quantBits). Below 6 bits the DAC's bits are the top ones of
// the six (the most significant errors).
inline float ladder(float x, int bits, float amount) {
    if (bits > 15) bits = 15;
    if (bits < 4) bits = 4;
    const float q = float(1 << (bits - 1));
    float y = x * q;
    y = y < -q ? -q : (y > q - 1.0f ? q - 1.0f : y);
    const int c = int(y + (y >= 0.0f ? 0.5f : -0.5f));
    const uint32_t code = uint32_t(c + (1 << (bits - 1)));   // offset binary 0 .. 2^bits - 1
    const uint32_t top = bits >= 6 ? (code >> (bits - 6)) & 63u : (code << (6 - bits)) & 63u;
    return (float(c) + amount * kLadder.e[top]) / q;
}

// Sample-and-hold period in frames for a rate in Hz (>= 1 frame).
inline float holdRate(float fs, float hz) {
    const float p = hz > 0.0f ? fs / hz : 1.0f;
    return p < 1.0f ? 1.0f : p;
}

}  // namespace chip
}  // namespace audio
}  // namespace ot
