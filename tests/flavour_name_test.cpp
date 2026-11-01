// The image's flavour name (docs/PROFILES.md "The flavours"): the profile's
// display_name (tools/profile.py -> OT_PROFILE_DISPLAY_NAME in ot_profile.h)
// is shown with the version on the boot splash, in Config > Firmware >
// Current version and on the Licenses page. Every profile's host-test run
// checks its own name, so the seven runs cover all seven flavours.
#include <cstring>
#include <string>

#include "test.h"
#include "ui_fakes.h"

#include "ot_profile.h"
#include "ot_version.h"
#include "ui/gfx.h"
#include "ui/screens.h"
#include "ui/widgets.h"

using namespace ot;

namespace {

struct Flavour {
    const char* profile;
    const char* name;
};
const Flavour kFlavours[] = {
    {"classic", "Classic"}, {"idm", "IDM"},   {"techno", "Techno"}, {"mini", "Mini"},
    {"nofm", "No-FM"},      {"full", "Full"}, {"core", "Core"},
};

// The flavour name this test expects for the profile it was built with
// (nullptr for a custom profile: profile.py's default, the name capitalised).
const char* expectedName() {
    for (const Flavour& f : kFlavours)
        if (std::strcmp(f.profile, OT_PROFILE_NAME) == 0) return f.name;
    return nullptr;
}

std::string expectedVersionLine() {
    const char* n = expectedName();
    return std::string("OpenTracker ") + OT_VERSION + " " + (n ? n : OT_PROFILE_DISPLAY_NAME);
}

ConfigScreen& cfgScreen(Harness& h) { return *static_cast<ConfigScreen*>(h.app->ui.screen(kModConfig)); }

constexpr int kFirmwareCat = 5, kCurrentVersionOpt = 1;

}  // namespace

TEST(profile_display_name_is_the_flavour) {
    const char* want = expectedName();
    if (want) CHECK(std::strcmp(OT_PROFILE_DISPLAY_NAME, want) == 0);
    CHECK(std::strlen(OT_PROFILE_DISPLAY_NAME) >= 1 && std::strlen(OT_PROFILE_DISPLAY_NAME) <= 10);
    CHECK(expectedVersionLine() == OT_VERSION_LINE);
    CHECK(std::string("v") + OT_VERSION + " " + OT_PROFILE_DISPLAY_NAME == OT_SPLASH_VERSION);
    // The two flavours named in the owner request, by their literal strings.
    if (std::strcmp(OT_PROFILE_NAME, "techno") == 0) CHECK(std::string(OT_VERSION_LINE).find(" Techno") != std::string::npos);
    if (std::strcmp(OT_PROFILE_NAME, "idm") == 0) CHECK(std::string(OT_VERSION_LINE).find(" IDM") != std::string::npos);
}

TEST(config_current_version_names_the_flavour) {
    Harness h;
    h.tap(btn::Config);
    ConfigScreen& c = cfgScreen(h);
    char t[64];
    ConfigScreen::OptionInfo info{};
    CHECK(ConfigScreen::optionInfo(kFirmwareCat, kCurrentVersionOpt, &info));
    CHECK(std::strcmp(info.name, "Current version") == 0);
    c.optionText(kFirmwareCat, kCurrentVersionOpt, t, sizeof t);
    CHECK(expectedVersionLine() == t);
    // On screen: Config > Firmware, the value right-aligned on its row.
    for (int i = 0; i < kFirmwareCat; ++i) h.tap(btn::Down);
    CHECK(c.category == kFirmwareCat);
    h.run(50);
    CHECK(h.disp.lastHasText(expectedVersionLine().c_str()));
    CHECK(h.disp.lastHasText("Current version"));
    // The value clears its label (names at x 214, values end at valueRight)
    // with the widest name profile.py allows: 10 of its widest glyph, in both
    // display font sets.
    const char* const allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+.-";
    for (uint8_t font = 0; font < 2; ++font) {
        h.app->cfg.font = font;
        h.app->ui.dirty();
        h.run(50);
        CHECK(h.disp.lastHasText(expectedVersionLine().c_str()));
        setFontSet(font);   // measure in this set
        char widest[2] = {allowed[0], 0};
        for (const char* p = allowed; *p; ++p) {
            const char ch[2] = {*p, 0};
            if (textWidth(kFont1, ch) > textWidth(kFont1, widest)) widest[0] = *p;
        }
        const std::string longest = std::string("OpenTracker ") + OT_VERSION + " " + std::string(10, widest[0]);
        CHECK(214 + textWidth(kFont1, "Current version") + 16 + textWidth(kFont1, longest.c_str()) <=
              ConfigScreen::valueRight(kFirmwareCat));
    }
    h.app->cfg.font = 0;
    setFontSet(0);
}

TEST(licenses_page_names_the_flavour) {
    Harness h;
    h.tap(btn::Config);
    for (int i = 0; i < 6; ++i) h.tap(btn::Down);   // Info
    h.tap(btn::Right);                                // Licenses
    h.tap(btn::F8);
    h.run(50);
    CHECK(h.disp.lastHasText(expectedVersionLine().c_str()));
}

TEST(boot_splash_names_the_flavour) {
    Harness h(false);
    h.app->ui.progress("Opening last project", 30);   // before any screen: the splash
    CHECK(h.disp.lastHasText("Opening last project"));
    const char* n = expectedName();
    const std::string want = std::string("v") + OT_VERSION + " " + (n ? n : OT_PROFILE_DISPLAY_NAME);
    bool found = false;
    for (const std::string& l : h.disp.last)
        if (l.rfind("TEXT 11 466 ", 0) == 0 && l.find(want) != std::string::npos) found = true;
    CHECK(found);
}
