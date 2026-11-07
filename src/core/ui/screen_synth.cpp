// Synth slot screens (2.x WP-G): Synth Parameters 1/2, Synth Macros 2/2,
// Edit Patch and the patch browser, laid out after the Tracker Mini manual
// p.147-153 and drawn with the stock 1.9.2 chrome (spec 20, widgets.h) [OT].
//
// Every edit applies live: records go through App::setSynthSlotRecord (the
// engine sync pushes them to SynthBank), patch values through synthParamSet
// + App::markSynthPatchDirty (SynthBank::patchChanged; the autosave writes
// Workspace/N.patch).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "app/app.h"
#include "audio/synth_engine.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "sdram_bss.h"
#include "modules/module.h"
#include "modules/synth_models.h"
#include "ui/screens.h"

namespace ot {

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The model list (F1): the five Mini models, then module engines (modules/synth_models.h).
int modelNames(const char** out) {
    const int n = synthModelListCount();
    for (int i = 0; i < n && i < kMaxSynthModels; ++i) out[i] = synthModelListName(i);
    return n < kMaxSynthModels ? n : kMaxSynthModels;
}
const char* const kOffOn[2] = {"Off", "On"};
constexpr float kMinBar = 1.0f / float(wg::kPanelH - 25);   // value 0: the 1.5-px line (screen_instrument)
constexpr int kMacroBarH = 270;                              // Macros page: bars over the legend
constexpr int kLegendY = 303;

void namesItem(void* ctx, int i, char* out, int cap) {
    std::snprintf(out, size_t(cap), "%s", static_cast<const char* const*>(ctx)[i]);
}

void barColumn(Gfx& g, const Theme& th, int col, float v, bool bipolar, bool enabled, int h = wg::kPanelH,
               uint32_t fill = wg::kThemeColour) {
    wg::ValueBarStyle st;
    st.bipolar = bipolar;
    st.fill = enabled ? fill : th.slot(slot::kBottomLabelOff);
    if (!bipolar && v < kMinBar) v = kMinBar;
    wg::valueBar(g, th, wg::columnX(col), wg::kPanelY, wg::columnW(), h, v, st);
}

void listColumn(Gfx& g, const Theme& th, int col, int span, const char* const* items, int count, int sel) {
    wg::ListView v;
    v.x = wg::columnX(col);
    v.w = wg::columnW(span);
    v.count = count;
    v.sel = sel;
    v.top = wg::listFollow(sel, count, wg::listVisibleRows(v.h), 0);
    v.selStyle = wg::ListSel::kWhite;
    v.item = namesItem;
    v.ctx = const_cast<const char**>(items);
    wg::list(g, th, v);
}

// Text cut so that it fits `room` px in `font` (the cut glyphs would still
// cost display-list words).
// A patch or macro name as shown: control bytes left out. A patch made
// elsewhere can end a name in "\r", which the font draws as a stray glyph
// (device report: "Cutoff" + a small D); the stored name keeps its bytes.
const char* shown(const char* s) {
    OT_SDRAM_BSS static char buf[4][kPatchNameMax + 1];   // SDRAM on the device (OP-5)
    static int next = 0;
    char* o = buf[next];
    next = (next + 1) & 3;
    int n = 0;
    for (; *s && n < kPatchNameMax; ++s)
        if (uint8_t(*s) >= 0x20 && uint8_t(*s) != 0x7F) o[n++] = *s;
    o[n] = 0;
    return o;
}

void fit(char* s, uint8_t font, int room) {
    int n = 0, pen = 0;
    while (s[n] && pen + textWidth(font, s + n, 1) <= room) pen += textWidth(font, s + n++, 1);
    s[n] = 0;
}

// Target k of macro mc set to parameter np (-1 clears it). Targets stay
// packed like the factory files: never -1 inside count. A target set on a
// row past the last one is appended, a cleared one is removed and the rest
// move up; unused entries are {0, 0}. Holes read from a file are closed by
// the same pass. Returns the target's new index (np >= 0), else -1.
int setMacroTarget(PatchMacro& mc, int k, int np, uint32_t newAmountBits) {
    int count = audio::synthMacroTargets(mc);
    int at = k;
    if (k >= count) {
        if (np < 0) return -1;
        at = count;
        mc.targets[at] = PatchTarget{np, newAmountBits};
        ++count;
    } else {
        if (mc.targets[k].param < 0) mc.targets[k].amountBits = newAmountBits;
        mc.targets[k].param = np;
    }
    int w = 0;
    for (int j = 0; j < count; ++j) {
        if (mc.targets[j].param < 0) {
            if (j < at) --at;
            continue;
        }
        mc.targets[w++] = mc.targets[j];
    }
    for (int j = w; j < kPatchMacroTargets; ++j) mc.targets[j] = PatchTarget{0, 0};
    mc.count = w;
    return np >= 0 ? at : -1;
}

// The engine's Init patch (Delete in Edit Patch), built in the browsers' list
// memory (screens.h): Edit Patch is never on screen with the patch browser,
// and every browser rebuilds its list when it opens. Main loop only.
static_assert(sizeof(SynthPatch) <= kBrowserListBytes && alignof(SynthPatch) <= kBrowserListAlign, "the Init patch fits the browser lists");

bool sortName(const char* a, const char* b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y || !x) return x < y;
    }
}
#if OT_SLOT_INSERTS
// The Insert list: "None", then the build's effects in module order.
constexpr int kInsertListMax = 17;
int insertList(const char** items) {
    mod::ModuleHost& h = mod::ModuleHost::instance();
    int n = 0;
    items[n++] = "None";
    for (int k = 0; k < h.effectCount() && n < kInsertListMax; ++k) items[n++] = h.effectAt(k)->name();
    return n;
}
// The first setting of an insert, when it is a list, starts with "Off" (the
// effect not run, its settings kept): Chorus Off, I, II, I+II.
constexpr int kInsertNamesMax = 16;
bool offInList(const audio::IEffect* fx, int i) { return fx && i == 0 && fx->param(0).names; }

// Slot s's insert in that list: 0 none, -1 an effect this build lacks.
int insertIndex(const App& a, int s) {
    const char* id = a.slotInsertId(s);
    if (!id) return 0;
    mod::ModuleHost& h = mod::ModuleHost::instance();
    for (int k = 0; k < h.effectCount() && k + 1 < kInsertListMax; ++k)
        if (std::strncmp(h.effectAt(k)->id(), id, 4) == 0) return k + 1;
    return -1;
}
#endif
}  // namespace

void synthui::slotLabel(const App& a, int s, char* out, int cap) {
    std::snprintf(out, size_t(cap), "Synth %d (%s)", s + 1, synthEngineDisplayName(a.slotEngineId(s)));
}

// ---------------------------------------------------------------------------
// State

int SynthScreen::entrySlot(App& a, Ui& ui, int active) {
    if (active == kModPattern) {
        if (auto* p = static_cast<PatternScreen*>(ui.screen(kModPattern))) {
            const Step& st = a.pattern().tracks[clampi(p->track, 0, kTrackCount - 1)].steps[clampi(p->step, 0, kStepCount - 1)];
            if (isSynthInstrument(st.instrument)) {
                a.setCurrentInstrument(st.instrument);
                return synthSlotOf(st.instrument);
            }
        }
    }
    return synthSlotOf(a.currentInstrument());
}

int32_t SynthScreen::engine() const { return app_.synthPatch(slot).engine; }

bool SynthScreen::available() const {
    // From the record and the patch (the framework picks a record change up
    // only at its next block).
    const int32_t id = app_.slotEngineId(slot);
    return id != 0 && audio::synthEngineFor(id) != nullptr && app_.synthPatch(slot).engine == id;
}

int SynthScreen::rows() const {
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(engine(), &sec);
    if (section < 0 || section >= n) return 0;
    return sec[section].macro >= 0 ? synthui::kMacroRows : sec[section].count;
}

void SynthScreen::start(int opt) {
    if (opt >= 1 && opt <= kSynthSlotCount) slot = opt - 1;
    else if (isSynthInstrument(app_.currentInstrument())) slot = synthSlotOf(app_.currentInstrument());
    if (view != View::Macros) view = View::Params;
    param = clampi(param, 0, 5);
    // Edit Patch starts at its first section (the index may belong to
    // another slot's / model's section list).
    section = row = 0;
    listTop_ = 0;
    refreshBar();
}

