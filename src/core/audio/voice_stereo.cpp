// Stereo sample voice path (docs/specs/2x-04 §3, docs/MINI_PLAN.md §2).
// A separate translation unit so the mono player / render in voice.cpp
// compile exactly as before (play8 +0 %).
#include "audio/voice.h"
#include "fastrun.h"
#include "audio/ring_io.h"

#include <cstring>

namespace ot {
namespace audio {

namespace {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Same helpers as voice.cpp (spec 04 §2.2 interpolation, fused output ramps).
inline int32_t interp(int32_t s0, int32_t s1, uint32_t frac) {
    return int16_t(s0 + (((s1 - s0) * int32_t(frac >> 2)) >> 14));
}
inline float clampFast(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    return (u & 0x7FFFFFFFu) <= 0x3F800000u ? v : clampUnit(v);
}
struct OutRamps {
    float s0, di, s1, ri, a0, ai, l0, li, r0, rri;
};

// Stereo output stage (Voice::renderSt fast path, 2x-04 §3.2): one envelope
// and AMP gain on both channels, balance = the stock pan gains per channel,
// sends get (L + R) / 2, o.amp = the mean of the clamped channels. o.l / o.r
// are always written (the meter peak reads them).
template <bool kD, bool kR, bool kAmp>
void fusedOutSt(const float* __restrict p, const float* __restrict pr, const float* __restrict eg, OutRamps g,
                float* __restrict ds, float* __restrict rs, VoiceOut& o, float* __restrict mL,
                float* __restrict mR) {
    float* __restrict oa = o.amp;
    float* __restrict ol = o.l;
    float* __restrict orr = o.r;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float xl = p[i] * eg[i], xr = pr[i] * eg[i];
        if (kD || kR) {
            const float m = xl + xr;   // send gains are pre-halved: (L + R) / 2
            if (kD) {
                g.s0 += g.di;
                ds[i] += m * g.s0;
            }
            if (kR) {
                g.s1 += g.ri;
                rs[i] += m * g.s1;
            }
        }
        g.a0 += g.ai;
        g.l0 += g.li;
        g.r0 += g.rri;
        const float al = clampFast(xl * g.a0), ar = clampFast(xr * g.a0);
        if (kAmp) oa[i] = 0.5f * (al + ar);
        const float l = al * g.l0, r = ar * g.r0;
        ol[i] = l;
        orr[i] = r;
        mL[i] += l;
        mR[i] += r;
    }
}

// fusedOutSt with the SVF on both channels in the same pass (OP-3): each
// channel's filter does the operations of svfRunSt in the same order (shared
// coefficient ramps), the output stage those of the general path, so the
// result is bit-identical to it. Both sends are fed (an inactive one goes to
// a scratch bus with a zero ramp).
template <int kType, bool kAmp>
void fusedOutStF(const float* __restrict p, const float* __restrict pr, const float* __restrict eg, OutRamps g,
                 Svf& sl, Svf& sr, float fmTarget, float dpTarget, float* __restrict ds, float* __restrict rs,
                 VoiceOut& o, float* __restrict mL, float* __restrict mR, float& lastL, float& lastR) {
    float* __restrict oa = o.amp;
    float* __restrict ol = o.l;
    float* __restrict orr = o.r;
    const float dfm = (fmTarget - sl.fm) / float(kBlock);
    const float ddp = (dpTarget - sl.dp) / float(kBlock);
    float f = sl.fm, d = sl.dp;
    float l = sl.lp, b = sl.bp, ip = sl.inPrev;
    float l2 = sr.lp, b2 = sr.bp, ip2 = sr.inPrev;
    float xl = 0.0f, xr = 0.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        f += dfm;
        d += ddp;
        {
            const float in = p[i] * eg[i];
            l += f * b;
            float h = (in + ip) * 0.5f - l - d * b;
            ip = in;
            b += f * h;
            const float l1 = l, b1 = b, h1 = h;
            l += f * b;
            h = in - l - d * b;
            b += f * h;
            const float y = kType == 0 ? (l + l1) : (kType == 1 ? (h + h1) : (b + b1));
            xl = clampUnitFast(y * 0.5f);
        }
        {
            const float in = pr[i] * eg[i];
            l2 += f * b2;
            float h = (in + ip2) * 0.5f - l2 - d * b2;
            ip2 = in;
            b2 += f * h;
            const float l1 = l2, b1 = b2, h1 = h;
            l2 += f * b2;
            h = in - l2 - d * b2;
            b2 += f * h;
            const float y = kType == 0 ? (l2 + l1) : (kType == 1 ? (h + h1) : (b2 + b1));
            xr = clampUnitFast(y * 0.5f);
        }
        const float m = xl + xr;   // send gains are pre-halved: (L + R) / 2
        g.s0 += g.di;
        ds[i] += m * g.s0;
        g.s1 += g.ri;
        rs[i] += m * g.s1;
        g.a0 += g.ai;
        g.l0 += g.li;
        g.r0 += g.rri;
        const float al = clampFast(xl * g.a0), ar = clampFast(xr * g.a0);
        if (kAmp) oa[i] = 0.5f * (al + ar);
        const float lo = al * g.l0, ro = ar * g.r0;
        ol[i] = lo;
        orr[i] = ro;
        mL[i] += lo;
        mR[i] += ro;
    }
    lastL = xl;
    lastR = xr;
    sl.lp = l > 16.0f ? 16.0f : (l < -16.0f ? -16.0f : l);
    sl.bp = b > 16.0f ? 16.0f : (b < -16.0f ? -16.0f : b);
    sl.inPrev = ip;
    sr.lp = l2 > 16.0f ? 16.0f : (l2 < -16.0f ? -16.0f : l2);
    sr.bp = b2 > 16.0f ? 16.0f : (b2 < -16.0f ? -16.0f : b2);
    sr.inPrev = ip2;
    sl.fm = sr.fm = fmTarget;
    sl.dp = sr.dp = dpTarget;
}

// Peak |post-balance output| of a stereo block (meters; every 4th frame of
// both channels, the same number of reads as the mono every-2nd-sample peak).
// |x| order = the order of (bits & 0x7FFFFFFF) for finite floats.
inline float stereoPeak(const VoiceOut& o) {
    const u32_alias* l = reinterpret_cast<const u32_alias*>(o.l);
    const u32_alias* r = reinterpret_cast<const u32_alias*>(o.r);
    uint32_t m = 0;
    for (uint32_t i = 0; i < kBlock; i += 4) {
        const uint32_t a = l[i] & 0x7FFFFFFFu, b = r[i] & 0x7FFFFFFFu;
        m = a > m ? a : m;
        m = b > m ? b : m;
    }
    float f;
    std::memcpy(&f, &m, 4);
    return f;
}

// Svf::process on both channels in one loop (shared coefficient ramps; per
// channel the same operations in the same order as Svf::run, so each channel
// is bit-identical to a mono filter). Requires equal fm / dp on both.
template <int kType>
void svfRunSt(Svf& sl, Svf& sr, float* __restrict xl, float* __restrict xr, float fmTarget, float dpTarget) {
    const float dfm = (fmTarget - sl.fm) / float(kBlock);
    const float ddp = (dpTarget - sl.dp) / float(kBlock);
    float f = sl.fm, d = sl.dp;
    float l = sl.lp, b = sl.bp, ip = sl.inPrev;
    float l2 = sr.lp, b2 = sr.bp, ip2 = sr.inPrev;
    for (uint32_t i = 0; i < kBlock; ++i) {
        f += dfm;
        d += ddp;
        {
            const float in = xl[i];
            l += f * b;
            float h = (in + ip) * 0.5f - l - d * b;
            ip = in;
            b += f * h;
            const float l1 = l, b1 = b, h1 = h;
            l += f * b;
            h = in - l - d * b;
            b += f * h;
            const float y = kType == 0 ? (l + l1) : (kType == 1 ? (h + h1) : (b + b1));
            xl[i] = clampUnitFast(y * 0.5f);
        }
        {
            const float in = xr[i];
            l2 += f * b2;
            float h = (in + ip2) * 0.5f - l2 - d * b2;
            ip2 = in;
            b2 += f * h;
            const float l1 = l2, b1 = b2, h1 = h;
            l2 += f * b2;
            h = in - l2 - d * b2;
            b2 += f * h;
            const float y = kType == 0 ? (l2 + l1) : (kType == 1 ? (h + h1) : (b2 + b1));
            xr[i] = clampUnitFast(y * 0.5f);
        }
    }
    sl.lp = l > 16.0f ? 16.0f : (l < -16.0f ? -16.0f : l);
    sl.bp = b > 16.0f ? 16.0f : (b < -16.0f ? -16.0f : b);
    sl.inPrev = ip;
    sr.lp = l2 > 16.0f ? 16.0f : (l2 < -16.0f ? -16.0f : l2);
    sr.bp = b2 > 16.0f ? 16.0f : (b2 < -16.0f ? -16.0f : b2);
    sr.inPrev = ip2;
    sl.fm = sr.fm = fmTarget;
    sl.dp = sr.dp = dpTarget;
}

void svfStereo(Svf& sl, Svf& sr, float* xl, float* xr, int type, float fmTarget, float dpTarget) {
    if (sl.fm != sr.fm || sl.dp != sr.dp) {   // not in step (never expected): one filter each
        sl.process(xl, kBlock, type, fmTarget, dpTarget);
        sr.process(xr, kBlock, type, fmTarget, dpTarget);
    } else if (type == 0) {
        svfRunSt<0>(sl, sr, xl, xr, fmTarget, dpTarget);
    } else if (type == 1) {
        svfRunSt<1>(sl, sr, xl, xr, fmTarget, dpTarget);
    } else {
        svfRunSt<2>(sl, sr, xl, xr, fmTarget, dpTarget);
    }
}

// Overdrive (§4.5) of one stereo channel: pre-gain then 3u / 1-(2-4u)^2/4 / hard 1.
inline void overdriveBlock(float* __restrict x, float gain) {
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float y = x[i] * gain;
        const float a = y < 0.0f ? -y : y;
        float v;
        if (a < 0.25f) v = 3.0f * a;
        else if (a < 0.5f) {
            const float t = 2.0f - 4.0f * a;
            v = 1.0f - t * t * 0.25f;
        } else v = 1.0f;
        x[i] = y < 0.0f ? -v : v;
    }
}

}  // namespace

