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

namespace {
// Where the ring has to start for the voice's position: past the resident
// head / loop head when the voice is inside one of them.
uint32_t coverOf(const StreamSource& s, uint32_t pos) {
    uint32_t cover = pos;
    if (pos < s.headFrames) cover = s.headFrames;
    if (s.loopFrames && pos >= s.loopStart && pos < s.loopStart + s.loopFrames) cover = s.loopStart + s.loopFrames;
    return cover;
}

// Fills up to maxChunks chunks into one ring. Returns the chunks read, 0 when
// there is nothing to do, -1 on a read error (the ring is left consistent).
int fillRing(StreamRing& ring, IStreamReader& reader, int maxChunks, const IClock* clock, StreamServiceStats* st) {
    const StreamSource* s = ring.src;
    if (!s) return 0;
    const uint32_t g = ring.gen;
    const uint32_t pos = ring.need;
    const uint32_t cover = coverOf(*s, pos);   // everything below is consumed or resident
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
    int done = 0;
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
        const uint32_t t0 = clock ? clock->micros() : 0;
        const bool ok = reader.read(*s, next, n, ring.bufL + slot, s->headR ? ring.bufR + slot : nullptr);
        if (st && clock) {
            const uint32_t us = clock->micros() - t0;
            st->sumReadUs += us;
            if (us > st->maxReadUs) st->maxReadUs = us;
        }
        if (!ok) {
            if (st) ++st->readErrors;
            return done ? done : -1;
        }
        barrier();
        if (ring.gen != g || ring.src != s) break;   // a new note took the ring meanwhile (the ISR)
        ring.avail = ring.avail + n;
        ++done;
        if (st) ++st->chunks;
    }
    return done;
}
}  // namespace

int streamService(StreamRing* rings, int count, IStreamReader& reader, int maxChunks) {
    int done = 0;
    for (int v = 0; v < count && done < maxChunks; ++v) {
        const int k = fillRing(rings[v], reader, maxChunks - done, nullptr, nullptr);
        if (k > 0) done += k;
    }
    return done;
}

uint32_t streamLead(const StreamRing& r) {
    const StreamSource* s = r.src;
    if (!s) return 0;
    const uint32_t pos = r.need;
    uint32_t end = coverOf(*s, pos);   // resident up to here
    if (r.fillGen == r.gen && r.avail && r.base <= end && r.base + r.avail > end) end = r.base + r.avail;
    if (end >= s->frames) return 0xFFFFFFFFu;
    return end > pos ? end - pos : 0;
}

int streamServiceTick(StreamRing* rings, int count, IStreamReader& reader, int maxChunks, const IClock* clock,
                      uint32_t deadlineUs, StreamServiceStats* st) {
    if (st) {
        ++st->passes;
        uint32_t u = 0;
        for (int v = 0; v < count; ++v) u += rings[v].underruns;
        st->underruns = u;
    }
    uint32_t skip = 0;   // rings done for this pass (full, finished or failed)
    int done = 0;
    while (done < maxChunks) {
        if (clock && done > 0 && int32_t(clock->micros() - deadlineUs) >= 0) break;
        int best = -1;
        uint32_t bestLead = 0xFFFFFFFFu;
        for (int v = 0; v < count && v < 32; ++v) {
            if (!rings[v].src || ((skip >> v) & 1u)) continue;
            const uint32_t lead = streamLead(rings[v]);
            if (st && lead < st->minLeadFrames) st->minLeadFrames = lead;
            if (lead == 0xFFFFFFFFu) continue;   // nothing more to read
            if (best < 0 || lead < bestLead) {
                best = v;
                bestLead = lead;
            }
        }
        if (best < 0) break;
        const int k = fillRing(rings[best], reader, 1, clock, st);
        if (k <= 0) {
            skip |= 1u << best;
            continue;
        }
        done += k;
    }
    return done;
}

}  // namespace audio
}  // namespace ot
