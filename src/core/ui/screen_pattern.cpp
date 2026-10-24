// Pattern editor (spec 06 §1).
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "app/app.h"
#include "app/project_fields.h"
#include "app/render.h"
#include "ui/fx_text.h"
#include "ui/notes.h"
#include "sequencer/seq_tables.h"
#include "ui/screens.h"

namespace ot {

namespace {
constexpr int kVisibleRows = 15;                    // spec 06 §1.2
constexpr int kVisibleTracks = 4;
constexpr int kStepColW = 40;
constexpr int kHeaderH = 26;                       // track header row inside the grid
constexpr int kRowHInfo = 26;                      // 15 rows of 26 px in the 395-px grid (top info on)
constexpr int kRowHOff = 28;                       // 15 rows of 28 px in the 423-px grid (top info off)
constexpr int kMaskTracks = 8;                     // spec 06 §1.2/§1.9: 8 tracks with a mask
constexpr int kHTrackLabelW = 50;                  // horizontal arrangement: track label column
// Column widths in characters: note "C-5", instrument "01 "/"M01", FX
// "V100" (spec 06 §1.2 cell text, spec 16 §A.8; ui/fx_text.h).
constexpr int kColW[4] = {3, 3, kFxCellChars, kFxCellChars};
// spec 06 §1.5 table 0xa71e4: direction codes for Up, Down, Right, Left per
// arrangement (0 = step-1, 1 = step+1, 2 = track+1, 3 = track-1).
constexpr uint8_t kArrowDir[3][4] = {{0, 1, 2, 3}, {0, 1, 2, 3}, {3, 2, 1, 0}};
constexpr uint8_t kLenPresets[] = {1, 2, 4, 8, 16, 32, 48, 64, 80, 96, 128};   // spec 06 §1.7
constexpr uint8_t kJumpPresets[] = {0, 1, 2, 4, 8, 16, 32};
constexpr uint32_t kNoteHoldMs = 500;              // spec 06 §0.4 item 2
constexpr uint32_t kColHoldMs = 180;

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Grid geometry for the current column-view mask (spec 06 §1.2, §1.9):
// 4 tracks with all columns, or 8 tracks with the masked columns only.
struct CellLayout {
    int tracks = kVisibleTracks;
    int trackW = (kScreenW - kStepColW) / kVisibleTracks;
    int off[4] = {-1, -1, -1, -1};   // char offset per column, -1 = hidden
    int w[4] = {0, 0, 0, 0};
    int chars = 0;
    int textOff = 0;                 // x offset that centres the line in the track
};

CellLayout cellLayout(uint8_t mask) {
    CellLayout L;
    if (mask) {
        L.tracks = kMaskTracks;
        L.trackW = (kScreenW - kStepColW) / kMaskTracks;
    }
    int c = 0;
    for (int k = 0; k < 4; ++k) {
        if (mask && !(mask & (1u << k))) {
            L.off[k] = -1;
            L.w[k] = 0;
            continue;
        }
        if (c) ++c;
        L.off[k] = c;
        L.w[k] = kColW[k];
        c += L.w[k];
    }
    L.chars = c;
    L.textOff = (L.trackW - L.chars * kMonoW) / 2;
    return L;
}

int presetStep(const uint8_t* tab, int n, int cur, int dir) {
    if (dir > 0) {
        for (int i = 0; i < n; ++i)
            if (tab[i] > cur) return tab[i];
        return tab[n - 1];
    }
    for (int i = n - 1; i >= 0; --i)
        if (tab[i] < cur) return tab[i];
    return tab[0];
}

int colForKey(uint8_t id) {
    switch (id) {
        case btn::Note: return 0;
        case btn::Instr: return 1;
        case btn::Fx1: return 2;
        case btn::Fx2: return 3;
        default: return -1;
    }
}

uint32_t rnd(void* ctx) {
    auto* s = static_cast<uint32_t*>(ctx);
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

void noteItem(void*, int i, char* out, int cap) {
    char n[4];
    noteName(int8_t(i), n);
    std::snprintf(out, size_t(cap), "%s", n);
}
// i = popup position (synthui::popupInstrument: Synth 1..3 first).
void instrItem(void* ctx, int pos, char* out, int cap) {
    App* a = static_cast<App*>(ctx);
    const int i = synthui::popupInstrument(pos);
    if (i < kSampleInstrumentCount) std::snprintf(out, size_t(cap), "%02d %s", i + 1, a->instrPresent[i] ? a->instruments[i].name : "");
    else if (isSynthInstrument(uint8_t(i))) std::snprintf(out, size_t(cap), "S%d", synthSlotOf(uint8_t(i)) + 1);
    else std::snprintf(out, size_t(cap), "MIDI %d", i - kFirstMidiInstrument + 1);
}
// i = MIDI-track popup position (synthui::popupInstrumentMidi: Synth 1..3 first).
void midiItem(void*, int pos, char* out, int cap) {
    const int i = synthui::popupInstrumentMidi(pos);
    if (isSynthInstrument(uint8_t(i))) std::snprintf(out, size_t(cap), "S%d", synthSlotOf(uint8_t(i)) + 1);
    else std::snprintf(out, size_t(cap), "MIDI %d", i - kFirstMidiInstrument + 1);
}
// FX list items carry the column letter, "V - Volume/Velocity" (stock FX
// picker strings 0xa3160..; docs/STRINGS_DIFF_1.9.3.md item 8).
void fxItem(void*, int i, char* out, int cap) {
    const FxInfo& f = fxInfo(kFxMenuOrder[i]);
    std::snprintf(out, size_t(cap), "%c - %s", f.symbol, f.name);
}

// Short FX names of the pattern top bar (stock table 0xa7730, 36-byte
// entries; docs/STRINGS_DIFF_1.9.3.md item 9). Types not listed there use
// the menu name.
const char* fxShortName(uint8_t type) {
    switch (FxId(type)) {
        case FxId::RandomNote: return "Rnd Note";
        case FxId::RandomInstrument: return "Rnd Instrument";
        case FxId::RandomFxValue: return "Rnd FX value";
        case FxId::RandomVolume: return "Rnd Volume";
        case FxId::ReversePlayback: return "Rev Playback";
        case FxId::FilterLfo: return "Filt LFO";
        case FxId::FinetuneLfo: return "F.tune LFO";
        case FxId::MicroTune: return "Micro-tune";
        // SPEC GAP: the strings diff lists "Slide up/down"; one label per
        // direction is assumed.
        case FxId::SlideUp: return "Slide up";
        case FxId::SlideDown: return "Slide down";
        default: return fxInfo(type).name;
    }
}

// Note popup F-key note window (stock popup handler 0x2b52c, labels set by
// 0x2b238; docs/STRINGS_DIFF_1.9.3.md item 1). F1..F8 play/pick base+0..7.
constexpr int kNoteKeys = 8;
constexpr int kNoteBaseMax = 127 - (kNoteKeys - 1);
constexpr uint16_t kNoteKeyVoice = 0x70;   // audition ids 0x70..0x77 (free range, app.h ids)

// Fill option names (spec 16 §B.3).
const char* kWhereNames[7] = {"Note", "No Note", "Fx", "No Fx", "Random", "Each", "Euclidean"};
const char* kFillTypes[3] = {"Constant", "From-To", "Random"};
uint32_t gRnd = 12345;
}  // namespace

bool PatternScreen::midiChordEntry(int t, uint8_t note, uint8_t velocity) {
    // spec 17 §1.4 step 8 (lg:0xc958c): the MIDI note already sounds on track
    // t (first free track from the target start). With edit mode on, the
    // sequencer not playing and no popup open, it is written at (startStep, t)
    // when t lies in the selected track range, so a chord fills the selected
    // tracks on one row; with a single-cell cursor only the first note is
    // written. The cursor does not move.
    App& a = app_;
    // Gated on the play state only (0xc916c); an armed, stopped sequencer
    // still takes step entry (spec 17 §1.4 step 8).
    if (!edit() || a.seq.isPlaying()) return false;
    if (focus != -1 || fillOpen || pop_.isOpen() || ui_.kb.isOpen() || ui_.askOpen()) return false;
    const SelRect r = target();
    if (t < r.t0 || t > r.t1) return false;
    if (note > 127) note = 60;   // spec 06 §1.6: a note > 127 becomes 60
    SelRect c;
    c.s0 = c.s1 = r.s0;
    c.t0 = c.t1 = t;
    a.pushUndo();
    // Instrument mapped to the track type (sample instrument on tracks >= 9
    // -> MIDI channel 1; spec 06 §1.6 0x133f0).
    pops::writeNote(a.pattern(), c, note, a.currentInstrument());
    // Velocity recording mode: FX-B = Volume, round(vol * 100 / 127).
    if (a.cfg.recordingOptions == 2 || a.cfg.recordingOptions == 3) {
        Step& st = a.pattern().tracks[t].steps[r.s0];
        st.fx[0].type = 18;
        st.fx[0].value = uint8_t((a.liveVolume(a.currentInstrument(), velocity) * 100 + 63) / 127);
    }
    a.project.setU8(pf::kLastNote, note);
    a.markProjectDirty();
    a.patternEdited();
    // 2.x: a note into a single cell moves the cursor by the step jump, as
    // a pad does (user request; stock keeps the cursor). A chord across a
    // selection fills its tracks on one row and stays.
    if (r.single()) advance();
    ui_.dirty();
    return true;
}

int PatternScreen::len() const { return pops::length(app_.pattern()); }
bool PatternScreen::edit() const { return app_.editMode; }
bool PatternScreen::recordCursor() const { return edit() || app_.seq.isRecording(); }
int PatternScreen::lastNote() const {
    int8_t n = int8_t(app_.project.u8(pf::kLastNote));
    return n < 0 ? 48 : n;
}

void PatternScreen::start(int opt) {
    (void)opt;
    focus = -1;
    fillOpen = false;
    recArmPage = false;
    colKey_ = 0;
    pickerKey_ = 0;
    for (auto& v : padVoice_) v = -1;
    if (step >= len()) step = len() - 1;
    statusStep_ = step;   // Ui resets the status cursor step before start()
    ui_.setStatusCursorStep(step);
    refreshBar();
}

void PatternScreen::stop() {
    app_.previewAllOff();
    noteKeysOff();
    if (pop_.isOpen()) ui_.closePopupHeader();
    pop_.close();
    pickerKey_ = 0;
    focus = -1;
}

SelRect PatternScreen::target() const {
    SelRect r;
    const int l = len();
    if (allTracks) {
        r.s0 = 0;
        r.s1 = l - 1;
        r.t0 = 0;
        r.t1 = kTrackCount - 1;
    } else if (wholeCol) {
        r.s0 = 0;
        r.s1 = l - 1;
        r.t0 = anchorTrack < track ? anchorTrack : track;
        r.t1 = anchorTrack < track ? track : anchorTrack;
    } else if (selState == 2) {
        r.s0 = anchorStep < step ? anchorStep : step;
        r.s1 = anchorStep < step ? step : anchorStep;
        r.t0 = anchorTrack < track ? anchorTrack : track;
        r.t1 = anchorTrack < track ? track : anchorTrack;
    } else {
        r.s0 = r.s1 = step;
        r.t0 = r.t1 = track;
    }
    if (r.s1 >= kStepCount) r.s1 = kStepCount - 1;
    return r;
}

void PatternScreen::collapse() {
    selState = 1;
    wholeCol = false;
    allTracks = false;
}

void PatternScreen::keepTrackVisible() {
    const int vis = colMask ? 8 : kVisibleTracks;
    if (track < firstTrack) firstTrack = track;
    if (track >= firstTrack + vis) firstTrack = track - vis + 1;
    firstTrack = clampi(firstTrack, 0, kTrackCount - vis);
}

void PatternScreen::advance(int extraRows) {
    // spec 06 §1.5 "Advance after entry".
    const int jump = app_.stepJump();
    collapse();
    if (jump == 0 && extraRows == 0) return;
    int ns = step + jump + extraRows;
    const int l = len();
    while (ns > l - 1) ns -= l;
    step = ns < 0 ? 0 : ns;
}

void PatternScreen::moveCursor(int dir, bool shiftHeld) {
    // spec 06 §1.5.
    const int l = len();
    const bool playingFollow = !edit() && app_.seq.isPlaying();
    if (!shiftHeld) {
        collapse();
        switch (dir) {
            case 0:
                if (!playingFollow) step = step == 0 ? l - 1 : step - 1;
                break;
            case 1:
                if (!playingFollow) step = step >= l - 1 ? 0 : step + 1;
                break;
            case 2: track = clampi(track + 1, 0, kTrackCount - 1); break;
            case 3: track = clampi(track - 1, 0, kTrackCount - 1); break;
        }
        keepTrackVisible();
        return;
    }
    // Shift: extend the selection.
    if (dir == 0 && step == 0) {
        if (!wholeCol && !allTracks) {
            wholeCol = true;
            anchorTrack = track;
            selState = 2;
        } else if (wholeCol) {
            allTracks = true;
        }
        return;
    }
    if (wholeCol && ((dir == 3 && track == 0) || (dir == 2 && track == kTrackCount - 1))) {
        allTracks = true;
        return;
    }
    if (selState == 1) {
        selState = 2;
        anchorTrack = track;
        anchorStep = step;
    }
    switch (dir) {
        case 0: step = clampi(step - 1, 0, l - 1); break;
        case 1: step = clampi(step + 1, 0, l - 1); break;
        case 2: track = clampi(track + 1, 0, kTrackCount - 1); break;
        case 3: track = clampi(track - 1, 0, kTrackCount - 1); break;
    }
    keepTrackVisible();
}

ColMode PatternScreen::heldColMode(bool forDelete) const {
    if (ui_.isDown(btn::Pattern)) return ColMode::Pattern;
    if (ui_.isDown(btn::Note)) return ColMode::Note;
    if (ui_.isDown(btn::Instr)) return ColMode::Instr;
    if (ui_.isDown(btn::Fx2)) return ColMode::Fx2;
    if (ui_.isDown(btn::Fx1)) return ColMode::Fx1;
    if (forDelete) {
        // spec 06 §1.11: without a held key Delete clears the cursor column (Instr: whole step).
        switch (col) {
            case 0: return ColMode::Note;
            case 2: return ColMode::Fx1;
            case 3: return ColMode::Fx2;
            default: return ColMode::All;
        }
    }
    return ColMode::All;
}

void PatternScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    char v[24];
    b.clear();
    if (fillOpen) {
        // Fill bar (spec 16 §B.3): the list fields F1/F3/F4 show their
        // lists above the bar (drawFill); F2/F5/F6 carry values.
        const pops::FillParams& f = fill[col];
        b.set(0, "Where?");
        if (f.where >= 4) {
            std::snprintf(v, sizeof(v), f.where == 4 ? "%d %%" : "%d", f.param[f.where]);
            b.set(1, f.where == 4 ? "Density" : (f.where == 5 ? "Step" : "Events"), v);
        } else {
            b.set(1, "", nullptr, false);
        }
        if (col == 0) b.set(2, "Scale");
        else if (col == 1) b.set(2, "", nullptr, false);
        else {
            // Two-line "Fx Type / <symbol>" [S snap 019]; "?" = random type.
            const char sym[2] = {f.scaleOrFx < 0 ? '?' : fxInfo(fillFxType()).symbol, 0};
            b.set(2, "Fx Type", sym);
        }
        b.set(3, "Fill Type");
        for (int k = 0; k < 2; ++k) {
            if (k == 1 && f.type == 0) {
                b.set(5, "", nullptr, false);
                continue;
            }
            fillValueText(k ? f.to : f.from, v);
            const char* constName = col == 0 ? "Note" : (col == 1 ? "Instrument" : "Fx Value");
            b.set(4 + k, k ? "To" : (f.type == 0 ? constName : "From"), v);
        }
        b.set(6, "Cancel");
        b.set(7, "Fill");
        // The selected field is framed on its column panel, not in the bar
        // [S snap 019].
        return;
    }
    if (pop_.isOpen() && pop_.kind() == 0) {
        // Note popup: 8 note names from the window base; with Shift held,
        // F1/F2/F7/F8 are the window shifts (stock Shift handler 0x3f13c
        // re-labels the open popup).
        const bool sh = ui_.shift();
        static const char* const kShiftLabels[kNoteKeys] = {"Oct-", "Note-", nullptr, nullptr,
                                                            nullptr, nullptr, "Note+", "Oct+"};
        for (int i = 0; i < kNoteKeys; ++i) {
            if (sh && kShiftLabels[i]) {
                b.set(i, kShiftLabels[i]);
                continue;
            }
            const int n = noteBase_ + i;
            char nm[4];
            noteName(int8_t(n), nm);
            b.set(i, nm);
            b.highlight(i, n == pop_.cursor());
        }
        return;
    }
    if (recArmPage) {
        const int base = track >= 8 ? 8 : 0;
        for (int i = 0; i < 8; ++i) {
            const int t = base + i;
            if (t >= kTrackCount) {
                b.set(i, "", nullptr, false);
                continue;
            }
            // Stock labels: "Live REC" on every top line, "Armed" /
            // "Unarmed" below (0x42314; docs/STRINGS_DIFF_1.9.3.md item 5).
            b.set(i, "Live REC", armed[t] ? "Armed" : "Unarmed");
            b.highlight(i, armed[t]);
        }
        return;
    }
    if (ui_.shift() && focus < 0 && !pop_.isOpen()) {
        // spec 06 §1.3 mute page.
        const int base = track >= 8 ? 8 : 0;
        for (int i = 0; i < 8; ++i) {
            const int t = base + i;
            if (t >= kTrackCount) {
                b.set(i, "", nullptr, false);
                continue;
            }
            char nm[24];
            app_.project.trackName(t, nm, sizeof(nm));
            b.set(i, nm, app_.trackMuted(t) ? "Unmute" : "Mute");
            b.highlight(i, app_.trackMuted(t));
        }
        return;
    }
    const bool e = edit();
    switch (page) {
        case 0: {
            const int queued = app_.seq.queuedPattern();
            std::snprintf(v, sizeof(v), app_.cfg.numbering == 2 ? "%02X" : "%d", queued ? queued : app_.patternNumber());
            b.set(0, "Pattern", v);
            std::snprintf(v, sizeof(v), app_.cfg.numbering == 2 ? "%02X" : "%d", len());
            b.set(1, "Length", v);
            std::snprintf(v, sizeof(v), "%d", app_.stepJump());
            b.set(2, "Step jump", v);
            b.set(3, "Fill", nullptr, e);
            b.set(4, "Preview");
            b.set(5, "Undo", nullptr, e);
            b.set(6, "Redo", nullptr, e);
            b.set(7, "More");
            // Underlines (spec 20 §1.2 part 3; Part 2 C7): green = available,
            // red = edit mode only (enabled in Rec, grey 333333 otherwise).
            for (int i : {0, 1, 2, 4}) b.underline(i, wg::kUnderlineAvailable);
            for (int i : {3, 5, 6}) b.underline(i, wg::kUnderlineEditMode);
            b.pages(7, 3, page);
            if (focus == 0) b.highlight(0, true);
            else if (queued) b.highlight(0, blink_);   // 200 ms blink (update)
            if (focus == 1) b.highlight(1, true);
            if (focus == 2) b.highlight(2, true);
            if (focus == 5) b.highlight(5, true);
            if (focus == 6) b.highlight(6, true);
            break;
        }
        case 1:
            b.setName(0, "Render", "selection", e);
            b.set(1, "Invert", nullptr, e);
            b.set(2, "Duplicate", nullptr, e);
            b.set(3, "Expand", nullptr, e);
            b.set(4, "Shrink", nullptr, e);
            b.setName(5, "Copy", "pattern", e);
            b.setName(6, "Paste", "pattern", e);
            b.set(7, "More");
            for (int i = 0; i < 7; ++i) b.underline(i, wg::kUnderlineEditMode);
            b.pages(7, 3, page);
            break;
        default:
            b.setName(0, "Pattern", "name");
            b.setName(1, "Track", "name");
            b.set(7, "More");
            b.underline(0, wg::kUnderlineAvailable);
            b.underline(1, wg::kUnderlineAvailable);
            b.pages(7, 3, page);
            break;
    }
    if (pop_.isOpen() && pop_.kind() == 3) {
        // FX list: "Description" over F7/F8 (spec 16 §B.4); F1..F6 keep
        // the page labels beside the popup [S snap 018].
        b.setWide(6, "Description");
        b.underline(6, wg::kUnderlineNone);
        b.highlight(6, pop_.descriptionOpen());
    }
}

// ---------------------------------------------------------------------------
// Input

void PatternScreen::onButton(uint8_t id, uint8_t state) {
    App& a = app_;
    if (patternHeld_ && id != btn::Pattern && state == kPress) patternCombo_ = true;
    if (fillOpen) {
        if (state == kPress) fillButton(id);
        refreshBar();
        return;
    }
    if (pop_.isOpen()) {
        // spec 06 §1.6 step 3: only the opening column key stays bound; the
        // FX list (kind 3) also binds F7/F8 to its description window
        // (spec 05 popup binder 0x29a44; spec 16 §B.4).
        if (id == pickerKey_ && state == kRelease) commitPicker();
        else if (state == kPress && popupMove(id)) ui_.dirty();
        else if (pop_.kind() == 0 && btn::isFKey(id)) noteKey(id, state);
        else if (pop_.kind() == 3 && state == kPress && (id == btn::F7 || id == btn::F8))
            pop_.toggleDescription();
        refreshBar();   // re-highlights "Description" / restores the bar on close
        return;
    }
    if (btn::isFKey(id)) {
        const bool muteOverlay = ui_.shift() && focus < 0;
        if (recArmPage && state == kPress) {
            const int t = (track >= 8 ? 8 : 0) + id;
            if (t < kTrackCount) {
                armed[t] = !armed[t];
                a.seq.setRecordEnabled(t, armed[t]);
            }
        } else if (muteOverlay && state == kPress) {
            const int t = (track >= 8 ? 8 : 0) + id;
            if (t < kTrackCount) a.setTrackMute(t, !a.trackMuted(t));
        } else if (!muteOverlay) {
            pageButton(id, state);
        }
        refreshBar();
        return;
    }
    const int ck = colForKey(id);
    if (ck >= 0) {
        if (state == kPress) {
            if (ui_.isDown(btn::Pattern)) {
                // Column-view mask (spec 06 §1.9): last two pressed columns.
                // SPEC GAP (exact UX inferred): pressing the same column key
                // twice in a row while Pattern is held clears the mask.
                if (maskKeys_[0] == ck && colMask == uint8_t(1u << ck)) {
                    colMask = 0;
                    maskKeys_[0] = maskKeys_[1] = -1;
                } else {
                    maskKeys_[1] = maskKeys_[0];
                    maskKeys_[0] = ck;
                    colMask = uint8_t(1u << ck);
                    if (maskKeys_[1] >= 0 && maskKeys_[1] != ck) colMask |= uint8_t(1u << maskKeys_[1]);
                }
            } else {
                if (colMask && !(colMask & (1u << ck))) colMask = uint8_t(1u << ck);
                col = ck;
                if (!ui_.shift()) {
                    colKey_ = id;
                    colKeyAt_ = ui_.now();
                }
            }
        } else if (state == kRelease) {
            if (colKey_ == id) colKey_ = 0;
        }
        keepTrackVisible();
        refreshBar();
        return;
    }
    switch (id) {
        case btn::Pattern:
            // Manual (Quick Reference): Pattern + column keys pick the
            // column view; pressing Pattern alone goes back to the 4-track
            // view (user report: the view could not be left).
            if (state == kPress) {
                patternCombo_ = false;
            } else if (state == kRelease) {
                if (patternHeld_ && !patternCombo_ && colMask) {
                    colMask = 0;
                    keepTrackVisible();
                }
                maskKeys_[0] = maskKeys_[1] = -1;
            }
            patternHeld_ = state != kRelease;
            break;
        case btn::Play:
            if (state == kPress) transport();
            break;
        case btn::Rec:
            if (state != kPress) break;
            // spec 06 §1.4 Rec (lg 0x3edec).
            if (ui_.shift()) {
                recArmPage = !recArmPage;
            } else if (a.seq.isRecording()) {
                // Live recording running: recording off (0x111c0(seq,0)),
                // the transport keeps playing; leave edit mode.
                a.seq.setRecording(false);
                a.editMode = false;
            } else {
                a.editMode = !a.editMode;
                recArmPage = false;
                // A playing selection preview collapses to the cursor.
                if (a.seq.isSelectionPlaying()) collapse();
            }
            break;
        case btn::Shift:
            if (state == kRelease) recArmPage = false;   // overlays end with Shift
            break;
        case btn::Insert:
            if (state != kPress) break;
            if (ui_.shift()) {
                step = 0;
                collapse();
            } else if (edit()) {
                a.pushUndo();
                pops::insertRows(a.pattern(), target());
                a.patternEdited();
            }
            break;
        case btn::Delete:
            if (state != kPress || !edit()) break;
            if (ui_.shift()) {
                SelRect r = target();
                a.pushUndo();
                if (pops::backspaceRows(a.pattern(), r)) {
                    step = step > 0 ? step - 1 : 0;
                    if (selState == 2) anchorStep = anchorStep > 0 ? anchorStep - 1 : 0;
                    a.patternEdited();
                }
            } else {
                a.pushUndo();
                pops::clearRect(a.pattern(), target(), heldColMode(true));
                a.patternEdited();
                advance();
            }
            break;
        case btn::Copy:
            if (state != kPress || !edit()) break;
            a.pushUndo();   // spec 06 §0.4 item 1: snapshot in both cases
            if (ui_.shift()) {
                if (!a.clipboardValid()) {
                    ui_.notify("Clipboard is empty");
                    break;
                }
                const SelRect t = target();
                const SelRect& src = a.clipRect();
                const ColMode m = heldColMode(false);
                pops::paste(a.pattern(), a.clipboard(), src, t.s0, t.t0, m, a.currentInstrument());
                a.patternEdited();
                step = t.s0;
                track = t.t0;
                if (m != ColMode::Pattern) advance(src.rows() - 1);
            } else {
                a.copyToClipboard(target());
                ui_.notify("Copied");
            }
            break;
        case btn::EncPush:
            if (state != kPress) break;
            if (!edit()) {
                if (!a.seq.isPlaying()) {
                    const SelRect r = target();
                    a.seq.playSelection(r.s0, r.s1, 0, kTrackCount - 1, false);
                }
                break;
            }
            {
                const SelRect r = target();
                if (col == 0 && selState == 2 && r.rows() == 1 && r.t0 != r.t1) {
                    // spec 06 §1.4: a single-row selection only auditions: the
                    // row plays once on the selected tracks (0x111d8, no
                    // loop); nothing is written and the cursor stays.
                    if (!a.seq.isPlaying()) a.seq.playSelection(r.s0, r.s1, r.t0, r.t1, false);
                    break;
                }
                a.pushUndo();
                if (col == 0) {
                    pops::writeNote(a.pattern(), r, lastNote(), a.currentInstrument());
                } else if (col == 1) {
                    pops::writeInstrument(a.pattern(), r, a.currentInstrument());
                } else if (ui_.isDown(col == 2 ? btn::Fx1 : btn::Fx2)) {
                    pops::writeFxType(a.pattern(), r, col, kFxMenuOrder[clampi(a.project.u8(pf::kLastFxType), 0, kFxCount - 1)]);
                } else {
                    pops::writeFxValue(a.pattern(), r, col, 0);
                }
                a.patternEdited();
                advance();
            }
            break;
        case btn::Up:
        case btn::Down:
        case btn::Left:
        case btn::Right:
            if (state != kPress) break;
            if (focus >= 0 || ui_.isDown(btn::Pattern)) {
                focusArrow(id);
                break;
            }
        {
            // spec 06 §1.5 table 0xa71e4 (entries Up, Down, Right, Left).
            const int k = id == btn::Up ? 0 : id == btn::Down ? 1 : id == btn::Right ? 2 : 3;
            moveCursor(kArrowDir[clampi(a.cfg.arrangement, 0, 2)][k], ui_.shift());
            break;
        }
        case btn::Aux:
            recArmPage = false;
            break;
        default:
            break;
    }
    refreshBar();
}

// Render selection, keyboard confirmed (spec 06 §1.11): renders the target
// rectangle to Export/<project>/Selection/<name>.wav through the export
// engine; "render & load" then imports it into the first free sample
// instrument slot and makes it the current instrument. F8 "Cancel" during
// the render (spec 05 appendix E1.4) stops it without a message.
bool PatternScreen::renderSelection(const char* name, bool load) {
    App& a = app_;
    int slot = -1;
    if (load) {
        slot = a.firstFreeSampleSlot();
        if (slot < 0) {
            ui_.notify("Not enough instruments");
            return false;
        }
    }
    char p[200];
    if (!a.exportName(App::ExportKind::Selection, p, sizeof(p))) {   // creates Export/<project>/Selection
        ui_.notify("Render failed");
        return false;
    }
    std::snprintf(p, sizeof(p), "%s/%s/Selection/%s.wav", kExportDir, a.project.name[0] ? a.project.name : "Project",
                  name);
    const SelRect r = target();
    if (!a.exportAudio(App::ExportKind::Selection, p, &r)) {
        if (!exportWasCancelled()) ui_.notify("Render failed");
        return false;
    }
    if (!load) {
        ui_.notify("Rendered");
        return true;
    }
    ui_.progress("Loading render", 50);
    const bool imported = a.importSample(p, slot);
    ui_.progressDone();
    if (!imported) {
        ui_.notify("Load failed");
        return false;
    }
    a.setCurrentInstrument(slot);
    // spec 16 §B.8: the Sample Loader (instrument list) opens with the new
    // instrument selected.
    ui_.switchTo(kModLoader, slot + 1);
    return true;
}

void PatternScreen::pageButton(int f, uint8_t state) {
    App& a = app_;
    const bool e = edit();
    if (page == 0) {
        switch (f) {
            case 0:
            case 1:
            case 2:
                if (state == kPress) focus = f;
                else if (state == kRelease && focus == f) focus = -1;
                return;
            case 3:
                if (state == kPress && e) {
                    fillOpen = true;
                    fillField = 0;
                    // spec 06 §1.10: Note-column defaults From = To = root
                    // note and Scale = project scale, until first set.
                    if (col == 0 && !fillNoteInit_) {
                        fillNoteInit_ = true;
                        fill[0].from = fill[0].to = int16_t(clampi(a.rootNote() + 12, 0, 127));   // option 0 = C1 = note 12
                        fill[0].scaleOrFx = int16_t(a.scale());
                    }
                    clampFillValues();   // Fx columns: From/To inside the chosen Fx's range
                }
                return;
            case 4:
                // Preview plays the cursor / selection once and stops, as on
                // stock 1.9.2 (user report: a looping one-row preview held
                // the cursor and looked like a hang until Play).
                if (state == kPress && !a.seq.isPlaying()) {
                    const SelRect r = target();
                    a.seq.playSelection(r.s0, r.s1, r.t0, r.t1, false);
                }
                return;
            case 5:
            case 6:
                if (!e) return;
                if (state == kPress) {
                    focus = f;
                    encUsedInHold_ = false;
                } else if (state == kRelease && focus == f) {
                    if (!encUsedInHold_) {
                        if (f == 5) a.undoEdit();
                        else a.redoEdit();
                    }
                    focus = -1;
                }
                return;
            case 7:
                if (state == kPress) page = (page + 1) % 3;
                return;
        }
        return;
    }
    if (state != kPress) return;
    if (f == 7) {
        page = (page + 1) % 3;
        return;
    }
    if (page == 1) {
        if (!e) return;
        switch (f) {
            case 0: {   // Render selection (spec 06 §1.11)
                if (a.seq.isPlaying()) a.seq.stop();
                char path[160];
                if (!a.exportName(App::ExportKind::Selection, path, sizeof(path))) return;
                const char* bn = std::strrchr(path, '/');
                char nm[40];
                std::snprintf(nm, sizeof(nm), "%.39s", bn ? bn + 1 : path);
                char* dot = std::strrchr(nm, '.');
                if (dot) *dot = 0;
                // F8 "Render", F7 "render & load": the file is then imported
                // into the first free sample instrument slot (spec 06 §1.11).
                ui_.kb.open("Render Selection", nm, 32, "Render\nSelection",
                            [](void* ctx, bool ok, const char* text) {
                                auto* self = static_cast<PatternScreen*>(ctx);
                                if (ok && text[0]) self->renderSelection(text, self->ui_.kb.alt());
                            },
                            this);
                ui_.kb.setAlt("Render\n& Load");   // spec 16 §B.8 labels
                return;
            }
            case 1:
                a.pushUndo();
                pops::invert(a.pattern(), target(), heldColMode(false));
                a.patternEdited();
                return;
            case 2:
            case 3:
            case 4: {
                a.pushUndo();
                bool ok = f == 2 ? pops::duplicate(a.pattern()) : f == 3 ? pops::expand(a.pattern()) : pops::shrink(a.pattern());
                if (!ok) ui_.notify(f == 4 ? "Pattern too short" : "Pattern too long");
                else a.patternEdited();
                if (step >= len()) step = len() - 1;
                return;
            }
            case 5: {
                SelRect all;
                all.s0 = 0;
                all.s1 = len() - 1;
                all.t0 = 0;
                all.t1 = kTrackCount - 1;
                a.pushUndo();
                a.copyToClipboard(all);
                ui_.notify("Pattern copied");
                return;
            }
            case 6:
                if (!a.clipboardValid()) {
                    ui_.notify("Clipboard is empty");
                    return;
                }
                a.pushUndo();
                pops::paste(a.pattern(), a.clipboard(), a.clipRect(), 0, 0, ColMode::Pattern, a.currentInstrument());
                a.patternEdited();
                if (step >= len()) step = len() - 1;
                return;
        }
        return;
    }
    // Page 2: names (spec 06 §1.12).
    if (f == 0) {
        char title[40];
        std::snprintf(title, sizeof(title), "Edit Pattern %d Name", a.patternNumber());
        ui_.kb.open(title, a.names.get(a.patternNumber()), PatternNames::kMaxLen, "Save",
                    [](void* ctx, bool ok, const char* text) {
                        if (!ok) return;
                        App& ap = *static_cast<App*>(ctx);
                        ap.names.set(ap.patternNumber(), text);
                        ap.markNamesDirty();
                    },
                    &a);
    } else if (f == 1) {
        char title[40], cur[24];
        std::snprintf(title, sizeof(title), "Edit Track %d Name", track + 1);
        a.project.trackName(track, cur, sizeof(cur));
        ui_.kb.open(title, cur, 7, "Save",
                    [](void* ctx, bool ok, const char* text) {
                        if (!ok) return;
                        auto* self = static_cast<PatternScreen*>(ctx);
                        self->app_.project.setTrackName(self->track, text);
                        self->app_.markProjectDirty();
                    },
                    this);
    }
}

void PatternScreen::transport() {
    // spec 06 §1.8.
    App& a = app_;
    Sequencer& s = a.seq;
    if (!s.isPlaying() && !s.isArmed()) {
        if (ui_.isDown(btn::Rec)) {
            for (int t = 0; t < kTrackCount; ++t) s.setRecordEnabled(t, armed[t]);
            s.record();
            a.editMode = false;
            if (s.isCountingIn()) {
                const uint32_t n = s.countInMs();   // whole seconds, saturating at 255
                countInUntil_ = ui_.now() + 1000u * (n < 1 ? 1 : n);
                if (!countInUntil_) countInUntil_ = 1;
            }
            if (a.cfg.clockIn) ui_.notify("Recording waits for clock");
        } else if (ui_.shift()) {
            if (selState == 2 || wholeCol || allTracks) {
                const SelRect r = target();
                s.playSelection(r.s0, r.s1, r.t0, r.t1, true);
            } else {
                s.play(step);
            }
        } else {
            s.userPlay(0);
        }
    } else {
        if (ui_.isDown(btn::Rec)) {
            // Punch in: recording on (0x111c0(seq,1)) without restarting.
            for (int t = 0; t < kTrackCount; ++t) s.setRecordEnabled(t, armed[t]);
            s.setRecording(true);
            a.editMode = false;
        } else {
            s.stop();
            a.allVoicesOff();
            // The cursor stays on the row where playback stopped (it
            // follows the playhead while playing), as on stock 1.9.2.
        }
    }
    focus = -1;
}

void PatternScreen::focusArrow(uint8_t id) {
    App& a = app_;
    const int fcs = focus >= 0 ? focus : 0;   // Pattern key held acts like focus 0
    if (fcs == 0) {
        // spec 16 §A.2: Up/Right = +1, Down/Left = -1. Up/Down change the
        // pattern at once, also while playing (FW:0x3d548). Left/Right queue
        // the change while playing (switch at the pattern end) and act at
        // once when stopped; App::selectPattern does both.
        const int d = (id == btn::Up || id == btn::Right) ? 1 : -1;
        if (id == btn::Up || id == btn::Down) {
            a.switchPatternNow(clampi(a.effectivePatternNumber() + d, 1, kPatternCount));
            return;
        }
        const int base = a.seq.queuedPattern() ? a.seq.queuedPattern() : a.patternNumber();
        a.selectPattern(clampi(base + d, 1, kPatternCount));
    } else if (fcs == 1) {
        int l = len();
        if (id == btn::Up) l += 1;
        else if (id == btn::Down) l -= 1;
        else l = presetStep(kLenPresets, sizeof(kLenPresets), l, id == btn::Right ? 1 : -1);
        a.pushUndo();
        pops::setLength(a.pattern(), clampi(l, 1, kStepCount));
        if (step >= len()) step = len() - 1;
        a.patternEdited();
    } else if (fcs == 2) {
        int j = a.stepJump();
        if (id == btn::Up) j += 1;
        else if (id == btn::Down) j -= 1;
        else j = presetStep(kJumpPresets, sizeof(kJumpPresets), j, id == btn::Right ? 1 : -1);
        a.setStepJump(clampi(j, 0, 32));
    } else if ((fcs == 5 || fcs == 6) && id == btn::Left) {
        encUsedInHold_ = true;
        a.undoEdit();
    }
}

void PatternScreen::focusDelta(int d) {
    App& a = app_;
    if (focus == 0 || (focus < 0 && ui_.isDown(btn::Pattern))) {
        // spec 16 §A.2: Pattern + encoder changes the pattern at once, also while
        // playing (like Up/Down; only Left/Right queue).
        a.switchPatternNow(clampi(a.effectivePatternNumber() + d, 1, kPatternCount));
    } else if (focus == 1) {
        a.pushUndo();
        pops::setLength(a.pattern(), clampi(len() + d, 1, kStepCount));
        if (step >= len()) step = len() - 1;
        a.patternEdited();
    } else if (focus == 2) {
        a.setStepJump(clampi(a.stepJump() + d, 0, 32));
    } else if (focus == 5 || focus == 6) {
        encUsedInHold_ = true;
        if (d < 0) a.undoEdit();
        else if (d > 0) a.redoEdit();
    }
}

#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
bool PatternScreen::encoderOnList() const {
    if (pop_.isOpen()) return true;          // note / instrument / FX lists
    if (fillOpen || focus >= 0) return false;
    return !edit();                          // stopped, not editing: the rows
}
#endif

void PatternScreen::onEncoder(int d) {
    if (patternHeld_) patternCombo_ = true;
    if (pop_.isOpen()) {   // spec 06 §1.6 step 3: the encoder scrolls the popup
        pop_.move(d);
        if (pop_.kind() == 0) followNoteWindow();
        refreshBar();
        ui_.dirty();
        return;
    }
    // spec 06 §1.7 dispatch order.
    if (fillOpen) {
        fillDelta(d);
        refreshBar();
        return;
    }
    if (focus >= 0 || ui_.isDown(btn::Pattern)) {
        focusDelta(d);
        refreshBar();
        return;
    }
    if (!edit()) {
        if (app_.seq.isPlaying()) return;
        if (d > 10) d = 10;
        if (d < -10) d = -10;
        step = clampi(step + d, 0, len() - 1);
        if (selState == 2) collapse();
        return;
    }
    if (colMask && !(colMask & (1u << col))) return;
    editEncoder(d);
    refreshBar();
}

void PatternScreen::editEncoder(int d) {
    App& a = app_;
    const SelRect r = target();
    a.pushUndo();
    if (col == 0) {
        if (d == 10 || d == -10) d = d > 0 ? 12 : -12;
        if (ui_.shift()) d *= 12;
        const int n = pops::stepNote(a.pattern(), r, d, a.currentInstrument());
        if (n >= 0) a.project.setU8(pf::kLastNote, uint8_t(n));
        a.markProjectDirty();
        if (!a.seq.isPlaying() && r.single() && n >= 0) {
            a.editorTrack = uint8_t(track);
            a.previewOn(App::kAuditionEncoder, a.pattern().tracks[r.t0].steps[r.s0].instrument, uint8_t(n), 127);
        }
    } else if (col == 1) {
        pops::stepInstrument(a.pattern(), r, d);
        if (r.single()) a.setCurrentInstrument(a.pattern().tracks[r.t0].steps[r.s0].instrument);
    } else {
        const bool typeMode = ui_.isDown(col == 2 ? btn::Fx1 : btn::Fx2);
        if (typeMode) {
            pops::stepFxType(a.pattern(), r, col, d);
            const StepFx& f = a.pattern().tracks[r.t0].steps[r.s0].fx[fxSlotForColumn(col)];
            a.project.setU8(pf::kLastFxType, uint8_t(pops::fxOrderIndex(f.type)));
        } else {
            pops::stepFxValue(a.pattern(), r, col, d);
        }
    }
    a.patternEdited();
}

void PatternScreen::onPad(uint8_t pad, uint8_t state) {
    App& a = app_;
    if (fillOpen) {
        if (state != kPress) return;
        pops::FillParams& f = fill[col];
        int v = pad;
        if (fillField >= 4 && col == 0) v = a.padNote(pad);
        else if (fillField >= 4 && col >= 2) v = pops::fxValueFromPad(fillFxType(), pad);
        switch (fillField) {
            case 0: f.where = int16_t(clampi(v, 0, 6)); break;
            case 1: if (f.where >= 4) f.param[f.where] = int16_t(clampi(v + 1, 1, 128)); break;
            case 2:
                if (col == 1) break;
                f.scaleOrFx = int16_t(col == 0 ? clampi(v, 0, kScaleCount - 1) : clampi(v - 1, -1, kFxCount - 2));
                if (col >= 2) clampFillValues();
                break;
            case 3: f.type = int16_t(clampi(v, 0, 2)); break;
            case 4: f.from = int16_t(v); break;
            case 5: f.to = int16_t(v); break;
        }
        refreshBar();
        return;
    }
    if (pop_.isOpen()) {   // the popup takes the pad (spec 06 §1.6 step 3)
        if (state == kPress) popupPad(pad);
        return;
    }
    // Audition on the track voices, voiceId = pad number, search from the
    // cursor track (spec 07 Q7).
    a.editorTrack = uint8_t(track);
    const uint16_t voice = pad;
    if (state == kRelease) {
        if (padVoice_[pad] >= 0) a.previewOff(uint16_t(padVoice_[pad]));
        padVoice_[pad] = -1;
        // Instr column: release only, no OFF step (manual p.95, MANUAL_AUDIT #4).
        if (a.seq.isRecording() && col <= 1) a.seq.liveNoteOff(col == 1 ? 60 : a.padNote(pad), col == 0);
        return;
    }
    if (state != kPress) return;
    // A pad pressed while a column key is held is the key + pad entry: the
    // key no longer opens its hold popup, so its release writes nothing more.
    colKey_ = 0;
    // Hold focus: pads pick values (spec 06 §1.6).
    if (focus == 0) {
        a.selectPattern(pad + 1);
        refreshBar();
        return;
    }
    if (focus == 1) {
        a.pushUndo();
        pops::setLength(a.pattern(), clampi(4 * (pad + 1), 1, kStepCount));
        if (step >= len()) step = len() - 1;
        a.patternEdited();
        refreshBar();
        return;
    }
    if (focus == 2) {
        a.setStepJump(clampi(pad, 0, 32));
        refreshBar();
        return;
    }
    if (focus >= 0) return;   // spec 06 §1.6: no note is entered while a focus is held
    const uint8_t note = a.padNote(pad);
    if (a.seq.isRecording() && col <= 1) {
        // spec 16 §A.6: the Note column records notes with the current
        // instrument; the Instr column records instrument triggers: note 60
        // with the pad's instrument (MIDI tracks: channel instrument).
        uint8_t ins = a.currentInstrument(), n = note;
        if (col == 1) {
            ins = uint8_t(isMidiTrack(track) ? synthui::midiTrackPadInstrument(pad) : pad);
            n = 60;
        }
        a.seq.liveNoteOn(n, 127, uint8_t(track), ins, a.recordVolume(ins));
        a.previewOn(voice, ins, n, 127);
        padVoice_[pad] = int8_t(pad);
        return;
    }
    if (a.seq.isRecording()) {
        // FX columns are not live-recorded: the pad sets the cursor step's
        // FX as in step edit (spec 16 §A.6).
        SelRect r;
        r.s0 = r.s1 = step;
        r.t0 = r.t1 = track;
        a.pushUndo();
        writeFxPad(r, pad);
        a.patternEdited();
        refreshBar();
        return;
    }
    if (!edit()) {
        if (col == 1) {
            a.setCurrentInstrument(isMidiTrack(track) ? synthui::midiTrackPadInstrument(pad) : pad);
            refreshBar();
        }
        // Instr column: the pad picks an instrument and auditions it at note
        // 60, like the Instr-column record and edit paths (spec 16 §A.6),
        // not at the pad's own note (user report: every instrument one
        // semitone higher than the previous one).
        a.previewOn(voice, a.currentInstrument(), col == 1 ? uint8_t(60) : note, 127);
        padVoice_[pad] = int8_t(pad);
        return;
    }
    const SelRect r = target();
    if (col == 0) {
        // Note entry in edit mode plays on the cursor track only, after
        // releasing its note (spec 07 Q7 exception).
        a.previewOn(voice, a.currentInstrument(), note, 127, track);
        padVoice_[pad] = int8_t(pad);
        if (selState == 2 && r.rows() == 1 && !r.single()) return;   // single-row selection only auditions
        a.pushUndo();
        pops::writeNote(a.pattern(), r, note, a.currentInstrument());
        a.project.setU8(pf::kLastNote, note);
        a.markProjectDirty();
        a.patternEdited();
        if (r.single()) advance();
    } else if (col == 1) {
        int ins = pad;
        for (int t = r.t0; t <= r.t1; ++t)
            if (isMidiTrack(t)) ins = synthui::midiTrackPadInstrument(pad);
        a.pushUndo();
        pops::writeInstrument(a.pattern(), r, uint8_t(ins));
        a.setCurrentInstrument(ins);
        a.patternEdited();
        if (a.seq.isPlaying()) a.previewOn(voice, uint8_t(ins), 60, 127);
        if (selState != 2) advance();   // spec 16 §A.5: every column advances
    } else {
        a.pushUndo();
        writeFxPad(r, pad);
        a.patternEdited();
        if (selState != 2) advance();
    }
    refreshBar();
}

void PatternScreen::writeFxPad(const SelRect& r, int pad) {
    App& a = app_;
    const int slot = fxSlotForColumn(col);
    if (ui_.isDown(col == 2 ? btn::Fx1 : btn::Fx2)) {
        const uint8_t type = kFxMenuOrder[pad < kFxCount ? pad : kFxCount - 1];
        pops::writeFxType(a.pattern(), r, col, type);
        a.project.setU8(pf::kLastFxType, uint8_t(pad < kFxCount ? pad : kFxCount - 1));
    } else {
        const uint8_t type = a.pattern().tracks[r.t0].steps[r.s0].fx[slot].type;
        pops::writeFxValue(a.pattern(), r, col, pops::fxValueFromPad(type, pad));
    }
}

// ---------------------------------------------------------------------------
// Column-key popups (spec 06 §1.6)

void PatternScreen::openPicker(int which) {
    App& a = app_;
    pickerKey_ = colKey_;
    colKey_ = 0;
    const Step& st = a.pattern().tracks[track].steps[step];
    if (which == 0) {
        const int n = st.note >= 0 ? st.note : lastNote();
        pop_.open(0, "Note", 128, n, noteItem, nullptr);
        // SPEC GAP: the window base at open is not recovered; it starts at
        // the pre-selected note, so F1 plays the cursor's note.
        noteBase_ = clampi(n, 0, kNoteBaseMax);
        noteKeysOn_ = 0;
        followNoteWindow();
    } else if (which == 1) {
        if (isMidiTrack(track)) {
            // 2.x: Synth 1..3 then MIDI 1..16 (synthui::popupInstrumentMidi).
            pop_.open(5, "Instrument", synthui::kMidiPopupCount, synthui::popupPositionMidi(a.currentInstrument()),
                      midiItem, nullptr);
        } else {
            // 2.x: the synth slots (instruments 64..66) are the first three
            // entries, as on the Tracker+ (manual p.100 figure); the cursor
            // is a list position (synthui::popupPosition).
            pop_.open(2, "Instrument", kInstrumentCount + kSynthSlotCount,
                      synthui::popupPosition(a.currentInstrument()), instrItem, &a);
        }
    } else {
        const int slot = fxSlotForColumn(which);
        pop_.open(3, "FX", kFxCount, pops::fxOrderIndex(st.fx[slot].type), fxItem, nullptr);
        pop_.setDescription([](void*, int i) { return fxHelpText(kFxMenuOrder[i < kFxCount ? i : 0]); });
    }
    // Stock popup header band over the title bar (Ui; Part 2 §7.2).
    ui_.openPopupHeader(which == 0 ? "Notes" : (which == 1 ? "Instruments" : "Fx"),
                        which == 0 ? Ui::PopupHeader::kFull : Ui::PopupHeader::kRight);
    refreshBar();
    ui_.dirty();   // opened from the update loop: draw it now
}

// Note popup F-keys (stock 0x2b52c / 0x2ba88): a press plays base+i at
// velocity 127 and picks it, the release stops it. With Shift held, F1/F2/
// F7/F8 move the window by -12 / -1 / +1 / +12.
// SPEC GAP: the strings diff reads the code's signs as F1 +12 and F8 -12 and
// leaves the convention to verify; the labels (Oct- on F1, Oct+ on F8) are
// followed here, the direction they name.
void PatternScreen::noteKey(uint8_t id, uint8_t state) {
    App& a = app_;
    const uint8_t bit = uint8_t(1u << id);
    if (state == kRelease) {
        if (noteKeysOn_ & bit) a.previewOff(uint16_t(kNoteKeyVoice + id));
        noteKeysOn_ = uint8_t(noteKeysOn_ & ~bit);
        return;
    }
    if (state != kPress) return;
    if (ui_.shift()) {
        int d = 0;
        switch (id) {
            case btn::F1: d = -12; break;
            case btn::F2: d = -1; break;
            case btn::F7: d = 1; break;
            case btn::F8: d = 12; break;
            default: break;
        }
        if (d) {
            noteBase_ = clampi(noteBase_ + d, 0, kNoteBaseMax);
            return;
        }
    }
    const int n = noteBase_ + id;
    if (n > 127) return;
    pop_.select(n);
    followNoteWindow();
    if (noteKeysOn_ & bit) a.previewOff(uint16_t(kNoteKeyVoice + id));
    a.editorTrack = uint8_t(track);
    a.previewOn(uint16_t(kNoteKeyVoice + id), a.currentInstrument(), uint8_t(n), 127);
    noteKeysOn_ = uint8_t(noteKeysOn_ | bit);
}

void PatternScreen::noteKeysOff() {
    for (int i = 0; i < kNoteKeys; ++i)
        if (noteKeysOn_ & (1u << i)) app_.previewOff(uint16_t(kNoteKeyVoice + i));
    noteKeysOn_ = 0;
}

void PatternScreen::commitPicker() {
    App& a = app_;
    noteKeysOff();
    const int kind = pop_.kind();
    const int v = pop_.cursor();
    pop_.close();
    ui_.closePopupHeader();
    pickerKey_ = 0;
    ui_.dirty();
    if (kind == 2 || kind == 5) {
        const int ins = kind == 5 ? synthui::popupInstrumentMidi(v) : synthui::popupInstrument(v);
        a.setCurrentInstrument(ins);
        if (edit()) {
            a.pushUndo();
            pops::writeInstrument(a.pattern(), target(), uint8_t(ins));
            a.patternEdited();
        }
    } else if (kind == 0) {
        a.project.setU8(pf::kLastNote, uint8_t(v));
        a.markProjectDirty();
        if (edit()) {
            SelRect r;
            r.s0 = r.s1 = step;
            r.t0 = r.t1 = track;
            a.pushUndo();
            pops::writeNote(a.pattern(), r, v, a.currentInstrument());
            a.patternEdited();
        }
    } else if (kind == 3) {
        a.project.setU8(pf::kLastFxType, uint8_t(v));
        a.markProjectDirty();
        if (edit()) {
            SelRect r;
            r.s0 = r.s1 = step;
            r.t0 = r.t1 = track;
            a.pushUndo();
            pops::writeFxType(a.pattern(), r, col, kFxMenuOrder[v]);
            a.patternEdited();
        }
    }
}

// ---------------------------------------------------------------------------
// Fill popup (spec 06 §1.10)

void PatternScreen::fillButton(uint8_t id) {
    if (id <= btn::F6) {
        const pops::FillParams& f = fill[col];
        // Blank keys (F2 without a Where parameter, F3 in the Instr column,
        // F6 for a constant fill) do nothing.
        if ((id == btn::F2 && f.where < 4) || (id == btn::F3 && col == 1) || (id == btn::F6 && f.type == 0)) return;
        if (fillField == id) {
            // Pressing the selected field again steps it +1 with wrap.
            pops::FillParams& fm = fill[col];
            switch (fillField) {
                case 0: fm.where = int16_t((fm.where + 1) % 7); break;
                case 3: fm.type = int16_t((fm.type + 1) % 3); break;
                default: fillDelta(1, true); break;
            }
        }
        fillField = id;
    } else if (id == btn::F7) {
        fillOpen = false;
    } else if (id == btn::F8) {
        fillApply();
    } else if (id == btn::Right || id == btn::Left) {
        // Left / Right move between the fields (columns), skipping blank
        // ones, like the Master pages [S owner test on 1.9.2]; Up / Down
        // and the encoder change the selected field.
        const pops::FillParams& f = fill[col];
        auto blank = [&](int k) { return (k == 1 && f.where < 4) || (k == 2 && col == 1) || (k == 5 && f.type == 0); };
        const int d = id == btn::Right ? 1 : -1;
        for (int k = fillField + d; k >= 0 && k <= 5; k += d)
            if (!blank(k)) {
                fillField = k;
                break;
            }
    } else if (id == btn::Up || id == btn::Down) {
        // The list fields (Where?, Scale / Fx Type, Fill Type) step down
        // their list with Down; value fields count up with Up.
        const bool list = fillField == 0 || fillField == 2 || fillField == 3;
        fillDelta((id == btn::Down) == list ? 1 : -1);
    }
    // A field that went blank (Where without parameter, constant To) moves
    // the selection back to Where?.
    const pops::FillParams& f = fill[col];
    if ((fillField == 1 && f.where < 4) || (fillField == 5 && f.type == 0)) fillField = fillField == 5 ? 4 : 0;
}

uint8_t PatternScreen::fillFxType() const {
    const int i = fill[col].scaleOrFx;
    return kFxMenuOrder[clampi(i, 0, kFxCount - 1)];
}

void PatternScreen::clampFillValues() {
    pops::FillParams& f = fill[col];
    if (col < 2 || f.scaleOrFx < 0) return;
    const uint8_t ft = fillFxType();
    f.from = int16_t(clampi(f.from, seq::fxMin(ft), seq::fxMax(ft)));
    f.to = int16_t(clampi(f.to, seq::fxMin(ft), seq::fxMax(ft)));
}

namespace {
void fillInstrCode(int val, char* out) {
    const int i = clampi(val, 0, 255);
    if (i < kSampleInstrumentCount) std::snprintf(out, 8, "%02d", i + 1);
    else if (isSynthInstrument(uint8_t(i))) std::snprintf(out, 8, "S%d", synthSlotOf(uint8_t(i)) + 1);
    else std::snprintf(out, 8, "M%02d", (i - kFirstMidiInstrument + 1) % 100);
}
}  // namespace

void PatternScreen::fillValueText(int val, char* out) const {
    // spec 16 §B.3: note names; Fx values as the plain number. Instruments:
    // stock shows bare numbers (1..48, MIDI n+3); 2.x writes them like the
    // grid, 07 / M01 / S1 (USER_REPORTS_R3 R3-5).
    if (col == 0) noteName(int8_t(clampi(val, 0, 127)), out);
    else if (col == 1) fillInstrCode(val, out);
    else if (fill[col].scaleOrFx < 0) std::snprintf(out, 8, "%d", clampi(val, 0, 255));   // random type
    else {
        // The Fx's display value, as in the grid (Panning -50..50; manual
        // p.87, MANUAL_AUDIT_2026-10-23 #8), without the grid's padding.
        char t[5];
        fxValueText(fillFxType(), uint8_t(clampi(val, 0, 255)), t);
        const char* p = t;
        while (*p == ' ') ++p;
        std::snprintf(out, 8, "%s", p);
    }
}

float PatternScreen::fillValueFrac(int v) const {
    if (col == 0) return v / 127.0f;
    if (col == 1) return v / 63.0f;
    // The engine range (Roll, Chord, LFOs and Bit Depth differ from the
    // file-format table; MANUAL_AUDIT_2026-10-23 #24).
    const uint8_t t = fillFxType();
    const int lo = seq::fxMin(t), span = seq::fxMax(t) - lo;
    return span > 0 ? float(v - lo) / float(span) : 0.0f;
}

void PatternScreen::fillDelta(int d, bool wrap) {
    pops::FillParams& f = fill[col];
    auto lim = [](int dd) { return dd > 5 ? 5 : (dd < -5 ? -5 : dd); };
    // spec 06 §1.10: re-pressing the selected field's key steps it +1 and
    // wraps to the minimum past the maximum; encoder/arrows clamp.
    auto clampw = [wrap](int v, int lo, int hi) { return wrap && v > hi ? lo : clampi(v, lo, hi); };
    int vmin = 0, vmax = 127;
    if (col == 1) vmax = kInstrumentCount - 1;
    if (col >= 2 && f.scaleOrFx >= 0) {
        // spec 06 §1.10: that Fx's min..max (engine ranges 0xcb7d8 / 0xcb750).
        vmin = seq::fxMin(fillFxType());
        vmax = seq::fxMax(fillFxType());
    }
    switch (fillField) {
        case 0: f.where = int16_t(clampw(f.where + d, 0, 6)); break;
        case 1:
            if (f.where >= 4) f.param[f.where] = int16_t(clampw(f.param[f.where] + lim(d), 1, 128));
            break;
        case 2:
            if (col == 0) f.scaleOrFx = int16_t(clampw(f.scaleOrFx + lim(d), 0, kScaleCount - 1));
            else if (col >= 2) {
                f.scaleOrFx = int16_t(clampw(f.scaleOrFx + lim(d), -1, kFxCount - 2));
                clampFillValues();
            }
            break;
        case 3: f.type = int16_t(clampw(f.type + d, 0, 2)); break;
        case 4: f.from = int16_t(clampw(f.from + d, vmin, vmax)); break;
        case 5: f.to = int16_t(clampw(f.to + d, vmin, vmax)); break;
    }
}

void PatternScreen::fillApply() {
    App& a = app_;
    a.pushUndo();
    pops::fill(a.pattern(), target(), col, fill[col], a.rootNote(), a.currentInstrument(), rnd, &gRnd);
    a.patternEdited();
    fillOpen = false;
}

// ---------------------------------------------------------------------------
// Update / LEDs

void PatternScreen::update(uint32_t now) {
    App& a = app_;
    // Playhead follow (not in edit mode): every pass, so the frame drawn for
    // a step change (App::onStepChanged -> Ui::dirty) already scrolls to the
    // new step. Behind the 20 ms throttle below the same step cost two
    // frames, one with the old window and one after the follow, the second
    // stalling on the pending swap (1.9.4 telemetry: ~2 frames per step).
    if (a.seq.isPlaying()) {
        const int ps = a.seq.step(track);
        if (ps != lastPlayStep_) {
            lastPlayStep_ = ps;
            if (!edit()) step = ps < len() ? ps : len() - 1;
            ui_.dirty();
        }
    } else {
        lastPlayStep_ = -1;
    }
    if (step >= len()) step = len() - 1;
    if (step != statusStep_) {   // status position = cursor step while stopped
        // Also every pass: behind the throttle it dirtied the UI once more
        // ~20 ms after the step frame (a second frame per step).
        statusStep_ = step;
        ui_.setStatusCursorStep(step);
        ui_.dirty();
    }
    if (now - lastUpdate_ < 20) return;   // spec 06 §1.2 update loop >= 20 ms
    lastUpdate_ = now;
    // Column-key hold popups.
    if (colKey_ && !pop_.isOpen() && !ui_.shift() && !ui_.isDown(btn::Pattern)) {
        const int which = colForKey(colKey_);
        const uint32_t thr = which == 0 ? kNoteHoldMs : kColHoldMs;
        if (now - colKeyAt_ > thr) openPicker(which);
    }
    // Shift mute-page refresh.
    if (ui_.shift() != lastShift_) {
        lastShift_ = ui_.shift();
        refreshBar();
        ui_.dirty();
    }
    const bool barPage0 =
        page == 0 && focus < 0 && !fillOpen && !ui_.shift() && !recArmPage && !pop_.isOpen();
    // spec 06 §1.2: F1 shows the queued pattern number, its colour
    // alternating every 200 ms.
    const int q = a.seq.queuedPattern();
    const bool b = q && ((now / 200) & 1) != 0;
    if (b != blink_) {
        blink_ = b;
        if (barPage0) ui_.bar.highlight(0, b);
        ui_.dirty();
    }
    if (barPage0) {
        char v[8];
        std::snprintf(v, sizeof(v), a.cfg.numbering == 2 ? "%02X" : "%d", q ? q : a.patternNumber());
        if (std::strcmp(v, ui_.bar.label[0].bottom) != 0) {
            ui_.bar.setBottom(0, v);
            ui_.dirty();
        }
    }
    // spec 06 §1.2: 500 ms blink while the sequencer waits for a recording
    // to start (external clock or count-in).
    const bool waiting = a.seq.isRecording() && (a.seq.isArmed() || a.seq.isCountingIn());
    const bool rb = waiting && ((now / 250) & 1) != 0;
    if (rb != recBlink_) {
        recBlink_ = rb;
        ui_.dirty();
    }
    if (waiting != recWaitShown_) {
        recWaitShown_ = waiting;
        ui_.dirty();
    }
    const bool ci = countInShown();
    if (ci != countInDrawn_) {
        countInDrawn_ = ci;
        ui_.dirty();
    }
}

// Pre-roll popup (spec 16 §B.9): "Counting in", 400 x 80, centred, shown for
// N = floor(count-in seconds) clamped to 1..255 s (Sequencer::countInMs).
bool PatternScreen::countInShown() const {
    return countInUntil_ && app_.seq.isRecording() && int32_t(ui_.now() - countInUntil_) < 0;
}

void PatternScreen::drawCountIn(Gfx& g) {
    if (!countInShown()) return;
    const Theme& th = ui_.th();
    g.fill(200, 200, 400, 80, th.panel2);
    g.frame(200, 200, 400, 80, th.dim);
    g.text(400, 240, kFontLarge, th.text, "Counting in", IDisplay::kOptCenter);
}

void PatternScreen::padLeds(uint8_t* lv) {
    App& a = app_;
    const uint8_t dim = a.cfg.padDim(), full = a.cfg.padFull();
    if (pop_.isOpen()) {
        popupLeds(lv);
        return;
    }
    if (focus == 0) {
        for (int p = 0; p < 48; ++p)
            if (a.patternPresent(p + 1) && a.patternHasData(p + 1)) lv[p] = dim;
        const int pn = a.patternNumber() - 1;
        if (pn >= 0 && pn < 48) lv[pn] = full;
        return;
    }
    if (focus == 1) {
        lv[clampi((len() - 3) * 47 / 188, 0, 47)] = full;
        return;
    }
    if (focus == 2) {
        lv[clampi(a.stepJump(), 0, 47)] = full;
        return;
    }
    const Step& st = a.pattern().tracks[track].steps[step];
    if (edit()) {
        if (col == 0) {
            const uint8_t ci = a.currentInstrument();
            if (ci < kSampleInstrumentCount && a.instruments[ci].playMode == uint8_t(PlayMode::BeatSlice)) {
                for (int p = 0; p < a.instruments[ci].sliceCount && p < 48; ++p) lv[p] = dim;
                if (st.note >= 24 && st.note < 72) lv[st.note - 24] = full;
            } else if (st.note >= 0) {
                a.refreshPadNotes();
                uint8_t pads[48];
                const int n = a.padNotes.padsForNote(st.note, pads, 48);
                for (int i = 0; i < n; ++i) lv[pads[i]] = dim;
            }
        } else if (col == 1) {
            if (st.instrument < 48) lv[st.instrument] = full;
            const int sp = isMidiTrack(track) ? synthui::midiTrackInstrumentPad(st.instrument) : -1;
            if (sp >= 0) lv[sp] = full;   // a synth on a MIDI track: pads 17-19
        } else {
            const StepFx& f = st.fx[fxSlotForColumn(col)];
            if (ui_.isDown(col == 2 ? btn::Fx1 : btn::Fx2)) {
                const int i = pops::fxOrderIndex(f.type);
                if (i < 48) lv[i] = full;
            } else {
                // The first pad that enters the current value.
                int lit = -1;
                for (int p = 0; p < 48 && lit < 0; ++p)
                    if (pops::fxValueFromPad(f.type, p) == f.value) lit = p;
                const int lo = seq::fxMin(f.type), hi = seq::fxMax(f.type);
                if (lit < 0 && hi > lo) lit = clampi((f.value - lo) * 47 / (hi - lo), 0, 47);
                if (lit >= 0) lv[lit] = full;
            }
        }
        return;
    }
    // Not editing: "Pads preview" (spec 06 §1.6 rule 7).
    if (a.cfg.padsPreview == 0) return;
    a.refreshPadNotes();
    const int t0 = a.cfg.padsPreview == 1 ? track : 0;
    const int t1 = a.cfg.padsPreview == 1 ? track : kTrackCount - 1;
    for (int t = t0; t <= t1; ++t) {
        const Step& s = a.pattern().tracks[t].steps[step];
        if (col == 1) {
            if (s.note >= 0 && s.instrument < 48) lv[s.instrument] = dim;
        } else if (s.note >= 0) {
            uint8_t pads[48];
            const int n = a.padNotes.padsForNote(s.note, pads, 48);
            for (int i = 0; i < n; ++i) lv[pads[i]] = dim;
        }
    }
}

// ---------------------------------------------------------------------------
// Drawing

void PatternScreen::stepLabel(int s, char* out) const {
    switch (app_.cfg.numbering) {
        case 0: std::snprintf(out, 12, "%3d", s & 0xFF); break;
        case 2: std::snprintf(out, 12, " %02X", s & 0xFF); break;
        default: std::snprintf(out, 12, "%3d", (s & 0xFF) + 1); break;
    }
}

void PatternScreen::cellText(int t, int s, char* out) const {
    // spec 06 §1.2 cell text: note, instrument (2 digits / "M" + 2 digits /
    // "--" for an empty note with a sample instrument), Fx1 then Fx2 as
    // abbreviation + value text (0x14918). With a column-view mask only the
    // masked columns are written (spec 06 §1.9).
    const CellLayout L = cellLayout(colMask);
    const Step& st = app_.pattern().tracks[t].steps[s];
    char part[4][8];
    noteName(st.note, part[0]);
    if (st.note == kNoteEmpty && !isMidiInstrument(st.instrument) && st.instrument < kInstrumentCount)
        std::strcpy(part[1], "-- ");
    else if (isMidiInstrument(st.instrument)) std::snprintf(part[1], sizeof(part[1]), "M%02d", st.instrument - 47);
    else if (st.instrument >= kInstrumentCount) std::snprintf(part[1], sizeof(part[1]), "S%c ", char('1' + (st.instrument - kInstrumentCount) % 9));
    else std::snprintf(part[1], sizeof(part[1]), "%02d ", st.instrument + 1);
    for (int k = 0; k < 2; ++k) fxCellText(st.fx[k].type, st.fx[k].value, part[2 + k]);
    int n = 0;
    for (int c = 0; c < 4; ++c) {
        if (L.off[c] < 0) continue;
        while (n < L.off[c]) out[n++] = ' ';
        for (int i = 0; i < L.w[c] && part[c][i]; ++i) out[n++] = part[c][i];
    }
    out[n] = 0;
}

// Horizontal arrangement (spec 06 §1.2 renderer 0x425c4; manual: tracks run
// left to right): the transposed grid, one band per track (rows) and one
// column per step. SPEC GAP: the spec gives the window rule only; the band
// layout is OpenTracker's own: each band stacks the shown columns (note,
// instrument, FX1, FX2, or the column-view mask) as text lines. The
// 15-step window scrolls only to keep the cursor within anchor-5..anchor+6, and while playing the anchor follows the playhead;
// steps outside 0..len-1 are blank.
int PatternScreen::hStepColW() { return (kScreenW - kHTrackLabelW) / kVisibleRows; }

void PatternScreen::drawGridHorizontal(Gfx& g) {
    App& a = app_;
    const Theme& th = ui_.th();
    const int l = len();
    const SelRect r = target();
    const bool hasSel = selState == 2 || wholeCol || allTracks;
    const int divider = a.project.u8(0x4F8);
    const int playStep = a.seq.isPlaying() ? a.seq.step(0) : -1;
    const int follow = playStep >= 0 ? playStep : step;
    if (follow < hAnchor_ - 5) hAnchor_ = follow + 5;
    if (follow > hAnchor_ + 6) hAnchor_ = follow - 6;
    if (hAnchor_ > l + 6) hAnchor_ = l + 6;
    if (hAnchor_ < -6) hAnchor_ = -6;
    const int first = windowTop();
    const int cw = hStepColW();
    const int headerY = gridTop() + 2;
    const int bandY0 = headerY + kHeaderH + 2;
    const int tracks = colMask ? kMaskTracks : kVisibleTracks;
    const int bandH = (kBarY - 2 - bandY0) / tracks;
    int shown[4], nShown = 0;
    for (int c = 0; c < 4; ++c)
        if (!colMask || (colMask & (1u << c))) shown[nShown++] = c;
    const int lineH = bandH / nShown;
    const int textDy = (lineH - kMonoH) / 2;
    const bool showNames = a.cfg.patternTopInfo == 1;
    char buf[24];
    // Step header and column backgrounds.
    for (int k = 0; k < kVisibleRows; ++k) {
        const int s = first + k;
        if (s < 0 || s >= l) continue;
        const int x = kHTrackLabelW + k * cw;
        uint32_t bg = th.bg;
        if (divider > 0 && s % (divider + 1) == 0) bg = th.panelDark;
        if (s == playStep) bg = th.panel;
        if (bg != th.bg) g.fill(x, bandY0, cw, tracks * bandH, bg);
        stepLabel(s, buf);
        const char* lb = buf;
        while (*lb == ' ') ++lb;
        g.fill(x + 1, headerY, cw - 2, kHeaderH, s == step ? th.panel : th.panel2);
        g.text(x + cw / 2, headerY + kHeaderH / 2, kFontMono, s == step ? th.text : th.dim, lb, IDisplay::kOptCenter);
    }
    constexpr int kMaxEmpty = kVisibleRows * kMaskTracks * 4;
    int16_t emptyX[kMaxEmpty], emptyY[kMaxEmpty], emptyW[kMaxEmpty];
    int nEmpty = 0;
    for (int v = 0; v < tracks; ++v) {
        const int t = firstTrack + v;
        if (t >= kTrackCount) break;
        const int y = bandY0 + v * bandH;
        const bool muted = a.trackMuted(t);
        // Track label at the left of its band.
        g.fill(0, y + 1, kHTrackLabelW - 2, bandH - 2, t == track ? th.panel : th.panel2);
        g.textf(4, y + (nShown > 1 ? 12 : bandH / 2), kFontSmall, muted ? th.dim : th.text, IDisplay::kOptCenterY, "%d",
                t + 1);
        if (showNames && nShown > 1) {
            a.project.trackName(t, buf, sizeof(buf));
            buf[6] = 0;
            g.text(4, y + 32, kFontSmall, muted ? th.dim : th.text, buf, IDisplay::kOptCenterY);
        }
        if (muted) g.text(kHTrackLabelW - 6, y + 12, kFontSmall, th.alert, "M", IDisplay::kOptCenterY | IDisplay::kOptRightX);
        for (int k = 0; k < kVisibleRows; ++k) {
            const int s = first + k;
            if (s < 0 || s >= l) continue;
            const int x = kHTrackLabelW + k * cw;
            const bool selected = hasSel && t >= r.t0 && t <= r.t1 && s >= r.s0 && s <= r.s1;
            if (selected) g.fill(x + 1, y + 1, cw - 2, bandH - 2, th.trackDark[0]);
            const Step& st = a.pattern().tracks[t].steps[s];
            for (int i = 0; i < nShown; ++i) {
                const int c = shown[i];
                const int ly = y + i * lineH;
                const bool cur = t == track && s == step && c == col;
                if (cur) g.fill(x + 2, ly + 1, cw - 4, lineH - 2, edit() ? th.accent : th.dim);
                char part[8];
                if (c == 0) {
                    noteName(st.note, part);
                } else if (c == 1) {
                    if (st.note == kNoteEmpty && !isMidiInstrument(st.instrument)) std::strcpy(part, "--");
                    else if (isMidiInstrument(st.instrument)) std::snprintf(part, sizeof(part), "M%02d", st.instrument - 47);
                    else if (st.instrument >= kInstrumentCount) std::snprintf(part, sizeof(part), "S%c", char('1' + (st.instrument - kInstrumentCount) % 9));
                    else std::snprintf(part, sizeof(part), "%02d", st.instrument + 1);
                } else {
                    fxCellText(st.fx[c - 2].type, st.fx[c - 2].value, part);
                }
                const bool empty = c == 0 ? st.note == kNoteEmpty
                                 : c == 1 ? st.note == kNoteEmpty
                                          : st.fx[c - 2].type == 0;
                const int chars = c == 0 ? 3 : (c == 1 ? 2 : kFxCellChars);
                if (empty && !cur) {
                    if (nEmpty < kMaxEmpty) {
                        emptyX[nEmpty] = int16_t(x + (cw - chars * kMonoW) / 2);
                        emptyY[nEmpty] = int16_t(ly + lineH / 2 - 1);
                        emptyW[nEmpty++] = int16_t(chars * kMonoW - 2);
                    }
                    continue;
                }
                const uint32_t fc = muted ? th.dim : (cur ? th.text : th.track[c]);
                // Glyph run, centred as CMD_TEXT OPT_CENTER would: one word
                // per glyph instead of a CMD_TEXT each (VERIFY_1.9.5 R6).
                g.glyphRun(x + cw / 2 - textWidth(kFontMono, part) / 2,
                           ly + textDy + kMonoH / 2 - fontHeight(kFontMono) / 2, kFontMono, part, fc);
            }
        }
        if (v > 0) g.hline(0, y, kScreenW, th.grid);
    }
    if (nEmpty) {
        g.rectsBegin(th.grid);
        for (int i = 0; i < nEmpty; ++i) g.rectsAdd(emptyX[i], emptyY[i], emptyW[i], 2);
        g.rectsEnd();
    }
    g.vline(kHTrackLabelW - 1, headerY, kBarY - headerY - 2, th.grid);
}

namespace {
void fillScaleItem(int i, char* out, int cap) { std::snprintf(out, size_t(cap), "%s", scaleName(uint8_t(i))); }
void fillFxItem(int i, char* out, int cap) {
    // Index -1 = random type, then the ordered Fx list (spec 06 §1.10).
    if (i < 0) std::snprintf(out, size_t(cap), "Random Fx");
    else std::snprintf(out, size_t(cap), "%s", fxInfo(kFxMenuOrder[i]).name);
    // The part before a '/' ("Volume/Velocity" -> "Volume") [S snap 019].
    if (char* sl = std::strchr(out, '/')) *sl = 0;
}
}  // namespace

// Fill popup (spec 06 §1.10, spec 20 §4.1, Part 2 §8.8): header band
// (0, 29, 800, 28) with the bold " Fill …" title, then one 0a0a0a column
// panel per bottom key at y 58: the list fields (F1 Where?, F3 Scale / Fx
// type, F4 Fill type) as lists, the value fields (F2, F5, F6) as value
// bars; the selected field's column carries the red focus frame.
void PatternScreen::drawFill(Gfx& g) {
    const Theme& th = ui_.th();
    static const char* kTitles[4] = {"Fill Notes", "Fill Instruments", "Fill Fx 1", "Fill Fx 2"};
    const pops::FillParams& f = fill[col];
    constexpr int kTop = 58, kListH = 365, kBarH = 364;
    g.fill(0, wg::kMainTop, kScreenW, kTop - wg::kMainTop, th.slot(slot::kTitle + 1));
    g.text(10, wg::kMainTop + 12, kFont4, th.slot(slot::kTitle), kTitles[col], IDisplay::kOptCenterY);
    if (col == 1) {   // 2.x: the From / To instruments by name (R3-5)
        App& a = app_;
        auto name = [&](int v, char* out, int cap) {
            char code[8];
            fillInstrCode(v, code);
            const int i = clampi(v, 0, 255);
            if (i < kSampleInstrumentCount) std::snprintf(out, size_t(cap), "%s %s", code, a.instrPresent[i] ? a.instruments[i].name : "");
            else if (isSynthInstrument(uint8_t(i))) std::snprintf(out, size_t(cap), "Synth %d", synthSlotOf(uint8_t(i)) + 1);
            else std::snprintf(out, size_t(cap), "MIDI %d", i - kFirstMidiInstrument + 1);
        };
        char n1[48], n2[48], line[112];
        name(f.from, n1, sizeof(n1));
        if (f.type != 0) {
            name(f.to, n2, sizeof(n2));
            std::snprintf(line, sizeof(line), "From %s   To %s", n1, n2);
        } else {
            std::snprintf(line, sizeof(line), "From %s", n1);
        }
        g.text(790, wg::kMainTop + 12, kFont3, th.slot(slot::kTitle), line, IDisplay::kOptCenterY | IDisplay::kOptRightX);
    }
    struct ListCtx {
        int first;
        void (*fn)(int, char*, int);
    };
    auto item = [](void* ctx, int i, char* out, int cap) {
        const ListCtx* c = static_cast<const ListCtx*>(ctx);
        c->fn(c->first + i, out, cap);
    };
    auto list = [&](int k, int count, int firstIdx, int cur, void (*fn)(int, char*, int)) {
        ListCtx ctx{firstIdx, fn};
        wg::ListView v;
        v.x = wg::columnX(k);
        v.y = kTop;
        v.w = wg::columnW();
        v.h = kListH;
        v.count = count;
        v.sel = cur - firstIdx;
        v.top = wg::listFollow(v.sel, count, wg::listVisibleRows(v.h), 0);
        v.selStyle = wg::ListSel::kWhite;
        v.item = item;
        v.ctx = &ctx;
        wg::list(g, th, v);
    };
    auto bar = [&](int k, float frac) {
        // Value 0 shows the 1.5-px white base line (widget, Part 2 §7.4).
        wg::valueBar(g, th, wg::columnX(k), kTop, wg::columnW(), kBarH, frac < 0 ? 0 : (frac > 1 ? 1 : frac));
    };
    auto empty = [&](int k) { wg::panel(g, th, wg::columnX(k), kTop, wg::columnW(), kListH); };
    auto valueFrac = [&](int v) { return fillValueFrac(v); };
    list(0, 7, 0, clampi(f.where, 0, 6), [](int i, char* o, int c) { std::snprintf(o, size_t(c), "%s", kWhereNames[i]); });
    if (f.where >= 4) bar(1, (f.param[f.where] - 1) / 127.0f);
    else empty(1);
    if (col == 0) list(2, kScaleCount, 0, clampi(f.scaleOrFx, 0, kScaleCount - 1), fillScaleItem);
    else if (col >= 2) list(2, kFxCount, -1, clampi(f.scaleOrFx, -1, kFxCount - 2), fillFxItem);
    else empty(2);
    list(3, 3, 0, clampi(f.type, 0, 2), [](int i, char* o, int c) { std::snprintf(o, size_t(c), "%s", kFillTypes[i]); });
    bar(4, valueFrac(f.from));
    if (f.type != 0) bar(5, valueFrac(f.to));
    else empty(5);
    empty(6);
    empty(7);
    if (fillField >= 0 && fillField < 6)
        wg::focusFrame(g, th, wg::columnX(fillField), kTop, wg::columnX(fillField) + wg::columnW(), 422);
}

bool PatternScreen::perfMarker() const {
    // spec 06 §1.2: in Performance mode, a track playing a pattern other
    // than the edited one shows the "PERF" marker.
    const Sequencer& sq = app_.seq;
    if (!sq.performanceMode()) return false;
    for (int t = 0; t < Sequencer::kVoiceTracks; ++t) {
        const int p = sq.trackPattern(t);
        if (p && p != app_.patternNumber()) return true;
    }
    return false;
}

bool PatternScreen::topInfoOn() const { return app_.cfg.patternTopInfo != 2; }
int PatternScreen::gridTop() const { return topInfoOn() ? kTitleH : 0; }
int PatternScreen::rowHeight() const { return topInfoOn() ? kRowHInfo : kRowHOff; }

// Title bar (spec 20 §1.2, §4.1 top info; Part 2 §7.2): pattern title,
// the status group (play time, transport glyphs, P pattern.step: the
// playhead while playing, the cursor step otherwise) and the right text
// (instrument "N. name" / "N. MIDI Channel c" / "Synth k (MODEL)", or the Fx name on an Fx
// column). A right-hand popup hides the status group and the right text.
void PatternScreen::drawTitleBar(Gfx& g, bool popupRight) {
    App& a = app_;
    char title[48];
    const char* nm = a.names.get(a.patternNumber());
    if (nm && nm[0]) std::snprintf(title, sizeof(title), "%s", nm);
    else std::snprintf(title, sizeof(title), a.cfg.numbering == 2 ? "Pattern %02X" : "Pattern %d", a.patternNumber());
    if (perfMarker()) std::strncat(title, "  PERF", sizeof(title) - std::strlen(title) - 1);
    char sub[64];
    if (col <= 1) {
        const uint8_t ci = a.currentInstrument();
        if (ci < kSampleInstrumentCount)
            std::snprintf(sub, sizeof(sub), "%d. %s", ci + 1, a.instrPresent[ci] ? a.instruments[ci].name : "");
        else if (isSynthInstrument(ci))   // "Synth 1 (WTFM)" (Tracker+, docs/SYNTH_UX_MINI_VS_OT.md row 3)
            synthui::slotLabel(a, synthSlotOf(ci), sub, sizeof(sub));
        else
            std::snprintf(sub, sizeof(sub), "%d. MIDI Channel %d", ci + 1, ci - kFirstMidiInstrument + 1);
    } else {
        const StepFx& f = a.pattern().tracks[track].steps[step].fx[fxSlotForColumn(col)];
        std::snprintf(sub, sizeof(sub), "%s", fxShortName(f.type));
    }
    // spec 06 §1.2, spec 20 §1.6: while the sequencer waits for a recording
    // to start (external clock, count-in) "REC" blinks with a 500 ms period
    // (update() toggles recBlink_ every 250 ms), next to the steady record
    // glyph of the status group. Placed before the right text so a long
    // instrument name (cut at its end) cannot push it out [I placement;
    // 1.9.4 appended it].
    if (recWaitShown_ && recBlink_) {
        static constexpr char kRec[] = "REC   ";
        const size_t n = sizeof(kRec) - 1;
        std::memmove(sub + n, sub, sizeof(sub) - n - 1);
        sub[sizeof(sub) - 1] = 0;
        std::memcpy(sub, kRec, n);
    }
    // Status: Ui::drawTitle with the cursor step set in update()
    // (setStatusCursorStep, shown while stopped: P10.014 after 13 rows
    // down); a right popup header hides the status group.
    ui_.drawTitle(g, title, popupRight ? nullptr : sub);
}
}  // namespace ot
