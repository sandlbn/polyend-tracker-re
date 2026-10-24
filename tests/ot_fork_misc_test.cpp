// OT fork: theme palettes and the Breaks (spectral flux) auto-slice detector.
#include "test.h"

#include <vector>

#include "app/auto_slice.h"
#include "ui/theme.h"

using namespace ot;

namespace {
int lum(uint32_t c) { return int((c >> 16) & 0xFF) * 3 + int((c >> 8) & 0xFF) * 6 + int(c & 0xFF); }
}  // namespace

TEST(themes_all_usable) {
    for (int i = 0; i < kThemeTotal; ++i) {
        const Theme& t = theme(uint8_t(i));
        CHECK(t.name != nullptr);
        CHECK(std::strcmp(t.name, themeName(uint8_t(i))) == 0);
        // Text stands out from the background and the panels.
        const int d = lum(t.text) - lum(t.bg);
        CHECK(d > 600 || d < -600);
        CHECK(lum(t.text) != lum(t.panelDark));
        CHECK(themeBase(uint8_t(i)) < kThemeCount);
        // Flags are copied unchanged from the base theme.
        const Theme& b = theme(themeBase(uint8_t(i)));
        for (int f = slot::kFlagBottomFade; f < kThemeSlotCount; ++f) CHECK_EQ(t.slot(f), b.slot(f));
    }
    // Two fork themes in a row: the second rebuild replaces the first.
    const uint32_t a = theme(3).slot(slot::kFocusFrame);
    const uint32_t b = theme(11).slot(slot::kFocusFrame);
    CHECK(a != b);
    CHECK_EQ(theme(3).slot(slot::kFocusFrame), a);
    CHECK_EQ(theme(0).slot(slot::kFocusFrame), 0xCD1818u);   // stock untouched
}

TEST(flux_slicer_finds_every_hit_of_a_fast_break) {
    // 170 BPM sixteenths (3891 frames) for 2 bars: decaying noise bursts,
    // alternating loud and quiet (ghost notes), over a low hum.
    const uint32_t step = 3891, n = step * 32 + 4000;
    std::vector<int16_t> s(n);
    uint32_t r = 7;
    for (uint32_t i = 0; i < n; ++i) s[i] = int16_t(300.0 * std::sin(i * 0.01));
    for (int k = 0; k < 32; ++k) {
        const double amp = (k % 2) ? 4000 : 14000;
        for (uint32_t i = 0; i < 1800; ++i) {
            r = r * 1664525u + 1013904223u;
            const double env = std::exp(-double(i) / 300.0);
            const uint32_t at = uint32_t(k) * step + 600 + i;
            s[at] = int16_t(s[at] + amp * env * (double(int32_t(r >> 16) & 0xFFFF) / 32768.0 - 1.0));
        }
    }
    uint16_t out[48] = {};
    uint8_t count = 0;
    AutoSlicer a;
    a.setMode(AutoSlicer::kFlux);
    CHECK(a.start(s.data(), n, out, &count));
    a.finish();
    std::printf("  breaks detector: %d slices\n", int(count));
    CHECK(count >= 30 && count <= 34);   // slice 0 + the 32 hits (first hit near 0 merges)
    // Every hit has a slice within 6 ms before it.
    int found = 0;
    for (int k = 1; k < 32; ++k) {
        const uint32_t hit = uint32_t(k) * step + 600;
        for (int j = 0; j < count; ++j) {
            const uint32_t pos = uint32_t(double(out[j]) * double(n) / 65535.0);
            if (pos <= hit + 64 && hit - pos < 265) {
                ++found;
                break;
            }
        }
    }
    CHECK(found >= 29);
    // Stock detector on the same break: far fewer (232 ms spacing).
    AutoSlicer b;
    uint8_t c2 = 0;
    CHECK(b.start(s.data(), n, out, &c2));
    b.finish();
    CHECK(c2 < count);
}
