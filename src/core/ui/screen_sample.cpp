// Sample Playback (spec 05 §2.9 + appendix B §1; spec 13 §F auto slice,
// §H.2 snap-to-zero indicator). The Sample Editor is in screen_editor.cpp,
// the Sample Recorder in screen_recorder.cpp.
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "audio/zero_snap.h"
#include "ui/screens.h"

namespace ot {

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Play-mode names (appendix B §1.3 table 0xa9578, spec 16 §B.5).
const char* kModes[8] = {"1-Shot", "Forward Loop", "Backward Loop", "Pingpong Loop", "Slice", "Beat Slice", "Wavetable", "Granular"};
const char* kShapes[3] = {"Square", "Triangle", "Gauss"};
const char* kLoops[3] = {"Forward", "Backward", "PingPong"};

// Waveform 600 px wide (appendix B §1.4) at x 0..599, rows 116..334 with
// the centre at 225; markers 108..334 (loop markers to 342); the play-mode
// list is the column panel above the Play Mode label (stock 008 / 025,
// VISUAL_DIFF §3.7, §8.14).
constexpr int kWaveX = 0, kWaveY = 116, kWaveW = 600, kWaveH = 219;
constexpr int kMarkTop = 108, kMarkBottom = 334, kLoopBottom = 342;
constexpr int kListX = 601, kListW = 197;

bool isSlice(int m) { return m == int(PlayMode::Slice) || m == int(PlayMode::BeatSlice); }
bool isLoop(int m) { return m >= 1 && m <= 3; }

// Point value by index 0 start, 1 loop start, 2 loop end, 3 end.
uint16_t& pointRef(Instrument& in, int which) {
    switch (which) {
        case 0: return in.startPoint;
        case 1: return in.loopPoint1;
        case 2: return in.loopPoint2;
        default: return in.endPoint;
    }
}

int markerX(uint16_t m, float v0, float v1) {
    const float f = float(m) / 65535.0f;
    return kWaveX + int((f - v0) / (v1 - v0) * float(kWaveW - 1) + 0.5f);
}

// Wavetable window size n (32 << n) limited to the sample length.
int maxWindowExp(uint32_t len) {
    int n = 11;
    while (n > 5 && (1u << n) > len) --n;
    return n;
}

void modeItem(void*, int i, char* out, int cap) { std::snprintf(out, size_t(cap), "%s", kModes[i & 7]); }
}  // namespace

// Waveform chrome and bar glyphs (wave.cpp).
void waveRegion(Gfx& g, const Theme& th, int xa, int xb, int y0, int h);
void waveCentre(Gfx& g, const Theme& th, int x0, int w, int y);
void waveMarker(Gfx& g, const Theme& th, int x, uint32_t c, int dir, bool top, bool bottom, int y0, int y1);
void barSelector(Gfx& g, const Theme& th, int i, const char* title, int d);

// ---------------------------------------------------------------------------

void PlaybackScreen::start(int opt) {
    (void)opt;
    App& a = app_;
    const uint8_t ci = a.currentInstrument();
    if (ci < kSampleInstrumentCount) {
        const Instrument& in = a.instruments[ci];
        // Appendix B §1.2: another instrument (or play mode) since the last
        // visit resets zoom and selection; selection = Play Mode.
        if (ci != lastInstr_ || in.playMode != lastMode_ || view.len != a.bank.frames(ci)) {
            view.reset(a.bank.frames(ci));
            view.centre = isSlice(in.playMode) && in.sliceCount ? in.slices[in.selectedSlice] : in.startPoint;
            sel = 6;
            more = false;
        }
        lastInstr_ = ci;
        lastMode_ = in.playMode;
    }
    heldPoints = 0;
    encPushDown_ = false;
    refreshBar();
}

void PlaybackScreen::stop() {
    app_.previewAllOff();
    heldPoints = 0;
    midiNote = -1;
    headTrack_ = headX_ = -1;
    // The stock detector keeps running in the background service; here it is
    // stepped by this screen, so finish it before the bank can move.
    if (slicer.busy()) {
        slicer.finish();
        sliceDone();
    }
}

void PlaybackScreen::update(uint32_t now) {
    // spec 13 §F.1: one 2048-frame block per main-loop pass.
    if (slicer.busy()) slicer.step();
    if (wasBusy_ && !slicer.busy()) sliceDone();
    wasBusy_ = slicer.busy();
    // spec 05 §6 (0x52cc0): the playhead is redrawn every >= 5 ms; a frame
    // is requested only when its pixel moves (or it appears / disappears).
    if (now - headAt_ >= kHeadPollMs) {
        headAt_ = now;
        const uint8_t ci = app_.currentInstrument();
        const bool wt = ci < kSampleInstrumentCount && (app_.instruments[ci].playMode & 7) == uint8_t(PlayMode::Wavetable);
        const bool gr = ci < kSampleInstrumentCount && (app_.instruments[ci].playMode & 7) == uint8_t(PlayMode::Granular);
        if (wt) {
            if (wavetableShownWindow() != wtShown_) ui_.dirty();   // the window moves (LFO / envelope)
        } else if (playheadX() != headX_ || (gr && granularShownPosition() != grainShown_)) {
            ui_.dirty();
        }
    }
}

