// Screen classes (spec 06 per-screen behaviour; spec 05 §2 where 06 is silent).
#pragma once

#include <cstdint>

#include "app/auto_slice.h"
#include "app/midi_cc.h"
#include "app/pattern_ops.h"
#include "app/perf_presets.h"
#include "app/project_browser.h"
#include "model/synth_patch.h"
#include "ui/ui.h"
#include "ui/wave.h"

namespace ot {

// [I] One buffer for the browsers' name lists (SDRAM on the device,
// screens.cpp): Sample Loader, Synth patch browser, File Open / Save,
// Import Mod and the firmware-update list. Only one is on screen at a time
// and each rebuilds its list when it opens (LoaderScreen::start,
// SynthScreen::openBrowser, FileScreen enterBrowser / F3, ConfigScreen's
// firmware entry), so they share it; each list's layout static_asserts that
// it fits. The largest is Import Mod (200 x 56 names + 200 kinds).
constexpr size_t kBrowserListBytes = 200 * 56 + 200;
extern unsigned char g_browserLists[kBrowserListBytes];   // screens.cpp (.sdram_bss on the device)
inline void* browserListMem() { return g_browserLists; }

void* createScreens(Ui& ui, App& app, Screen** out);   // returns the owner
void destroyScreens(void* owner, Screen** screens);

// ---------------------------------------------------------------------------
// Pattern editor (spec 06 §1)
class PatternScreen final : public Screen {
public:
    using Screen::Screen;
    const char* name() const override { return "Pattern"; }
    void start(int opt) override;
    void stop() override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onPad(uint8_t pad, uint8_t state) override;
    void onEncoder(int d) override;
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const override;
#endif
    bool encoderFast() const override { return true; }
    bool ownsModuleKey(uint8_t id) const override { return id == btn::Pattern; }
    void padLeds(uint8_t* levels) override;
    // The Pattern screen draws its whole frame (strip, title, hairline, bar
    // labels) itself: the popups cover the bar and the title hairline
    // (VISUAL_DIFF Part 2 §7.2, §7.3).
    bool fullScreen() const override { return true; }

    // Editor state (spec 06 §1.1 table), public for tests.
    int track = 0, step = 0, col = 0;
    int firstTrack = 0;
    int selState = 1;               // 1 cursor, 2 rectangle
    int anchorTrack = 0, anchorStep = 0;
    bool wholeCol = false;
    bool allTracks = false;
    int focus = -1;                 // -1 none, 0 pattern, 1 length, 2 step jump, 5 undo, 6 redo
    int page = 0;
    bool fillOpen = false;
    void fillValueText(int val, char* out) const;   // From/To display text (8 bytes)
    float fillValueFrac(int val) const;             // From/To bar fraction 0..1
    int fillField = 0;
    pops::FillParams fill[4];
    uint8_t colMask = 0;            // column-view mask (spec 06 §1.9)
    bool recArmPage = false;
    // Live-REC arm per track (spec 06 §1.3; 2.x: 16 tracks, 2x-01 §2.4).
    bool armed[kTrackCount] = {true, true, true, true, true, true, true, true,
                               true, true, true, true, true, true, true, true};

    SelRect target() const;
    bool edit() const;
    // The cursor is red in edit mode and while recording (Rec + Play leaves
    // edit mode; stock keeps the red cursor, USER_REPORTS_R3 R3-3).
    bool recordCursor() const;
    int windowTop() const { return hAnchor_ - 7; }   // first visible step, horizontal arrangement
    static int hStepColW();                          // horizontal arrangement: step column width

    // MIDI note-in step entry (spec 17 §1.4 step 8): the note sounds on
    // track t; with the sequencer stopped, edit mode on and t inside the
    // target track range, it is written at (startStep, t). No cursor move.
    // Returns false when entry does not apply.
    bool midiChordEntry(int t, uint8_t note, uint8_t velocity);
    // Grid geometry for the "Pattern top info" config (spec 06 §1.2).
    bool topInfoOn() const;
    int gridTop() const;           // y of the grid control (28, or 0 when off)
    int rowHeight() const;
    // "PERF" marker in the top info bar (spec 06 §1.2).
    bool perfMarker() const;
    // Render selection after the name keyboard (spec 06 §1.11); load = the
    // "render & load" variant (first free sample slot). False on failure,
    // cancel or no free slot.
    bool renderSelection(const char* name, bool load);
    // "Counting in" popup shown (spec 16 §B.9).
    bool countInShown() const;
    // Note popup window base: the note under F1 (tests).
    int noteWindowBase() const { return noteBase_; }
    // Column-key popups (spec 06 §1.6): Note (kind 0, 12 x 4 keys), Instrument
    // (2) / MIDI channel (5) and Fx (3) lists. The screen keeps its own popup
    // state (the shared Ui picker is not used here) and draws it in stock
    // style (VISUAL_DIFF Part 2 §8.4..§8.7).
    const Picker& popup() const { return pop_; }
    // Notes popup: note of the top-left key's octave row 3 (C of the lowest
    // shown octave; rows show win .. win + 47, highest octave on top).
    int noteKeysWindow() const { return noteWin_; }

private:
    void refreshBar();
    void moveCursor(int dir, bool shiftHeld);   // dir: 0 step-1, 1 step+1, 2 track+1, 3 track-1
    void advance(int extraRows = 0);
    void collapse();
    void keepTrackVisible();
    ColMode heldColMode(bool forDelete) const;
    void focusArrow(uint8_t id);
    void focusDelta(int d);
    void editEncoder(int d);
    void openPicker(int which);
    void commitPicker();
    void noteKey(uint8_t id, uint8_t state);    // Note popup F1..F8 (play / pick / Shift window shift)
    void noteKeysOff();
    void writeFxPad(const SelRect& r, int pad);   // FX column pad: type (Fx key held) or value
    void transport();
    void fillButton(uint8_t id);
    void fillDelta(int d, bool wrap = false);
    void fillApply();
    uint8_t fillFxType() const;                // Fx id of the Fill "Fx Type" index (>= 0)
    void clampFillValues();                    // From/To into the chosen Fx's range
    void pageButton(int f, uint8_t state);
    void drawGrid(Gfx& g);
    void drawGridVertical(Gfx& g, bool threeTracks);
    void drawTitleBar(Gfx& g, bool popupRight);
    void drawBar(Gfx& g);
    void drawMuteBar(Gfx& g);
    void drawListPopup(Gfx& g);
    void drawNotesPopup(Gfx& g);
    bool popupMove(uint8_t id);                // arrows inside a popup
    void popupPad(uint8_t pad);
    void popupLeds(uint8_t* lv) const;
    void followNoteWindow();
    void drawGridHorizontal(Gfx& g);   // arrangement 2: tracks as rows, steps as columns
    void drawFill(Gfx& g);
    void drawCountIn(Gfx& g);
    void cellText(int t, int s, char* out) const;
    void stepLabel(int s, char* out) const;
    int len() const;
    int lastNote() const;

