// Delay, reverb, limiter (spec 04 §5).
//
// The reverb follows the topology of Mutable Instruments' Clouds/Rings
// reverb (stmlib FxEngine, MIT licence, (c) Emilie Gillet) as adapted in
// spec 04 §5.2; the code here is written from the spec, not copied.
#include "audio/effects.h"
#include "fastrun.h"

#include <cmath>
#include <cstring>
#include <type_traits>

#include "audio/dsp.h"
#include "audio/ring_io.h"

namespace ot {
namespace audio {

namespace {
constexpr uint32_t kDMask = kDelayLineLen - 1;
constexpr uint32_t kRMask = kReverbRingLen - 1;
// |v| as OR-able magnitude: v >= 0 -> v, v < 0 -> -v - 1 (ones' complement).
inline uint32_t mag(int32_t v) { return uint32_t(v ^ (v >> 31)); }

// Block working buffers (SRAM). One engine renders at a time.
int16_t gX[kBlock];
int16_t gR0[kBlock], gR1[kBlock], gR2[kBlock];
int16_t gW0[kBlock], gW1[kBlock], gW2[kBlock];
}  // namespace

// --- Delay (spec 04 §5.1) ----------------------------------------------------

void Delay::init(int16_t* mem) {
    a_ = mem;
    b_ = mem + kDelayLineLen;
    c_ = mem + 2 * kDelayLineLen;
    setFilter(20000.0f);
    clear();
}

void Delay::clear() {
    if (a_) std::memset(a_, 0, sizeof(int16_t) * kMemorySamples);
    xa_ = xb_ = xc_ = ya_ = yb_ = yc_ = 0;
    w_ = 0;
    quiet_ = kDelayLineLen;    // an empty ring is silent
    zeroed_ = kDelayLineLen;   // all lines are zero
    bypass_ = false;
}

namespace {
// One chunk of n <= t samples of the ping-pong lines A/B (reads ra_/rb_ at
// w - t, writes wa_/wb_) and of the mono line C. Every line is written every
// sample (spec 04 §5.1); OUT selects whether the set feeds the outputs. The
// two sets are independent, so they run as two lean loops. st = {x1, y1}
// filter state per line. Return the OR of the read/written magnitudes.
// Inlined into Delay::process, which runs from SRAM_L on the device (fastrun.h).
template <bool OUT>
__attribute__((always_inline)) inline uint32_t delayChunkAB(const int16_t* __restrict x, uint32_t n, int32_t b, int32_t c, int32_t k, int32_t* st,
                      const int16_t* __restrict ra_, const int16_t* __restrict rb_, int16_t* __restrict wa_,
                      int16_t* __restrict wb_, float* __restrict oL, float* __restrict oR) {
    int32_t xa = st[0], xb = st[1], ya = st[2], yb = st[3];
    uint32_t loud = 0;
    for (uint32_t j = 0; j < n; ++j) {
        const int32_t ra = ra_[j], rb = rb_[j];
        // One-pole filter per tap (spec 07 Q9): y = (x*b + x1*b - y1*c) >> 10.
        const int32_t fa = (ra * b + xa * b - ya * c) >> 10;
        const int32_t fb = (rb * b + xb * b - yb * c) >> 10;
        xa = ra;
        xb = rb;
        ya = fa;
        yb = fb;
        if (OUT) {
            oL[j] = s16ToF(sat16(fa));
            oR[j] = s16ToF(sat16(fb));
        }
        // Ping-pong cross-feed A<->B (spec 04 §5.1).
        const int32_t wb = sat16(x[j] + ((fa * k) >> 8));
        const int32_t wa = sat16((fb * k) >> 8);
        wb_[j] = int16_t(wb);
        wa_[j] = int16_t(wa);
        loud |= mag(ra) | mag(rb) | mag(wa) | mag(wb);
    }
    st[0] = xa;
    st[1] = xb;
    st[2] = ya;
    st[3] = yb;
    return loud;
}

template <bool OUT>
__attribute__((always_inline)) inline uint32_t delayChunkC(const int16_t* __restrict x, uint32_t n, int32_t b, int32_t c, int32_t k, int32_t* st,
                     const int16_t* __restrict rc_, int16_t* __restrict wc_, float* __restrict oL,
                     float* __restrict oR) {
    int32_t xc = st[0], yc = st[1];
    uint32_t loud = 0;
    for (uint32_t j = 0; j < n; ++j) {
        const int32_t rc = rc_[j];
        const int32_t fc = (rc * b + xc * b - yc * c) >> 10;
        xc = rc;
        yc = fc;
        if (OUT) {
            const float o = s16ToF(sat16(fc));
            oL[j] = o;
            oR[j] = o;
        }
        // Mono line C (spec 04 §5.1).
        const int32_t wc = sat16(x[j] + ((fc * k) >> 8));
        wc_[j] = int16_t(wc);
        loud |= mag(rc) | mag(wc);
    }
    st[0] = xc;
    st[1] = yc;
    return loud;
}
}  // namespace

OT_FASTRUN_FN bool Delay::process(const float* in, float* outL, float* outR) {
    if (disabled_ || !a_) {
        {
            zeroFloats(outL, kBlock);
            zeroFloats(outR, kBlock);
        }
        return false;
    }
    int32_t any = 0;
    for (uint32_t i = 0; i < kBlock; ++i) {
        gX[i] = toS16(in[i]);
        any |= gX[i];
    }
    const uint32_t t = time_;
    const bool pp = pingpong_;
    const uint32_t yq = mag(ya_) | mag(yb_) | mag(yc_);
    if (any == 0 && quiet_ >= t && yq <= uint32_t(kQuiet)) {
        // Bypass: silent input, decayed tail.
        bypass_ = true;
        {
            zeroFloats(outL, kBlock);
            zeroFloats(outR, kBlock);
        }
        if (zeroed_ < kDelayLineLen) {
            ringZero(a_, kDMask, w_, kBlock);
            ringZero(b_, kDMask, w_, kBlock);
            ringZero(c_, kDMask, w_, kBlock);
            zeroed_ += kBlock;
        }
        xa_ = xb_ = xc_ = ya_ = yb_ = yc_ = 0;
        w_ = (w_ + kBlock) & kDMask;
        if (quiet_ < 0x7FFFFFFFu) quiet_ += kBlock;
        return false;
    }
    bypass_ = false;
    zeroed_ = 0;   // the ring is no longer known to be clear
    const int32_t b = b_coef_, c = c_coef_, k = k_;
    int32_t sab[4] = {xa_, xb_, ya_, yb_};
    int32_t sc[2] = {xc_, yc_};
    uint32_t w = w_;
    uint32_t loud = 0;
    for (uint32_t i0 = 0; i0 < kBlock;) {
        // Chunks of at most t samples: every read precedes the write of the
        // same position (reads at w - t, writes at w).
        uint32_t n = kBlock - i0;
        if (n > t) n = t;
        const uint32_t r = (w - t) & kDMask;
        const int16_t* x = gX + i0;
        float* oL = outL + i0;
        float* oR = outR + i0;
        ringIn(a_, kDMask, r, gR0, n);
        ringIn(b_, kDMask, r, gR1, n);
        ringIn(c_, kDMask, r, gR2, n);
        if (pp) {
            loud |= delayChunkAB<true>(x, n, b, c, k, sab, gR0, gR1, gW0, gW1, oL, oR);
            loud |= delayChunkC<false>(x, n, b, c, k, sc, gR2, gW2, oL, oR);
        } else {
            loud |= delayChunkAB<false>(x, n, b, c, k, sab, gR0, gR1, gW0, gW1, oL, oR);
            loud |= delayChunkC<true>(x, n, b, c, k, sc, gR2, gW2, oL, oR);
        }
        ringOut(a_, kDMask, w, gW0, n);
        ringOut(b_, kDMask, w, gW1, n);
        ringOut(c_, kDMask, w, gW2, n);
        w = (w + n) & kDMask;
        i0 += n;
    }
    xa_ = sab[0];
    xb_ = sab[1];
    ya_ = sab[2];
    yb_ = sab[3];
    xc_ = sc[0];
    yc_ = sc[1];
    w_ = w;
    if (loud > uint32_t(kQuiet)) quiet_ = 0;
    else if (quiet_ < 0x7FFFFFFFu) quiet_ += kBlock;
    return true;
}

// --- Reverb (spec 04 §5.2) ---------------------------------------------------
//
// Ring A layout (offsets from the decrementing write pointer, spec 04 §5.2):
//   ap1 0..149, ap2 151..364, ap3 366..684, ap4 686..1212,
//   dap1a 1214..3395, dap1b 3397..6086, del1 6088..10588, pre-delay write 10590.
// Ring B: dap2a 0..2524, dap2b 2526..4722, del2 base 4724 (modulated read +6211 + 100*(lfo+0.5)).

void Reverb::init(int16_t* mem) {
    a_ = mem;
    b_ = mem + kReverbRingLen;
    // LFO: s[n+1] = c*s[n] - s[n-1], c ~ 1.99999, updated every 32 samples.
    lfoC_ = 1.99999f;
    clear();
}

void Reverb::clear() {
    if (a_) std::memset(a_, 0, sizeof(int16_t) * kMemorySamples);
    std::memset(ap_, 0, sizeof(ap_));
    lp1_ = lp2_ = lp3_ = 0.0f;
    const float w = std::acos(lfoC_ * 0.5f);
    lfoPrev_ = 0.0f;
    lfoS_ = std::sin(w);
    p_ = 0;
    level_ = 0.0f;
    quiet_ = kReverbRingLen;   // an empty ring is silent
    bypass_ = false;
}

namespace {
// Reverb block buffers (SRAM). Taps are read as 128-sample spans at fixed
// offsets from the block's start pointer P: sample i (pointer P - i) uses
// index 127 - i of a span read from P + offset - 127; all-pass results go
// to separate write spans. Every span starts at an odd or even int16 to
// match the parity of its ring position, so ringIn / ringOut always take
// their 32-bit word path.
constexpr uint32_t kModSpan = kBlock + 210;   // modulated del2 read (+-100 + 1)
constexpr uint32_t kSpanStore = kBlock + 2;
alignas(4) int16_t gTapIn[5][kSpanStore];   // dap1a, dap1b, del1 tap, dap2a, dap2b
alignas(4) int16_t gTapOut[6][kSpanStore];  // dap1a, dap1b, del1, dap2a, dap2b, del2
alignas(4) int16_t gPre[kSpanStore], gPd[kSpanStore];
alignas(4) int16_t gMod[kModSpan + 2];
float gAcc[kBlock], gWetL[kBlock];   // pass results
// The span buffer for ring position pos: the same int16 parity as ring + pos.
inline int16_t* spanFor(int16_t* store, const int16_t* ring, uint32_t pos) {
    return store + ((reinterpret_cast<uintptr_t>(ring + (pos & kRMask)) >> 1) & 1u);
}
// Modulated del2 read: LFO segments (the offset only changes every 32 samples).
struct ModSeg {
    uint32_t i0, n;
    int32_t oi;
    float of;
};
}  // namespace

void Reverb::stepLfo(uint32_t p) {
    if ((p & 31u) == 0) {
        const float s = lfoC_ * lfoS_ - lfoPrev_;
        lfoPrev_ = lfoS_;
        lfoS_ = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
    }
}

OT_FASTRUN_FN bool Reverb::process(const float* in, float* outL, float* outR) {
    if (!a_) {
        {
            zeroFloats(outL, kBlock);
            zeroFloats(outR, kBlock);
        }
        return false;
    }
    int16_t* A = a_;
    int16_t* B = b_;
    const uint32_t P = p_;
    const float kap = kap_, klp = klp_, krt = krt_, amount = amount_, gain = gain_;
    const int32_t pre = predelay_ < 0 ? 4999 : predelay_;
    // Pre-delay input (written before its read, spec order) and silence check.
    const uint32_t prePos = P + 10590u - (kBlock - 1);
    int16_t* const preBuf = spanFor(gPre, A, prePos);
    // (q tracks the ring writes for a silent input only, and a silent
    // input writes zeros here, so this loop does not feed q.)
    // anyIn = some input sample != 0.0f: its bits without the sign are not
    // all zero (NaN counts as non-zero, as with != 0.0f).
    // With no pre-delay the pre-delay output is computed from `in` in the
    // all-pass loop (kDirect below) and only the silence check runs here.
    uint32_t q = 0;
    bool anyIn = false;
    if (pre != 0) {
        uint32_t inBits = 0;
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float L = in[i], R = in[i];
            uint32_t b;
            std::memcpy(&b, &in[i], sizeof(b));
            inBits |= b << 1;
            preBuf[kBlock - 1 - i] = toS16((L + R) * gain);
        }
        anyIn = inBits != 0;
    } else {
        for (uint32_t i = 0; i < kBlock; ++i) {
            uint32_t b;
            std::memcpy(&b, &in[i], sizeof(b));
            if (b << 1) {
                anyIn = true;
                break;
            }
        }
    }
    constexpr float kQuietF = float(kQuiet) / 32768.0f;
    auto small = [&](float v) { return v <= kQuietF && v >= -kQuietF; };
    if (!anyIn && quiet_ >= kReverbRingLen && small(lp1_) && small(lp2_) && small(lp3_)) {
        // Bypass: the whole ring holds +-kQuiet residue only.
        bypass_ = true;
        {
            zeroFloats(outL, kBlock);
            zeroFloats(outR, kBlock);
        }
        uint32_t p = P;
        for (uint32_t i = 0; i < kBlock; ++i) {
            stepLfo(p);
            p = (p - 1u) & kRMask;
        }
        p_ = p;
        lp1_ = lp2_ = lp3_ = 0.0f;
        level_ = 0.0f;
        if (quiet_ < 0x7FFFFFFFu) quiet_ += kBlock;
        return false;
    }
    bypass_ = false;
    // LFO and the modulated del2 read positions (relative to P), per
    // segment: the LFO steps when (pointer & 31) == 0, so the offset is
    // constant from one such sample to the next.
    ModSeg seg[kBlock / 32 + 1];
    uint32_t nSeg = 0;
    int32_t lo = 0x7FFFFFFF, hi = -0x7FFFFFFF;
    {
        uint32_t p = P;
        for (uint32_t i = 0; i < kBlock;) {
            stepLfo(p);
            const float off = 4724.0f + 6211.0f + 100.0f * (lfoS_ + 0.5f);
            const uint32_t u = uint32_t(off);
            uint32_t n = (p & 31u) ? (p & 31u) : 32u;
            if (n > kBlock - i) n = kBlock - i;
            ModSeg& s = seg[nSeg++];
            s.i0 = i;
            s.n = n;
            s.oi = int32_t(u);
            s.of = off - float(u);
            const int32_t first = s.oi - int32_t(i), last = s.oi - int32_t(i + n - 1);
            hi = first > hi ? first : hi;
            lo = last < lo ? last : lo;
            i += n;
            p = (p - n) & kRMask;
        }
        p_ = p;
    }
    uint32_t modN = uint32_t(hi - lo + 2);
    if (modN > kModSpan) modN = kModSpan;   // cannot happen (|lfo| <= 1)
    // Reads (all before the block's writes: every other tap pair is >= 128
    // samples apart or reads the older value).
    auto tapIn = [&](const int16_t* ring, uint32_t off, int16_t* store) {
        const uint32_t pos = P + off - (kBlock - 1);
        int16_t* buf = spanFor(store, ring, pos);
        ringIn(ring, kRMask, pos, buf, kBlock);
        return buf;
    };
    const int16_t* const r0 = tapIn(A, 3395, gTapIn[0]);
    const int16_t* const r1 = tapIn(A, 6086, gTapIn[1]);
    const int16_t* const r2 = tapIn(A, 10588, gTapIn[2]);
    const int16_t* const r3 = tapIn(B, 2524, gTapIn[3]);
    const int16_t* const r4 = tapIn(B, 4722, gTapIn[4]);
    int16_t* const mod = spanFor(gMod, B, P + uint32_t(lo));
    ringIn(B, kRMask, P + uint32_t(lo), mod, modN);
    // Write spans (filled by the passes, written to the rings at the end).
    auto tapPos = [&](uint32_t off) { return P + off - (kBlock - 1); };
    int16_t* const w0 = spanFor(gTapOut[0], A, tapPos(1214));
    int16_t* const w1 = spanFor(gTapOut[1], A, tapPos(3397));
    int16_t* const wd1 = spanFor(gTapOut[2], A, tapPos(6088));
    int16_t* const w3 = spanFor(gTapOut[3], B, tapPos(0));
    int16_t* const w4 = spanFor(gTapOut[4], B, tapPos(2526));
    int16_t* const wd2 = spanFor(gTapOut[5], B, tapPos(4724));
    // Pre-delay: write the block, then read it back pre samples later. The
    // pre-delay region (offsets >= 10590) is read by nothing else and is
    // rewritten by ap1 before the pointer comes round, so with no pre-delay
    // the read returns exactly the block just written: skip both accesses.
    const int16_t* pd = preBuf;
    if (pre != 0 && prevPre_ == 0) {
        // Pre-delay switched on: the region was not kept up to date; it
        // starts silent (the first pre samples read zeros).
        ringZero(A, kRMask, P + 10591u, 5000u + kBlock);
    }
    prevPre_ = pre;
    if (pre != 0) {
        ringOut(A, kRMask, prePos, preBuf, kBlock);
        pd = tapIn(A, 10590u + uint32_t(pre), gPd);
    }

