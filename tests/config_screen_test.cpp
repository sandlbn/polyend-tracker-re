// Config screen: display brightness -> backlight (spec 05 §1.8) and the
// Licenses / Info page (spec 06 §7.5). PLAN_PHASE2 T1c.
#include <cmath>

#include "test.h"
#include "ui_fakes.h"
#include "nvstore_fake.h"

#include "app/project_fields.h"

#include "ui/config_help.h"
#include "ui/screens.h"

using namespace ot;

static ConfigScreen& cfgScreen(Harness& h) { return *static_cast<ConfigScreen*>(h.app->ui.screen(kModConfig)); }

TEST(brightness_duty_mapping) {
    static_assert(IDisplay::brightnessDuty(0) == 20, "Low");
    static_assert(IDisplay::brightnessDuty(1) == 50, "Medium");
    static_assert(IDisplay::brightnessDuty(2) == 128, "High");
    static_assert(IDisplay::brightnessDuty(7) == 128, "clamps to High");
    CHECK(IDisplay::brightnessDuty(3) == 128);
}

TEST(config_brightness_applies_backlight) {
    Harness h;
    App& a = *h.app;
    CHECK(a.cfg.brightness == 2);
    h.tap(btn::Config);
    ConfigScreen& c = cfgScreen(h);
    h.tap(btn::Down);         // General
    CHECK(c.category == 1);
    h.tap(btn::Right);        // options
    h.tap(btn::Down);         // Brightness
    CHECK(c.option == 1);
    h.tap(btn::F8);           // edit
    CHECK(c.editing);
    h.tap(btn::Up);           // High -> Medium
    CHECK(h.disp.backlightCalls == 0);   // not applied before Apply
    h.tap(btn::F8);
    CHECK(a.cfg.brightness == 1);
    CHECK(h.disp.backlight == 50);
    h.tap(btn::F8);
    h.tap(btn::Up);           // Medium -> Low
    h.tap(btn::F8);
    CHECK(a.cfg.brightness == 0);
    CHECK(h.disp.backlight == 20);
    // Another option does not touch the backlight.
    const int calls = h.disp.backlightCalls;
    h.tap(btn::Down);         // Theme
    h.tap(btn::F8);
    h.tap(btn::F8);
    CHECK(h.disp.backlightCalls == calls);
}

TEST(config_reset_reapplies_backlight) {
    Harness h;
    App& a = *h.app;
    a.cfg.brightness = 0;
    h.tap(btn::Config);
    for (int i = 0; i < 5; ++i) h.tap(btn::Down);   // Firmware
    CHECK(cfgScreen(h).category == 5);
    h.tap(btn::Right);
    for (int i = 0; i < 3; ++i) h.tap(btn::Down);   // Reset config
    h.tap(btn::F8);
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);
    CHECK(a.cfg.brightness == 2);
    CHECK(h.disp.backlight == 128);
}

TEST(config_licenses_page) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Config);
    ConfigScreen& c = cfgScreen(h);
    for (int i = 0; i < 6; ++i) h.tap(btn::Down);   // Info
    CHECK(c.category == 6);
    h.tap(btn::Right);        // Licenses
    CHECK(c.option == 0);
    h.tap(btn::F8);
    h.run(50);
    CHECK(h.disp.lastHasText("FatFs"));
    CHECK(h.disp.lastHasText("Teensy"));
    CHECK(h.disp.lastHasText("tracker-lib"));
    CHECK(h.disp.lastHasText("Close"));
    // Arrows and F1..F7 are unbound while the page is open.
    h.tap(btn::Up);
    h.tap(btn::F1);
    h.tap(btn::F7);
    CHECK(c.option == 0);
    CHECK(c.focus == 1);
    h.run(50);
    CHECK(h.disp.lastHasText("FatFs"));
    // F8 closes back to the option list.
    h.tap(btn::F8);
    h.run(50);
    CHECK(!h.disp.lastHasText("FatFs"));
    CHECK(h.disp.lastHasText("Licenses"));
    CHECK(a.ui.active() == kModConfig);
}

