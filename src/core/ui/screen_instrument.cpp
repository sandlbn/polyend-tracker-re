// Instrument parameters (spec 05 §2.7 and appendix C): Parameters 1/2,
// Effects, Automation 2/2, MIDI Mapping and MIDI instrument pages.
// Sample-instrument values go through app/instrument_edit (shared with the
// MIDI CC map, appendix C §8).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "app/instrument_edit.h"
#include "app/midi_cc.h"
#include "ui/screens.h"

namespace ot {

// Bar glyphs (wave.cpp).
void barSelector(Gfx& g, const Theme& th, int i, const char* title, int d);

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Stock value labels (docs/STRINGS_DIFF_1.9.3.md §2).
const char* kFilterNames[4] = {"Disabled", "Low-pass", "High-pass", "Band-pass"};
const char* kDestNames[6] = {"Volume", "Panning", "Cutoff", "Wavetable Position", "Granular Position", "Finetune"};
const char* kTypeNames[3] = {"Off", "Envelope", "LFO"};
const char* kShapeNames[5] = {"Rev saw", "Saw", "Triangle", "Square", "Random"};
// "128 steps" ... "1/64 step" (stock table 0xa69cc); plural above one step
// is assumed for the entries between the two listed ends (README).
const char* kSpeedNames[29] = {"128 steps", "96 steps", "64 steps", "48 steps", "32 steps", "24 steps",
                               "16 steps",  "12 steps", "8 steps",  "6 steps",  "4 steps",  "3 steps",
                               "2 steps",   "3/2 steps", "1 step",  "3/4 step", "1/2 step", "3/8 step",
                               "1/3 step",  "1/4 step", "3/16 step", "1/6 step", "1/8 step", "1/12 step",
                               "1/16 step", "1/24 step", "1/32 step", "1/48 step", "1/64 step"};

// Column -> parameter for the sample-instrument pages (appendix C §4-§6).
const InstrParam kParamCols[7] = {InstrParam::Volume,     InstrParam::Panning, InstrParam::Tune,
                                  InstrParam::Finetune,   InstrParam::FilterType, InstrParam::Cutoff,
                                  InstrParam::Resonance};
const InstrParam kFxCols[4] = {InstrParam::Overdrive, InstrParam::BitDepth, InstrParam::ReverbSend,
                               InstrParam::DelaySend};
// Automation columns 1..6 for an envelope; an LFO uses columns 1..4.
const InstrParam kEnvCols[7] = {InstrParam::AutoType,    InstrParam::AutoType,    InstrParam::AutoAttackShape,
                                InstrParam::AutoDecaySpeed, InstrParam::AutoSustain, InstrParam::AutoRelease,
                                InstrParam::AutoAmount};
const InstrParam kLfoCols[5] = {InstrParam::AutoType, InstrParam::AutoType, InstrParam::AutoAttackShape,
                                InstrParam::AutoDecaySpeed, InstrParam::AutoAmount};

constexpr int kMapRows = 11;          // visible list rows (335 px / ~30 px)
constexpr int kMapRowH = 30;

void instrItem(void* ctx, int i, char* out, int cap) { instrumentChooserItem(ctx, i, out, cap); }

// Stock column layout (snap 011 / 042, VISUAL_DIFF §3.10, §8.26): 8 column
// panels, white 76-px value bars, value 0 = the 1.5-px line at the bottom
// (one fill row), bipolar parameters as a line around the centre, list
// columns (Filter Type, automation Destination / Type / LFO Shape / Speed)
// as lists with a white row frame, the selected column in a red frame.
constexpr float kMinBar = 1.0f / float(wg::kPanelH - 25);

void listItem(void* ctx, int i, char* out, int cap) {
    std::snprintf(out, size_t(cap), "%s", static_cast<const char* const*>(ctx)[i]);
}

void barColumn(Gfx& g, const Theme& th, int col, float frac, bool bipolar, bool enabled) {
    wg::ValueBarStyle st;
    st.bipolar = bipolar;
    if (!enabled) st.fill = th.slot(slot::kBottomLabelOff);
    float v = bipolar ? frac * 2.0f - 1.0f : (frac < kMinBar ? kMinBar : frac);
    wg::valueBar(g, th, wg::columnX(col), wg::kPanelY, wg::columnW(), wg::kPanelH, v, st);
}

// "More" label with page dots (stock 011: title at the two-line title row,
// 7-px dots 13 px apart from cx - 6, the other page in the label background
// colour). Local stand-in for wg::pageDots, which spaces them 14 px and uses
// slot kBottomLabel + 17.
void moreLabel(Gfx& g, const Theme& th, int i, const char* title, int pages, int page) {
    const int cx = wg::labelCentre(i);
    g.text(cx, wg::kLabelY1, kFont3, th.slot(slot::kBottomLabel), title, IDisplay::kOptCenter);
    const int x0 = cx - 6 - (pages - 2) * 13 / 2;
    for (int p = 0; p < pages; ++p)
        wg::dot(g, x0 + 13 * p, wg::kDotY, 3.0f, th.slot(p == page ? slot::kBottomLabel : slot::kBottomLabel + 1));
}

void listColumn(Gfx& g, const Theme& th, int col, int span, const char* const* items, int count, int sel) {
    wg::ListView v;
    v.x = wg::columnX(col);
    v.w = wg::columnW(span);
    v.count = count;
    v.sel = sel;
    v.top = wg::listFollow(sel, count, wg::listVisibleRows(v.h), 0);
    v.selStyle = wg::ListSel::kWhite;
    v.item = listItem;
    v.ctx = const_cast<const char**>(items);
    wg::list(g, th, v);
}

// MIDI instrument CC A-F: 131-item lists of CC names (spec 19 §4, manual
// p.256-261; MANUAL_AUDIT_2026-10-23 #6).
void ccItem(void*, int i, char* out, int cap) { midicc::ccValueName(i, out, cap); }
void ccListColumn(Gfx& g, const Theme& th, int col, int sel) {
    wg::ListView v;
    v.x = wg::columnX(col);
    v.w = wg::columnW(1);
    v.count = midicc::kCcValueCount;
    v.sel = sel;
    v.top = wg::listFollow(sel, v.count, wg::listVisibleRows(v.h), 0);
    v.selStyle = wg::ListSel::kWhite;
    v.item = ccItem;
    wg::list(g, th, v);
}

int firstRow(int sel, int count) {
    int f = sel - kMapRows / 2;
    if (f > count - kMapRows) f = count - kMapRows;
    return f < 0 ? 0 : f;
}
}  // namespace

