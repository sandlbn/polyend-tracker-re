// UI framework: screen manager, input dispatch, bottom bar, popups,
// on-screen keyboard and the pad-grid value picker (spec 05 §1.5, §2.2,
// §5.2, §5.3; spec 06 §0.3).
//
// Rendering is change-driven: anything that changes what is shown calls
// Ui::dirty(); the main loop renders a full FT81x display list when dirty
// (rate-limited). Static blocks (title strip, bottom bar, Pattern grid parts)
// go through Gfx::cached(): RAM_G fragments appended with CMD_APPEND like the
// stock per-control lists (spec 05 §3.3), keyed by the hash of their output.
#pragma once

#include <cstdint>

#include "hal_api.h"
#include "ui/buttons.h"
#include "ui/gfx.h"
#include "ui/screen_factory.h"
#include "ui/screen_usb_storage.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ot {

class App;
class Ui;

// Module indices (spec 05 §1.5 module table).
enum Module : int8_t {
    kModFile = 0,
    kModLoader = 1,
    kModPattern = 2,
    kModPlayback = 3,
    kModSong = 4,
    kModInstrument = 5,
    kModEditor = 6,
    kModConfig = 7,
    kModRecorder = 8,
    kModPerformance = 9,
    kModSynth = 10,          // 2.x synth slots (Tracker Mini; Instrument Params on S1..S3)
    kModGames = 11,          // spec 14 §1.1 (no module key; File F7)
    kModMaster = 12,
    kModResample = 13,       // [I] live resampling (no module key; Sample Recorder prompt)
    kModCount = 14,
    kModNone = -1,
};

// Progress popup titles of the storage states (spec 05 §1.3 progress popup;
// stock title table, docs/STRINGS_DIFF_1.9.3.md item 6).
constexpr const char* kLoadingFiles = "Loading files list";
constexpr const char* kLoadingProjects = "Loading projects list";
constexpr const char* kLoadingFirmwares = "Loading firmwares list";
constexpr const char* kExportingSong = "Exporting Song";
constexpr const char* kExportingSongStems = "Exporting Song Stems";
constexpr const char* kExportingPattern = "Exporting Pattern";
constexpr const char* kExportingPatternStems = "Exporting Pattern Stems";
constexpr const char* kExportingSelection = "Exporting Selection";

// Module for a module key, or kModNone (spec 05 §1.5 button -> module map).
int moduleForButton(uint8_t id);

class Screen {
public:
    Screen(Ui& ui, App& app) : ui_(ui), app_(app) {}
    virtual ~Screen() = default;
    virtual const char* name() const = 0;
    virtual void start(int opt) { (void)opt; }
    virtual void stop() {}
    virtual void update(uint32_t now) { (void)now; }
    virtual void draw(Gfx& g) = 0;
    virtual void onButton(uint8_t id, uint8_t state);   // default: Play toggles transport
    virtual void onPad(uint8_t pad, uint8_t state);      // default: audition current instrument
    virtual void onEncoder(int d) { (void)d; }
    virtual bool encoderFast() const { return false; }   // Shift-fast acceleration (spec 05 §2.1.4)
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)   // simulator / host only
    // True while the encoder moves a highlight through a list (names,
    // options, rows): clockwise = down the list. False on values, where
    // clockwise = more. Not used by the firmware; the simulator reads it so
    // its mouse wheel scrolls lists with the wheel.
    virtual bool encoderOnList() const { return false; }
#endif
    // True if this screen gives its own module key a local meaning (spec 06 §0.3).
    virtual bool ownsModuleKey(uint8_t id) const { (void)id; return false; }
    // Base pad LED layer, levels 0..31.
    virtual void padLeds(uint8_t* levels) { (void)levels; }
    // True while the screen draws the whole 800 x 480 frame itself (Games,
    // spec 14 §3.2): the bottom bar is not drawn over it.
    virtual bool fullScreen() const { return false; }
    // MIDI note input owned by the screen's own preview player (spec 17 §1.3
    // owners 2..4: Sample Playback, Sample Editor, Sample Recorder). Returns
    // true when the note was taken; false = the normal track-voice path.
    virtual bool onMidiNote(uint8_t note, uint8_t velocity, bool on) {
        (void)note; (void)velocity; (void)on;
        return false;
    }

