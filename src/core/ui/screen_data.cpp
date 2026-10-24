// OT fork: Data screen (DATASCREEN-lite), File F7 "Data" in images without
// Games. An Ikeda / datamatics style view of what the sequencer plays, black
// and white like the Datamatic theme:
//
//   F1 Barcode  a scrolling barcode: one column per step, one row per track,
//               a bar where a note was played, the bar width = its level
//   F2 Matrix   the pattern around the playhead, 16 tracks x 32 steps
//   F3 Numbers  the playing steps as raw data: hex note / instrument / FX
//               per track, the tick and step counters
//
// The pads show the next 12 steps of the four track groups as a moving
// barcode. Play starts / stops as everywhere; a module key leaves.
#include "ui/screen_data.h"

#if !OT_GAMES

#include <cstdio>
#include <cstring>

#include "app/app.h"

namespace ot {

namespace {
constexpr uint32_t kWhite = 0xFFFFFF, kGrey = 0x5A5A5A, kDim = 0x222222;
constexpr int kTop = 30;   // under the title strip
}  // namespace

void openData(App& app) { app.ui.switchTo(kModGames, 0); }

void DataScreen::start(int opt) {
    (void)opt;
    lastStep_ = -1;
    refreshBar();
}

void DataScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    b.clear();
    static const char* const kModes[3] = {"Barcode", "Matrix", "Numbers"};
    for (int i = 0; i < 3; ++i) {
        b.set(i, kModes[i]);
        b.highlight(i, mode == i);
    }
    b.set(7, app_.seq.isPlaying() ? "Stop" : "Play");
}

void DataScreen::update(uint32_t now) {
    App& a = app_;
    // Track levels decay, refreshed from the engine meters (read and reset).
    for (int t = 0; t < kTracks; ++t) {
        const float p = t < 12 ? a.engine.trackPeak(t) : 0.0f;
        level_[t] = p > level_[t] ? p : level_[t] * 0.9f;
    }
    const int st = a.seq.isPlaying() ? a.seq.step(0) : -1;
    if (st != lastStep_) {
        lastStep_ = st;
        if (st >= 0) {
            // One barcode column per step: a bit per track with a note.
            uint16_t bits = 0;
            uint8_t widths[kTracks];
            for (int t = 0; t < kTracks; ++t) {
                const int s = a.seq.step(t);
                const Step& x = a.pattern().tracks[t].steps[s < 0 ? 0 : (s > 127 ? 127 : s)];
                const bool hit = x.note >= 0;
                bits = uint16_t(bits | (hit ? (1u << t) : 0u));
                int w = int(level_[t] * 9.0f) + (hit ? 2 : 0);
                widths[t] = uint8_t(w > 10 ? 10 : w);
            }
            head_ = (head_ + 1) % kHistory;
            hist_[head_] = bits;
            std::memcpy(width_[head_], widths, sizeof(widths));
        }
        ui_.dirty();
    }
    if (now - lastDraw_ >= 40) {   // the meters and numbers move between steps too
        lastDraw_ = now;
        ui_.dirty();
    }
}

void DataScreen::draw(Gfx& g) {
    App& a = app_;
    g.fill(0, kTop, kScreenW, 393, 0x000000);
    char t[48];
    std::snprintf(t, sizeof(t), "Data  %s", mode == 0 ? "barcode" : (mode == 1 ? "matrix" : "numbers"));
    ui_.drawTitle(g, t, a.seq.isPlaying() ? "PLAY" : "STOP");
    if (mode == 0) {
        // Newest column on the right; rows of 22 px for 16 tracks.
        g.rectsBegin(kWhite);
        for (int c = 0; c < kHistory; ++c) {
            const int idx = (head_ - c + kHistory) % kHistory;
            const int x = kScreenW - 12 - c * 12;
            for (int tr = 0; tr < kTracks; ++tr)
                if (hist_[idx] & (1u << tr)) g.rectsAdd(x, kTop + 8 + tr * 22, width_[idx][tr], 18);
        }
        g.rectsEnd();
        // Level lines under the barcode.
        g.rectsBegin(kGrey);
        for (int tr = 0; tr < 12; ++tr) g.rectsAdd(10 + tr * 65, 400, int(level_[tr] * 60.0f) + 1, 3);
        g.rectsEnd();
    } else if (mode == 1) {
        const int s0 = lastStep_ < 0 ? 0 : (lastStep_ / 32) * 32;
        g.rectsBegin(kDim);
        for (int tr = 0; tr < kTracks; ++tr) g.rectsAdd(8, kTop + 8 + tr * 23, 784, 1);
        g.rectsEnd();
        g.rectsBegin(kWhite);
        for (int tr = 0; tr < kTracks; ++tr)
            for (int s = 0; s < 32; ++s) {
                const Step& x = a.pattern().tracks[tr].steps[(s0 + s) & 127];
                if (x.note >= 0) g.rectsAdd(8 + s * 24 + 2, kTop + 10 + tr * 23, 20, 20);
                else if (x.fx[0].type || x.fx[1].type) g.rectsAdd(8 + s * 24 + 9, kTop + 18 + tr * 23, 6, 4);
            }
        g.rectsEnd();
        if (lastStep_ >= 0) g.fill(8 + (lastStep_ - s0) * 24, kTop + 4, 24, 3, kWhite);
    } else {
        for (int tr = 0; tr < kTracks; ++tr) {
            const int s = a.seq.isPlaying() ? a.seq.step(tr) : 0;
            const Step& x = a.pattern().tracks[tr].steps[s < 0 ? 0 : (s & 127)];
            std::snprintf(t, sizeof(t), "%02X %02X %02X:%02X %02X:%02X %03d", uint8_t(x.note), x.instrument,
                          x.fx[0].type, x.fx[0].value, x.fx[1].type, x.fx[1].value, s);
            g.text(16 + (tr / 8) * 400, kTop + 14 + (tr % 8) * 46, kFont3, x.note >= 0 ? kWhite : kGrey, t);
        }
        std::snprintf(t, sizeof(t), "%08lX  %06.2f BPM", static_cast<unsigned long>(++frame_),
                      double(a.seq.effectiveTempo()));
        g.text(16, 400, kFont2, kGrey, t);
    }
}

void DataScreen::onButton(uint8_t id, uint8_t state) {
    if (state != kPress) return;
    if (id <= btn::F3) {
        mode = id;
        refreshBar();
        ui_.dirty();
        return;
    }
    Screen::onButton(id == btn::F8 ? uint8_t(btn::Play) : id, state);   // F8 = Play, as the label says
    refreshBar();
}

void DataScreen::padLeds(uint8_t* lv) {
    // Columns = the next 12 steps, rows = track groups 1-4 / 5-8 / 9-12 / 13-16.
    App& a = app_;
    const uint8_t full = a.cfg.padFull();
    const int base = lastStep_ < 0 ? 0 : lastStep_;
    for (int p = 0; p < 48; ++p) {
        const int row = p / 12, col = p % 12;
        bool hit = false;
        for (int tr = row * 4; tr < row * 4 + 4; ++tr) {
            const Track& trk = a.pattern().tracks[tr];
            hit = hit || trk.steps[(base + col) % trk.length()].note >= 0;
        }
        lv[p] = hit ? (col == 0 ? full : uint8_t(full / 2)) : 0;
    }
}

}  // namespace ot

#endif  // !OT_GAMES