void InstrumentScreen::start(int opt) {
    const uint8_t ci = app_.currentInstrument();
    // Appendix C §1: MIDI instruments force mode 3; re-entry keeps only
    // modes 0/1 (MIDI Mapping is entered from the Effects page).
    if (isMidiInstrument(ci)) mode = 3;
    else if (opt == 2) mode = 1;
    else if (mode == 3 || mode == 2) mode = 0;
    effects = false;
    column = clampi(column, 0, columns() - 1);
    // Appendix C §6 entry fix-up: destination Off -> column <= 1.
    if (mode == 1 && ci < kSampleInstrumentCount && iedit::autoType(app_.instruments[ci], dest) == 0 && column > 1)
        column = 1;
    refreshBar();
}

bool InstrumentScreen::selectable(int c) const {
    // Appendix C §3: mode 1 columns > 1 unselectable while the type is Off,
    // > 4 while it is an LFO; mode 0 Cutoff/Resonance only with a filter.
    const uint8_t ci = app_.currentInstrument();
    if (c < 0 || c >= columns()) return false;
    if (ci >= kSampleInstrumentCount) return true;
    const Instrument& in = app_.instruments[ci];
    if (mode == 1) {
        const int t = iedit::autoType(in, dest);
        return t == 0 ? c <= 1 : (t == 2 ? c <= 4 : true);
    }
    if (mode == 0 && !effects && c >= 5) return in.filterEnabled != 0;
    return true;
}

int InstrumentScreen::columns() const {
    if (mode == 3) return 8;
    if (mode == 2) return 2;
    if (mode == 1) return 7;
    return effects ? 4 : 7;
}

