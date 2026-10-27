// Sample Editor (spec 06 §6; spec 05 §2.10): Start/End markers with zoom,
// 17 offline effects with parameters (app/sample_fx.*), preview, apply and
// exactly one level of undo.
//
// Memory: the edited instrument stays in the sample bank. An effect result is
// built at the bank tail and swapped in with SampleBank::replaceFromTail(),
// which leaves the previous data at the new tail -- the one-level undo
// buffer. Undo swaps back and clears it (spec 06 §6.3 F4). A preview result
// is processed after the undo data and played through the file-preview path.
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "app/sample_fx.h"
#include "ui/screens.h"

namespace ot {

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Waveform x 0..599, rows 116..334 (centre 225), markers 108..334; effect
// list in the panel at 601 (stock 006, VISUAL_DIFF §3.5).
constexpr int kWaveY = 116, kWaveH = 219, kMarkTop = 108, kMarkBottom = 334;
constexpr int kListX = 601, kListW = 197;

void effectItem(void*, int i, char* out, int cap) { std::snprintf(out, size_t(cap), "%s", sfx::effect(i).name); }
}  // namespace

// Waveform chrome and bar glyphs (wave.cpp).
void waveRegion(Gfx& g, const Theme& th, int xa, int xb, int y0, int h);
void waveMarker(Gfx& g, const Theme& th, int x, uint32_t c, int dir, bool top, bool bottom, int y0, int y1);
void barSelector(Gfx& g, const Theme& th, int i, const char* title, int d);
void barGreyLabel(Gfx& g, const Theme& th, int i, const char* top, const char* bottom);