    uint8_t colKey_ = 0;            // column key waiting for its hold popup
    uint32_t colKeyAt_ = 0;
    uint8_t pickerKey_ = 0;
    int noteBase_ = 48;             // Note popup: note under F1 (window of 8)
    uint8_t noteKeysOn_ = 0;        // Note popup F-keys sounding (bit = F-key)
    bool encUsedInHold_ = false;
    bool patternHeld_ = false;
    bool patternCombo_ = false;   // another key / the encoder was used while Pattern was held
    int maskKeys_[2] = {-1, -1};
    uint32_t lastUpdate_ = 0;
    int lastPlayStep_ = -1;
    int hAnchor_ = 7;               // horizontal arrangement window anchor (spec 06 §1.2)
    bool blink_ = false;
    bool lastShift_ = false;
    bool recBlink_ = false;         // 500 ms blink while a recording waits to start
    bool recWaitShown_ = false;
    bool fillNoteInit_ = false;     // Fill note defaults taken from the project (spec 06 §1.10)
    uint32_t countInUntil_ = 0;     // "Counting in" popup end (ms), 0 = none
    bool countInDrawn_ = false;
    int8_t padVoice_[48];
    Picker pop_;                    // column-key popup state (drawn by this screen)
    int noteWin_ = 36;              // Notes popup: lowest shown note (C3)
    int popTop_ = 0;                // list popup: first visible row
    int statusStep_ = -2;           // step last given to Ui::setStatusCursorStep
};

// ---------------------------------------------------------------------------
// Song (spec 06 §2)
class SongScreen final : public Screen {
public:
    using Screen::Screen;
    const char* name() const override { return "Song"; }
    void start(int opt) override;
    void stop() override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onEncoder(int d) override;
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const override;
#endif
    bool ownsModuleKey(uint8_t id) const override { return id == btn::Song; }

    int length() const;
    int cursor = 0;
    bool edit = false;
    int focus = 1;                 // 0 grid, 1 list, 2 tempo
    int selRow0 = 0, selRows = 1, selTrack0 = 0, selTracks = 1;
    // 2.x (docs/M1_PLAN.md §4, spec gap 4): the grid shows 12 of the 16
    // tracks; the first shown track is 0 or 4 and follows the grid cursor.
    static constexpr int kGridCols = 12;
    int firstTrack = 0;
    int tapCount = 0;
    uint32_t tapT0 = 0;
    bool tapShown = false;         // "TAP n/4" display flag SO+0x131 (spec 06 §2.4)
    int transportState() const;    // F1/F2 label state 0..5 (spec 06 §2.2)
    // Loop Pattern active in the current play run (a stop elsewhere ends it).
    bool loopingInSong() const;
    bool loopTogglePending() const { return togglePending_; }

private:
    void refreshBar();
    void setPattern(int slot, int num);
    void tempoDelta(float d, bool integer);
    void addSlot(bool empty);
    void deleteSlot();
    void copyJob(bool paste);
    void deleteJob();
    void followGridCursor(int edge);   // window so that track `edge` is shown
    void applyLoopToggle();
    void reloadPlayingSlot();
    int savedFocus_ = 1;
    bool songKeyHeld_ = false;
    bool playWhileSongKey_ = false;
    bool clipValid_ = false;
    int clipRow0_ = 0, clipRows_ = 0, clipTrack0_ = 0, clipTracks_ = 0;
    uint32_t lastUpdate_ = 0;
    int followPos_ = -1;           // playing slot the cursor last followed (-1: follow at once)
    uint32_t loopRun_ = 0;         // App::playStartMs() of the run that set loopInSong_
    bool loopInSong_ = false;      // state 3: song paused on one pattern (spec 06 §2.2)
    bool togglePending_ = false;   // F1 deferred action (seq+0x548 = 0x5ae00, spec 06 §2.3)
    int toggleStep_ = 0;           // master step at scheduling; fires on the next one
};

// ---------------------------------------------------------------------------
// Instrument parameters (spec 05 §2.7 + appendix C)
class InstrumentScreen final : public Screen {
public:
    using Screen::Screen;
    const char* name() const override { return "Instrument"; }
    void start(int opt) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onEncoder(int d) override;
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const override;
#endif
    bool encoderFast() const override { return true; }
    bool ownsModuleKey(uint8_t id) const override { return id == btn::InstrParams; }
    void onPad(uint8_t pad, uint8_t state) override;
    void padLeds(uint8_t* levels) override;

    // 0 params, 1 automation, 2 MIDI Mapping, 3 MIDI instrument; effects = mode 0 + effects flag
    int mode = 0;
    bool effects = false;
    int column = 0;        // mode 2: 0 = CC list pane, 1 = Instruments pane
    int dest = 0;          // automation destination 0..5
    int ccScroll = 0;      // MIDI Mapping CC list row 0..46 (appendix C §1 +0x334)
    uint8_t auditionNote() const;   // Enc-push audition note (appendix C §3, 0x30138)

private:
    int columns() const;
    bool selectable(int c) const;   // appendix C §3 column rules
    void onMappingButton(uint8_t id);
    void mappingMove(int d);
    void drawMapping(Gfx& g);
    void edit(int col, int d, bool tap);
    void reset(int col);
    void refreshBar();
    void valueText(int col, char* out, int cap, float* frac, bool* bipolar) const;
    bool held_[8] = {};
    bool usedWhileHeld_[8] = {};
};

// ---------------------------------------------------------------------------
// Sample loader (spec 06 §5)
class LoaderScreen final : public Screen {
public:
    using Screen::Screen;
    const char* name() const override { return "Sample Loader"; }
    void start(int opt) override;
    void stop() override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onPad(uint8_t pad, uint8_t state) override;
    void onEncoder(int d) override;
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const override;
#endif

