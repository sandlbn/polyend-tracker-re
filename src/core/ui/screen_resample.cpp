// [I] Resample screen: live resampling (app/resample.h) while the pattern
// plays. Opened by the Sample Recorder's "Resample" prompt button; Shift +
// Sample Recorder arms / stops a take from any screen with these settings.
//
// Columns (x = 2 + 100 k, as the Sample Recorder): From, Length, Slices,
// Into, Place on, Gain and Monitor (Line in takes only), Record with the
// take's state (and the Line in level) above it.
// The screen routes the codec to Line in on entry: the recorder's Source
// list may have left it on a microphone (#9).
#include <cstdio>

#include "app/app.h"
#include "ui/screens.h"

namespace ot {

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
const char* const kFrom[ResampleSettings::kSources] = {"Master",  "Track 1", "Track 2", "Track 3", "Track 4",
                                                       "Track 5", "Track 6", "Track 7", "Track 8", "Line in"};
const char* const kLength[ResampleSettings::kLengths] = {"Manual", "1 bar", "2 bars", "4 bars", "8 bars", "16 bars"};
const char* const kSlices[ResampleSettings::kSlices] = {"Off", "Per beat", "Per step", "Beat hits", "Step hits"};
const char* const kPlace[ResampleSettings::kPlaces] = {"Off",     "Track 1", "Track 2", "Track 3", "Track 4",
                                                       "Track 5", "Track 6", "Track 7", "Track 8"};
const char* const kMonitor[2] = {"On", "Off"};
constexpr int kColX0 = 2;
constexpr int kColSlices = 2, kColInto = 3, kColPlace = 4, kColGain = 5, kColMonitor = 6, kColState = 7;
// Selectable columns: From, Length, Slices, Into, Place on; Gain / Monitor for Line in.
constexpr int kCols[7] = {0, 1, kColSlices, kColInto, kColPlace, kColGain, kColMonitor};
constexpr float kMinBar = 1.0f / float(wg::kPanelH - 25);

void listItem(void* ctx, int i, char* out, int cap) {
    std::snprintf(out, size_t(cap), "%s", static_cast<const char* const*>(ctx)[i]);
}

// Into: "New", then the sample slots by number and name.
void intoItem(void* ctx, int i, char* out, int cap) {
    const App& a = *static_cast<const App*>(ctx);
    if (i == 0) std::snprintf(out, size_t(cap), "New");
    else if (a.instrPresent[i - 1]) std::snprintf(out, size_t(cap), "%d %s", i, a.instruments[i - 1].name);
    else std::snprintf(out, size_t(cap), "%d", i);
}

void list(Gfx& g, const Theme& th, int col, const char* const* items, int count, int sel) {
    wg::ListView v;
    v.x = kColX0 + 100 * col;
    v.w = wg::columnW();
    v.count = count;
    v.sel = sel;
    v.selStyle = wg::ListSel::kWhite;
    v.item = listItem;
    v.ctx = const_cast<const char**>(items);
    wg::list(g, th, v);
}

void bar(Gfx& g, const Theme& th, int col, float frac, uint32_t fill = wg::kThemeColour) {
    wg::ValueBarStyle st;
    st.fill = fill;
    wg::valueBar(g, th, kColX0 + 100 * col, wg::kPanelY, wg::columnW(), wg::kPanelH, frac < kMinBar ? kMinBar : frac,
                 st);
}
}  // namespace

bool ResampleScreen::lineIn() const { return app_.resampleCfg.source == audio::AudioEngine::kTapLineIn; }

void ResampleScreen::start(int opt) {
    (void)opt;
    App& a = app_;
    a.previewAllOff();
    a.capture.stop();
    a.selectInput(0, a.cfg.recGain[0]);
    if (sel < 0 || sel > kColPlace) sel = lineIn() && (sel == kColGain || sel == kColMonitor) ? sel : 0;
    meter_ = 0;
    shown_ = 0xFFFFFFFFu;
    route();
    refreshBar();
}

void ResampleScreen::stop() {
    App& a = app_;
    a.capture.setMetering(false);
    a.inputMonitorMuted = false;
}

void ResampleScreen::route() {
    // From = Line in: meter the input and monitor it as the Sample Recorder
    // does (unity, Monitor Off keeps it out of the mix). Other sources hear
    // line in through the Global Mixer only.
    App& a = app_;
    const bool on = lineIn();
    a.capture.setMetering(on);
    a.inputMonitorMuted = on && a.cfg.recMonitor == 0;
    if (!on && (sel == kColGain || sel == kColMonitor)) sel = 0;
}

void ResampleScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    App& a = app_;
    const ResampleSettings& r = a.resampleCfg;
    b.clear();
    b.set(0, "From", kFrom[r.source % ResampleSettings::kSources]);
    b.set(1, "Length", kLength[r.length % ResampleSettings::kLengths]);
    b.set(kColSlices, "Slices", kSlices[r.slices % ResampleSettings::kSlices]);
    b.set(kColPlace, "Place on", kPlace[r.place % ResampleSettings::kPlaces]);
    char v[12];
    if (r.into) std::snprintf(v, sizeof(v), "Instr %u", unsigned(r.into));
    else std::snprintf(v, sizeof(v), "New");
    b.set(kColInto, "Into", v);
    std::snprintf(v, sizeof(v), "%u", unsigned(a.cfg.recGain[0]));
    b.set(kColGain, "Gain", v, lineIn());
    b.set(kColMonitor, "Monitor", a.cfg.recMonitor ? "On" : "Off", lineIn());
    const Resampler::State st = a.resampler.state();
    b.set(7, st == Resampler::kIdle ? "Record" : st == Resampler::kArmed ? "Cancel" : "Stop");
    b.fill(7, true);
    b.highlight(sel, true);
}

void ResampleScreen::change(int d) {
    App& a = app_;
    ResampleSettings& r = a.resampleCfg;
    switch (sel) {
        case 0:
            r.source = uint8_t(clampi(r.source + d, 0, ResampleSettings::kSources - 1));
            route();
            break;
        case 1: r.length = uint8_t(clampi(r.length + d, 0, ResampleSettings::kLengths - 1)); break;
        case kColSlices: r.slices = uint8_t(clampi(r.slices + d, 0, ResampleSettings::kSlices - 1)); break;
        case kColPlace: r.place = uint8_t(clampi(r.place + d, 0, ResampleSettings::kPlaces - 1)); break;
        case kColInto:
            r.into = uint8_t(clampi(r.into + d, 0, ResampleSettings::kInto - 1));
            a.resampleIntoOk = -1;
            break;
        case kColGain:
            a.cfg.recGain[0] = uint8_t(clampi(a.cfg.recGain[0] + d, 0, 100));
            a.selectInput(0, a.cfg.recGain[0]);
            a.markConfigDirty();
            break;
        case kColMonitor:
            a.cfg.recMonitor = d > 0 ? 0 : 1;   // the list: On above Off
            a.inputMonitorMuted = a.cfg.recMonitor == 0;
            a.markConfigDirty();
            break;
        default: break;
    }
    refreshBar();
    ui_.dirty();
}

void ResampleScreen::onEncoder(int d) { change(d); }

void ResampleScreen::onButton(uint8_t id, uint8_t st) {
    App& a = app_;
    if (st != kPress) return;
    if (id == btn::Play) {   // starts too: an armed take waits for Play
        a.togglePlay(ui_.shift());
        return;
    }
    if (id == btn::Rec || id == btn::F8) {
        ui_.resampleKey();
        refreshBar();
        return;
    }
    const int n = lineIn() ? 7 : 5;
    if (id == btn::Left || id == btn::Right) {
        int i = 0;
        while (i < n - 1 && kCols[i] != sel) ++i;
        sel = kCols[clampi(i + (id == btn::Right ? 1 : -1), 0, n - 1)];
        refreshBar();
        ui_.dirty();
        return;
    }
    if (id == btn::Up || id == btn::Down) {
        // Lists drawn top to bottom: Up = the item above; Gain: Up = more.
        const int d = id == btn::Up ? 1 : -1;
        change(sel == kColGain ? d : -d);
        return;
    }
    if (id >= btn::F1 && id <= btn::F7) {
        // F1..F5 select From / Length / Slices / Into / Place on (again: the
        // next item, with wrap); F6 / F7 Gain / Monitor (again on Monitor: toggle).
        const int c = id - btn::F1;
        int i = 0;
        while (i < n && kCols[i] != c) ++i;
        if (i == n) return;
        if (sel == c) {
            ResampleSettings& r = a.resampleCfg;
            if (c == 0) {
                r.source = uint8_t((r.source + 1) % ResampleSettings::kSources);
                route();
            }
            if (c == 1) r.length = uint8_t((r.length + 1) % ResampleSettings::kLengths);
            if (c == kColSlices) r.slices = uint8_t((r.slices + 1) % ResampleSettings::kSlices);
            if (c == kColPlace) r.place = uint8_t((r.place + 1) % ResampleSettings::kPlaces);
            if (c == kColInto) {
                r.into = uint8_t((r.into + 1) % ResampleSettings::kInto);
                a.resampleIntoOk = -1;
            }
            if (c == kColMonitor) change(a.cfg.recMonitor ? 1 : -1);
        }
        if (lineIn() || c <= kColPlace) sel = c;
        refreshBar();
        ui_.dirty();
        return;
    }
    Screen::onButton(id, st);
}