// ---- Complete option list (spec 05 §5.4: 6 categories, 42 options) --------

TEST(config_menu_lists_all_options_with_defaults) {
    Harness h;
    h.tap(btn::Config);
    ConfigScreen& c = cfgScreen(h);
    struct Want {
        const char* name;
        int kind;     // 2 numeric, 3 enum, 5 text, 7 action
        int store;    // 0 config, 1 project, 2 none, 3 live USB storage state
        int count;    // picker entries
        int min;
        int def;      // default picker index (-1: not checked)
    };
    const Want project[] = {
        {"Performance presets", 3, 0, 2, 0, 0}, {"Pattern divider", 2, 1, 16, 0, 3},
        {"Notes scale", 3, 1, 39, 0, 0},      {"Root note", 3, 1, 37, 0, 24},
        {"Notes layout", 3, 1, 12, 0, 11},    {"Anti-aliasing", 3, 1, 2, 0, 1},
        {"Limiter mode", 3, 1, 2, 0, 0},      {"Sends mode", 3, 1, 2, 0, 1},
        {"Extra headroom", 3, 1, 2, 0, 0},    {"Snap to zero", 3, 1, 2, 0, 0},
    };
    const Want general[] = {
        {"Radio region", 3, 0, 4, 0, 0},      {"Display brightness", 3, 0, 3, 0, 2},
        {"Display theme", 3, 0, 3, 0, 0},     {"Display font", 3, 0, 2, 0, 0},
        {"Export theme", 7, 2, 0, 0, -1},                                    // 2.x [I] theme files
        {"Pads brightness", 3, 0, 3, 0, 2},   {"Pads preview", 3, 0, 3, 0, 2},
        {"Recording options", 3, 0, 4, 0, 0}, {"Numbering mode", 3, 0, 3, 0, 1},
        {"Pattern arrangement", 3, 0, 3, 0, 0}, {"Pattern top info", 3, 0, 3, 0, 0},
        {"Line in channel", 3, 0, 3, 0, 0},   {"CPU meter", 3, 0, 3, 0, 0},   // 2.x [I]
        {"Synth voices", 2, 0, 8, 1, 3},                                     // 2.x [I], default 4
        {"Export sample rate", 3, 0, 2, 0, 1},                               // 2.x [I], default 44.1 kHz
        {"Stream from card", 3, 0, 3, 0, 0},                                 // 2.x [I] SD streaming, Off
        {"Stream threshold", 3, 0, 4, 0, 1},                                 // 2.x [I], 10 s
    };
    const Want midi[] = {
        {"Clock in", 3, 0, 3, 0, 0},          {"Clock out", 3, 0, 4, 0, 0},
        {"Transport in", 3, 0, 3, 0, 0},      {"Transport out", 3, 0, 4, 0, 0},
        {"Notes in", 3, 0, 4, 0, 0},          {"Notes in channel", 3, 0, 17, 0, 0},
        {"MIDI out", 3, 0, 4, 0, 0},          {"CC in", 3, 0, 4, 0, 0},
        {"CC in channel", 3, 0, 17, 0, 0},    {"Middle C", 3, 0, 4, 0, 2},
        {"Clock sync correction", 3, 0, 13, 0, 6},
    };
    const Want metro[] = {
        {"State", 3, 0, 2, 0, 0},             {"Pre roll", 3, 0, 2, 0, 0},
        {"Time signature num", 3, 0, 12, 0, 3}, {"Time signature denum", 3, 0, 12, 0, 3},
        {"Volume", 2, 0, 101, 0, 50},
    };
    const Want firmware[] = {
        {"Firmware update", 7, 2, 0, 0, -1}, {"Current version", 5, 2, 0, 0, -1},
        {"Build", 5, 2, 0, 0, -1},           {"Reset config", 7, 2, 0, 0, -1},
    };
    // 2.x USB Storage: live state, never stored (store 3), Off at boot.
    const Want usb[] = {{"USB Storage", 3, 3, 2, 0, 0}};
    const Want info[] = {{"Licenses", 7, 2, 0, 0, -1},
                         // 2.x [I] (spec 2x-02 §2.2)
                         {"CPU overruns", 5, 2, 0, 0, -1},
                         {"Governor reductions", 5, 2, 0, 0, -1},
                         {"Synth voice cap", 5, 2, 0, 0, -1}};
    struct CatWant {
        const char* name;
        const Want* w;
        int n;
    };
    const CatWant cats[] = {
        {"Project Settings", project, 10}, {"General", general, 17}, {"MIDI", midi, 11},
        {"USB", usb, 1},                   // [I] 2.x (Tracker Mini order)
        {"Metronome", metro, 5},           {"Firmware", firmware, 4}, {"Info", info, 4},
    };
    CHECK_EQ(ConfigScreen::categoryCount(), 7);
    int total = 0;
    for (int k = 0; k < 7; ++k) {
        CHECK_STR(ConfigScreen::categoryName(k), cats[k].name);
        CHECK_EQ(ConfigScreen::optionCount(k), cats[k].n);
        for (int i = 0; i < cats[k].n; ++i, ++total) {
            ConfigScreen::OptionInfo o;
            CHECK(ConfigScreen::optionInfo(k, i, &o));
            const Want& w = cats[k].w[i];
            CHECK_STR(o.name, w.name);
            CHECK_EQ(o.kind, w.kind);
            CHECK_EQ(o.store, w.store);
            CHECK_EQ(o.count, w.count);
            CHECK_EQ(o.min, w.min);
            if (w.def >= 0) CHECK_EQ(c.optionIndex(k, i), w.def);
        }
    }
    CHECK_EQ(total, 52);   // stock 42 + 2.x Export theme, CPU meter, Synth voices, Export sample rate, two stream rows, three Info lines, USB Storage
    char t[48];
    c.optionText(3, 0, t, sizeof t);   // USB Storage: Off
    CHECK_STR(t, "Off");
    c.optionText(4, 2, t, sizeof t);   // time signature numerator default 4
    CHECK_STR(t, "4");
    c.optionText(2, 10, t, sizeof t);  // clock sync correction default 0
    CHECK_STR(t, "0");
    c.optionText(0, 1, t, sizeof t);   // pattern divider: stored 3, shown 4 (spec 15 §2.1)
    CHECK_STR(t, "4");
    // Help texts: 25 options (spec 15 §3.2): all of Project Settings and
    // General, CC in, CC in channel, Clock sync correction, Firmware update.
    int helps = 0;
    for (int k = 0; k < 7; ++k)
        for (int i = 0; i < cats[k].n; ++i) {
            ConfigScreen::OptionInfo o;
            CHECK(ConfigScreen::optionInfo(k, i, &o));
            const bool want = k <= 1 || (k == 2 && (i == 7 || i == 8 || i == 10)) || k == 3 || (k == 5 && i == 0);
            CHECK_EQ(o.help != nullptr, want);
            helps += o.help != nullptr;
        }
    CHECK_EQ(helps, 32);   // stock 25 + 2.x Export theme, CPU meter, Synth voices, Export sample rate, two stream rows, USB Storage
    CHECK_EQ(configHelpCount(), 32);
    c.optionText(5, 1, t, sizeof t);
    CHECK(std::strlen(t) > 0);
}