void PlaybackScreen::followHead(uint16_t voiceId) {
    headTrack_ = app_.auditionTrack(voiceId);
    headAt_ -= kHeadPollMs;   // poll on the next pass
}

uint32_t PlaybackScreen::wavetableShownWindow() const {
    const App& a = app_;
    const uint8_t ci = a.currentInstrument();
    if (ci >= kSampleInstrumentCount) return 0;
    const Instrument& in = a.instruments[ci];
    if (headTrack_ >= 0 && headTrack_ < audio::kVoices && a.engine.voice(headTrack_).instrument() == &in &&
        a.engine.voicePlayhead(headTrack_) >= 0)
        return a.engine.voice(headTrack_).wavetableWindow();
    return in.wavetableCurrentWindow;
}

uint16_t PlaybackScreen::granularShownPosition() const {
    const App& a = app_;
    const uint8_t ci = a.currentInstrument();
    if (ci >= kSampleInstrumentCount) return 0;
    const Instrument& in = a.instruments[ci];
    if (headTrack_ >= 0 && headTrack_ < audio::kVoices && a.engine.voice(headTrack_).instrument() == &in &&
        a.engine.voicePlayhead(headTrack_) >= 0)
        return a.engine.voice(headTrack_).grainPosition();
    return in.granularPosition;
}

int PlaybackScreen::wavetablePositionX() const {
    const App& a = app_;
    const uint8_t ci = a.currentInstrument();
    if (ci >= kSampleInstrumentCount || (a.instruments[ci].playMode & 7) != uint8_t(PlayMode::Wavetable)) return -1;
    const uint32_t n = a.instruments[ci].wavetableWindowCount;
    const uint32_t w = wavetableShownWindow();
    const float f = n > 1 ? float(w < n ? w : n - 1) / float(n - 1) : 0.0f;
    return kWaveX + 10 + int(f * float(kWaveW - 20) + 0.5f);
}

int PlaybackScreen::playheadX() const {
    // USER_REPORTS_R2 §1c: the play position of the voice this screen last
    // started (pads, Enc-push, F1 Preview, MIDI in), all play modes, while
    // that voice still plays the current instrument. SPEC GAP: no stock
    // capture of the look; drawn as a 1-px line in the Loader trim
    // playhead's colour (th.good).
    const App& a = app_;
    const uint8_t ci = a.currentInstrument();
    if (headTrack_ < 0 || headTrack_ >= audio::kVoices || ci >= kSampleInstrumentCount) return -1;
    if (a.engine.voice(headTrack_).instrument() != &a.instruments[ci]) return -1;
    const int32_t p = a.engine.voicePlayhead(headTrack_);
    const uint32_t len = a.bank.frames(ci);
    if (p < 0 || len == 0) return -1;
    const uint32_t m = uint32_t(uint64_t(uint32_t(p) < len ? uint32_t(p) : len) * 65535u / len);
    float v0, v1;
    view.window(&v0, &v1);
    const int x = markerX(uint16_t(m), v0, v1);
    return x >= kWaveX && x < kWaveX + kWaveW ? x : -1;
}

void PlaybackScreen::sliceDone() {
    // spec 13 §F.7: redraw, "N of M" label, pads, re-arm the voices.
    wasBusy_ = false;
    const uint8_t ci = app_.currentInstrument();
    if (ci < kSampleInstrumentCount) {
        Instrument& in = app_.instruments[ci];
        if (in.sliceCount) keepVisible(in.slices[in.selectedSlice], false);
    }
    if (sliceInstr_ >= 0) app_.markInstrumentDirty(sliceInstr_);
    sliceInstr_ = -1;
    refreshBar();
    ui_.dirty();
}

uint8_t PlaybackScreen::zeroMask() const {
    const uint8_t ci = app_.currentInstrument();
    if (ci >= kSampleInstrumentCount) return 0;
    const Instrument& in = app_.instruments[ci];
    const int16_t* d = app_.bank.left(ci);
    const uint32_t len = app_.bank.frames(ci);
    const bool snap = app_.project.u8(0x602) == 1;   // Snap to zero (.mt 0x602)
    uint8_t m = 0;
    const uint16_t pts[4] = {in.startPoint, in.loopPoint1, in.loopPoint2, in.endPoint};
    for (int i = 0; i < 4; ++i)
        if (audio::markerOnCrossing(d, len, pts[i], snap)) m |= uint8_t(1 << i);
    return m;
}

void PlaybackScreen::keepVisible(uint16_t m, bool force) {
    // Appendix B §1.9: the view re-centres when the point leaves it.
    float v0, v1;
    view.window(&v0, &v1);
    const float f = float(m) / 65535.0f;
    if (force || f < v0 || f > v1) view.centre = m;
}

void PlaybackScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    App& a = app_;
    b.clear();
    const uint8_t ci = a.currentInstrument();
    if (ci >= kSampleInstrumentCount) return;
    const Instrument& in = a.instruments[ci];
    const float secs = float(a.bank.frames(ci)) / 44100.0f;
    char v[24];
    auto t = [&](uint16_t m, const char* fmt) {
        std::snprintf(v, sizeof(v), fmt, double(secs * float(m) / 65535.0f));
        return v;
    };
    const int mode = in.playMode & 7;
    if (mode <= 3) {
        // Appendix B §1.4 a).
        b.set(0, "Preview", "-");
        b.set(1, "Start", t(in.startPoint, "%.3fs"));
        // 1-Shot: F3/F4 stay blank [S snap 008].
        if (mode) {
            b.set(2, "Loop Start", t(in.loopPoint1, "%.3fs"));
            b.set(3, "Loop End", t(in.loopPoint2, "%.3fs"));
        }
        b.set(4, "End", t(in.endPoint, "%.3fs"));
        std::snprintf(v, sizeof(v), "%.2f", double(view.zoom()));
        b.set(5, "Zoom", v);
    } else if (isSlice(mode) && !more) {
        // b) page 0.
        if (in.sliceCount) std::snprintf(v, sizeof(v), "%d of %d", in.selectedSlice + 1, in.sliceCount);
        else std::snprintf(v, sizeof(v), "-");
        b.set(0, "Slice", v);
        b.set(1, "Adjust", in.sliceCount ? t(in.slices[in.selectedSlice], "%.3f s") : "-");
        b.set(2, "Add");
        b.set(3, "Remove");
        std::snprintf(v, sizeof(v), "%.2f", double(view.zoom()));
        b.set(4, "Zoom", v);
        b.set(5, "More");
    } else if (isSlice(mode)) {
        // b) page 1 ("More").
        b.setName(0, "Slice", "Auto");
        b.setName(1, "Slice", "Evenly");
        std::snprintf(v, sizeof(v), "%d", a.cfg.sliceCount);
        b.set(2, "No of Slices", v);
        b.set(3, "", nullptr, false);
        b.set(4, "", nullptr, false);
        b.set(5, "More");
        b.highlight(5, true);
    } else if (mode == 7) {
        // c) granular.
        b.set(0, "Preview", "-");
        b.set(1, "Position", t(in.granularPosition, "%0.3f s"));
        std::snprintf(v, sizeof(v), "%0.1f ms", double(in.grainLength / 44.1f));
        b.set(2, "Length", v);
        b.set(3, "Shape", kShapes[in.grainShape % 3]);
        b.set(4, "Loop", kLoops[in.granularLoop % 3]);
    } else {
        // d) wavetable.
        b.set(0, "Preview", "-");
        std::snprintf(v, sizeof(v), "%u", unsigned(in.wavetableCurrentWindow));
        b.set(1, "Position", v);
        std::snprintf(v, sizeof(v), "%u", unsigned(in.wavetableWindowSize));
        b.set(2, "Window", v);
    }
    // "▲ Play Mode ▼" selector [S snap 008, 025]: draw() renders it; the
    // label keeps the mode name but is not drawn. The focused list's red
    // frame marks the selection, not the label.
    b.setWide(6, "Play Mode", kModes[mode]);
    b.label[6].hidden = true;
    b.separators = uint8_t(b.separators & ~(1u << 7));
    for (int i = 0; i < 6; ++i) b.highlight(i, i == sel || (i >= 1 && i <= 4 && (heldPoints >> (i - 1)) & 1) || (i == 5 && more && isSlice(mode)));
    b.stripBg = wg::mix(0x000000, ui_.th().slot(slot::kStrip + 1), 128);   // 050505 strip [Part 2 C5]
}

void PlaybackScreen::setMode(int m) {
    App& a = app_;
    const uint8_t ci = a.currentInstrument();
    Instrument& in = a.instruments[ci];
    in.playMode = uint8_t(clampi(m, 0, 7));
    const uint32_t len = a.bank.frames(ci);
    // Appendix B §1.3 side effects.
    if (in.playMode == uint8_t(PlayMode::Wavetable)) {
        in.sampleType = 1;
        uint32_t lim = in.wavetableWindowSize < len ? in.wavetableWindowSize : len;
        if (lim > 2048) lim = 2048;
        uint32_t size = 32;
        while (size * 2 <= lim) size *= 2;
        in.wavetableWindowSize = uint16_t(size);
        in.wavetableWindowCount = len / size;
        if (in.wavetableWindowCount && in.wavetableCurrentWindow >= in.wavetableWindowCount)
            in.wavetableCurrentWindow = in.wavetableWindowCount - 1;
    } else {
        in.sampleType = 0;
    }
    if (isLoop(in.playMode)) {
        // keep start < loop start < loop end < end
        if (in.loopPoint1 <= in.startPoint) in.loopPoint1 = uint16_t(in.startPoint + 1);
        if (in.loopPoint2 >= in.endPoint) in.loopPoint2 = uint16_t(in.endPoint - 1);
        if (in.loopPoint2 <= in.loopPoint1) {
            in.loopPoint1 = uint16_t(in.startPoint + 1);
            in.loopPoint2 = uint16_t(in.endPoint - 1);
        }
    }
    if (isSlice(in.playMode) && in.sliceCount) keepVisible(in.slices[in.selectedSlice], true);
    if (!isSlice(in.playMode)) more = false;
}