    static constexpr int kMaxEntries = 101;   // spec 06 §5.2
    static constexpr int kMaxDepth = 20;
    char dir[200] = {};
    int count = 0;
    // In browserListMem(): names, then the folder flags.
    char (*entries)[40] = static_cast<char (*)[40]>(browserListMem());
    bool* isDir = static_cast<bool*>(browserListMem()) + kMaxEntries * 40;
    int sdCursor = 0;
    int insCursor = 0;
    int focus = 0;          // 0 SD, 1 instruments, 2 preview volume
    // Trim (import popup, spec 06 §5.5)
    bool trim = false;
    uint16_t trimStart = 0, trimEnd = 65535;
    int trimSel = 0;
    uint32_t trimLen_ = 0;

    void list();
    void enter();
    void add(bool next);
    void openImport();                  // F5 (spec 06 §5.5 step 1)
    // §5.5 gating: Normal iff 2n < free - 10000, low quality iff n < free - 10000.
    bool importAllowed(bool lowQuality) const;
    bool over() const { return over_; }

private:
    void refreshBar();
    void selectionRange(int* first, int* last) const;               // focused list
    void listRange(bool sd, int* first, int* last) const;
    void memoryProjection();
    void startTrim();
    void moveCursor(int d);
    void doAdd(bool next);
    void entryPath(int i, char* out, int cap) const;
    uint32_t now() const { return ui_.now(); }
    int depth_ = 0;
    int cursorHist_[kMaxDepth] = {};
    uint8_t marked_[kMaxEntries] = {};
    uint8_t insMarked_[48] = {};
    int prevFocus_ = 0;
    uint32_t projected_ = 0;
    bool over_ = false;
    bool addNext_ = false;
    int copySrc_ = -1, copyCount_ = 0;
    bool previewing_ = false;
    uint32_t pendingProjAt_ = 0;
    bool f7Hold_ = false;               // F7 produced a hold tick (§5.3)
    bool trimPlaying_ = false;
    uint32_t trimPlayAt_ = 0, trimElapsed_ = 0;
};

// ---------------------------------------------------------------------------
// Master / mixer (spec 06 §4)
class MixerScreen final : public Screen {
public:
    using Screen::Screen;
    const char* name() const override { return "Master"; }
    void start(int opt) override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onEncoder(int d) override;
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const override;
#endif
    bool ownsModuleKey(uint8_t id) const override { return id == btn::Master; }

    int page = 0;           // 0 master, 1 track mixer, 2 global mixer
    int sub = 0;            // master sub-page: 0 main, 1 reverb, 2 delay, 3 limiter, 4 EQ
    int param = 0;          // selected parameter on the (sub-)page
    bool rename = false;
    // 2.x Track Mixer bank (docs/specs/2x-01 §4): 0 = tracks 1-8, 1 = tracks
    // 9-16; Left / Right switch it.
    int bank = 0;
    // Bank B note-activity dots (tracks 9-16 have no audio meter in M1):
    // lit for kActivityMs after a step with a note starts on the track.
    static constexpr uint32_t kActivityMs = 150;
    bool activity[8] = {};

    // Track Mixer level meter law (spec 06 §4.4, 0x35454): mean of the last
    // 10 level readings / 0.71 -> dB; -3.6..+0.6 dB maps linearly onto 0..100
    // (clamped outside). Peak hold falls 1 unit per 500 ms.
    static int meterBar(float meanLevel);
    int meter[8] = {};      // current bar 0..100
    int meterHold[8] = {};  // peak hold 0..100
    int globalMeter[4][2] = {};   // Global Mixer 3/3: Delay, Reverb, Dry, Line In x L/R, 0..100
    // Master Limiter gain-reduction meter (spec 06 §4.3): dB of reduction
    // from the engine's mean limiter gain, read every 50 ms; the bar spans
    // 0..kGrRangeDb (OpenTracker's scale).
    static constexpr float kGrRangeDb = 24.0f;
    static float reductionDb(float meanGain);
    float gainReduction = 0.0f;   // dB, >= 0
    // 2.x Global Mixer 3/3 synth meters (manual p.220, "right is the
    // actual"): mean of the last 10 slot-bus peaks (10 ms apart), -48..0 dBFS
    // onto 0..100 [I] (the stock track law spans only -3.6..+0.6 dB).
    static int synthMeterBar(float meanPeak);
    int synthMeter[3] = {};
    float synthHist_[3][10] = {};
    // True when parameter `param` of Master sub-page `sub` is an option list
    // (encoder + / Down = next item, Up = previous item) rather than a bar.
    static bool isListParam(int sub, int param);

private:
    void refreshBar();
    void change(int d);
    void masterChange(int d);
    void mixerKey(int i, uint8_t state);
    void updateActivity(uint32_t now);
    void drawBankB(Gfx& g, uint32_t off);
    bool held_[8] = {};
    bool changed_[8] = {};
    // Master key (spec 06 §4.1): snapshot taken on press.
    bool masterPending_ = false;
    bool otherKeyDuringMaster_ = false;
    bool masterHoldFired_ = false;
    int snapPage_ = 0, snapSub_ = 0, snapParam_ = 0;
    int solo_ = -1;
    int globalSolo_ = -1;
    // Level meters (spec 06 §4.4).
    uint32_t lastMeter_ = 0;
    uint32_t lastDecay_ = 0;
    uint32_t lastGr_ = 0;
    float hist_[8][10] = {};
    float ghist_[4][2][10] = {};
    int histPos_ = 0;
    // Bank B activity (update()): last step seen and the dot's end time.
    int16_t actStep_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint32_t actUntil_[8] = {};
};

// ---------------------------------------------------------------------------
// File (spec 05 §2.12 + appendix E)

class FileScreen final : public Screen {
public:
    using Screen::Screen;
    const char* name() const override { return "File"; }
    void start(int opt) override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onEncoder(int d) override;
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const override;
#endif