    float lp1 = lp1_, lp2 = lp2_, lp3 = lp3_;
    float sumAbs = 0.0f;
    // Block passes (OpenTracker CPU optimisation, same arithmetic per sample
    // as the per-sample spec order). Every delay inside the block is longer
    // than the block (all-passes >= 149, tank lines >= 2181 samples) and the
    // only recursions are the three one-pole filters, so the chain splits
    // into passes over the whole block: input filter + four all-passes ->
    // gAcc, left tank -> gWetL, right tank + output mix.
    // Each stage still rounds to int16 exactly where the spec writes the
    // rings, and every read still sees the value the per-sample order would
    // (a stage reads its own ring 149+ samples back; where an all-pass reads
    // a cell the next all-pass overwrites two samples later, the read is in
    // an earlier pass).
    // Quietness is tracked only for a silent input (otherwise quiet_ resets).
    auto run = [&](auto trackTag) {
        constexpr bool kTrack = decltype(trackTag)::value;
        float* __restrict acc = gAcc;
        uint32_t qa = 0;
        // Input one-pole (pre-delay output) and the four diffusing
        // all-passes (SRAM ring ap_) in one loop over linear chunks: the
        // ring indices decrease with i, a chunk ends before any of them wraps.
        // kDirect (no pre-delay): the pre-delay output is the int16-rounded
        // input of the same sample, rounded in registers.
        auto ap = [&](auto directTag) {
            constexpr bool kDirect = decltype(directTag)::value;
            static constexpr uint16_t kApBase[4] = {0, 151, 366, 686};
            static constexpr uint16_t kApTail[4] = {149, 364, 684, 1212};
            constexpr uint32_t kM = kApLen - 1;
            uint32_t i = 0;
            while (i < kBlock) {
                const uint32_t pi = P - i;
                uint32_t n = kBlock - i;
                for (int a = 0; a < 4; ++a) {
                    const uint32_t r = ((pi + kApTail[a]) & kM) + 1u, w = ((pi + kApBase[a]) & kM) + 1u;
                    n = r < n ? r : n;
                    n = w < n ? w : n;
                }
                const int16_t* __restrict x = pd + (kBlock - 1 - i);
                const float* __restrict xf = in + i;
                const int16_t* t0 = ap_ + ((pi + kApTail[0]) & kM);
                const int16_t* t1 = ap_ + ((pi + kApTail[1]) & kM);
                const int16_t* t2 = ap_ + ((pi + kApTail[2]) & kM);
                const int16_t* t3 = ap_ + ((pi + kApTail[3]) & kM);
                int16_t* b0 = ap_ + ((pi + kApBase[0]) & kM);
                int16_t* b1 = ap_ + ((pi + kApBase[1]) & kM);
                int16_t* b2 = ap_ + ((pi + kApBase[2]) & kM);
                int16_t* b3 = ap_ + ((pi + kApBase[3]) & kM);
                float* __restrict o = acc + i;
                for (uint32_t j = 0; j < n; ++j) {
                    float pdv;
                    if (kDirect) {
                        const float L = xf[j], R = xf[j];
                        pdv = s16Round((L + R) * gain);
                    } else {
                        pdv = s16ToF(*x--);
                    }
                    lp1 += klp * (pdv - lp1);
                    float s = lp1;
                    float t;
                    int16_t v;
                    t = s16ToF(*t0--);
                    s += kap * t;
                    v = toS16(s);
                    *b0-- = v;
                    if (kTrack) qa |= mag(v);
                    s = t - kap * s;
                    t = s16ToF(*t1--);
                    s += kap * t;
                    v = toS16(s);
                    *b1-- = v;
                    if (kTrack) qa |= mag(v);
                    s = t - kap * s;
                    t = s16ToF(*t2--);
                    s += kap * t;
                    v = toS16(s);
                    *b2-- = v;
                    if (kTrack) qa |= mag(v);
                    s = t - kap * s;
                    t = s16ToF(*t3--);
                    s += kap * t;
                    v = toS16(s);
                    *b3-- = v;
                    if (kTrack) qa |= mag(v);
                    o[j] = t - kap * s;
                }
                i += n;
            }
        };
        if (pre == 0) ap(std::true_type{});
        else ap(std::false_type{});
        // Left tank: modulated read of del2 (ring B), lp2, dap1a, dap1b, del1.
        {
            float* __restrict wet = gWetL;
            const int16_t* __restrict ta = r0 + (kBlock - 1);
            const int16_t* __restrict tb = r1 + (kBlock - 1);
            int16_t* __restrict oa = w0 + (kBlock - 1);
            int16_t* __restrict ob = w1 + (kBlock - 1);
            int16_t* __restrict od = wd1 + (kBlock - 1);
            uint32_t ql = 0;
            for (uint32_t sg = 0; sg < nSeg; ++sg) {
                const uint32_t i0 = seg[sg].i0, n = seg[sg].n;
                const float frac = seg[sg].of;
                const int16_t* __restrict mp = mod + (seg[sg].oi - int32_t(i0) - lo);
                float a1 = s16ToF(mp[1]);   // the next sample's a1 is this sample's a0
                for (uint32_t j = 0; j < n; ++j) {
                    const uint32_t i = i0 + j;
                    const float a0 = s16ToF(*mp--);
                    float s = acc[i] + krt * (a0 + (a1 - a0) * frac);
                    a1 = a0;
                    lp2 += klp * (s - lp2);
                    s = lp2;
                    {   // dap1a: read -kap, write-allpass +kap
                        const float t = s16ToF(ta[-int32_t(i)]);
                        s += -kap * t;
                        const int16_t v = toS16(s);
                        oa[-int32_t(i)] = v;
                        if (kTrack) ql |= mag(v);
                        s = kap * s + t;
                    }
                    {   // dap1b: read +kap, write-allpass -kap
                        const float t = s16ToF(tb[-int32_t(i)]);
                        s += kap * t;
                        const int16_t v = toS16(s);
                        ob[-int32_t(i)] = v;
                        if (kTrack) ql |= mag(v);
                        s = t - kap * s;
                    }
                    {   // del1
                        const int16_t v = toS16(s);
                        od[-int32_t(i)] = v;
                        if (kTrack) ql |= mag(v);
                    }
                    wet[i] = 2.0f * s;
                }
            }
            qa |= ql;
        }
        // Right tank: fixed del1 tap, lp3, dap2a, dap2b, del2; then the
        // output mix (mono send in, duplicated to L and R).
        {
            const float* __restrict wetL = gWetL;
            const int16_t* __restrict tc = r2 + (kBlock - 1);
            const int16_t* __restrict td = r3 + (kBlock - 1);
            const int16_t* __restrict te = r4 + (kBlock - 1);
            int16_t* __restrict oc = w3 + (kBlock - 1);
            int16_t* __restrict oe = w4 + (kBlock - 1);
            int16_t* __restrict od = wd2 + (kBlock - 1);
            uint32_t qr = 0;
            for (uint32_t i = 0; i < kBlock; ++i) {
                float s = acc[i] + krt * (s16ToF(tc[-int32_t(i)]));
                lp3 += klp * (s - lp3);
                s = lp3;
                {   // dap2a
                    const float t = s16ToF(td[-int32_t(i)]);
                    s += kap * t;
                    const int16_t v = toS16(s);
                    oc[-int32_t(i)] = v;
                    if (kTrack) qr |= mag(v);
                    s = t - kap * s;
                }
                {   // dap2b
                    const float t = s16ToF(te[-int32_t(i)]);
                    s += -kap * t;
                    const int16_t v = toS16(s);
                    oe[-int32_t(i)] = v;
                    if (kTrack) qr |= mag(v);
                    s = kap * s + t;
                }
                {   // del2
                    const int16_t v = toS16(s);
                    od[-int32_t(i)] = v;
                    if (kTrack) qr |= mag(v);
                }
                const float wr = 2.0f * s, wl = wetL[i];
                const float x = in[i];
                outL[i] = clampUnitFast(x + amount * (wl - x));
                outR[i] = clampUnitFast(x + amount * (wr - x));
                sumAbs += std::fabs(wl);
            }
            qa |= qr;
        }
        if (kTrack) q |= qa;
    };
    if (anyIn) run(std::false_type{});
    else run(std::true_type{});
    // Writes (disjoint spans).
    auto tapOut = [&](int16_t* ring, uint32_t off, const int16_t* buf) {
        ringOut(ring, kRMask, tapPos(off), buf, kBlock);
    };
    tapOut(A, 1214, w0);
    tapOut(A, 3397, w1);
    tapOut(A, 6088, wd1);
    tapOut(B, 0, w3);
    tapOut(B, 2526, w4);
    tapOut(B, 4724, wd2);
    // Keep the filter states finite and bounded.
    lp1_ = clampUnit(lp1);
    lp2_ = clampUnit(lp2);
    lp3_ = clampUnit(lp3);
    level_ = sumAbs / float(kBlock);
    if (anyIn || q > uint32_t(kQuiet)) quiet_ = 0;
    else if (quiet_ < 0x7FFFFFFFu) quiet_ += kBlock;
    return true;
}

// --- Limiter (spec 04 §5.3) --------------------------------------------------

void Limiter::reset() {
    env_ = 0.0f;
    gs_ = 1.0f;
    for (float& h : hist_) h = 0.0f;
    hp_ = 0;
}

namespace {
template <bool kSc>
inline float limiterRun(const float* __restrict x, const float* __restrict sc, float* __restrict out,
                        const float* __restrict dl, float& envR, float& gsR, float& gR, float ca, float cr,
                        float thr) {
    float env = envR, gs = gsR, g = gR, sum = 0.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float d = kSc ? sc[i] : x[i];
        const float a = d < 0.0f ? -d : d;
        const float c1 = a > env ? ca : cr;
        env = c1 * a + (1.0f - c1) * env;
        g = env > thr ? thr / env : 1.0f;
        const float c2 = g < gs ? ca : cr;
        gs = c2 * g + (1.0f - c2) * gs;
        // 4-sample look-ahead line.
        float y = gs * dl[i];
        if (kSc) y += (1.0f - gs) * sc[i];
        out[i] = y;
        sum += gs;
    }
    envR = env;
    gsR = gs;
    gR = g;
    return sum;
}
}  // namespace