protected:
    Ui& ui_;
    App& app_;
};

// One bottom label (spec 05 §2.2, spec 20 §1.2): text only on the strip;
// `highlight` draws the red focus frame, `fill` the solid red box of an
// active toggle (Record armed).
struct BarLabel {
    char top[24] = {};
    char bottom[24] = {};
    bool enabled = true;
    bool highlight = false;
    bool wide = false;      // spans this and the next button
    bool hidden = false;
    bool fill = false;
    bool name = false;      // two-line name (set +0x44), see BottomBar::setName
    // wg::Underline: none / green (available) / red (edit-mode only). A state,
    // independent of `enabled` (Part 2 C7: Rec mode = enabled + red line).
    uint8_t underline = 0;
    uint8_t pages = 0;      // > 1: page dots ("More")
    uint8_t page = 0;
    uint32_t dotOff = wg::kThemeColour;   // other page dots (Instrument Params: 0x323132)
};

class BottomBar {
public:
    BarLabel label[8];
    // Strip separators at x = 100 i for bits 1..7 (stock 0xff; Song 0xbf).
    // A wide label also hides the separator in its middle.
    uint8_t separators = 0xFE;
    // Strip fill; wg::kThemeColour = slot kStrip+1 (0a0a0a). Stock Config and
    // Instrument Parameters show a half-dark 050505 strip [S snap 004, 011].
    uint32_t stripBg = wg::kThemeColour;
    // Named strip variant (Part 2 C5), used while stripBg is kThemeColour:
    // kNormal 0a0a0a / separators 464646, kDark 050505 / 434343 (Instrument
    // Params, Playback, Fill popup, Games, Config pickers). Prefer this over
    // a literal stripBg: it follows the theme.
    wg::StripStyle stripStyle = wg::StripStyle::kNormal;
    // Class-0x20 label strip (keyboard): single-line labels top-aligned
    // (caps 437..447) instead of centred on 451.
    bool labelStrip = false;
    void setStripStyle(wg::StripStyle s) { stripStyle = s; }
    void clear();
    void set(int i, const char* top, const char* bottom = nullptr, bool enabled = true);
    void setWide(int i, const char* top, const char* bottom = nullptr, bool enabled = true);
    void setBottom(int i, const char* bottom);
    // A name on two lines ("Export" / "Song"): both lines grey, font 3.
    void setName(int i, const char* line1, const char* line2, bool enabled = true);
    void enable(int i, bool on) { if (i >= 0 && i < 8) label[i].enabled = on; }
    void highlight(int i, bool on) { if (i >= 0 && i < 8) label[i].highlight = on; }
    void highlightOnly(int i);
    void fill(int i, bool on) { if (i >= 0 && i < 8) label[i].fill = on; }
    void underline(int i, uint8_t kind) { if (i >= 0 && i < 8) label[i].underline = kind; }
    void pages(int i, int count, int current) {
        if (i < 0 || i >= 8) return;
        label[i].pages = uint8_t(count < 0 ? 0 : count);
        label[i].page = uint8_t(current < 0 ? 0 : current);
    }
    // Separator mask actually drawn (wide labels removed).
    uint8_t separatorMask() const;
    // Strip (fill + separators), drawn under the screen; labels on top.
    void drawStrip(Gfx& g, const Theme& th) const;
    // Fill colour drawStrip uses (stripBg, else the stripStyle variant).
    uint32_t stripColour(const Theme& th) const;
    void drawLabels(Gfx& g, const Theme& th) const;
    void draw(Gfx& g, const Theme& th) const {
        drawStrip(g, th);
        drawLabels(g, th);
    }
};

