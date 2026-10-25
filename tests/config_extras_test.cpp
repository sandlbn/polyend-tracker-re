// Config extras: factory-test entry code and test mode (spec 06 §7.6, 05
// §2.12), firmware list (06 §7.5), picker F1/F2 (06 §7.4), /.develop overlay
// (06 §7.2), power popup (05 §1.5 state 5), Master Limiter gain-reduction
// meter (06 §4.3), recorder F4 Metronome return (05 appendix C §2.9).
#include "test.h"
#include "ui_fakes.h"

#include <cmath>

#include "ui/screens.h"

using namespace ot;

static ConfigScreen& cfgScreen(Harness& h) { return *static_cast<ConfigScreen*>(h.app->ui.screen(kModConfig)); }

TEST(factory_code_fkeys) {
    Harness h(true, 256 * 1024);
    App& a = *h.app;
    h.tap(btn::Config);
    ConfigScreen& c = cfgScreen(h);
    h.tap(btn::F8);
    h.tap(btn::F1);
    h.tap(btn::F7);
    CHECK_EQ(c.codePos, 3);
    CHECK(!a.ui.factoryTestActive());
    h.tap(btn::F2);
    CHECK(a.ui.factoryTestActive());
    CHECK_EQ(c.codePos, 0);
    CHECK_EQ(a.ui.factory.step(), int(FactoryTest::kScreen));
    // F1 aborts back to Config.
    h.tap(btn::F1);
    CHECK(!a.ui.factoryTestActive());
    CHECK(a.ui.active() == kModConfig);
}

TEST(factory_code_mismatch_and_pads) {
    Harness h(true, 256 * 1024);
    App& a = *h.app;
    h.tap(btn::Config);
    ConfigScreen& c = cfgScreen(h);
    h.tap(btn::F8);
    h.tap(btn::F1);
    h.tap(btn::F1);   // mismatch -> 0, not re-tested as a first element
    CHECK_EQ(c.codePos, 0);
    h.tap(btn::F8);
    h.pad(11);        // wrong pad -> 0
    CHECK_EQ(c.codePos, 0);
    h.tap(btn::F8);   // the counter is shared: F8 then pad 10 is a match
    h.pad(10);
    CHECK_EQ(c.codePos, 2);
    h.tap(btn::F3);
    CHECK_EQ(c.codePos, 0);
    // Pads do not audition in Config.
    h.pad(47);
    h.pad(10);
    h.pad(45);
    CHECK(!a.ui.factoryTestActive());
    h.pad(8);
    CHECK(a.ui.factoryTestActive());
    // Not entered from another screen.
    h.tap(btn::F1);
    h.tap(btn::Pattern);
    h.pad(47);
    h.pad(10);
    h.pad(45);
    h.pad(8);
    CHECK(!a.ui.factoryTestActive());
}

