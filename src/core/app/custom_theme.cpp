// 2.x theme files from the card (ui/theme_file.h): the app side of the
// Display theme choice (see App::loadCustomTheme in app.h).
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "sdram_bss.h"
#include "ui/theme_file.h"

namespace ot {

namespace {
// The one loaded theme file (main loop only; Ui::th() reads it per draw call).
OT_SDRAM_BSS uint32_t g_customSlots[kThemeSlotCount];
}  // namespace

bool App::loadCustomTheme(const char* name) {
    if (usbActive_ || !hal_.storage || !themeNameValid(name)) return false;
    ThemeParse r;
    if (!loadThemeFile(fs(), name, g_customSlots, &r)) return false;
    std::snprintf(themeName_, sizeof(themeName_), "%s", name);
    themeView_ = themeView(themeName_, g_customSlots);
    themeActive_ = true;
    cfg.theme = r.base;
    std::snprintf(cfg.themeFile, sizeof(cfg.themeFile), "%s", name);
    ui.dirty();
    return true;
}

void App::clearCustomTheme() {
    themeActive_ = false;
    std::memset(cfg.themeFile, 0, sizeof(cfg.themeFile));
    ui.dirty();
}

// boot(): the chosen file again (the card may have changed); without it the
// base theme shows and the name stays for the next card.
void App::restoreCustomTheme() {
    themeActive_ = false;
    if (!cfg.themeFile[0]) return;
    char name[sizeof(cfg.themeFile)];
    std::memcpy(name, cfg.themeFile, sizeof(name));
    loadCustomTheme(name);
}

App::ThemeExport App::exportTheme() {
    if (usbActive_ || !hal_.storage) return ThemeExport::kUnavailable;
    const uint32_t* slots = themeActive_ ? g_customSlots : theme(cfg.theme).slots;
    switch (writeThemeFile(fs(), activeThemeName(), slots, cfg.theme)) {
        case ThemeWrite::kOk: return ThemeExport::kOk;
        case ThemeWrite::kExists: return ThemeExport::kExists;
        default: return ThemeExport::kFailed;
    }
}

}  // namespace ot