// ---- EEPROM image (P15) ---------------------------------------------------

TEST(config_eeprom_image_layout) {
    FakeNvStore nv;
    Harness h(false);
    h.app->setNvStore(&nv);
    h.app->boot();
    // Fresh EEPROM: defaults written at the stock offsets (spec 05 §5.4).
    CHECK_BYTES(nv.mem.data(), 'E', 'E', 'P', 'R', 'O', 'M', 0);
    CHECK_EQ(nv.mem[0x08] | (nv.mem[0x09] << 8), 0x0A00);
    CHECK_EQ(nv.mem[0x0C], 0xF4);
    CHECK_EQ(nv.mem[0x1B], 81);      // master volume
    CHECK_EQ(nv.mem[0x2F], 2);       // Middle C
    CHECK_EQ(nv.mem[0x30], 6);       // clock correction
    CHECK_EQ(nv.mem[0x34], 10);      // pad LED levels (High)
    CHECK_EQ(nv.mem[0x35], 31);
    CHECK_EQ(nv.mem[0x36], 5);
    CHECK_EQ(nv.mem[0x38], 1);       // vestigial per-track patterns
    CHECK_EQ(nv.mem[0x48], 1);       // perf types 1..12
    CHECK_EQ(nv.mem[0x53], 12);
    CHECK_EQ(nv.mem[0xCB], 2);       // brightness
    CHECK_EQ(nv.mem[0xCF], 0);       // perf presets: global
    CHECK_EQ(nv.mem[0xD0], 1);       // numbering
    CHECK_EQ(nv.mem[0xD1], 2);       // pads preview
    CHECK_EQ(nv.mem[0xD7], 86);      // preview volume
    CHECK_EQ(nv.mem[0xDC], 1);
    CHECK_EQ(nv.mem[0xE8], 3);       // time signature
    CHECK_EQ(nv.mem[0xEA], 50);      // metronome volume
    CHECK_EQ(nv.mem[0xF2], 8);       // slices
    CHECK_EQ(nv.mem[0xF4], 0xFF);    // nothing past the 0xF4-byte image
    CHECK(!h.fs.files.count(kConfigPath));   // no SD config file
    // An option change reaches the EEPROM after the 5 s write-back only.
    h.tap(btn::Config);
    ConfigScreen& c = cfgScreen(h);
    h.tap(btn::Down);
    h.tap(btn::Right);
    h.tap(btn::Down);         // Brightness
    h.tap(btn::F8);
    h.tap(btn::Up);
    h.tap(btn::F8);
    CHECK_EQ(c.option, 1);
    CHECK_EQ(nv.mem[0xCB], 2);
    h.run(5100);
    CHECK_EQ(nv.mem[0xCB], 1);
    // Unknown bytes are preserved across load/save.
    nv.mem[0x1D] = 0xAB;
    nv.mem[0xCD] = 0x5A;
    Harness h2(false);
    h2.app->setNvStore(&nv);
    h2.app->boot();
    CHECK_EQ(h2.app->cfg.brightness, 1);
    h2.app->cfg.theme = 1;
    CHECK(h2.app->saveConfig());
    CHECK_EQ(nv.mem[0xD5], 1);
    CHECK_EQ(nv.mem[0x1D], 0xAB);
    CHECK_EQ(nv.mem[0xCD], 0x5A);
}

