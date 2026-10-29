// SD streaming: stream policy (stream_policy.h).
#include "app/stream_policy.h"

namespace ot {

namespace {
constexpr uint32_t kMinStreamFrames = 4u * audio::kStreamChunkFrames;   // 1.49 s: below, the head holds most of it
constexpr uint32_t kRate = 44100;
// Largest resident size of one streamed instrument (stereo head + loop head).
constexpr uint64_t kMaxResidentBytes = uint64_t(2) * 2u * audio::kStreamHeadFrames * 2u;
}  // namespace

bool streamDecision(const StreamDecisionIn& in) {
    if (in.mode == StreamMode::Off || !in.readerAvailable || in.streamsInUse >= audio::kMaxStreams) return false;
    if (in.playMode != uint8_t(PlayMode::OneShot) && in.playMode != uint8_t(PlayMode::ForwardLoop)) return false;
    if (in.frames <= kMinStreamFrames) return false;
    if (in.mode == StreamMode::All) return true;
    return in.frames >= uint32_t(in.thresholdSec) * kRate || (in.overBudget && in.frames >= 2u * kRate);
}

ImportDecision importDecision(const ImportDecisionIn& in) {
    ImportDecision d;
    if (in.mode == StreamMode::Off || !in.readerAvailable) return d;
    StreamDecisionIn o;
    o.mode = in.mode;
    o.playMode = in.playMode;
    o.frames = in.frames;
    o.stereo = in.stereo;
    o.thresholdSec = in.thresholdSec;
    o.overBudget = !in.fitsWhole;
    o.readerAvailable = true;
    o.streamsInUse = in.streamsInUse;
    if (streamDecision(o)) {
        d.streams = true;
        d.why = in.mode == StreamMode::All                          ? StreamWhy::All
                : in.frames >= uint32_t(in.thresholdSec) * kRate ? StreamWhy::Threshold
                                                                   : StreamWhy::NoFit;
        return d;
    }
    if (in.fitsWhole) return d;
    d.streams = true;   // does not fit: the fallback, whatever the length
    d.why = StreamWhy::NoFit;
    d.forcedOneShot = in.playMode != uint8_t(PlayMode::OneShot) && in.playMode != uint8_t(PlayMode::ForwardLoop);
    return d;
}

StreamResident streamResident(uint8_t playMode, uint32_t frames, uint16_t loopPoint1) {
    StreamResident r;
    r.headFrames = frames < audio::kStreamHeadFrames ? frames : audio::kStreamHeadFrames;
    if (playMode == uint8_t(PlayMode::ForwardLoop)) {
        const uint32_t ls = uint32_t(uint64_t(frames) * loopPoint1 / 65535u);   // the voice's pointToFrames
        if (ls >= audio::kStreamHeadFrames && ls < frames) {
            r.loopStart = ls;
            const uint32_t rest = frames - ls;
            r.loopFrames = rest < audio::kStreamHeadFrames ? rest : audio::kStreamHeadFrames;
        }
    }
    return r;
}

uint64_t streamProjectedBytes(const uint32_t* ptiPayload, const bool* hasPti, int n, StreamMode mode,
                              uint8_t thresholdSec) {
    uint64_t sum = 0;
    for (int i = 0; i < n; ++i) {
        if (!hasPti[i]) continue;
        const uint64_t p = ptiPayload[i];
        bool streams = false;
        if (mode == StreamMode::All) streams = p > kMaxResidentBytes;
        else if (mode == StreamMode::Auto) streams = p >= uint64_t(thresholdSec) * kRate * 2u && p > kMaxResidentBytes;
        sum += streams ? kMaxResidentBytes : p;
    }
    return sum;
}

}  // namespace ot