void PlaybackScreen::movePoint(int which, int d) {
    // Appendix B §1.8 constraints (point units 0..65535).
    Instrument& in = app_.instruments[app_.currentInstrument()];
    const bool loopMode = isLoop(in.playMode);
    int s = in.startPoint, l1 = in.loopPoint1, l2 = in.loopPoint2, e = in.endPoint;
    switch (which) {
        case 0: {
            s = clampi(s + d, 0, 65535);
            if (s >= e) s = e - 2;
            if (s < 0) s = 0;
            if (loopMode && l1 <= s) {
                const int ll = l2 - l1;
                l1 = s + 1;
                l2 = l1 + ll;
                if (l2 >= e) {
                    l2 = e - 1;
                    l1 = l2 - ll;
                    s = l1 - 1;
                }
            }
            break;
        }
        case 1:
            if (loopMode) l1 = clampi(l1 + d, s + 1, l2 - 1 > 0xFFFF ? 0xFFFF : l2 - 1);
            break;
        case 2:
            if (loopMode) l2 = clampi(l2 + d, l1 + 1, e - 1);
            break;
        default: {
            e = clampi(e + d, 0, 65535);
            if (e <= s) e = s + 2;
            if (e > 65535) e = 65535;
            if (loopMode && l2 >= e) {
                const int ll = l2 - l1;
                l2 = e - 1;
                l1 = l2 - ll;
                if (l1 <= s) s = l1 - 1;
            }
            break;
        }
    }
    in.startPoint = uint16_t(clampi(s, 0, 65535));
    in.loopPoint1 = uint16_t(clampi(l1, 0, 65535));
    in.loopPoint2 = uint16_t(clampi(l2, 0, 65535));
    in.endPoint = uint16_t(clampi(e, 0, 65535));
}

void PlaybackScreen::change(int d) {
    App& a = app_;
    const uint8_t ci = a.currentInstrument();
    if (ci >= kSampleInstrumentCount || d == 0) return;
    Instrument& in = a.instruments[ci];
    const int stepU = view.pixelUnits();   // one detent = one pixel (§1.8)
    const int mode = in.playMode;
    if (sel == 6) {
        setMode(in.playMode + clampi(d, -4, 4));   // §1.3
    } else if (mode <= 3) {
        if (heldPoints) {
            // §1.5 multi-select: negative deltas from start towards end,
            // positive from end towards start.
            for (int k = 0; k < 4; ++k) {
                const int w = d < 0 ? k : 3 - k;
                if ((heldPoints >> w) & 1) movePoint(w, d * stepU);
            }
        } else if (sel >= 1 && sel <= 4) {
            movePoint(sel - 1, d * stepU);
            keepVisible(pointRef(in, sel - 1), false);
        } else if (sel == 5) {
            view.step(d);
            return;
        } else {
            return;
        }
    } else if (isSlice(mode)) {
        if (sel == 0 && in.sliceCount) {
            in.selectedSlice = uint8_t(clampi(in.selectedSlice + d, 0, in.sliceCount - 1));
            keepVisible(in.slices[in.selectedSlice], false);
        } else if (sel == 1 && in.sliceCount) {
            const int s = in.selectedSlice;
            const int lo = s > 0 ? in.slices[s - 1] + 1 : 0;
            const int hi = s + 1 < in.sliceCount ? in.slices[s + 1] - 1 : 65535;
            in.slices[s] = uint16_t(clampi(in.slices[s] + d * stepU, lo, hi));
            keepVisible(in.slices[s], false);
        } else if (sel == 2 && more) {
            a.cfg.sliceCount = uint8_t(clampi(a.cfg.sliceCount + d, 1, 48));
            a.markConfigDirty();
            return;
        } else if (sel == 4) {
            if (in.sliceCount) view.centre = in.slices[in.selectedSlice];
            view.step(d);
            return;
        } else {
            return;
        }
    } else if (mode == 7) {
        // §1.11
        if (sel == 1) in.granularPosition = uint16_t(clampi(in.granularPosition + d * 65, 0, 65535));
        else if (sel == 2) {
            const int mx = int(a.bank.frames(ci) < 44100 ? a.bank.frames(ci) : 44100);
            in.grainLength = uint16_t(clampi(in.grainLength + d * 11, 44, mx > 44 ? mx : 44));
        } else if (sel == 3) in.grainShape = uint8_t(clampi(in.grainShape + (d > 0 ? 1 : -1), 0, 2));
        else if (sel == 4) in.granularLoop = uint8_t(clampi(in.granularLoop + (d > 0 ? 1 : -1), 0, 2));
        else return;
    } else {
        // §1.12 wavetable
        const uint32_t len = a.bank.frames(ci);
        if (sel == 1) {
            const uint32_t wins = in.wavetableWindowSize ? len / in.wavetableWindowSize : 0;
            in.wavetableCurrentWindow = uint32_t(clampi(int(in.wavetableCurrentWindow) + d, 0, wins ? int(wins) - 1 : 0));
        } else if (sel == 2) {
            int n = 5;
            while ((1 << n) < in.wavetableWindowSize && n < 11) ++n;
            const int nn = clampi(n + d, 5, maxWindowExp(len));
            // Rescale the position so the same audio region stays selected.
            uint32_t pos = in.wavetableCurrentWindow;
            pos = nn > n ? pos >> (nn - n) : pos << (n - nn);
            in.wavetableWindowSize = uint16_t(1 << nn);
            in.wavetableWindowCount = len / in.wavetableWindowSize;
            if (in.wavetableWindowCount && pos >= in.wavetableWindowCount) pos = in.wavetableWindowCount - 1;
            in.wavetableCurrentWindow = pos;
        } else {
            return;
        }
    }
    a.markInstrumentDirty(ci);
}

