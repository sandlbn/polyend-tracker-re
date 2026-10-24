// Config "Font" -> display font set (spec 05 §3.6) and Config "Snap to zero"
// -> project byte 0x602 -> playback flag + marker indicator (spec 13 §H.2).
#include "test.h"
#include "ui_fakes.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "app/project_fields.h"
#include "audio/zero_snap.h"
#include "formats/wav.h"
#include "ui/gfx.h"
#include "ui/screens.h"

using namespace ot;

static int framesTextWithFont(Harness& h, unsigned font) {
    const std::string tag = " f" + std::to_string(font) + " ";
    int n = 0;
    for (const std::string& s : h.disp.last)
        if (s.rfind("TEXT ", 0) == 0 && s.find(tag) != std::string::npos) ++n;
    return n;
}

TEST(font_role_mapping_per_set) {
    // Set 0: roles are the antialiased ROM fonts themselves.
    CHECK_EQ(romFontFor(kFontSmall, 0), 26);
    CHECK_EQ(romFontFor(kFontText, 0), 27);
    CHECK_EQ(romFontFor(kFontLarge, 0), 28);
    CHECK_EQ(romFontFor(kFontTitle, 0), 29);
    // Set 1: bitmap ROM fonts of near-equal height; the mono grid font never changes.
    CHECK_EQ(romFontFor(kFontSmall, 1), 21);
    CHECK_EQ(romFontFor(kFontText, 1), 22);
    CHECK_EQ(romFontFor(kFontLarge, 1), 23);
    CHECK_EQ(romFontFor(kFontTitle, 1), 24);
    CHECK_EQ(romFontFor(kFontMono, 0), kFontMono);
    CHECK_EQ(romFontFor(kFontMono, 1), kFontMono);
    setFontSet(7);   // out of range -> set 0
    CHECK_EQ(fontSet(), 0);
}

TEST(config_font_option_switches_all_text) {
    Harness h;
    App& a = *h.app;
    CHECK_EQ(fontSet(), 0);
    h.tap(btn::Config);
    ConfigScreen& c = *static_cast<ConfigScreen*>(a.ui.screen(kModConfig));
    h.tap(btn::Down);   // General
    h.tap(btn::Right);
    for (int i = 0; i < 3; ++i) h.tap(btn::Down);   // Font
    CHECK_EQ(c.option, 3);
    h.tap(btn::F8);
    h.tap(btn::Down);
    h.tap(btn::F8);     // Apply
    CHECK_EQ(a.cfg.font, 1);
    CHECK_EQ(fontSet(), 1);
    h.run(50);
    CHECK(framesTextWithFont(h, 22) > 0);
    CHECK_EQ(framesTextWithFont(h, 27), 0);
    CHECK_EQ(framesTextWithFont(h, 26), 0);
    // Applied at boot from the stored config.
    a.cfg.font = 0;
    a.applySequencerSettings();
    CHECK_EQ(fontSet(), 0);
    a.cfg.font = 1;
    a.applySequencerSettings();
    CHECK_EQ(fontSet(), 1);
    setFontSet(0);
}

TEST(config_snap_to_zero_drives_playback_and_markers) {
    Harness h;
    App& a = *h.app;
    std::vector<int16_t> s(4000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(10000 * std::sin(double(i) * 0.01));
    std::vector<uint8_t> wav(s.size() * 2 + 64);
    wav.resize(wavEncode(s.data(), uint32_t(s.size()), 1, wav.data(), wav.size()));
    h.fs.files["s.wav"] = wav;
    CHECK(a.importSample("s.wav", 0));
    a.setCurrentInstrument(0);
    a.instruments[0].startPoint = 10700;   // 10700/65535*4000 = 653 frames; crossing at 628
    h.tap(btn::SamplePlayback);
    PlaybackScreen& p = *static_cast<PlaybackScreen*>(a.ui.screen(kModPlayback));
    CHECK_EQ(p.zeroMask() & 1, 0);

    h.tap(btn::Config);
    ConfigScreen& c = *static_cast<ConfigScreen*>(a.ui.screen(kModConfig));
    CHECK_EQ(c.category, 0);   // Project settings
    h.tap(btn::Right);
    for (int i = 0; i < 9; ++i) h.tap(btn::Down);   // Snap to zero
    CHECK_EQ(c.option, 9);
    h.tap(btn::F8);
    h.tap(btn::Down);
    h.tap(btn::F8);     // Apply
    CHECK_EQ(a.project.u8(pf::kSnapToZero), 1);
    h.audio.pump(1);
    CHECK(audio::gSnapToZero);
    CHECK_EQ(p.zeroMask() & 1, 1);
    CHECK_EQ(a.instruments[0].startPoint, 10700);   // stored markers unchanged
    // The selected marker turns green on a zero crossing too (manual p.123 /
    // 125: "red but will change to green"; MANUAL_AUDIT_2026-10-23 #13).
    p.sel = 1;
    CHECK_EQ(p.markerColour(0), a.ui.th().good);

    h.tap(btn::F8);
    h.tap(btn::Up);
    h.tap(btn::F8);     // back to Off
    CHECK_EQ(a.project.u8(pf::kSnapToZero), 0);
    h.audio.pump(1);
    CHECK(!audio::gSnapToZero);
    CHECK_EQ(p.zeroMask() & 1, 0);
    CHECK_EQ(p.markerColour(0), a.ui.th().accent);   // selected, no crossing: red
}
