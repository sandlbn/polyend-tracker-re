// Performance (spec 06 §3, spec 12 §A-§E).
//
// Presets come from one of two sources (spec 12 §A): the global presets in the
// config image (EEPROM) or the per-project presets in project.mt, chosen by
// the Config option at screen start. The engine-side state is the current
// value per slot (cur); it is pushed to the audio engine offsets
// (audio::PerfOffsets) and the sequencer's performance hooks (step repeater,
// play mode, pattern length, MIDI velocity/transpose, spec 03 §2.6).
// Per-track patterns go through Sequencer::setTrackPattern (spec 12 §C).
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "app/perf_presets.h"
#include "ui/screens.h"

namespace ot {

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr uint32_t kCcPollMs = 50;      // spec 06 §3.4 update 0x436b4
constexpr uint32_t kLabelPollMs = 100;
// Text anchors of the FX panels (centre y; vertical label bottom) [S snap 021].
constexpr int kTitleValueY = 53;
constexpr int kRowY = 298;
constexpr int kLabelBottom = 266;
constexpr int kLabelDx = 2;
// Cell text: perf::valueText, with a zero shown as "---" [S snap 021].
void cellText(uint8_t ty, int v, char* out, int cap) {
    perf::valueText(ty, v, out, cap);
    if (std::strcmp(out, perf::kZeroText) == 0) std::snprintf(out, size_t(cap), "---");
}
// One cell centred on (cx, cy) in font 2; "---" is three 6 x 1 dashes at a
// 10-px pitch on row cy + 1 (the stock 0.5-px glyph strokes) [S snap 021].
void cell(Gfx& g, int cx, int cy, uint32_t c, const char* v) {
    if (std::strcmp(v, "---") != 0) {
        g.text(cx, cy, kFont2, c, v, IDisplay::kOptCenter);
        return;
    }
    g.alpha(222);
    g.rectsBegin(c);
    for (int k = 0; k < 3; ++k) g.rectsAdd(cx - 14 + 10 * k, cy + 1, 6, 1);
    g.rectsEnd();
    g.alpha(255);
}
}  // namespace

// ---------------------------------------------------------------------------
// start / stop (spec 06 §3.1, spec 12 §A.5, §C.4)

void PerformanceScreen::start(int opt) {
    (void)opt;
    App& a = app_;
    if (!a.perfCc) a.perfCc = this;   // MIDI CC 41-48 / 51-62 (spec 12 §B)
    // spec 16 §A.15: coming back while the FX were kept alive resumes
    // without resetting the rows.
    if (keepAlive_ != KeepAlive::None) {
        keepAlive_ = KeepAlive::None;
        lastPattern_ = a.patternNumber();
        labelsDirty_ = false;
        refreshBar();
        return;
    }
    // Rows of the previously selected source are cleared before the source
    // pointers switch (spec 12 §A.5 quirk: a newly selected source keeps its
    // stale rows; they are not applied). The engine starts on source 0.
    if (!src.valid()) src = perf::globalSource(a.cfg);
    for (int s = 0; s < perf::kSlots; ++s) src.setRow(s, 0);
    sourceIndex = a.cfg.perfPresets < 2 ? a.cfg.perfPresets : sourceIndex;   // spec 12 §A.3
    src = perf::select(sourceIndex, a.cfg, a.project);
    src.validate();
    for (int s = 0; s < perf::kSlots; ++s) {
        const int r = src.row(s);
        cur[s] = (r >= 1 && r <= 3) ? src.value(s, r) : 0;   // recomputed, not applied
        live[s] = 0;
        liveValid[s] = false;
        held_[s] = false;
        ccPending_[s] = false;
    }
    // Per-track patterns := the current pattern (spec 06 §3.1 step 1).
    const uint8_t curPat = a.patternNumber();
    for (int t = 0; t < 8; ++t) {
        perf::setTrackPattern(a.project, t, curPat);
        keyHeld_[t] = keyUsed_[t] = false;
        trackCcPending_[t] = false;
    }
    // Step 3: FX enabled on every track. 2.x tracks 9-16 take the FX too
    // but have no pattern override (the Tracker Mini stores 1-8 only).
    for (int t = 0; t < kTrackCount; ++t) {
        fxOn[t] = true;
        pending_[t] = 0;
    }
    lastPattern_ = curPat;
    edit = false;
    copySrc_ = -1;
    a.seq.setPerformanceMode(true);   // step 4
    labelsDirty_ = false;
    refreshBar();
}

