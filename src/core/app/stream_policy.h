// SD streaming (docs/PLAN_SD_STREAMING_P1C_P2.md §2.1): which instruments of a
// project stream from the card, what stays resident, and the projected memory
// of a project for the Open pre-check. Pure, host-tested
// (tests/stream_policy_test.cpp).
#pragma once

#include <cstdint>

#include "audio/stream_source.h"
#include "model/instrument.h"

namespace ot {

enum class StreamMode : uint8_t { Off = 0, Auto = 1, All = 2 };
constexpr uint8_t kStreamThresholdSec[4] = {5, 10, 20, 30};

struct StreamDecisionIn {
    StreamMode mode = StreamMode::Off;
    uint8_t playMode = 0;
    uint32_t frames = 0;
    bool stereo = false;
    uint8_t thresholdSec = 10;
    bool overBudget = false;        // the whole project does not fit the bank
    bool readerAvailable = false;
    int streamsInUse = 0;
};
// Off / no reader / kMaxStreams in use -> RAM; only OneShot and ForwardLoop
// stream; <= 65,536 frames stays in RAM; All streams, Auto streams from the
// threshold up, or from 2 s up when the project does not fit.
bool streamDecision(const StreamDecisionIn& in);

// Sample Loader import (sample_import.cpp): the open rule above, with "the
// project does not fit" read as "this sample does not fit the free memory".
// A sample that fits but may not stream (play mode, length, 16 in use) loads
// into memory; one that does not fit streams anyway (no length floor, a play
// mode that cannot stream becomes 1-Shot). Off / no reader: never streams.
enum class StreamWhy : uint8_t { None, Threshold, All, NoFit };
struct ImportDecisionIn {
    StreamMode mode = StreamMode::Off;
    uint8_t playMode = 0;
    uint32_t frames = 0;
    bool stereo = false;
    uint8_t thresholdSec = 10;
    bool fitsWhole = true;          // the whole sample fits the free sample memory
    bool readerAvailable = false;
    int streamsInUse = 0;
};
struct ImportDecision {
    bool streams = false;
    StreamWhy why = StreamWhy::None;
    bool forcedOneShot = false;
};
ImportDecision importDecision(const ImportDecisionIn& in);

// Resident part of a streamed instrument: the head, plus the loop head for a
// forward loop starting past the head (frames per channel).
struct StreamResident {
    uint32_t headFrames = 0, loopStart = 0, loopFrames = 0;
    uint32_t framesPerChannel() const { return headFrames + loopFrames; }
};
StreamResident streamResident(uint8_t playMode, uint32_t frames, uint16_t loopPoint1);

// Open pre-check from file sizes only (no headers yet): payload bytes of the
// instruments, the streamed ones counted at most at the largest resident size
// (stereo head + loop head).
uint64_t streamProjectedBytes(const uint32_t* ptiPayload, const bool* hasPti, int n, StreamMode mode,
                              uint8_t thresholdSec);

}  // namespace ot
