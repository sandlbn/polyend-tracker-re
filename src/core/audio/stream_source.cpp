// SD sample streaming, engine side (stream_source.h).
#include "audio/stream_source.h"

#include <cstring>

namespace ot {
namespace audio {

namespace {
constexpr uint32_t kMask = kStreamRingFrames - 1;
inline void barrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
inline uint32_t minu(uint32_t a, uint32_t b) { return a < b ? a : b; }
}  // namespace

bool streamRead(const StreamSource& s, StreamRing* ring, uint32_t gen, int32_t b, uint32_t n, int16_t* l, int16_t* r) {
    bool ok = true;
    // The ring's window, sampled once (the main loop cannot run inside the ISR).
    const bool ringOk = ring && ring->fillGen == gen;
    const uint32_t rb = ringOk ? ring->base : 0, ra = ringOk ? ring->avail : 0;
    // Fade state (without a ring: plain copy, silence for missing frames).
    uint32_t fin = ring ? ring->fadeIn : kStreamFade, fout = ring ? ring->fadeOut : 0;
    int32_t lastL = ring ? ring->lastL : 0, lastR = ring ? ring->lastR : 0;
    uint32_t i = 0;
    while (i < n) {
        const int64_t fs = int64_t(b) + i;
        const int16_t* pl = nullptr;   // the run's source, nullptr = missing
        const int16_t* pr = nullptr;
        uint32_t k = 1;
        if (fs < 0) {
        } else if (uint32_t(fs) < s.headFrames) {
            const uint32_t f = uint32_t(fs);
            k = minu(n - i, s.headFrames - f);
            pl = s.headL + f;
            pr = (s.headR ? s.headR : s.headL) + f;
        } else if (s.loopFrames && uint32_t(fs) >= s.loopStart && uint32_t(fs) < s.loopStart + s.loopFrames) {
            const uint32_t o = uint32_t(fs) - s.loopStart;
            k = minu(n - i, s.loopFrames - o);
            pl = s.loopL + o;
            pr = (s.loopR ? s.loopR : s.loopL) + o;
        } else if (ra && uint32_t(fs) >= rb && uint32_t(fs) < rb + ra) {
            const uint32_t f = uint32_t(fs);
            k = minu(minu(n - i, rb + ra - f), kStreamRingFrames - (f & kMask));
            pl = ring->bufL + (f & kMask);
            pr = (s.headR ? ring->bufR : ring->bufL) + (f & kMask);
        }
        if (!pl) {   // missing: a linear decay from the last frame read, then silence
            ok = false;
            fin = 0;
            if (fout) --fout;   // lastL / lastR stay the level where the gap began
            l[i] = int16_t(lastL * int32_t(fout) / int32_t(kStreamFade));
            if (r) r[i] = int16_t(lastR * int32_t(fout) / int32_t(kStreamFade));
        } else if (fin >= kStreamFade) {   // the common case: a plain copy
            std::memcpy(l + i, pl, k * 2);
            if (r) std::memcpy(r + i, pr, k * 2);
            lastL = l[i + k - 1];
            lastR = r ? r[i + k - 1] : lastL;
            fout = kStreamFade;
        } else {   // back after an underrun: fade in
            for (uint32_t j = 0; j < k; ++j) {
                if (fin < kStreamFade) ++fin;
                l[i + j] = int16_t(int32_t(pl[j]) * int32_t(fin) / int32_t(kStreamFade));
                if (r) r[i + j] = int16_t(int32_t(pr[j]) * int32_t(fin) / int32_t(kStreamFade));
            }
            lastL = l[i + k - 1];
            lastR = r ? r[i + k - 1] : lastL;
            fout = kStreamFade;
        }
        i += k;
    }
    if (ring) {
        ring->fadeIn = uint8_t(fin);
        ring->fadeOut = uint8_t(fout);
        ring->lastL = int16_t(lastL);
        ring->lastR = int16_t(lastR);
        if (!ok) ring->underruns = ring->underruns + 1;
    }
    return ok;
}

int streamService(StreamRing* rings, int count, IStreamReader& reader, int maxChunks) {
    int done = 0;
    for (int v = 0; v < count && done < maxChunks; ++v) {
        StreamRing& ring = rings[v];
        const StreamSource* s = ring.src;
        if (!s) continue;
        const uint32_t g = ring.gen;
        const uint32_t pos = ring.need;
        // Where the ring has to start for this position: past the resident
        // head / loop head when the voice is inside one of them.
        uint32_t cover = pos;                       // everything below is consumed or resident
        if (pos < s->headFrames) cover = s->headFrames;
        if (s->loopFrames && pos >= s->loopStart && pos < s->loopStart + s->loopFrames)
            cover = s->loopStart + s->loopFrames;
        const uint32_t keep = pos > kStreamMargin ? pos - kStreamMargin : 0;   // still readable behind pos
        const uint32_t protect = cover > keep ? cover : keep;   // ring slots below this may be overwritten
        bool reset = ring.fillGen != g;
        if (!reset) {
            const uint32_t end = ring.base + ring.avail;
            // The voice jumped (loop wrap, new start) out of what the ring holds.
            reset = cover < ring.base || cover > end + kStreamChunkFrames;
        }
        if (reset) {
            ring.avail = 0;
            barrier();
            ring.base = cover - cover % kStreamChunkFrames;
            ring.fillGen = g;
            barrier();
        }
        while (done < maxChunks) {
            const uint32_t next = ring.base + ring.avail;
            if (next >= s->frames) break;   // the whole rest of the sample is in the ring
            // Writing [next, next + chunk) reuses the slots of frames next - ring ...: they must be done.
            if (next + kStreamChunkFrames > kStreamRingFrames && next + kStreamChunkFrames - kStreamRingFrames > protect)
                break;
            const uint32_t n = minu(kStreamChunkFrames, s->frames - next);
            const uint32_t slot = next & kMask;   // chunk-aligned: never wraps inside a chunk
            if (ring.avail + kStreamChunkFrames > kStreamRingFrames) {
                // The chunk reuses the slots of the oldest one (done, checked above):
                // shrink the window first (avail, then base: every intermediate
                // state the ISR can see is a valid sub-window), then write.
                ring.avail = ring.avail - kStreamChunkFrames;
                barrier();
                ring.base = ring.base + kStreamChunkFrames;
                barrier();
            }
            if (!reader.read(*s, next, n, ring.bufL + slot, s->headR ? ring.bufR + slot : nullptr)) break;
            barrier();
            if (ring.gen != g || ring.src != s) break;   // a new note took the ring meanwhile (the ISR)
            ring.avail = ring.avail + n;
            ++done;
        }
    }
    return done;
}

}  // namespace audio
}  // namespace ot