TEST(factory_full_run) {
    Harness h(true, 256 * 1024);
    App& a = *h.app;
    const Config before = a.cfg;
    h.lineGen = [](int16_t* b, uint64_t n) {
        for (int i = 0; i < 128; ++i) b[2 * i] = b[2 * i + 1] = int16_t(8000.0 * std::sin((n * 128 + i) * 0.05));
    };
    h.tap(btn::Config);
    for (uint8_t p : {47, 10, 45, 8}) h.pad(p);
    FactoryTest& f = a.ui.factory;
    CHECK(f.active());
    h.tap(btn::F8);   // screen: pass
    CHECK_EQ(f.result(FactoryTest::kScreen), FactoryTest::kPass);
    CHECK_EQ(f.step(), int(FactoryTest::kInputs));
    for (uint8_t id = 0; id < FactoryTest::kButtons; ++id) h.tap(id);
    for (uint8_t p = 0; p < 48; ++p) h.pad(p);
    CHECK_EQ(f.inputsLeft(), 2);
    h.enc(1);
    h.enc(-1);
    CHECK_EQ(f.result(FactoryTest::kInputs), FactoryTest::kPass);
    CHECK(a.ui.active() == kModConfig);   // module keys did not switch screens
    h.run(1000);
    CHECK_EQ(f.step(), int(FactoryTest::kPadLeds));
    h.run(100);
    h.tap(btn::F7);   // pad backlight: fail
    CHECK_EQ(f.result(FactoryTest::kPadLeds), FactoryTest::kFail);
    CHECK_EQ(f.step(), int(FactoryTest::kRam));
    h.run(500);
    CHECK_EQ(f.ramPass(), FactoryTest::kRamPasses);
    CHECK_EQ(f.ramErrors(), 0u);
    CHECK_EQ(f.result(FactoryTest::kRam), FactoryTest::kPass);
    h.run(1000);
    // No DIN loop on the fake port.
    CHECK_EQ(f.result(FactoryTest::kMidi), FactoryTest::kFail);
    h.run(1000);
    CHECK_EQ(f.result(FactoryTest::kRadio), FactoryTest::kSkip);   // no tuner
    h.run(1000);
    CHECK_EQ(f.result(FactoryTest::kBattery), FactoryTest::kSkip);
    CHECK_EQ(f.step(), int(FactoryTest::kAudio));
    for (int i = 0; i < 200 && f.step() == FactoryTest::kAudio; ++i) {
        h.audio.pump(10);
        h.run(30);
    }
    CHECK(h.fs.files.count("line_in_test.wav") == 1);
    CHECK(h.fs.files.count("mic_test.wav") == 1);
    CHECK_EQ(f.result(FactoryTest::kAudio), FactoryTest::kPass);
    CHECK(!a.capture.active());
    h.run(1000);
    CHECK_EQ(f.result(FactoryTest::kUsbSd), FactoryTest::kPass);
    CHECK(h.fs.files.count("factory_test.tmp") == 0);
    CHECK_EQ(f.step(), int(FactoryTest::kFinished));
    h.tap(btn::F8);
    CHECK(!f.active());
    CHECK(a.ui.active() == kModConfig);
    // Never touches the config image.
    CHECK(std::memcmp(&before, &a.cfg, sizeof(Config)) == 0);
}

TEST(factory_inputs_shift_skip) {
    Harness h(true, 256 * 1024);
    App& a = *h.app;
    h.tap(btn::Config);
    for (uint8_t p : {47, 10, 45, 8}) h.pad(p);
    h.tap(btn::F8);
    h.down(btn::Shift);
    h.run(1600);
    h.up(btn::Shift);
    CHECK_EQ(a.ui.factory.result(FactoryTest::kInputs), FactoryTest::kSkip);
}

TEST(firmware_list_sorted_max_101) {
    Harness h;
    App& a = *h.app;
    h.fs.dirs.insert("Firmware");
    char n[48];
    for (int i = 119; i >= 0; --i) {
        std::snprintf(n, sizeof(n), "Firmware/PolyendTrackerOT%03d.ptf", i);
        h.fs.files[n] = {1};
    }
    h.tap(btn::Config);
    for (int i = 0; i < 5; ++i) h.tap(btn::Down);   // Firmware
    h.tap(btn::Right);
    h.tap(btn::F8);                                 // Firmware update
    CHECK(cfgScreen(h).editing);
    CHECK_EQ(ConfigScreen::firmwareCount(), 101);
    for (int i = 1; i < ConfigScreen::firmwareCount(); ++i)
        CHECK(std::strcmp(ConfigScreen::firmwareName(i - 1), ConfigScreen::firmwareName(i)) < 0);
    CHECK_STR(a.ui.bar.label[7].top, "Select");
}

TEST(picker_ignores_f1_f2) {
    Harness h;
    h.tap(btn::Config);
    ConfigScreen& c = cfgScreen(h);
    h.tap(btn::Down);    // General
    h.tap(btn::Right);
    h.tap(btn::F8);      // edit Radio region
    CHECK(c.editing);
    h.tap(btn::F1);
    h.tap(btn::F2);
    CHECK(c.editing);
    CHECK_EQ(c.category, 1);
}