TEST(config_imports_sd_file_once) {
    FakeNvStore nv;
    Harness h(false);
    // Older OpenTracker config file (v1: raw typed fields).
    Config old;
    old.numbering = 2;
    old.middleC = 0;
    old.sliceCount = 16;
    std::vector<uint8_t> f(8 + offsetof(Config, image));
    std::memcpy(f.data(), "OTCF", 4);
    f[4] = 1;
    f[5] = uint8_t(offsetof(Config, image));
    std::memcpy(f.data() + 8, &old, offsetof(Config, image));
    h.fs.mkdir(kConfigDir);
    h.fs.files[kConfigPath] = f;
    h.app->setNvStore(&nv);
    h.app->boot();
    CHECK_EQ(h.app->cfg.numbering, 2);
    CHECK_EQ(nv.mem[0xD0], 2);
    CHECK_EQ(nv.mem[0x2F], 0);
    CHECK_EQ(nv.mem[0xF2], 16);
    // Once the EEPROM is valid the file is ignored.
    Config other;
    other.numbering = 0;
    std::memcpy(f.data() + 8, &other, offsetof(Config, image));
    h.fs.files[kConfigPath] = f;
    Harness h2(false);
    h2.fs.files = h.fs.files;
    h2.fs.dirs = h.fs.dirs;
    h2.app->setNvStore(&nv);
    h2.app->boot();
    CHECK_EQ(h2.app->cfg.numbering, 2);
}