void PerformanceScreen::stop() {
    App& a = app_;
    for (auto& h : held_) h = false;
    // spec 16 §A.15: switching to Instrument Params or Master keeps the FX
    // live indefinitely; to any other screen while playing they stay live
    // until the next pattern end (or Stop); stopped, they revert at once.
    // The switch happens on the screen key's press, so it is still down.
    if (ui_.isDown(btn::InstrParams) || ui_.isDown(btn::Master)) {
        keepAlive_ = KeepAlive::Always;
    } else if (a.seq.isPlaying()) {
        keepAlive_ = KeepAlive::UntilPatternEnd;
        keepEnds_ = a.patternEndCount();
    } else {
        revert();
    }
    leave();
}

void PerformanceScreen::perfBackground(uint32_t now) {
    (void)now;
    if (keepAlive_ != KeepAlive::UntilPatternEnd) return;
    App& a = app_;
    if (!a.seq.isPlaying() || a.patternEndCount() != keepEnds_) {
        keepAlive_ = KeepAlive::None;
        revert();
    }
}

// spec 16 §A.15: the stock screen-key handler runs on every screen key while
// the FX are live, not only when leaving the Performance screen. From
// Instrument Params / Master: Instrument Params or Master keep them;
// the Performance key comes back and resumes; any other screen key reverts
// at once when stopped, and while playing
// keeps them until the next pattern end (or Stop).
void PerformanceScreen::perfScreenKey(uint8_t id) {
    if (keepAlive_ != KeepAlive::Always || id == btn::InstrParams || id == btn::Master || id == btn::Performance) return;
    App& a = app_;
    if (a.seq.isPlaying()) {
        keepAlive_ = KeepAlive::UntilPatternEnd;
        keepEnds_ = a.patternEndCount();
    } else {
        keepAlive_ = KeepAlive::None;
        revert();
    }
}

// Exports and renders run without Performance FX: stock reverts them on any
// other screen while stopped (spec 16 §A.15), and a render always stops the
// transport first.
void PerformanceScreen::perfRevertKept() {
    if (keepAlive_ == KeepAlive::None) return;
    keepAlive_ = KeepAlive::None;
    revert();
}

void PerformanceScreen::revert() {
    // spec 12 §C.4: full revert, perf mode off, overrides dropped (the
    // sequencer plays the current pattern again).
    applyAll(true);
    app_.seq.setPerformanceMode(false);
}

void PerformanceScreen::leave() {
    App& a = app_;
    // "Leave" callback (spec 12 §A.5): global -> forced config write,
    // per project -> forced Workspace autosave.
    if (sourceIndex == 0) a.requestConfigSave(true);
    else {
        a.markProjectDirty();
        a.autosave();
    }
}

void PerformanceScreen::edited() {
    // "Edited" callback (spec 12 §A.5).
    if (sourceIndex == 0) app_.markConfigDirty();
    else app_.markProjectDirty();
}

// ---------------------------------------------------------------------------
// FX apply / revert (spec 12 §E)