    int state = 0;          // 0 main, 1 open, 2 import mod, 3 save, 4 export
    int count = 0;
    // Open / Save list names (ProjectBrowser::kMaxEntries x 40). 2.x: 200
    // entries (stock 80, spec 05 appendix E1.2), in browserListMem().
    char (*entries)[40] = static_cast<char (*)[40]>(browserListMem());
    int cursor = 0;
    int focus = 0;          // open: 0 SD list, 1 recent
    int pending = 0;        // action after a save prompt: 1 new, 2 open, 3 recent, 4 import, 5 .it

    void doNew();
    void doOpenSelected();
    void list();

    // Project cover preview (spec 05 appendix E1.6, spec 13 §I.3): 0 idle,
    // 1 waiting for the 2 s hold, 2 loaded (drawn), 3 no/invalid image.
    int coverState = 0;
    uint16_t coverW = 0, coverH = 0;

private:
    void refreshBar();
    void askSaveThen(int action);
    void runExport(int id);
    void onImportButton(uint8_t id);
    void openAction();
    void importAction();
    void askStopThen(int action);
    int exportMode_ = 0;
    int stopThen_ = 0;              // action after "stop the pattern": 1 open, 2 import
    char recent_[10][64] = {};
    int recentCount_ = 0;
    char coverKey_[300] = {};       // "<folder>/<project>" the cover timer runs for
    uint32_t coverAt_ = 0;
};

// ---------------------------------------------------------------------------
// Config (spec 06 §7)
class ConfigScreen final : public Screen {
public:
    using Screen::Screen;
    const char* name() const override { return "Config"; }
    void start(int opt) override;
    void stop() override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onPad(uint8_t pad, uint8_t state) override;
    void onEncoder(int d) override;
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const override;
#endif
    bool encoderFast() const override { return true; }
    // The Sample Recorder key goes back to the recorder that opened Config
    // with F4 Metronome (spec 05 appendix C §2.9).
    bool ownsModuleKey(uint8_t id) const override;

    // Factory-test entry code (spec 06 §7.6): F8,F1,F7,F2 or pads 47,10,45,8,
    // one shared counter.
    int codePos = 0;
    void feedCode(bool pad, uint8_t v);
    // Firmware-update list (spec 06 §7.5): at most 101 names, sorted.
    static constexpr int kFwMax = 101;
    static int firmwareCount();
    static const char* firmwareName(int i);
    // /.develop overlay values (spec 06 §7.2), refreshed every > 1 s.
    DevStats dev;
    bool devValid = false;

    int category = 0;
    int option = 0;
    int focus = 0;          // 0 categories, 1 options
    bool editing = false;
    int picker = 0;