void InstrumentScreen::valueText(int c, char* out, int cap, float* frac, bool* bipolar) const {
    *bipolar = false;
    *frac = 0;
    const uint8_t ci = app_.currentInstrument();
    if (mode == 3) {
        MidiInstrument m = app_.project.midiInstrument(ci - kFirstMidiInstrument);
        // Appendix C §7: col0 Velocity (rec+0), col1 Offset (rec+7 = the
        // sequencer's note-delay byte, spec 13 §I.1), col2..7 CC A..F.
        if (c == 0) {
            std::snprintf(out, size_t(cap), "%d", m.velocity);
            *frac = float(m.velocity) / 127.0f;
        } else if (c == 1) {
            const int o = m.extra[0] > 100 ? 100 : m.extra[0];
            std::snprintf(out, size_t(cap), "%d", o);
            *frac = float(o) / 100.0f;
        } else {
            const int v = m.cc[c - 2] > 130 ? 130 : m.cc[c - 2];
            // Appendix C §7 list 0xa6750: CC number + role name, PC, AT.
            midicc::ccValueName(m.cc[c - 2], out, cap);
            *frac = float(v) / 130.0f;
        }
        return;
    }
    const Instrument& in = app_.instruments[ci < kSampleInstrumentCount ? ci : 0];
    if (mode == 1) {
        const Envelope& e = in.env[dest];
        const Lfo& l = in.lfo[dest];
        const int type = e.enabled ? (e.loop ? 2 : 1) : 0;
        switch (c) {
            case 0: std::snprintf(out, size_t(cap), "%s", kDestNames[dest]); *frac = float(dest) / 5.0f; return;
            case 1: std::snprintf(out, size_t(cap), "%s", kTypeNames[type]); *frac = float(type) / 2.0f; return;
            default: break;
        }
        if (type == 2) {
            if (c == 2) { std::snprintf(out, size_t(cap), "%s", kShapeNames[l.shape % 5]); *frac = float(l.shape) / 4.0f; }
            else if (c == 3) {
                const int idx = dest == 0 ? l.speed + 5 : l.speed;
                std::snprintf(out, size_t(cap), "%s", kSpeedNames[clampi(idx, 0, 28)]);
                *frac = float(l.speed) / (dest == 0 ? 23.0f : 28.0f);
            } else if (c == 4) { std::snprintf(out, size_t(cap), "%.0f", double(l.amount * 100)); *frac = l.amount; }
            else out[0] = 0;
            return;
        }
        if (type == 0) {
            out[0] = 0;
            return;
        }
        int ms = 0;
        switch (c) {
            case 2: ms = e.attackMs; break;
            case 3: ms = e.decayMs; break;
            case 5: ms = e.releaseMs; break;
            case 4: std::snprintf(out, size_t(cap), "%.0f", double(e.sustain * 100)); *frac = e.sustain; return;
            case 6: std::snprintf(out, size_t(cap), "%.0f", double(e.amount * 100)); *frac = e.amount; return;
        }
        std::snprintf(out, size_t(cap), "%.3fs", double(ms) / 1000.0);
        *frac = ms <= 1000 ? float(ms) * 0.5f / 1000.0f : 0.5f + float(ms - 1000) * 0.5f / 9000.0f;
        return;
    }
    if (effects) {
        switch (c) {
            case 0: std::snprintf(out, size_t(cap), "%d", in.overdrive); *frac = in.overdrive / 100.0f; break;
            case 1: std::snprintf(out, size_t(cap), "%d", in.bitDepth); *frac = (in.bitDepth - 4) / 12.0f; break;
            case 2:
            case 3: {
                const int v = c == 2 ? in.reverbSend : in.delaySend;
                if (v == 0) std::snprintf(out, size_t(cap), "-inf dB");
                else std::snprintf(out, size_t(cap), "%.2f dB", double((v - 100) * 0.4f));
                *frac = v / 100.0f;
                break;
            }
        }
        return;
    }
    switch (c) {
        case 0:
            if (in.volume == 0) std::snprintf(out, size_t(cap), "-inf dB");
            else std::snprintf(out, size_t(cap), "%.2f dB", double((in.volume - 50) * 0.48f));
            *frac = in.volume / 100.0f;
            break;
        case 1:
            std::snprintf(out, size_t(cap), "%d", in.panning - 50);
            *frac = in.panning / 100.0f;
            *bipolar = true;
            break;
        case 2:
            std::snprintf(out, size_t(cap), "%d", in.tune);
            *frac = 0.5f + in.tune / 48.0f;
            *bipolar = true;
            break;
        case 3:
            std::snprintf(out, size_t(cap), "%d", in.finetune);
            *frac = 0.5f + in.finetune / 200.0f;
            *bipolar = true;
            break;
        case 4: {
            const int ft = in.filterEnabled ? in.filterType + 1 : 0;
            std::snprintf(out, size_t(cap), "%s", kFilterNames[clampi(ft, 0, 3)]);
            *frac = ft / 3.0f;
            break;
        }
        case 5:
            std::snprintf(out, size_t(cap), "%d", int(std::lround(in.cutoff * 100)));
            *frac = in.cutoff;
            break;
        case 6:
            std::snprintf(out, size_t(cap), "%d", int(std::lround(in.resonance / 4.3f * 100)));
            *frac = in.resonance / 4.3f;
            break;
    }
}

