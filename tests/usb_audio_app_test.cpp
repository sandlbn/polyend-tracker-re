// [I] USB audio in (#97) through the app: off in a new project like Line in,
// the Global Mixer USB column, the Sample Recorder's USB source and monitor,
// Resample From USB, and usb_audio.bin.
#include <cmath>
#include <cstdlib>
#include <string>

#include "test.h"
#include "ui_fakes.h"

#include "app/project_fields.h"
#include "audio/audio_tables.h"
#include "audio/engine.h"
#include "formats/file_io.h"
#include "ui/screens.h"

using namespace ot;
using namespace ot::audio;

namespace {

constexpr float kPi = 3.14159265f;
constexpr int kAmp = 6000;   // below the new-project limiter

// A sine on L, its negative on R (the take check tells the channels apart).
void usbSine(int16_t* b, uint64_t blk) {
    for (int i = 0; i < 128; ++i) {
        const float ph = 2.0f * kPi * 1000.0f * float(blk * 128 + uint64_t(i)) / 44100.0f;
        const int16_t v = int16_t(float(kAmp) * std::sin(ph));
        b[2 * i] = v;
        b[2 * i + 1] = int16_t(-v);
    }
}

void lineDc(int16_t* b, uint64_t) {
    for (int i = 0; i < 256; ++i) b[i] = 3000;
}

// True when the last frame drew `s` as a text run.
bool frameHas(Harness& h, const char* s) {
    h.app->ui.dirty();
    h.run(40);
    const std::string q = std::string("\"") + s + "\"";
    for (const std::string& l : h.disp.last)
        if (l.find(q) != std::string::npos) return true;
    return false;
}

MixerScreen& globalMixer(Harness& h) {
    h.tap(btn::Master);
    h.tap(btn::Master);
    h.tap(btn::Master);
    MixerScreen& m = *static_cast<MixerScreen*>(h.app->ui.screen(kModMaster));
    CHECK(m.page == 2);
    return m;
}

int settle(Harness& h) {
    h.run(20);
    h.audio.pump(10);
    return h.audio.pump(30);
}

}  // namespace

TEST(new_project_usb_off) {
    Harness h;
    App& a = *h.app;
    CHECK_EQ(a.project.usbMute, 1);
    CHECK_EQ(a.project.usbLevel, 86);
    CHECK(a.engine.settings().usbMute);
    h.usbGen = usbSine;
    CHECK_EQ(settle(h), 0);
}

TEST(mixer_usb_column_unmutes_and_meters) {
    Harness h;
    App& a = *h.app;
    h.usbGen = usbSine;
    MixerScreen& m = globalMixer(h);
    CHECK(frameHas(h, "USB"));
    CHECK_STR(a.ui.bar.label[7].bottom, "Unmute");
    h.tap(btn::F8);   // release without change: mute toggles
    CHECK_EQ(a.project.usbMute, 0);
    CHECK_STR(a.ui.bar.label[7].bottom, "Mute");
    // Level 86 = 0 dB, then the master volume.
    const float want = float(kAmp) * trkGain(a.cfg.masterVolume);
    CHECK_NEAR(double(settle(h)), double(want), double(want) * 0.03 + 2.0);
    for (int i = 0; i < 20; ++i) {
        h.audio.pump(4);
        h.run(12);
    }
    CHECK(m.globalMeter[4][0] > 0);
    CHECK(m.globalMeter[4][1] > 0);
    CHECK_EQ(m.globalMeter[3][0], 0);   // Line in: nothing
    // Hold + encoder: level; hold + Delete: back to 86.
    h.down(btn::F8);
    h.enc(-20);
    h.up(btn::F8);
    CHECK_EQ(a.project.usbLevel, 66);
    CHECK_EQ(a.project.usbMute, 0);   // a changed level doesn't toggle the mute
    CHECK(settle(h) < int(want * 0.5f));
    h.down(btn::F8);
    h.tap(btn::Delete);
    h.up(btn::F8);
    CHECK_EQ(a.project.usbLevel, 86);
    // Shift + F4 solos Line in: USB muted with the others; again: all back.
    h.down(btn::Shift);
    h.tap(btn::F4);
    h.up(btn::Shift);
    CHECK_EQ(a.project.usbMute, 1);
    CHECK_EQ(a.project.u8(pf::kLineInMute), 0);
    h.down(btn::Shift);
    h.tap(btn::F4);
    h.up(btn::Shift);
    CHECK_EQ(a.project.usbMute, 0);
}