    // Menu model (spec 06 §7.1, option list spec 15 §2), for tests/tools.
    struct OptionInfo {
        const char* name;
        uint8_t kind;       // 2 numeric, 3 enum, 5 text, 7 action (spec 06 §7.1)
        uint8_t store;      // 0 global config, 1 project, 2 none, 3 live USB storage state (2.x)
        int count;          // picker entries (numeric: max - min + 1)
        int min;            // numeric minimum (stored value of picker index 0)
        int displayOffset;  // numeric: shown value = stored + displayOffset
        const char* help;   // help box text (spec 15 §3.2), nullptr if none
    };
    static int categoryCount();
    static const char* categoryName(int cat);
    static int optionCount(int cat);
    static bool optionInfo(int cat, int i, OptionInfo* out);
    int optionIndex(int cat, int i) const;   // current picker index of the value
    void optionText(int cat, int i, char* out, int cap) const;
    // Select option i of category cat, pick `index` and Apply, like the keys
    // do (tests/tools; also the way to reach a row under a full-screen page).
    void applyOption(int cat, int i, int index);

private:
    void refreshBar();
    void enter();
    void apply();
    void moveCat(int d);
    void moveOpt(int d);
    void movePick(int d);
    bool returnToRecorder();
    uint32_t lastDev_ = 0;
};

// ---------------------------------------------------------------------------
// Sample playback (spec 05 §2.9 + appendix B)
class PlaybackScreen final : public Screen {
public:
    using Screen::Screen;
    const char* name() const override { return "Sample Playback"; }
    void start(int opt) override;
    void stop() override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onPad(uint8_t pad, uint8_t state) override;
    void onEncoder(int d) override;
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const override;
#endif
    bool encoderFast() const override { return true; }
    void padLeds(uint8_t* levels) override;
    bool onMidiNote(uint8_t note, uint8_t velocity, bool on) override;
    // Selected parameter (appendix B §1.5 obj+0xd6): 0 preview/slice,
    // 1 start/adjust/position, 2 loop start/no of slices/length/window,
    // 3 loop end/shape, 4 end/zoom (slices)/loop type, 5 zoom, 6 play mode.
    int sel = 1;
    bool more = false;          // slice "More" page (obj+0xf0)
    uint8_t heldPoints = 0;     // held point buttons: 1 start, 2 loop start, 4 loop end, 8 end (obj+0xab2)
    WaveZoom view;              // spp zoom (appendix B §1.9)
    AutoSlicer slicer;          // spec 13 §F, stepped from update()
    int midiNote = -1;          // MIDI note holding the b0 preview voice (spec 17 §1.3 owner 2), -1 none
    float zoom() const { return view.zoom(); }
    // Playhead (spec 05 §6, 0x52cc0: redraw every >= 5 ms): x of the last
    // auditioned voice's play position in the waveform, -1 when none shows.
    static constexpr uint32_t kHeadPollMs = 5;
    int playheadX() const;
    // Wavetable mode (manual p.130-131, spec 05 appB §1.4d): the window shown
    // (the preview voice's, so an LFO animates it; else the instrument's) and
    // the x of its marker on the position bar (-1 when not in Wavetable).
    uint32_t wavetableShownWindow() const;
    uint16_t granularShownPosition() const;   // Granular: the preview voice's grain position, else the instrument's
    static constexpr uint32_t kGrainColour = 0x5A8CDC;   // the manual's blue grain (p.135) [OT shade]
    int wavetablePositionX() const;
    uint32_t wtShown_ = 0xFFFFFFFFu;
    uint16_t grainShown_ = 0;   // wavetable window last drawn (update redraws on change)
    int drawnHeadX() const { return headX_; }
    // Marker-on-crossing bits (spec 13 §H.2, obj+0xab3): same order as heldPoints.
    uint8_t zeroMask() const;
    // Marker i (0 start, 1 loop 1, 2 loop 2, 3 end): green on a zero crossing,
    // else red when selected / held, else the wave colour.
    uint32_t markerColour(int i) const;
    // Slice operations (appendix B §1.10) on the current instrument.
    void addSlice();
    void removeSlice();
    void slicesEvenly();
    void slicesAuto();
    void requestSlices(bool autoMode);   // with the overwrite confirmation

private:
    void refreshBar();
    void change(int d);
    void setMode(int m);
    void movePoint(int which, int d);
    void keepVisible(uint16_t m, bool force);
    void sliceDone();
    void drawModeList(Gfx& g, const Theme& th, int mode) const;
    int lastInstr_ = -1;
    int lastMode_ = -1;
    int sliceInstr_ = -1;       // instrument the detector writes to
    bool wasBusy_ = false;
    bool encPushDown_ = false;
    void followHead(uint16_t voiceId);
    int headTrack_ = -1;        // track of the last audition started here
    int headX_ = -1;            // playhead x in the last drawn frame
    uint32_t headAt_ = 0;       // last playhead poll (ms)
};

// ---------------------------------------------------------------------------
// Performance (spec 06 §3, spec 12 §A-§E)
class PerformanceScreen final : public Screen, public PerformanceCcSink {
public:
    using Screen::Screen;
    const char* name() const override { return "Performance"; }
    void start(int opt) override;
    void stop() override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onPad(uint8_t pad, uint8_t state) override;
    void onEncoder(int d) override;
    bool encoderFast() const override { return true; }
    // 2.x: the Performance key toggles the track bank (docs/specs/2x-01 §5).
    bool ownsModuleKey(uint8_t id) const override { return id == btn::Performance; }
    void padLeds(uint8_t* levels) override;
    // MIDI CC 41-48 / 51-62 (spec 12 §B.2, §B.3); handled at the next update.
    void perfTrackPatternCc(uint8_t track, uint8_t value) override;
    void perfSlotCc(uint8_t slot, uint8_t value) override;
    void perfBackground(uint32_t now) override;
    void perfScreenKey(uint8_t id) override;
    void perfRevertKept() override;
    // FX kept live on another screen (spec 16 §A.15).
    enum class KeepAlive : uint8_t { None, Always, UntilPatternEnd };
    KeepAlive keepAlive() const { return keepAlive_; }

    bool edit = false;
    int column = 0;
    int bank = 0;                   // 2.x: F1..F8 = tracks 1-8 (0) or 9-16 (1)
    // FX enabled per track (session state; every track on at screen start).
    bool fxOn[kTrackCount] = {true, true, true, true, true, true, true, true,
                              true, true, true, true, true, true, true, true};
    perf::Source src;               // active preset source (spec 12 §A.1)
    uint8_t sourceIndex = 0;        // 0 global (config), 1 per project
    int16_t cur[12] = {};           // current value per slot (what is applied)
    int16_t live[12] = {};          // MIDI-CC live value (row 4)
    bool liveValid[12] = {};
    uint8_t shownPattern(int t) const;   // "Patt" label value of track t

private:
    void refreshBar();
    void applyAll(bool revertEverything);
    void revert();
    void leave();
    void resetOthers(int slot);
    void edited();
    void changeType(int d);
    void swapColumn(int nc);
    void play();
    bool anyTrackKey() const;
    void trackPatternNow(int t, int num);
    void trackPatternQueued(int t, int num);
    uint8_t trackPatternOf(int t) const;
    void storeTrackPattern(int t, uint8_t num);
    bool held_[12] = {};
    bool keyHeld_[8] = {};   // per F key
    bool keyUsed_[8] = {};
    uint8_t pending_[kTrackCount] = {};   // per track
    int copySrc_ = -1;
    bool ccPending_[12] = {};
    uint8_t ccVal_[12] = {};
    bool trackCcPending_[8] = {};
    uint8_t trackCcVal_[8] = {};
    uint8_t lastPattern_ = 0;
    uint32_t lastPoll_ = 0, lastLabels_ = 0;
    bool blinkOn_ = true;          // queued-pattern label blink phase
    bool labelsDirty_ = false;
    KeepAlive keepAlive_ = KeepAlive::None;
    uint32_t keepEnds_ = 0;     // App::patternEndCount() when UntilPatternEnd began
};

// ---------------------------------------------------------------------------
// Sample editor (spec 06 §6)
class EditorScreen final : public Screen {
public:
    using Screen::Screen;
    float playheadFrac() const;   // position in the sample 0..1, -1 = nothing plays
    const char* name() const override { return "Sample Editor"; }
    void start(int opt) override;
    void stop() override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onEncoder(int d) override;
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const override;
#endif
    bool encoderFast() const override { return true; }
    bool ownsModuleKey(uint8_t id) const override { return id == btn::SampleEditor; }
    bool onMidiNote(uint8_t note, uint8_t velocity, bool on) override;
    void onPad(uint8_t pad, uint8_t state) override;