void SynthScreen::stop() {
    audition(false, 0);
    if (view == View::Browser) closeBrowser();   // leaving the screen cancels the popup
    if (view == View::Presets && previewKey_ >= 0) endPresetPreview();
}

void SynthScreen::audition(bool on, uint8_t note) {
    audio::SynthBank& b = app_.engine.synth();
    if (on) {
        b.auditionOn(slot, note);
        return;
    }
    b.auditionOff();
    padsOff();
}

void SynthScreen::padsOff() {
    for (int p = 0; p < kPadCount; ++p)
        if (padsHeld_ >> p & 1) app_.previewOff(uint16_t(p));
    padsHeld_ = 0;
}

void SynthScreen::edited() {
    edited_[clampi(slot, 0, kSynthSlotCount - 1)] = true;
    app_.markSynthPatchDirty(slot);
    refreshBar();
    ui_.dirty();
}

// ---------------------------------------------------------------------------
// Bottom bar

void SynthScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    b.clear();
    b.setStripStyle(wg::StripStyle::kDark);
    const SynthSlotRecord r = app_.synthSlot(slot);
    const bool ro = !available();
    char v[24];
    // Focus never rests on a disabled column: a read-only slot keeps only
    // F1 Model selectable (Params) and shows no macro focus.
    if (ro) param = 0;
    switch (view) {
        case View::Params: {
            b.set(0, "Model", synthEngineDisplayName(app_.slotEngineId(slot)));
            const audio::SynthSlotView sv = app_.engine.synth().slotView(slot);
            if (sv.limited) std::snprintf(v, sizeof(v), "%u (%d)", unsigned(r.polyphony), sv.playable);
            else std::snprintf(v, sizeof(v), "%u", unsigned(r.polyphony));
            b.set(1, "Polyphony", v, !ro);
            synthui::gainText(r.gainDb, v, sizeof(v));
            b.set(2, "Gain", v, !ro);
            std::snprintf(v, sizeof(v), "%d", int(std::lround(r.pan)));
            b.set(3, "Panning", v, !ro);
            synthui::sendText(r.reverbSend, v, sizeof(v));
            b.set(4, "Reverb Send", v, !ro);
            synthui::sendText(r.delaySend, v, sizeof(v));
            b.set(5, "Delay Send", v, !ro);
            b.setName(6, "Load", "Patch", !ro);
            b.setName(7, "Edit", "Patch");
            b.highlight(param, true);
            break;
        }
        case View::Macros: {
            const SynthPatch& p = app_.synthPatch(slot);
            const audio::SynthSlotView sv = app_.engine.synth().slotView(slot);
            for (int k = 0; k < audio::kSynthSeqMacros; ++k) {
                char top[24];
                std::snprintf(top, sizeof(top), "M%d %s", k + 1, shown(p.macros[k].name));
                fit(top, kFont3, 92);
                const int val = sv.overridden[k] ? sv.macro[k] : r.macros[k];   // as the bar: an FX CC override
                if (p.macros[k].bipolar) std::snprintf(v, sizeof(v), "%d", val - 63);
                else std::snprintf(v, sizeof(v), "%d", val);
                b.set(k, top, v, !ro);
            }
            std::snprintf(v, sizeof(v), "%d%%", int(app_.slotArpGate(slot)));
            b.set(kArpGateColumn, "Arp Gate", v, !ro);
            b.set(7, "Presets");
            b.highlight(macro, !ro);
            break;
        }
        case View::Edit: {
            // Manual p.153: Save, Save As, Init Patch, -, Up, the section,
            // Down, Back; F5..F7 form one group (no separators inside, the
            // arrows are drawn by drawEdit).
            b.set(0, "Save", nullptr, !ro);
            b.set(1, "Save As", nullptr, !ro);
            b.setName(2, "Init", "Patch", !ro);
            b.label[3].hidden = b.label[4].hidden = b.label[6].hidden = true;
            const synthui::Section* sec = nullptr;
            const int n = synthui::sections(engine(), &sec);
            if (section >= 0 && section < n) b.set(5, sec[section].longName);
            // A macro's Name row: F8 "Edit" opens the naming keyboard (Mini
            // manual p.155 "Select edit to use naming editor"; Tracker+
            // tutorial V2 7:52). Back stays on F8 for every other row.
            b.set(7, nameRow() ? "Edit" : "Back", nullptr, !nameRow() || !ro);
            b.separators = uint8_t(0xFE & ~((1u << 5) | (1u << 6)));
            break;
        }
        case View::Insert:
#if OT_SLOT_INSERTS
            insertBar();
#endif
            break;
        case View::Presets: {
            const bool used = app_.synthPresetUsed(row + 1);
            b.set(0, "Store", nullptr, !ro);
            b.set(1, "Load", nullptr, used);
            b.set(2, "Clear", nullptr, used);
            b.label[3].hidden = b.label[6].hidden = true;
            b.setWide(4, "Preview", nullptr, used && !ro);
            b.set(7, "Back");
            break;
        }
        case View::Browser:
            // Tracker+ 1.0 Load Patch popup: F5-F6 Preview, F7 Cancel, F8 Confirm.
            for (int i = 0; i < 4; ++i) b.label[i].hidden = true;
            b.setWide(4, "Preview", nullptr, browserCount > 0 && !ro);
            b.set(6, "Cancel");
            b.set(7, "Confirm", nullptr, browserCount > 0 && !ro);
            break;
    }
}

// ---------------------------------------------------------------------------
// Edits

void SynthScreen::paramsChange(int d, bool wrap) {
    SynthSlotRecord r = app_.synthSlot(slot);
    if (param == 0) {
        // The Model key cycles through the models and wraps (manual p.147-148
        // "Press [Model] to cycle"); the list (Up / Down, encoder) stops at
        // its ends like every list.
        const int n = synthModelListCount();
        int cur = synthModelListIndex(app_.slotEngineId(slot));
        if (cur < 0) cur = 0;
        const int m = wrap ? (cur + d + n) % n : clampi(cur + d, 0, n - 1);
        if (m != cur) {
            pendingModel_ = m;
            // Unsaved patch edits: ask before the new model's Init replaces them.
            if (edited_[slot]) {
                ui_.ask("Change Model", "Discard edits?", nullptr, "No", "Yes",
                        [](void* c, int key) {
                            auto* s = static_cast<SynthScreen*>(c);
                            if (key == btn::F8) s->applyModel();
                        },
                        this);
            } else {
                applyModel();
            }
        }
        refreshBar();
        ui_.dirty();
        return;
    }
    if (!available()) return;
    switch (param) {
        case 1: r.polyphony = uint32_t(clampi(int(r.polyphony) + d, 0, 8)); break;
        case 2: r.gainDb = clampf(std::round((r.gainDb + 0.5f * float(d)) * 2.0f) / 2.0f, -96.0f, 12.0f); break;
        case 3: r.pan = clampf(std::round(r.pan) + float(d), -100.0f, 100.0f); break;
        case 4: r.reverbSend = clampf(std::round(r.reverbSend) + float(d), 0.0f, 100.0f); break;
        case 5: r.delaySend = clampf(std::round(r.delaySend) + float(d), 0.0f, 100.0f); break;
        default: return;
    }
    app_.setSynthSlotRecord(slot, r);
    refreshBar();
    ui_.dirty();
}

// Delete: the selected value back to its default (2x-05 §6.1 / §6.2).
// Params: the slot defaults of spec 21 §3.3 (polyphony 1, -9 dB, centre,
// sends off); the model is not reset. Macros: the patch default (0, or the
// centre of a bipolar macro). Edit Patch: the Init patch's value of the
// selected row (Tracker+ tutorial, docs/SYNTH_UX_MINI_VS_OT.md row 15).
void SynthScreen::resetSelected() {
    if (!available()) return;
    if (view == View::Edit) {
        resetEditRow();
        return;
    }
    SynthSlotRecord r = app_.synthSlot(slot);
    const SynthSlotRecord d = SynthSlotRecord::defaults(slot);
    if (view == View::Params) {
        switch (param) {
            case 1: r.polyphony = d.polyphony; break;
            case 2: r.gainDb = d.gainDb; break;
            case 3: r.pan = d.pan; break;
            case 4: r.reverbSend = d.reverbSend; break;
            case 5: r.delaySend = d.delaySend; break;
            default: return;
        }
    } else if (view == View::Macros) {
        if (macro == kArpGateColumn) {
            app_.setSlotArpGate(slot, 100);
            refreshBar();
            ui_.dirty();
            return;
        }
        const int k = clampi(macro, 0, audio::kSynthSeqMacros - 1);
        r.macros[k] = audio::synthMacroDefault(app_.synthPatch(slot).macros[k].bipolar);
        app_.engine.synth().releaseMacro(slot, k);   // the hand edit wins over an FX CC
    } else {
        return;
    }
    app_.setSynthSlotRecord(slot, r);
    refreshBar();
    ui_.dirty();
}