static int gResets = 0;
TEST(develop_overlay) {
    Harness h(false);
    h.fs.files[".develop"] = {};
    h.app->boot();
    App& a = *h.app;
    CHECK(a.ui.developMode());
    a.ui.platform.devStats = [](DevStats& d) {
        d.cpuMin = 100;
        d.cpuMax = 400;
        d.cpuAvg = 250;
        d.cpuLast = 300;
        d.memFree = 1 << 20;
        return true;
    };
    a.ui.platform.devStatsReset = []() { ++gResets; };
    h.tap(btn::Config);
    h.run(1200);
    ConfigScreen& c = cfgScreen(h);
    CHECK(c.devValid);
    CHECK_EQ(c.dev.cpuAvg, 250u);
    h.tap(btn::Play);
    CHECK_EQ(gResets, 1);
    h.tap(btn::Play);
    // Without the file there is no overlay.
    Harness h2;
    CHECK(!h2.app->ui.developMode());
}

TEST(power_popup) {
    Harness h;
    App& a = *h.app;
    h.down(btn::Power);
    CHECK(a.ui.powerPopupOpen());
    h.tap(btn::Config);   // input blocked
    CHECK(a.ui.active() == kModPattern);
    h.up(btn::Power);     // released before 1.5 s: cancelled
    CHECK(!a.ui.powerPopupOpen());
    h.tap(btn::Config);
    CHECK(a.ui.active() == kModConfig);
    // Shift + Power is the screenshot, no popup.
    h.down(btn::Shift);
    h.down(btn::Power);
    CHECK(!a.ui.powerPopupOpen());
    h.up(btn::Power);
    h.up(btn::Shift);
}

TEST(power_popup_countdown) {
    // spec 05 §1.5: the "Shutting down" popup shows the remaining part of the
    // 1500 ms hold (100..0 %, the bar shrinks as on stock), redrawn while the
    // button is held.
    CHECK_EQ(Ui::powerPercent(0), 100);
    CHECK_EQ(Ui::powerPercent(750), 50);
    CHECK_EQ(Ui::powerPercent(1500), 0);
    CHECK_EQ(Ui::powerPercent(4000), 0);
    Harness h;
    App& a = *h.app;
    h.down(btn::Power);   // runs 20 ms
    CHECK(a.ui.powerPopupOpen());
    CHECK(h.disp.lastHasText("Shutting down"));
    const int p0 = a.ui.powerPopupPercent();
    CHECK(p0 > 90 && p0 <= 100);
    h.run(700);
    const int p1 = a.ui.powerPopupPercent();
    CHECK(p1 >= 45 && p1 <= 55);
    const int frames = h.disp.swaps;
    h.run(300);
    CHECK(h.disp.swaps > frames);   // the bar shrinks while held
    CHECK(a.ui.powerPopupPercent() < p1);
    h.up(btn::Power);     // early release: closed, as before
    CHECK(!a.ui.powerPopupOpen());
    CHECK_EQ(a.ui.powerPopupPercent(), 0);
}

TEST(limiter_gain_reduction_meter) {
    CHECK(MixerScreen::reductionDb(1.0f) == 0.0f);
    CHECK(std::fabs(MixerScreen::reductionDb(0.5f) - 6.0206f) < 0.01f);
    CHECK(MixerScreen::reductionDb(1e-6f) == MixerScreen::kGrRangeDb);
    Harness h;
    App& a = *h.app;
    h.tap(btn::Master);
    h.tap(btn::F4);
    MixerScreen& m = *static_cast<MixerScreen*>(a.ui.screen(kModMaster));
    CHECK_EQ(m.sub, 3);
    CHECK_STR(a.ui.bar.label[3].top, "Gain Reduce");
    // One centred line, no value [S round 3, Snapshot_008]; the reduction
    // shows on the cyan meter above F4.
    CHECK_STR(a.ui.bar.label[3].bottom, "");
}