    uint16_t startM = 0, endM = 65535;
    int focus = 0;          // page 1: 0 start, 1 end, 2 zoom, 3 effect list (spec 06 §6.3)
    int effect = 0;         // 0..16 (sfx::Effect)
    int page = 1;           // 1 markers/effect list, 2 effect parameters
    int paramPage = 0;      // 4-parameter page on page 2
    int paramFocus = 0;     // parameter index on page 2
    float params[17][9] = {};
    bool undoAvail = false;
    int previewState = 0;   // 0 dirty, 1 processed (spec 06 §6.4 F7)
    // What the processed preview was made from: a different selection or
    // effect makes it dirty again (preview()).
    uint32_t prevS0_ = 0, prevS1_ = 0;
    int prevEffect_ = -1;
    bool previewPlaying = false;
    // Preview playhead (owner request): the selection that plays (the
    // processed output may be longer or shorter), the line drawn last frame.
    uint32_t playS0_ = 0, playSel_ = 0, playOut_ = 0;
    int headX_ = -1;
    uint32_t headAt_ = 0;
    bool inert = false;     // MIDI / empty slot, too long, or waiting for the stop prompt
    WaveZoom view;

    // Run the selected effect on Start..End and commit it (keeps one undo).
    bool apply();
    bool undo();
    bool preview(int note = 60);   // process the selection and play it at `note` (60 = own pitch)

private:
    void enter();
    void refreshBar();
    void markerDelta(int which, int d);
    void paramDelta(int d);
    bool fits(uint32_t newFrames, uint32_t extraSamples);
    void stopPreview();
    void selection(uint32_t* s0, uint32_t* s1) const;
    uint32_t undoFrames_ = 0;
    bool undoStereo_ = false;
    uint8_t slot_ = 0;
    uint8_t lastInstr_ = 0xFF;
    int padPreview_ = -1;   // pad that started the running preview
    int midiNote_ = -1;     // MIDI note that started the running preview (spec 17 §1.3 owner 3)
};

// ---------------------------------------------------------------------------
// Sample recorder (spec 05 §2.11 + appendix C §2)
class RecorderScreen final : public Screen {
public:
    using Screen::Screen;
    int playheadX() const;   // preview playhead x in the edit view, -1 none
    const char* name() const override { return "Sample Recorder"; }
    void start(int opt) override;
    void stop() override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onPad(uint8_t pad, uint8_t state) override;
    bool onMidiNote(uint8_t note, uint8_t velocity, bool on) override;
    void onEncoder(int d) override;
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const override;
#endif
    bool encoderFast() const override { return true; }

    static constexpr int kUndoLevels = 5;   // appendix C §2.11 "depth counter max 5"
    int state = 0;          // 0 setup, 1 edit after recording, 2 naming (appendix C §2.3)
    int sel = 0;            // selected bottom-bar parameter
    uint16_t startM = 0, endM = 65535;
    uint32_t off = 0, len = 0;               // current (cropped) view into the capture buffer
    int undoDepth = 0;
    int meter = 0;          // 0..100 meter units (left / mono)
    int meterR = 0;         // right meter (2.x stereo line in; = meter in mono)
    // Level meter ballistics (appendix C §2.6): read every display frame,
    // instant rise, fall 1 unit per ms down to the current reading.
    static constexpr uint32_t kMeterMs = 16;
    // Left / Right target from `sel` (dir +-1) on page `state` (0 setup, 1 edit).
    static int arrowSel(int sel, int dir, int state, bool radio);
    static int meterFall(int meter, int reading, uint32_t elapsedMs) {
        if (reading >= meter) return reading;
        const int m = meter - int(elapsedMs > 100 ? 100 : elapsedMs);
        return m > reading ? m : reading;
    }
    bool sourceOk = true;
    char folder[32] = {};
    char fileName[40] = {};
    WaveZoom view;
    int playNote = -1;      // note of the running pad / Preview playback, -1 none
    uint32_t playFrames = 0;  // frames of that playback (after transposition)

