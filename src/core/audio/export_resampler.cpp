// [I] 44.1 kHz conversion of the WAV export (spec 04 §6.1; the design is in
// export_resampler.h).
//
// Processing order: the output frames a block completes are converted in
// chunks of kChunk frames. Per chunk the weights of each frame are computed
// once and shared by all stems. Then, per group of up to two stems (four
// channels), the chunk's input span (at most kChunk + kTaps frames) is copied
// from the stem's history and the new block (both SDRAM on the device; word
// copies, audio/ring_io.h) to the stack, widened to float, and one kernel
// forms the dot products of all channels of the group. The inner loop reads
// the stack only. Each channel sums its taps in ascending order in its own
// accumulator, so its result does not depend on the other channels in the
// kernel.
//
// A channel whose window is all zero for a chunk (a silent stem, an empty
// side) skips the kernel: its sums would be exactly 0 (products +-0 added to
// +0), so its output frames are set to 0. A right channel whose window equals
// the left one (a mono stem) skips it too and gets a copy of the left result,
// which is what its own sums would give. The widening loop notes both on the
// way, without another pass over the samples.
//
// convertBlock() holds the chunk's weights (4 KB) and windows on its stack.
// It stays in this translation unit, so its frame stays apart from
// exportWav's (make perf-gate checks every frame against the stack guard).
#include "audio/export_resampler.h"

#include "audio/dsp.h"
#include "audio/ring_io.h"