// Value picker shown as a 12 x 4 grid mirroring the pads (spec 05 §5.3
// "Value popups", spec 06 §1.6).
class Picker {
public:
    using ItemFn = void (*)(void* ctx, int index, char* out, int cap);
    using DescFn = const char* (*)(void* ctx, int index);
    void open(int kind, const char* title, int count, int cursor, ItemFn fn, void* ctx);
    void close() { open_ = false; }
    // Description window for the highlighted item (FX picker F7/F8,
    // manual); set after open().
    void setDescription(DescFn fn) { descFn_ = fn; }
    // F7/F8 toggle; the on/off state persists across opens (spec 16 §B.4).
    void toggleDescription() {
        if (descFn_) descOpen_ = !descOpen_;
    }
    bool descriptionOpen() const { return open_ && descFn_ && descOpen_; }
    bool isOpen() const { return open_; }
    int kind() const { return kind_; }
    int cursor() const { return cursor_; }
    void move(int d);
    void pick(int pad);   // pad index on the current page
    void select(int index) { if (index >= 0 && index < count_) cursor_ = index; }   // any item
    void draw(Gfx& g, const Theme& th) const;
    void leds(uint8_t* levels, uint8_t dim, uint8_t full) const;

private:
    bool open_ = false;
    int kind_ = 0;
    int count_ = 0;
    int cursor_ = 0;
    char title_[32] = {};
    ItemFn fn_ = nullptr;
    void* ctx_ = nullptr;
    DescFn descFn_ = nullptr;
    bool descOpen_ = false;
};

// On-screen keyboard (spec 05 §5.2): 4 x 12 keys mapped 1:1 onto the pads.
class Keyboard {
public:
    using DoneFn = void (*)(void* ctx, bool ok, const char* text);
    void open(const char* title, const char* initial, int maxLen, const char* okLabel, DoneFn fn, void* ctx);
    // Second confirm key (call after open): F6 = Cancel, F7 = altLabel, F8 =
    // ok. alt() tells which one finished (Sample Recorder naming,
    // spec 05 appendix C §2.3 state 2).
    void setAlt(const char* altLabel);
    bool alt() const { return alt_; }
    void close() { open_ = false; }
    bool isOpen() const { return open_; }
    const char* text() const { return text_; }
    // Returns true if the event was consumed.
    bool onButton(uint8_t id, uint8_t state);
    void onPad(uint8_t pad, uint8_t state);
    void onEncoder(int d);
    void draw(Gfx& g, const Theme& th) const;
    // keepTitle: the screen's title bar (status group, right text) is
    // already drawn and stays; only its title text is replaced (stock 020:
    // "Edit Pattern 2 Name" + status + FX name).
    void draw(Gfx& g, const Theme& th, bool keepTitle) const;
    void leds(uint8_t* levels, uint8_t dim, uint8_t full) const;
    static void autoName(char* out, int cap, uint32_t seed);

    int cursor() const { return cursor_; }          // text cursor (insert position)
    bool nameFocused() const { return nameFocus_; }  // the name row is highlighted
    int highlighted() const { return hl_; }
    bool allSelected() const { return selAll_; }

private:
    void typeKey(int k);
    void typeChar(char c);
    void clearSelection();
    void backspace();
    void moveKey(int dir);
    bool autoNameAllowed() const;
    void finish(bool ok);

    bool open_ = false;
    bool caps_ = false;
    bool nameFocus_ = false;
    int maxLen_ = 31;
    int hl_ = 0;
    int cursor_ = 0;
    bool selAll_ = false;    // the whole name is selected (just opened)
    char title_[40] = {};
    char text_[40] = {};
    char okLabel_[20] = {};   // '\n' splits a two-line label
    char altLabel_[20] = {};
    bool alt_ = false;
    DoneFn fn_ = nullptr;
    void* ctx_ = nullptr;
    uint32_t seed_ = 1;
};

// /.develop overlay numbers (spec 06 §7.2). OpenTracker shows its existing
// telemetry: audio render load (permille of the block budget) and SDRAM left
// over by the memory layout / the longest main-loop pass.
struct DevStats {
    uint32_t cpuMin = 0, cpuMax = 0, cpuAvg = 0, cpuLast = 0;   // permille
    uint32_t memFree = 0;                                       // bytes
    uint32_t loopMaxUs = 0;
};

// Headphone-amp boot level (volume(v), spec 01 §5.1; the stock value is not
// in the specs): 0.715 = HP_CTRL 0x24 = -6.0 dB, +1 dB over the first
// builds' 0.70 after a "stock is louder" report (USER_REPORTS_R2 §2).
constexpr float kHeadphoneBootLevel = 0.715f;