// --- stereo player / block render (2x-04 §3) ---------------------------------
//
// A stereo note (stereo_: planar instrument, play mode 0..5) runs the same
// player arithmetic as renderPlayer() on both channels with one playhead:
// start / loop / end points, slices, pitch, glide and reverse act on L and R
// identically. The 32-frame read window holds both channels.

void Voice::renderPlayerSt(float* outL, float* outR, uint32_t n) {
    if (!stereo_) {   // mono note in a stereo block (retrigger): same signal on both channels
        renderPlayer(outL, n);
        std::memcpy(outR, outL, n * sizeof(float));
        return;
    }
    if (!playing_ || !data_ || !dataR_ || frames_ == 0) {
        zeroFloats(outL, n);
        zeroFloats(outR, n);
        return;
    }
    const int16_t* dL = data_;
    const int16_t* dR = dataR_;
    const int32_t last = int32_t(frames_) - 1;
    const float* tab = pitchTable();
    const float rmin = tab[0], rmax = tab[kPitchTableSize - 1];
    float rate = clampf(targetRate_ + glideOff_, rmin, rmax);
    uint32_t si = uint32_t(rate);
    uint32_t sf = uint32_t((rate - float(si)) * 65535.0f);
    int32_t pos = pos_;
    uint32_t frac = frac_;
    int32_t dir = dir_;

    auto glide = [&]() {
        if (glideLeft_ == 0) return;
        glideOff_ += glideInc_;
        if (--glideLeft_ == 0) glideOff_ = 0.0f;
        rate = clampf(targetRate_ + glideOff_, rmin, rmax);
        si = uint32_t(rate);
        sf = uint32_t((rate - float(si)) * 65535.0f);
    };
    auto advance = [&]() {
        frac += sf;
        pos += dir * int32_t(si);
        if (frac > 65534u) {
            frac -= 65535u;
            pos += dir;
        }
    };
    auto nb = [&](int32_t p) {
        int32_t q = p + dir;
        return q < 0 ? 0 : (q > last ? last : q);
    };
    // Interleaved window: win[j] = L (low half) | R (high half) of frame
    // wb + j, so one word load gives both channels of a frame.
    constexpr int32_t kWin = 32;
    uint32_t win[kWin];
    uint32_t wb = 0x80000000u;
    const StreamSource* const st = stream_;
    const bool useWin = int32_t(frames_) >= kWin && (st || (rate < 4.0f && targetRate_ < 4.0f));
    auto fill = [&](int32_t b) {
        if (b > int32_t(frames_) - kWin) b = int32_t(frames_) - kWin;
        if (b < 0) b = 0;
        wb = uint32_t(b);
        if (st) {   // streamed: both channels from the head / loop head / ring
            int16_t tl[kWin], tr[kWin];
            streamRead(*st, ring_, streamGen_, b, uint32_t(kWin), tl, tr);
            for (int32_t j = 0; j < kWin; ++j) win[j] = uint16_t(tl[j]) | (uint32_t(uint16_t(tr[j])) << 16);
            return;
        }
        OT_SDRAM_ACCESS(kWin + 2);
        const int16_t* l = dL + b;
        const int16_t* r = dR + b;
        uint32_t* w = win;
        uint32_t k = uint32_t(kWin);
        if ((reinterpret_cast<uintptr_t>(l) ^ reinterpret_cast<uintptr_t>(r)) & 2u) {
            OT_SDRAM_ACCESS(kWin - 2);   // halfword reads (odd length: the blocks differ in alignment)
            do {
                *w++ = uint16_t(*l++) | (uint32_t(uint16_t(*r++)) << 16);
            } while (--k);
            return;
        }
        if (reinterpret_cast<uintptr_t>(l) & 2u) {
            *w++ = uint16_t(*l++) | (uint32_t(uint16_t(*r++)) << 16);
            --k;
        }
        const u32_alias* lw = reinterpret_cast<const u32_alias*>(l);
        const u32_alias* rw = reinterpret_cast<const u32_alias*>(r);
        for (; k >= 2; k -= 2) {
            const uint32_t a = *lw++, c = *rw++;   // two frames per channel
            w[0] = (a & 0xFFFFu) | (c << 16);
            w[1] = (a >> 16) | (c & 0xFFFF0000u);
            w += 2;
        }
        if (k) *w = uint16_t(*reinterpret_cast<const int16_t*>(lw)) |
                    (uint32_t(uint16_t(*reinterpret_cast<const int16_t*>(rw))) << 16);
    };
    // Window offset of idx (refilled when idx leaves the window); -1 = direct read.
    auto at = [&](int32_t idx) -> int32_t {
        if (!useWin) return -1;
        uint32_t off = uint32_t(idx) - wb;
        if (off >= uint32_t(kWin)) {
            fill(dir > 0 ? idx - 1 : idx - (kWin - 2));
            off = uint32_t(idx) - wb;
        }
        return int32_t(off);
    };
    // One output frame at p, interpolated towards q when ip.
    auto frame = [&](uint32_t i, int32_t p, int32_t q, bool ip) {
        int32_t l0, r0;
        const int32_t o0 = at(p);
        if (o0 < 0) {
            OT_SDRAM_ACCESS(2);
            l0 = dL[p];
            r0 = dR[p];
        } else {
            l0 = int16_t(win[o0]);
            r0 = int32_t(win[o0]) >> 16;
        }
        if (!ip) {
            outL[i] = s16ToF(l0);
            outR[i] = s16ToF(r0);
            return;
        }
        int32_t l1, r1;
        const int32_t o1 = at(q);
        if (o1 < 0) {
            OT_SDRAM_ACCESS(2);
            l1 = dL[q];
            r1 = dR[q];
        } else {
            l1 = int16_t(win[o1]);
            r1 = int32_t(win[o1]) >> 16;
        }
        outL[i] = s16ToF(interp(l0, l1, frac));
        outR[i] = s16ToF(interp(r0, r1, frac));
    };
    const bool fast = useWin && aa_ && glideLeft_ == 0;
    auto fastRun = [&](uint32_t& i, int32_t lim) {
        if (uint32_t(pos) - wb >= uint32_t(kWin) - 1u) fill(pos);   // also pos == wb - 1 (loop wrap)
        const int32_t wend = int32_t(wb) + kWin - 1;
        const int32_t stop = lim < wend ? lim : wend;
        uint32_t k = uint32_t(stop - pos) / (si + 1u);
        if (k == 0) k = 1;
        if (k > n - i) k = n - i;
        i += k;
        int32_t q = pos - int32_t(wb);
        uint32_t f = frac;
        float* ol = outL + (i - k);
        float* orr = outR + (i - k);
        do {
            const uint32_t a = win[q], c = win[q + 1];
            *ol++ = s16ToF(interp(int16_t(a), int16_t(c), f));
            *orr++ = s16ToF(interp(int32_t(a) >> 16, int32_t(c) >> 16, f));
            f += sf;
            q += int32_t(si);
            if (f > 65534u) {
                f -= 65535u;
                q += 1;
            }
        } while (--k);
        pos = q + int32_t(wb);
        frac = f;
    };

    const uint8_t mode = mode_;
    if (mode >= uint8_t(PlayMode::ForwardLoop) && mode <= uint8_t(PlayMode::PingPongLoop) && loop2_ > loop1_) {
        const int32_t L1 = loop1_, L2 = loop2_, rl = L2 - L1;
        const int32_t lim = L2 < last ? L2 : last;
        for (uint32_t i = 0; i < n; ++i) {
            if (fast && dir > 0) {
                while (i < n && pos >= 0 && pos < lim) {
                    fastRun(i, lim);
                    if (pos >= L2) {
                        const int32_t ex = (pos - L2) % rl;
                        if (mode == uint8_t(PlayMode::ForwardLoop)) {
                            pos = L1 + ex;
                        } else {
                            dir = -1;
                            pos = L2 - 1 - ex;
                            break;
                        }
                    }
                }
                if (i >= n) break;
            }
            glide();
            if (pos < 0 || pos > last) {
                playing_ = false;
                zeroFloats(outL + i, n - i);
                zeroFloats(outR + i, n - i);
                break;
            }
            frame(i, pos, nb(pos), aa_);
            advance();
            if (dir > 0) {
                if (pos >= L2) {
                    const int32_t ex = (pos - L2) % rl;
                    if (mode == uint8_t(PlayMode::ForwardLoop)) {
                        pos = L1 + ex;
                    } else {
                        dir = -1;
                        pos = L2 - 1 - ex;
                    }
                }
            } else if (pos < L1) {
                const int32_t ex = (L1 - 1 - pos) % rl;
                if (mode == uint8_t(PlayMode::PingPongLoop)) {
                    dir = 1;
                    pos = L1 + ex;
                } else {
                    pos = L2 - 1 - ex;
                }
            }
        }
    } else {
        // One-shot / slice / beat-slice (and degenerate loops).
        const int32_t s = start_, e = end_;
        for (uint32_t i = 0; i < n; ++i) {
            if (fast && dir > 0) {
                while (i < n && pos >= 0 && pos < e - 1) fastRun(i, e - 1);
                if (i >= n) break;
            }
            glide();
            if (dir > 0 ? pos >= e : pos < s) {
                playing_ = false;
                zeroFloats(outL + i, n - i);
                zeroFloats(outR + i, n - i);
                break;
            }
            const int32_t q = pos + dir;
            frame(i, pos, q, aa_ && (dir > 0 ? q < e : q >= s));
            advance();
        }
    }
    pos_ = pos;
    frac_ = frac;
    dir_ = int8_t(dir);
    if (st && ring_) {
        if (playing_) ring_->need = uint32_t(pos > 0 ? pos : 0);
        else ring_->src = nullptr;
    }
}