TEST(usb_mixes_with_line_in) {
    Harness h;
    App& a = *h.app;
    a.project.setU8(pf::kLineInMute, 0);
    a.project.usbMute = 0;
    a.markProjectDirty();
    h.lineGen = lineDc;
    const int lineOnly = settle(h);
    CHECK(lineOnly > 1000);
    h.usbGen = usbSine;
    CHECK(settle(h) > lineOnly + 2000);
}

TEST(recorder_usb_source_records_and_monitors) {
    Harness h;
    App& a = *h.app;
    h.lineGen = lineDc;
    h.usbGen = usbSine;
    h.tap(btn::SampleRecorder);
    RecorderScreen& r = *static_cast<RecorderScreen*>(a.ui.screen(kModRecorder));
    for (int i = 0; i < 4; ++i) h.tap(btn::F1);
    CHECK_EQ(a.cfg.recSource, Config::kRecSourceUsb);
    CHECK(r.sourceOk);
    CHECK(a.capture.stereo());
    // Monitor On: USB at unity although the Global Mixer USB is muted; the
    // line stays at its Global Mixer setting (muted).
    const float want = float(kAmp) * trkGain(a.cfg.masterVolume);
    CHECK_NEAR(double(settle(h)), double(want), double(want) * 0.03 + 2.0);
    // Record a moment: the take is the USB stream, stereo (R = -L), not the line.
    h.tap(btn::F8);
    for (int i = 0; i < 40; ++i) {
        h.audio.pump(4);
        h.run(12);
    }
    CHECK(a.capture.active());
    h.tap(btn::F8);
    for (int i = 0; i < 10; ++i) {
        h.audio.pump(2);
        h.run(12);
    }
    CHECK(!a.capture.active());
    const int16_t* L = a.capture.buffer();
    const int16_t* R = a.capture.bufferR();
    CHECK(L != nullptr && R != nullptr);
    int peak = 0;
    bool mirrored = true;
    for (uint32_t i = 0; i < 2000; ++i) {
        peak = std::abs(L[i]) > peak ? std::abs(L[i]) : peak;
        mirrored = mirrored && std::abs(L[i] + R[i]) <= 1;
    }
    CHECK(peak > kAmp / 2);
    CHECK(mirrored);
    // Saved as USB in usb_audio.bin; project.mt's source byte reads Line in.
    a.syncRecorderSettings();
    CHECK_EQ(a.project.recUsb, 1);
    CHECK_EQ(a.project.u8(pf::kRecSource), 0);
}

TEST(recorder_usb_monitor_off) {
    Harness h;
    App& a = *h.app;
    a.project.usbMute = 0;   // heard through the Global Mixer outside the Recorder
    a.markProjectDirty();
    h.usbGen = usbSine;
    CHECK(settle(h) > 1000);
    h.tap(btn::SampleRecorder);
    for (int i = 0; i < 4; ++i) h.tap(btn::F1);
    CHECK_EQ(a.cfg.recSource, Config::kRecSourceUsb);
    h.tap(btn::F7);
    h.tap(btn::F7);
    CHECK(a.inputMonitorMuted);
    CHECK_EQ(settle(h), 0);
    h.tap(btn::Pattern);
    CHECK(settle(h) > 1000);
}

// The USB settings aren't saved: project.mt keeps its stock bytes (the
// Recorder's source byte reads Line in) and no other file is written.
TEST(usb_audio_settings_not_saved) {
    Harness h;
    Project p;
    h.fs.mkdir("P");
    CHECK(saveProjectFile(h.fs, "P/project.mt", p) == FmtStatus::Ok);
    const auto plain = h.fs.files["P/project.mt"];
    const size_t files = h.fs.files.size();
    p.usbMute = 0;
    p.usbLevel = 70;
    p.recUsb = 1;
    CHECK(saveProjectFile(h.fs, "P/project.mt", p) == FmtStatus::Ok);
    CHECK(h.fs.files["P/project.mt"] == plain);
    CHECK_EQ(h.fs.files.size(), files);
}

TEST(project_with_usb_source_reopens_on_usb) {
    Harness h;
    App& a = *h.app;
    a.project.recUsb = 1;
    a.syncRecorderSettings();
    CHECK_EQ(a.cfg.recSource, Config::kRecSourceUsb);
    a.project.recUsb = 0;
    a.syncRecorderSettings();
    CHECK_EQ(a.cfg.recSource, 0);
}