// ---------------------------------------------------------------------------
// Slices (appendix B §1.10)

void PlaybackScreen::addSlice() {
    const uint8_t ci = app_.currentInstrument();
    if (ci >= kSampleInstrumentCount) return;
    Instrument& in = app_.instruments[ci];
    if (in.sliceCount == 0) {
        in.slices[0] = 0;
        in.sliceCount = 1;
        in.selectedSlice = 0;
    } else if (in.sliceCount < kSliceCount) {
        const int s = in.selectedSlice;
        const int next = s + 1 < in.sliceCount ? in.slices[s + 1] : 65535;
        for (int k = in.sliceCount; k > s + 1; --k) in.slices[k] = in.slices[k - 1];
        in.slices[s + 1] = uint16_t((in.slices[s] + next) / 2);
        ++in.sliceCount;
        in.selectedSlice = uint8_t(s + 1);
    } else {
        return;   // refused at 48
    }
    keepVisible(in.slices[in.selectedSlice], false);
    app_.markInstrumentDirty(ci);
}

void PlaybackScreen::removeSlice() {
    const uint8_t ci = app_.currentInstrument();
    if (ci >= kSampleInstrumentCount) return;
    Instrument& in = app_.instruments[ci];
    if (!in.sliceCount) return;
    for (int k = in.selectedSlice; k < in.sliceCount - 1; ++k) in.slices[k] = in.slices[k + 1];
    in.slices[in.sliceCount - 1] = 0;
    --in.sliceCount;
    if (in.selectedSlice > 0) --in.selectedSlice;
    app_.markInstrumentDirty(ci);
}

void PlaybackScreen::slicesEvenly() {
    // 0x6cd4c: count = "No of Slices", slice k at k*65535/count (synchronous).
    const uint8_t ci = app_.currentInstrument();
    if (ci >= kSampleInstrumentCount) return;
    Instrument& in = app_.instruments[ci];
    const int n = clampi(app_.cfg.sliceCount, 1, kSliceCount);
    std::memset(in.slices, 0, sizeof(in.slices));
    for (int k = 0; k < n; ++k) in.slices[k] = uint16_t(k * 65535 / n);
    in.sliceCount = uint8_t(n);
    in.selectedSlice = 0;
    app_.markInstrumentDirty(ci);
}

void PlaybackScreen::slicesAuto() {
    // spec 13 §F.2 (0x6cd14): clear the slice array, selected slice 0, then
    // arm the detector; it fills positions and the count incrementally.
    const uint8_t ci = app_.currentInstrument();
    if (ci >= kSampleInstrumentCount) return;
    Instrument& in = app_.instruments[ci];
    slicer.cancel();
    std::memset(in.slices, 0, sizeof(in.slices));
    in.sliceCount = 0;
    in.selectedSlice = 0;
    sliceInstr_ = ci;
    if (slicer.start(app_.bank.left(ci), app_.bank.frames(ci), in.slices, &in.sliceCount)) wasBusy_ = true;
    app_.markInstrumentDirty(ci);
}

void PlaybackScreen::requestSlices(bool autoMode) {
    const uint8_t ci = app_.currentInstrument();
    if (ci >= kSampleInstrumentCount || slicer.busy()) return;
    if (app_.instruments[ci].sliceCount == 0) {
        if (autoMode) slicesAuto();
        else slicesEvenly();
        return;
    }
    // Appendix B §1.10: two-line rewrite confirmation, F7 Cancel, F8 Confirm.
    ui_.ask("Slices will be rewritten", "Do you want to continue?", nullptr, "Cancel", "Confirm",
            autoMode ? +[](void* ctx, int key) {
                auto* self = static_cast<PlaybackScreen*>(ctx);
                if (key == btn::F8) self->slicesAuto();
                self->refreshBar();
            }
                     : +[](void* ctx, int key) {
                           auto* self = static_cast<PlaybackScreen*>(ctx);
                           if (key == btn::F8) self->slicesEvenly();
                           self->refreshBar();
                       },
            this);
}

// ---------------------------------------------------------------------------
// Input