void SynthScreen::resetEditRow() {
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(engine(), &sec);
    if (section < 0 || section >= n || row < 0 || row >= rows()) return;
    SynthPatch& init = *new (browserListMem()) SynthPatch();
    if (!patchInit(engine(), &init)) return;
    SynthPatch& p = app_.synthPatch(slot);
    if (sec[section].macro < 0) {
        const int idx = sec[section].params[row];
        const int mini = audio::synthParamCount(engine());
        if (idx >= mini) {
            // [OT] extension (Width, FAT Stereo): absent in the Init patch.
            const int k = idx - mini;
            if (k >= p.extCount || !p.ext[k].present) return;
            p.ext[k] = PatchParam{0u, false};
            edited();
            return;
        }
        if (idx >= init.paramCount) return;
        const PatchParam want = init.params[idx];
        if (idx < p.paramCount && p.params[idx].present == want.present && (!want.present || p.params[idx].bits == want.bits))
            return;
        if (want.present) audio::synthParamSet(p, idx, patchBitsToFloat(want.bits));
        else if (idx < p.paramCount) p.params[idx] = PatchParam{0u, false};   // absent = the engine default
        else return;
    } else {
        const int m = sec[section].macro;
        PatchMacro& mc = p.macros[m];
        const PatchMacro& im = init.macros[m];
        const int initCount = audio::synthMacroTargets(im);
        if (row == 0) {
            std::memcpy(mc.name, im.name, sizeof(mc.name));
            mc.nameLen = im.nameLen;
        } else if (row == 1) {
            mc.bipolar = im.bipolar;
        } else {
            const int k = (row - 2) / 2;
            const bool live = k < audio::synthMacroTargets(mc) && mc.targets[k].param >= 0;
            if ((row - 2) % 2 == 0) {
                const PatchTarget t = k < initCount ? im.targets[k] : PatchTarget{-1, patchFloatToBits(1.0f)};
                if (live && mc.targets[k].param == t.param) return;
                if (!live && t.param < 0) return;
                setMacroTarget(mc, k, t.param, t.amountBits);
            } else {
                if (!live) return;
                mc.targets[k].amountBits = k < initCount ? im.targets[k].amountBits : patchFloatToBits(1.0f);
            }
        }
    }
    edited();
}

void SynthScreen::macroChange(int d) {
    if (!available()) return;
    if (macro == kArpGateColumn) {
        app_.setSlotArpGate(slot, int(app_.slotArpGate(slot)) + d);
        refreshBar();
        ui_.dirty();
        return;
    }
    SynthSlotRecord r = app_.synthSlot(slot);
    const int k = clampi(macro, 0, audio::kSynthSeqMacros - 1);
    r.macros[k] = uint8_t(clampi(int(r.macros[k]) + d, 0, 127));
    app_.engine.synth().releaseMacro(slot, k);   // the hand edit wins over an FX CC
    app_.setSynthSlotRecord(slot, r);
    refreshBar();
    ui_.dirty();
}

void SynthScreen::macroRowText(int m, int r, char* label, int lcap, char* value, int vcap) const {
    const PatchMacro& mc = app_.synthPatch(slot).macros[clampi(m, 0, kPatchMacros - 1)];
    const int count = audio::synthMacroTargets(mc);
    if (r == 0) {
        std::snprintf(label, size_t(lcap), "Name");
        std::snprintf(value, size_t(vcap), "%s", mc.name[0] ? shown(mc.name) : "-");
    } else if (r == 1) {
        std::snprintf(label, size_t(lcap), "Bipolar");
        std::snprintf(value, size_t(vcap), "%s", mc.bipolar ? "On" : "Off");
    } else {
        const int k = (r - 2) / 2;
        const bool live = k < count && mc.targets[k].param >= 0;
        if ((r - 2) % 2 == 0) {
            std::snprintf(label, size_t(lcap), "Parameter %d", k + 1);
            if (live) synthui::paramFullName(engine(), mc.targets[k].param, value, vcap);
            else std::snprintf(value, size_t(vcap), "Unassigned");   // Mini manual p.173 figure
        } else {
            std::snprintf(label, size_t(lcap), "Amount %d", k + 1);
            if (live) std::snprintf(value, size_t(vcap), "%.2f", double(patchBitsToFloat(mc.targets[k].amountBits)));
            else std::snprintf(value, size_t(vcap), "-");
        }
    }
}

void SynthScreen::macroRowChange(int m, int r, int d) {
    SynthPatch& p = app_.synthPatch(slot);
    PatchMacro& mc = p.macros[clampi(m, 0, kPatchMacros - 1)];
    int count = audio::synthMacroTargets(mc);
    if (r == 0) return;   // the name: keyboard (F7 / encoder push)
    if (r == 1) {
        mc.bipolar = !mc.bipolar;
        // The macro's value restarts at its neutral position for the new
        // mode (0, or the centre 63 = "0" when bipolar), as on a patch load;
        // keeping the old 0 showed a bipolar macro at -63 (user report).
        if (m < audio::kSynthSeqMacros) {
            SynthSlotRecord rec = app_.synthSlot(slot);
            rec.macros[m] = audio::synthMacroDefault(mc.bipolar);
            app_.setSynthSlotRecord(slot, rec);
        }
    } else {
        const int k = (r - 2) / 2;
        if ((r - 2) % 2 == 0) {
            const int cur = k < count ? mc.targets[k].param : -1;
            const int n = synthui::targetChoices(engine());
            const int ci = clampi(synthui::targetChoiceIndex(engine(), cur) + d, 0, n - 1);
            const int np = synthui::targetChoice(engine(), ci);
            if (np == cur) return;
            const int at = setMacroTarget(mc, k, np, patchFloatToBits(1.0f));
            if (at >= 0) row = 2 + 2 * at;   // the cursor follows an appended target
        } else {
            if (k >= count || mc.targets[k].param < 0) return;
            float a = patchBitsToFloat(mc.targets[k].amountBits);
            a = clampf(std::round((a + 0.01f * float(d)) * 100.0f) / 100.0f, -2.0f, 2.0f);
            mc.targets[k].amountBits = patchFloatToBits(a);
        }
    }
    edited();
}

bool SynthScreen::nameRow() const {
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(engine(), &sec);
    return view == View::Edit && section >= 0 && section < n && sec[section].macro >= 0 && row == 0;
}

// Macro name row (F8 Edit, encoder push): the naming keyboard.
void SynthScreen::macroNameKeyboard() {
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(engine(), &sec);
    if (!available() || section >= n || sec[section].macro < 0 || row != 0) return;
    const PatchMacro& mc = app_.synthPatch(slot).macros[sec[section].macro];
    // Renaming drops the control bytes the name was shown without.
    ui_.kb.open("Macro Name", shown(mc.name), kPatchMacroNameMax - 1, "Enter",
                [](void* c, bool ok, const char* text) {
                    auto* s = static_cast<SynthScreen*>(c);
                    const synthui::Section* sc = nullptr;
                    const int ns = synthui::sections(s->engine(), &sc);
                    if (!ok || !text || s->section >= ns || sc[s->section].macro < 0) return;
                    PatchMacro& m = s->app_.synthPatch(s->slot).macros[sc[s->section].macro];
                    std::snprintf(m.name, sizeof(m.name), "%s", text);
                    m.nameLen = uint8_t(std::strlen(m.name));
                    s->edited();
                },
                this);
}

void SynthScreen::editChange(int d) {
    if (!available()) return;
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(engine(), &sec);
    if (section < 0 || section >= n) return;
    if (sec[section].macro >= 0) {
        macroRowChange(sec[section].macro, row, d);
        return;
    }
    if (row < 0 || row >= sec[section].count) return;
    const int idx = sec[section].params[row];
    SynthPatch& p = app_.synthPatch(slot);
    const float v = audio::synthParamGet(p, idx);
    const float nv = synthui::stepValue(engine(), idx, v, d);
    if (nv == v && p.params[idx].present) return;
    audio::synthParamSet(p, idx, nv);
    edited();
}