    bool record();          // start (count-in when the metronome is on)
    void stopRecording();
    bool crop();
    bool undoCrop();
    // Write <folder>/<name>.wav; load = also import into the first free slot.
    bool save(const char* name, bool load, bool overwrite);
    // F4 Metronome opened Config; Config returns with switchTo(kModRecorder, kModConfig).
    bool returnsFromConfig() const { return configReturn_; }

private:
    void enter();
    void play(int note);    // selection at `note` (60 = own pitch), appendix C §2.11
    void playStop();
    void refreshBar();
    void setSource(int s);
    void change(int d);
    void openNaming(const char* initial);
    void selection(uint32_t* s0, uint32_t* s1) const;
    const int16_t* data() const;
    const int16_t* dataR() const;          // right lane (stereo take), else nullptr
    bool stereoInput() const;              // Line In source with Config Line in channel = Stereo
    const char* sourceName(int src) const; // "Line in ST" / "Line in L" / "Line in R" for Line In
    struct View {
        uint32_t off, len;
    } undo_[kUndoLevels] = {};
    uint32_t lastMeterAt_ = 0, clipUntil_ = 0, lastRecDrawAt_ = 0;
    bool clipShown_ = false;
    uint32_t lastRadioCheckAt_ = 0;   // radio source re-route (update)
    uint32_t lastLen_ = 0;
    bool seeking_ = false;
    bool countIn_ = false;
    bool previewing_ = false;
    // Preview playhead (owner request): x of the line drawn in the last frame,
    // the selection start of the playback, the last poll.
    int headX_ = -1;
    uint32_t playS0_ = 0;
    uint32_t headAt_ = 0;
    bool recording_ = false;
    bool configReturn_ = false;   // stock flag +0x71a (appendix C §2.9)
    int playPad_ = -1;            // pad holding the playback, -1 = b0 Preview, -2 = MIDI note
    uint8_t lastMidi_ = 48;       // last MIDI instrument used here (stock +0x709)
};

// [I] Live resampling (app/resample.h, screen_resample.cpp): From / Length /
// Place on, the take's state, Gain and Monitor for Line in takes.
class ResampleScreen final : public Screen {
public:
    using Screen::Screen;
    const char* name() const override { return "Resample"; }
    void start(int opt) override;
    void stop() override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onEncoder(int d) override;
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const override;
#endif
    int sel = 0;   // column: 0 From, 1 Length, 2 Slices, 3 Into, 4 Place on, 5 Gain, 6 Monitor

private:
    bool lineIn() const;
    void route();   // meter / monitor the line input when From = Line in
    void change(int d);
    void refreshBar();
    uint32_t shown_ = 0xFFFFFFFFu;   // take state / bar / playing last shown
    int intoTop_ = 0;                // Into list: first visible row
    int meter_ = 0;
    uint32_t lastMeterAt_ = 0;
};

// ---------------------------------------------------------------------------
// Synth slots (2.x WP-G; Tracker Mini manual p.147-153, docs/MINI_PLAN.md §3
// UI). One screen with four views over slot `slot` (instruments 64..66):
//   Synth Parameters 1/2: F1 Model, F2 Polyphony, F3 Gain, F4 Panning,
//     F5 Reverb Send, F6 Delay Send, F7 Load Patch, F8 Edit Patch;
//   Synth Macros 2/2: F1..F6 macro M1..M6 page values, legend below;
//   Edit Patch: sections (pads / Left, Right), parameters (Up / Down),
//     value (encoder); F1 Init, F2 Save, F3 Save As, F7 section, F8 Back;
//   Load Patch: a popup list of Patches/<MODEL>/ at x 600..799 over the
//     dimmed page 1/2 (Tracker+ 1.0, docs/SYNTH_UX_MINI_VS_OT.md row 24):
//     F5-F6 Preview, F7 Cancel, F8 Confirm; Preview and the pads play the
//     highlighted patch in the slot (App::previewSynthLibraryPatch), Cancel
//     restores the slot's patch byte for byte.
// The Instrument Params key toggles pages 1/2 (and leaves Edit / Load Patch).
// A slot whose model has no engine in this build (FAT until 2.1, WTFM until 2.2) shows
// "Synth k (MODEL) not available" and is read-only except F1 Model.
namespace synthui {
// Value display / step kinds of one engine parameter.
enum Fmt : uint8_t {
    kFmtPct,      // v * 100 "%"
    kFmtVal100,   // v * 100, no unit
    kFmtSec,      // seconds
    kFmtHz,       // log frequency (cutoff)
    kFmtLfoHz,    // log frequency, two decimals
    kFmtEnum,     // names[]
    kFmtOnOff,
    kFmtSemi,     // semitones
    kFmtCents,
    kFmtInt,      // plain integer (index without known labels)
    kFmtMs,
    kFmtDb,
    kFmtNote,     // MIDI note name (PERC pitch)
    kFmtPan,      // -1..1 -> L100..R100
    kFmtPctMax,   // v / max(|min|, |max|) * 100 % (Resonance 0..1.5, Pitch LFO / Mod Amt bipolar)
    kFmtMs10,     // stored 0..100 shown as 0..10 ms (PERC Body / Snap Attack)
    kFmtPercNote, // PERC pitch 12..119 as C0..B8 (manual p.166)
    kFmtBodyWave, // PERC Body Wave 0..175 = 16 waves x 0..100 % (manual p.166)
    kFmtRatio,    // WTFM Ratio index 0..47 shown as 0.25 .. 12.00 (spec 2x-08 §1)
};
struct ParamUi {
    const char* label;
    uint8_t fmt;
    const char* const* names;   // kFmtEnum
    uint8_t nameCount;
};
// A section of the Edit Patch page: parameter indices in display order, or
// macro m (0..6) when macro >= 0.
struct Section {
    const char* name;       // grid cell ("Osc")
    const char* longName;   // F6 label ("Oscillator")
    const uint8_t* params;
    uint8_t count;
    int8_t macro;
};
constexpr int kMaxSections = 16;              // 4 x 4 grid (pads 0-3, 12-15, 24-27, 36-39)
// Macro targets shown (manual p.155: Parameter / Amount 1..5). A 6th target
// that a patch stores stays in the patch, unshown.
constexpr int kMacroShownTargets = 5;
constexpr int kMacroRows = 2 + 2 * kMacroShownTargets;   // Name, Bipolar, Param/Amount x 5
// Parameter descriptor (nullptr: unknown engine / index, or not shown).
const ParamUi* paramUi(int32_t engine, int index);
// Sections of an engine's Edit Patch page (macros M1..M7 last).
int sections(int32_t engine, const Section** out);
// "Osc Saw Mix" (section + label; macro target names).
void paramFullName(int32_t engine, int index, char* out, int cap);
void formatValue(int32_t engine, int index, float v, char* out, int cap);
float valueFrac(int32_t engine, int index, float v);   // 0..1 (bar position)
bool valueBipolar(int32_t engine, int index);
// v moved by d detents (clamped to the range).
float stepValue(int32_t engine, int index, float v, int d);
// Macro target choice: -1 ("Unassigned") then every shown parameter in section order.
int targetChoices(int32_t engine);
int targetChoice(int32_t engine, int k);        // k-th choice -> param (-1 none)
int targetChoiceIndex(int32_t engine, int param);
// Slot record texts.
void gainText(float db, char* out, int cap);    // "-9.00 dB", "-inf dB" at -96
void sendText(float v, char* out, int cap);     // SEND law dB, "-inf dB" at 0
// Pattern Instrument popup order (Tracker+ manual p.100 figure, docs/
// SYNTH_UX_MINI_VS_OT.md row 1): Synth 1..3 first, then the sample
// instruments 1..48 and MIDI 1..16 in the stock order. The stored numbers
// stay 64..66 / 0..63; only the list position differs.
inline int popupInstrument(int pos) {
    return pos < kSynthSlotCount ? kFirstSynthInstrument + pos : pos - kSynthSlotCount;
}
// "Synth 1 (VAP)": a synth slot named as the Tracker+ popup and title bar
// do (manual p.100 figure; docs/SYNTH_UX_MINI_VS_OT.md rows 1, 3).
void slotLabel(const App& a, int slot, char* out, int cap);
inline int popupPosition(int instrument) {
    return isSynthInstrument(uint8_t(instrument)) ? instrument - kFirstSynthInstrument : instrument + kSynthSlotCount;
}
// Instrument popup on the MIDI tracks 9-16 (Mini manual §1.8: "tracks 9-16
// are used for MIDI and synths"; USER_REPORTS_R2 item 5): Synth 1..3 first,
// as on tracks 1-8, then MIDI 1..16 (positions 3..18 = 48..63).
constexpr int kMidiPopupCount = kSynthSlotCount + kMidiInstrumentCount;
inline int popupInstrumentMidi(int pos) {
    return pos < kSynthSlotCount ? kFirstSynthInstrument + pos : kFirstMidiInstrument + pos - kSynthSlotCount;
}
inline int popupPositionMidi(int instrument) {
    if (isSynthInstrument(uint8_t(instrument))) return instrument - kFirstSynthInstrument;
    if (instrument >= kFirstMidiInstrument && instrument < kInstrumentCount)
        return instrument - kFirstMidiInstrument + kSynthSlotCount;
    return kSynthSlotCount;   // a sample instrument: MIDI 1
}
// Instr-column pad on a MIDI track [OT: the Mini's pad rule there is not
// documented]: pads 1-16 = MIDI 1..16 (stock), pads 17-19 = Synth 1..3, the
// other pads keep the stock MIDI wrap.
inline int midiTrackPadInstrument(int pad) {
    const int k = pad - kMidiInstrumentCount;
    return k >= 0 && k < kSynthSlotCount ? kFirstSynthInstrument + k : kFirstMidiInstrument + (pad & 15);
}
// Pad of a MIDI-track instrument (the inverse; -1: none).
inline int midiTrackInstrumentPad(int instrument) {
    if (isSynthInstrument(uint8_t(instrument))) return kMidiInstrumentCount + instrument - kFirstSynthInstrument;
    return -1;
}
}  // namespace synthui

// Instr-hold chooser (Instrument Params, Synth pages): sample instruments
// 1..48, MIDI 1..16, then Synth 1..3 (2.x, Tracker Mini).
constexpr int kInstrumentChooserCount = kInstrumentCount + kSynthSlotCount;
void instrumentChooserItem(void* ctx, int i, char* out, int cap);

class SynthScreen final : public Screen {
public:
    using Screen::Screen;
    const char* name() const override { return "Synth"; }
    void start(int opt) override;   // opt 1..3 = slot (0: keep)
    void stop() override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onPad(uint8_t pad, uint8_t state) override;
    void onEncoder(int d) override;
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const override;
#endif
    bool encoderFast() const override { return true; }
    bool ownsModuleKey(uint8_t id) const override { return id == btn::InstrParams; }
    void padLeds(uint8_t* levels) override;