static InstrParam paramFor(int mode, bool effects, int c, int type) {
    if (mode == 1) return type == 2 ? kLfoCols[c < 5 ? c : 4] : kEnvCols[c < 7 ? c : 6];
    if (effects) return kFxCols[c < 4 ? c : 3];
    return kParamCols[c < 7 ? c : 6];
}

void InstrumentScreen::edit(int c, int d, bool tap) {
    App& a = app_;
    const uint8_t ci = a.currentInstrument();
    if (mode == 2) return;
    if (mode == 3) {
        const int idx = ci - kFirstMidiInstrument;
        MidiInstrument m = a.project.midiInstrument(idx);
        if (c == 0) m.velocity = uint8_t(clampi(m.velocity + d, 0, 127));
        else if (c == 1) m.extra[0] = uint8_t(clampi(clampi(m.extra[0], 0, 100) + d, 0, 100));
        else m.cc[c - 2] = uint8_t(clampi(clampi(m.cc[c - 2], 0, 130) + clampi(d, -10, 10), 0, 130));
        a.project.setMidiInstrument(idx, m);
        a.markProjectDirty();
        return;
    }
    if (ci >= kSampleInstrumentCount) return;
    Instrument& in = a.instruments[ci];
    bool changed = false;
    if (mode == 1) {
        if (c == 0) {
            dest = clampi(dest + clampi(d, -3, 3), 0, 5);   // setter 0x2e168 clamps 0..5
            return;
        }
        const InstrParam p = paramFor(1, false, c, iedit::autoType(in, dest));
        if (p == InstrParam::AutoDecaySpeed && iedit::autoType(in, dest) == 2) d = clampi(d, -5, 5);
        // Type / LFO shape / LFO speed are lists: a tap wraps.
        changed = iedit::step(in, p, dest, d, tap);
    } else {
        const InstrParam p = paramFor(0, effects, c, 0);
        if (p == InstrParam::Tune && (d == 10 || d == -10)) d = d > 0 ? 12 : -12;   // appendix C col2
        changed = iedit::step(in, p, 0, d, tap && p == InstrParam::FilterType);
    }
    if (changed) a.markInstrumentDirty(ci);
}

void InstrumentScreen::reset(int c) {
    App& a = app_;
    const uint8_t ci = a.currentInstrument();
    if (mode == 2) return;
    if (mode == 3) {
        const int idx = ci - kFirstMidiInstrument;
        MidiInstrument m = a.project.midiInstrument(idx), def;
        if (c == 0) m.velocity = def.velocity;           // reset 127
        else if (c == 1) m.extra[0] = 0;                  // Offset reset 0
        else m.cc[c - 2] = def.cc[c - 2];
        a.project.setMidiInstrument(idx, m);
        a.markProjectDirty();
        return;
    }
    if (ci >= kSampleInstrumentCount) return;
    Instrument& in = a.instruments[ci];
    if (mode == 1) {
        // Appendix C §6: copy the single field of the selected destination
        // from the default image.
        if (c >= 1) {
            const InstrParam p = paramFor(1, false, c, iedit::autoType(in, dest));
            iedit::set(in, p, dest, iedit::defaultValue(p, dest));
        }
    } else {
        const InstrParam p = paramFor(0, effects, c, 0);
        if (p == InstrParam::FilterType) {
            in.filterEnabled = 0;
            in.filterType = 0;
        } else if (p == InstrParam::Cutoff) {
            in.cutoff = 1.0f;
        } else if (p == InstrParam::Resonance) {
            in.resonance = 0;
        } else {
            iedit::set(in, p, 0, iedit::defaultValue(p));
        }
    }
    a.markInstrumentDirty(ci);
}

void InstrumentScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    b.clear();
    char v[24];
    float fr;
    bool bi;
    static const char* kP[7] = {"Volume", "Panning", "Tune", "Finetune", "Filter Type", "Cutoff", "Resonance"};
    static const char* kE[4] = {"Overdrive", "Bit depth", "Reverb", "Delay"};
    static const char* kEnv[7] = {"Destination", "Type", "Attack", "Decay", "Sustain", "Release", "Amount"};
    static const char* kLfo[7] = {"Destination", "Type", "Shape", "Speed", "Amount", "", ""};
    static const char* kMidi[8] = {"Velocity", "Offset", "CC A", "CC B", "CC C", "CC D", "CC E", "CC F"};
    if (mode == 2) {
        // Appendix C §8 bottom labels.
        b.set(1, "CC List");
        b.set(5, "Instruments");
        b.set(7, "Back");
        b.highlight(1, column == 0);
        b.highlight(5, column == 1);
        return;
    }
    // The selected column's red frame marks the selection: no label frame,
    // 050505 strip [S snap 011, Part 2 C5].
    b.stripBg = wg::mix(0x000000, ui_.th().slot(slot::kStrip + 1), 128);
    if (mode == 3) {
        for (int i = 0; i < 8; ++i) {
            valueText(i, v, sizeof(v), &fr, &bi);
            b.set(i, kMidi[i], v);
        }
        return;
    }
    if (mode == 1) {
        const Instrument& in = app_.instruments[app_.currentInstrument() % kSampleInstrumentCount];
        const int type = in.env[dest].enabled ? (in.env[dest].loop ? 2 : 1) : 0;
        b.setWide(0, "Destination");   // "▲ Destination ▼": arrows drawn by draw() [Part 2 §8.26]
        for (int c = 1; c < 7; ++c) {
            valueText(c, v, sizeof(v), &fr, &bi);
            const char* nm = type == 2 ? kLfo[c] : kEnv[c];
            b.set(c + 1, nm, v, c == 1 || (type == 1) || (type == 2 && c <= 4));
        }
        return;
    }
    if (effects) {
        for (int c = 0; c < 4; ++c) {
            valueText(c, v, sizeof(v), &fr, &bi);
            b.set(c, kE[c], v);
        }
        b.set(4, "MIDI Map");   // appendix C §5 button 4 "Instrument / MIDI Mapping"
        b.set(7, "Back");
        return;
    }
    const Instrument& in = app_.instruments[app_.currentInstrument() % kSampleInstrumentCount];
    for (int c = 0; c < 7; ++c) {
        valueText(c, v, sizeof(v), &fr, &bi);
        b.set(c, kP[c], v, c < 5 || in.filterEnabled);
    }
    // "More" with two page dots, drawn by draw() (moreLabel) [S snap 011].
    b.set(7, "More");
    b.label[7].hidden = true;
}

void InstrumentScreen::onButton(uint8_t id, uint8_t state) {
    App& a = app_;
    if (id == btn::InstrParams) {
        // Own module key toggles Parameters <-> Automation (appendix C §1).
        if (state == kPress && mode != 3) {
            mode = mode == 0 ? 1 : 0;
            effects = false;
            column = 0;
        }
        refreshBar();
        return;
    }
    if (mode == 2) {
        // Appendix C §3 mode 2: Instr/Note/Delete/Rec and friends disabled.
        if (state == kPress) onMappingButton(id);
        refreshBar();
        return;
    }
    if (id == btn::Instr) {
        if (state == kPress) {
            ui_.picker.open(2, "Instrument", kInstrumentChooserCount, a.currentInstrument(), instrItem, &a);
        } else if (state == kRelease && ui_.picker.isOpen()) {
            a.setCurrentInstrument(uint8_t(ui_.picker.cursor()));
            ui_.picker.close();
            if (isSynthInstrument(a.currentInstrument())) {   // 2.x: the synth slot's page
                ui_.switchTo(kModInstrument);
                return;
            }
            const bool fx = effects;
            start(0);
            effects = fx && mode == 0;
        }
        refreshBar();
        return;
    }
    if (ui_.picker.isOpen()) return;
    if (btn::isFKey(id)) {
        const int f = id;
        if (state == kPress) {
            if (mode == 0 && !effects && f == 7) {
                effects = true;
                column = 0;
            } else if (mode == 0 && effects && f == 7) {
                effects = false;
                column = 0;
            } else if (mode == 0 && effects && f == 4) {
                // Appendix C §3 Effects button 4 -> MIDI Mapping (mode 2); it
                // keeps its own instrument index (+0x335), seeded from a
                // sample instrument (§1).
                mode = 2;
                effects = false;
                column = 0;
                if (a.currentInstrument() < kSampleInstrumentCount) a.ccMapInstrument = a.currentInstrument();
                ccScroll = clampi(ccScroll, 0, midicc::kCcListCount - 1);
            } else {
                int c = mode == 1 ? (f <= 1 ? 0 : f - 1) : f;
                if (selectable(c)) {
                    // A tap on the already selected list column steps it with wrap.
                    const Instrument& ins = a.instruments[a.currentInstrument() % kSampleInstrumentCount];
                    const bool lfo = ins.env[dest].enabled && ins.env[dest].loop;
                    const bool listCol = mode == 0 ? (!effects && c == 4)
                                                   : mode == 1 ? (c == 1 || (lfo && (c == 2 || c == 3))) : false;
                    if (mode == 1 && c == 0 && column == 0) {
                        // P23: F1 = previous, F2 = next destination (clamped 0..5).
                        dest = clampi(dest + (f == 0 ? -1 : 1), 0, 5);
                        if (iedit::autoType(ins, dest) == 0 && column > 1) column = 1;
                    } else if (c == column && listCol) {
                        edit(c, 1, true);
                    }
                    column = c;
                    held_[f] = true;
                    usedWhileHeld_[f] = false;
                }
            }
        } else if (state == kRelease) {
            held_[f] = false;
        }
        refreshBar();
        return;
    }
    if (state != kPress) {
        if (id == btn::EncPush && state == kRelease) a.previewOff(App::kAuditionInstrument);
        return;
    }
    switch (id) {
        case btn::Left:
            if (selectable(column - 1)) --column;
            break;
        case btn::Right:
            if (selectable(column + 1)) ++column;
            break;
        case btn::Up:
        case btn::Down: {
            // Appendix C §3: list columns (filter type, LFO speed, CC choice)
            // invert the button direction so that Up moves up in the list.
            const uint8_t ci = a.currentInstrument();
            bool inv = false;
            if (mode == 0 && !effects) inv = column == 4;
            // Automation 2/2: Destination, Type and the LFO Shape are
            // top-down lists too (user reports: Up moved down in them).
            else if (mode == 1 && ci < kSampleInstrumentCount)
                inv = column <= 1 || ((column == 2 || column == 3) && iedit::autoType(a.instruments[ci], dest) == 2);
            else if (mode == 3) inv = column >= 2;
            const int d = id == btn::Up ? 1 : -1;
            edit(column, inv ? -d : d, false);
            break;
        }
        case btn::Delete: reset(column); break;
        case btn::EncPush: a.previewOn(App::kAuditionInstrument, a.currentInstrument(), auditionNote(), 127); break;
        default: Screen::onButton(id, state); break;
    }
    // Filter disabled: Cutoff/Resonance columns snap back to Filter type.
    if (mode == 0 && !effects && column >= 5 && a.currentInstrument() < kSampleInstrumentCount &&
        !a.instruments[a.currentInstrument()].filterEnabled)
        column = 4;
    refreshBar();
}