// ---------------------------------------------------------------------------
// Patch I/O

void SynthScreen::listBrowser() {
    browserCount = 0;
    const SynthSlotRecord r = app_.synthSlot(slot);
    char dir[32];
    std::snprintf(dir, sizeof(dir), "Patches/%s", synthEngineFolder(app_.slotEngineId(slot)));
    app_.fs().listDir(dir, [](const IStorage::DirEntry& e, void* ctx) {
        auto* s = static_cast<SynthScreen*>(ctx);
        if (e.isDir || s->browserCount >= kBrowserMax) return true;
        const size_t n = std::strlen(e.name);
        if (n <= 6 || n - 6 > size_t(kBrowserNameMax) || e.name[0] == '.') return true;
        const char* ext = e.name + n - 6;
        if (!(ext[0] == '.' && (ext[1] | 32) == 'p' && (ext[2] | 32) == 'a' && (ext[3] | 32) == 't' &&
              (ext[4] | 32) == 'c' && (ext[5] | 32) == 'h'))
            return true;
        std::memcpy(s->browser[s->browserCount], e.name, n - 6);
        s->browser[s->browserCount][n - 6] = 0;
        ++s->browserCount;
        return true;
    }, this);
    // Sorted by name, case-insensitive (stock lists).
    for (int i = 1; i < browserCount; ++i)
        for (int j = i; j > 0 && sortName(browser[j], browser[j - 1]); --j) {
            char t[kBrowserNameMax + 1];
            std::memcpy(t, browser[j], sizeof(t));
            std::memcpy(browser[j], browser[j - 1], sizeof(t));
            std::memcpy(browser[j - 1], t, sizeof(t));
        }
    browserSel = 0;
    for (int i = 0; i < browserCount; ++i)
        if (std::strcmp(browser[i], r.patchName) == 0) browserSel = i;
    browserTop_ = 0;
}

void SynthScreen::applyModel() {
    if (pendingModel_ < 0) return;
    audition(false, 0);
    app_.setSynthEngine(slot, synthModelListEngine(pendingModel_));
    pendingModel_ = -1;
    edited_[slot] = false;
    section = row = 0;
    ui_.notify("Init patch loaded");   // Tracker+ 1.0 on every model change (docs/SYNTH_UX_MINI_VS_OT.md row 7)
    refreshBar();
    ui_.dirty();
}

void SynthScreen::openBrowser() {
    listBrowser();
    previewSel_ = previewKey_ = -1;
    view = View::Browser;
    ui_.openPopupHeader("Load Patch", Ui::PopupHeader::kRightBar);
}

void SynthScreen::closeBrowser() {
    audition(false, 0);
    previewKey_ = -1;
    app_.endSynthPreview();
    previewSel_ = -1;
    if (view == View::Browser) {
        view = View::Params;
        ui_.closePopupHeader();
    }
}

// Preview / pads: the highlighted patch plays in the slot until Confirm or
// Cancel (read again only when the highlight moved).
bool SynthScreen::previewHighlighted() {
    if (!available() || browserSel < 0 || browserSel >= browserCount) return false;
    if (previewSel_ == browserSel) return true;
    audition(false, 0);
    if (!app_.previewSynthLibraryPatch(slot, browser[browserSel])) {
        app_.endSynthPreview();
        previewSel_ = -1;
        ui_.notify("Patch could not be loaded");
        return false;
    }
    previewSel_ = browserSel;
    return true;
}

bool SynthScreen::loadSelected() {
    if (!available() || browserSel < 0 || browserSel >= browserCount) return false;
    if (edited_[slot]) {   // unsaved patch edits: ask first (2x-05 §6.3)
        ui_.ask("Load Patch", "Discard edits?", nullptr, "No", "Yes",
                [](void* c, int key) {
                    auto* s = static_cast<SynthScreen*>(c);
                    if (key == btn::F8) s->loadSelectedNow();
                },
                this);
        return false;
    }
    return loadSelectedNow();
}

bool SynthScreen::loadSelectedNow() {
    if (browserSel < 0 || browserSel >= browserCount) return false;
    audition(false, 0);
    previewKey_ = -1;
    app_.endSynthPreview();   // the slot's own patch first, then the real load
    previewSel_ = -1;
    const bool ok = app_.loadSynthLibraryPatch(slot, browser[browserSel]);
    if (ok) {
        edited_[slot] = false;
        view = View::Params;   // Confirm closes the popup
        ui_.closePopupHeader();
    }
    ui_.notify(ok ? "Patch loaded" : "Patch could not be loaded");
    refreshBar();
    ui_.dirty();
    return ok;
}

void SynthScreen::initPatch() {
    if (!available()) return;
    ui_.ask("Init Patch", "Load the init patch?", nullptr, "No", "Yes",
            [](void* c, int key) {
                auto* s = static_cast<SynthScreen*>(c);
                if (key != btn::F8) return;
                s->app_.initSynthPatch(s->slot);
                s->edited_[s->slot] = false;
                s->section = s->row = 0;
                s->ui_.notify("Init patch loaded");
                s->refreshBar();
            },
            this);
}

void SynthScreen::savePatch() {
    if (!available()) return;
    const SynthSlotRecord r = app_.synthSlot(slot);
    if (!r.patchName[0] || std::strcmp(r.patchName, "Init") == 0) {
        savePatchAs();
        return;
    }
    const bool ok = app_.saveSynthLibraryPatch(slot, r.patchName);
    if (ok) edited_[slot] = false;
    ui_.notify(ok ? "Patch saved" : "Patch could not be saved");
    refreshBar();
}

void SynthScreen::saveAsNow() {
    const bool ok = app_.saveSynthLibraryPatch(slot, pendingName_);
    if (ok) edited_[slot] = false;
    ui_.notify(ok ? "Patch saved" : "Patch could not be saved");
    refreshBar();
    ui_.dirty();
}

void SynthScreen::savePatchAs() {
    if (!available()) return;
    const SynthSlotRecord r = app_.synthSlot(slot);
    char init[40];
    if (r.patchName[0] && std::strcmp(r.patchName, "Init") != 0) std::snprintf(init, sizeof(init), "%s", r.patchName);
    else Keyboard::autoName(init, sizeof(init), ui_.now() + uint32_t(slot));
    ui_.kb.open("Save Patch As", init, kBrowserNameMax, "Save",
                [](void* c, bool ok, const char* text) {
                    auto* s = static_cast<SynthScreen*>(c);
                    if (!ok) return;
                    char* name = s->pendingName_;
                    std::snprintf(name, sizeof(s->pendingName_), "%s", text ? text : "");
                    // Trailing spaces would make a different file name.
                    for (size_t n = std::strlen(name); n > 0 && name[n - 1] == ' '; --n) name[n - 1] = 0;
                    if (!name[0]) {
                        s->ui_.notify("Patch name is empty");
                        return;
                    }
                    for (char* q = name; *q; ++q)
                        if (*q == '/' || *q == '\\') *q = '_';
                    char path[96];
                    std::snprintf(path, sizeof(path), "Patches/%s/%s.patch", synthEngineFolder(s->app_.slotEngineId(s->slot)),
                                  name);
                    if (s->app_.fs().exists(path)) {
                        s->ui_.ask("Patch exists", "Overwrite?", nullptr, "Cancel", "Overwrite",
                                   [](void* c2, int key) {
                                       if (key == btn::F8) static_cast<SynthScreen*>(c2)->saveAsNow();
                                   },
                                   s);
                        return;
                    }
                    s->saveAsNow();
                },
                this);
}

// ---------------------------------------------------------------------------
// Input

void instrumentChooserItem(void* ctx, int i, char* out, int cap) {
    App* a = static_cast<App*>(ctx);
    // "N. name" / "N. MIDI Channel n" (manual p.256 / 265; MANUAL_AUDIT #26).
    if (i < kSampleInstrumentCount)
        std::snprintf(out, size_t(cap), "%d. %s", i + 1, a->instrPresent[i] ? a->instruments[i].name : "");
    else if (i < kInstrumentCount) std::snprintf(out, size_t(cap), "%d. MIDI Channel %d", i + 1, i - kFirstMidiInstrument + 1);
    else synthui::slotLabel(*a, i - kFirstSynthInstrument, out, cap);
}