namespace ot {
namespace audio {

namespace {

#include "audio/export_sinc_table.inc"

constexpr int kTaps = ExportResampler::kTaps;
constexpr uint32_t kChunk = ExportResampler::kChunk;
constexpr uint32_t kPhases = ExportResampler::kOutputPerInput;   // mu = phase / 2499
constexpr uint32_t kTablePhases = 32;                            // table rows per unit of mu
constexpr uint32_t kTableRows = kTablePhases / 2 + 3;            // mu = -1/32 .. 17/32
constexpr uint32_t kHistFrames = kTaps;                          // input frames kept per stem
constexpr uint32_t kAhead = kTaps / 2;                           // taps after x[n]: x[n + 1 .. n + 16]
constexpr uint32_t kBehind = kTaps / 2 - 1;                      // taps before x[n]: x[n - 15 .. n - 1]
constexpr uint32_t kSpan = kChunk + kTaps;                       // input frames of one chunk, at most
constexpr int kGroupStems = 2;                                   // stems per kernel run (4 channels)

static_assert(sizeof(kExportSincTable) == kTableRows * kTaps * sizeof(float), "export_sinc_table.inc shape");
static_assert(kChunk < kPhases, "a chunk spans at most kChunk + kTaps input frames");
static_assert(kChunk <= 255, "window offsets (<= kChunk) fit a uint8_t");

// Input frame n(m) of output frame m: m * 2500 / 2499 = m + m / 2499 + (m % 2499) / 2499.
inline uint32_t inputFrameOf(uint32_t m) { return m + m / kPhases; }

// Round half away from zero, then saturate to int16.
inline int16_t roundS16(float acc) { return int16_t(sat16(int32_t(acc + (acc >= 0.0f ? 0.5f : -0.5f)))); }

// dst = `span` frames from frame `first` of (history ++ block), interleaved
// stereo: frames 0 .. kHistFrames - 1 are the history, then the block.
void gather(const int16_t* hist, const int16_t* block, uint32_t first, uint32_t span, int16_t* dst) {
    if (first < kHistFrames) {
        const uint32_t n = kHistFrames - first < span ? kHistFrames - first : span;
        OT_SDRAM_ACCESS((2u * n + 2u) / 2u);
        spanIn(hist + 2u * first, dst, 2u * n);
        dst += 2u * n;
        first += n;
        span -= n;
    }
    if (span) {
        OT_SDRAM_ACCESS((2u * span + 2u) / 2u);
        spanIn(block + 2u * (first - kHistFrames), dst, 2u * span);
    }
}

// The dot products of one chunk for NCH (1..4) channels:
// out[c][2 * j] = round(sum_k w[j][k] * x[c][base[j] + k]), j < frames.
// One accumulator per channel and the taps in ascending order: a channel's
// result is the same for every NCH and every channel next to it, so the
// kernel can run on whichever channels are not silent.
// A split-Q15 kernel (each weight as hi + lo * 2^-15, SMLAD / SMLALD on int16
// sample pairs) could replace this kernel and the float widening in
// convertBlock(); nothing else depends on the float windows.
template <int NCH>
void dotChunk(const float (*w)[kTaps], const uint8_t* base, uint32_t frames, const float* const* x,
              int16_t* const* out) {
    for (uint32_t j = 0; j < frames; ++j) {
        const float* wj = w[j];
        const uint32_t b = base[j];
        const float* x0 = x[0] + b;
        const float* x1 = x[NCH > 1 ? 1 : 0] + b;
        const float* x2 = x[NCH > 2 ? 2 : 0] + b;
        const float* x3 = x[NCH > 3 ? 3 : 0] + b;
        float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        for (int k = 0; k < kTaps; ++k) {
            const float wk = wj[k];
            a0 += wk * x0[k];
            if (NCH > 1) a1 += wk * x1[k];
            if (NCH > 2) a2 += wk * x2[k];
            if (NCH > 3) a3 += wk * x3[k];
        }
        out[0][2 * j] = roundS16(a0);
        if (NCH > 1) out[1][2 * j] = roundS16(a1);
        if (NCH > 2) out[2][2 * j] = roundS16(a2);
        if (NCH > 3) out[3][2 * j] = roundS16(a3);
    }
}

void dotChunk(int nch, const float (*w)[kTaps], const uint8_t* base, uint32_t frames, const float* const* x,
              int16_t* const* out) {
    switch (nch) {
    case 1: dotChunk<1>(w, base, frames, x, out); break;
    case 2: dotChunk<2>(w, base, frames, x, out); break;
    case 3: dotChunk<3>(w, base, frames, x, out); break;
    default: dotChunk<4>(w, base, frames, x, out); break;
    }
}

// The worker of pushBlock() and finish(): takes `frames` input frames of each
// stem that start at input frame i0 (in[s]: interleaved stereo of stem s),
// emits the output frames from `next` on that are complete now and below
// `cap`, advances `next`, and keeps the last kHistFrames input frames in
// hist. Invariant: every output frame complete with the input before i0 has
// been emitted, so the first one still needs x[i0 - kHistFrames + 1] at the
// earliest. False when the sink returned false.
__attribute__((noinline)) bool convertBlock(ExportResampler::History* hist, int stems, const int16_t* const* in,
                                            uint32_t i0, uint32_t frames, uint32_t cap, uint32_t& next,
                                            ExportResampler::Sink sink, void* ctx) {
    float w[kChunk][kTaps];                        // weights of the chunk's output frames
    float win[2 * kGroupStems][kSpan];             // input windows of the group's channels
    alignas(4) int16_t stage[2 * kSpan];           // one stem's input span, then the group's output
    uint8_t base[kChunk];                          // first window frame of each output frame
    static_assert(sizeof(stage) >= kGroupStems * 2 * kChunk * sizeof(int16_t), "output fits the stage");
    // Kernel row c: window win[c]; output: stem c / 2 of the group, side c & 1.
    const float* const xs[2 * kGroupStems] = {win[0], win[1], win[2], win[3]};
    int16_t* const outs[2 * kGroupStems] = {stage, stage + 1, stage + 2 * kChunk, stage + 2 * kChunk + 1};

    const uint32_t i1 = i0 + frames;   // input frames [i0 - kHistFrames, i1) are at hand
    uint32_t m = next;
    while (m < cap) {
        const uint32_t nA = inputFrameOf(m);
        uint32_t cnt = 0;
        while (cnt < kChunk && m + cnt < cap) {
            const uint32_t mm = m + cnt;
            const uint32_t q = mm / kPhases;
            const uint32_t n = mm + q;
            if (n + kAhead >= i1) break;   // x[n + 16] not pushed yet
            base[cnt] = uint8_t(n - nA);
            exportSincWeights(mm - q * kPhases, w[cnt]);
            ++cnt;
        }
        if (!cnt) break;
        // The chunk reads x[nA - 15 .. n(last) + 16]; index in (history ++ block).
        const uint32_t first = nA + kHistFrames - kBehind - i0;
        const uint32_t span = base[cnt - 1] + uint32_t(kTaps);
        for (int s0 = 0; s0 < stems; s0 += kGroupStems) {
            const int gs = stems - s0 < kGroupStems ? stems - s0 : kGroupStems;
            // Channel c of the group is side c & 1 of stem c / 2: window win[c],
            // output slot outs[c]. Bit c of `silent`: window c is all zero; of
            // `same`: window c (a right channel) equals window c - 1 and is not
            // silent.
            uint32_t silent = 0, same = 0;
            for (int g = 0; g < gs; ++g) {
                gather(hist[s0 + g], in[s0 + g], first, span, stage);
                float* l = win[2 * g];
                float* r = win[2 * g + 1];
                const u32_alias* f = reinterpret_cast<const u32_alias*>(stage);
                uint32_t any = 0;    // OR of the frames: L in the low half, R in the high (little-endian)
                uint32_t diff = 0;   // low half: OR of L ^ R
                for (uint32_t i = 0; i < span; ++i) {
                    const uint32_t v = f[i];
                    any |= v;
                    diff |= v ^ (v >> 16);
                    l[i] = float(int16_t(v));
                    r[i] = float(int16_t(v >> 16));
                }
                if (!(any & 0xffffu)) silent |= 1u << (2 * g);
                if (!(any >> 16)) silent |= 2u << (2 * g);
                else if (!(diff & 0xffffu) && (any & 0xffffu)) same |= 2u << (2 * g);
            }
            // The windows to compute move to the front (each to a row whose own
            // window has moved already or is not computed), the kernel runs on
            // them, then each result moves to its channel's slot, top down for
            // the same reason. Silent channels get 0; a `same` channel copies
            // the result of channel c - 1, still in slot nch - 1 at that point.
            const uint32_t skip = silent | same;
            const int nc = 2 * gs;
            int nch = 0;
            for (int c = 0; c < nc; ++c) {
                if (skip >> c & 1u) continue;
                if (nch != c)
                    for (uint32_t i = 0; i < span; ++i) {
                        OT_NO_LIBCALL();
                        win[nch][i] = win[c][i];
                    }
                ++nch;
            }
            if (nch) dotChunk(nch, w, base, cnt, xs, outs);
            for (int c = nc - 1; c >= 0; --c) {
                int16_t* o = outs[c];
                if (silent >> c & 1u) {
                    for (uint32_t j = 0; j < cnt; ++j) o[2 * j] = 0;
                } else if (same >> c & 1u) {
                    // Invariant: a `same` R has a non-silent L (see the detection), so
                    // L was computed and its result is still in slot nch - 1.
                    for (uint32_t j = 0; j < cnt; ++j) o[2 * j] = outs[nch - 1][2 * j];
                } else if (--nch != c) {
                    for (uint32_t j = 0; j < cnt; ++j) o[2 * j] = outs[nch][2 * j];
                }
            }
            for (int g = 0; g < gs; ++g)
                if (!sink(ctx, s0 + g, stage + g * 2 * int(kChunk), cnt)) return false;
        }
        m += cnt;
    }
    next = m;

    // Keep the last kHistFrames input frames: (old history ++ block) shifted.
    for (int s = 0; s < stems; ++s) {
        gather(hist[s], in[s], frames, kHistFrames, stage);
        OT_SDRAM_ACCESS((2u * kHistFrames + 2u) / 2u);
        spanOut(hist[s], stage, 2u * kHistFrames);
    }
    return true;
}

}  // namespace

void exportSincWeights(uint32_t phase, float* w) {
    if (phase >= kPhases) phase %= kPhases;
    // w[k](mu) = w[31 - k](1 - mu): phases above 1/2 read the row of 2499 - phase backwards.
    const bool mirror = phase >= (kPhases + 1) / 2;
    const uint32_t r = mirror ? kPhases - phase : phase;   // 0 .. 1249
    const uint32_t pos = r * kTablePhases;                 // mu * 32 = pos / 2499
    const uint32_t i = pos / kPhases;                      // 0 .. 15
    const float f = float(pos - i * kPhases) / float(kPhases);
    // Catmull-Rom between the rows of mu = (i - 1) / 32 .. (i + 2) / 32.
    const float f2 = f * f, f3 = f2 * f;
    const float c0 = -0.5f * f3 + f2 - 0.5f * f;
    const float c1 = 1.5f * f3 - 2.5f * f2 + 1.0f;
    const float c2 = -1.5f * f3 + 2.0f * f2 + 0.5f * f;
    const float c3 = 0.5f * f3 - 0.5f * f2;
    const float* r0 = kExportSincTable[i];
    const float* r1 = kExportSincTable[i + 1];
    const float* r2 = kExportSincTable[i + 2];
    const float* r3 = kExportSincTable[i + 3];
    // Two loops, so the mirror test is made once per frame, not per tap.
    if (mirror) {
        for (int k = 0; k < kTaps; ++k) w[kTaps - 1 - k] = c0 * r0[k] + c1 * r1[k] + c2 * r2[k] + c3 * r3[k];
    } else {
        for (int k = 0; k < kTaps; ++k) w[k] = c0 * r0[k] + c1 * r1[k] + c2 * r2[k] + c3 * r3[k];
    }
}

uint32_t ExportResampler::outputFrames(uint32_t inputFrames) {
    return uint32_t(uint64_t(inputFrames) * kOutputPerInput / kInputPerOutput);
}

void ExportResampler::begin(int stems, History* history) {
    stems_ = stems < 0 ? 0 : (stems > kMaxStems ? kMaxStems : stems);
    history_ = history;
    inputFrames_ = 0;
    outputFrames_ = 0;
    for (int s = 0; s < stems_; ++s) spanZero(history_[s], 2u * kTaps);   // x = 0 before frame 0
}

bool ExportResampler::pushBlock(const int16_t* const* in, uint32_t frames, Sink out, void* ctx) {
    if (!frames) return true;
    const bool ok = convertBlock(history_, stems_, in, inputFrames_, frames, UINT32_MAX, outputFrames_, out, ctx);
    inputFrames_ += frames;
    return ok;
}

bool ExportResampler::finish(const int16_t* zeroBlock, Sink out, void* ctx) {
    const uint32_t pushed = inputFrames_;
    const uint32_t target = outputFrames(pushed);
    const int16_t* zeros[kMaxStems];
    for (int s = 0; s < stems_; ++s) zeros[s] = zeroBlock;
    bool ok = true;
    // x = 0 after the last input frame: feed silence up to x[n(target - 1) + 16]
    // (at most kAhead frames), emitting no frame at or beyond target.
    while (ok && outputFrames_ < target) {
        const uint32_t last = inputFrameOf(target - 1) + kAhead;
        if (last < inputFrames_) break;
        const uint32_t need = last + 1 - inputFrames_;
        const uint32_t frames = need < kBlock ? need : kBlock;
        ok = convertBlock(history_, stems_, zeros, inputFrames_, frames, target, outputFrames_, out, ctx);
        inputFrames_ += frames;
    }
    inputFrames_ = pushed;   // inputFrames() stays the caller's count
    return ok;
}

}  // namespace audio
}  // namespace ot