// Optional platform extras injected by the device glue (null on host / sim).
struct UiPlatform {
    bool (*devStats)(DevStats& out) = nullptr;
    void (*devStatsReset)() = nullptr;   // Config Play resets the CPU statistics
    // Config /.develop "HP level" (USER_REPORTS_R2 §2): device sets the codec
    // headphone amp (volume(v), spec 01 §5.1) and logs it to diag.
    void (*setHeadphoneLevel)(float v) = nullptr;
    // Power-on checks for the factory test's first page; returns the count.
    int (*bootChecks)(FactoryCheck* out, int cap) = nullptr;
    // First frame per screen that did not fit the FT81x RAM_DL (the guard
    // in Ui::render dropped `dropped` primitives at `words`); device: diag
    // log, host tests: a hard failure (tests/ui_fakes.h).
    void (*dlOverflow)(const char* screen, uint32_t words, uint32_t dropped, bool lowDetail) = nullptr;
    // Every Ui::notify() text (host checks: the sim's expect-open-ok and the
    // test-song host test fail on load notices). Null on the device.
    void (*notified)(const char* text) = nullptr;
};

class Ui {
public:
    using AskFn = void (*)(void* ctx, int key);   // key = F-index 5..7, or -1 (cancelled)
    // Popup header owned by the open popup (VISUAL_DIFF Part 2 §7.2).
    enum class PopupHeader : uint8_t {
        kNone,
        kRight,   // Instruments / Fx popups: header x 600..799, F7 / F8 labels hidden
        kFull,    // Notes popup: full-width header, no bottom bar, no hairline
        kRightBar,  // Synth Load Patch (Tracker+ 1.0): the kRight header, all bottom labels stay
    };

    explicit Ui(App& app);
    ~Ui();
    void attach(IDisplay* display, ILeds* leds) {
        display_ = display;
        leds_ = leds;
    }
    void start();                                // boot into the Pattern editor
    void handle(const InputEvent& ev, uint32_t now);
    void update(uint32_t now);                   // screen update, timers, pad LEDs
    bool renderIfDirty(uint32_t now);
    void render();
    void dirty();   // marks the frame dirty; the first call after a frame timestamps it (telemetry)

    void switchTo(int module, int opt = 0);
    int active() const { return active_; }
    int previous() const { return previous_; }
#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
    bool encoderOnList() const;   // overlays first, then the active screen (Screen::encoderOnList)
#endif
    Screen* screen(int module) const { return module >= 0 && module < kModCount ? screens_[module] : nullptr; }
    Screen* current() const { return screen(active_); }

    bool isDown(uint8_t id) const { return id < 64 && ((down_ >> id) & 1); }
    bool shift() const { return isDown(btn::Shift); }
    bool padDown(int p) const { return p >= 0 && p < 48 && ((pads_ >> p) & 1); }
    uint32_t now() const { return now_; }
    uint32_t downTime(uint8_t id) const { return id < 64 ? downAt_[id] : 0; }

    // Popups (spec 05 §5.3).
    void notify(const char* text, uint32_t ms = 2000);
    const char* notice() const { return note_; }   // the current notification text (tests)
    void ask(const char* title, const char* text, const char* l5, const char* l6, const char* l7, AskFn fn,
             void* ctx);
    bool askOpen() const { return askOpen_; }
    // [I] Live resampling: arm / stop / cancel with a notice (Shift + Sample
    // Recorder anywhere, Record on the Resample screen).
    void resampleKey();
    void closeAsk() { askOpen_ = false; dirty(); }
    // Blocking-operation progress: draws a frame immediately.
    void progress(const char* title, int percent);
    void progressDone() { progressOpen_ = false; dirty(); }
    // Background-operation progress (spec 05 §1.3 progress popup): updates
    // the popup without drawing; the main loop redraws it.
    void setProgress(const char* title, int percent);
    bool progressOpen() const { return progressOpen_; }
    // Title of the open (or last closed) progress popup (host tests).
    const char* progressTitle() const { return progTitle_; }

    // Stock title bar (spec 20 §1.2): bold title, status group (play time,
    // transport glyphs, P pattern.step; hidden on the Sample Editor as
    // stock) and `right` right-aligned at x 790. The hairline under it is
    // drawn after the screen (render()).
    // True when this frame drew the bottom strip under the screen (not while
    // the keyboard is open); `colour` = its fill. A screen that paints over
    // the strip's top rows (soft edge row kStripY - 1 at alpha 230, fill from
    // kStripY) can put them back (Mixer: one fade across its columns).
    bool stripUnder(uint32_t& colour) const {
        colour = stripUnderC_;
        return stripUnder_;
    }
    void drawTitle(Gfx& g, const char* title, const char* right = nullptr);
    void drawTitle(Gfx& g, const char* title, const char* right, bool status);
    const Theme& th() const;