OT_FASTRUN_FN bool Limiter::process(const float* x, const float* sc, float* out) {
    if (disabled_) {
        for (uint32_t i = 0; i < kBlock; ++i) out[i] = x[i];
        return true;
    }
    // At rest (empty look-ahead, envelope ~0, gain at its fixed point 1) a
    // silent block leaves the state unchanged and outputs zeros.
    if (thr_ > 0.0f && env_ <= 1e-30f && hist_[0] == 0.0f && hist_[1] == 0.0f && hist_[2] == 0.0f &&
        hist_[3] == 0.0f) {
        const float c2 = 1.0f < gs_ ? ca_ : cr_;
        if (c2 * 1.0f + (1.0f - c2) * gs_ == gs_) {
            bool silent = true;
            for (uint32_t i = 0; i < kBlock; ++i) silent &= x[i] == 0.0f;
            if (sc)
                for (uint32_t i = 0; i < kBlock; ++i) silent &= sc[i] == 0.0f;
            if (silent) {
                zeroFloats(out, kBlock);
                env_ = 0.0f;
                lastG_ = 1.0f;
                meanG_ = gs_;
                return false;
            }
        }
    }
    // Look-ahead: dl[i] = x[i - 4] (history first, oldest at hp_).
    float dl[kBlock + 4];
    for (uint32_t j = 0; j < 4; ++j) dl[j] = hist_[(hp_ + j) & 3u];
    for (uint32_t i = 0; i < kBlock; ++i) {
        OT_NO_LIBCALL();
        dl[i + 4] = x[i];
    }
    float g = 1.0f;
    const float sum = sc ? limiterRun<true>(x, sc, out, dl, env_, gs_, g, ca_, cr_, thr_)
                         : limiterRun<false>(x, sc, out, dl, env_, gs_, g, ca_, cr_, thr_);
    for (uint32_t j = 0; j < 4; ++j) hist_[j] = x[kBlock - 4 + j];
    hp_ = 0;
    lastG_ = g;
    meanG_ = sum / float(kBlock);
    return true;
}

