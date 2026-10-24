// Sample Recorder (spec 05 §2.11 and appendix C §2; spec 02 §8.3).
//
// Sources: line in, radio (needs an IRadio driver), mic low / high gain
// (need the HalRefs::selectInput routing hook); no USB source (spec 04 §5.7).
// Audio comes from the App's input tap (app/capture.*) into the free end of
// the sample bank; after Stop the recording is auto-trimmed, can be cropped
// (5 undo levels, kept as views into the buffer, so no copies) and is saved
// as Recordings[/ N]/recordingNNN.wav, optionally loaded into the first free
// instrument slot ("Save & Load").
//
// 2.x stereo sampling (2x-04 §5, revived 2026-10-19 as an OpenTracker extra;
// the Tracker Mini records mono): the Line In source with Config "Line in
// channel" = Stereo records two lanes (Capture stereo mode) and saves a
// 2-channel WAV; the radio (PWR_MONO) and the mics stay mono. Pads and MIDI
// notes play the selection through the preview player's pitch step, so a
// transposed stereo take keeps both lanes.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "ui/screens.h"

namespace ot {

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
const char* const kSources[4] = {"Line in", "Radio", "Mic LG", "Mic HG"};
// spec 05 appendix C §2.8 region table: band minimum and step (max 108 MHz).
const float kRadioMin[4] = {87.5f, 87.5f, 87.5f, 76.0f};
const float kRadioStep[4] = {0.1f, 0.2f, 0.2f, 0.1f};
constexpr float kRadioMax = 108.0f;
const char* const kMonitor[2] = {"On", "Off"};
// Source 0 = Line in (kSources); Config::lineInChannel 0 Stereo, 1 Mono
// Left, 2 Mono Right (screen_config.cpp kLineIn). No named constants exist
// for either, so the values are spelled here once.
constexpr uint8_t kSourceLineIn = 0;
constexpr uint8_t kLineInStereo = 0;
const char* const kLineInNames[3] = {"Line in ST", "Line in L", "Line in R"};

// Setup layout (stock 005, VISUAL_DIFF §3.4): seven column panels at
// x = 2 + 100 k mirroring the bar (source list, Radio Freq bar, a blank
// double panel, Level meter, Gain bar, Monitor list, Record); the selected
// column in the red frame of the column at 1 + 100 k.
constexpr int kColX0 = 2;
constexpr float kMinBar = 1.0f / float(wg::kPanelH - 25);
// Waveform (recording / edit): x 100..699, rows 116..334, markers 108..334.
constexpr int kWaveX = 100, kWaveW = 600, kWaveY = 116, kWaveH = 219;
// Stereo take: two lanes, L on top, R below (like the Tracker+ recorder),
// split by a 5-row gap, unlabelled like Tracker+; the markers still span
// the whole area [I].
constexpr int kLaneH = (kWaveH - 5) / 2, kLaneY2 = kWaveY + kLaneH + 5;

void listItem(void* ctx, int i, char* out, int cap) {
    std::snprintf(out, size_t(cap), "%s", static_cast<const char* const*>(ctx)[i]);
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

// Waveform chrome and bar glyphs (wave.cpp).
void waveRegion(Gfx& g, const Theme& th, int xa, int xb, int y0, int h);
void waveMarker(Gfx& g, const Theme& th, int x, uint32_t c, int dir, bool top, bool bottom, int y0, int y1);
void barSeekArrows(Gfx& g, const Theme& th, int i, int dir, bool enabled);

const int16_t* RecorderScreen::data() const {
    const int16_t* b = app_.capture.buffer();
    return b ? b + off : nullptr;
}

const int16_t* RecorderScreen::dataR() const {
    const int16_t* b = app_.capture.bufferR();
    return b ? b + off : nullptr;
}

bool RecorderScreen::stereoInput() const {
    return app_.cfg.recSource == kSourceLineIn && app_.cfg.lineInChannel == kLineInStereo;
}

const char* RecorderScreen::sourceName(int src) const {
    if (src == kSourceLineIn) return kLineInNames[app_.cfg.lineInChannel > 2 ? 0 : app_.cfg.lineInChannel];
    return kSources[clampi(src, 0, 3)];
}

void RecorderScreen::start(int opt) {
    App& a = app_;
    // Coming back from Config opened by F4 Metronome (appendix C §2.2 start
    // argument / §2.9 flag +0x71a): the setup state with Metronome selected.
    const bool fromConfig = configReturn_ && opt == kModConfig;
    configReturn_ = false;
    state = 0;
    len = off = 0;
    undoDepth = 0;
    if (fromConfig && !a.seq.isPlaying()) {
        enter();
        sel = 3;
        refreshBar();
        return;
    }
    if (a.seq.isPlaying()) {
        // appendix C §2.2: "this will stop the pattern" -- b6 leaves, b7 stops and enters.
        // [I] b5 "Resample": the Resample screen, the pattern keeps playing.
        ui_.ask("Sample Recorder", "This will stop the pattern. Continue?", "Resample", "Cancel", "Yes",
                [](void* c, int key) {
                    auto* s = static_cast<RecorderScreen*>(c);
                    if (key == btn::F6) {
                        s->ui_.switchTo(kModResample);
                        return;
                    }
                    if (key != btn::F8) {
                        s->ui_.switchTo(s->ui_.previous());
                        return;
                    }
                    if (s->app_.seq.isPlaying()) s->app_.togglePlay(false);
                    s->enter();
                },
                this);
        return;
    }
    enter();
}

void RecorderScreen::enter() {
    App& a = app_;
    a.previewAllOff();
    a.capture.stop();
    a.capture.setBuffer(a.bank.tail(), uint32_t(a.bank.freeSamples()));
    a.capture.setMetering(true);
    a.inputMonitorMuted = a.cfg.recMonitor == 0;   // appendix C §2.2 monitor
    meter = meterR = 0;
    // appendix C §2.2: the current instrument is forced into the MIDI range
    // 48..63 (the last one used here, else 48) so pads and notes on this
    // screen never trigger a sample instrument.
    const uint8_t ci = a.currentInstrument();
    if (isMidiInstrument(ci)) lastMidi_ = ci;
    else a.setCurrentInstrument(lastMidi_);
    playNote = -1;
    playPad_ = -1;
    sel = 0;
    setSource(a.cfg.recSource);
    refreshBar();
}

void RecorderScreen::stop() {
    App& a = app_;
    a.capture.stop();
    a.capture.setMetering(false);
    a.inputMonitorMuted = false;
    // Leaving the recorder: route the line input again so a selected radio
    // tuner powers down (the HAL powers it off for non-radio sources).
    if (a.cfg.recSource == 1) a.selectInput(0, a.cfg.recGain[0]);
    if (previewing_) playStop();
    if (isMidiInstrument(a.currentInstrument())) lastMidi_ = a.currentInstrument();
    if (seeking_) ui_.progressDone();
    seeking_ = false;
}

void RecorderScreen::setSource(int s) {
    App& a = app_;
    s = clampi(s, 0, 3);
    if (s != a.cfg.recSource) {
        a.cfg.recSource = uint8_t(s);
        a.markConfigDirty();
    }
    sourceOk = a.selectInput(uint8_t(s), a.cfg.recGain[s]);
    a.capture.setStereo(stereoInput());   // setup-page meters (2.x)
    if (s == 1 && a.radio()) {
        const int r = a.cfg.radioRegion & 3;
        a.radio()->setBand(kRadioMin[r], kRadioMax, kRadioStep[r]);
        a.radio()->tune(float(a.cfg.radioFreq10) / 10.0f);
    }
    if (!sourceOk) ui_.notify(s == 1 ? "Radio not available" : "Input not available");
}

void RecorderScreen::selection(uint32_t* s0, uint32_t* s1) const {
    *s0 = uint32_t(uint64_t(startM) * len / 65535);
    *s1 = uint32_t(uint64_t(endM) * len / 65535);
    if (*s1 > len) *s1 = len;
    if (*s1 <= *s0) *s1 = *s0 + 1 <= len ? *s0 + 1 : len;
}

void RecorderScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    App& a = app_;
    b.clear();
    char v[24];
    if (a.capture.active()) {   // recording: only Stop (appendix C §2.3)
        b.set(7, "Stop");
        b.highlight(7, true);
        return;
    }
    if (state == 0) {
        const int src = a.cfg.recSource;
        b.set(0, "Source", sourceOk ? sourceName(src) : "n/a");
        if (src == 1) {
            std::snprintf(v, sizeof(v), "%.1fMHz", double(a.cfg.radioFreq10) / 10.0);
            b.set(1, "Radio Freq", v, sourceOk);
            // Seek up / down: blank labels drawn as arrow cells (appendix
            // C §2.3), "◀◀" on F3 and "▶▶" on F4 [S snap 005].
            b.set(2, "", nullptr, sourceOk);
            b.set(3, "", nullptr, sourceOk);
        } else {
            b.set(3, "Metronome", "Config");   // spec 05 appC §2.3, manual p.195 (MANUAL_AUDIT #29)
        }
        b.set(4, "Level");
        std::snprintf(v, sizeof(v), "%d", a.cfg.recGain[src]);
        b.set(5, "Gain", v);
        b.set(6, "Monitor", a.cfg.recMonitor ? "On" : "Off");
        b.set(7, "Record", nullptr, sourceOk);
        // Record keeps the solid red box; the selected parameter is marked by
        // its column's red frame, not on the bar [S snap 005].
        b.fill(7, true);
        return;
    }
    if (state == 1) {
        uint32_t s0, s1;
        selection(&s0, &s1);
        b.set(0, "Preview", "-");
        std::snprintf(v, sizeof(v), "%.3f s", double(s0) / 44100.0);
        b.set(1, "Start Point", v);
        std::snprintf(v, sizeof(v), "%.3f s", double(s1) / 44100.0);
        b.set(2, "End Point", v);
        std::snprintf(v, sizeof(v), "%.2f", double(view.zoom()));
        b.set(3, "Zoom", v);
        b.set(4, "Crop", nullptr, startM > 0 || endM < 65535);
        b.set(5, "Undo", nullptr, undoDepth > 0);
        b.set(6, "Cancel");
        b.set(7, "Save", nullptr, len > 0);
        b.highlightOnly(sel);
    }
}

bool RecorderScreen::record() {
    App& a = app_;
    if (!sourceOk) {
        ui_.notify("Input not available");
        return false;
    }
    a.capture.stop();
    a.capture.setBuffer(a.bank.tail(), uint32_t(a.bank.freeSamples()));
    // 2.x: Line In with Config "Line in channel" = Stereo records two lanes
    // (half the frames of the same memory); every other case is mono.
    const bool stereo = stereoInput();
    if (a.capture.capacityFor(stereo) < 44100) {
        // [I] A resample take holds the free memory while it runs.
        ui_.ask("Sample Recorder", a.bank.reserved() ? "Sample memory is reserved by the resample take" : "Not enough sample memory",
                nullptr, nullptr, "OK", nullptr, nullptr);
        return false;
    }
    // Count-in and metronome (appendix C §2.9, spec 07 Q10 case 3): only with
    // the metronome on and a source other than the radio; count-in of one
    // bar of (tsNum+1)*(tsDen+1) steps like the sequencer, then clicks on
    // every step boundary while recording. Steps follow the project tempo
    // (spec 07 open item 3: whether the transport must run is not known).
    uint32_t steps = 0, beat = 1, bar = 1;
    float stepSecs = 0.0f;
    const bool click = a.cfg.metronome && a.cfg.recSource != 1;
    if (click) {
        steps = uint32_t(a.cfg.tsNum + 1) * uint32_t(a.cfg.tsDen + 1);
        beat = uint32_t(a.cfg.tsDen + 1);
        bar = steps;
        const float t = a.project.tempo();
        stepSecs = 15.0f / (t >= 1.0f ? t : 120.0f);
    }
    a.capture.start(steps, stepSecs, beat, bar, click, stereo);
    countIn_ = steps > 0;
    recording_ = true;
    state = 1;
    len = off = 0;
    lastLen_ = 0;
    undoDepth = 0;
    refreshBar();
    return true;
}

void RecorderScreen::stopRecording() {
    App& a = app_;
    a.capture.stop();
    countIn_ = false;
    recording_ = false;
    len = a.capture.length();
    off = 0;
    undoDepth = 0;
    uint32_t s = 0, e = len;
    autoTrimLR(a.capture.buffer(), a.capture.bufferR(), len, &s, &e);   // appendix C §2.10, both lanes
    startM = len ? uint16_t(uint64_t(s) * 65535 / len) : 0;
    endM = len ? uint16_t(uint64_t(e) * 65535 / len) : 65535;
    if (endM <= startM) endM = uint16_t(startM + 1);
    view.reset(len);
    view.centre = startM;
    sel = 1;
    state = 1;
    refreshBar();
}

bool RecorderScreen::crop() {
    if (state != 1 || len == 0 || startM >= endM) return false;
    uint32_t s0, s1;
    selection(&s0, &s1);
    if (undoDepth == kUndoLevels) {   // keep the newest levels
        for (int i = 1; i < kUndoLevels; ++i) undo_[i - 1] = undo_[i];
        --undoDepth;
    }
    undo_[undoDepth++] = View{off, len};
    off += s0;
    len = s1 - s0;
    startM = 0;
    endM = 65535;
    view.reset(len);
    refreshBar();
    return true;
}

bool RecorderScreen::undoCrop() {
    if (state != 1 || undoDepth == 0) return false;
    const View v = undo_[--undoDepth];
    off = v.off;
    len = v.len;
    startM = 0;
    endM = 65535;
    view.reset(len);
    refreshBar();
    return true;
}

void RecorderScreen::openNaming(const char* initial) {
    state = 2;
    ui_.kb.open("Recording name", initial, 32, "Save",
                [](void* c, bool ok, const char* text) {
                    auto* s = static_cast<RecorderScreen*>(c);
                    if (!ok || !text[0]) {
                        s->state = 1;   // b5 Cancel: back to editing
                        s->refreshBar();
                        return;
                    }
                    s->save(text, s->ui_.kb.alt(), false);
                },
                this);
    ui_.kb.setAlt("Save & Load");
}

bool RecorderScreen::save(const char* name, bool load, bool overwrite) {
    App& a = app_;
    if (previewing_) playStop();   // Save & Load reuses the memory being played
    std::snprintf(fileName, sizeof(fileName), "%s", name);
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s.wav", folder, name);
    uint32_t s0, s1;
    selection(&s0, &s1);
    const uint32_t frames = s1 - s0;
    const int16_t* r = dataR();   // stereo recording: the right lane
    int slot = -1;
    if (load) {
        // appendix C §2.12: memory and a free sample slot are checked first.
        slot = a.firstFreeSampleSlot();
        if (slot < 0) {
            ui_.ask("Sample Recorder", "Not enough instruments", nullptr, nullptr, "OK", nullptr, nullptr);
            state = 1;
            refreshBar();
            return false;
        }
        if (uint64_t(a.bank.usedBytes()) + uint64_t(frames) * (r ? 4u : 2u) > a.bank.capacityBytes()) {
            ui_.ask("Sample Recorder", "Not enough memory in sample bank", nullptr, nullptr, "OK", nullptr, nullptr);
            state = 1;
            refreshBar();
            return false;
        }
    }
    if (!overwrite && a.fs().exists(path)) {
        static bool sLoad;
        sLoad = load;
        ui_.ask("Sample Recorder", "Name exists. Overwrite?", nullptr, "Rename", "Overwrite",
                [](void* c, int key) {
                    auto* s = static_cast<RecorderScreen*>(c);
                    if (key == btn::F8) {
                        s->save(s->fileName, sLoad, true);
                        return;
                    }
                    // Rename: first free "<name><k>" (appendix C §2.12), back to the keyboard.
                    char nm[48], p[112];
                    for (int k = 1; k < 1000; ++k) {
                        std::snprintf(nm, sizeof(nm), "%.32s%d", s->fileName, k);
                        std::snprintf(p, sizeof(p), "%s/%s.wav", s->folder, nm);
                        if (!s->app_.fs().exists(p)) break;
                    }
                    s->openNaming(nm);
                },
                this);
        return false;
    }
    ui_.progress("Saving recording", 0);
    const bool ok = a.writeWav(path, data() + s0, r ? r + s0 : nullptr, frames);
    ui_.progressDone();
    if (!ok) {
        ui_.notify("Save failed");
        state = 1;
        refreshBar();
        return false;
    }
    if (load) {
        ui_.progress("Loading recording", 50);
        const bool imported = a.importSample(path, slot);
        ui_.progressDone();
        len = 0;   // the bank tail (our buffer) was reused by the import
        if (!imported) {
            ui_.notify("Load failed");
            state = 0;
            refreshBar();
            return false;
        }
        a.setCurrentInstrument(slot);
        ui_.switchTo(kModLoader, slot + 1);   // appendix C §2.12: screen 0xd with slot+1
        return true;
    }
    char msg[64];
    std::snprintf(msg, sizeof(msg), "Saved %s.wav", name);
    ui_.notify(msg);
    state = 1;
    refreshBar();
    return true;
}

void RecorderScreen::change(int d) {
    App& a = app_;
    if (state == 0) {
        const int src = a.cfg.recSource;
        switch (sel) {
            case 0: setSource(clampi(src + d, 0, 3)); break;   // encoder: clamp, no wrap
            case 1:
                if (src == 1 && sourceOk) {
                    const int r = a.cfg.radioRegion & 3;
                    const int st = int(kRadioStep[r] * 10.0f + 0.5f);
                    a.cfg.radioFreq10 = uint16_t(clampi(a.cfg.radioFreq10 + d * st, int(kRadioMin[r] * 10.0f + 0.5f), 1080));
                    a.radio()->tune(float(a.cfg.radioFreq10) / 10.0f);
                    a.markConfigDirty();
                }
                break;
            case 5:
                a.cfg.recGain[src] = uint8_t(clampi(a.cfg.recGain[src] + d, 0, 100));
                a.selectInput(uint8_t(src), a.cfg.recGain[src]);
                a.markConfigDirty();
                break;
            case 6:
                a.cfg.recMonitor = d > 0 ? 1 : 0;
                a.inputMonitorMuted = a.cfg.recMonitor == 0;
                a.markConfigDirty();
                break;
            default: break;
        }
    } else if (state == 1) {
        // One detent = range/480 (appendix C §2.11).
        const int st = int(65535.0f / view.zoom() / 480.0f) < 1 ? 1 : int(65535.0f / view.zoom() / 480.0f);
        if (sel == 1) {
            startM = uint16_t(clampi(startM + d * st, 0, endM - 1));
            view.centre = startM;
        } else if (sel == 2) {
            endM = uint16_t(clampi(endM + d * st, startM + 1, 65535));
            view.centre = endM;
        } else if (sel == 3) {
            view.step(d);
        }
    }
    refreshBar();
}

#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
bool RecorderScreen::encoderOnList() const {
    return state == 0 && sel == 0;   // source list
}
#endif

void RecorderScreen::onEncoder(int d) {
    if (app_.capture.active()) return;
    change(d);
}

void RecorderScreen::play(int note) {
    App& a = app_;
    a.previewFile(nullptr, 0, false);
    uint32_t s0, s1;
    selection(&s0, &s1);
    const uint32_t n = s1 - s0;
    const int16_t* r = dataR();
    // The preview player transposes (16.16 step, linear interpolation per
    // lane; 2.x). Before, the selection was resampled into the free sample
    // memory behind the recording, mono only and cut short when that memory
    // ran out.
    const uint32_t step = App::previewStep(note);
    a.previewPitched(data() + s0, r ? r + s0 : nullptr, n, note);
    previewing_ = true;
    playS0_ = s0;
    headAt_ = 0;   // poll the playhead on the next pass
    playNote = note;
    playFrames = n > 1 ? uint32_t((uint64_t(n - 1) << 16) / step + 1) : n;
}

void RecorderScreen::playStop() {
    app_.previewFile(nullptr, 0, false);
    previewing_ = false;
    playNote = -1;
    playPad_ = -1;
}

void RecorderScreen::onPad(uint8_t pad, uint8_t st) {
    // appendix C §2.11: pads play the recording chromatically through the
    // pad -> note map (note 60 = own pitch); release = note-off. One
    // playback at a time: the last pad pressed sounds.
    if (state != 1 || app_.capture.active() || len == 0) return;
    if (st == kPress) {
        play(app_.padNote(pad));
        playPad_ = pad;
    } else if (st == kRelease && previewing_ && playPad_ == int(pad)) {
        playStop();
    }
}

bool RecorderScreen::onMidiNote(uint8_t note, uint8_t velocity, bool on) {
    // spec 17 §1.3 owner 4: the recorder preview (0x580f0 / 0x56750), the
    // same chromatic playback as the pads (appendix C §2.11; note 60 = own
    // pitch, the last note sounds). SPEC GAP: velocity is not used and only
    // the sounding note's note-off stops it.
    (void)velocity;
    if (state != 1 || app_.capture.active() || len == 0) return true;
    if (on) {
        play(note);
        playPad_ = -2;
    } else if (previewing_ && playPad_ == -2 && playNote == int(note)) {
        playStop();
    }
    return true;
}

void RecorderScreen::onButton(uint8_t id, uint8_t st) {
    App& a = app_;
    const bool rec = a.capture.active();
    if (id == btn::F1 && state == 1 && !rec) {   // b0 Preview: hold to play at note 60
        if (st == kPress && len > 0) {
            play(60);
            playPad_ = -1;
            sel = 0;
        } else if (st == kRelease && previewing_ && playPad_ < 0) {
            playStop();
        }
        refreshBar();
        return;
    }
    if (st != kPress) return;
    if (id == btn::Play) {   // stops the sequencer (appendix C §2.13)
        if (a.seq.isPlaying()) a.togglePlay(false);
        return;
    }
    if (rec) {
        if (id == btn::F8 || id == btn::Rec) stopRecording();
        return;
    }
    if (id == btn::Rec && state == 0) {
        record();
        return;
    }
    if (id == btn::Left || id == btn::Right) {
        sel = arrowSel(sel, id == btn::Right ? 1 : -1, state, a.cfg.recSource == 1);
        if (state == 1 && (sel == 1 || sel == 2)) view.centre = sel == 1 ? startM : endM;
        refreshBar();
        return;
    }
    if (id == btn::Up || id == btn::Down) {
        // [I] The Source list is drawn top to bottom: Up = the item above,
        // Down = the item below (the encoder's + is the next item down), as
        // in PlaybackScreen. Monitor (On above Off) already moves this way.
        const int d = id == btn::Up ? 1 : -1;
        change(state == 0 && sel == 0 ? -d : d);
        return;
    }
    if (state == 0) {
        const int src = a.cfg.recSource;
        switch (id) {
            case btn::F1:
                if (sel == 0) setSource((src + 1) % 4);   // already selected: next source, wrapping
                sel = 0;
                break;
            case btn::F2:
                if (src == 1) sel = 1;
                break;
            case btn::F3:
            case btn::F4:
                if (src == 1) {
                    if (sourceOk && a.radio() && a.radio()->seek(id == btn::F3)) {
                        seeking_ = true;
                        ui_.notify("Seeking...", 10000);
                    }
                } else if (id == btn::F4) {
                    // Metronome / Config (appendix C §2.9): remember to come back.
                    ui_.switchTo(kModConfig);
                    configReturn_ = true;
                    return;
                }
                break;
            case btn::F5: sel = 4; break;
            case btn::F6: sel = 5; break;
            case btn::F7:
                if (sel == 6) change(a.cfg.recMonitor ? -1 : 1);
                sel = 6;
                break;
            case btn::F8: record(); return;
            default: break;
        }
        refreshBar();
        return;
    }
    if (state == 1) {
        switch (id) {
            case btn::F2: sel = 1; view.centre = startM; break;
            case btn::F3: sel = 2; view.centre = endM; break;
            case btn::F4: sel = 3; break;
            case btn::F5: crop(); break;
            case btn::F6: undoCrop(); break;
            case btn::F7:
                ui_.ask("Sample Recorder", "Changes will be lost. Continue?", nullptr, "Cancel", "Yes",   // manual p.196 (MANUAL_AUDIT #30)
                       
                        [](void* c, int key) {
                            auto* s = static_cast<RecorderScreen*>(c);
                            if (key != btn::F8) return;
                            s->state = 0;
                            s->len = 0;
                            s->sel = 0;
                            s->refreshBar();
                        },
                        this);
                return;
            case btn::F8:
                if (len == 0) break;
                if (!a.recordingsFolder(folder, sizeof(folder))) {
                    ui_.notify("Cannot create the Recordings folder");
                    break;
                }
                char nm[40];
                if (!a.recordingName(folder, nm, sizeof(nm))) std::snprintf(nm, sizeof(nm), "recording");
                openNaming(nm);
                return;
            default: break;
        }
        refreshBar();
    }
}

// Left / Right: previous / next parameter (appendix C §2.13), i.e. only the
// boxes the F-keys select: setup Source, Radio Freq (radio source), Level,
// Gain, Monitor; edit Start, End, Zoom. Stepping through every slot 0..7
// left the focus on blank or action cells where the arrows and the encoder
// did nothing (user report on 2.0.1, also 1.9.4).
int RecorderScreen::arrowSel(int sel, int dir, int state, bool radio) {
    static const int8_t kSetup[] = {0, 1, 4, 5, 6};
    static const int8_t kEdit[] = {1, 2, 3};
    const int8_t* list = state == 1 ? kEdit : kSetup;
    const int n = state == 1 ? 3 : 5;
    int8_t slots[5];
    int k = 0;
    for (int i = 0; i < n; ++i)
        if (list[i] != 1 || state == 1 || radio) slots[k++] = list[i];
    if (dir > 0) {
        for (int i = 0; i < k; ++i)
            if (slots[i] > sel) return slots[i];
        return slots[k - 1];
    }
    for (int i = k - 1; i >= 0; --i)
        if (slots[i] < sel) return slots[i];
    return slots[0];
}

// x of the preview playhead in the edit view, -1 when nothing plays or it is
// outside the zoom window (the Playback screen's look: a 1-px line, th.good).
int RecorderScreen::playheadX() const {
    if (state != 1 || !previewing_) return -1;
    const int32_t p = app_.engine.previewBufferPos();
    if (p < 0 || len == 0) return -1;   // len: the take as cropped (the edit view's length)
    float v0, v1;
    view.window(&v0, &v1);
    const float f = float(playS0_ + uint32_t(p)) / float(len);
    const int x = kWaveX + int((f - v0) / (v1 - v0) * 599.0f + 0.5f);
    return x >= kWaveX && x < kWaveX + kWaveW ? x : -1;
}

void RecorderScreen::update(uint32_t now) {
    App& a = app_;
    // Preview playhead: polled every 5 ms, a frame only when its pixel moves.
    if (now - headAt_ >= 5) {
        headAt_ = now;
        if (playheadX() != headX_) ui_.dirty();
    }
    // Monitor (appendix C §2.7) only on the setup page and while recording:
    // after a take, the input (e.g. the radio) no longer plays under Preview
    // and the pads while the recording is trimmed (owner request).
    a.inputMonitorMuted = a.cfg.recMonitor == 0 || (state != 0 && !a.capture.active());
    // Recording finished by the tap itself (memory full, appendix C §2.10).
    if (recording_ && !a.capture.active()) {
        stopRecording();
        if (a.capture.full()) ui_.ask("Memory full", "Recording stopped", nullptr, nullptr, "OK", nullptr, nullptr);
    }
    if (a.capture.active()) {
        if (countIn_ && a.capture.recording()) {
            countIn_ = false;
            ui_.dirty();
        }
        const uint32_t l = a.capture.length();
        // Own timer: the meter below advances lastMeterAt_ every frame, so the
        // recording view (time, waveform, memory) needs a separate 40 ms gate.
        if (l != lastLen_ && now - lastRecDrawAt_ >= 40) {
            lastRecDrawAt_ = now;
            lastLen_ = l;
            ui_.dirty();
        }
    }
    // Radio source: re-route about once a second. The HAL answers from its
    // cache while the tuner runs; after an I2C failure dropped it (radio_si4703
    // fail()) this powers it up again and it re-tunes to the last frequency.
    if (a.cfg.recSource == 1 && sourceOk && !seeking_ && now - lastRadioCheckAt_ >= 1000) {
        lastRadioCheckAt_ = now;
        a.selectInput(1, a.cfg.recGain[1]);
    }
    // Meter (appendix C §2.6): stock reads the level every frame and lets the
    // bar fall one unit per update pass once > 500 us have passed (spec 05
    // appendix A "UI animation timers (micros)": 0x56800). The level is read
    // once per display frame (kMeterMs) and falls 1 unit per ms, so a 0..100
    // fall takes 0.1 s; the frame is redrawn only when the bar or the clip
    // colour changes.
    if (now - lastMeterAt_ >= kMeterMs) {
        const uint32_t dt = now - lastMeterAt_;
        lastMeterAt_ = now;
        // 2.x stereo: one meter per lane (side by side); mono: both the mix.
        // Config "Line in channel" may have changed (Metronome -> Config):
        // follow it while idle on the setup page.
        if (state == 0 && !a.capture.active()) a.capture.setStereo(stereoInput());
        const bool st2 = a.capture.stereo();
        const int u = meterUnits(a.capture.takePeak(0));
        const int ur = st2 ? meterUnits(a.capture.takePeak(1)) : u;
        if (!st2) a.capture.takePeak(1);   // keep the unused side fresh
        if (u >= 85 || ur >= 85) clipUntil_ = now + 350;
        const int before = meter, beforeR = meterR;
        meter = meterFall(meter, u, dt);
        meterR = meterFall(meterR, ur, dt);
        const bool clip = int32_t(now - clipUntil_) < 0;
        if (state == 0 && (meter != before || meterR != beforeR || clip != clipShown_)) ui_.dirty();
        clipShown_ = clip;
    }
    if (seeking_ && a.radio() && !a.radio()->busy()) {
        seeking_ = false;
        a.cfg.radioFreq10 = uint16_t(a.radio()->frequency() * 10.0f + 0.5f);
        a.markConfigDirty();
        ui_.notify("Tuned", 800);
        refreshBar();
    }
}

void RecorderScreen::draw(Gfx& g) {
    App& a = app_;
    const Theme& th = ui_.th();
    ui_.drawTitle(g, "Sample Recorder");
    if (state == 2) return;   // keyboard
    const uint32_t wave = th.slot(slot::kWave);
    if (a.capture.active()) {
        const uint32_t l = a.capture.length();
        if (a.capture.stereo()) {
            waveRegion(g, th, kWaveX, kWaveX + kWaveW - 1, kWaveY, kLaneH);
            waveRegion(g, th, kWaveX, kWaveX + kWaveW - 1, kLaneY2, kLaneH);
            drawWaveData(g, th, a.capture.buffer(), l, 0.0f, 1.0f, kWaveX, kWaveY, kWaveW, kLaneH, wave);
            drawWaveData(g, th, a.capture.bufferR(), l, 0.0f, 1.0f, kWaveX, kLaneY2, kWaveW, kLaneH, wave);
        } else {
            waveRegion(g, th, kWaveX, kWaveX + kWaveW - 1, kWaveY, kWaveH);
            drawWaveData(g, th, a.capture.buffer(), l, 0.0f, 1.0f, kWaveX, kWaveY, kWaveW, kWaveH, wave);
        }
        if (a.capture.state() == Capture::kCountIn) {
            g.text(400, 80, kFontLarge, th.yellow, "Counting in...", IDisplay::kOptCenter);
        } else {
            g.textf(400, 80, kFontLarge, th.alert, IDisplay::kOptCenter, a.capture.stereo() ? "Recording ST  %.1f s"
                                                                                          : "Recording  %.1f s",
                    double(l) / 44100.0);
        }
        g.textf(400, 370, kFontSmall, th.dim, IDisplay::kOptCenter, "Memory: %.1f s left",
                double(a.capture.capacity() - l) / 44100.0);
        return;
    }
    if (state == 0) {
        const int src = a.cfg.recSource;
        const char* names[4] = {sourceName(0), kSources[1], kSources[2], kSources[3]};
        list(g, th, 0, names, 4, src);
        if (src == 1 && sourceOk) {
            // Radio frequency as a bar (needle position 0..100, appendix C §2.8).
            const int r = a.cfg.radioRegion & 3;
            const float f = float(a.cfg.radioFreq10) / 10.0f;
            bar(g, th, 1, (f - kRadioMin[r]) / (kRadioMax - kRadioMin[r]));
        } else {
            wg::panel(g, th, kColX0 + 100);
        }
        wg::panel(g, th, kColX0 + 200, wg::kPanelY, wg::columnW(2));
        // Free recording time for the current mode (2.x): stereo takes two
        // samples per frame, so half the time of mono. On the bottom row of
        // the blank double panel, which stays blank above it as on stock.
        const bool st2 = a.capture.stereo();
        const double freeS = double(a.bank.freeSamples()) / (st2 ? 2.0 : 1.0) / 44100.0;
        g.textf(kColX0 + 200 + wg::columnW(2) / 2, 407, kFontSmall, th.dim, IDisplay::kOptCenter, "Free %.1f s %s",
                freeS, st2 ? "stereo" : "mono");
        // Level meter (appendix C §2.3 obj+0x68, §2.6): a green bar, the
        // 61..70 / 71..84 bands and the clip window in their colours. 2.x
        // stereo: L and R as two half-width bars side by side in the column.
        const bool clipNow = int32_t(ui_.now() - clipUntil_) < 0;
        auto meterColour = [&](int units) {
            uint32_t c = th.good;
            if (units > 70) c = th.yellow;
            if (units >= 85 || clipNow) c = th.alert;
            return c;
        };
        const int units = clampi(meter, 0, 100);
        if (!st2) {
            bar(g, th, 4, float(units) / 100.0f, meterColour(units));
        } else {
            const int unitsR = clampi(meterR, 0, 100);
            const int x = kColX0 + 400, w = wg::columnW();
            wg::ValueBarStyle vs;
            wg::panel(g, th, x, wg::kPanelY, w, wg::kPanelH, th.slot(vs.set + 4));
            vs.background = false;
            vs.fill = meterColour(units);
            wg::valueBar(g, th, x, wg::kPanelY, w / 2 + 1, wg::kPanelH, units < 1 ? kMinBar : float(units) / 100.0f, vs);
            vs.fill = meterColour(unitsR);
            wg::valueBar(g, th, x + w / 2 - 1, wg::kPanelY, w / 2 + 1, wg::kPanelH,
                         unitsR < 1 ? kMinBar : float(unitsR) / 100.0f, vs);
        }
        bar(g, th, 5, float(a.cfg.recGain[src]) / 100.0f);
        list(g, th, 6, kMonitor, 2, a.cfg.recMonitor ? 0 : 1);
        wg::panel(g, th, kColX0 + 700);
        if (sel == 0 || sel == 1 || sel == 4 || sel == 5 || sel == 6) wg::panelFocus(g, th, wg::columnX(sel), wg::columnW());
        if (src == 1) {
            barSeekArrows(g, th, 2, -1, sourceOk);
            barSeekArrows(g, th, 3, 1, sourceOk);
        }
        return;
    }
    // state 1: edit after recording
    float v0, v1;
    view.window(&v0, &v1);
    auto mx = [&](uint16_t m) { return kWaveX + int((float(m) / 65535.0f - v0) / (v1 - v0) * 599.0f + 0.5f); };
    const int xs = mx(startM), xe = mx(endM);
    const int last = kWaveX + kWaveW - 1;
    if (const int16_t* r = dataR()) {   // stereo take: L and R lanes
        if (xe >= kWaveX && xs <= last) {
            waveRegion(g, th, clampi(xs, kWaveX, last), clampi(xe, kWaveX, last), kWaveY, kLaneH);
            waveRegion(g, th, clampi(xs, kWaveX, last), clampi(xe, kWaveX, last), kLaneY2, kLaneH);
        }
        drawWaveData(g, th, data(), len, v0, v1, kWaveX, kWaveY, kWaveW, kLaneH, wave);
        drawWaveData(g, th, r, len, v0, v1, kWaveX, kLaneY2, kWaveW, kLaneH, wave);
    } else {
        if (xe >= kWaveX && xs <= last) waveRegion(g, th, clampi(xs, kWaveX, last), clampi(xe, kWaveX, last), kWaveY, kWaveH);
        drawWaveData(g, th, data(), len, v0, v1, kWaveX, kWaveY, kWaveW, kWaveH, wave);
    }
    if (xs >= kWaveX && xs <= last) waveMarker(g, th, xs, sel == 1 ? th.accent : wave, 1, true, false, 108, 334);
    if (xe >= kWaveX && xe <= last) waveMarker(g, th, xe, sel == 2 ? th.accent : wave, -1, true, false, 108, 334);
    headX_ = playheadX();
    if (headX_ >= 0) g.vline(headX_, kWaveY, kWaveH, th.good);
    g.textf(400, 80, kFontText, th.text, IDisplay::kOptCenter, "Recorded %.2f s   crop levels %d/%d", double(len) / 44100.0,
            undoDepth, kUndoLevels);
}

}  // namespace ot