uint8_t InstrumentScreen::auditionNote() const {
    // 0x30138: pad 12's note; a Beat Slice instrument plays note 24 +
    // selected slice instead.
    const uint8_t ci = app_.currentInstrument();
    if (ci < kSampleInstrumentCount && app_.instruments[ci].playMode == uint8_t(PlayMode::BeatSlice))
        return uint8_t(24 + app_.instruments[ci].selectedSlice);
    return app_.padNote(12);
}

#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
bool InstrumentScreen::encoderOnList() const {
    if (mode == 2) return true;   // CC mapping list
    if (mode == 0) return !effects && column == 4;   // filter type
    if (mode == 3) return column >= 2;
    const uint8_t ci = app_.currentInstrument();
    if (ci >= kSampleInstrumentCount) return false;
    // Automation 2/2: Destination, Type; LFO Shape and Speed.
    return column <= 1 || ((column == 2 || column == 3) && iedit::autoType(app_.instruments[ci], dest) == 2);
}
#endif

void InstrumentScreen::onEncoder(int d) {
    if (mode == 2) {
        mappingMove(d);
        return;
    }
    // Several held F-keys edit all their columns at once (appendix C §3).
    bool any = false;
    for (int f = 0; f < 8; ++f) {
        if (!held_[f]) continue;
        const int c = mode == 1 ? (f <= 1 ? 0 : f - 1) : f;
        if (c < columns() && c != column) {
            edit(c, d, false);
            any = true;
        }
    }
    (void)any;
    edit(column, d, false);
    refreshBar();
}

// ---------------------------------------------------------------------------
// MIDI Mapping page (mode 2, appendix C §8)

void InstrumentScreen::mappingMove(int d) {
    App& a = app_;
    if (column == 0) {
        ccScroll = clampi(ccScroll + d, 0, midicc::kCcListCount - 1);
    } else {
        a.ccMapInstrument = uint8_t(clampi(a.ccMapInstrument + d, 0, kSampleInstrumentCount - 1));
        a.setCurrentInstrument(a.ccMapInstrument);
    }
    refreshBar();
}