TEST(config_eeprom_not_ready_falls_back_to_file) {
    FakeNvStore nv;
    nv.usable = false;   // FlexRAM not EEPROM-ready: never written
    Harness h(false);
    h.app->setNvStore(&nv);
    h.app->boot();
    CHECK_EQ(nv.writes, 0);
    CHECK(h.fs.files.count(kConfigPath));
}

// ---- Recorder settings per project (spec 02 §3.3 0x134..0x13C) -------------

TEST(config_recorder_settings_are_per_project) {
    Harness h;
    App& a = *h.app;
    h.run(20);
    // A project without stored settings takes OpenTracker's defaults.
    CHECK_EQ(a.cfg.recGain[0], 50);
    CHECK_EQ(a.project.u8(pf::kRecGainLine), 50);
    CHECK_EQ(a.cfg.recMonitor, 1);                  // On
    CHECK_EQ(a.project.u8(pf::kRecMonitor), 0);     // stock byte: 0 = On (spec 22 §5)
    // Recorder edits -> project bytes, project changed.
    a.cfg.recSource = 2;
    a.cfg.recGain[2] = 70;   // mic low gain
    a.cfg.recGain[1] = 33;   // radio
    a.cfg.radioFreq10 = 1011;
    h.run(20);
    CHECK_EQ(a.project.u8(pf::kRecSource), 2);
    CHECK_EQ(a.project.u8(pf::kRecGainMicLow), 70);
    CHECK_EQ(a.project.u8(pf::kRecGainRadio), 33);
    CHECK_NEAR(a.project.f32(pf::kRadioFreq), 101.1, 1e-4);
    CHECK(a.unsavedChanges());
    // Another project's bytes -> working copies.
    a.project.setU8(pf::kRecSource, 3);
    a.project.setU8(pf::kRecGainMicHigh, 12);
    a.project.setU8(pf::kRecMonitor, 1);   // stock: Off
    a.project.setF32(pf::kRadioFreq, 99.0f);
    h.run(20);
    CHECK_EQ(a.cfg.recSource, 3);
    CHECK_EQ(a.cfg.recGain[3], 12);
    CHECK_EQ(a.cfg.recMonitor, 0);
    CHECK_EQ(a.cfg.radioFreq10, 990);
}

TEST(config_recorder_source_radio_and_mic_hg_survive_save_and_open) {
    // 0x134 recorder source on the original Tracker: Radio (1) and Mic high
    // gain (3) come back from a saved project (2.0.0 used the Mini's
    // `> 2 -> 0` load rule, which reset Mic high gain to line in).
    for (uint8_t src : {uint8_t(1), uint8_t(3)}) {
        Harness h;
        App& a = *h.app;
        h.run(20);
        a.cfg.recSource = src;
        a.cfg.recGain[1] = 41;
        a.cfg.radioFreq10 = 1037;
        h.run(20);
        CHECK(a.saveProjectAs("RadioSrc"));
        CHECK(a.newProject());
        h.run(20);
        CHECK_EQ(a.cfg.recSource, 0);
        CHECK(a.openProject("Projects", "RadioSrc"));
        h.run(20);
        CHECK_EQ(a.project.u8(pf::kRecSource), src);
        CHECK_EQ(a.cfg.recSource, src);
        CHECK_EQ(a.cfg.recGain[1], 41);
        CHECK_EQ(a.cfg.radioFreq10, 1037);
    }
}