// --- Line-input dynamics (spec 04 §5.6) --------------------------------------

namespace {
// log2(v) for a positive normal float, |error| < 2.1e-4 (0.0013 dB).
inline float fastLog2(float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    const float e = float(int32_t(b >> 23) - 127);
    b = (b & 0x007FFFFFu) | 0x3F800000u;
    float m;
    std::memcpy(&m, &b, 4);
    const float f = m - 1.0f;
    return e + (2.0372331e-4f + f * (1.4361024f + f * (-0.66952725f + f * (0.31222615f + f * -0.079153834f))));
}
// 2^v for -126 < v < 127, relative error < 2.1e-4 (0.002 dB).
inline float fastExp2(float v) {
    const int32_t i = int32_t(v) - (v < 0.0f ? 1 : 0);   // floor
    const float f = v - float(i);
    const float p = 0.99981218f + f * (0.69683754f + f * (0.22412730f + f * 0.079020150f));
    uint32_t b;
    std::memcpy(&b, &p, 4);
    b += uint32_t(i) << 23;
    float r;
    std::memcpy(&r, &b, 4);
    return r;
}
constexpr float kDbPerLog2 = 6.02059991f;     // 20 * log10(2)
constexpr float kLog2PerDb = 0.166096405f;    // 1 / kDbPerLog2
constexpr float kOpenLin = 0.0251188643f;     // 10^(-32/20)
constexpr float kCloseLin = 0.0125892541f;    // 10^(-38/20)
constexpr float kKneeLoLin = 0.00707945784f;  // 10^(-43/20): below it no compression
constexpr float kAttack128 = 7.39726663e-6f;  // kAttack^kBlock
constexpr float kRelease128 = 0.306836576f;   // kRelease^kBlock
static_assert(kBlock == 128, "closed-form idle() constants assume 128-sample blocks");
}  // namespace