// Rebuild every track's performance state from the slots' current values and
// push it to the engine and the sequencer. 2.x: all 16 tracks; the voice FX
// (audio::PerfOffsets) exist on the sample tracks 1-8 only, tracks 9-16 take
// the sequencer side (Volume as velocity offset, Tune as transpose, step
// repeater, play mode; 2x-01 §5). A slot with cur = 0 is reverted;
// disabled tracks are reverted; a global FX (Pattern length) is applied once
// (spec 12 §E.1). FX types are exclusive, so at most one slot per type is
// normally active; if two are, the higher slot wins.
void PerformanceScreen::applyAll(bool revertEverything) {
    App& a = app_;
    Sequencer& q = a.seq;
    int globalLen = -1;
    for (int t = 0; t < kTrackCount; ++t) {
        audio::PerfOffsets p;
        int8_t repeat = -1, velOff = 0, transpose = 0;
        bool transposeOn = false;
        uint8_t playMode = 0;
        const bool on = fxOn[t] && !revertEverything;
        for (int s = 0; s < perf::kSlots && !revertEverything; ++s) {
            const uint8_t ty = src.valid() ? src.type(s) : 0;
            const int v = cur[s];
            if (ty == 0 || v == 0) continue;
            if (ty == perf::kPatternLength) {
                globalLen = perf::lengthOverride(v);
                continue;
            }
            if (!on) continue;
            switch (ty) {
                case perf::kVolume: p.volume = int16_t(v); velOff = int8_t(v); break;
                case perf::kPanning: p.pan = int16_t(v); break;   // spec 12 §E.2: pan + v, 0..100
                case perf::kTune:
                    p.tune = int16_t(v);
                    if (v >= -48 && v <= 48) {   // MIDI transpose (spec 12 §E.2 id 3)
                        transposeOn = true;
                        transpose = int8_t(v);
                    }
                    break;
                case perf::kLowPass: case perf::kHighPass: case perf::kBandPass:
                    p.filterType = int8_t(ty - perf::kLowPass);
                    p.cutoff = float(v) / 100.0f;
                    break;
                case perf::kDelaySend: p.dsend = int16_t(v); break;
                case perf::kReverbSend: p.rsend = int16_t(v); break;
                case perf::kStart: p.start = perf::startOffset(v); break;
                case perf::kEnd: p.end = perf::endOffset(v); break;
                case perf::kDirection: p.reverse = v != 0; break;
                case perf::kVolLfo: p.lfoSpeed[kAutoVolume] = int8_t(v); break;
                case perf::kPanLfo: p.lfoSpeed[kAutoPanning] = int8_t(v); break;
                case perf::kFineLfo: p.lfoSpeed[kAutoFinetune] = int8_t(v); break;
                case perf::kFilterLfo: p.lfoSpeed[kAutoCutoff] = int8_t(v); break;
                case perf::kPosLfo: p.lfoSpeed[kAutoWavetablePos] = p.lfoSpeed[kAutoGranularPos] = int8_t(v); break;
                case perf::kRepeater: repeat = int8_t(v - 1); break;
                case perf::kPlayMode: playMode = uint8_t(v); break;
                case perf::kBitDepth: p.bits = int16_t(v); break;
                case perf::kOverdrive: p.od = int16_t(v); break;
                default: break;
            }
        }
        if (t < kMidiTrackStart) a.engine.setPerf(t, p);
        q.setPerfRepeat(t, repeat);
        q.setPerfPlayMode(t, playMode);
        q.setPerfVelocityOffset(t, velOff);
        q.setPerfTranspose(t, transposeOn && transpose != 0, transpose);
    }
    q.setPerfGlobalLength(globalLen);
}

// Other slots with the same FX type, or an excluded filter type, go to row 0
// with value 0 (spec 12 §B.2 step 3, spec 06 §3.4 pad step 2).
void PerformanceScreen::resetOthers(int c) {
    const uint8_t ty = src.type(c);
    if (ty == 0) return;
    const perf::FxDesc& d = perf::fx(ty);
    for (int s = 0; s < perf::kSlots; ++s) {
        if (s == c) continue;
        const uint8_t o = src.type(s);
        if (o != ty && (o == 0 || (o != d.exclA && o != d.exclB))) continue;
        src.setRow(s, 0);
        cur[s] = 0;
        liveValid[s] = false;
    }
}

// ---------------------------------------------------------------------------
// Inputs (spec 06 §3.4)

void PerformanceScreen::onPad(uint8_t pad, uint8_t state) {
    const int c = pad % 12, r = pad / 12;
    if (c >= perf::kSlots || r >= perf::kRows || !src.valid()) return;
    if (state == kPress) {
        liveValid[c] = false;
        const uint8_t row = uint8_t(src.type(c) ? r : 0);
        src.setRow(c, row);
        held_[c] = true;
        resetOthers(c);
        cur[c] = row ? src.value(c, row) : 0;
        applyAll(false);
    } else if (state == kRelease) {
        // Re-apply the stored value: an encoder tweak outside edit mode is
        // dropped (spec 12 §E.1). The row stays latched.
        const int row = src.row(c);
        cur[c] = (row >= 1 && row <= 3) ? src.value(c, row) : (row == perf::kLiveRow && liveValid[c] ? live[c] : 0);
        held_[c] = false;
        applyAll(false);
    }
    ui_.dirty();
}

