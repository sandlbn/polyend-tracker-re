#include "ui/wave.h"

#include "ui/widgets.h"

namespace ot {

// Waveform columns: one 2-px column per pixel pair (3-px in half-height
// views), each the min..max of the samples it covers, so the view shows the
// sample's envelope like the Tracker Mini / Tracker+ (2.x; stock 1.9 drew a
// stem from the centre to one sample per column, VISUAL_DIFF §3.5, §8.14).
// A column reads every sample it covers up to 512, strided beyond (at most
// ~1024 reads): a redraw stays bounded on long samples. Drawn in colour c on top of whatever
// the caller put below (background, selection strip, centre line).
void drawWaveData(Gfx& g, const Theme& th, const int16_t* d, uint32_t len, float v0, float v1, int x0, int y0, int w,
                  int h, uint32_t c) {
    if (!d || !len) {
        g.text(x0 + w / 2, y0 + h / 2, kFontText, th.dim, "No sample", IDisplay::kOptCenter);
        return;
    }
    const int mid = y0 + h / 2;
    const int amp = h / 2;
    const float last = float(len - 1);
    auto at = [&](float p) {
        if (p < 0) p = 0;
        if (p > last) p = last;
        const uint32_t i = uint32_t(p);
        const uint32_t j = i + 1 < len ? i + 1 : i;
        return float(d[i]) + (float(d[j]) - float(d[i])) * (p - float(i));
    };
    // Half-height (stereo) views use 3-px columns: two of them must fit the
    // 2048-word display list next to the editor's list and bar.
    const int pitch = h < 150 ? 3 : 2;
    g.rectsBegin(c);
    for (int x = 0; x < w; x += pitch) {
        const int cw = x + pitch <= w ? pitch : w - x;
        const float p0 = (v0 + (v1 - v0) * float(x) / float(w)) * float(len);
        const float p1 = (v0 + (v1 - v0) * float(x + pitch) / float(w)) * float(len);
        float mn = at(p0), mx = mn;
        const float e = at(p1);
        if (e < mn) mn = e;
        if (e > mx) mx = e;
        uint32_t s = p0 < 0 ? 0 : uint32_t(p0) + 1;
        const uint32_t s1 = p1 > last ? len - 1 : uint32_t(p1);
        const uint32_t stride = (s1 > s ? s1 - s : 1) / 512 + 1;
        for (; s <= s1 && s < len; s += stride) {
            if (d[s] < mn) mn = d[s];
            if (d[s] > mx) mx = d[s];
        }
        const int top = mid - int(mx * float(amp) / 32768.0f);
        const int bot = mid - int(mn * float(amp) / 32768.0f);
        g.rectsAdd(x0 + x, top, cw, bot - top + 1);
    }
    g.rectsEnd();
}

// ---------------------------------------------------------------------------
// Waveform chrome shared by the sample screens (declared in the screen files:
// wave.h is frozen for this work package).

// Selection / loop strip (overlay class 0x0d colour [1], 0a0a0a): columns
// xa..xb, rows y0..y0 + h - 1, half-covered rows above and below.
void waveRegion(Gfx& g, const Theme& th, int xa, int xb, int y0, int h) {
    if (xb < xa) return;
    const uint32_t c = th.slot(slot::kWaveOverlay + 1);
    g.alpha(128);
    g.rectsBegin(c);
    g.rectsAdd(xa, y0 - 1, xb - xa + 1, 1);
    g.rectsAdd(xa, y0 + h, xb - xa + 1, 1);
    g.rectsEnd();
    g.alpha(255);
    g.fill(xa, y0, xb - xa + 1, h, c);
}

// Centre line (slot kWave + 1): two rows at y, y + 1.
void waveCentre(Gfx& g, const Theme& th, int x0, int w, int y) { g.fill(x0, y, w, 2, th.slot(slot::kWave + 1)); }

namespace {
// Hollow marker flag, 7 x 9 px (stock 008 / 025, measured): runs of
// (row, first column, last column, full or half cover) relative to the
// marker line; mirrored by dir (+1 points right) and flipped for the
// bottom flag.
struct FlagRun {
    int8_t row, x0, x1;
    bool full;
};
constexpr FlagRun kFlag[] = {
    {0, 1, 1, false}, {1, 1, 2, true},  {1, 3, 3, false}, {2, 1, 3, true},  {2, 4, 4, false},
    {3, 2, 2, false}, {3, 3, 5, true},  {3, 6, 6, false}, {4, 3, 3, false}, {4, 4, 6, true},
    {5, 2, 2, false}, {5, 3, 5, true},  {5, 6, 6, false}, {6, 1, 3, true},  {6, 4, 4, false},
    {7, 1, 2, true},  {7, 3, 3, false}, {8, 1, 1, false},
};

void flag(Gfx& g, int x, int yRow0, int ydir, int dir, uint32_t c) {
    for (int pass = 0; pass < 2; ++pass) {
        const bool full = pass == 0;
        if (!full) g.alpha(128);
        g.rectsBegin(c);
        for (const FlagRun& r : kFlag) {
            if (r.full != full) continue;
            const int a = x + dir * r.x0, b = x + dir * r.x1;
            g.rectsAdd(a < b ? a : b, yRow0 + ydir * r.row, (a < b ? b - a : a - b) + 1, 1);
        }
        g.rectsEnd();
    }
    g.alpha(255);
}
}  // namespace

// Marker (playback markers / class 0x11): a 1.5-px line, column x full and
// x + dir half, rows y0..y1 with soft end rows, plus a hollow flag pointing
// to dir at the top (rows y0 - 1 .. y0 + 7) and / or the bottom
// (y1 - 7 .. y1 + 1).
void waveMarker(Gfx& g, const Theme& th, int x, uint32_t c, int dir, bool top, bool bottom, int y0, int y1) {
    (void)th;
    g.alpha(120);
    g.fill(x + dir, y0, 1, y1 - y0 + 1, c);
    g.alpha(136);
    g.rectsBegin(c);
    g.rectsAdd(x, y0 - 1, 1, 1);
    g.rectsAdd(x, y1 + 1, 1, 1);
    g.rectsEnd();
    g.alpha(255);
    g.fill(x, y0, 1, y1 - y0 + 1, c);
    if (top) flag(g, x, y0 - 1, 1, dir, c);
    if (bottom) flag(g, x, y1 + 1, -1, dir, c);
}

// ---------------------------------------------------------------------------
// Bottom-bar glyphs the screens draw on the strip (stock 005 / 006 / 008).

// Wide selector "▲ title ▼" over labels i, i + 1: the title in the label
// colour centred on the pair, 14 x 6 triangles at cx - d .. cx - d + 13 and
// cx + d - 14 .. cx + d - 1 (d 57 Sample Editor, 67 Playback / Automation).
void barSelector(Gfx& g, const Theme& th, int i, const char* title, int d) {
    const int cx = wg::labelCentre(i, true);
    const uint32_t c = th.slot(slot::kBottomLabel);
    if (title && *title) g.text(cx, wg::kLabelY, kFont3, c, title, IDisplay::kOptCenter);
    const int ax[2] = {cx - d, cx + d - 14};
    const int y0 = wg::kLabelY;   // rows 451..456
    for (int k = 0; k < 2; ++k) {
        const bool up = k == 0;
        const int x0 = ax[k];
        auto row = [&](int r) { return up ? y0 + r : y0 + 5 - r; };
        g.rectsBegin(c);
        for (int r = 1; r < 6; ++r) g.rectsAdd(x0 + 7 - r, row(r), 2 * r, 1);
        g.rectsEnd();
        g.alpha(176);
        g.rectsBegin(c);
        g.rectsAdd(x0 + 6, row(0), 2, 1);
        for (int r = 1; r < 6; ++r) {
            g.rectsAdd(x0 + 6 - r, row(r), 1, 1);
            g.rectsAdd(x0 + 7 + r, row(r), 1, 1);
        }
        g.rectsEnd();
        g.alpha(255);
    }
}

// Two-line grey label (colour set +0x44: both lines font 3, label colour,
// stock 006 "Select / effect"): drawn by the screen for a hidden label i.
void barGreyLabel(Gfx& g, const Theme& th, int i, const char* top, const char* bottom) {
    const int cx = wg::labelCentre(i);
    const uint32_t c = th.slot(slot::kBottomLabel);
    g.text(cx, wg::kLabelY1, kFont3, c, top, IDisplay::kOptCenter);
    g.text(cx, wg::kLabelY2, kFont3, c, bottom, IDisplay::kOptCenter);
}

// Radio seek cell: "◀◀" (dir -1) or "▶▶" (dir +1) on label i, two 6 x 13
// triangles 10 px apart (stock 005: F3 242..257, F4 342..357, rows 446..458).
void barSeekArrows(Gfx& g, const Theme& th, int i, int dir, bool enabled) {
    const uint32_t c = th.slot(enabled ? slot::kBottomLabel : slot::kBottomLabelOff);
    const int left = wg::labelCentre(i) - 8, cy = wg::kLabelY + 1;
    for (int t = 0; t < 2; ++t) {
        const int bx = left + 10 * t;   // box bx .. bx + 5
        g.rectsBegin(c);
        for (int k = 1; k < 6; ++k) {   // column k from the tip
            const int x = dir < 0 ? bx + k : bx + 5 - k;
            g.rectsAdd(x, cy - k, 1, 2 * k + 1);
        }
        g.rectsEnd();
        g.alpha(150);
        g.rectsBegin(c);
        g.rectsAdd(dir < 0 ? bx : bx + 5, cy, 1, 1);
        g.rectsAdd(dir < 0 ? bx + 5 : bx, cy - 6, 1, 1);
        g.rectsAdd(dir < 0 ? bx + 5 : bx, cy + 6, 1, 1);
        g.rectsEnd();
        g.alpha(255);
    }
}

// ---------------------------------------------------------------------------

float WaveZoom::zoom() const {
    if (len < 600 || spp == 0) return 1.0f;
    return float(len) / (float(spp) * 600.0f);
}

void WaveZoom::step(int d) {
    if (len < 600 || d == 0) return;
    const uint32_t mx = len / 600, mn = len / 65535 + 1;
    if (mx <= mn) return;
    const uint32_t st = (mx - mn) / 100 > 1 ? (mx - mn) / 100 : 1;
    int64_t s = spp ? spp : mx;
    s -= int64_t(d) * int64_t(st);
    if (s < int64_t(mn)) s = mn;
    if (s > int64_t(mx)) s = mx;
    spp = uint32_t(s) >= mx ? 0 : uint32_t(s);
}

void WaveZoom::window(float* v0, float* v1) const {
    const float span = 1.0f / zoom();
    float a = float(centre) / 65535.0f - span / 2;
    if (a < 0) a = 0;
    if (a + span > 1) a = 1 - span;
    if (a < 0) a = 0;
    *v0 = a;
    *v1 = a + span;
}

int WaveZoom::pixelUnits() const {
    const int u = int(65535.0f / zoom() / 600.0f);
    return u < 1 ? 1 : u;
}

}  // namespace ot