void EditorScreen::start(int opt) {
    (void)opt;
    App& a = app_;
    page = 1;
    inert = true;
    stopPreview();
    const uint8_t ci = a.currentInstrument();
    slot_ = ci;
    if (ci != lastInstr_) undoAvail = false;   // the undo belongs to one instrument
    lastInstr_ = ci;
    ui_.bar.clear();
    if (ci >= kSampleInstrumentCount || !a.bank.loaded(ci)) return;   // MIDI / empty: inert (spec 06 §6.1 step 4)
    if (a.bank.fullFrames(ci) > sfx::kMaxFrames) {
        // spec 06 §6.1 step 1: message, F8 leaves.
        ui_.ask("Sample Editor", "The selected sample is too long", nullptr, nullptr, "OK",
                [](void* c, int) {
                    auto* s = static_cast<EditorScreen*>(c);
                    s->ui_.switchTo(s->ui_.previous());
                },
                this);
        return;
    }
    if (a.streamed(ci)) {
        // SD streaming: only the head is in memory; edits need the whole
        // sample (docs/PLAN_SD_STREAMING_P1C_P2.md §2.4).
        ui_.ask("Sample Editor", "This sample streams from the card. Load it into memory?", nullptr, "Cancel",
                "Load",
                [](void* c, int key) {
                    auto* s = static_cast<EditorScreen*>(c);
                    if (key != btn::F8 || !s->app_.loadStreamedIntoMemory(s->slot_)) {
                        s->ui_.switchTo(s->ui_.previous());
                        return;
                    }
                    s->start(0);
                },
                this);
        return;
    }
    if (a.seq.isPlaying()) {
        // spec 06 §6.1 step 2: F7 Cancel leaves, F8 stops the pattern and enters.
        ui_.ask("Sample Editor", "This will stop the pattern. Continue?", nullptr, "Cancel", "Yes",
                [](void* c, int key) {
                    auto* s = static_cast<EditorScreen*>(c);
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

void EditorScreen::enter() {
    App& a = app_;
    a.previewAllOff();
    inert = false;
    startM = 0;
    endM = 65535;
    view.reset(a.bank.frames(slot_));
    // spec 06 §6.1: load the 17 x 9 default table on every normal entry.
    for (int e = 0; e < sfx::kEffectCount; ++e) sfx::defaults(e, params[e]);
    const float t = a.project.tempo();
    if (t >= 40.0f && t <= 800.0f) params[sfx::kStretchBeat][0] = float(int(t + 0.5f));
    {   // Timestretch Note Length starts at the sample's length (manual p.218,
        // MANUAL_AUDIT_2026-10-23 #14), 0.01 s steps, 0.1..8 s.
        float sec = float(a.bank.frames(slot_)) / 44100.0f;
        sec = float(int(sec * 100.0f + 0.5f)) / 100.0f;
        params[sfx::kStretchNote][0] = sec < 0.1f ? 0.1f : (sec > 8.0f ? 8.0f : sec);
    }
    previewState = 0;
    refreshBar();
}

void EditorScreen::stop() {
    stopPreview();
    // The undo data lives in the free bank tail, which other screens reuse.
    undoAvail = false;
}

// Where the preview plays, as a fraction of the sample (the processed
// selection maps back onto the selection), -1 when nothing plays.
float EditorScreen::playheadFrac() const {
    if (!previewPlaying || playOut_ == 0) return -1.0f;
    const int32_t p = app_.engine.previewBufferPos();
    const uint32_t len = app_.bank.frames(slot_);
    if (p < 0 || len == 0) return -1.0f;
    const double inSel = double(p) * double(playSel_) / double(playOut_);
    return float((double(playS0_) + inSel) / double(len));
}

void EditorScreen::update(uint32_t now) {
    // Preview playhead: polled every 5 ms, a frame only when its pixel moves
    // (any change of the fraction worth a pixel at the widest view).
    if (now - headAt_ >= 5) {
        headAt_ = now;
        const float f = playheadFrac();
        const int x = f < 0.0f ? -1 : int(f * 2000.0f);
        if (x != headX_) {
            headX_ = x;
            ui_.dirty();
        }
    }
    // [I] A resample take holds the free bank tail, where the undo data
    // lives: the take records over it.
    if (undoAvail && app_.bank.reserved()) {
        undoAvail = false;
        refreshBar();
        ui_.dirty();
    }
}

void EditorScreen::stopPreview() {
    if (previewPlaying) app_.previewFile(nullptr, 0, false);
    previewPlaying = false;
    midiNote_ = -1;
}

void EditorScreen::selection(uint32_t* s0, uint32_t* s1) const {
    const uint32_t len = app_.bank.frames(slot_);
    *s0 = uint32_t(uint64_t(startM) * len / 65535);
    *s1 = uint32_t(uint64_t(endM) * len / 65535);
    if (*s1 > len) *s1 = len;
    if (*s1 <= *s0) *s1 = *s0 + 1 <= len ? *s0 + 1 : len;
}

bool EditorScreen::fits(uint32_t newFrames, uint32_t extraSamples) {
    const uint32_t ch = app_.bank.stereo(slot_) ? 2u : 1u;
    if (newFrames == 0 || newFrames > sfx::kMaxFrames ||
        uint64_t(newFrames) * ch + extraSamples > app_.bank.freeSamples()) {
        const bool stretch = effect == sfx::kStretchBeat || effect == sfx::kStretchNote;
        ui_.ask("Sample Editor", stretch ? "The stretched sample is too long" : "The result would be too long", nullptr,
                nullptr, "OK", nullptr, nullptr);
        return false;
    }
    return true;
}

bool EditorScreen::apply() {
    App& a = app_;
    if (inert || !a.bank.loaded(slot_)) return false;
    const uint32_t len = a.bank.frames(slot_);
    const bool stereo = a.bank.stereo(slot_);
    const uint32_t ch = stereo ? 2u : 1u;
    uint32_t s0, s1;
    selection(&s0, &s1);
    const uint32_t sel = s1 - s0;
    const float* p = params[effect];
    const uint32_t outSel = sfx::outputFrames(effect, p, sel);
    const uint32_t newLen = effect == sfx::kCrop ? outSel : len - sel + outSel;
    if (!fits(newLen, 0)) return false;
    stopPreview();
    a.allVoicesOff();   // voices point into the bank
    ui_.progress("Applying", 0);
    int16_t* t = a.bank.tail();
    const int16_t* cur = a.bank.data(slot_);
    for (uint32_t c = 0; c < ch; ++c) {
        const int16_t* src = cur + c * len;
        int16_t* dst = t + c * newLen;
        if (effect == sfx::kCrop) {
            std::memcpy(dst, src + s0, size_t(sel) * sizeof(int16_t));
        } else {
            std::memcpy(dst, src, size_t(s0) * sizeof(int16_t));
            sfx::process(effect, p, src + s0, sel, dst + s0, outSel);
            std::memcpy(dst + s0 + outSel, src + s1, size_t(len - s1) * sizeof(int16_t));
        }
        ui_.progress("Applying", int((c + 1) * 100 / ch));
    }
    if (!a.bank.replaceFromTail(slot_, newLen, stereo)) {
        ui_.progressDone();
        return false;
    }
    a.refreshStreamSources();   // later slots may have moved (SD streaming heads)
    undoFrames_ = len;
    undoStereo_ = stereo;
    undoAvail = true;
    a.instruments[slot_].lengthFrames = newLen;
    // Markers: the processed region (crop: the whole result).
    if (effect == sfx::kCrop || newLen == 0) {
        startM = 0;
        endM = 65535;
    } else {
        startM = uint16_t(uint64_t(s0) * 65535 / newLen);
        endM = uint16_t(uint64_t(s0 + outSel) * 65535 / newLen);
        if (endM <= startM) endM = uint16_t(startM + 1);
    }
    view.reset(newLen);
    a.markInstrumentDirty(slot_, true);
    ui_.progressDone();
    previewState = 0;
    page = 1;
    refreshBar();
    return true;
}

bool EditorScreen::undo() {
    App& a = app_;
    if (inert || !undoAvail || !a.bank.loaded(slot_)) return false;
    stopPreview();
    a.allVoicesOff();
    // The undo data sits at the tail: swap it in (the current data goes to
    // the tail and is dropped -- one level only, spec 06 §6.3 F4).
    if (!a.bank.replaceFromTail(slot_, undoFrames_, undoStereo_)) return false;
    a.refreshStreamSources();   // later slots may have moved (SD streaming heads)
    undoAvail = false;
    a.instruments[slot_].lengthFrames = undoFrames_;
    startM = 0;
    endM = 65535;
    view.reset(undoFrames_);
    a.markInstrumentDirty(slot_, true);
    refreshBar();
    return true;
}

bool EditorScreen::preview(int note) {
    App& a = app_;
    if (inert || !a.bank.loaded(slot_)) return false;
    stopPreview();
    const uint32_t len = a.bank.frames(slot_);
    const bool stereo = a.bank.stereo(slot_);
    const uint32_t ch = stereo ? 2u : 1u;
    uint32_t s0, s1;
    selection(&s0, &s1);
    const uint32_t sel = s1 - s0;
    const uint32_t outSel = sfx::outputFrames(effect, params[effect], sel);
    // The processed preview belongs to one selection and effect: moving a
    // marker made the old audio play cut to the new length (user report:
    // the further Start moved, the more was missing at the end).
    if (s0 != prevS0_ || s1 != prevS1_ || effect != prevEffect_) previewState = 0;
    // Keep the undo data at the tail intact: process after it.
    const uint32_t keep = undoAvail ? undoFrames_ * (undoStereo_ ? 2u : 1u) : 0u;
    if (!fits(outSel, keep)) return false;
    int16_t* out = a.bank.tail() + keep;
    if (previewState == 0) {
        ui_.progress("Processing", 0);
        const int16_t* cur = a.bank.data(slot_);
        for (uint32_t c = 0; c < ch; ++c) sfx::process(effect, params[effect], cur + c * len + s0, sel, out + c * outSel, outSel);
        ui_.progressDone();
        previewState = 1;
        prevS0_ = s0;
        prevS1_ = s1;
        prevEffect_ = effect;
    }
    // Pads play at their note (2.x; 60 = own pitch) through the player's
    // 16.16 step; Preview and MIDI notes keep the own pitch.
    a.engine.previewBuffer(out, outSel, stereo, 0xFFFFFFFFu, App::previewStep(note));
    previewPlaying = true;
    playS0_ = s0;
    playSel_ = sel;
    playOut_ = outSel;
    headAt_ = 0;
    refreshBar();
    return true;
}

void EditorScreen::onPad(uint8_t pad, uint8_t state) {
    // Pads audition the sample being edited, like the Preview button: the
    // processed selection (crop and the current effect, not applied), cut
    // when the pad is released. Before, pads played the instrument through
    // the Sample Playback points and envelope, so an edit was only heard
    // after the job finished (user report) [I]. 2.x: at the pad's note
    // (App::padNote, note 60 = own pitch), as the recorder pads play.
    if (state == kPress) {
        if (preview(app_.padNote(pad))) padPreview_ = pad;
    } else if (state == kRelease && previewPlaying && padPreview_ == int(pad)) {
        padPreview_ = -1;
        stopPreview();
        refreshBar();
    }
}

bool EditorScreen::onMidiNote(uint8_t note, uint8_t velocity, bool on) {
    // spec 17 §1.3 owner 3: the editor preview (0x4ade8 / 0x48f00). SPEC GAP
    // (§1.12 leaves the owner semantics to the screen): a note-on starts the
    // F7 preview of the processed selection at its own pitch (the note number
    // and velocity are not used), and that note's note-off stops it.
    (void)velocity;
    if (on) {
        if (preview()) midiNote_ = note;
    } else if (previewPlaying && midiNote_ == int(note)) {
        stopPreview();
        refreshBar();
    }
    return true;
}

void EditorScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    App& a = app_;
    b.clear();
    if (inert) return;
    const sfx::EffectDesc& e = sfx::effect(effect);
    char v[24];
    if (page == 1) {
        const uint32_t len = a.bank.frames(slot_);
        // spec 06 §6.3 time = len / (44100 / (1 - q/12)) * marker / 65535 (q = quality byte +0x173).
        const int q = int8_t(a.instruments[slot_].reserved173);
        const float secs = float(len) / (44100.0f / (1.0f - float(q) / 12.0f));
        std::snprintf(v, sizeof(v), "%.3fs", double(secs * float(startM) / 65535.0f));
        b.set(0, "Start", v);
        std::snprintf(v, sizeof(v), "%.3fs", double(secs * float(endM) / 65535.0f));
        b.set(1, "End", v);
        std::snprintf(v, sizeof(v), "%.2f", double(view.zoom()));
        b.set(2, "Zoom", v);
        b.set(3, "Undo", nullptr, undoAvail);
        if (e.params) {
            b.set(5, "Select", "effect");   // grey two-line label, drawn by draw() [S snap 006]
            b.label[5].hidden = true;
        } else {
            b.set(5, "Apply");
        }
        // "▲ Effect ▼" selector [S snap 006]: draw() renders it; the label
        // keeps the effect name but is not drawn. The focused list's red
        // frame marks the selection instead of a bar frame.
        b.setWide(6, "Effect", e.name);
        b.label[6].hidden = true;
        b.separators = uint8_t(b.separators & ~(1u << 7));
        b.highlightOnly(focus == 3 ? -1 : focus);
        return;
    }
    for (int i = 0; i < 4; ++i) {
        const int k = paramPage * 4 + i;
        if (k >= e.params) continue;
        sfx::formatParam(effect, k, params[effect][k], v, sizeof(v));
        b.set(i, e.p[k].name, v);
    }
    if (e.params > 4) b.set(4, "More");
    b.set(5, "Back");
    b.set(6, previewPlaying ? "Stop" : "Preview");
    b.set(7, "Apply");
    b.highlightOnly(paramFocus - paramPage * 4);
}

void EditorScreen::markerDelta(int which, int d) {
    const int st = view.pixelUnits();   // one pixel of the current view (spec 06 §6.3)
    if (which == 0) {
        startM = uint16_t(clampi(startM + d * st, 0, endM - 1));
        view.centre = startM;
    } else {
        endM = uint16_t(clampi(endM + d * st, startM + 1, 65535));
        view.centre = endM;
    }
}

void EditorScreen::paramDelta(int d) {
    const sfx::EffectDesc& e = sfx::effect(effect);
    if (paramFocus >= e.params) return;
    float& v = params[effect][paramFocus];
    v = sfx::clampParam(effect, paramFocus, v + float(d) * e.p[paramFocus].step);
    previewState = 0;   // any change -> back to "dirty" (spec 06 §6.4)
    stopPreview();
}

void EditorScreen::onButton(uint8_t id, uint8_t state) {
    App& a = app_;
    if (id == btn::Instr) {
        // spec 06 §6.3: press opens the instrument chooser; release restarts on a new one.
        if (state == kPress) {
            ui_.picker.open(2, "Instrument", kSampleInstrumentCount, a.currentInstrument() < 48 ? a.currentInstrument() : 0,
                            [](void* ctx, int i, char* out, int cap) {
                                App* ap = static_cast<App*>(ctx);
                                std::snprintf(out, size_t(cap), "%02d %s", i + 1, ap->instrPresent[i] ? ap->instruments[i].name : "");
                            },
                            &a);
        } else if (state == kRelease && ui_.picker.isOpen()) {
            const int ni = ui_.picker.cursor();
            ui_.picker.close();
            if (ni != a.currentInstrument()) {
                a.setCurrentInstrument(ni);
                start(0);
            }
        }
        return;
    }
    if (id == btn::SampleEditor) {   // own key re-binds (spec 06 §6.3)
        if (state == kPress && !inert) refreshBar();
        return;
    }
    if (state != kPress || inert) {
        if (id == btn::Play && state == kPress) Screen::onButton(id, state);
        return;
    }
    const sfx::EffectDesc& e = sfx::effect(effect);
    if (page == 1) {
        switch (id) {
            case btn::F1: focus = 0; view.centre = startM; break;
            case btn::F2: focus = 1; view.centre = endM; break;
            case btn::F3: focus = 2; break;
            case btn::F4: undo(); break;
            case btn::F6:
                if (e.params == 0) {
                    apply();
                } else {
                    page = 2;
                    paramPage = 0;
                    paramFocus = 0;
                    previewState = 0;
                }
                break;
            case btn::F7:
            case btn::F8:
                if (focus == 3) effect = clampi(effect + (id == btn::F8 ? 1 : -1), 0, sfx::kEffectCount - 1);
                focus = 3;
                break;
            case btn::Left: focus = clampi(focus - 1, 0, 3); break;
            case btn::Right: focus = clampi(focus + 1, 0, 3); break;
            // Up / Down (spec 06 §6.3): markers / zoom +-1, but on the effect
            // list (drawn top to bottom) Up = previous, Down = next effect.
            case btn::Up: onEncoder(focus == 3 ? -1 : 1); return;
            case btn::Down: onEncoder(focus == 3 ? 1 : -1); return;
            case btn::Delete:
                if (focus == 0) startM = 0;
                else if (focus == 1) endM = 65535;
                else if (focus == 2) view.spp = 0;
                break;
            default: Screen::onButton(id, state); break;
        }
        refreshBar();
        return;
    }
    // Page 2 (spec 06 §6.4).
    switch (id) {
        case btn::F1:
        case btn::F2:
        case btn::F3:
        case btn::F4:
            if (paramPage * 4 + id < e.params) paramFocus = paramPage * 4 + id;
            break;
        case btn::F5:
            if (e.params > 4) {
                const int pages = (e.params + 3) / 4;
                paramPage = (paramPage + 1) % pages;
                paramFocus = paramPage * 4;
            }
            break;
        case btn::F6:
            stopPreview();
            page = 1;
            break;
        case btn::F7:
            if (previewPlaying) stopPreview();
            else preview();
            break;
        case btn::F8: apply(); return;
        case btn::Left:
        case btn::Right:
            paramFocus = clampi(paramFocus + (id == btn::Right ? 1 : -1), 0, e.params - 1);
            paramPage = paramFocus / 4;
            break;
        case btn::Up: paramDelta(1); break;
        case btn::Down: paramDelta(-1); break;
        case btn::Delete:
            if (paramFocus < e.params) {
                params[effect][paramFocus] = e.p[paramFocus].def;
                previewState = 0;
                stopPreview();
            }
            break;
        default: Screen::onButton(id, state); break;
    }
    refreshBar();
}

#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
bool EditorScreen::encoderOnList() const {
    return !inert && page != 2 && focus == 3;   // effect list
}
#endif

void EditorScreen::onEncoder(int d) {
    if (inert) return;
    if (page == 2) {
        paramDelta(d);
    } else {
        switch (focus) {
            case 0: markerDelta(0, d); break;
            case 1: markerDelta(1, d); break;
            case 2: view.step(d); break;
            default: effect = clampi(effect + clampi(d, -5, 5), 0, sfx::kEffectCount - 1); break;
        }
    }
    refreshBar();
}

void EditorScreen::draw(Gfx& g) {
    App& a = app_;
    const Theme& th = ui_.th();
    const uint8_t ci = a.currentInstrument();
    const sfx::EffectDesc& e = sfx::effect(effect);
    char title[48], sub[48];
    if (page == 2) std::snprintf(title, sizeof(title), "Sample Editor  %s", e.name);
    else std::snprintf(title, sizeof(title), e.params ? "Sample Editor 1/2" : "Sample Editor");
    const bool sample = ci < kSampleInstrumentCount && a.bank.loaded(ci);
    if (ci < kSampleInstrumentCount) std::snprintf(sub, sizeof(sub), "%d. %s", ci + 1, a.instruments[ci].name);
    else if (isSynthInstrument(ci)) synthui::slotLabel(a, synthSlotOf(ci), sub, sizeof(sub));
    else std::snprintf(sub, sizeof(sub), "MIDI instrument");
    ui_.drawTitle(g, title, sub);
    if (!sample) {
        g.text(400, 220, kFontText, th.dim,
               ci < kSampleInstrumentCount ? "Empty instrument" : (isSynthInstrument(ci) ? "Synth instrument" : "MIDI instrument"),
               IDisplay::kOptCenter);
        return;
    }
    float v0, v1;
    view.window(&v0, &v1);
    const uint32_t len = a.bank.frames(ci);
    const uint32_t wave = th.slot(slot::kWave);
    if (page == 2) {
        // Parameter bars in the column panels (spec 06 §6.2); the selection
        // preview on the right. SPEC GAP: no stock capture of page 2.
        for (int i = 0; i < 4; ++i) {
            const int k = paramPage * 4 + i;
            const int x = wg::columnX(i);
            if (k >= e.params) {
                wg::panel(g, th, x);
                continue;
            }
            const sfx::ParamDesc& pd = e.p[k];
            wg::ValueBarStyle st;
            st.bipolar = pd.min < 0 && pd.max > 0;
            float f = (params[effect][k] - pd.min) / (pd.max - pd.min);
            if (st.bipolar) f = f * 2.0f - 1.0f;
            else if (f < 1.0f / 369.0f) f = 1.0f / 369.0f;   // value 0 = the 1.5-px line [Part 2 §7.4]
            wg::valueBar(g, th, x, wg::kPanelY, wg::columnW(), wg::kPanelH, f, st);
            if (k == paramFocus) wg::panelFocus(g, th, x, wg::columnW());
        }
        waveRegion(g, th, 401, 798, kWaveY, kWaveH);
        if (a.bank.stereo(ci)) {   // L over R, like the main page
            drawWaveData(g, th, a.bank.left(ci), len, v0, v1, 401, kWaveY, 398, 109, wave);
            drawWaveData(g, th, a.bank.right(ci), len, v0, v1, 401, kWaveY + 110, 398, 109, wave);
        } else {
            drawWaveData(g, th, a.bank.left(ci), len, v0, v1, 401, kWaveY, 398, kWaveH, wave);
        }
        if (previewPlaying) g.text(600, 370, kFontText, th.dim, "Playing", IDisplay::kOptCenter);
        const float hf = playheadFrac();
        if (hf >= 0.0f) {
            const int hx = 401 + int((hf - v0) / (v1 - v0) * 397.0f + 0.5f);
            if (hx >= 401 && hx <= 798) g.vline(hx, kWaveY, kWaveH, th.good);
        }
        return;
    }
    auto mx = [&](uint16_t m) { return int((float(m) / 65535.0f - v0) / (v1 - v0) * 599.0f + 0.5f); };
    const int xs = mx(startM), xe = mx(endM);
    const int a0 = clampi(xs, 0, 599), a1 = clampi(xe, 0, 599);
    // Selection strip, waveform, then the Start (red while selected) and End
    // markers with their flags [S snap 006].
    if (xe >= 0 && xs <= 599) waveRegion(g, th, a0, a1, kWaveY, kWaveH);
    if (a.bank.stereo(ci)) {
        drawWaveData(g, th, a.bank.left(ci), len, v0, v1, 0, kWaveY, 600, 109, wave);
        drawWaveData(g, th, a.bank.right(ci), len, v0, v1, 0, kWaveY + 110, 600, 109, wave);
    } else {
        drawWaveData(g, th, a.bank.left(ci), len, v0, v1, 0, kWaveY, 600, kWaveH, wave);
    }
    if (xs >= 0 && xs < 600) waveMarker(g, th, xs, focus == 0 ? th.accent : wave, 1, true, false, kMarkTop, kMarkBottom);
    if (xe >= 0 && xe < 600) waveMarker(g, th, xe, focus == 1 ? th.accent : wave, -1, true, false, kMarkTop, kMarkBottom);
    {   // Preview playhead (the Playback screen's look: a 1-px line, th.good).
        const float hf = playheadFrac();
        if (hf >= 0.0f) {
            const int hx = int((hf - v0) / (v1 - v0) * 599.0f + 0.5f);
            if (hx >= 0 && hx < 600) g.vline(hx, kWaveY, kWaveH, th.good);
        }
    }
    // Effect list (601, 29, 197, 394): 14 visible rows of 17.
    wg::ListView v;
    v.x = kListX;
    v.w = kListW;
    v.count = sfx::kEffectCount;
    v.sel = effect;
    v.top = wg::listFollow(effect, v.count, wg::listVisibleRows(v.h), 0);
    v.selStyle = wg::ListSel::kWhite;
    v.item = effectItem;
    wg::list(g, th, v);
    if (focus == 3) wg::panelFocus(g, th, kListX, kListW);
    if (inert) return;
    const BottomBar& b = ui_.bar;
    if (b.label[6].hidden && !ui_.askOpen()) barSelector(g, th, 6, b.label[6].top, 57);
    if (b.label[5].hidden) barGreyLabel(g, th, 5, b.label[5].top, b.label[5].bottom);
}

}  // namespace ot