void SynthScreen::onButton(uint8_t id, uint8_t state) {
    // Instr held: the instrument chooser, as on the Instrument Params page
    // (user report: the key did nothing on a synth page). Releasing it opens
    // the page of the picked instrument (sample, MIDI or synth slot).
    if (id == btn::Instr && view != View::Browser) {
        if (state == kPress) {
            audition(false, 0);
            ui_.picker.open(2, "Instrument", kInstrumentChooserCount, app_.currentInstrument(), instrumentChooserItem,
                            &app_);
        } else if (state == kRelease && ui_.picker.isOpen()) {
            app_.setCurrentInstrument(uint8_t(ui_.picker.cursor()));
            ui_.picker.close();
            ui_.switchTo(kModInstrument);   // routes to this page for S1..S3
        }
        return;
    }
    if (ui_.picker.isOpen()) return;
    if (id == btn::InstrParams) {
        if (state != kPress) return;
        audition(false, 0);
        if (view == View::Browser) closeBrowser();   // Cancel, back to page 1/2
#if OT_SLOT_INSERTS
        // [3.x] Shift + Instrument Params: the slot's insert; Instrument
        // Params from there: back to page 1/2.
        else if (view == View::Insert) view = View::Params;
        else if (ui_.shift()) view = View::Insert;
#endif
        else view = view == View::Params ? View::Macros : View::Params;
        refreshBar();
        ui_.dirty();
        return;
    }
    if (id == btn::EncPush) {
        if (view == View::Browser) {
            if (state == kPress) loadSelected();
        } else if (nameRow()) {
            if (state == kPress) macroNameKeyboard();
        } else if (state == kPress) {
            audition(true, 60);   // manual p.152: the centre button auditions
        } else if (state == kRelease) {
            app_.engine.synth().auditionOff();   // its own note; held pads play on
        }
        return;
    }
    if (view == View::Presets && (id == btn::F5 || id == btn::F6) && state == kRelease) {
        if (previewKey_ == int(id)) endPresetPreview();
        return;
    }
    if (view == View::Browser && (id == btn::F5 || id == btn::F6) && state == kRelease) {
        if (previewKey_ == int(id)) {
            previewKey_ = -1;
            app_.engine.synth().auditionOff();   // its own note; held pads play on
        }
        return;
    }
    if (state != kPress) {
        if (!btn::isFKey(id) && id != btn::Up && id != btn::Down && id != btn::Left && id != btn::Right)
            Screen::onButton(id, state);
        return;
    }
    const bool ro = !available();
    switch (view) {
        case View::Params:
            if (id <= btn::F6) {
                if (id == btn::F1 && param == 0) paramsChange(1, true);   // Model pressed again: next model, wraps
                else if (id == btn::F1 || !ro) param = id;
            } else if (id == btn::F7) {
                if (ro) break;
                openBrowser();
            } else if (id == btn::F8) {
                view = View::Edit;
                const synthui::Section* sec = nullptr;
                const int n = synthui::sections(engine(), &sec);
                if (section >= n) section = row = 0;
                row = clampi(row, 0, rows() > 0 ? rows() - 1 : 0);
                listTop_ = 0;
            } else if (id == btn::Up || id == btn::Down) {
                // The model list moves like a list (Up = previous), the values like bars.
                const int d = id == btn::Up ? 1 : -1;
                paramsChange(param == 0 ? -d : d);
            } else if (id == btn::Delete) {
                resetSelected();
            } else if (id == btn::Left || id == btn::Right) {
                param = clampi(param + (id == btn::Right ? 1 : -1), 0, ro ? 0 : 5);
            } else {
                Screen::onButton(id, state);
            }
            break;
        case View::Macros:
            if (id <= btn::F7) macro = id;
            else if (id == btn::Up || id == btn::Down) macroChange(id == btn::Up ? 1 : -1);
            else if (id == btn::Delete) resetSelected();
            else if (id == btn::Left || id == btn::Right) macro = clampi(macro + (id == btn::Right ? 1 : -1), 0, kArpGateColumn);
            else if (id == btn::F8) {
                view = View::Presets;
                row = 0;
            } else if (id != btn::F7) Screen::onButton(id, state);
            break;
        case View::Presets:
            presetKey(id);
            break;
        case View::Edit: {
            const synthui::Section* sec = nullptr;
            const int n = synthui::sections(engine(), &sec);
            if (id == btn::F1) savePatch();
            else if (id == btn::F2) savePatchAs();
            else if (id == btn::F3) initPatch();
            else if (id == btn::F6 && n > 0) {   // the section label: next section
                section = (section + 1) % n;
                row = 0;
                listTop_ = 0;
            } else if (id == btn::F8) {
                if (nameRow()) macroNameKeyboard();
                else view = View::Params;
            } else if (id == btn::Left || id == btn::Right) {
                section = clampi(section + (id == btn::Right ? 1 : -1), 0, n - 1);
                row = 0;
                listTop_ = 0;
            } else if (id == btn::Up || id == btn::Down || id == btn::F5 || id == btn::F7) {
                const bool down = id == btn::Down || id == btn::F7;
                row = clampi(row + (down ? 1 : -1), 0, rows() - 1);
            } else if (id == btn::Delete) {
                resetSelected();
            } else if (!btn::isFKey(id)) {
                Screen::onButton(id, state);
            }
            break;
        }
        case View::Insert:
#if OT_SLOT_INSERTS
            insertKey(id);
#endif
            break;
        case View::Browser:
            if (id == btn::F5 || id == btn::F6) {
                // Preview (Tracker+ 1.0; "preview low notes or high notes"):
                // F5 C3, F6 C5 of the highlighted patch, held while pressed.
                if (!ro && previewHighlighted()) {
                    previewKey_ = id;
                    audition(true, id == btn::F5 ? 48 : 72);
                }
            } else if (id == btn::F7) {
                closeBrowser();
            } else if (id == btn::F8) {
                loadSelected();
            } else if (id == btn::Up || id == btn::Down)
                browserSel = clampi(browserSel + (id == btn::Down ? 1 : -1), 0, browserCount > 0 ? browserCount - 1 : 0);
            else if (!btn::isFKey(id) && id != btn::Left && id != btn::Right) Screen::onButton(id, state);
            break;
    }
    refreshBar();
    ui_.dirty();
}

#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
bool SynthScreen::encoderOnList() const {
    if (view == View::Browser) return true;     // patch list
    if (view == View::Params) return param == 0;   // model list
#if OT_SLOT_INSERTS
    if (view == View::Insert) {   // the insert list and enum settings are lists
        if (insFocus == 0) return true;
        audio::IEffect* fx = app_.slotInsertEffect(slot);
        return fx && insFocus <= fx->paramCount() && fx->param(insFocus - 1).names;
    }
#endif
    if (view != View::Edit || !available()) return false;
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(engine(), &sec);
    if (section < 0 || section >= n) return false;
    // A macro's Off / On row is a two-item list (drawn by listColumn).
    if (sec[section].macro >= 0) return row == 1;
    if (row < 0 || row >= sec[section].count) return false;
    const synthui::ParamUi* u = synthui::paramUi(engine(), sec[section].params[row]);
    // Enum rows (Filter Type, LFO Waveform, ...) and On / Off rows are drawn
    // as lists (tester report: the wheel ran backwards on On / Off rows).
    return u && (u->fmt == synthui::kFmtEnum || u->fmt == synthui::kFmtOnOff);
}
#endif

void SynthScreen::onEncoder(int d) {
    switch (view) {
        case View::Params: paramsChange(param == 0 ? (d > 0 ? 1 : -1) : d); break;
        case View::Macros: macroChange(d); break;
        case View::Edit: editChange(d); break;
        case View::Presets: row = clampi(row + d, 0, kSynthPresetCount - 1); break;
        case View::Insert:
#if OT_SLOT_INSERTS
            insertChange(d);
#endif
            break;
        case View::Browser:
            browserSel = clampi(browserSel + d, 0, browserCount > 0 ? browserCount - 1 : 0);
            break;
    }
    ui_.dirty();
}