void LineDynamics::process(const float* x, float* y) {
    float env = env_, gdb = gateDb_, cdb = compDb_;
    bool open = open_;
    for (uint32_t i = 0; i < kBlock; ++i) {
        OT_NO_LIBCALL();
        const float xi = x[i];
        const float a = xi < 0.0f ? -xi : xi;
        const float er = env * kRelease;
        env = a > er ? a : er;
        open = env >= kOpenLin ? true : (env < kCloseLin ? false : open);
        const float gt = open ? 0.0f : kFloorDb;
        gdb = gt + (gdb - gt) * (gt > gdb ? kAttack : kRelease);
        float lim = 0.0f;
        if (env > kKneeLoLin) {
            const float lvl = kDbPerLog2 * fastLog2(env);
            const float ct = staticGainDb(lvl);
            cdb = ct + (cdb - ct) * (ct < cdb ? kAttack : kRelease);
            const float over = lvl + cdb + kMakeupDb - kLimitDb;
            lim = over > 0.0f ? -over : 0.0f;
        } else {
            cdb *= kRelease;   // target 0 dB, gain rising
        }
        y[i] = xi * fastExp2((gdb + cdb + kMakeupDb + lim) * kLog2PerDb);
    }
    env_ = env;
    gateDb_ = gdb;
    compDb_ = cdb;
    open_ = open;
    settled_ = false;
}

void LineDynamics::decay() {
    env_ *= kRelease128;
    if (env_ < kCloseLin) open_ = false;
    const float gt = open_ ? 0.0f : kFloorDb;
    gateDb_ = gt + (gateDb_ - gt) * (gt > gateDb_ ? kAttack128 : kRelease128);
    compDb_ *= kRelease128;
    if (env_ < 1e-9f && !open_ && gateDb_ < kFloorDb + 0.01f && compDb_ > -0.001f) reset();
}

}  // namespace audio
}  // namespace ot