void PerformanceScreen::padLeds(uint8_t* lv) {
    // One lit pad per column at the selected row (spec 06 §3.4), DIM level.
    if (!src.valid()) return;
    for (int c = 0; c < perf::kSlots; ++c) {
        const int r = src.row(c);
        if (r < perf::kRows) lv[r * 12 + c] = app_.cfg.padDim();
    }
}

uint8_t PerformanceScreen::shownPattern(int t) const {
    if (t < 0 || t >= kTrackCount) return 1;
    return pending_[t] ? pending_[t] : trackPatternOf(t);
}

// Pattern the track plays: stock 0x142 for tracks 1-8; tracks 9-16 always
// play the current pattern (no override on the Tracker Mini).
uint8_t PerformanceScreen::trackPatternOf(int t) const {
    App& a = app_;
    if (t < kMidiTrackStart) return perf::trackPattern(a.project, t);
    return a.patternNumber();
}

void PerformanceScreen::storeTrackPattern(int t, uint8_t num) {
    App& a = app_;
    if (t >= kMidiTrackStart) return;
    perf::setTrackPattern(a.project, t, num);
    a.markProjectDirty();
}

void PerformanceScreen::trackPatternNow(int t, int num) {
    // Track key + encoder / Up / Down: immediate switch (spec 12 §C.2).
    App& a = app_;
    if (t >= kMidiTrackStart) return;
    num = clampi(num, 1, 255);
    storeTrackPattern(t, uint8_t(num));
    a.seq.setTrackPattern(t, uint8_t(num), true);
    pending_[t] = 0;
}

void PerformanceScreen::trackPatternQueued(int t, int num) {
    // Track key release after Left/Right, CC 41-48: switch at that track's
    // loop end (spec 12 §C.2).
    App& a = app_;
    if (t >= kMidiTrackStart) return;
    num = clampi(num, 1, 255);
    storeTrackPattern(t, uint8_t(num));
    a.seq.setTrackPattern(t, uint8_t(num), false);
}

bool PerformanceScreen::anyTrackKey() const {
    for (bool k : keyHeld_)
        if (k) return true;
    return false;
}

void PerformanceScreen::onEncoder(int d) {
    if (anyTrackKey()) {
        for (int i = 0; i < 8; ++i)
            if (keyHeld_[i]) {
                const int t = bank * 8 + i;
                keyUsed_[i] = true;
                trackPatternNow(t, trackPatternOf(t) + d);
            }
        refreshBar();
        return;
    }
    if (!src.valid()) return;
    bool any = false;
    for (int c = 0; c < perf::kSlots; ++c) {
        const uint8_t ty = src.type(c);
        if (!held_[c] || ty == 0) continue;
        const perf::FxDesc& f = perf::fx(ty);
        cur[c] = int16_t(clampi(cur[c] + d, f.min, f.max));
        const int row = src.row(c);
        // Stored only in edit mode on rows 1..3; never into the live row
        // (spec 12 §B.2: stock writes out of the slot there).
        if (edit && row >= 1 && row <= 3) {
            src.setValue(c, row, cur[c]);
            edited();
        } else if (row == perf::kLiveRow) {
            live[c] = cur[c];
        }
        any = true;
    }
    if (any) {
        applyAll(false);
        ui_.dirty();
    }
}

void PerformanceScreen::changeType(int d) {
    const int c = column;
    // The old type is reverted first (spec 12 §A.5 [FW:0x445fc]).
    src.setType(c, uint8_t((src.type(c) + perf::kFxCount + d) % perf::kFxCount));
    for (int r = 0; r < perf::kRows; ++r) src.setValue(c, r, 0);
    src.setRow(c, 0);
    cur[c] = 0;
    liveValid[c] = false;
    edited();
    applyAll(false);
}

