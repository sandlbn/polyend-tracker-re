// Config (spec 06 §7; option list and labels spec 15 §2, supersedes 05 §5.4).
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "app/project_io.h"
#include "app/project_fields.h"
#include "ui/config_help.h"
#include "ui/notes.h"
#include "ui/screens.h"
#include "ui/theme_file.h"
#include "sdram_bss.h"

#ifndef OT_VERSION
#define OT_VERSION "dev"
#endif
#ifndef OT_BUILD
#define OT_BUILD __DATE__
#endif

namespace ot {

namespace {
enum Kind : uint8_t { kNum = 2, kEnum = 3, kText = 5, kAction = 7 };
// kUsb: the live USB Storage mode state (2.x), never stored anywhere.
enum Store : uint8_t { kCfg = 0, kProj = 1, kNone = 2, kUsb = 3 };

struct Opt {
    const char* name;
    uint8_t kind;
    uint8_t store;
    uint16_t off;
    int16_t min, max;          // numeric range, or enum count in max
    const char* const* labels;
    int16_t displayOffset;
};

const char* const kOffOn[] = {"Off", "On"};
const char* const kLowMedHigh[] = {"Low", "Medium", "High"};
const char* const kThemes[] = {"Original", "Monochrome", "Anniversary"};
const char* const kFonts[] = {"Original", "New"};
const char* const kRegions[] = {"Europe", "US", "Australia", "Japan"};
const char* const kPreview[] = {"Off", "Track", "Pattern"};
const char* const kRecOpts[] = {"Only notes", "Microtiming", "Velocity", "Microtiming + Velocity"};
const char* const kNumbering[] = {"Decimal, Steps from 0", "Decimal, Steps from 1", "Hexadecimal"};
const char* const kArrange[] = {"Vertical Revolver", "Vertical Original", "Horizontal"};
const char* const kTopInfo[] = {"Additional info", "Track names", "Off"};
const char* const kLineIn[] = {"Stereo", "Mono Left", "Mono Right"};
const char* const kPresets[] = {"Global", "Per Project"};
const char* const kCpuMeter[] = {"Auto", "Always", "Off"};   // [I] 2.x, spec 2x-02 §2.2
const char* const kExportRates[] = {"44117 Hz", "44.1 kHz"};  // [I] 2.x: device rate (stock), converted
// [I] 2.x SD streaming (docs/PLAN_SD_STREAMING_P1C_P2.md §2.7); kStreamThresholdSec.
const char* const kStreamModes[] = {"Off", "Auto", "All"};
const char* const kStreamThresholds[] = {"5 s", "10 s", "20 s", "30 s"};
const char* const kClockIn[] = {"Internal", "USB", "MIDI In jack"};
const char* const kPortsOut[] = {"Off", "USB", "MIDI Out jack", "USB + MIDI Out jack"};
const char* const kPortsIn[] = {"Off", "USB", "MIDI In jack", "USB + MIDI In jack"};
const char* const kChannels[] = {"ALL", "CH1", "CH2", "CH3", "CH4", "CH5", "CH6", "CH7", "CH8", "CH9",
                                 "CH10", "CH11", "CH12", "CH13", "CH14", "CH15", "CH16"};
const char* const kClockSync[] = {"-6", "-5", "-4", "-3", "-2", "-1", "0", "1", "2", "3", "4", "5", "6"};
const char* const kMiddleC[] = {"C-3", "C-4", "C-5", "C-6"};
const char* const kOneTo12[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"};
const char* const kLimiter[] = {"Subtle", "Extreme"};
const char* const kSends[] = {"Pre-fader", "Post-fader"};
const char* const kRoots[] = {"C1", "C#1", "D1", "D#1", "E1", "F1", "F#1", "G1", "G#1", "A1", "A#1", "B1",
                              "C2", "C#2", "D2", "D#2", "E2", "F2", "F#2", "G2", "G#2", "A2", "A#2", "B2",
                              "C3", "C#3", "D3", "D#3", "E3", "F3", "F#3", "G3", "G#3", "A3", "A#3", "B3", "C4"};

#define CF(f) uint16_t(offsetof(Config, f))
const Opt kProject[] = {
    {"Performance presets", kEnum, kCfg, CF(perfPresets), 0, 2, kPresets, 0},
    {"Pattern divider", kNum, kProj, uint16_t(pf::kPatternDivider), 0, 15, nullptr, 1},   // shown 1..16
    {"Notes scale", kEnum, kProj, uint16_t(pf::kScale), 0, kScaleCount, nullptr, 0},
    {"Root note", kEnum, kProj, uint16_t(pf::kRootNote), 0, 37, kRoots, 0},
    {"Notes layout", kEnum, kProj, uint16_t(pf::kNotesLayout), 0, 12, kOneTo12, 0},
    {"Anti-aliasing", kEnum, kProj, uint16_t(pf::kAntiAlias), 0, 2, kOffOn, 0},
    {"Limiter mode", kEnum, kProj, uint16_t(pf::kLimiterMode), 0, 2, kLimiter, 0},
    {"Sends mode", kEnum, kProj, uint16_t(pf::kSendsMode), 0, 2, kSends, 0},
    {"Extra headroom", kEnum, kProj, uint16_t(pf::kExtraHeadroom), 0, 2, kOffOn, 0},
    {"Snap to zero", kEnum, kProj, uint16_t(pf::kSnapToZero), 0, 2, kOffOn, 0},
};
const Opt kGeneral[] = {
    {"Radio region", kEnum, kCfg, CF(radioRegion), 0, 4, kRegions, 0},
    {"Display brightness", kEnum, kCfg, CF(brightness), 0, 3, kLowMedHigh, 0},
    {"Display theme", kEnum, kCfg, CF(theme), 0, 3, kThemes, 0},
    {"Display font", kEnum, kCfg, CF(font), 0, 2, kFonts, 0},
    {"Export theme", kAction, kNone, 4, 0, 0, nullptr, 0},   // [I] 2.x theme files
    {"Pads brightness", kEnum, kCfg, CF(padsBrightness), 0, 3, kLowMedHigh, 0},
    {"Pads preview", kEnum, kCfg, CF(padsPreview), 0, 3, kPreview, 0},
    {"Recording options", kEnum, kCfg, CF(recordingOptions), 0, 4, kRecOpts, 0},
    {"Numbering mode", kEnum, kCfg, CF(numbering), 0, 3, kNumbering, 0},
    {"Pattern arrangement", kEnum, kCfg, CF(arrangement), 0, 3, kArrange, 0},
    {"Pattern top info", kEnum, kCfg, CF(patternTopInfo), 0, 3, kTopInfo, 0},
    {"Line in channel", kEnum, kCfg, CF(lineInChannel), 0, 3, kLineIn, 0},
    {"CPU meter", kEnum, kCfg, CF(cpuMeter), 0, 3, kCpuMeter, 0},   // [I] 2.x
    // [I] 2.x: the synth voice budget (governor budget B, OPTIMIZATION_2X item 10 / OP-8).
    {"Synth voices", kNum, kCfg, CF(synthVoices), 1, Config::kMaxSynthVoices, nullptr, 0},
    {"Export sample rate", kEnum, kCfg, CF(exportRate), 0, 2, kExportRates, 0},   // [I] 2.x
    {"Stream from card", kEnum, kCfg, CF(streamMode), 0, 3, kStreamModes, 0},          // [I] 2.x
    {"Stream threshold", kEnum, kCfg, CF(streamThreshold), 0, 4, kStreamThresholds, 0},   // [I] 2.x
};
const Opt kMidi[] = {
    {"Clock in", kEnum, kCfg, CF(clockIn), 0, 3, kClockIn, 0},
    {"Clock out", kEnum, kCfg, CF(clockOut), 0, 4, kPortsOut, 0},
    {"Transport in", kEnum, kCfg, CF(transportIn), 0, 3, kClockIn, 0},
    {"Transport out", kEnum, kCfg, CF(transportOut), 0, 4, kPortsOut, 0},
    {"Notes in", kEnum, kCfg, CF(notesIn), 0, 4, kPortsIn, 0},
    {"Notes in channel", kEnum, kCfg, CF(notesInChannel), 0, 17, kChannels, 0},
    {"MIDI out", kEnum, kCfg, CF(midiOut), 0, 4, kPortsOut, 0},
    {"CC in", kEnum, kCfg, CF(ccIn), 0, 4, kPortsIn, 0},
    {"CC in channel", kEnum, kCfg, CF(ccInChannel), 0, 17, kChannels, 0},
    {"Middle C", kEnum, kCfg, CF(middleC), 0, 4, kMiddleC, 0},
    {"Clock sync correction", kEnum, kCfg, CF(clockCorrection), 0, 13, kClockSync, 0},   // spec 15 §2.3: enum
};
// 2.x USB (Tracker Mini manual §3 Config options: USB > Storage, USB > Audio;
// §13.10). [I] The Mini row is "Storage" with Enter; OpenTracker's row is
// "USB Storage" Off / On (owner request 2026-10-17). Not persisted: the device
// always boots with it Off. No USB audio on this hardware.
const Opt kUsbOpts[] = {
    {"USB Storage", kEnum, kUsb, 0, 0, 2, kOffOn, 0},
};
const Opt kMetronome[] = {
    {"State", kEnum, kCfg, CF(metronome), 0, 2, kOffOn, 0},
    {"Pre roll", kEnum, kCfg, CF(preRoll), 0, 2, kOffOn, 0},
    {"Time signature num", kEnum, kCfg, CF(tsNum), 0, 12, kOneTo12, 0},
    {"Time signature denum", kEnum, kCfg, CF(tsDen), 0, 12, kOneTo12, 0},
    {"Volume", kNum, kCfg, CF(metronomeVolume), 0, 100, nullptr, 0},
};
// /.develop only (USER_REPORTS_R2 §2): the headphone-amp level for A/B
// measurement against stock, whose boot value is unknown. volume(v) per
// spec 01 §5.1: HP_CTRL = 0x80 - round(v * 129), 0x18 = 0 dB, 0.5 dB/step.
// Capped at 0.85 (HP-amp clipping risk above). Not saved.
const char* const kHpLevels[] = {"0.50 (-19.5 dB)", "0.55 (-16.5 dB)", "0.60 (-13.5 dB)", "0.65 (-10.0 dB)",
                                 "0.715 (-6.0 dB)", "0.75 (-3.5 dB)",  "0.80 (-0.5 dB)",  "0.85 (+3.0 dB)"};
const float kHpValues[] = {0.50f, 0.55f, 0.60f, 0.65f, kHeadphoneBootLevel, 0.75f, 0.80f, 0.85f};
uint8_t gHpLevel = 4;   // kHeadphoneBootLevel, app_main's boot value
constexpr uint8_t kDev = 4;   // Opt::store: RAM only, /.develop rows (2.x uses 3 for kUsb)
const Opt kFirmware[] = {
    {"Firmware update", kAction, kNone, 1, 0, 0, nullptr, 0},
    {"Current version", kText, kNone, 0, 0, 0, nullptr, 0},
    {"Build", kText, kNone, 1, 0, 0, nullptr, 0},
    {"Reset config", kAction, kNone, 2, 0, 0, nullptr, 0},
    {"HP level", kEnum, kDev, 0, 0, 8, kHpLevels, 0},   // /.develop only, keep last
};
const Opt kInfo[] = {
    {"Licenses", kAction, kNone, 3, 0, 0, nullptr, 0},
    // [I] 2.x (spec 2x-02 §2.2): since boot.
    {"CPU overruns", kText, kNone, 2, 0, 0, nullptr, 0},
    {"Governor reductions", kText, kNone, 3, 0, 0, nullptr, 0},
    {"Synth voice cap", kText, kNone, 4, 0, 0, nullptr, 0},
};
#undef CF

struct Cat {
    const char* name;
    const Opt* opts;
    int count;
};
const Cat kCats[] = {
    {"Project Settings", kProject, int(sizeof(kProject) / sizeof(Opt))},
    {"General", kGeneral, int(sizeof(kGeneral) / sizeof(Opt))},
    {"MIDI", kMidi, int(sizeof(kMidi) / sizeof(Opt))},
    {"USB", kUsbOpts, int(sizeof(kUsbOpts) / sizeof(Opt))},   // [I] 2.x, Mini order
    {"Metronome", kMetronome, int(sizeof(kMetronome) / sizeof(Opt))},
    {"Firmware", kFirmware, int(sizeof(kFirmware) / sizeof(Opt))},
    {"Info", kInfo, int(sizeof(kInfo) / sizeof(Opt))},
};
constexpr int kCatCount = int(sizeof(kCats) / sizeof(Cat));
bool gDevRows = false;   // /.develop: show the develop-only rows (ConfigScreen::start)

// Visible options of a category: the trailing develop row of Firmware only
// in /.develop mode.
int catCount(int cat) { return kCats[cat].count - (kCats[cat].opts == kFirmware && !gDevRows ? 1 : 0); }

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Stock wide-selector icons (spec 20 §5 icons 5 / 6, drawn as our own
// primitives): 14 x 6 triangles at y 451..456 [S snap 004, 030].
void barTriangle(Gfx& g, int x, bool up, uint32_t c) {
    g.rectsBegin(c);
    for (int r = 0; r < 6; ++r) {
        const int w = 4 + 2 * r;
        g.rectsAdd(x + 7 - w / 2, up ? 451 + r : 456 - r, w, 1);
    }
    g.rectsEnd();
}
// "▲ text ▼" across the labels x0..x1 (strip coordinates): the icons sit
// `inset` px inside the span (13 for two labels, 43 for four) [S snap 004].
void wideArrows(Gfx& g, const Theme& th, int x0, int x1, int inset) {
    const uint32_t c = th.slot(slot::kBottomLabel);
    barTriangle(g, x0 + inset, true, c);
    barTriangle(g, x1 - inset - 13, false, c);
}

constexpr int kValueRight = 590;   // pen end: value ink ends at x 586..588 [S snap 004]
constexpr int kHelpTop = 60;       // help box (210, 60, 185, 365), spec 20 §4.2

// Stock Config strip: half-dark 050505 on the Original theme (C5); the other
// themes keep their strip colour [S snap 034, 038]. Theme::darkStrip().
uint32_t configStrip(const Theme& th) { return th.panelFade() ? th.darkStrip() : wg::kThemeColour; }

// Display brightness to the backlight (spec 05 §1.8: Low/Medium/High ->
// REG_PWM_DUTY 20/50/128). The device glue also re-applies it every loop.
void applyBacklight(App& a) {
    if (a.hal().display) a.hal().display->setBacklight(IDisplay::brightnessDuty(a.cfg.brightness));
}

// Licenses / Info page (spec 06 §7.5): full-page text box, F8 Close. The
// stock text is not reproduced; this is OpenTracker's own list of the
// open-source components it uses.
bool gLicMode = false;
const char* const kLicenseLines[] = {
    ("OpenTracker " OT_VERSION),
    "Open-source firmware for this sampler/sequencer hardware.",
    "",
    "Third-party components:",
    "- Teensy 3.x core library (c) PJRC.COM, LLC",
    "    MIT-style permissive licence. USB, timing and startup code.",
    "- FatFs R0.15a (c) ChaN",
    "    FatFs licence (BSD-style, 1 clause). SD card file system.",
    "- tracker-lib (MIT licence)",
    "    Reference for the project, pattern and instrument file formats.",
    "- DejaVu fonts (Bitstream Vera licence) - desktop simulator only.",
    "",
    "Full licence texts ship with the OpenTracker source code",
    "(third_party/ and sim/third_party/).",
};
constexpr int kLicenseLineCount = int(sizeof(kLicenseLines) / sizeof(kLicenseLines[0]));

// Firmware-update file list (spec 06 §7.5): at most 101 entries, sorted.
constexpr int kFwMax = ConfigScreen::kFwMax;
char (*const gFw)[48] = reinterpret_cast<char (*)[48]>(g_browserLists);   // shared (screens.h)
int gFwCount = 0;
bool gFwMode = false;
struct FwCtx {
    App* app;
};
bool fwFn(const IStorage::DirEntry& e, void* c) {
    App* a = static_cast<FwCtx*>(c)->app;
    if (e.isDir || gFwCount >= kFwMax || std::strlen(e.name) >= sizeof(gFw[0])) return gFwCount < kFwMax;
    if (a->hal().isUpdateImageName && a->hal().isUpdateImageName(e.name)) {
        // Insertion into the sorted list.
        int i = gFwCount++;
        while (i > 0 && std::strcmp(gFw[i - 1], e.name) > 0) {
            std::memcpy(gFw[i], gFw[i - 1], sizeof(gFw[0]));
            --i;
        }
        std::snprintf(gFw[i], sizeof(gFw[0]), "%s", e.name);
    }
    return gFwCount < kFwMax;
}

// [I] 2.x theme files: the Display theme picker lists the stock themes, then
// Themes/*.txt (ui/theme_file.h), rebuilt on every Enter.
char (*const gThemeList)[kThemeNameMax + 1] = reinterpret_cast<char (*)[kThemeNameMax + 1]>(g_browserLists);
int gThemeCount = 0;
bool gThemeMode = false;
static_assert(kThemeListMax * (kThemeNameMax + 1) <= kBrowserListBytes, "Theme list");
bool isThemeOpt(const Opt& o) { return o.store == kCfg && o.off == offsetof(Config, theme); }

// Factory-test entry sequences (spec 06 §7.6).
const uint8_t kCodeKeys[4] = {btn::F8, btn::F1, btn::F7, btn::F2};
const uint8_t kCodePads[4] = {47, 10, 45, 8};
}  // namespace

static uint8_t* optVar(App& a, const Opt& o) {
    if (o.store == kCfg) return reinterpret_cast<uint8_t*>(&a.cfg) + o.off;
    if (o.store == kProj) return &a.project.values[o.off - mt::kValuesOff];
    if (o.store == kDev) return &gHpLevel;
    return nullptr;
}

static int optCount(const Opt& o) { return o.kind == kNum ? o.max - o.min + 1 : o.max; }

static void optValueText(App& a, const Opt& o, int index, char* out, int cap) {
    if (o.kind == kText) {
        const audio::Governor& gov = a.engine.governor();
        switch (o.off) {
        case 2:
            std::snprintf(out, size_t(cap), "%lu", static_cast<unsigned long>(a.engine.cpu().overruns()));
            break;
        case 3: std::snprintf(out, size_t(cap), "%lu", static_cast<unsigned long>(gov.cuts())); break;
        case 4: std::snprintf(out, size_t(cap), "%d/%d", gov.cap(), gov.budget()); break;
        default: std::snprintf(out, size_t(cap), "%s", o.off == 0 ? "OpenTracker " OT_VERSION : OT_BUILD);
        }
        return;
    }
    if (o.kind == kAction) {
        out[0] = 0;
        return;
    }
    if (o.kind == kNum) {
        std::snprintf(out, size_t(cap), "%d", index + o.min + o.displayOffset);
        return;
    }
    if (isThemeOpt(o) && a.customThemeActive()) std::snprintf(out, size_t(cap), "%s", a.customThemeName());
    else if (o.labels) std::snprintf(out, size_t(cap), "%s", o.labels[clampi(index, 0, o.max - 1)]);
    else if (o.off == pf::kScale && o.store == kProj) std::snprintf(out, size_t(cap), "%s", scaleName(uint8_t(index)));
    else std::snprintf(out, size_t(cap), "%d", index);
    (void)a;
}

static int optIndex(App& a, const Opt& o) {
    if (o.store == kUsb) return a.usbStorageActive() ? 1 : 0;
    uint8_t* v = optVar(a, o);
    if (!v) return 0;
    return clampi(int(*v) - (o.kind == kNum ? o.min : 0), 0, optCount(o) - 1);
}

void ConfigScreen::start(int opt) {
    (void)opt;
    editing = false;
    gFwMode = false;
    gThemeMode = false;
    gLicMode = false;
    focus = 0;
    gDevRows = ui_.developMode();
    option = clampi(option, 0, catCount(category) - 1);
    refreshBar();
}

int ConfigScreen::firmwareCount() { return gFwCount; }
int ConfigScreen::themeCount() { return gThemeCount; }
const char* ConfigScreen::themeName(int i) { return i >= 0 && i < gThemeCount ? gThemeList[i] : ""; }
const char* ConfigScreen::firmwareName(int i) { return i >= 0 && i < gFwCount ? gFw[i] : ""; }

void ConfigScreen::feedCode(bool pad, uint8_t v) {
    // spec 06 §7.6: mismatch -> 0 (not re-tested as a first element);
    // the 4th match -> 0 and interface event 11.
    const uint8_t want = pad ? kCodePads[codePos] : kCodeKeys[codePos];
    if (v != want) {
        codePos = 0;
        return;
    }
    if (++codePos < 4) return;
    codePos = 0;
    ui_.startFactoryTest();
}

static RecorderScreen* recorderOf(Ui& ui) { return static_cast<RecorderScreen*>(ui.screen(kModRecorder)); }

bool ConfigScreen::ownsModuleKey(uint8_t id) const {
    const RecorderScreen* r = recorderOf(ui_);
    return id == btn::SampleRecorder && r && r->returnsFromConfig();
}

// Leaving Config opened from the recorder's F4 Metronome goes back to the
// recorder setup with Metronome selected (spec 05 appendix C §2.9).
bool ConfigScreen::returnToRecorder() {
    const RecorderScreen* r = recorderOf(ui_);
    if (!r || !r->returnsFromConfig()) return false;
    ui_.switchTo(kModRecorder, kModConfig);
    return true;
}

void ConfigScreen::update(uint32_t now) {
    // The recorder sets its return flag after the switch: show "Back" then.
    if (!gLicMode && !editing && focus == 0) {
        const RecorderScreen* r = recorderOf(ui_);
        if ((ui_.bar.label[6].top[0] != 0) != (r && r->returnsFromConfig())) {
            refreshBar();
            ui_.dirty();
        }
    }
    // spec 06 §7.2 developer overlay: refreshed every > 1 s.
    if (!ui_.developMode() || (devValid && now - lastDev_ <= 1000)) return;
    lastDev_ = now;
    devValid = true;
    if (!ui_.platform.devStats || !ui_.platform.devStats(dev)) dev = DevStats{};
    ui_.dirty();
}

void ConfigScreen::onPad(uint8_t pad, uint8_t state) {
    // spec 06 §7.4: pads only feed the factory-test code (also on the
    // Licenses page, §7.5).
    if (state == kPress) feedCode(true, pad);
}

void ConfigScreen::stop() {
    // spec 06 §7: stop forces a config save.
    app_.requestConfigSave(true);
    editing = false;
    gLicMode = false;
}

void ConfigScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    b.clear();
    if (gLicMode) {   // spec 06 §7.5: F1..F7 unbound, F8 Close
        b.set(7, "Close");
        return;
    }
    // Stock strip variant: 050505, separators only at 200 / 600 / 700
    // (VISUAL_DIFF C5 / C9) [S snap 004, 030].
    b.stripBg = configStrip(ui_.th());
    b.separators = 0xC4;
    const Cat& c = kCats[category];
    b.setWide(0, c.name);
    const Opt& o = c.opts[clampi(option, 0, catCount(category) - 1)];
    // F3..F6: "▲ <item> ▼" centred on 400 (also while the category list has
    // the focus [S snap 004]).
    b.setWide(3, gFwMode ? "Firmware update" : o.name);
    if (!editing) {
        const RecorderScreen* r = recorderOf(ui_);
        b.set(6, focus == 1 || (r && r->returnsFromConfig()) ? "Back" : "");
        b.set(7, focus == 1 && o.kind == kText ? "" : "Enter");
    } else {
        b.set(6, "Cancel");
        b.set(7, gFwMode ? "Select" : "Apply");
    }
}

void ConfigScreen::moveCat(int d) {
    editing = false;
    gFwMode = false;
    gThemeMode = false;
    category = clampi(category + d, 0, kCatCount - 1);
    option = 0;
}
void ConfigScreen::moveOpt(int d) { option = clampi(option + d, 0, catCount(category) - 1); }
void ConfigScreen::movePick(int d) {
    const Opt& o = kCats[category].opts[option];
    const int n = gFwMode ? gFwCount : (gThemeMode ? kThemeCount + gThemeCount : optCount(o));
    picker = clampi(picker + d, 0, n > 0 ? n - 1 : 0);
}

void ConfigScreen::enter() {
    App& a = app_;
    const Opt& o = kCats[category].opts[option];
    if (o.kind == kNum || o.kind == kEnum) {
        editing = true;
        picker = optIndex(a, o);
        if (isThemeOpt(o)) {
            // [I] 2.x: the card's theme files after the stock three (never
            // while the computer owns the card).
            gThemeMode = true;
            gThemeCount = a.usbStorageActive() || !a.hal().storage ? 0 : listThemeFiles(a.fs(), gThemeList, kThemeListMax);
            for (int i = 0; a.customThemeActive() && i < gThemeCount; ++i)
                if (!std::strcmp(gThemeList[i], a.customThemeName())) picker = kThemeCount + i;
        }
        return;
    }
    if (o.kind != kAction) return;
    if (o.off == 1) {
        if (!a.hal().requestFirmwareUpdate) {
            ui_.notify("Firmware update not available");
            return;
        }
        gFwCount = 0;
        FwCtx c{&a};
        ui_.progress(kLoadingFirmwares, 0);   // spec 05 §1.3 progress popup while the list loads
        a.fs().listDir("Firmware", fwFn, &c);
        ui_.progressDone();
        if (!gFwCount) {
            ui_.notify("No firmware files in /Firmware");
            return;
        }
        gFwMode = true;
        editing = true;
        picker = 0;
    } else if (o.off == 2) {
        ui_.ask("Reset config", "Restore all settings to defaults?", nullptr, "Cancel", "Yes",
                [](void* ctx, int key) {
                    if (key != btn::F8) return;
                    App& ap = *static_cast<App*>(ctx);
                    // Only the global config; recorder working copies are per project.
                    Config keep = ap.cfg;
                    ap.cfg.setDefaults();
                    ap.cfg.recSource = keep.recSource;
                    std::memcpy(ap.cfg.recGain, keep.recGain, sizeof(keep.recGain));
                    ap.cfg.recMonitor = keep.recMonitor;
                    ap.cfg.radioFreq10 = keep.radioFreq10;
                    ap.clearCustomTheme();
                    ap.requestConfigSave(true);
                    ap.applySequencerSettings();
                    applyBacklight(ap);   // spec 06 §7.5: re-apply the hardware state
                    ap.ui.notify("Config reset");
                },
                &a);
    } else if (o.off == 4) {
        // [I] 2.x Export theme: the active theme as a file to edit.
        char msg[64];
        const char* name = a.activeThemeName();
        switch (a.exportTheme()) {
            case App::ThemeExport::kOk: std::snprintf(msg, sizeof(msg), "Saved %s/%s.txt", kThemesDir, name); break;
            case App::ThemeExport::kExists: std::snprintf(msg, sizeof(msg), "%s/%s.txt exists", kThemesDir, name); break;
            case App::ThemeExport::kUnavailable: std::snprintf(msg, sizeof(msg), "Not available in USB Storage mode"); break;
            default: std::snprintf(msg, sizeof(msg), "Cannot write %s/%s.txt", kThemesDir, name); break;
        }
        ui_.notify(msg);
    } else {
        gLicMode = true;
    }
}

void ConfigScreen::apply() {
    App& a = app_;
    if (!gFwMode && kCats[category].opts[option].store == kDev) {   // /.develop HP level (RAM only)
        gHpLevel = uint8_t(clampi(picker, 0, 7));
        if (ui_.platform.setHeadphoneLevel) ui_.platform.setHeadphoneLevel(kHpValues[gHpLevel]);
        editing = false;
        return;
    }
    if (gFwMode) {
        if (picker < 0 || picker >= gFwCount) return;
        ui_.ask("Flash new firmware?", gFw[picker], nullptr, "Cancel", "Yes",
                [](void* ctx, int key) {
                    auto* self = static_cast<ConfigScreen*>(ctx);
                    if (key != btn::F8) return;
                    App& ap = self->app_;
                    ap.autosave();
                    ap.saveConfig();
                    if (!ap.hal().requestFirmwareUpdate(ap.fs(), gFw[self->picker])) ap.ui.notify("Update failed");
                },
                this);
        gFwMode = false;
        editing = false;
        return;
    }
    if (gThemeMode) {
        // [I] 2.x: a stock theme drops the file; a file must open to apply.
        gThemeMode = false;
        editing = false;
        if (picker < kThemeCount) {
            a.clearCustomTheme();
            a.cfg.theme = uint8_t(clampi(picker, 0, kThemeCount - 1));
        } else if (picker - kThemeCount < gThemeCount) {
            const char* name = gThemeList[picker - kThemeCount];
            if (!a.loadCustomTheme(name)) {
                char msg[64];
                std::snprintf(msg, sizeof(msg), "Cannot read %s/%s.txt", kThemesDir, name);
                ui_.notify(msg);
                return;
            }
        }
        a.markConfigDirty();
        a.applySequencerSettings();
        return;
    }
    // Values are stored only on Apply (spec 06 §7.4).
    const Opt& o = kCats[category].opts[option];
    if (o.store == kUsb) {
        // 2.x USB Storage: On enters the mode (its page covers the screen),
        // Off leaves it like the page's Exit. Refusals show a notice.
        editing = false;
        ui_.setUsbStorageMode(picker == 1);
        return;
    }
    uint8_t* v = optVar(a, o);
    const uint8_t was = v ? *v : 0;
    if (v) *v = uint8_t(picker + (o.kind == kNum ? o.min : 0));
    if (o.store == kCfg) {
        a.markConfigDirty();
        if ((v == &a.cfg.streamMode || v == &a.cfg.streamThreshold) && *v != was) {
            // SD streaming: instruments are chosen at load time, so the
            // workspace reloads (saved first); the Config screen stays.
            if (a.cfg.streamMode != 0 && !a.hal().streamReader) {
                a.cfg.streamMode = 0;
                ui_.notify("Streaming is not available in this build");
            } else {
                a.autosave();
                if (!workspaceFitsMemory(a)) {
                    // The reload would drop samples (and a later save could
                    // lose their files): keep the previous setting.
                    *v = was;
                    ui_.notify("Keep Stream from card: the project does not fit in memory without it", 4000);
                } else if (!a.startLoadWorkspace()) {
                    ui_.notify("Applies when a project opens next");   // no saved workspace
                }
            }
        }
        a.applySequencerSettings();
        if (v == &a.cfg.brightness) applyBacklight(a);
    } else {
        a.markProjectDirty();
        a.refreshPadNotes();
    }
    editing = false;
}

void ConfigScreen::onButton(uint8_t id, uint8_t state) {
    if (state != kPress) return;
    if (id == btn::SampleRecorder) {   // owned only while the recorder waits
        returnToRecorder();
        return;
    }
    if (id == btn::Play && ui_.platform.devStatsReset) ui_.platform.devStatsReset();   // spec 06 §7.4
    const bool fkey = btn::isFKey(id) && !gLicMode;
    if (gLicMode) {
        // spec 06 §7.5: F1..F8 and the arrows are unbound except F8 = Close;
        // module keys still leave the screen.
        switch (id) {
            case btn::F8: gLicMode = false; break;
            case btn::F1: case btn::F2: case btn::F3: case btn::F4:
            case btn::F5: case btn::F6: case btn::F7:
            case btn::Up: case btn::Down: case btn::Left: case btn::Right: break;
            default: Screen::onButton(id, state); break;
        }
    } else if (editing) {
        switch (id) {
            case btn::F3:
            case btn::Up: movePick(-1); break;
            case btn::F6:
            case btn::Down: movePick(1); break;
            case btn::F7:
            case btn::Left:
                editing = false;
                gFwMode = false;
                gThemeMode = false;
                break;
            case btn::F8: apply(); break;
            case btn::F1:
            case btn::F2: break;   // spec 06 §7.4: no action while the picker is open
            default: Screen::onButton(id, state); break;
        }
    } else if (focus == 0) {
        switch (id) {
            case btn::F1:
            case btn::Up: moveCat(-1); break;
            case btn::F2:
            case btn::Down: moveCat(1); break;
            case btn::F3:
            case btn::F4:
            case btn::F5:
            case btn::F6:
            case btn::F8:
            case btn::Right: focus = 1; break;
            case btn::F7:
            case btn::Left:
                if (returnToRecorder()) return;
                break;
            default: Screen::onButton(id, state); break;
        }
    } else {
        switch (id) {
            case btn::F1:
            case btn::F2: focus = 0; break;
            case btn::F3:
            case btn::Up: moveOpt(-1); break;
            case btn::F6:
            case btn::Down: moveOpt(1); break;
            case btn::F7:
            case btn::Left: focus = 0; break;
            case btn::F8:
            case btn::Right: enter(); break;
            default: Screen::onButton(id, state); break;
        }
    }
    // spec 06 §7.6: the F-key still did its normal action.
    if (fkey && ui_.active() == kModConfig) feedCode(false, id);
    if (ui_.active() == kModConfig) refreshBar();
}

#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
bool ConfigScreen::encoderOnList() const {
    return !gLicMode;   // categories, options and their pick lists
}
#endif

void ConfigScreen::onEncoder(int d) {
    d = clampi(d, -3, 3);   // spec 06 §7.4
    if (gLicMode) return;
    if (editing) movePick(d);
    else if (focus == 0) moveCat(d);
    else moveOpt(d);
    refreshBar();
}

namespace {
struct ListCtx {
    App* app;
    const Opt* opts;   // option names (null: categories)
    const Opt* pick;   // picker values of this option (null: firmware list)
};
void catItem(void*, int i, char* out, int cap) { std::snprintf(out, size_t(cap), "%s", kCats[i].name); }
void optItem(void* c, int i, char* out, int cap) {
    std::snprintf(out, size_t(cap), "%s", static_cast<ListCtx*>(c)->opts[i].name);
}
void pickItem(void* c, int i, char* out, int cap) {
    const ListCtx& x = *static_cast<ListCtx*>(c);
    if (gThemeMode) std::snprintf(out, size_t(cap), "%s", i < kThemeCount ? theme(uint8_t(i)).name : gThemeList[i - kThemeCount]);
    else if (x.pick) optValueText(*x.app, *x.pick, i, out, cap);
    else std::snprintf(out, size_t(cap), "%s", gFw[i]);
}
int gOptTop = 0, gPickTop = 0;   // first visible rows (scrolling lists)
}  // namespace

void ConfigScreen::draw(Gfx& g) {
    App& a = app_;
    const Theme& th = ui_.th();
    ui_.drawTitle(g, "Config");
    if (gLicMode) {
        // spec 06 §7.5 text box (0, 30, 780, 450). SPEC GAP: height kept to
        // 394 so the bottom bar's "Close" label stays visible. [I] no stock
        // shot: one full-width panel, font 1 rows at the list pitch.
        wg::panel(g, th, 1, wg::kPanelY, 797, wg::kPanelH);
        for (int i = 0; i < kLicenseLineCount; ++i)
            g.text(14, 46 + i * wg::kListRowH, kFont1, th.slot(slot::kList + 4), kLicenseLines[i],
                   IDisplay::kOptCenterY);
        return;
    }
    const uint32_t textC = th.slot(slot::kList + 4);
    // spec 20 §4.2: category list (1, 29, 197, 394), white selection outline.
    wg::ListView cv;
    cv.count = kCatCount;
    cv.sel = category;
    cv.selStyle = wg::ListSel::kWhite;
    cv.item = catItem;
    wg::list(g, th, cv);
    const Cat& c = kCats[category];
    // Third column: empty panel (601, 29, 197, 394) [S snap 004].
    wg::panelColumn(g, th, 6, 2);
    if (ui_.developMode() && devValid) {
        // spec 06 §7.2 developer overlay, labels at (615, 240) and (615, 300).
        char l[48];
        if (ui_.platform.devStats) {
            std::snprintf(l, sizeof(l), "CPU %lu/%lu/%lu/%lu%%", static_cast<unsigned long>(dev.cpuMin / 10),
                          static_cast<unsigned long>(dev.cpuMax / 10), static_cast<unsigned long>(dev.cpuAvg / 10),
                          static_cast<unsigned long>(dev.cpuLast / 10));
        } else {
            std::snprintf(l, sizeof(l), "CPU n/a");
        }
        g.text(615, 240, kFont1, th.slot(slot::kBottomLabel), "min/max/avg/last", IDisplay::kOptCenterY);
        g.text(615, 262, kFont1, textC, l, IDisplay::kOptCenterY);
        std::snprintf(l, sizeof(l), "free %lu KB", static_cast<unsigned long>(dev.memFree / 1024));
        g.text(615, 300, kFont1, textC, l, IDisplay::kOptCenterY);
        std::snprintf(l, sizeof(l), "loop max %lu us", static_cast<unsigned long>(dev.loopMaxUs));
        g.text(615, 322, kFont1, textC, l, IDisplay::kOptCenterY);
    }
    // Wide "▲ ... ▼" selectors over F1+F2 and F3..F6 [S snap 004, 030].
    wideArrows(g, th, 0, 199, 13);
    if (ui_.bar.label[3].top[0]) wideArrows(g, th, 200, 599, 43);
    if (editing) {
        // Picker (spec 20 §4.2 / VISUAL_DIFF §8.19): help panel x 204..399
        // and value list x 401..595 from y 34, one red focus frame around
        // both; caption font 1 at x 215, help text font 5, pitch 19.
        const Opt& o = c.opts[option];
        const int n = gFwMode ? gFwCount : (gThemeMode ? kThemeCount + gThemeCount : optCount(o));
        wg::panel(g, th, 204, 34, 195, 389);
        wg::panel(g, th, 401, 34, 194, 389);
        const char* title = gFwMode ? "Firmware update" : o.name;
        g.text(215, 46, kFont1, textC, title, IDisplay::kOptCenterY);
        const char* help = gFwMode ? nullptr : configHelp(category, option);
        if (help) wg::textBox(g, th, 210, kHelpTop, 185, 413 - kHelpTop, kFont5, help, 19 - fontHeight(kFont5), textC);
        ListCtx x{&a, nullptr, gFwMode ? nullptr : &o};
        wg::ListView pv;
        pv.x = 401;
        pv.background = false;
        pv.count = n;
        pv.sel = picker;
        pv.selStyle = wg::ListSel::kWhite;
        gPickTop = wg::listFollow(picker, n, wg::listVisibleRows(pv.h), gPickTop);
        pv.top = gPickTop;
        pv.item = pickItem;
        pv.ctx = &x;
        wg::list(g, th, pv);
        wg::focusFrame(g, th, 201, 31, 598, 422);
        return;
    }
    if (focus == 0) wg::panelFocus(g, th, 1, 197);
    // Item list (201, 29, 397, 394): names at x 214, values right-aligned
    // with their ink ending at x 588, white [S snap 004]; the cursor row is
    // outlined also while the category list has the focus.
    ListCtx x{&a, c.opts, nullptr};
    wg::ListView ov;
    ov.x = 201;
    ov.w = 397;
    ov.count = catCount(category);
    ov.sel = option;
    ov.selStyle = wg::ListSel::kWhite;
    gOptTop = wg::listFollow(option, catCount(category), wg::listVisibleRows(ov.h), focus == 1 ? gOptTop : 0);
    ov.top = gOptTop;
    ov.item = optItem;
    ov.ctx = &x;
    wg::list(g, th, ov);
    char v[48];
    const int rows = wg::listVisibleRows(ov.h);
    for (int r = 0; r < rows && ov.top + r < catCount(category); ++r) {
        const Opt& o = c.opts[ov.top + r];
        optValueText(a, o, optIndex(a, o), v, sizeof(v));
        if (v[0])
            g.text(valueRight(category), ov.y + 17 + wg::kListRowH * r, kFont1, textC, v,
                   IDisplay::kOptCenterY | IDisplay::kOptRightX);
    }
    if (focus == 1) wg::panelFocus(g, th, 201, 397);
}

// ---------------------------------------------------------------------------
// Menu model access (tests/tools)

int ConfigScreen::categoryCount() { return kCatCount; }
const char* ConfigScreen::categoryName(int cat) { return cat >= 0 && cat < kCatCount ? kCats[cat].name : ""; }
int ConfigScreen::valueRight(int cat) {
    // With a scroll bar the list's text area is 7 px narrower (wg::list:
    // 13 instead of 6 px at the right): the values move with it. General
    // scrolls since 2.x added rows; stock's lists never did.
    const int n = optionCount(cat);
    return n > wg::listVisibleRows(wg::ListView{}.h) ? kValueRight - 7 : kValueRight;
}

int ConfigScreen::optionCount(int cat) { return cat >= 0 && cat < kCatCount ? catCount(cat) : 0; }

bool ConfigScreen::optionInfo(int cat, int i, OptionInfo* out) {
    if (cat < 0 || cat >= kCatCount || i < 0 || i >= catCount(cat) || !out) return false;
    const Opt& o = kCats[cat].opts[i];
    out->name = o.name;
    out->kind = o.kind;
    out->store = o.store;
    out->count = (o.kind == kNum || o.kind == kEnum) ? optCount(o) : 0;
    out->min = o.kind == kNum ? o.min : 0;
    out->displayOffset = o.kind == kNum ? o.displayOffset : 0;
    out->help = configHelp(cat, i);
    return true;
}

int ConfigScreen::optionIndex(int cat, int i) const {
    if (cat < 0 || cat >= kCatCount || i < 0 || i >= catCount(cat)) return -1;
    return optIndex(app_, kCats[cat].opts[i]);
}

void ConfigScreen::optionText(int cat, int i, char* out, int cap) const {
    out[0] = 0;
    if (cat < 0 || cat >= kCatCount || i < 0 || i >= catCount(cat)) return;
    const Opt& o = kCats[cat].opts[i];
    optValueText(app_, o, optIndex(app_, o), out, cap);
}

void ConfigScreen::applyOption(int cat, int i, int index) {
    if (cat < 0 || cat >= kCatCount || i < 0 || i >= kCats[cat].count) return;
    const Opt& o = kCats[cat].opts[i];
    if (o.kind != kNum && o.kind != kEnum) return;
    category = cat;
    option = i;
    gFwMode = false;
    gThemeMode = false;
    picker = clampi(index, 0, optCount(o) - 1);
    apply();
    if (ui_.active() == kModConfig) refreshBar();
}

}  // namespace ot
