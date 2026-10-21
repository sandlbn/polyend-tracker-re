#include "app/resample.h"

#include <cstdio>

namespace ot {

namespace {
inline int16_t toS16(float x) {
    const float v = x * 32767.0f;
    return int16_t(v >= 32767.0f ? 32767 : (v <= -32768.0f ? -32768 : int32_t(v)));
}
}  // namespace

void Resampler::arm(int16_t* area, uint32_t areaSamples, uint8_t bars) {
    area_ = area;
    half_ = areaSamples / 2;
    bars_ = bars;
    frames_ = 0;
    barsDone_ = 0;
    end_ = kEndNone;
    stats_ = TakeStats{};
    silentRun_ = 0;
    state_ = kArmed;
}

void Resampler::requestStop() {
    if (state_ == kRecording) state_ = kStopping;
}

void Resampler::reset() {
    // area_ stays valid: on the host the audio thread may still be inside
    // tapBlock(); it sees kIdle on its next block.
    state_ = kIdle;
    end_ = kEndNone;
    frames_ = 0;
    barsDone_ = 0;
}

void Resampler::onBlock(bool playing, bool barStart) {
    const uint8_t s = state_;
    if (s == kRecording || s == kStopping) {
        if (!playing) {
            finish(kEndTransport);
            return;
        }
        if (barStart) {
            // This block belongs to the next bar: not recorded.
            barsDone_ = barsDone_ + 1;
            if (s == kStopping) {
                finish(kEndStopped);
                return;
            }
            if (bars_ && barsDone_ >= bars_) {
                finish(kEndBars);
                return;
            }
        }
        return;
    }
    if (s == kArmed && playing && barStart) {
        frames_ = 0;
        barsDone_ = 0;
        stats_ = TakeStats{};
        silentRun_ = 0;
        state_ = kRecording;
    }
}

void Resampler::tapBlock(const float* l, const float* r, uint32_t n) {
    const uint8_t s = state_;
    if (s != kRecording && s != kStopping) return;
    uint32_t f = frames_;
    const uint32_t room = half_ - f;
    const uint32_t m = n < room ? n : room;
    int16_t* L = area_ + f;
    int16_t* R = area_ + half_ + f;
    uint32_t pk = 0;
    for (uint32_t i = 0; i < m; ++i) {
        const int16_t a = toS16(l[i]), b = toS16(r[i]);
        L[i] = a;
        R[i] = b;
        const uint32_t pa = uint32_t(a < 0 ? -int32_t(a) : a), pb = uint32_t(b < 0 ? -int32_t(b) : b);
        if (pa > pk) pk = pa;
        if (pb > pk) pk = pb;
    }
    if (m) {
        ++stats_.blocks;
        if (pk > stats_.peak) stats_.peak = uint16_t(pk > 65535 ? 65535 : pk);
        if (pk == 0) {
            ++stats_.silentBlocks;
            if (++silentRun_ > stats_.longestSilent) stats_.longestSilent = silentRun_;
        } else {
            silentRun_ = 0;
        }
    }
    frames_ = f + m;
    if (m < n || frames_ >= half_) finish(kEndFull);
}

namespace {
// Peak of the first difference over [a, b) on both channels.
int diffPeak(const int16_t* l, const int16_t* r, uint32_t a, uint32_t b) {
    int pk = 0;
    for (uint32_t i = a + 1; i < b; ++i) {
        int d = int(l[i]) - int(l[i - 1]);
        if (d < 0) d = -d;
        if (d > pk) pk = d;
        if (r) {
            d = int(r[i]) - int(r[i - 1]);
            if (d < 0) d = -d;
            if (d > pk) pk = d;
        }
    }
    return pk;
}
}  // namespace

int findHitCells(const int16_t* l, const int16_t* r, uint32_t n, int cells, uint16_t* out, int max) {
    if (!l || n < 2 || cells <= 0) return 0;
    constexpr uint32_t kWindow = 441;   // 10 ms: the level before the cell
    constexpr uint32_t kAttack = 1323;  // 30 ms: a hit may land late (micro timing, Line in latency)
    constexpr uint32_t kEarly = 88;     // 2 ms: or just before the cell
    constexpr int kFloor = 200;         // first-difference peak (~ -44 dBFS)
    int found = 0;
    if (max > 0) out[0] = 0;
    ++found;
    for (int k = 1; k < cells; ++k) {
        const uint32_t f = uint32_t(uint64_t(n) * uint64_t(k) / uint64_t(cells));
        const uint32_t cell = uint32_t(uint64_t(n) / uint64_t(cells));
        const uint32_t w = cell / 2 < kWindow ? cell / 2 : kWindow;
        const uint32_t aw = cell / 2 < kAttack ? cell / 2 : kAttack;
        const uint32_t early = w / 4 < kEarly ? w / 4 : kEarly;
        if (w < 8 || f < w + early || f + aw > n) continue;
        const int attack = diffPeak(l, r, f - early, f + aw);
        const int before = diffPeak(l, r, f - early - w, f - early);
        if (attack < kFloor || attack < 2 * before) continue;
        if (found < max) out[found] = uint16_t(k);
        ++found;
    }
    return found;
}

size_t formatResampleReport(char* out, size_t cap, const ResampleReport& r, uint32_t rxRestarts,
                            uint32_t rxStale) {
    if (!out || !cap) return 0;
    char src[12];
    if (r.source == 0) std::snprintf(src, sizeof(src), "master");
    else if (r.source == audio::AudioEngine::kTapLineIn) std::snprintf(src, sizeof(src), "line-in");
    else std::snprintf(src, sizeof(src), "track%u", unsigned(r.source));
    static const char* const kEnds[] = {"none", "bars", "stopped", "transport", "full"};
    const char* end = r.end < 5 ? kEnds[r.end] : "?";
    char res[24];
    switch (r.outcome) {
        case ResampleReport::kOk: std::snprintf(res, sizeof(res), "instrument %d", r.slot + 1); break;
        case ResampleReport::kEmpty: std::snprintf(res, sizeof(res), "nothing recorded"); break;
        case ResampleReport::kNoSlot: std::snprintf(res, sizeof(res), "no free slot"); break;
        case ResampleReport::kFailed: std::snprintf(res, sizeof(res), "failed"); break;
        default: std::snprintf(res, sizeof(res), "cancelled"); break;
    }
    const uint32_t cs = uint32_t(uint64_t(r.frames) * 100u / 44100u);   // centiseconds
    const int n = std::snprintf(
        out, cap,
        "resample: take %lu source %s end %s -> %s | %lu frames (%lu.%02lu s) blocks %lu silent %lu longest %lu "
        "peak %u (%u%%) | rx restarts +%lu stale +%lu",
        static_cast<unsigned long>(r.seq), src, end, res, static_cast<unsigned long>(r.frames),
        static_cast<unsigned long>(cs / 100), static_cast<unsigned long>(cs % 100),
        static_cast<unsigned long>(r.stats.blocks), static_cast<unsigned long>(r.stats.silentBlocks),
        static_cast<unsigned long>(r.stats.longestSilent), unsigned(r.stats.peak),
        unsigned(uint32_t(r.stats.peak) * 100u / 32767u), static_cast<unsigned long>(rxRestarts),
        static_cast<unsigned long>(rxStale));
    if (n < 0) return 0;
    return size_t(n) < cap ? size_t(n) : cap - 1;
}

}  // namespace ot
