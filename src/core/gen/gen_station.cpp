// Station generator: a numbers-station line from the radio. The RDS
// programme name, the PI code and the frequency are read as digits (a digit
// stays a digit, a letter is its alphabet position, "JUNGLE" -> 1 0 2 1 1 4 7
// 1 2 5), in groups of five with a pause, like a numbers broadcast. Notes:
// digit d plays scale degree d. Slices: digit d plays slice d of a sliced
// instrument -- slice a recording of a voice counting 0..9 and the Tracker
// reads the station out (CIPHER on the hardware).
#include <cstdio>

#include "gen/gen_internal.h"

namespace ot {
namespace gen {

namespace {
int putNumber(uint32_t v, uint8_t* out, int at, int cap) {
    char b[12];
    const int n = std::snprintf(b, sizeof(b), "%lu", static_cast<unsigned long>(v));
    for (int i = 0; i < n && at < cap; ++i) out[at++] = uint8_t(b[i] - '0');
    return at;
}
int putText(const char* s, uint8_t* out, int at, int cap) {
    for (; s && *s && at < cap; ++s) {
        const char c = *s;
        if (c >= '0' && c <= '9') out[at++] = uint8_t(c - '0');
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') at = putNumber(uint32_t((c | 0x20) - 'a' + 1), out, at, cap);
    }
    return at;
}
}  // namespace

void genStation(const int16_t* v, Target& t, Rng& rng, Result& r) {
    const int source = v[0], mode = v[1], repeat = v[4];
    static const int kRate[3] = {1, 2, 4};
    const int rate = kRate[v[2]];
    const bool fives = v[3] == 1;
    uint8_t dig[64];
    int n = 0;
    if (source == 0 || source == 3) n = putText(t.text, dig, n, 64);
    if (source == 1 || source == 3) n = t.pi ? putNumber(t.pi, dig, n, 64) : n;
    if (source == 2 || source == 3 || n == 0) n = putNumber(t.freq10, dig, n, 64);
    if (n == 0) {
        std::snprintf(r.msg, sizeof(r.msg), "No station: tune the radio first");
        return;
    }
    // Seed > 0: a fixed scramble of the digits (the "key" of the broadcast).
    if (v[5] > 0)
        for (int i = n - 1; i > 0; --i) {
            const int j = rng.below(i + 1);
            const uint8_t x = dig[i];
            dig[i] = dig[j];
            dig[j] = x;
        }
    if (mode == 1 && t.slices.count < 10) {
        std::snprintf(r.msg, sizeof(r.msg), "Slices: needs 10 slices (0..9)");
        return;
    }
    uint8_t deg[12];
    const int nd = scaleDegrees(t.scaleMask, deg);
    Pattern& p = *t.pat;
    const int len = t.s1 - t.s0 + 1;
    for (int tr = t.t0; tr <= t.t1; ++tr)
        for (int i = 0; i < len; ++i) p.tracks[tr].steps[t.s0 + i].clear();
    int pos = 0, written = 0, k = 0;
    const int total = n * repeat;
    while (pos < len && k < total) {
        const int d = dig[k % n];
        for (int tr = t.t0; tr <= t.t1; ++tr) {
            Step& st = p.tracks[tr].steps[t.s0 + pos];
            if (mode == 1) {
                writeSliceHit(st, t.slices, d, t.instrument);
            } else {
                st.note = int8_t(degreeNote(t.rootNote, deg, nd, d));
                st.instrument = t.instrument;
                if (rate > 1) putFx(st, kFxGate, 60);   // spoken digits, not a legato line
            }
        }
        ++written;
        ++k;
        pos += rate;
        if (fives && k % 5 == 0) pos += rate;          // the pause between groups
        if (k % n == 0 && k < total) pos += 2 * rate;  // and between repeats
    }
    std::snprintf(r.msg, sizeof(r.msg), "Station: %d digits", written);
    r.ok = true;
}

}  // namespace gen
}  // namespace ot