void PlaybackScreen::onButton(uint8_t id, uint8_t state) {
    App& a = app_;
    const uint8_t ci = a.currentInstrument();
    if (id == btn::Instr) {
        if (state == kPress)
            ui_.picker.open(2, "Instrument", kSampleInstrumentCount, ci < 48 ? ci : 0,
                            [](void* ctx, int i, char* out, int cap) {
                                App* ap = static_cast<App*>(ctx);
                                std::snprintf(out, size_t(cap), "%02d %s", i + 1, ap->instrPresent[i] ? ap->instruments[i].name : "");
                            },
                            &a);
        else if (state == kRelease && ui_.picker.isOpen()) {
            // §1.6: on release the screen re-reads the instrument.
            a.setCurrentInstrument(ui_.picker.cursor());
            ui_.picker.close();
            start(0);
        }
        refreshBar();
        return;
    }
    if (ci >= kSampleInstrumentCount || ui_.picker.isOpen()) {
        if (id != btn::Aux) Screen::onButton(id, state);
        return;
    }
    Instrument& in = a.instruments[ci];
    const int mode = in.playMode;
    const bool slice = isSlice(mode);
    // b0 Preview (type 3): note 60 velocity 127, release = note off (§1.6).
    if (id == btn::F1 && !slice) {
        if (state == kPress) {
            a.previewOn(App::kAuditionSampleEditor, ci, 60, 127);
            followHead(App::kAuditionSampleEditor);
        } else if (state == kRelease) a.previewOff(App::kAuditionSampleEditor);
        return;
    }
    // Enc-push (0x21): pad-12 preview; selects slice 12 in Beat Slice (§1.6).
    if (id == btn::EncPush) {
        if (state == kPress && !encPushDown_) {
            encPushDown_ = true;
            onPad(12, kPress);
        } else if (state == kRelease && encPushDown_) {
            encPushDown_ = false;
            onPad(12, kRelease);
        }
        return;
    }
    // Point buttons b1..b4 (point modes): select / multi-select (§1.5).
    if (mode <= 3 && id >= btn::F2 && id <= btn::F5) {
        const int w = id - btn::F2;   // 0 start, 1 loop start, 2 loop end, 3 end
        if ((w == 1 || w == 2) && mode == 0) return;   // ignored in 1-shot
        if (state == kPress) {
            sel = w + 1;
            heldPoints |= uint8_t(1 << w);
            if (view.zoom() > 1.0f) keepVisible(pointRef(in, w), true);
        } else if (state == kRelease) {
            heldPoints &= uint8_t(~(1 << w));
        }
        refreshBar();
        return;
    }
    if (state != kPress) return;
    switch (id) {
        case btn::F1:   // slice modes
            if (!more) sel = 0;
            else requestSlices(true);
            break;
        case btn::F2:
            if (slice) {
                if (!more) sel = 1;
                else requestSlices(false);
            } else {
                sel = 1;   // granular position / wavetable position
            }
            break;
        case btn::F3:
            if (slice) {
                if (!more) addSlice();
                else sel = 2;
            } else {
                sel = 2;   // granular length / wavetable window
            }
            break;
        case btn::F4:
            if (slice && !more) removeSlice();
            else if (mode == 7) sel = 3;
            break;
        case btn::F5:
            if (slice && !more) sel = 4;
            else if (mode == 7) sel = 4;
            break;
        case btn::F6:
            // b5: Zoom in point modes, More toggle in slice modes, ignored in 6/7.
            if (slice) {
                more = !more;
                if (sel != 6) sel = more ? 2 : 0;
            } else if (mode <= 3) {
                sel = 5;
            }
            break;
        case btn::F7:
        case btn::F8:
            // §1.3: the first press selects Play Mode, further presses step it.
            if (sel == 6) {
                setMode(mode + (id == btn::F8 ? 1 : -1));
                a.markInstrumentDirty(ci);
            }
            sel = 6;
            break;
        case btn::Insert:
            if (slice && !more) addSlice();   // 0x51028
            break;
        case btn::Delete:
            // 0x536e4: slice modes, selection 0/1 -> remove the selected slice.
            // Point modes (0x5351c) are not decoded (appendix B open item).
            if (slice && (sel == 0 || sel == 1)) removeSlice();
            break;
        case btn::Up:
        case btn::Down: {
            // The Play mode list (sel 6) is drawn top to bottom: Up = the
            // item above, Down = the item below, like the encoder (+ = next
            // item down) [S owner test on 1.9.2]. Values: Up = higher.
            const int d = id == btn::Up ? 1 : -1;
            change(sel == 6 ? -d : d);
            break;
        }
        case btn::Left:
        case btn::Right: {
            // Visible parameters per mode (jump table 0x52120).
            static const int kPoint0[] = {1, 4, 5, 6};
            static const int kPointL[] = {1, 2, 3, 4, 5, 6};
            static const int kSlice0[] = {0, 1, 4, 6};
            static const int kSlice1[] = {2, 6};
            static const int kGran[] = {1, 2, 3, 4, 6};
            static const int kWave[] = {1, 2, 6};
            const int* l;
            int n;
            if (mode == 0) l = kPoint0, n = 4;
            else if (mode <= 3) l = kPointL, n = 6;
            else if (slice) l = more ? kSlice1 : kSlice0, n = more ? 2 : 4;
            else if (mode == 7) l = kGran, n = 5;
            else l = kWave, n = 3;
            int i = 0;
            while (i < n && l[i] < sel) ++i;
            if (id == btn::Left) --i;
            else if (i < n && l[i] == sel) ++i;
            sel = l[clampi(i, 0, n - 1)];
            break;
        }
        case btn::Play:
            // §1.6: stops a running sequencer (the other branch is not decoded).
            if (a.seq.isPlaying()) a.togglePlay(false);
            break;
        default: break;   // Rec, Shift, Aux: no action here
    }
    refreshBar();
}