void SynthScreen::onPad(uint8_t pad, uint8_t state) {
    // Edit Patch: the 4 x 4 block at the left of the pad grid selects the
    // section (manual p.153 "select using the lit pads").
    if (view == View::Edit && pad < kPadCount) {
        const int c = pad % kPadCols, r = pad / kPadCols;
        const synthui::Section* sec = nullptr;
        const int n = synthui::sections(engine(), &sec);
        // (A release falls through: the pad may have been pressed in
        // another view and still hold its note.)
        if (c < 4 && r * 4 + c < n && state == kPress) {
            section = r * 4 + c;
            row = 0;
            listTop_ = 0;
            refreshBar();
            ui_.dirty();
            return;
        }
    }
    // Pads audition the slot (scale map of the pad grid), a chord up to its
    // polyphony; in Load Patch the highlighted patch (Tracker+ 1.0).
    if (pad >= kPadCount) return;
    if (state == kPress) {
        if (view == View::Browser && !previewHighlighted()) return;
        padsHeld_ |= uint64_t(1) << pad;
        app_.previewOn(pad, uint8_t(kFirstSynthInstrument + slot), app_.padNote(pad), 127);
    } else if (state == kRelease) {
        // Also a pad pressed on another page (its note is released here).
        padsHeld_ &= ~(uint64_t(1) << pad);
        app_.previewOff(pad);
    }
}

void SynthScreen::padLeds(uint8_t* lv) {
    if (view != View::Edit) return;
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(engine(), &sec);
    for (int i = 0; i < n; ++i) lv[(i / 4) * kPadCols + i % 4] = i == section ? app_.cfg.padFull() : app_.cfg.padDim();
}

// ---------------------------------------------------------------------------
// Drawing

void SynthScreen::title(Gfx& g, const char* text) {
    char right[48];
    const int32_t id = app_.slotEngineId(slot);
    const char* m = synthEngineDisplayName(id);
    if (available()) {
        const int z = app_.synthPresetActive(slot);
        if (z) std::snprintf(right, sizeof(right), "Synth %d (%s) P%d", slot + 1, m, z);   // why it sounds different
        else std::snprintf(right, sizeof(right), "Synth %d (%s)", slot + 1, m);
        ui_.drawTitle(g, text, right);
    } else {
        // The longer header needs the status group's room (hidden). A model
        // that has an engine but holds another engine's patch is not "not
        // available": the header names the patch's engine instead.
        const int32_t pid = app_.synthPatch(slot).engine;
        const char* pe = patchEngineName(pid) ? synthEngineDisplayName(pid) : nullptr;
        if (id && audio::synthEngineFor(id))
            std::snprintf(right, sizeof(right), "Synth %d (%s) %s patch", slot + 1, m, pe ? pe : "unknown");
        else
            std::snprintf(right, sizeof(right), "Synth %d (%s) not available", slot + 1, m);
        ui_.drawTitle(g, text, right, false);
    }
}

#if OT_SLOT_INSERTS
// ---------------------------------------------------------------------------
// [3.x] The Synth Insert view: F1 the insert, F2..F7 its settings, F8 Back.

void SynthScreen::insertBar() {
    BottomBar& b = ui_.bar;
    audio::IEffect* fx = app_.slotInsertEffect(slot);
    const char* id = app_.slotInsertId(slot);
    char idText[5] = {};
    if (id && !fx) std::memcpy(idText, id, 4);
    b.set(0, "Insert", fx ? fx->name() : (id ? idText : "None"));
    const int np = fx ? std::min(fx->paramCount(), audio::kEffectMaxParams) : 0;
    const audio::EffectSettings set = app_.slotInsertSettings(slot);
    for (int i = 0; i < audio::kEffectMaxParams; ++i) {
        if (i < np) {
            char v[24];
            if (offInList(fx, i) && app_.slotInsertOff(slot)) std::snprintf(v, sizeof(v), "Off");
            else fx->valueText(i, set.v[i], v, sizeof(v));
            b.set(i + 1, fx->param(i).label, v);
        } else {
            b.label[i + 1].hidden = true;
        }
    }
    b.set(7, "Back");
    if (insFocus > np) insFocus = 0;
    b.highlight(insFocus, true);
}

void SynthScreen::insertChange(int d) {
    if (!d) return;
    if (insFocus == 0) {   // the list: one step, no wrap
        const char* items[kInsertListMax];
        const int n = insertList(items);
        const int cur = std::max(insertIndex(app_, slot), 0);
        const int k = clampi(cur + (d > 0 ? 1 : -1), 0, n - 1);
        if (k != cur) app_.setSlotInsertKind(slot, k ? mod::ModuleHost::instance().effectAt(k - 1)->id() : nullptr);
        return;
    }
    audio::IEffect* fx = app_.slotInsertEffect(slot);
    if (!fx || insFocus > fx->paramCount()) return;
    const int i = insFocus - 1;
    if (offInList(fx, i)) {   // Off, then the values
        const int cur = app_.slotInsertOff(slot) ? 0 : app_.slotInsertSettings(slot).v[0] + 1;
        const int k = clampi(cur + d, 0, fx->param(0).max + 1);
        if (k == cur) return;
        if (k) app_.setSlotInsertValue(slot, 0, uint8_t(k - 1));
        app_.setSlotInsertOff(slot, k == 0);
        return;
    }
    const int v = clampi(int(app_.slotInsertSettings(slot).v[i]) + d, 0, fx->param(i).max);
    app_.setSlotInsertValue(slot, i, uint8_t(v));
}

void SynthScreen::insertKey(uint8_t id) {
    audio::IEffect* fx = app_.slotInsertEffect(slot);
    const int np = fx ? std::min(fx->paramCount(), audio::kEffectMaxParams) : 0;
    const bool list = insFocus == 0 || (fx && fx->param(insFocus - 1).names);
    if (id == btn::F1) {   // focuses the list only: the wheel / arrows pick (no effect switched on by accident)
        insFocus = 0;
    } else if (id >= btn::F2 && id <= btn::F7) {
        if (int(id) <= np) insFocus = id;
    } else if (id == btn::F8) {
        view = View::Params;
    } else if (id == btn::Up || id == btn::Down) {
        // Lists move like lists (Up = the item above), values like bars.
        const int d = id == btn::Up ? 1 : -1;
        insertChange(list ? -d : d);
    } else if (id == btn::Left || id == btn::Right) {
        insFocus = clampi(insFocus + (id == btn::Right ? 1 : -1), 0, np);
    } else if (id == btn::Delete) {
        if (insFocus == 0) app_.setSlotInsertKind(slot, nullptr);
        else if (fx && insFocus <= np) {
            app_.setSlotInsertValue(slot, insFocus - 1, fx->param(insFocus - 1).def);
            if (offInList(fx, insFocus - 1)) app_.setSlotInsertOff(slot, false);
        }
    } else if (!btn::isFKey(id)) {
        Screen::onButton(id, kPress);
    }
}

void SynthScreen::drawInsert(Gfx& g) {
    const Theme& th = ui_.th();
    title(g, "Synth Insert");
    const char* items[kInsertListMax];
    const int n = insertList(items);
    const int cur = insertIndex(app_, slot);
    listColumn(g, th, 0, 1, items, n, cur < 0 ? 0 : cur);
    audio::IEffect* fx = app_.slotInsertEffect(slot);
    const int np = fx ? std::min(fx->paramCount(), audio::kEffectMaxParams) : 0;
    const audio::EffectSettings set = app_.slotInsertSettings(slot);
    for (int i = 0; i < audio::kEffectMaxParams; ++i) {
        const int col = i + 1;
        if (i >= np) {
            wg::panelColumn(g, th, col);
            continue;
        }
        const audio::EffectParam& p = fx->param(i);
        if (offInList(fx, i)) {
            const char* names[kInsertNamesMax + 1] = {"Off"};
            const int nn = std::min(int(p.max) + 1, kInsertNamesMax);
            for (int k = 0; k < nn; ++k) names[k + 1] = p.names[k];
            listColumn(g, th, col, 1, names, nn + 1, app_.slotInsertOff(slot) ? 0 : std::min(int(set.v[i]), nn - 1) + 1);
        } else if (p.names) listColumn(g, th, col, 1, p.names, p.max + 1, set.v[i]);
        else barColumn(g, th, col, p.max ? float(set.v[i]) / float(p.max) : 0.0f, false, true);
    }
    wg::panelColumn(g, th, 7);
    if (cur < 0) {   // an insert of an effect this build lacks: named, not played
        char t[32];
        std::snprintf(t, sizeof(t), "%.4s not in this build", app_.slotInsertId(slot));
        g.text(wg::columnX(1) + 12, 47, kFont3, th.slot(slot::kFocusFrame), t, IDisplay::kOptCenterY);
    }
    wg::panelFocus(g, th, wg::columnX(insFocus), wg::columnW());
}
#endif