// ---- Stock labels, help box, clock sync picker, line in (spec 15) -----------

TEST(config_stock_value_labels) {
    Harness h;
    App& a = *h.app;
    ConfigScreen& c = cfgScreen(h);
    char t[48];
    struct L {
        int cat, opt, val;
        const char* want;
    };
    const L ls[] = {
        {0, 0, 1, "Per Project"},          {1, 0, 0, "Europe"},
        {1, 0, 2, "Australia"},            {1, 3, 1, "New"},
        {1, 7, 0, "Only notes"},           {1, 7, 3, "Microtiming + Velocity"},   // after 2.x Export theme
        {1, 8, 1, "Decimal, Steps from 1"}, {1, 8, 2, "Hexadecimal"},
        {1, 9, 0, "Vertical Revolver"},    {1, 10, 0, "Additional info"},
        {1, 11, 1, "Mono Left"},           {1, 11, 2, "Mono Right"},
        {2, 0, 2, "MIDI In jack"},         {2, 1, 3, "USB + MIDI Out jack"},
        {2, 4, 3, "USB + MIDI In jack"},   {2, 5, 0, "ALL"},
        {2, 5, 16, "CH16"},                {2, 10, 0, "-6"},
        {2, 10, 12, "6"},
    };
    uint8_t* vars[] = {&a.cfg.perfPresets, &a.cfg.radioRegion, &a.cfg.radioRegion, &a.cfg.font,
                       &a.cfg.recordingOptions, &a.cfg.recordingOptions, &a.cfg.numbering, &a.cfg.numbering,
                       &a.cfg.arrangement, &a.cfg.patternTopInfo, &a.cfg.lineInChannel, &a.cfg.lineInChannel,
                       &a.cfg.clockIn, &a.cfg.clockOut, &a.cfg.notesIn, &a.cfg.notesInChannel,
                       &a.cfg.notesInChannel, &a.cfg.clockCorrection, &a.cfg.clockCorrection};
    const Config keep = a.cfg;
    for (size_t k = 0; k < sizeof(ls) / sizeof(ls[0]); ++k) {
        *vars[k] = uint8_t(ls[k].val);
        c.optionText(ls[k].cat, ls[k].opt, t, sizeof t);
        CHECK_STR(t, ls[k].want);
    }
    a.cfg = keep;
}

TEST(config_help_box_and_pickers) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::Config);
    ConfigScreen& c = cfgScreen(h);
    h.tap(btn::Right);
    h.tap(btn::Down);         // Pattern divider
    h.tap(btn::F8);
    CHECK(c.editing);
    CHECK_EQ(c.picker, 3);
    h.run(50);
    CHECK(h.disp.lastHasText("16"));     // picker shows 1..16
    // The help text (OpenTracker's wording) is drawn while editing.
    CHECK(h.disp.lastHasText("highlighted"));
    h.tap(btn::F7);
    // MIDI > Clock sync correction: enum picker -6..6, stored index.
    h.tap(btn::F1);
    h.tap(btn::Down);
    h.tap(btn::Down);
    CHECK_EQ(c.category, 2);
    h.tap(btn::Right);
    for (int i = 0; i < 10; ++i) h.tap(btn::Down);
    h.tap(btn::F8);
    CHECK(c.editing);
    CHECK_EQ(c.picker, 6);
    h.tap(btn::Down);
    h.tap(btn::Down);
    h.tap(btn::F8);
    CHECK_EQ(a.cfg.clockCorrection, 8);
    CHECK_EQ(a.seq.settings().clockSyncCorrection, 8);
    // Options without stock help show no help text.
    h.tap(btn::Up);           // Middle C
    h.tap(btn::F8);
    h.run(50);
    CHECK(!h.disp.lastHasText("clock pulses"));
}