void InstrumentScreen::onMappingButton(uint8_t id) {
    if (btn::isFKey(id)) {
        const int f = id;
        if (f <= 3) {
            // 0..3: focus the CC list; when focused, 0 = up, 3 = down.
            if (column != 0) column = 0;
            else if (f == 0) mappingMove(-1);
            else if (f == 3) mappingMove(1);
        } else if (f <= 6) {
            // 4..6: focus Instruments; when focused, 4 = previous, 6 = next.
            if (column != 1) column = 1;
            else if (f == 4) mappingMove(-1);
            else if (f == 6) mappingMove(1);
        } else {
            // 7: Back to the Effects page.
            mode = 0;
            effects = true;
            column = 0;
        }
        return;
    }
    switch (id) {
        case btn::InstrParams: mode = 0; effects = false; column = 0; break;   // §1: mode 2 -> 0
        case btn::Left: column = 0; break;
        case btn::Right: column = 1; break;
        case btn::Up: mappingMove(-1); break;     // lists: Up moves up (§3)
        case btn::Down: mappingMove(1); break;
        case btn::Play: Screen::onButton(id, kPress); break;
        default: break;                            // Instr, Note, Delete, Rec, ... disabled
    }
}

void InstrumentScreen::onPad(uint8_t pad, uint8_t state) {
    if (mode != 2) {
        Screen::onPad(pad, state);
        return;
    }
    // Appendix C §3: a pad selects the mapping instrument directly (0..47).
    // SPEC GAP: like the encoder it also becomes the current instrument.
    if (state != kPress) return;
    App& a = app_;
    a.ccMapInstrument = uint8_t(clampi(pad, 0, kSampleInstrumentCount - 1));
    a.setCurrentInstrument(a.ccMapInstrument);
    column = 1;
    refreshBar();
}

void InstrumentScreen::padLeds(uint8_t* lv) {
    // Appendix C §3 "Pad LEDs on entry": mode 2 lights the mapping instrument's pad.
    if (mode == 2) {
        lv[clampi(app_.ccMapInstrument, 0, kSampleInstrumentCount - 1)] = app_.cfg.padFull();
        return;
    }
    // 0x2fea4: a Beat Slice instrument lights one pad per slice.
    const uint8_t ci = app_.currentInstrument();
    if (ci < kSampleInstrumentCount && app_.instruments[ci].playMode == uint8_t(PlayMode::BeatSlice))
        for (int p = 0; p < app_.instruments[ci].sliceCount && p < kPadCount; ++p) lv[p] = app_.cfg.padDim();
}

void InstrumentScreen::drawMapping(Gfx& g) {
    App& a = app_;
    const Theme& th = ui_.th();
    char sub[48], row[48];
    const int mi = clampi(a.ccMapInstrument, 0, kSampleInstrumentCount - 1);
    std::snprintf(sub, sizeof(sub), "%d. %s", mi + 1, a.instrPresent[mi] ? a.instruments[mi].name : "(empty)");
    ui_.drawTitle(g, "Instrument MIDI Mapping", sub);
    // Hint text box (appendix C §2, x=10 y=30 w=780 h=30). SPEC GAP: wording.
    g.text(10, 45, kFontSmall, a.seq.isPlaying() ? th.alert : th.dim,
           "MIDI CC mapping works only while the sequencer is stopped", IDisplay::kOptCenterY);
    // Left pane: CC reference list (x=1 y=80 w=397 h=335).
    g.fill(1, 80, 397, 335, th.panelDark);
    int first = firstRow(ccScroll, midicc::kCcListCount);
    for (int r = 0; r < kMapRows && first + r < midicc::kCcListCount; ++r) {
        const int i = first + r;
        const int y = 80 + 4 + r * kMapRowH;
        if (i == ccScroll) g.fill(3, y, 393, kMapRowH - 2, column == 0 ? th.accent : th.panel2);
        // "Name = CC" (spec 16 §A.13, manual p.265).
        std::snprintf(row, sizeof(row), "%s = %d", midicc::kCcList[i].label, midicc::kCcList[i].cc);
        g.text(12, y + kMapRowH / 2, kFontText, th.text, row, IDisplay::kOptCenterY);
    }
    // Right pane: the 48 sample instruments (x=401 y=80 w=297 h=335).
    g.fill(401, 80, 297, 335, th.panelDark);
    first = firstRow(mi, kSampleInstrumentCount);
    for (int r = 0; r < kMapRows && first + r < kSampleInstrumentCount; ++r) {
        const int i = first + r;
        const int y = 80 + 4 + r * kMapRowH;
        if (i == mi) g.fill(403, y, 293, kMapRowH - 2, column == 1 ? th.accent : th.panel2);
        instrItem(&a, i, row, sizeof(row));
        g.text(412, y + kMapRowH / 2, kFontText, a.instrPresent[i] ? th.text : th.dim, row, IDisplay::kOptCenterY);
    }
    // Selection frame (rect table 0xa69a8).
    if (column == 0) g.frame(1, 80, 397, 342, th.accent, 2);
    else g.frame(401, 80, 297, 342, th.accent, 2);
}

