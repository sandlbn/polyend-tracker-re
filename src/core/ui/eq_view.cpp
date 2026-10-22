#include "ui/eq_view.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace ot {
namespace eqv {

void freqText(int hz, char* out, int cap) {
    if (hz < 1000) std::snprintf(out, size_t(cap), "%d Hz", hz);
    else std::snprintf(out, size_t(cap), "%.2f kHz", double(hz) / 1000.0);
}

// The third value of a band: gain, or the cut slope on the cuts of bands 1 and 4.
void gainText(const audio::EqBand& b, int index, char* out, int cap) {
    if (audio::eqSlopeAllowed(index, b.type)) std::snprintf(out, size_t(cap), b.slope ? "48 dB" : "12 dB");
    else if (!audio::eqUsesGain(b.type)) std::snprintf(out, size_t(cap), "-");
    else std::snprintf(out, size_t(cap), "%+.1f dB", double(b.gainDb10) / 10.0);
}

void qText(const audio::EqBand& b, char* out, int cap) {
    std::snprintf(out, size_t(cap), "%.2f", double(b.q100) / 100.0);
}

namespace {
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

void drawGraph(Gfx& g, const Theme& th, const audio::TrackEqParams& tp, int eqBand, int gx, int gy, int gw, int gh) {
    wg::panel(g, th, gx, gy, gw, gh);
    const int px = gx + 14, pw = gw - 28, py = gy + 12, ph = gh - 24;
    const uint32_t grid = wg::mix(th.slot(slot::kEmptyPanel + 1), th.slot(slot::kValueBar), 22);
    const uint32_t axis = wg::mix(th.slot(slot::kEmptyPanel + 1), th.slot(slot::kValueBar), 45);
    const float kRange = 15.0f;
    auto yOf = [&](float db) {
        db = clampf(db, -kRange, kRange);
        return py + int((kRange - db) / (2.0f * kRange) * float(ph) + 0.5f);
    };
    auto xOf = [&](float hz) { return px + int(std::log(hz / 20.0f) / std::log(1000.0f) * float(pw) + 0.5f); };
    for (float hz : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f}) g.vline(xOf(hz), py, ph, grid);
    for (float db : {-12.0f, -6.0f, 6.0f, 12.0f}) g.hline(px, yOf(db), pw, grid);
    g.hline(px, yOf(0.0f), pw, axis);
    g.text(xOf(100.0f), gy + gh - 10, kFont5, th.slot(slot::kList + 4), "100", IDisplay::kOptCenter);
    g.text(xOf(1000.0f), gy + gh - 10, kFont5, th.slot(slot::kList + 4), "1k", IDisplay::kOptCenter);
    g.text(xOf(10000.0f), gy + gh - 10, kFont5, th.slot(slot::kList + 4), "10k", IDisplay::kOptCenter);
    // Response curve.
    const uint32_t curve = th.slot(slot::kGreen);
    int lx = px, ly = yOf(audio::eqResponseDb(tp, 20.0f));
    for (int x = 8; x <= pw; x += 8) {
        const float hz = 20.0f * std::pow(1000.0f, float(x) / float(pw));
        const int cx = px + x, cy = yOf(audio::eqResponseDb(tp, hz));
        g.line(lx, ly, cx, cy, curve);
        lx = cx;
        ly = cy;
    }
    // Band markers: filled = on, the selected band larger.
    for (int i = 0; i < audio::kEqBands; ++i) {
        const audio::EqBand& bd = tp.band[i];
        const float gdb = audio::eqUsesGain(bd.type) ? float(bd.gainDb10) / 10.0f : 0.0f;
        const int mx = xOf(float(bd.freqHz)), my = yOf(gdb);
        const uint32_t c = bd.on ? th.slot(slot::kFocusFrame) : th.slot(slot::kList + 4);
        g.point(mx, my, i == eqBand ? 7.0f : 5.0f, c);
        char n[16];
        std::snprintf(n, sizeof(n), "%d", i + 1);
        g.text(mx, my - 13, kFont5, c, n, IDisplay::kOptCenter);
    }
}

}  // namespace eqv
}  // namespace ot