void SynthScreen::drawParams(Gfx& g) {
    const Theme& th = ui_.th();
    const SynthSlotRecord r = app_.synthSlot(slot);
    const bool ro = !available();
    title(g, "Synth Parameters 1/2");
    // Model list above F1 (manual p.147), bars above F2..F6, the patch panel
    // above F7 / F8.
    const char* models[kMaxSynthModels];
    const int nModels = modelNames(models);
    const int curModel = synthModelListIndex(app_.slotEngineId(slot));
    listColumn(g, th, 0, 1, models, nModels, curModel < 0 ? 0 : curModel);
    barColumn(g, th, 1, float(r.polyphony) / 8.0f, false, !ro);
    barColumn(g, th, 2, (r.gainDb + 96.0f) / 108.0f, false, !ro);
    barColumn(g, th, 3, r.pan / 100.0f, true, !ro);
    barColumn(g, th, 4, r.reverbSend / 100.0f, false, !ro);
    barColumn(g, th, 5, r.delaySend / 100.0f, false, !ro);
    wg::panelColumn(g, th, 6, 2);
    const int x = wg::columnX(6) + 12;
    g.text(x, 47, kFont3, th.slot(slot::kBottomLabel), "Patch:", IDisplay::kOptCenterY);
    char name[40];
    std::snprintf(name, sizeof(name), "%s", r.patchName[0] ? shown(r.patchName) : "-");
    fit(name, kFont2, wg::columnW(2) - 24);
    g.text(x, 73, kFont2, th.slot(slot::kList + 4), name, IDisplay::kOptCenterY);
    if (ro) {
        const int32_t id = app_.slotEngineId(slot);
        const bool foreign = id && audio::synthEngineFor(id);
        g.text(x, 103, kFont3, th.slot(slot::kFocusFrame), foreign ? "Patch of another model" : "Not available",
               IDisplay::kOptCenterY);
    }
    wg::panelFocus(g, th, wg::columnX(param), wg::columnW());
}

void SynthScreen::drawMacros(Gfx& g) {
    const Theme& th = ui_.th();
    const SynthSlotRecord r = app_.synthSlot(slot);
    const SynthPatch& p = app_.synthPatch(slot);
    const audio::SynthSlotView sv = app_.engine.synth().slotView(slot);
    const bool ro = !available();
    title(g, "Synth Macros 2/2");
    for (int k = 0; k < audio::kSynthSeqMacros; ++k) {
        const int val = sv.overridden[k] ? sv.macro[k] : r.macros[k];
        const bool bi = p.macros[k].bipolar;
        const float v = bi ? clampf(float(val - 63) / 64.0f, -1.0f, 1.0f) : float(val) / 127.0f;
        // An FX CC override (sequencer) shows in yellow until Stop or a hand edit.
        barColumn(g, th, k, v, bi, !ro, kMacroBarH, sv.overridden[k] ? th.slot(slot::kYellow) : wg::kThemeColour);
    }
    barColumn(g, th, kArpGateColumn, float(app_.slotArpGate(slot)) / 100.0f, false, !ro, kMacroBarH);
    wg::panel(g, th, wg::columnX(7), wg::kPanelY, wg::columnW(), kMacroBarH);
    if (!ro) wg::focusFrame(g, th, wg::columnX(macro), 31, wg::columnX(macro) + wg::columnW(), wg::kPanelY + kMacroBarH - 1);
    // Legend (manual p.149): "Macro k   <name>" in two columns.
    wg::panel(g, th, 1, kLegendY, 797, wg::kPanelY + wg::kPanelH - kLegendY);
    const uint32_t dim = th.slot(slot::kBottomLabel), white = th.slot(slot::kList + 4);
    for (int k = 0; k < audio::kSynthSeqMacros; ++k) {
        const int x = 21 + 400 * (k / 3), y = kLegendY + 26 + 32 * (k % 3);
        char lab[20], name[40];
        std::snprintf(lab, sizeof(lab), "Macro %d", k + 1);
        std::snprintf(name, sizeof(name), "%s", p.macros[k].name[0] ? shown(p.macros[k].name) : "-");
        fit(name, kFont3, 240);
        g.text(x, y, kFont3, dim, lab, IDisplay::kOptCenterY);
        g.text(x + 110, y, kFont3, k == macro ? white : dim, name, IDisplay::kOptCenterY);
    }
}

void SynthScreen::drawEdit(Gfx& g) {
    const Theme& th = ui_.th();
    const SynthPatch& p = app_.synthPatch(slot);
    const SynthSlotRecord r = app_.synthSlot(slot);
    const int32_t eng = engine();
    char t[64];
    std::snprintf(t, sizeof(t), "Edit Patch: %s", r.patchName[0] ? shown(r.patchName) : "-");
    title(g, t);
    // Section grid (4 x 4, mirrors the lit pads), columns 0..3.
    wg::panel(g, th, 1, wg::kPanelY, wg::columnW(4), wg::kPanelH);
    const synthui::Section* sec = nullptr;
    const int n = synthui::sections(eng, &sec);
    for (int i = 0; i < n; ++i) {
        const int x = 9 + 97 * (i % 4), y = 40 + 64 * (i / 4);
        wg::key(g, th, x, y, 91, 56, sec[i].name, i == section, wg::kThemeColour, th.slot(slot::kFocusFrame));
    }
    // Parameter list above F5..F7: label left, value right.
    const int lx = wg::columnX(4), lw = wg::columnW(3);
    const int nr = rows();
    wg::ListView v;
    v.x = lx;
    v.w = lw;
    v.count = nr;
    v.sel = row;
    listTop_ = wg::listFollow(row, nr, wg::listVisibleRows(v.h), listTop_);
    v.top = listTop_;
    v.selStyle = wg::ListSel::kWhite;
    wg::list(g, th, v);   // background, selection and scroll bar; the rows are drawn here
    const bool macroSec = section < n && sec[section].macro >= 0;
    const int innerW = lw - (nr > wg::listVisibleRows(v.h) ? 13 : 6);
    const uint32_t white = th.slot(slot::kList + 4), dim = th.slot(slot::kBottomLabel);
    for (int k = 0; k < wg::listVisibleRows(v.h) && listTop_ + k < nr; ++k) {
        const int i = listTop_ + k;
        char lab[40], val[40];
        if (macroSec) macroRowText(sec[section].macro, i, lab, sizeof(lab), val, sizeof(val));
        else {
            const int idx = sec[section].params[i];
            const synthui::ParamUi* u = synthui::paramUi(eng, idx);
            std::snprintf(lab, sizeof(lab), "%s", u ? u->label : "?");
            synthui::formatValue(eng, idx, audio::synthParamGet(p, idx), val, sizeof(val));
        }
        fit(val, kFont1, innerW / 2 - 8);
        const int vw = textWidth(kFont1, val);
        fit(lab, kFont1, innerW - vw - 30);
        const int y = v.y + 17 + wg::kListRowH * k;
        g.text(lx + 13, y, kFont1, white, lab, IDisplay::kOptCenterY);
        g.text(lx + innerW - 6, y, kFont1, i == row ? white : dim, val, IDisplay::kOptCenterY | IDisplay::kOptRightX);
    }
    // The selected value above F8 (manual p.152 "above screen button 8").
    const bool ro = !available();
    if (section >= n || row >= nr) {
        wg::panelColumn(g, th, 7);
    } else if (macroSec) {
        const PatchMacro& mc = p.macros[sec[section].macro];
        const int count = audio::synthMacroTargets(mc);
        if (row == 0) {
            // The Name row's "Edit" key above F8 (Mini manual p.155).
            wg::panelColumn(g, th, 7);
            wg::key(g, th, wg::columnX(7) + 6, 40, wg::columnW() - 12, 56, "Edit", !ro, wg::kThemeColour,
                    th.slot(slot::kFocusFrame));
        } else if (row == 1) {
            listColumn(g, th, 7, 1, kOffOn, 2, mc.bipolar ? 1 : 0);
        } else if (row >= 2 && (row - 2) % 2 == 1 && (row - 2) / 2 < count && mc.targets[(row - 2) / 2].param >= 0) {
            barColumn(g, th, 7, patchBitsToFloat(mc.targets[(row - 2) / 2].amountBits) / 2.0f, true, !ro);
        } else {
            wg::panelColumn(g, th, 7);
        }
    } else {
        const int idx = sec[section].params[row];
        const synthui::ParamUi* u = synthui::paramUi(eng, idx);
        const float val = audio::synthParamGet(p, idx);
        if (u && u->fmt == synthui::kFmtEnum && u->names) {
            listColumn(g, th, 7, 1, u->names, u->nameCount, clampi(int(std::lround(val)), 0, u->nameCount - 1));
        } else if (u && u->fmt == synthui::kFmtOnOff) {
            listColumn(g, th, 7, 1, kOffOn, 2, val >= 0.5f ? 1 : 0);
        } else if (synthui::valueBipolar(eng, idx)) {
            barColumn(g, th, 7, synthui::valueFrac(eng, idx, val) * 2.0f - 1.0f, true, !ro);
        } else {
            barColumn(g, th, 7, synthui::valueFrac(eng, idx, val), false, !ro);
        }
    }
    (void)dim;
    // F5 / F7 of the bottom bar: the row arrows (manual p.153).
    const uint32_t arrow = th.slot(slot::kBottomLabel);
    g.icon(450 - kIconSize / 2, 451 - kIconSize / 2, kIconArrowUp, arrow);
    g.icon(650 - kIconSize / 2, 451 - kIconSize / 2, kIconArrowDown, arrow);
}