void PlaybackScreen::onPad(uint8_t pad, uint8_t state) {
    // §1.10: pad -> note through the pad map, velocity 127; Beat Slice also
    // selects slice = pad when it exists. The selection is written BEFORE the
    // note-on: an idle voice starts inside the call (USER_REPORTS_R2 §1b).
    const uint8_t ci = app_.currentInstrument();
    if (state == kPress && ci < kSampleInstrumentCount) {
        Instrument& in = app_.instruments[ci];
        if (in.playMode == uint8_t(PlayMode::BeatSlice) && pad < in.sliceCount) {
            in.selectedSlice = pad;
            keepVisible(in.slices[pad], false);
            refreshBar();
        }
    }
    Screen::onPad(pad, state);
    if (state == kPress) followHead(pad);
}

bool PlaybackScreen::onMidiNote(uint8_t note, uint8_t velocity, bool on) {
    // spec 17 §1.3 owner 2: the SP preview note-on/off (0x51b90 / 0x51d3c),
    // the call b0 Preview makes with note 60 (appendix B §1.6). SPEC GAP
    // (§1.12 leaves the owner semantics to the screen): one preview voice
    // (the b0 voice), the last note sounds and only its note-off releases
    // it; velocity as on the live path; in Beat Slice note n plays slice n - 24
    // (the voice derives it, USER_REPORTS_R2 §1a).
    App& a = app_;
    if (on) {
        if (a.previewOn(App::kAuditionSampleEditor, a.currentInstrument(), note, velocity) >= 0) midiNote = note;
        else midiNote = -1;
        followHead(App::kAuditionSampleEditor);
    } else if (midiNote == int(note)) {
        a.previewOff(App::kAuditionSampleEditor);
        midiNote = -1;
    }
    return true;
}

void PlaybackScreen::padLeds(uint8_t* lv) {
    // 0x51d50: Beat Slice lights pads 0..count-1.
    const uint8_t ci = app_.currentInstrument();
    if (ci >= kSampleInstrumentCount) return;
    const Instrument& in = app_.instruments[ci];
    if (in.playMode != uint8_t(PlayMode::BeatSlice)) return;
    for (int p = 0; p < in.sliceCount && p < kPadCount; ++p) lv[p] = app_.cfg.padDim();
}

#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
bool PlaybackScreen::encoderOnList() const {
    return sel == 6;   // play-mode list
}
#endif

void PlaybackScreen::onEncoder(int d) {
    change(d);
    refreshBar();
}

uint32_t PlaybackScreen::markerColour(int i) const {
    // spec 13 §H.2: green = the played position lies on a zero crossing; the
    // selected (or held) point is red "but will change to green" on one
    // (manual p.123 / 125; MANUAL_AUDIT_2026-10-23 #13).
    const Theme& th = ui_.th();
    if ((zeroMask() >> i) & 1) return th.good;
    if (sel == i + 1 || ((heldPoints >> i) & 1)) return th.accent;
    return th.slot(slot::kWave);
}