    // Popup headers (VISUAL_DIFF Part 2 §7.2). While one is open drawTitle
    // omits the status group, and render() draws the white header band
    // (wg::rightPopupHeader / wg::fullPopupHeader) over the title bar after
    // the hairline. kRight also hides bottom labels F7 / F8 and fills the bar
    // under x 601..799 with the panel colour; the screen draws its popup
    // panel (wg::rightPopupPanel) and list at (600, 30). kFull draws no bottom
    // bar and no hairline. Closed by closePopupHeader() and on every screen
    // switch.
    void openPopupHeader(const char* title, PopupHeader kind = PopupHeader::kRight);
    void closePopupHeader();
    PopupHeader popupHeader() const { return popupKind_; }
    // Status group (time, transport, position) on / off in drawTitle,
    // independent of the popup header; reset on a screen switch.
    void hideStatus(bool hidden) { statusHidden_ = hidden; dirty(); }
    bool statusHidden() const { return statusHidden_ || popupKind_ != PopupHeader::kNone; }
    // 2.x CPU cell state as last polled (host tests).
    const wg::CpuCell& cpuCell() const { return cpuCell_; }
    // Title hairline colour for the current screen (kThemeColour = theme
    // slot): Monochrome Config draws it in ffffff (034: rows 27 / 28
    // bfbfc0 / dfdfe0). Reset on a screen switch.
    void setTitleHairline(uint32_t colour) { hairline_ = colour; dirty(); }
    // Step shown in the status position "P10.014" while the transport is
    // stopped: the screen's cursor step, 0-based (Pattern: stock shows the
    // cursor row, s02 = 13 rows down -> P10.014); -1 = the sequencer step.
    // While playing the sequencer step is always shown. Reset on a screen
    // switch; set it before drawTitle (e.g. every draw).
    void setStatusCursorStep(int step0) { cursorStep_ = step0; }
    int statusCursorStep() const { return cursorStep_; }

    // spec 05 §1.5 state 5: "shutting down" popup from the power-button
    // press until its release (the board powers off after a 1.5 s hold).
    bool powerPopupOpen() const { return powerOpen_; }
    // Power-off hold (spec 05 §1.5 state 5): the board powers off after
    // 1500 ms (0x14984); the "Shutting down" popup shows the remaining part
    // of the hold as a progress bar, 100..0 % (the bar shrinks as on stock),
    // redrawn every >= 30 ms.
    static constexpr uint32_t kPowerHoldMs = 1500;
    static constexpr uint32_t kPowerRedrawMs = 30;
    static int powerPercent(uint32_t elapsedMs) {
        return elapsedMs >= kPowerHoldMs ? 0 : 100 - int(elapsedMs * 100u / kPowerHoldMs);
    }
    int powerPopupPercent() const { return powerOpen_ ? powerPercent(now_ - powerAt_) : 0; }
    // `/.develop` existed on the card at boot (spec 06 §7.2).
    bool developMode() const { return develop_; }
    // Factory test (interface event 11, spec 05 §2.12).
    void startFactoryTest();
    void closeFactoryTest();   // back to Config
    bool factoryTestActive() const { return factory.active(); }
    // USB Storage Mode page (interface event 10, spec 01 §6.1): shown over
    // the active screen until Exit. 2.x: on = App::enterUsbStorage() (the
    // card goes to the computer; refused with a notice when busy), off = Exit
    // (App::exitUsbStorage(): card back, Workspace reloaded).
    void setUsbStorageMode(bool on);
    bool usbStorageMode() const { return usb.active(); }

    BottomBar bar;
    Keyboard kb;
    Picker picker;
    App& app;
    UiPlatform platform;
    FactoryTest factory;
    UsbStorageMode usb;