// Load Patch popup (Tracker+ 1.0 frames, V1 3:10-3:22): Synth Parameters 1/2
// dimmed under it, the list at x 600..799 under the "Load Patch" header band
// (Ui, kRightBar), the loaded patch marked.
void SynthScreen::drawPresets(Gfx& g) {
    const Theme& th = ui_.th();
    wg::ListView v;
    title(g, "Synth Presets");
    v.x = 0;
    v.y = wg::kPanelFillTop;
    v.w = kScreenW;
    v.h = wg::kPanelH;
    v.count = kSynthPresetCount;
    v.sel = row;
    v.selStyle = wg::ListSel::kWhite;
    v.item = [](void* c, int i, char* out, int cap) {
        auto* s = static_cast<SynthScreen*>(c);
        const int n = i + 1;
        const char* mark = s->app_.synthPresetActive(s->slot) == n ? "* " : "";
        if (!s->app_.synthPresetUsed(n)) {
            std::snprintf(out, size_t(cap), "%sP%d  -", mark, n);
            return;
        }
        const int32_t e = s->app_.synthPreset(n).engine;
        std::snprintf(out, size_t(cap), "%sP%d  %s", mark, n,
                      audio::synthEngineFor(e) ? synthEngineDisplayName(e) : "not available");
    };
    v.ctx = this;
    wg::list(g, th, v);
}

// Back to what the slot played before the held preview.
void SynthScreen::endPresetPreview() {
    previewKey_ = -1;
    app_.engine.synth().auditionOff();
    app_.engine.synth().presetSelect(slot, previewPrev_);
}

void SynthScreen::loadPresetNow() {
    if (!app_.loadSynthPreset(slot, row + 1)) {
        ui_.notify("Engine not available");
        return;
    }
    edited_[slot] = false;
    section = 0;
    ui_.notify("Preset loaded");
}

void SynthScreen::presetKey(uint8_t id) {
    const int n = row + 1;
    const bool used = app_.synthPresetUsed(n);
    switch (id) {
        case btn::F1:
            if (!available()) break;
            if (!used) {
                app_.storeSynthPreset(n, slot);
                ui_.notify("Preset stored");
                break;
            }
            ui_.ask("Store Preset", "Replace this preset?", nullptr, "No", "Yes",
                    [](void* c, int key) {
                        auto* s = static_cast<SynthScreen*>(c);
                        if (key != btn::F8) return;
                        s->app_.storeSynthPreset(s->row + 1, s->slot);
                        s->ui_.notify("Preset stored");
                        s->refreshBar();
                    },
                    this);
            break;
        case btn::F2: {
            if (!used) break;
            if (!edited_[slot]) {
                loadPresetNow();
                break;
            }
            ui_.ask("Load Preset", "Discard patch edits?", nullptr, "No", "Yes",
                    [](void* c, int key) {
                        auto* s = static_cast<SynthScreen*>(c);
                        if (key != btn::F8) return;
                        s->loadPresetNow();
                        s->refreshBar();
                    },
                    this);
            break;
        }
        case btn::F3:
            if (!used) break;
            ui_.ask("Clear Preset", "Clear this preset?", nullptr, "No", "Yes",
                    [](void* c, int key) {
                        auto* s = static_cast<SynthScreen*>(c);
                        if (key != btn::F8) return;
                        s->app_.clearSynthPreset(s->row + 1);
                        s->refreshBar();
                    },
                    this);
            break;
        case btn::F5:
        case btn::F6:
            if (!used || !available()) break;
            previewPrev_ = uint8_t(app_.synthPresetActive(slot));
            previewKey_ = id;
            app_.engine.synth().presetSelect(slot, uint8_t(n));
            audition(true, id == btn::F5 ? 48 : 72);
            break;
        case btn::F8:
            view = View::Macros;
            break;
        case btn::Up:
        case btn::Down:
            row = clampi(row + (id == btn::Down ? 1 : -1), 0, kSynthPresetCount - 1);
            break;
        default:
            if (!btn::isFKey(id) && id != btn::Left && id != btn::Right) Screen::onButton(id, kPress);
            break;
    }
}

void SynthScreen::drawBrowser(Gfx& g) {
    const Theme& th = ui_.th();
    drawParams(g);
    wg::blendFill(g, 0, wg::kPanelFillTop, wg::kRightPopupX, wg::kStripY - wg::kPanelFillTop, 0x000000, 160);
    g.fill(wg::kRightPopupX + 1, wg::kPanelFillTop, kScreenW - wg::kRightPopupX - 1, wg::kStripY - wg::kPanelFillTop,
           th.slot(slot::kEmptyPanel + 1));
    wg::ListView v;
    v.x = wg::kRightPopupX;
    v.y = wg::kPanelFillTop;
    v.w = kScreenW - wg::kRightPopupX;
    v.h = wg::kPanelH;
    v.count = browserCount;
    v.sel = browserCount ? browserSel : -1;
    browserTop_ = wg::listFollow(browserSel, browserCount, wg::listVisibleRows(v.h), browserTop_);
    v.top = browserTop_;
    v.background = false;
    v.selStyle = wg::ListSel::kWhite;
    v.item = [](void* c, int i, char* out, int cap) {
        std::snprintf(out, size_t(cap), "%s", static_cast<SynthScreen*>(c)->browser[i]);
    };
    v.marked = [](void* c, int i) {
        auto* s = static_cast<SynthScreen*>(c);
        return std::strcmp(s->browser[i], s->app_.synthSlot(s->slot).patchName) == 0;
    };
    v.ctx = this;
    wg::list(g, th, v);
    if (!browserCount) g.text(wg::kRightPopupX + 100, 60, kFont3, th.slot(slot::kBottomLabel), "No patches", IDisplay::kOptCenter);
}

void SynthScreen::update(uint32_t now) {
    Screen::update(now);
    if (view == View::Presets) {   // a Z in the pattern moves the marker
        const uint32_t z = uint32_t(app_.synthPresetActive(slot));
        if (z != shownMacros_) {
            shownMacros_ = z;
            ui_.dirty();
        }
        return;
    }
    if (view != View::Macros) return;
    const SynthSlotRecord r = app_.synthSlot(slot);
    const audio::SynthSlotView sv = app_.engine.synth().slotView(slot);
    uint32_t shown = 0;
    for (int k = 0; k < audio::kSynthSeqMacros; ++k)
        shown = shown * 257u + (sv.overridden[k] ? 128u + sv.macro[k] : r.macros[k]);
    if (shown != shownMacros_) {
        shownMacros_ = shown;
        ui_.dirty();
    }
}

void SynthScreen::draw(Gfx& g) {
    refreshBar();   // FX CC overrides and the governor cap change from outside
    switch (view) {
        case View::Params: drawParams(g); break;
        case View::Macros: drawMacros(g); break;
        case View::Edit: drawEdit(g); break;
        case View::Browser: drawBrowser(g); break;
        case View::Presets: drawPresets(g); break;
        case View::Insert:
#if OT_SLOT_INSERTS
            drawInsert(g);
#endif
            break;
    }
}

}  // namespace ot
