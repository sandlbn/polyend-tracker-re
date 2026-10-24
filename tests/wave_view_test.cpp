// Waveform columns (ui/wave.cpp, 2.x): each column shows the min..max of the
// samples it covers (the Tracker+ / Mini envelope), not one sample per column
// (stock 1.9's stem), so a short hit inside a column is never lost.
#include "test.h"
#include "ui_fakes.h"

#include <cstdio>
#include <string>
#include <vector>

#include "ui/gfx.h"
#include "ui/theme.h"
#include "ui/wave.h"

using namespace ot;

namespace {
// Tallest rectangle (rows) in the frame's RECTS vertex pairs.
int tallestRect(const std::vector<std::string>& dl) {
    int best = 0, y0 = 0;
    bool inRects = false, first = true;
    for (const std::string& s : dl) {
        if (s.rfind("BEGIN ", 0) == 0) {
            inRects = true;
            first = true;
            continue;
        }
        if (s == "END") inRects = false;
        int x, y;
        if (!inRects || std::sscanf(s.c_str(), "VERTEX %d %d", &x, &y) != 2) continue;
        if (first) y0 = y;
        else if (y - y0 + 1 > best) best = y - y0 + 1;
        first = !first;
    }
    return best;
}
}  // namespace

TEST(wave_columns_show_hits_between_column_edges) {
    // 600 px over 60000 frames: 200 frames per 2-px column. Every column is
    // silent except one full-scale-ish hit in its middle.
    std::vector<int16_t> d(60000, 0);
    for (size_t i = 100; i < d.size(); i += 200) d[i] = 24000;
    RecordingDisplay disp;
    Gfx g(disp);
    Theme th;
    g.beginFrame(0);
    drawWaveData(g, th, d.data(), uint32_t(d.size()), 0.0f, 1.0f, 0, 100, 600, 200, 0xFFFFFF);
    g.endFrame();
    // The hits reach ~73 % of the half-height (100 rows): the columns must show it.
    const int h = tallestRect(disp.last);
    CHECK(h > 60);
    if (h <= 60) std::printf("       tallest column %d rows\n", h);
}