    // Last frame statistics (host tests).
    uint32_t frames() const { return frames_; }
    // Render timing for the diag telemetry (field report: screen blurs while
    // playing; 1.9.5: sluggish refresh while playing). Per report window:
    // frames drawn, render time sum / max (us; the SPI upload included),
    // words of the last frame, frames deferred by a busy display, the
    // longest gap between two consecutive frames (ms), the longest wait from
    // the first dirty() after a frame to the start of the frame that showed
    // it (us), and the longest wait from an input event (its capture time)
    // to the start of the frame after its handling (ms).
    // RAM_DL budget (docs/VERIFY_1.9.5.md §1, §6): wordsMax = the largest
    // frame of the window, overBudget = frames the guard had to cut (they
    // would have faulted the coprocessor), lowDetail = frames drawn in the
    // low-detail fallback.
    struct RenderStats {
        uint32_t frames, usSum, usMax, words, deferred, gapMaxMs, dirtyLatMaxUs, inputLatMaxMs;
        uint32_t wordsMax, overBudget, lowDetail;
    };
    RenderStats takeRenderStats() {
        RenderStats r{frames_ - statFrames0_, renderUsSum_, renderUsMax_, lastWords_, deferred_ - statDeferred0_,
                      gapMaxMs_, dirtyLatMaxUs_, inputLatMaxMs_, maxWords_, overBudget_ - statOver0_,
                      lowDetailFrames_ - statLow0_};
        statFrames0_ = frames_;
        statDeferred0_ = deferred_;
        statOver0_ = overBudget_;
        statLow0_ = lowDetailFrames_;
        renderUsSum_ = renderUsMax_ = gapMaxMs_ = dirtyLatMaxUs_ = inputLatMaxMs_ = maxWords_ = 0;
        return r;
    }
    // FT81x RAM_DL: 2048 words, of which Ft81xDisplay::fontSetup takes 36
    // per frame (6 handles x BITMAP_HANDLE/SOURCE/LAYOUT_H/LAYOUT/SIZE_H/SIZE).
    static constexpr uint32_t kDlLimitWords = 2048;
    static constexpr uint32_t kDriverDlWords = 36;
    static constexpr uint32_t kDlFrameWords = kDlLimitWords - kDriverDlWords;
    // Design target for any screen + overlay (tests/display_budget_test.cpp).
    static constexpr uint32_t kDlTargetWords = 1900;
    // Frame limit of the guard (host tests lower it to force the fallback).
    void setDlFrameLimit(uint32_t words) { dlFrameLimit_ = words; }
    uint32_t overBudgetFrames() const { return overBudget_; }
    uint32_t hwFaultFrames() const { return hwFaultFrames_; }
    uint8_t dlScale() const { return dlScale_; }   // 64 = 1.0
    uint32_t lowDetailFrames() const { return lowDetailFrames_; }
    bool lowDetail() const { return lowDetail_; }
    // Display-list words of the last frame including appended fragments
    // (RAM_DL budget), and the words uploaded through the command FIFO.
    uint32_t lastDlWords() const { return lastWords_; }
    uint32_t lastUploadWords() const { return lastUpload_; }
    // Frames skipped because the display was still busy (non-blocking swap).
    uint32_t framesDeferred() const { return deferred_; }
    // Cached display-list fragments (gfx.h); `fragments.enabled = false`
    // draws everything inline (A/B checks).
    FragmentCache fragments;

private:
    void dispatchButton(uint8_t id, uint8_t state);
    uint8_t resampleShown_ = 0;   // [I] resample state last noticed (Ui::update)
    void updateLeds();
    // Module key whose entering press is held back for the new screen
    // (Master / Song hold, see dispatchButton).
    static bool routesEnteringKey(uint8_t id);
    void deliverEnteringKey(bool asHold = false);
    uint8_t overlayBits() const;
    void finishFrame(Gfx& g, const char* screenName);