TEST(config_line_in_channel_reaches_engine) {
    Harness h;
    App& a = *h.app;
    // Line input with signal on the left side only.
    h.lineGen = [](int16_t* buf, uint64_t n) {
        for (int i = 0; i < 128; ++i) {
            buf[2 * i] = int16_t(8000.0 * std::sin((n * 128 + i) * 0.05));
            buf[2 * i + 1] = 0;
        }
    };
    a.project.setU8(pf::kLineInLevel, 100);
    a.project.setU8(pf::kLineInMute, 0);
    a.markProjectDirty();
    h.audio.pump(8);
    CHECK_EQ(a.engine.settings().lineInChannel, 0);
    const int stereoPeak = h.audio.pump(8);
    CHECK(stereoPeak > 0);
    h.tap(btn::Config);
    h.tap(btn::Down);         // General
    h.tap(btn::Right);
    for (int i = 0; i < 11; ++i) h.tap(btn::Down);   // Line in channel (after 2.x Export theme)
    h.tap(btn::F8);
    h.tap(btn::Down);         // Mono Left
    h.tap(btn::Down);         // Mono Right
    h.tap(btn::F8);
    CHECK_EQ(a.cfg.lineInChannel, 2);
    h.audio.pump(8);
    CHECK_EQ(a.engine.settings().lineInChannel, 2);   // Global-mixer pass-through
    CHECK_EQ(h.audio.pump(8), 0);                      // right side is silent
    // Reset config re-applies the default (stereo).
    h.tap(btn::F1);
    for (int i = 0; i < 4; ++i) h.tap(btn::Down);    // Firmware
    h.tap(btn::Right);
    for (int i = 0; i < 3; ++i) h.tap(btn::Down);    // Reset config
    h.tap(btn::F8);
    h.tap(btn::F8);
    h.audio.pump(1);
    CHECK_EQ(a.engine.settings().lineInChannel, 0);
}

// 2.x [I]: Config > General > Synth voices (1..8, default 4) sets the
// governor budget (VERIFY 2.0.0 R3).
TEST(config_synth_voices_reaches_governor) {
    Harness h;
    App& a = *h.app;
    CHECK_EQ(a.engine.governor().budget(), 4);
    h.tap(btn::Config);
    h.tap(btn::Down);         // General
    h.tap(btn::Right);
    for (int i = 0; i < 13; ++i) h.tap(btn::Down);   // Synth voices (after 2.x Export theme)
    ConfigScreen& c = cfgScreen(h);
    char t[48];
    c.optionText(1, 13, t, sizeof t);
    CHECK_STR(t, "4");
    h.tap(btn::F8);
    h.tap(btn::Down);
    h.tap(btn::Down);         // 6
    h.tap(btn::F8);
    CHECK_EQ(a.cfg.synthVoices, 6);
    a.preBlock(0.0f);
    CHECK_EQ(a.engine.governor().budget(), 6);
}

// Values of a list with a scroll bar end left of it (device photo: General,
// longer since 2.x, drew "Europe" / "Medium" under the bar with the Original
// font). A list without one keeps the stock position [S snap 004].
TEST(config_values_clear_the_scroll_bar) {
    int general = -1, metronome = -1;
    for (int c = 0; c < ConfigScreen::categoryCount(); ++c) {
        if (std::string(ConfigScreen::categoryName(c)) == "General") general = c;
        if (std::string(ConfigScreen::categoryName(c)) == "Metronome") metronome = c;
    }
    CHECK(general >= 0 && metronome >= 0);
    CHECK(ConfigScreen::optionCount(general) > wg::listVisibleRows(wg::ListView{}.h));   // it scrolls
    CHECK(ConfigScreen::optionCount(metronome) <= wg::listVisibleRows(wg::ListView{}.h));
    // The list's text area ends at x 201 + 3 + (397 - 13) = 588 with the bar.
    CHECK(ConfigScreen::valueRight(general) <= 584);
    CHECK_EQ(ConfigScreen::valueRight(metronome), 590);
}
