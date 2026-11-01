// [I] USB audio, computer -> Tracker (#97). See usb_audio_in.h.
#include "usb/usb_audio_in.h"

#include <cstring>

namespace ot {
namespace usbaudio {

static_assert((kRingFrames & (kRingFrames - 1)) == 0, "ring size is a power of two");
static_assert(kTarget + kBlockFrames < kRingFrames, "room above the target");

void UsbAudioIn::receive(const int16_t* lr, uint32_t frames) {
    packets_ = packets_ + 1;
    const uint32_t w = w_;
    const uint32_t space = kRingFrames - (w - r_);
    const uint32_t n = frames < space ? frames : space;
    if (n < frames) dropped_ = dropped_ + (frames - n);
    const uint32_t at = w & (kRingFrames - 1);
    const uint32_t first = n < kRingFrames - at ? n : kRingFrames - at;
    std::memcpy(&ring_[2 * at], lr, first * 4);
    if (n > first) std::memcpy(&ring_[0], lr + 2 * first, (n - first) * 4);
    w_ = w + n;   // publish after the data
}

const int16_t* UsbAudioIn::block(int16_t* out) {
    const uint32_t pk = packets_;
    if (pk == seenPackets_) {
        if (idle_ < kIdleBlocks && ++idle_ == kIdleBlocks) {
            playing_ = false;
            r_ = w_;   // the computer stopped: drop what's left
        }
    } else {
        idle_ = 0;
        seenPackets_ = pk;
    }
    uint32_t r = r_;
    uint32_t fill = w_ - r;
    // Far over the target (the audio blocks stopped for a while, e.g. a
    // project load, while the computer kept sending): skip to the target
    // instead of playing late until the feedback has drained it.
    if (fill > kTarget + kBlockFrames + kSkipAbove) {
        const uint32_t skip = fill - (kTarget + kBlockFrames);
        r += skip;
        r_ = r;
        fill -= skip;
        skipped_ = skipped_ + skip;
        fillQ8_ = int32_t(kTarget << 8);
    }
    if (!playing_) {
        // Start once a block plus the target is queued, so the first blocks
        // don't under-run while the feedback settles.
        if (idle_ >= kIdleBlocks || fill < kTarget + kBlockFrames) return nullptr;
        playing_ = true;
        fillQ8_ = int32_t(kTarget << 8);
    } else if (fill < kBlockFrames) {
        if (idle_ == 0) ++underruns_;   // (no packet this block: the computer is stopping)
        playing_ = false;   // keep what's there and fill up again
        return nullptr;
    }
    const uint32_t at = r & (kRingFrames - 1);
    const uint32_t first = kBlockFrames < kRingFrames - at ? kBlockFrames : kRingFrames - at;
    std::memcpy(out, &ring_[2 * at], first * 4);
    if (first < kBlockFrames) std::memcpy(out + 2 * first, &ring_[0], (kBlockFrames - first) * 4);
    r_ = r + kBlockFrames;   // publish after the copy
    fill -= kBlockFrames;
    // Feedback: a PI loop on the low-passed fill (Q8 frames). The fill seen
    // here jumps with the packet phase (up to one packet), hence the filter.
    // One frame of error = 16 LSB proportional (a 64-frame error corrects in
    // about a second); the integral learns the clock offset between the
    // computer and the codec (crystal tolerances, tens of ppm).
    fillQ8_ += (int32_t(fill << 8) - fillQ8_) >> 3;
    const int32_t errQ8 = int32_t(kTarget << 8) - fillQ8_;
    integ_ += errQ8 >> 12;
    const int32_t span = int32_t(kFeedbackSpan);
    if (integ_ > span) integ_ = span;
    if (integ_ < -span) integ_ = -span;
    int32_t d = integ_ + (errQ8 >> 4);
    if (d > span) d = span;
    if (d < -span) d = -span;
    fb_ = uint32_t(int32_t(kFeedbackNominal) + d);
    return out;
}

}  // namespace usbaudio
}  // namespace ot