    IDisplay* display_ = nullptr;
    ILeds* leds_ = nullptr;
    Screen* screens_[kModCount] = {};
    void* screenOwner_ = nullptr;
    int active_ = kModNone;
    int previous_ = kModPattern;
    uint32_t gen_ = 1;                    // bumps on every screen switch
    uint32_t pressGen_[64] = {};          // screen generation that received the press
    uint8_t enterKey_ = 0;                // held module key that entered the screen (0 = none)
    uint32_t enterGen_ = 0;
    bool songHandedEarly_ = false;   // Song's entering press handed over before its hold tick
    bool songTemporary_ = false;          // Song entered by a hold: release goes back
    uint64_t down_ = 0;
    uint64_t pads_ = 0;
    uint64_t holdSent_ = 0;
    uint32_t downAt_[64] = {};
    uint32_t now_ = 0;
    uint32_t lastRender_ = 0;
    bool dirty_ = true;
    bool stripUnder_ = false;             // this frame drew the bottom strip under the screen
    uint32_t stripUnderC_ = 0;            // its fill colour
    bool titleDrawn_ = false;             // this frame drew a title bar (hairline pending)
    // Last drawTitle() arguments of the current screen (gen), redrawn under
    // the on-screen keyboard instead of the whole screen.
    char lastTitle_[48] = {};
    char lastRight_[48] = {};
    bool lastHasRight_ = false, lastStatus_ = true, lastTitleValid_ = false;
    uint32_t lastTitleGen_ = 0;
    uint32_t frames_ = 0, lastWords_ = 0, lastUpload_ = 0, deferred_ = 0;
    bool readyRetry_ = false;             // frameReady() was false: next poll at readyRetryAt_
    uint32_t readyRetryAt_ = 0;
    // Device RAM_DL feedback (render): faults seen, the measured size last
    // used, the estimate of the frame before the last, and the guard scale
    // (64 = the estimate is right, up to 128 = the device needs twice as
    // many words; it only grows).
    uint32_t hwFaultsSeen_ = 0, hwFaultFrames_ = 0, hwMeasSeen_ = 0, prevWords_ = 0;
    uint32_t scaleGen_ = 0, frameGen_ = 0;   // screen the scale / last frame belong to
    uint8_t dlScale_ = 64;                   // per screen: reset when it changes
    uint32_t maxWords_ = 0, overBudget_ = 0, lowDetailFrames_ = 0, statOver0_ = 0, statLow0_ = 0;
    uint32_t overLogged_ = 0;             // modules whose first overflow was logged
    uint32_t dlFrameLimit_ = kDlFrameWords;
    bool lowDetail_ = false;              // RAM_DL fallback for this screen + overlay set
    uint32_t lowDetailGen_ = 0;
    uint8_t lowDetailOverlays_ = 0;
    uint32_t statFrames0_ = 0, statDeferred0_ = 0, renderUsSum_ = 0, renderUsMax_ = 0;
    uint32_t dirtyAtUs_ = 0;              // first dirty() since the last frame (micros)
    uint32_t lastFrameAtMs_ = 0;          // start of the last frame (ms), 0 = none yet
    uint32_t inputAtMs_ = 0;              // oldest unshown input event (capture time)
    bool inputPending_ = false;
    uint32_t gapMaxMs_ = 0, dirtyLatMaxUs_ = 0, inputLatMaxMs_ = 0;

    bool powerOpen_ = false;
    uint32_t powerAt_ = 0;       // power press time
    uint32_t powerDrawn_ = 0;    // last popup redraw
    bool develop_ = false;

    char note_[64] = {};
    uint32_t noteUntil_ = 0;

    bool askOpen_ = false;
    char askTitle_[40] = {};
    char askText_[64] = {};
    char askLabel_[3][16] = {};
    AskFn askFn_ = nullptr;
    void* askCtx_ = nullptr;

    bool progressOpen_ = false;
    char progTitle_[40] = {};
    int progPct_ = 0;
    uint32_t bootDrawnAt_ = 0;            // boot splash frames (no screen yet)
    bool bootDrawn_ = false;

    PopupHeader popupKind_ = PopupHeader::kNone;
    char popupTitle_[32] = {};
    bool statusHidden_ = false;
    // 2.x CPU cell (spec 2x-02 §2.2): polled at 5 Hz in update(), the frame
    // is marked dirty only when the shown state changes.
    static constexpr uint32_t kCpuPollMs = 200;
    wg::CpuCell cpuCell_;
    uint32_t cpuPollAt_ = 0;
    void pollCpuCell(uint32_t now);
    uint32_t hairline_ = wg::kThemeColour;
    int cursorStep_ = -1;

    uint8_t ledLast_[48] = {};
    bool ledInit_ = false;
};

}  // namespace ot
