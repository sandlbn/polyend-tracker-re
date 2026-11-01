// [I] 44.1 kHz conversion of the WAV export. Stock writes the device-rate
// samples (180 MHz * 16 / 255 / 256 = 44100 * 2500 / 2499 Hz) with 44117 in
// the header (spec 04 §6.1); exportWav(resampleTo44100 = true) converts them
// to 44100 Hz without changing pitch or duration.
//
// Output frame m reads the input at t = m * 2500 / 2499 = n + mu with
// n = m + m / 2499 and mu = (m % 2499) / 2499 (2500 = 1 mod 2499), so there
// are exactly 2499 phases and no running phase error. Each output sample is a
// 32-tap fractional-delay FIR sum over x[n - 15 .. n + 16] (least-squares design,
// band edge 0.82 of Nyquist = 18.1 kHz); x is 0 before frame 0 and after the
// last input frame. The weights of a frame come from a mirrored polyphase
// table (export_sinc_table.inc, tools/gen_export_sinc.py) and are shared by
// all stems and channels. Sums are rounded to the nearest int16 and clamped.
//
// Memory: the only state kept between blocks is the last kTaps input frames
// of each stem (int16, caller-provided; SDRAM on the device). Each block is
// converted in chunks of kChunk output frames whose weights and input
// windows live on the stack of pushBlock()'s worker (export_resampler.cpp,
// a translation unit of its own so its frame stays apart from exportWav's).
#pragma once

#include <cstdint>

#include "audio/audio_tables.h"

namespace ot {
namespace audio {

// The 32 weights of phase r (0..2498, = m % 2499) for x[n - 15 .. n + 16].
// Phase 0 is the identity (weight 1 at x[n]). Tests and the bench use it.
void exportSincWeights(uint32_t phase, float* w32);

class ExportResampler {
public:
    static constexpr int kTaps = 32;
    static constexpr int kMaxStems = 14;                // StemBlock::kStems
    static constexpr uint32_t kInputPerOutput = 2500;   // 2500 input frames give
    static constexpr uint32_t kOutputPerInput = 2499;   // 2499 output frames
    static constexpr uint32_t kChunk = 32;              // output frames per weight set

    // The last kTaps input frames of one stem, interleaved stereo.
    typedef int16_t History[kTaps * 2];
    // Receives `frames` converted interleaved stereo frames of `stem`, in
    // order; false stops the conversion (write error).
    typedef bool (*Sink)(void* ctx, int stem, const int16_t* interleaved, uint32_t frames);

    // Output length for inputFrames device-rate frames: floor(in * 2499 / 2500).
    static uint32_t outputFrames(uint32_t inputFrames);

    // Starts a conversion of `stems` (1..kMaxStems) stereo streams that all
    // advance together. `history` holds `stems` entries; begin() zeroes them.
    void begin(int stems, History* history);

    // Feeds the next `frames` (1..kBlock) input frames of every stem
    // (in[s] = interleaved stereo of stem s) and hands every output frame that
    // is now complete to `out`. False when `out` returned false.
    bool pushBlock(const int16_t* const* in, uint32_t frames, Sink out, void* ctx);

    // Ends the conversion: emits the remaining frames up to outputFrames() of
    // all input pushed so far. `zeroBlock` is kBlock interleaved stereo frames
    // of silence (the caller's buffer, used as the input after the end).
    bool finish(const int16_t* zeroBlock, Sink out, void* ctx);

    uint32_t inputFrames() const { return inputFrames_; }
    uint32_t emittedFrames() const { return outputFrames_; }

private:
    History* history_ = nullptr;
    int stems_ = 0;
    uint32_t inputFrames_ = 0;    // input frames pushed so far
    uint32_t outputFrames_ = 0;   // output frames emitted so far (next m)
};

}  // namespace audio
}  // namespace ot
