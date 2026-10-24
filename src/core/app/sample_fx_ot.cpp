// OT fork Sample Editor effects (after TRANSCODE / FRAMELOSS):
//
//  Transcode  telephone / radio codec generation loss: resample to a codec
//             rate, then 1..16 tandem passes of G.711 (u-law, A-law) or G.726
//             ADPCM (40/32/24/16 kbit/s, the ITU algorithm in the classic Sun
//             g72x structure), with bit errors on the codes and lost 10 ms
//             packets (silence, repeat or glitch concealment).
//  DCT Crush  short-time spectrum crush: each 50 %-overlap frame keeps only
//             its strongest bins, quantised to a few bits, and can freeze (the
//             previous frame again): low-bitrate MP3-like artefacts.
//
// Pure functions on mono int16, no allocation (state on the stack).
#include "app/sample_fx_ot.h"

#include <cmath>
#include <cstring>

#include "fmath.h"

namespace ot {
namespace sfx {

namespace {

inline int16_t sat16(int32_t v) { return int16_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

// --- G.711 ------------------------------------------------------------------

uint8_t linearToUlaw(int16_t pcm) {
    constexpr int kBias = 0x84, kClip = 32635;
    int s = pcm;
    const int sign = (s >> 8) & 0x80;
    if (sign) s = -s;
    if (s > kClip) s = kClip;
    s += kBias;
    int exp = 7;
    for (int mask = 0x4000; (s & mask) == 0 && exp > 0; mask >>= 1) --exp;
    const int mant = (s >> (exp + 3)) & 0x0F;
    return uint8_t(~(sign | (exp << 4) | mant));
}

int16_t ulawToLinear(uint8_t u) {
    u = uint8_t(~u);
    const int t = (((u & 0x0F) << 3) + 0x84) << ((u & 0x70) >> 4);
    return int16_t((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

uint8_t linearToAlaw(int16_t pcm) {
    int s = pcm >> 3;   // 13-bit
    int mask;
    if (s >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        s = -s - 1;
    }
    static const int16_t kSegEnd[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
    int seg = 0;
    while (seg < 8 && s > kSegEnd[seg]) ++seg;
    if (seg >= 8) return uint8_t(0x7F ^ mask);
    int a = seg << 4;
    a |= seg < 2 ? (s >> 1) & 0x0F : (s >> seg) & 0x0F;
    return uint8_t(a ^ mask);
}

int16_t alawToLinear(uint8_t a) {
    a ^= 0x55;
    int t = (a & 0x0F) << 4;
    const int seg = (a & 0x70) >> 4;
    if (seg == 0) t += 8;
    else if (seg == 1) t += 0x108;
    else t = (t + 0x108) << (seg - 1);
    return int16_t((a & 0x80) ? t : -t);
}

// --- G.726 (Sun g72x structure) ---------------------------------------------

const int16_t kPower2[15] = {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000};

int quan(int val, const int16_t* table, int size) {
    int i = 0;
    while (i < size && val >= table[i]) ++i;
    return i;
}

struct G72x {
    int32_t yl = 34816;
    int16_t yu = 544, dms = 0, dml = 0, ap = 0;
    int16_t a[2] = {0, 0}, b[6] = {0, 0, 0, 0, 0, 0}, pk[2] = {0, 0};
    int16_t dq[6] = {32, 32, 32, 32, 32, 32}, sr[2] = {32, 32};
    uint8_t td = 0;
};

int fmult(int an, int srn) {
    const int anmag = an > 0 ? an : ((-an) & 0x1FFF);
    const int anexp = quan(anmag, kPower2, 15) - 6;
    const int anmant = anmag == 0 ? 32 : (anexp >= 0 ? anmag >> anexp : anmag << -anexp);
    const int wanexp = anexp + ((srn >> 6) & 0xF) - 13;
    const int wanmant = (anmant * (srn & 077) + 0x30) >> 4;
    const int ret = wanexp >= 0 ? ((wanmant << wanexp) & 0x7FFF) : (wanmant >> -wanexp);
    return (an ^ srn) < 0 ? -ret : ret;
}

int predictorZero(const G72x& s) {
    int sezi = fmult(s.b[0] >> 2, s.dq[0]);
    for (int i = 1; i < 6; ++i) sezi += fmult(s.b[i] >> 2, s.dq[i]);
    return sezi;
}

int predictorPole(const G72x& s) { return fmult(s.a[1] >> 2, s.sr[1]) + fmult(s.a[0] >> 2, s.sr[0]); }

int stepSize(const G72x& s) {
    if (s.ap >= 256) return s.yu;
    int y = int(s.yl >> 6);
    const int dif = s.yu - y, al = s.ap >> 2;
    if (dif > 0) y += (dif * al) >> 6;
    else if (dif < 0) y += (dif * al + 0x3F) >> 6;
    return y;
}

int quantize(int d, int y, const int16_t* table, int size) {
    const int dqm = d < 0 ? -d : d;
    const int exp = quan(dqm >> 1, kPower2, 15);
    const int mant = ((dqm << 7) >> exp) & 0x7F;
    const int dl = (exp << 7) + mant;
    const int dln = dl - (y >> 2);
    const int i = quan(dln, table, size);
    if (size == 1) return d < 0 ? 3 - i : i;   // 16 kbit/s: no zero rule
    if (d < 0) return (size << 1) + 1 - i;
    if (i == 0) return (size << 1) + 1;
    return i;
}

int reconstruct(bool sign, int dqln, int y) {
    const int dql = dqln + (y >> 2);
    if (dql < 0) return sign ? -0x8000 : 0;
    const int dex = (dql >> 7) & 15;
    const int dqt = 128 + (dql & 127);
    const int dq = (dqt << 7) >> (14 - dex);
    return sign ? dq - 0x8000 : dq;
}

void update(int codeSize, int y, int wi, int fi, int dq, int sr, int dqsez, G72x& s) {
    const int pk0 = dqsez < 0 ? 1 : 0;
    int mag = dq & 0x7FFF;
    const int ylint = int(s.yl >> 15), ylfrac = int((s.yl >> 10) & 0x1F);
    const int thr1 = (32 + ylfrac) << ylint;
    const int thr2 = ylint > 9 ? 31 << 10 : thr1;
    const int dqthr = (thr2 + (thr2 >> 1)) >> 1;
    const int tr = (s.td != 0 && mag > dqthr) ? 1 : 0;
    int yu = y + ((wi - y) >> 5);
    yu = yu < 544 ? 544 : (yu > 5120 ? 5120 : yu);
    s.yu = int16_t(yu);
    s.yl += s.yu + ((-s.yl) >> 6);
    int a2p = s.a[1];
    if (tr == 1) {
        s.a[0] = s.a[1] = 0;
        for (int i = 0; i < 6; ++i) s.b[i] = 0;
        a2p = 0;
    } else {
        const int pks1 = pk0 ^ s.pk[0];
        a2p = s.a[1] - (s.a[1] >> 7);
        if (dqsez != 0) {
            const int fa1 = pks1 ? s.a[0] : -s.a[0];
            if (fa1 < -8191) a2p -= 0x100;
            else if (fa1 > 8191) a2p += 0xFF;
            else a2p += fa1 >> 5;
            if (pk0 ^ s.pk[1]) {
                if (a2p <= -12160) a2p = -12288;
                else if (a2p >= 12416) a2p = 12288;
                else a2p -= 0x80;
            } else if (a2p <= -12416) {
                a2p = -12288;
            } else if (a2p >= 12160) {
                a2p = 12288;
            } else {
                a2p += 0x80;
            }
        }
        s.a[1] = int16_t(a2p);
        int a1 = s.a[0] - (s.a[0] >> 8);
        if (dqsez != 0) a1 += pks1 == 0 ? 192 : -192;
        const int a1ul = 15360 - a2p;
        a1 = a1 < -a1ul ? -a1ul : (a1 > a1ul ? a1ul : a1);
        s.a[0] = int16_t(a1);
        for (int i = 0; i < 6; ++i) {
            int bi = s.b[i] - (codeSize == 5 ? (s.b[i] >> 9) : (s.b[i] >> 8));
            if (dq & 0x7FFF) bi += ((dq ^ s.dq[i]) >= 0) ? 128 : -128;
            s.b[i] = int16_t(bi);
        }
    }
    for (int i = 5; i > 0; --i) s.dq[i] = s.dq[i - 1];
    if (mag == 0) {
        s.dq[0] = int16_t(dq >= 0 ? 0x20 : 0xFC20);
    } else {
        const int exp = quan(mag, kPower2, 15);
        s.dq[0] = int16_t(dq >= 0 ? (exp << 6) + ((mag << 6) >> exp) : (exp << 6) + ((mag << 6) >> exp) - 0x400);
    }
    s.sr[1] = s.sr[0];
    if (sr == 0) {
        s.sr[0] = 0x20;
    } else if (sr > 0) {
        const int exp = quan(sr, kPower2, 15);
        s.sr[0] = int16_t((exp << 6) + ((sr << 6) >> exp));
    } else if (sr > -32768) {
        mag = -sr;
        const int exp = quan(mag, kPower2, 15);
        s.sr[0] = int16_t((exp << 6) + ((mag << 6) >> exp) - 0x400);
    } else {
        s.sr[0] = int16_t(0xFC20);
    }
    s.pk[1] = s.pk[0];
    s.pk[0] = int16_t(pk0);
    s.td = (tr == 1) ? 0 : (a2p < -11776 ? 1 : 0);
    s.dms = int16_t(s.dms + ((fi - s.dms) >> 5));
    s.dml = int16_t(s.dml + (((fi << 2) - s.dml) >> 7));
    if (tr == 1) {
        s.ap = 256;
    } else if (y < 1536 || s.td == 1) {
        s.ap = int16_t(s.ap + ((0x200 - s.ap) >> 4));
    } else {
        int diff = (s.dms << 2) - s.dml;
        if (diff < 0) diff = -diff;
        s.ap = int16_t(s.ap + (diff >= (s.dml >> 3) ? ((0x200 - s.ap) >> 4) : ((-s.ap) >> 4)));
    }
}

struct RateTables {
    int bits;
    const int16_t* qtab;
    int qsize;
    const int16_t* dqln;
    const int16_t* wi;
    const int16_t* fi;
    int wiShift;
};

const int16_t kQ16[1] = {261};
const int16_t kDq16[4] = {116, 365, 365, 116};
const int16_t kWi16[4] = {-704, 14048, 14048, -704};
const int16_t kFi16[4] = {0, 0xE00, 0xE00, 0};
const int16_t kQ24[3] = {8, 218, 331};
const int16_t kDq24[8] = {-2048, 135, 273, 373, 373, 273, 135, -2048};
const int16_t kWi24[8] = {-128, 960, 4384, 18624, 18624, 4384, 960, -128};
const int16_t kFi24[8] = {0, 0x200, 0x400, 0xE00, 0xE00, 0x400, 0x200, 0};
const int16_t kQ32[7] = {-124, 80, 178, 246, 300, 349, 400};
const int16_t kDq32[16] = {-2048, 4, 135, 213, 273, 323, 373, 425, 425, 373, 323, 273, 213, 135, 4, -2048};
const int16_t kWi32[16] = {-12, 18, 41, 64, 112, 198, 355, 1122, 1122, 355, 198, 112, 64, 41, 18, -12};
const int16_t kFi32[16] = {0, 0, 0, 0x200, 0x200, 0x200, 0x600, 0xE00, 0xE00, 0x600, 0x200, 0x200, 0x200, 0, 0, 0};
const int16_t kQ40[15] = {-122, -16, 68, 139, 198, 250, 298, 339, 378, 413, 445, 475, 502, 528, 553};
const int16_t kDq40[32] = {-2048, -66, 28,  104, 169, 224, 274, 318, 358, 395, 429, 459, 488, 514, 539, 566,
                           566,   539, 514, 488, 459, 429, 395, 358, 318, 274, 224, 169, 104, 28,  -66, -2048};
const int16_t kWi40[32] = {448,   448,   768,   1248,  1280,  1312, 1856, 3200, 4512, 5728, 7008,
                           8960,  11456, 14080, 16928, 22272, 22272, 16928, 14080, 11456, 8960, 7008,
                           5728,  4512,  3200,  1856,  1312,  1280,  1248,  768,   448,   448};
const int16_t kFi40[32] = {0,     0,     0,     0,     0,     0x200, 0x200, 0x200, 0x200, 0x200, 0x400,
                           0x600, 0x800, 0xA00, 0xC00, 0xC00, 0xC00, 0xC00, 0xA00, 0x800, 0x600, 0x400,
                           0x200, 0x200, 0x200, 0x200, 0x200, 0,     0,     0,     0,     0};

const RateTables kRates[4] = {
    {5, kQ40, 15, kDq40, kWi40, kFi40, 0},
    {4, kQ32, 7, kDq32, kWi32, kFi32, 5},
    {3, kQ24, 3, kDq24, kWi24, kFi24, 0},
    {2, kQ16, 1, kDq16, kWi16, kFi16, 0},
};

int g726Encode(G72x& s, const RateTables& r, int sl) {
    sl >>= 2;   // 14-bit
    const int sezi = predictorZero(s);
    const int sez = sezi >> 1;
    const int se = (sezi + predictorPole(s)) >> 1;
    const int d = sl - se;
    const int y = stepSize(s);
    const int i = quantize(d, y, r.qtab, r.qsize);
    const int dq = reconstruct((i & (1 << (r.bits - 1))) != 0, r.dqln[i], y);
    const int sr = dq < 0 ? se - (dq & 0x3FFF) : se + dq;
    const int dqsez = sr + sez - se;
    update(r.bits, y, r.wi[i] * (1 << r.wiShift), r.fi[i], dq, sr, dqsez, s);
    return i;
}

int g726Decode(G72x& s, const RateTables& r, int i) {
    i &= (1 << r.bits) - 1;
    const int sezi = predictorZero(s);
    const int sez = sezi >> 1;
    const int se = (sezi + predictorPole(s)) >> 1;
    const int y = stepSize(s);
    const int dq = reconstruct((i & (1 << (r.bits - 1))) != 0, r.dqln[i], y);
    const int sr = dq < 0 ? se - (dq & 0x3FFF) : se + dq;
    const int dqsez = sr - se + sez;
    update(r.bits, y, r.wi[i] * (1 << r.wiShift), r.fi[i], dq, sr, dqsez, s);
    int out = sr * 4;
    return out > 32767 ? 32767 : (out < -32768 ? -32768 : out);
}

struct Lcg {
    uint32_t s = 0x1234567u;
    uint32_t next() { return s = s * 1664525u + 1013904223u; }
    float uni() { return float(next() >> 8) * (1.0f / 16777216.0f); }
};

// One codec generation: encode, corrupt, decode.
struct Gen {
    G72x enc, dec;
    int16_t prev = 0;   // the previous input of this pass (the re-sampling hop)
};

constexpr int kMaxGens = 16;

int16_t codecChain(int codec, int gens, float ber, int16_t x, Gen* g, Lcg& rng) {
    int v = x;
    for (int k = 0; k < gens; ++k) {
        if (k > 0) {
            // Between passes the signal goes through a D/A - A/D hop: a
            // half-sample shift (two-tap average) and 8-bit u-law PCM, as a
            // telephone network transcodes. A synchronous G.726 tandem alone
            // would hardly lose anything more per pass.
            const int16_t in = int16_t(v);
            v = ulawToLinear(linearToUlaw(int16_t((int(in) + int(g[k].prev)) / 2)));
            g[k].prev = in;
        }
        int code, bits;
        if (codec <= 1) {
            code = codec == 0 ? linearToUlaw(int16_t(v)) : linearToAlaw(int16_t(v));
            bits = 8;
        } else {
            const RateTables& r = kRates[codec - 2];
            code = g726Encode(g[k].enc, r, v);
            bits = r.bits;
        }
        if (ber > 0)
            for (int b = 0; b < bits; ++b)
                if (rng.uni() < ber) code ^= 1 << b;
        if (codec <= 1) v = codec == 0 ? ulawToLinear(uint8_t(code)) : alawToLinear(uint8_t(code));
        else v = g726Decode(g[k].dec, kRates[codec - 2], code);
    }
    return int16_t(v);
}

// --- FFT (radix 2, in place) ------------------------------------------------

void fft(float* re, float* im, int n, bool inverse) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    // Stage twiddles, cached per size (double trigonometry is software on the M4F).
    static int cachedN = 0;
    static float cosT[12], sinT[12];
    if (cachedN != n) {
        int st = 0;
        for (int len = 2; len <= n && st < 12; len <<= 1, ++st) {
            const double ang = -2.0 * 3.14159265358979323846 / double(len);
            cosT[st] = float(fm::cosD(ang));
            sinT[st] = float(fm::sinD(ang));
        }
        cachedN = n;
    }
    int stage = 0;
    for (int len = 2; len <= n; len <<= 1, ++stage) {
        const float wr = cosT[stage], wi = inverse ? -sinT[stage] : sinT[stage];
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = a + len / 2;
                const float xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
                const float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

}  // namespace

void fftInPlace(float* re, float* im, int n, bool inverse) { fft(re, im, n, inverse); }

const char* const kTcCodecs[6] = {"u-law", "A-law", "G726 40k", "G726 32k", "G726 24k", "G726 16k"};
const char* const kTcRates[4] = {"8 kHz", "11 kHz", "16 kHz", "22 kHz"};
const char* const kTcConceal[3] = {"Silence", "Repeat", "Glitch"};
const char* const kDcBlocks[4] = {"64", "128", "256", "512"};

uint32_t transcode(const float* p, const int16_t* in, uint32_t n, int16_t* out) {
    static const float kRateHz[4] = {8000, 11025, 16000, 22050};
    const int codec = int(p[0] + 0.5f);
    const float rate = kRateHz[int(p[1] + 0.5f) & 3];
    int gens = int(p[2] + 0.5f);
    gens = gens < 1 ? 1 : (gens > kMaxGens ? kMaxGens : gens);
    const float ber = p[3] / 10000.0f;
    const float loss = p[4] / 100.0f;
    const int conceal = int(p[5] + 0.5f);
    const float mix = p[6] / 100.0f;
    Gen g[kMaxGens];
    Lcg rng;
    // Packets of 10 ms at the codec rate.
    const int packet = int(rate / 100.0f);
    int16_t last[256] = {};   // the previous packet (concealment)
    int16_t cur[256] = {};
    int pk = 0;
    bool lost = false;
    const float step = rate / 44100.0f;   // codec samples per input sample
    float phase = 1.0f;                   // >= 1: a codec sample is due
    float acc = 0, accN = 0;              // box low-pass over the decimation interval
    float y0 = 0, y1 = 0;                 // the last two codec outputs (linear upsample)
    for (uint32_t i = 0; i < n; ++i) {
        acc += float(in[i]);
        accN += 1;
        phase += step;
        if (phase >= 1.0f) {
            phase -= 1.0f;
            const int16_t x = sat16(int32_t(acc / accN));
            acc = accN = 0;
            int16_t y = codecChain(codec, gens, ber, x, g, rng);
            if (pk == 0) lost = loss > 0 && rng.uni() < loss;
            cur[pk] = y;
            if (lost) {
                if (conceal == 0) y = 0;
                else if (conceal == 1) y = last[pk];
                else y = last[(pk * 2) % packet];   // glitch: the last packet at double speed
            }
            if (++pk >= packet) {
                pk = 0;
                if (!lost) std::memcpy(last, cur, sizeof(int16_t) * size_t(packet));
            }
            y0 = y1;
            y1 = float(y);
        }
        const float wet = y0 + (y1 - y0) * phase;
        out[i] = sat16(int32_t(float(in[i]) * (1.0f - mix) + wet * mix));
    }
    return n;
}

uint32_t dctCrush(const float* p, const int16_t* in, uint32_t n, int16_t* out) {
    static const int kN[4] = {64, 128, 256, 512};
    const int N = kN[int(p[0] + 0.5f) & 3];
    const int hop = N / 2;
    const float keep = p[1] / 100.0f;
    const int bits = int(p[2] + 0.5f);
    const float hold = p[3] / 100.0f;
    const float mix = p[4] / 100.0f;
    float win[512], re[512], im[512], prevRe[512], prevIm[512], acc[512 + 256];
    for (int i = 0; i < N; ++i) win[i] = float(fm::sinD(3.14159265358979323846 * (double(i) + 0.5) / double(N)));   // sqrt-Hann
    for (int i = 0; i < N; ++i) prevRe[i] = prevIm[i] = 0;
    for (int i = 0; i < N + hop; ++i) acc[i] = 0;
    Lcg rng;
    const float levels = float((1 << (bits < 1 ? 1 : bits)) - 1);
    // Frames start at -hop so the first samples get a full overlap.
    for (int64_t f = -hop; f < int64_t(n); f += hop) {
        for (int i = 0; i < N; ++i) {
            const int64_t k = f + i;
            re[i] = (k >= 0 && k < int64_t(n) ? float(in[k]) : 0.0f) * win[i];
            im[i] = 0;
        }
        fft(re, im, N, false);
        if (hold > 0 && rng.uni() < hold) {
            for (int i = 0; i < N; ++i) {
                re[i] = prevRe[i];
                im[i] = prevIm[i];
            }
        } else {
            // Keep the strongest bins: threshold at the (1 - keep) quantile of
            // the magnitudes, found by a few bisection passes.
            float mx = 0;
            for (int i = 0; i <= N / 2; ++i) {
                const float m = re[i] * re[i] + im[i] * im[i];
                if (m > mx) mx = m;
            }
            float lo = 0, hi = mx;
            const int want = int(keep * float(N / 2 + 1) + 0.5f);
            for (int it = 0; it < 16; ++it) {
                const float mid = 0.5f * (lo + hi);
                int cnt = 0;
                for (int i = 0; i <= N / 2; ++i) cnt += re[i] * re[i] + im[i] * im[i] > mid;
                if (cnt > want) lo = mid;
                else hi = mid;
            }
            const float amax = std::sqrt(mx);
            for (int i = 0; i <= N / 2; ++i) {
                const float m2 = re[i] * re[i] + im[i] * im[i];
                if (m2 <= hi || amax <= 0) {
                    re[i] = im[i] = 0;
                } else if (bits < 16) {
                    // Quantise the magnitude (relative to the frame peak), keep the phase.
                    const float m = std::sqrt(m2);
                    const float q = float(int(m / amax * levels + 0.5f)) / levels * amax;
                    re[i] *= q / m;
                    im[i] *= q / m;
                }
            }
            for (int i = 1; i < N / 2; ++i) {   // Hermitian: a real frame
                re[N - i] = re[i];
                im[N - i] = -im[i];
            }
            for (int i = 0; i < N; ++i) {
                prevRe[i] = re[i];
                prevIm[i] = im[i];
            }
        }
        fft(re, im, N, true);
        for (int i = 0; i < N; ++i) acc[i] += re[i] / float(N) * win[i];
        // Samples [f, f + hop) are complete.
        for (int i = 0; i < hop; ++i) {
            const int64_t k = f + i;
            if (k >= 0 && k < int64_t(n)) out[k] = sat16(int32_t(float(in[k]) * (1.0f - mix) + acc[i] * mix));
        }
        std::memmove(acc, acc + hop, sizeof(float) * size_t(N));
        for (int i = N; i < N + hop; ++i) acc[i] = 0;
    }
    return n;
}

void codecDirect(int codec, int gens, const int16_t* in, uint32_t n, int16_t* out) {
    Gen g[kMaxGens];
    Lcg rng;
    if (gens > kMaxGens) gens = kMaxGens;
    for (uint32_t i = 0; i < n; ++i) out[i] = codecChain(codec, gens, 0, in[i], g, rng);
}

int16_t ulawRoundTrip(int16_t x) { return ulawToLinear(linearToUlaw(x)); }
int16_t alawRoundTrip(int16_t x) { return alawToLinear(linearToAlaw(x)); }

}  // namespace sfx
}  // namespace ot