OT_FASTRUN_FN bool Voice::renderSt(const VoiceControl& c, VoiceOut& o, float* dsend, float* rsend, float* mixL, float* mixR) {
    if (!lastSt_) {   // mono -> stereo: the right channel state continues from the left one
        svfR_ = svf_;
        filtLastR_ = filtLast_;
        lastSt_ = true;
    }
    float xl[kBlock], xr[kBlock], eg[kBlock];
    const float hs = c.headroom ? 0.5f : 1.0f;   // extra headroom (§1.4)
    uint32_t done = 0;
    for (uint32_t g = 0; g < kBlock; g += 8) {
        const int ev = env_.group(eg + g);
        if (ev) {
            if (g > done) renderPlayerSt(xl + done, xr + done, g - done);
            done = g;
            if (ev & VolumeEnvelope::kEvForcedDone) startNote();
            if (ev & VolumeEnvelope::kEvIdle) {
                playing_ = false;
                if (ring_) ring_->src = nullptr;   // released: the stream service frees the ring
            }
        }
    }
    if (done < kBlock) renderPlayerSt(xl + done, xr + done, kBlock - done);
    if (!playing_ && !pending_.valid) env_.stop();
    float* __restrict oa = o.amp;
    float* __restrict ol = o.l;
    float* __restrict orr = o.r;
    if (hs == 1.0f && mask_ == 0xFFFF && !(odGain_ > 0.0f) && !filtOn_ && !filtXfade_ && mixL && mixR) {
        filtLast_ = xl[kBlock - 1] * eg[kBlock - 1];
        filtLastR_ = xr[kBlock - 1] * eg[kBlock - 1];
        OutRamps g;
        g.di = sendD_.begin(g.s0);
        g.ri = sendR_.begin(g.s1);
        g.ai = amp_.begin(g.a0);
        g.li = panL_.begin(g.l0);
        g.rri = panR_.begin(g.r0);
        const bool sd = g.s0 != 0.0f || g.di != 0.0f, sr = g.s1 != 0.0f || g.ri != 0.0f;
        sendsTouched_ = uint8_t((sd ? 1 : 0) | (sr ? 2 : 0));
        g.s0 *= 0.5f;   // (L + R) / 2 into the sends (exact halving)
        g.di *= 0.5f;
        g.s1 *= 0.5f;
        g.ri *= 0.5f;
        switch ((sd ? 1 : 0) | (sr ? 2 : 0) | (c.ampTap ? 4 : 0)) {
            case 0: fusedOutSt<false, false, false>(xl, xr, eg, g, dsend, rsend, o, mixL, mixR); break;
            case 1: fusedOutSt<true, false, false>(xl, xr, eg, g, dsend, rsend, o, mixL, mixR); break;
            case 2: fusedOutSt<false, true, false>(xl, xr, eg, g, dsend, rsend, o, mixL, mixR); break;
            case 3: fusedOutSt<true, true, false>(xl, xr, eg, g, dsend, rsend, o, mixL, mixR); break;
            case 4: fusedOutSt<false, false, true>(xl, xr, eg, g, dsend, rsend, o, mixL, mixR); break;
            case 5: fusedOutSt<true, false, true>(xl, xr, eg, g, dsend, rsend, o, mixL, mixR); break;
            case 6: fusedOutSt<false, true, true>(xl, xr, eg, g, dsend, rsend, o, mixL, mixR); break;
            default: fusedOutSt<true, true, true>(xl, xr, eg, g, dsend, rsend, o, mixL, mixR); break;
        }
        peak_ = stereoPeak(o);
        return true;
    }
    if (hs == 1.0f && mask_ == 0xFFFF && !(odGain_ > 0.0f) && filtOn_ && !filtXfade_ && mixL && mixR &&
        svf_.fm == svfR_.fm && svf_.dp == svfR_.dp) {
        // Filter only: envelope, both filters, sends, volume, balance and
        // the dry-bus sum in one pass.
        OutRamps g;
        g.di = sendD_.begin(g.s0);
        g.ri = sendR_.begin(g.s1);
        g.ai = amp_.begin(g.a0);
        g.li = panL_.begin(g.l0);
        g.rri = panR_.begin(g.r0);
        const bool sd = g.s0 != 0.0f || g.di != 0.0f, sr = g.s1 != 0.0f || g.ri != 0.0f;
        sendsTouched_ = uint8_t((sd ? 1 : 0) | (sr ? 2 : 0));
        g.s0 *= 0.5f;   // (L + R) / 2 into the sends (exact halving)
        g.di *= 0.5f;
        g.s1 *= 0.5f;
        g.ri *= 0.5f;
        float scratch[kBlock];
        if (!sd || !sr) zeroFloats(scratch, kBlock);
        float* ds = sd ? dsend : scratch;
        float* rs = sr ? rsend : scratch;
        const int t = filtType_ == 0 ? 0 : (filtType_ == 1 ? 1 : 2);
        typedef void (*Fn)(const float*, const float*, const float*, OutRamps, Svf&, Svf&, float, float, float*,
                           float*, VoiceOut&, float*, float*, float&, float&);
        static const Fn kFn[3][2] = {{&fusedOutStF<0, false>, &fusedOutStF<0, true>},
                                     {&fusedOutStF<1, false>, &fusedOutStF<1, true>},
                                     {&fusedOutStF<2, false>, &fusedOutStF<2, true>}};
        kFn[t][c.ampTap ? 1 : 0](xl, xr, eg, g, svf_, svfR_, fmTarget_, dpTarget_, ds, rs, o, mixL, mixR, filtLast_,
                                 filtLastR_);
        peak_ = stereoPeak(o);
        return true;
    }
    if (hs == 1.0f) {
        for (uint32_t i = 0; i < kBlock; ++i) {
            xl[i] *= eg[i];
            xr[i] *= eg[i];
        }
    } else {
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float e = eg[i] * hs;
            xl[i] *= e;
            xr[i] *= e;
        }
    }
    // Bit crusher, overdrive, filter: per channel, same settings (2x-04 §3.2).
    if (mask_ != 0xFFFF) {
        // Round to the nearest step (mid-tread), as the mono voice: quiet
        // signals and release tails stay silent (no floor hiss).
        const int32_t m = int32_t(int16_t(mask_));
        const int32_t half = (((~m) & 0xFFFF) + 1) >> 1;
        for (uint32_t i = 0; i < kBlock; ++i) {
            xl[i] = float((int32_t(toS16(xl[i])) + half) & m) * (1.0f / 32768.0f);
            xr[i] = float((int32_t(toS16(xr[i])) + half) & m) * (1.0f / 32768.0f);
        }
    }
    if (odGain_ > 0.0f) {
        overdriveBlock(xl, odGain_);
        overdriveBlock(xr, odGain_);
    }
    if (filtOn_) {
        svfStereo(svf_, svfR_, xl, xr, filtType_, fmTarget_, dpTarget_);
    }
    if (filtXfade_) {
        const float lastL = filtLast_, lastR = filtLastR_;
        for (uint32_t i = 0; i < kBlock; ++i) {
            xl[i] = (float(i + 1) * xl[i] + float(127 - i) * lastL) * (1.0f / 128.0f);
            xr[i] = (float(i + 1) * xr[i] + float(127 - i) * lastR) * (1.0f / 128.0f);
        }
        filtXfade_ = false;
    }
    filtLast_ = xl[kBlock - 1];
    filtLastR_ = xr[kBlock - 1];
    // Sends: (L + R) / 2, post-filter, pre-volume.
    float s0, s1;
    const float di = sendD_.begin(s0);
    const float ri = sendR_.begin(s1);
    const bool sd = s0 != 0.0f || di != 0.0f, sr = s1 != 0.0f || ri != 0.0f;
    sendsTouched_ = uint8_t((sd ? 1 : 0) | (sr ? 2 : 0));
    if (sd || sr) {
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float m = 0.5f * (xl[i] + xr[i]);
            s0 += di;
            s1 += ri;
            if (sd) dsend[i] += m * s0;
            if (sr) rsend[i] += m * s1;
        }
    }
    // Volume, then balance: the stock pan gains scale each channel.
    float a0, l0, r0;
    const float ai = amp_.begin(a0);
    const float li = panL_.begin(l0);
    const float rri = panR_.begin(r0);
    for (uint32_t i = 0; i < kBlock; ++i) {
        a0 += ai;
        l0 += li;
        r0 += rri;
        const float al = clampFast(xl[i] * a0), ar = clampFast(xr[i] * a0);
        ol[i] = al * l0;
        orr[i] = ar * r0;
        if (c.ampTap) oa[i] = 0.5f * (al + ar);
    }
    if (mixL && mixR) {
        for (uint32_t i = 0; i < kBlock; ++i) {
            mixL[i] += ol[i];
            mixR[i] += orr[i];
        }
    }
    peak_ = stereoPeak(o);
    return true;
}

}  // namespace audio
}  // namespace ot
