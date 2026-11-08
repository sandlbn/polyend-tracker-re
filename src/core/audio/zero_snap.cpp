// Snap to zero (spec 13 §H.2, spec 04 §2.3).
#include "audio/zero_snap.h"

namespace ot {
namespace audio {

volatile bool gSnapToZero = false;

namespace {
bool crossing(const int16_t* x, uint32_t len, int32_t p) {
    if (p < 0 || uint32_t(p) >= len) return false;
    if (x[p] == 0) return true;
    return uint32_t(p) + 1 < len && ((x[p] < 0) != (x[p + 1] < 0));
}

// Distance (0..window) to the nearest crossing, its frame in *at; -1 if none.
int32_t nearest(const int16_t* x, uint32_t len, int32_t frame, int32_t* at) {
    for (int32_t d = 0; d <= kZeroSnapWindow; ++d) {
        if (crossing(x, len, frame + d)) {
            *at = frame + d;
            return d;
        }
        if (d && crossing(x, len, frame - d)) {
            *at = frame - d;
            return d;
        }
    }
    return -1;
}
}  // namespace

int32_t snapToZero(const int16_t* x, uint32_t len, int32_t frame) {
    if (!x || !len) return frame;
    int32_t at = frame;
    return nearest(x, len, frame, &at) >= 0 ? at : frame;
}

bool markerOnCrossing(const int16_t* x, uint32_t len, uint16_t marker, bool snapOn) {
    if (!x || !len) return false;
    int32_t f = int32_t(uint64_t(len) * marker / 65535u);
    if (uint32_t(f) >= len) f = int32_t(len) - 1;
    if (!snapOn) {
        // The frames this marker step covers, up to the next step's first.
        uint32_t e = marker < 65535 ? uint32_t(uint64_t(len) * (marker + 1u) / 65535u) : len;
        if (e > len) e = len;
        if (e <= uint32_t(f)) e = uint32_t(f) + 1;
        for (uint32_t p = uint32_t(f); p < e; ++p)
            if (crossing(x, len, int32_t(p))) return true;
        return false;
    }
    int32_t at = f;
    const int32_t d = nearest(x, len, f, &at);
    return d >= 0 && d < kZeroSnapWindow;
}

}  // namespace audio
}  // namespace ot