void InstrumentScreen::draw(Gfx& g) {
    App& a = app_;
    // Values may change from outside (MIDI CC, appendix C §8): rebuild the bar.
    refreshBar();
    if (mode == 2) {
        drawMapping(g);
        return;
    }
    const Theme& th = ui_.th();
    const uint8_t ci = a.currentInstrument();
    char title[48], sub[48];
    if (mode == 3) std::snprintf(title, sizeof(title), "MIDI Instrument Parameters");
    else if (mode == 1) std::snprintf(title, sizeof(title), "Instrument Automation 2/2");
    else if (effects) std::snprintf(title, sizeof(title), "Instrument Effects");
    else std::snprintf(title, sizeof(title), "Instrument Parameters 1/2");
    if (ci < kSampleInstrumentCount) std::snprintf(sub, sizeof(sub), "%d. %s", ci + 1, a.instrPresent[ci] ? a.instruments[ci].name : "(empty)");
    else if (isSynthInstrument(ci)) synthui::slotLabel(a, synthSlotOf(ci), sub, sizeof(sub));
    else std::snprintf(sub, sizeof(sub), "%d. MIDI Channel %d", ci + 1, ci - kFirstMidiInstrument + 1);
    ui_.drawTitle(g, title, sub);
    char v[24];
    float fr;
    bool bi;
    bool drawn[8] = {};
    int focusX = -1, focusW = wg::columnW();
    if (mode == 1) {
        // Destination list (columns 0-1), Type list, then the envelope bars
        // or the LFO Shape / Speed lists and Amount bar [S snap 042].
        const Instrument& in = a.instruments[ci % kSampleInstrumentCount];
        const int type = in.env[dest].enabled ? (in.env[dest].loop ? 2 : 1) : 0;
        listColumn(g, th, 0, 2, kDestNames, 6, dest);
        listColumn(g, th, 2, 1, kTypeNames, 3, type);
        drawn[0] = drawn[1] = drawn[2] = true;
        for (int c = 2; c < 7; ++c) {
            const int col = c + 1;
            if (type == 0 || (type == 2 && c > 4)) break;
            drawn[col] = true;
            if (type == 2 && c == 2) {
                listColumn(g, th, col, 1, kShapeNames, 5, in.lfo[dest].shape % 5);
            } else if (type == 2 && c == 3) {
                // Volume's LFO speeds run 24 steps .. 1/64 step: the list
                // starts at "24 steps", nothing above it is listed (stock).
                if (dest == 0) listColumn(g, th, col, 1, kSpeedNames + 5, 24, clampi(in.lfo[0].speed, 0, 23));
                else listColumn(g, th, col, 1, kSpeedNames, 29, clampi(in.lfo[dest].speed, 0, 28));
            } else {
                valueText(c, v, sizeof(v), &fr, &bi);
                barColumn(g, th, col, fr, false, true);
            }
        }
        focusX = column == 0 ? wg::columnX(0) : wg::columnX(column + 1);
        if (column == 0) focusW = wg::columnW(2);
        if (!ui_.askOpen()) barSelector(g, th, 0, nullptr, 67);
    } else {
        const Instrument& in = a.instruments[ci % kSampleInstrumentCount];
        const int n = columns();
        for (int c = 0; c < n; ++c) {
            drawn[c] = true;
            if (mode == 0 && !effects && c == 4) {
                const int ft = in.filterEnabled ? in.filterType + 1 : 0;
                listColumn(g, th, c, 1, kFilterNames, 4, clampi(ft, 0, 3));
                continue;
            }
            if (mode == 3 && c >= 2) {
                const MidiInstrument m = a.project.midiInstrument(ci - kFirstMidiInstrument);
                ccListColumn(g, th, c, clampi(m.cc[c - 2], 0, midicc::kCcValueCount - 1));
                continue;
            }
            valueText(c, v, sizeof(v), &fr, &bi);
            barColumn(g, th, c, fr, bi, selectable(c));
        }
        focusX = wg::columnX(column);
        if (mode == 0 && !effects && ui_.bar.label[7].hidden) moreLabel(g, th, 7, ui_.bar.label[7].top, 2, 0);
    }
    for (int c = 0; c < 8; ++c)
        if (!drawn[c]) wg::panelColumn(g, th, c);
    if (focusX >= 0) wg::panelFocus(g, th, focusX, focusW);
}

}  // namespace ot