TEST(recorder_metronome_config_back) {
    Harness h;
    App& a = *h.app;
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = *static_cast<RecorderScreen*>(a.ui.screen(kModRecorder));
    h.tap(btn::F4);
    CHECK(a.ui.active() == kModConfig);
    CHECK_STR(a.ui.bar.label[6].top, "Back");
    h.tap(btn::F7);   // leave Config
    CHECK(a.ui.active() == kModRecorder);
    CHECK_EQ(r.sel, 3);
    CHECK(!r.returnsFromConfig());
    // The recorder key does the same; Left from the category list too.
    h.tap(btn::F4);
    h.tap(btn::SampleRecorder);
    CHECK(a.ui.active() == kModRecorder);
    CHECK_EQ(r.sel, 3);
    h.tap(btn::F4);
    h.tap(btn::Left);
    CHECK(a.ui.active() == kModRecorder);
    // Normal Config: F7 at the category list does nothing.
    h.tap(btn::Config);
    h.tap(btn::F7);
    CHECK(a.ui.active() == kModConfig);
}

// USER_REPORTS_R2 §2: /.develop adds Config > Firmware > "HP level" (RAM
// only) so the headphone level can be A/B'd against stock.
static float gHp = -1.0f;
TEST(develop_hp_level) {
    int fw = -1;
    for (int k = 0; k < ConfigScreen::categoryCount(); ++k)
        if (std::strcmp(ConfigScreen::categoryName(k), "Firmware") == 0) fw = k;
    CHECK(fw >= 0);
    {
        Harness plain;
        plain.tap(btn::Config);
        CHECK_EQ(ConfigScreen::optionCount(fw), 4);   // hidden without /.develop
    }
    Harness h(false);
    h.fs.files[".develop"] = {};
    h.app->boot();
    h.app->ui.platform.setHeadphoneLevel = [](float v) { gHp = v; };
    h.tap(btn::Config);
    ConfigScreen& c = cfgScreen(h);
    CHECK_EQ(ConfigScreen::optionCount(fw), 5);
    ConfigScreen::OptionInfo info;
    CHECK(ConfigScreen::optionInfo(fw, 4, &info));
    CHECK(std::strcmp(info.name, "HP level") == 0);
    CHECK_EQ(info.count, 8);
    // Labels match volume(v): HP_CTRL = 0x80 - round(v * 129), 0x18 = 0 dB.
    char t[32], want[32];
    // Entry 4 is the boot level: +1 dB over the first builds' 0.70 (owner
    // decision 2026-10-23, USER_REPORTS_R2 §2), HP_CTRL 0x24 = -6.0 dB.
    const float vals[8] = {0.50f, 0.55f, 0.60f, 0.65f, 0.715f, 0.75f, 0.80f, 0.85f};
    CHECK(std::fabs(kHeadphoneBootLevel - 0.715f) < 1e-6f);
    CHECK_EQ(0x80 - int(kHeadphoneBootLevel * 129.0f + 0.5f), 0x24);
    for (int i = 0; i < 8; ++i) {
        const float v = vals[i];
        const int reg = 0x80 - int(v * 129.0f + 0.5f);
        const float db = float(0x18 - reg) * 0.5f;
        std::snprintf(want, sizeof(want), i == 4 ? "%.3f (%s%.1f dB)" : "%.2f (%s%.1f dB)", double(v),
                      db >= 0 ? "+" : "", double(db));
        c.category = fw;
        c.focus = 1;
        c.option = 4;
        h.tap(btn::F8);   // Enter
        CHECK(c.editing);
        c.picker = i;
        h.tap(btn::F8);   // Apply
        CHECK(!c.editing);
        c.optionText(fw, 4, t, sizeof(t));
        CHECK(std::strcmp(t, want) == 0);
        CHECK(std::fabs(gHp - v) < 1e-6f);
    }
    h.tap(btn::F8);
    c.picker = 4;
    h.tap(btn::F8);
    c.optionText(fw, 4, t, sizeof(t));
    CHECK(std::strcmp(t, "0.715 (-6.0 dB)") == 0);
}