void PerformanceScreen::swapColumn(int nc) {
    // Shift + Left/Right in edit mode: type + 4 values (spec 06 §3.4).
    const int c = column;
    const uint8_t t = src.type(nc);
    src.setType(nc, src.type(c));
    src.setType(c, t);
    for (int r = 0; r < perf::kRows; ++r) {
        const int16_t v = src.value(nc, r);
        src.setValue(nc, r, src.value(c, r));
        src.setValue(c, r, v);
    }
    const int pair[2] = {c, nc};
    for (int s : pair) {
        const int r = src.row(s);
        cur[s] = (r >= 1 && r <= 3) ? src.value(s, r) : 0;
        liveValid[s] = false;
    }
    edited();
    applyAll(false);
}

void PerformanceScreen::onButton(uint8_t id, uint8_t state) {
    App& a = app_;
    if (id == btn::Performance) {
        // 2.x: the Performance key toggles the track bank (2x-01 §5); held
        // track keys are dropped (their queued patterns with them).
        if (state != kPress) return;
        bank ^= 1;
        for (int i = 0; i < 8; ++i) keyHeld_[i] = keyUsed_[i] = false;
        for (auto& p : pending_) p = 0;
        refreshBar();
        ui_.dirty();
        return;
    }
    if (btn::isFKey(id)) {
        const int k = id, t = bank * 8 + k;
        if (state == kPress) {
            if (ui_.shift()) trk::setMuted(a, t, !trk::muted(a, t));
            else {
                keyHeld_[k] = true;
                keyUsed_[k] = false;
            }
        } else if (state == kRelease && keyHeld_[k]) {
            keyHeld_[k] = false;
            if (pending_[t]) {
                trackPatternQueued(t, pending_[t]);
                pending_[t] = 0;
            } else if (!keyUsed_[k]) {
                fxOn[t] = !fxOn[t];
                applyAll(false);   // enable = apply the active slots, disable = revert
            }
        }
        refreshBar();
        return;
    }
    if (state != kPress) {
        if (id == btn::Shift) refreshBar();
        return;
    }
    if (!src.valid()) return;
    switch (id) {
        case btn::Rec: edit = !edit; break;
        case btn::Left:
        case btn::Right: {
            const int d = id == btn::Right ? 1 : -1;
            if (edit) {
                const int nc = clampi(column + d, 0, perf::kSlots - 1);
                if (ui_.shift() && nc != column) swapColumn(nc);
                column = nc;
            } else {
                for (int i = 0; i < 8; ++i)
                    if (keyHeld_[i]) {
                        const int t = bank * 8 + i;
                        keyUsed_[i] = true;
                        if (t < kMidiTrackStart) pending_[t] = uint8_t(clampi(shownPattern(t) + d, 1, 255));
                    }
            }
            break;
        }
        case btn::Up:
        case btn::Down: {
            const int d = id == btn::Up ? 1 : -1;
            if (anyTrackKey()) {
                for (int i = 0; i < 8; ++i)
                    if (keyHeld_[i]) {
                        const int t = bank * 8 + i;
                        keyUsed_[i] = true;
                        trackPatternNow(t, trackPatternOf(t) + d);
                    }
            } else if (edit) {
                changeType(d);
            }
            break;
        }
        case btn::Copy:
            if (!edit) break;
            if (!ui_.shift()) copySrc_ = column;
            else if (copySrc_ >= 0 && copySrc_ != column) {
                src.setType(column, src.type(copySrc_));
                for (int r = 0; r < perf::kRows; ++r) src.setValue(column, r, src.value(copySrc_, r));
                src.setRow(column, 0);
                cur[column] = 0;
                liveValid[column] = false;
                edited();
                applyAll(false);
            }
            break;
        case btn::Play: play(); break;
        default: break;
    }
    refreshBar();
}

void PerformanceScreen::play() {
    // spec 06 §3.4 Play, spec 12 §C.3.
    App& a = app_;
    const uint8_t curPat = a.patternNumber();
    if (a.seq.isPlaying()) {
        if (!ui_.shift()) {
            a.togglePlay(false);   // stop everything; perf mode and FX stay (§C.4)
        } else {
            // SPEC GAP: "continue in song/pattern mode at the next sync
            // point" -- every track is queued back to the current pattern, so
            // each switches at its own loop end.
            for (int t = 0; t < kTrackCount; ++t) trackPatternQueued(t, curPat);
        }
        return;
    }
    if (ui_.shift()) {
        for (int t = 0; t < kTrackCount; ++t) {
            if (t < kMidiTrackStart) perf::setTrackPattern(a.project, t, curPat);
            pending_[t] = 0;
        }
        a.seq.clearTrackPatterns();
        a.seq.playSong(a.project.playlistPos);
    } else {
        a.seq.userPlay(0);   // performance mode stays set: all tracks from step 0
    }
}