void ResampleScreen::update(uint32_t now) {
    App& a = app_;
    // Follow the take (bar count, Record / Cancel / Stop).
    const uint32_t key = uint32_t(a.resampler.state()) << 16 | a.resampler.barsDone() << 1 | (a.seq.isPlaying() ? 1u : 0u);
    if (key != shown_) {
        shown_ = key;
        refreshBar();
        ui_.dirty();
    }
    // Line in level, as the Sample Recorder's meter (one reading per frame).
    if (lineIn() && now - lastMeterAt_ >= RecorderScreen::kMeterMs) {
        const uint32_t dt = now - lastMeterAt_;
        lastMeterAt_ = now;
        const int before = meter_;
        meter_ = RecorderScreen::meterFall(meter_, meterUnits(a.capture.takePeak()), dt);
        if (meter_ != before) ui_.dirty();
    }
}

void ResampleScreen::draw(Gfx& g) {
    App& a = app_;
    const Theme& th = ui_.th();
    ui_.drawTitle(g, "Resample");
    const ResampleSettings& r = a.resampleCfg;
    list(g, th, 0, kFrom, ResampleSettings::kSources, r.source % ResampleSettings::kSources);
    list(g, th, 1, kLength, ResampleSettings::kLengths, r.length % ResampleSettings::kLengths);
    list(g, th, kColSlices, kSlices, ResampleSettings::kSlices, r.slices % ResampleSettings::kSlices);
    list(g, th, kColPlace, kPlace, ResampleSettings::kPlaces, r.place % ResampleSettings::kPlaces);
    {
        wg::ListView v;
        v.x = kColX0 + 100 * kColInto;
        v.w = wg::columnW();
        v.count = ResampleSettings::kInto;
        v.sel = r.into % ResampleSettings::kInto;
        intoTop_ = wg::listFollow(v.sel, v.count, wg::listVisibleRows(v.h), intoTop_);
        v.top = intoTop_;
        v.selStyle = wg::ListSel::kWhite;
        v.item = intoItem;
        v.ctx = &a;
        wg::list(g, th, v);
    }
    // The take's state above Record; for Line in the level below.
    wg::panel(g, th, kColX0 + 100 * kColState);
    const Resampler& rs = a.resampler;
    const char* line1 = "Ready";
    char line2[24] = "";
    switch (rs.state()) {
        case Resampler::kArmed:
            line1 = "Armed";
            std::snprintf(line2, sizeof(line2), a.seq.isPlaying() ? "next bar" : "with Play");
            break;
        case Resampler::kRecording:
        case Resampler::kStopping:
            line1 = rs.state() == Resampler::kStopping ? "Stopping" : "Recording";
            if (rs.bars())
                std::snprintf(line2, sizeof(line2), "bar %lu/%u", static_cast<unsigned long>(rs.barsDone() + 1), rs.bars());
            else
                std::snprintf(line2, sizeof(line2), "bar %lu", static_cast<unsigned long>(rs.barsDone() + 1));
            break;
        default: {
            bool inPlace = false;
            const int t = a.resampleTarget(0, &inPlace, nullptr);
            if (t < 0) std::snprintf(line2, sizeof(line2), "no free slot");
            else std::snprintf(line2, sizeof(line2), inPlace ? "replace %d" : "-> Instr %d", t + 1);
            break;
        }
    }
    const int cx = wg::columnX(kColState) + wg::columnW() / 2;
    const bool on = rs.state() == Resampler::kRecording || rs.state() == Resampler::kStopping;
    g.text(cx, 60, kFontText, on ? th.alert : th.text, line1, IDisplay::kOptCenter);
    if (line2[0]) g.text(cx, 88, kFontSmall, th.dim, line2, IDisplay::kOptCenter);
    if (lineIn()) {
        // Line in level: a vertical bar, green / yellow over 70 / red from 85.
        const int units = clampi(meter_, 0, 100);
        const uint32_t c = units >= 85 ? th.alert : units > 70 ? th.yellow : th.good;
        constexpr int kTop = 130, kBottom = 400, kW = 24;
        const int x0 = cx - kW / 2, h = (kBottom - kTop) * units / 100;
        g.text(cx, 118, kFontSmall, th.dim, "Level", IDisplay::kOptCenter);
        g.rectsBegin(th.dim);
        g.rectsAdd(x0, kTop, kW, kBottom - kTop - h);
        g.rectsEnd();
        if (h > 0) {
            g.rectsBegin(c);
            g.rectsAdd(x0, kBottom - h, kW, h);
            g.rectsEnd();
        }
        bar(g, th, kColGain, float(a.cfg.recGain[0]) / 100.0f);
        list(g, th, kColMonitor, kMonitor, 2, a.cfg.recMonitor ? 0 : 1);
    } else {
        wg::panel(g, th, kColX0 + 100 * kColGain);
        wg::panel(g, th, kColX0 + 100 * kColMonitor);
    }
    wg::panelFocus(g, th, wg::columnX(sel), wg::columnW());
}

}  // namespace ot