void PlaybackScreen::draw(Gfx& g) {
    App& a = app_;
    const Theme& th = ui_.th();
    const uint8_t ci = a.currentInstrument();
    char sub[48];
    if (ci < kSampleInstrumentCount) std::snprintf(sub, sizeof(sub), "%d. %s", ci + 1, a.instruments[ci].name);
    else if (isSynthInstrument(ci)) synthui::slotLabel(a, synthSlotOf(ci), sub, sizeof(sub));
    else std::snprintf(sub, sizeof(sub), "MIDI instrument");
    ui_.drawTitle(g, "Sample Playback", sub);
    if (ui_.bar.label[6].hidden && !ui_.askOpen()) barSelector(g, th, 6, ui_.bar.label[6].top, 67);
    if (ci >= kSampleInstrumentCount) return;
    const Instrument& in = a.instruments[ci];
    float v0, v1;
    view.window(&v0, &v1);
    const int mode = in.playMode & 7;
    if (mode == int(PlayMode::Wavetable) && in.wavetableWindowSize && in.wavetableWindowCount) {
        // One window as the graph, its position on a bar below (manual
        // p.130-131; spec 05 appB §1.4d; MANUAL_AUDIT_2026-10-23 #12).
        const uint32_t size = in.wavetableWindowSize, n = in.wavetableWindowCount;
        uint32_t w = wavetableShownWindow();
        if (w >= n) w = n - 1;
        const uint32_t len = a.bank.frames(ci);
        const uint32_t off = w * size;
        const uint32_t cnt = off + size <= len ? size : (len > off ? len - off : 0);
        const int graphH = kWaveH - 30;
        waveCentre(g, th, kWaveX, kWaveW, kWaveY + graphH / 2);
        if (cnt) drawWaveData(g, th, a.bank.left(ci) + off, cnt, 0.0f, 1.0f, kWaveX, kWaveY, kWaveW, graphH, th.slot(slot::kWave));
        const int by = kWaveY + kWaveH - 14;
        g.fill(kWaveX + 10, by, kWaveW - 20, 1, th.slot(slot::kWave));
        const int mx = wavetablePositionX();
        g.fill(mx - 4, by - 3, 8, 7, sel == 1 ? th.accent : th.slot(slot::kWave));
        wtShown_ = w;
        headX_ = -1;
        drawModeList(g, th, mode);
        return;
    }
    auto clampX = [](int x) { return x < kWaveX ? kWaveX : (x > kWaveX + kWaveW - 1 ? kWaveX + kWaveW - 1 : x); };
    // Strip under the played part: the loop in the loop modes, start..end
    // in 1-Shot, the whole sample otherwise [S snap 008, 025].
    int ra = markerX(0, v0, v1), rb = markerX(65535, v0, v1);
    if (isLoop(mode)) {
        ra = markerX(in.loopPoint1, v0, v1);
        rb = markerX(in.loopPoint2, v0, v1);
    } else if (mode == 0) {
        ra = markerX(in.startPoint, v0, v1);
        rb = markerX(in.endPoint, v0, v1);
    }
    // Stereo: L in the upper lane, R in the lower one (the Sample Editor's
    // 109 + 1 + 109 split; Tracker+ draws stereo samples the same way).
    const bool st = a.bank.stereo(ci);
    if (rb >= kWaveX && ra < kWaveX + kWaveW) waveRegion(g, th, clampX(ra), clampX(rb), kWaveY, kWaveH);
    if (st) {
        constexpr int kLaneH = (kWaveH - 1) / 2;
        waveCentre(g, th, kWaveX, kWaveW, kWaveY + kLaneH / 2);
        waveCentre(g, th, kWaveX, kWaveW, kWaveY + kLaneH + 1 + kLaneH / 2);
        drawWaveData(g, th, a.bank.left(ci), a.bank.frames(ci), v0, v1, kWaveX, kWaveY, kWaveW, kLaneH,
                     th.slot(slot::kWave));
        drawWaveData(g, th, a.bank.right(ci), a.bank.frames(ci), v0, v1, kWaveX, kWaveY + kLaneH + 1, kWaveW,
                     kLaneH, th.slot(slot::kWave));
    } else {
        waveCentre(g, th, kWaveX, kWaveW, kWaveY + kWaveH / 2);
        drawWaveData(g, th, a.bank.left(ci), a.bank.frames(ci), v0, v1, kWaveX, kWaveY, kWaveW, kWaveH,
                     th.slot(slot::kWave));
    }
    auto inView = [&](int x) { return x >= kWaveX && x < kWaveX + kWaveW; };
    if (mode <= 3) {
        auto col = [&](int i) { return markerColour(i); };
        auto mark = [&](uint16_t m, int i, int dir, bool loop) {
            const int x = markerX(m, v0, v1);
            if (inView(x)) waveMarker(g, th, x, col(i), dir, !loop, loop, kMarkTop, loop ? kLoopBottom : kMarkBottom);
        };
        if (mode) {
            mark(in.loopPoint1, 1, 1, true);
            mark(in.loopPoint2, 2, -1, true);
        }
        mark(in.startPoint, 0, 1, false);
        mark(in.endPoint, 3, -1, false);
    } else if (isSlice(mode)) {
        // SPEC GAP: no stock capture of the slice marks (VISUAL_DIFF §6).
        for (int k = 0; k < in.sliceCount; ++k) {
            const int x = markerX(in.slices[k], v0, v1);
            if (!inView(x)) continue;
            if (k == in.selectedSlice) waveMarker(g, th, x, th.accent, 1, true, false, kMarkTop, kMarkBottom);
            else g.vline(x, kWaveY, kWaveH, th.yellow);
        }
    } else if (mode == 7) {
        // The grain as a blue span (its length) around the blue position
        // line; the play head is red (manual p.135-136, MANUAL_AUDIT #28).
        // The position follows the preview voice (LFO / envelope).
        const uint16_t pos = granularShownPosition();
        const uint32_t len = a.bank.frames(ci);
        const uint32_t half = len ? uint32_t(uint64_t(in.grainLength) * 65535u / len / 2) : 0;
        const int xa = markerX(uint16_t(pos > half ? pos - half : 0), v0, v1);
        const int xb = markerX(uint16_t(pos + half < 65535u ? pos + half : 65535u), v0, v1);
        if (xb >= kWaveX && xa < kWaveX + kWaveW) {
            g.alpha(70);
            g.fill(clampX(xa), kWaveY, clampX(xb) - clampX(xa) + 1, kWaveH, kGrainColour);
            g.alpha(255);
        }
        const int x = markerX(pos, v0, v1);
        if (inView(x)) waveMarker(g, th, x, sel == 1 ? th.accent : kGrainColour, 1, true, false, kMarkTop, kMarkBottom);
        grainShown_ = pos;
    }
    headX_ = playheadX();
    if (headX_ >= 0) g.vline(headX_, kWaveY, kWaveH, mode == 7 ? th.alert : th.good);
    drawModeList(g, th, mode);
}

void PlaybackScreen::drawModeList(Gfx& g, const Theme& th, int mode) const {
    // Appendix B §1.4: the 8-item play-mode list in the column panel above
    // the Play Mode label; current mode in a white frame, the panel framed
    // red while Play Mode is the selected parameter [S snap 008, 025].
    wg::ListView v;
    v.x = kListX;
    v.w = kListW;
    v.count = 8;
    v.sel = mode;
    v.selStyle = wg::ListSel::kWhite;
    v.item = modeItem;
    wg::list(g, th, v);
    if (sel == 6) wg::panelFocus(g, th, kListX, kListW);
}

}  // namespace ot