// ---------------------------------------------------------------------------
// MIDI CC (spec 12 §B.2, §B.3): latched here, handled in update().

void PerformanceScreen::perfTrackPatternCc(uint8_t track, uint8_t value) {
    if (track >= 8) return;
    trackCcVal_[track] = value;
    trackCcPending_[track] = true;
}

void PerformanceScreen::perfSlotCc(uint8_t slot, uint8_t value) {
    if (slot >= perf::kSlots) return;
    ccVal_[slot] = value;
    ccPending_[slot] = true;
}

void PerformanceScreen::update(uint32_t now) {
    App& a = app_;
    if (now - lastPoll_ >= kCcPollMs) {
        lastPoll_ = now;
        bool apply = false;
        for (int t = 0; t < 8; ++t) {
            if (!trackCcPending_[t]) continue;
            trackCcPending_[t] = false;
            keyHeld_[t] = keyUsed_[t] = false;
            pending_[t] = 0;
            trackPatternQueued(t, trackCcVal_[t] + 1);   // 1..128
            labelsDirty_ = true;
        }
        for (int s = 0; s < perf::kSlots && src.valid(); ++s) {
            if (!ccPending_[s]) continue;
            ccPending_[s] = false;
            const uint8_t ty = src.type(s);
            src.setRow(s, uint8_t(ty ? perf::kLiveRow : 0));
            live[s] = int16_t(perf::ccToValue(ty, ccVal_[s]));
            liveValid[s] = true;
            resetOthers(s);
            cur[s] = ty ? live[s] : 0;
            apply = true;
        }
        // OpenTracker applies a Pattern length slot too (stock only shows it,
        // spec 12 §B.2 quirk; README).
        if (apply) {
            applyAll(false);
            ui_.dirty();
        }
        // Song advance / queued whole-pattern change: the overrides are gone,
        // the labels show the new current pattern (spec 12 §C.3).
        const uint8_t curPat = a.patternNumber();
        if (curPat != lastPattern_) {
            lastPattern_ = curPat;
            for (int t = 0; t < 8; ++t) perf::setTrackPattern(a.project, t, curPat);
            labelsDirty_ = true;
        }
    }
    // A queued track pattern blinks on its label (manual p.278, MANUAL_AUDIT #39).
    bool queued = false;
    for (int t = 0; t < 8; ++t) queued = queued || pending_[t] || a.seq.queuedTrackPattern(t) != 0;
    const bool on = !queued || (now / 200u) % 2u == 0;
    if (on != blinkOn_) {
        blinkOn_ = on;
        ui_.dirty();
    }
    if (labelsDirty_ && now - lastLabels_ >= kLabelPollMs) {
        lastLabels_ = now;
        labelsDirty_ = false;
        refreshBar();
        ui_.dirty();
    }
}

// ---------------------------------------------------------------------------
// Layout (spec 06 §3.3)

void PerformanceScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    App& a = app_;
    b.clear();
    for (int i = 0; i < 8; ++i) {
        const int t = bank * 8 + i;
        const bool m = trk::muted(a, t);
        char nm[24], v[24];
        trk::name(a, t, nm, sizeof(nm));
        if (ui_.shift()) std::snprintf(v, sizeof(v), "%s", m ? "Unmute" : "Mute");
        else std::snprintf(v, sizeof(v), a.cfg.numbering == 2 ? "Patt: %02X" : "Patt: %d", shownPattern(t));
        // 1.9.2 manual: a highlighted label = FX applied to the track; the
        // text colour shows the mute state (spec 06 §3.3).
        b.set(i, nm, v, !m);
        b.highlight(i, fxOn[t]);
        // Drawn by draw(): the stock bar shows no focus frames here
        // [S snap 021], so "FX on" is the plain stock look and "FX off"
        // greys the title only [I].
        b.label[i].hidden = true;
    }
}

