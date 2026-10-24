// SD sample streaming, engine side (docs/PLAN_SD_STREAMING.md P1).
//
// A streamed instrument keeps only its head (the first frames) and the head
// of its loop resident; the rest comes from the card. Each track voice owns a
// ring that the main loop fills ahead of the voice's play position
// (streamService); the voice reads through its 32-frame window, which takes
// each frame from the head, the loop head or the ring (streamRead). A frame
// that is not there yet reads as silence and counts as an underrun: the
// audio block never waits for the card.
//
// Concurrency: the ring is written by the main loop only and read by the
// audio ISR only. The main loop publishes data before `avail` (a DMB between
// them); the ISR publishes the voice's position (`need`) and a new note's
// generation (`gen`); a ring whose `fillGen` differs from the voice's
// generation is treated as empty.
#pragma once

#include <cstdint>

namespace ot {
namespace audio {

constexpr uint32_t kStreamChunkFrames = 16384;                    // one card read per channel (32 KB)
constexpr uint32_t kStreamRingFrames = 4 * kStreamChunkFrames;    // per channel, power of two
constexpr uint32_t kStreamHeadFrames = 2 * kStreamChunkFrames;    // resident head (~743 ms at 1x)
constexpr uint32_t kStreamMargin = 64;                            // frames behind the position kept readable

// One streamed instrument (main loop builds it; read-only for the audio side).
struct StreamSource {
    const int16_t* headL = nullptr;
    const int16_t* headR = nullptr;     // stereo instruments, else nullptr
    uint32_t headFrames = 0;            // frames [0, headFrames) resident
    const int16_t* loopL = nullptr;     // frames [loopStart, loopStart + loopFrames) resident
    const int16_t* loopR = nullptr;
    uint32_t loopStart = 0;
    uint32_t loopFrames = 0;
    uint32_t frames = 0;                // the whole sample
    uint8_t instrument = 0;
};

// The read-ahead buffer of one track voice.
struct StreamRing {
    int16_t* bufL = nullptr;            // kStreamRingFrames each
    int16_t* bufR = nullptr;
    const StreamSource* volatile src = nullptr;   // audio: set at note-on, nullptr when the voice stops
    volatile uint32_t gen = 0;          // audio: the voice's note generation
    volatile uint32_t need = 0;         // audio: the next frame the voice reads
    volatile uint32_t fillGen = 0;      // main loop: the generation the content belongs to
    volatile uint32_t base = 0;         // main loop: frames [base, base + avail) are in the ring
    volatile uint32_t avail = 0;
    volatile uint32_t underruns = 0;    // audio: window reads with a missing frame
    // Audio only (reset at note-on): an underrun fades out from the last
    // frame over kStreamFade frames instead of cutting to 0, and the frames
    // after it fade back in, so a late card read never clicks.
    int16_t lastL = 0, lastR = 0;       // the last frame read (after the fade)
    uint8_t fadeOut = 0;                // decay steps left (kStreamFade after a frame was read)
    uint8_t fadeIn = 0;                 // fade-in step (kStreamFade = full level)
};

constexpr uint8_t kStreamFade = 32;

// Audio side, at note-on: a fresh fade state (full level, nothing to decay).
inline void streamFadeReset(StreamRing& r) {
    r.lastL = r.lastR = 0;
    r.fadeOut = 0;
    r.fadeIn = kStreamFade;
}

// Audio side: frames [b, b + n) of source s into l (and r when not nullptr);
// a frame that is not available reads as 0. False when any frame was missing.
bool streamRead(const StreamSource& s, StreamRing* ring, uint32_t gen, int32_t b, uint32_t n, int16_t* l, int16_t* r);

// Main loop: reads sample data from the card (or memory on the host).
class IStreamReader {
public:
    virtual ~IStreamReader() = default;
    // Frames [frame, frame + n) of the instrument into l (and r for stereo).
    virtual bool read(const StreamSource& s, uint32_t frame, uint32_t n, int16_t* l, int16_t* r) = 0;
};

// Main loop: keeps every active ring filled ahead of its voice; at most
// maxChunks card reads per call. Returns the reads done.
int streamService(StreamRing* rings, int count, IStreamReader& reader, int maxChunks);

}  // namespace audio
}  // namespace ot