    // Slot the Instrument Params key opens from module `active` (the Pattern
    // cursor step's synth instrument, else the current instrument), -1 = not
    // a synth (the sample / MIDI Instrument screen opens).
    static int entrySlot(App& a, Ui& ui, int active);

    enum class View : uint8_t { Params, Macros, Edit, Browser };
    View view = View::Params;
    int slot = 0;           // 0..2
    int param = 0;          // Params page: F1..F6 (0 model .. 5 delay send)
    int macro = 0;          // Macros page: M1..M6
    int section = 0;        // Edit: section index
    int row = 0;            // Edit: row in the section
    // Browser (Patches/<MODEL>/*.patch, sorted).
    static constexpr int kBrowserMax = 128;
    static constexpr int kBrowserNameMax = 32;
    int browserCount = 0;
    int browserSel = 0;
    // In browserListMem() (filled by listBrowser).
    char (*browser)[kBrowserNameMax + 1] = static_cast<char (*)[kBrowserNameMax + 1]>(browserListMem());

    int32_t engine() const;
    bool available() const;   // the model has an engine in this build
    int rows() const;         // rows of the current Edit section
    void listBrowser();
    void openBrowser();       // F7 Load Patch: the popup
    void closeBrowser();      // Cancel: the slot's patch as before the popup
    bool previewHighlighted();   // the highlighted patch into the slot (Preview / pads)
    int previewSel() const { return previewSel_; }
    bool loadSelected();      // Confirm
    void initPatch();         // with the confirmation popup
    void savePatch();         // to the record's name (Save As when "Init" / empty)
    void savePatchAs();       // keyboard

private:
    void refreshBar();
    void paramsChange(int d, bool wrap = false);
    void macroChange(int d);
    void editChange(int d);
    void macroRowChange(int m, int r, int d);
    void macroRowText(int m, int r, char* label, int lcap, char* value, int vcap) const;
    void macroNameKeyboard();
    bool nameRow() const;     // Edit Patch on a macro's Name row (F8 = Edit)
    void resetSelected();     // Delete on Params / Macros / Edit Patch
    void resetEditRow();      // Delete in Edit Patch: the Init patch's value
    void applyModel();        // pendingModel_ -> the slot (after "Discard edits?")
    bool loadSelectedNow();
    void saveAsNow();         // pendingName_ (after "Overwrite?")
    // Patch edits since the last load / init / save / model change, per slot
    // ("Discard edits?" before Load or a model change).
    bool edited_[kSynthSlotCount] = {};
    int pendingModel_ = -1;
    char pendingName_[kBrowserNameMax + 1] = {};
    void edited();
    void drawParams(Gfx& g);
    void drawMacros(Gfx& g);
    void drawEdit(Gfx& g);
    void drawBrowser(Gfx& g);
    void title(Gfx& g, const char* text);
    void audition(bool on, uint8_t note);
    int padNote_ = -1;      // pad holding the audition, -1 none
    int previewSel_ = -1;   // Load Patch: browser entry playing in the slot, -1 none
    int previewKey_ = -1;   // Load Patch: F5 / F6 holding the Preview note
    int browserTop_ = 0;
    int listTop_ = 0;
};

static_assert(LoaderScreen::kMaxEntries * (40 + sizeof(bool)) <= kBrowserListBytes, "Loader list");
static_assert(SynthScreen::kBrowserMax * (SynthScreen::kBrowserNameMax + 1) <= kBrowserListBytes, "Synth list");
static_assert(ProjectBrowser::kMaxEntries * 40 <= kBrowserListBytes, "File list");
static_assert(ConfigScreen::kFwMax * 48 <= kBrowserListBytes, "Firmware list");

}  // namespace ot