void PerformanceScreen::draw(Gfx& g) {
    const Theme& th = ui_.th();
    // 2.x bank B: right-hand text "Tracks 9-16" (M1 plan spec gap 5).
    const char* right = bank ? (edit ? "EDIT  Tracks 9-16" : "Tracks 9-16") : (edit ? "EDIT" : nullptr);
    ui_.drawTitle(g, "Performance FX", right);
    if (!src.valid()) return;
    // spec 20 §4.10 / VISUAL_DIFF §8.10: 12 FX panels (class 0x0b) 63 px
    // wide at x 11 + 65 i: a title band y 34..69 with the current (or live
    // MIDI-CC, spec 16 §A.14) value, the FX name as vertical font-2 text
    // reading upwards (bottom y 265), and a body y 279..413 + fade with the
    // four stored rows at a 30-px pitch, the active row green [S snap 021].
    const int set = slot::kPerfPanels;
    // Edit mode: the active rows are red (manual p.274, spec 06 §3.3 "the cell
    // colour differs in edit mode"; MANUAL_AUDIT_2026-10-23 #41).
    const uint32_t bg = th.slot(set + 1), textC = th.slot(set), hiC = edit ? th.alert : th.slot(set + 2);
    // Drawn in layers over all 12 columns instead of column by column: the
    // RECTS batches keep the display list (RAM_DL, 8 KB) small enough for
    // the FT81x -- column by column this screen expanded past it on
    // hardware and every frame faulted (the screen froze). The columns do
    // not overlap and each column keeps its own drawing order, so the
    // pixels are the same.
    auto colX = [](int c) { return 11 + 65 * c; };
    // 1. Panel bodies (opaque): title band y 34..70, body y 279..424.
    g.rectsBegin(bg);
    for (int c = 0; c < perf::kSlots; ++c) {
        g.rectsAdd(colX(c), 34, 63, 37);
        g.rectsAdd(colX(c), 279, 63, 146);
    }
    g.rectsEnd();
    // 2. Soft 1-px side edges and the blended rows 33 / 70 / 278 (row 70
    //    lies on the title band: after 1). The column after the one being
    //    edited leaves its translucent left pixels to step 5 (drawn once,
    //    over the focus frame, as column by column).
    const int after = edit && column >= 0 && column + 1 < perf::kSlots ? column + 1 : -1;
    g.alpha(wg::kEdgeA);
    g.rectsBegin(bg);
    for (int c = 0; c < perf::kSlots; ++c) {
        const int x = colX(c), skip = c == after ? 1 : 0;
        if (!skip) {
            g.rectsAdd(x - 1, 34, 1, 37);
            g.rectsAdd(x - 1, 279, 1, 146);
        }
        g.rectsAdd(x + 63, 34, 1, 37);
        g.rectsAdd(x + 63, 279, 1, 146);
        g.rectsAdd(x + skip, 33, 63 - skip, 1);
        g.rectsAdd(x, 70, 63, 1);
        g.rectsAdd(x + skip, 278, 63 - skip, 1);
    }
    g.rectsEnd();
    g.alpha(255);
    // 3. Body fades (one clipped gradient each).
    for (int c = 0; c < perf::kSlots; ++c) wg::bottomFade(g, th, colX(c), 279, 62, 145, bg);
    // 4. Values, FX names and stored rows; "---" cells are collected and
    //    drawn as one batch per colour (three 6 x 1 dashes each).
    struct Dash {
        int16_t cx, cy;
    };
    Dash dashes[2][perf::kSlots * (perf::kRows + 1)];
    int nDash[2] = {0, 0};
    auto put = [&](int cx, int cy, bool hi, const char* v) {
        if (std::strcmp(v, "---") == 0) {
            dashes[hi][nDash[hi]++] = Dash{int16_t(cx), int16_t(cy)};
            return;
        }
        cell(g, cx, cy, hi ? hiC : textC, v);
    };
    for (int c = 0; c < perf::kSlots; ++c) {
        const int x = colX(c), cx = x + 32;
        const uint8_t ty = src.type(c);
        char v[12] = "";
        if (ty != 0) cellText(ty, liveValid[c] ? live[c] : cur[c], v, sizeof(v));
        put(cx, kTitleValueY, false, v[0] ? v : "");
        if (ty != 0)
            g.textRotated(cx + kLabelDx, kLabelBottom - textWidth(kFont2, perf::fx(ty).name), kFont2,
                          th.slot(slot::kPerfVertical), perf::fx(ty).name, 1, IDisplay::kOptCenterX);
        for (int r = 0; r < perf::kRows; ++r) {
            v[0] = 0;
            if (ty != 0) cellText(ty, src.value(c, r), v, sizeof(v));
            put(cx, kRowY + 30 * r, src.row(c) == r, v);
        }
    }
    g.alpha(222);
    for (int hi = 0; hi < 2; ++hi) {
        if (!nDash[hi]) continue;
        g.rectsBegin(hi ? hiC : textC);
        for (int i = 0; i < nDash[hi]; ++i)
            for (int k = 0; k < 3; ++k) g.rectsAdd(dashes[hi][i].cx - 14 + 10 * k, dashes[hi][i].cy + 1, 6, 1);
        g.rectsEnd();
    }
    g.alpha(255);
    // 5. Edit mode: the column being edited carries the red focus frame [I].
    //    Column by column, the next column was drawn over the frame's right
    //    side (its edge at x + 64 and first pixel column x + 65): redraw that
    //    strip in the next column's order (edge, body, blended rows, fade).
    if (edit && column >= 0 && column < perf::kSlots) {
        const int x = colX(column);
        wg::focusFrame(g, th, x - 1, 31, x + 63, 422);
        if (column + 1 < perf::kSlots) {
            const int nx = colX(column + 1);   // x + 65
            g.alpha(wg::kEdgeA);
            g.rectsBegin(bg);
            g.rectsAdd(nx - 1, 34, 1, 37);
            g.rectsAdd(nx - 1, 279, 1, 146);
            g.rectsEnd();
            g.alpha(255);
            g.rectsBegin(bg);
            g.rectsAdd(nx, 34, 1, 37);
            g.rectsAdd(nx, 279, 1, 146);
            g.rectsEnd();
            g.alpha(wg::kEdgeA);
            g.rectsBegin(bg);
            g.rectsAdd(nx, 33, 1, 1);
            g.rectsAdd(nx, 70, 1, 1);
            g.rectsAdd(nx, 278, 1, 1);
            g.rectsEnd();
            g.alpha(255);
            wg::bottomFade(g, th, nx, 279, 62, 145, bg);   // opaque, no text there
        }
    }
    // Labels of tracks with performance FX turned off are lit red (stock 1.9.2,
    // owner photo 2026-10-23: all tracks start enabled and unlit, spec 06 §3.1
    // step 3; a track key release turns a track off and its label red). One
    // batched RECTS list (per-label fills cost ~10 display-list words each).
    {
        bool any = false;
        for (int t = 0; t < 8; ++t) {
            if (ui_.bar.label[t].highlight) continue;   // highlight = FX on
            if (!any) g.rectsBegin(th.slot(slot::kFocusFrame));
            any = true;
            g.rectsAdd(100 * t + 3, 424, 95, 56);
        }
        if (any) g.rectsEnd();
    }
    for (int t = 0; t < 8; ++t) {
        const BarLabel& l = ui_.bar.label[t];
        const int cx = wg::labelCentre(t);
        uint32_t tc = th.slot(l.enabled && l.highlight ? slot::kBottomLabel : slot::kBottomLabelOff);
        uint32_t vc = th.slot((l.enabled ? slot::kBottomLabel : slot::kBottomLabelOff) + 3);
        if (!l.highlight) {
            // On the red fill: white text while audible, black when muted
            // (manual p.272 text colours).
            tc = vc = l.enabled ? 0xFFFFFFu : 0x000000u;
        }
        g.text(cx, wg::kLabelY1, kFont3, tc, l.top, IDisplay::kOptCenter);
        const bool queued = pending_[t] || app_.seq.queuedTrackPattern(t) != 0;
        if (!queued || blinkOn_ || ui_.shift()) g.text(cx, wg::kLabelY2, kFont2, vc, l.bottom, IDisplay::kOptCenter);
    }
}

}  // namespace ot
