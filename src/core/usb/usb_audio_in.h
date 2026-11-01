// [I] USB audio, computer -> Tracker (#97): the stereo stream the computer
// plays into the Tracker's USB audio device, handed from the USB interrupt to
// the audio block.
//
// Clock: the codec runs at 44117.647 Hz (MCLK / 256, audio_tables.h), not the
// 44100 Hz the descriptor names. The OUT endpoint is asynchronous with an
// explicit feedback endpoint: feedback() tells the computer how many frames
// per 1 ms USB frame to send (10.14 fixed point, USB 2.0 5.12.4.2), steered
// so the ring stays about kTarget frames full after each block. No
// resampling: the computer's samples reach the engine unchanged. Linux and
// Windows follow the feedback; macOS caps it below our rate (PJRC's
// MACOSX_ADAPTIVE_LIMIT), so it under-runs there now and then (not supported).
//
// Threading: receive() runs in the USB interrupt, block() in the audio block
// context. One producer, one consumer: the producer owns w_, the consumer r_,
// each published after its data. feedback() is one aligned word.
#pragma once

#include <cstdint>

namespace ot {
namespace usbaudio {

constexpr uint32_t kBlockFrames = 128;   // audio::kBlock (this header stays free of audio/)
constexpr uint32_t kRingFrames = 512;    // power of two
// Frames left queued after a block is taken (about 4.4 ms): one block of
// headroom for packets arriving late plus the 44/45-frame packet steps.
constexpr uint32_t kTarget = 192;
// 44117.647 / 1000 * 16384, rounded.
constexpr uint32_t kFeedbackNominal = 722824;
// Feedback limits: +-1 % around nominal (Linux reads a value far off nominal
// as the other feedback format).
constexpr uint32_t kFeedbackSpan = 7228;
// No packet for this many blocks (about 12 ms): the computer stopped
// streaming; the ring is flushed and primed again on the next packet.
constexpr uint32_t kIdleBlocks = 4;
// Fill above target + one block + this (the audio blocks paused while the
// computer kept sending): the excess is skipped.
constexpr uint32_t kSkipAbove = 128;

class UsbAudioIn {
public:
    // USB interrupt: one isochronous OUT packet of `frames` interleaved
    // stereo int16 frames. What doesn't fit (computer far too fast) is dropped.
    void receive(const int16_t* lr, uint32_t frames);

    // Audio block: the next kBlockFrames interleaved frames copied into `out`
    // (2 * kBlockFrames samples), or nullptr while the computer isn't
    // streaming or the ring is (re)filling after a start or an under-run.
    // Updates feedback().
    const int16_t* block(int16_t* out);

    // 10.14 frames per USB frame for the feedback endpoint.
    uint32_t feedback() const { return fb_ ? fb_ : kFeedbackNominal; }
    bool playing() const { return playing_; }
    uint32_t fill() const { return w_ - r_; }

    // Diagnostics (telemetry): under-runs while playing, frames dropped (ring
    // full, or skipped after a pause), packets received.
    uint32_t underruns() const { return underruns_; }
    uint32_t dropped() const { return dropped_ + skipped_; }
    uint32_t packets() const { return packets_; }

private:
    // Zero-initialised (no constructor): lives in plain .bss on the device.
    int16_t ring_[2 * kRingFrames];
    volatile uint32_t w_;         // frames written (producer)
    volatile uint32_t r_;         // frames read (consumer)
    volatile uint32_t packets_;
    volatile uint32_t dropped_;
    uint32_t seenPackets_;        // packets_ at the last block (consumer)
    uint32_t idle_;               // blocks without a packet
    uint32_t underruns_;
    uint32_t skipped_;            // frames skipped after a pause (consumer)
    int32_t fillQ8_;              // low-passed fill after a block, Q8 frames
    int32_t integ_;               // integral term, feedback LSBs
    volatile uint32_t fb_;        // 0 = nominal
    bool playing_;
};

}  // namespace usbaudio
}  // namespace ot
