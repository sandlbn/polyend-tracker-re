// 2.x theme files from the card, app side: Config > General > Display theme
// lists the stock themes then /Themes/*.txt, the choice persists in the
// config extension record (v5) and reloads at boot / after USB Storage mode,
// Export theme writes the active theme (docs/CHANGES_2X.md).
#include "test.h"
#include "ui_fakes.h"
#include "nvstore_fake.h"

#include <string>

#include "ui/screens.h"
#include "ui/theme_file.h"

using namespace ot;

namespace {

ConfigScreen& cfgScreen(Harness& h) { return *static_cast<ConfigScreen*>(h.app->ui.screen(kModConfig)); }

constexpr int kGeneral = 1;
constexpr int kThemeRow = 2;
constexpr int kExportRow = 4;

const char* const kNeon =
    "# Neon\nbase = original\nbackground = #06030F\npanel = #140A26\naccent = #FF2BD6\n";
const char* const kPaper = "base = monochrome\nbackground = #F0EEE8\n";

void putFile(FakeStorage& fs, const std::string& path, const std::string& text) {
    fs.mkdir(path.substr(0, path.rfind('/')).c_str());
    fs.addFile(path, std::vector<uint8_t>(text.begin(), text.end()));
}

std::string fileText(FakeStorage& fs, const std::string& path) {
    const auto& v = fs.files[path];
    return std::string(v.begin(), v.end());
}

// Config > General > <row> with the keys.
void gotoGeneralRow(Harness& h, int row) {
    h.tap(btn::Pattern);
    h.tap(btn::Config);   // opens with the category list focused
    ConfigScreen& c = cfgScreen(h);
    for (int i = 0; i < 10 && c.category < kGeneral; ++i) h.tap(btn::Down);
    for (int i = 0; i < 10 && c.category > kGeneral; ++i) h.tap(btn::Up);
    h.tap(btn::Right);
    for (int i = 0; i < 30 && c.option < row; ++i) h.tap(btn::Down);
    for (int i = 0; i < 30 && c.option > row; ++i) h.tap(btn::Up);
    CHECK_EQ(c.option, row);
}

// Opens the Display theme picker and applies entry `pick`.
void pickTheme(Harness& h, int pick) {
    gotoGeneralRow(h, kThemeRow);
    ConfigScreen& c = cfgScreen(h);
    h.tap(btn::F8);   // Enter: the picker
    for (int i = 0; i < 40 && c.picker < pick; ++i) h.tap(btn::Down);
    for (int i = 0; i < 40 && c.picker > pick; ++i) h.tap(btn::Up);
    CHECK_EQ(c.picker, pick);
    h.tap(btn::F8);   // Apply
}

bool sameDl(const std::vector<std::string>& a, const std::vector<std::string>& b) { return a == b; }

}  // namespace

TEST(theme_picker_lists_stock_then_card_themes) {
    Harness h;
    putFile(h.fs, "Themes/Neon.txt", kNeon);
    putFile(h.fs, "Themes/amber.txt", "base = original\n");
    putFile(h.fs, "Themes/readme.md", "");
    gotoGeneralRow(h, kThemeRow);
    ConfigScreen& c = cfgScreen(h);
    // The row keeps its stock model: three entries without the card.
    ConfigScreen::OptionInfo o;
    CHECK(ConfigScreen::optionInfo(kGeneral, kThemeRow, &o));
    CHECK_EQ(o.count, 3);
    h.tap(btn::F8);
    CHECK(c.editing);
    CHECK_EQ(c.picker, 0);
    CHECK_EQ(ConfigScreen::themeCount(), 2);
    CHECK_STR(ConfigScreen::themeName(0), "amber");
    CHECK_STR(ConfigScreen::themeName(1), "Neon");
    for (int i = 0; i < 8; ++i) h.tap(btn::Down);
    CHECK_EQ(c.picker, 4);   // Original, Monochrome, Anniversary, amber, Neon
    h.run(40);
    CHECK(h.disp.lastHasText("Anniversary"));
    CHECK(h.disp.lastHasText("amber"));
    CHECK(h.disp.lastHasText("Neon"));
    CHECK(!h.disp.lastHasText("readme"));
    h.tap(btn::F7);   // Cancel: nothing changes
    CHECK(!c.editing);
    CHECK(!h.app->customThemeActive());
    // Without a Themes folder: the stock three only.
    Harness h2;
    gotoGeneralRow(h2, kThemeRow);
    h2.tap(btn::F8);
    for (int i = 0; i < 8; ++i) h2.tap(btn::Down);
    CHECK_EQ(cfgScreen(h2).picker, 2);
    CHECK_EQ(ConfigScreen::themeCount(), 0);
}

TEST(apply_card_theme_activates_table) {
    Harness h;
    App& a = *h.app;
    putFile(h.fs, "Themes/Neon.txt", kNeon);
    pickTheme(h, 3);
    CHECK(!cfgScreen(h).editing);
    CHECK(a.customThemeActive());
    CHECK_STR(a.customThemeName(), "Neon");
    CHECK_STR(a.cfg.themeFile, "Neon");
    CHECK_EQ(a.cfg.theme, 0);
    CHECK_EQ(a.ui.th().bg, 0x06030Fu);
    CHECK_EQ(a.ui.th().accent, 0xFF2BD6u);
    CHECK_EQ(a.ui.th().slot(slot::kList + 3), 0x140A26u);
    CHECK_STR(a.ui.th().name, "Neon");
    // The picker opens on the active entry.
    h.tap(btn::F8);
    CHECK_EQ(cfgScreen(h).picker, 3);
    h.tap(btn::F7);
    // A Monochrome-based file sets the stock byte to its base.
    putFile(h.fs, "Themes/Paper.txt", kPaper);
    pickTheme(h, 4);   // Neon, Paper
    CHECK_STR(a.customThemeName(), "Paper");
    CHECK_EQ(a.cfg.theme, 1);
    CHECK_EQ(a.ui.th().bg, 0xF0EEE8u);
    CHECK_EQ(a.ui.th().slot(slot::kTitle + 1), theme(1).slot(slot::kTitle + 1));
}

TEST(apply_card_theme_open_failure_keeps_previous) {
    Harness h;
    App& a = *h.app;
    a.cfg.theme = 2;
    putFile(h.fs, "Themes/Neon.txt", kNeon);
    gotoGeneralRow(h, kThemeRow);
    ConfigScreen& c = cfgScreen(h);
    h.tap(btn::F8);
    CHECK_EQ(c.picker, 2);
    h.tap(btn::Down);
    h.fs.files.erase("Themes/Neon.txt");   // gone before Apply
    h.tap(btn::F8);
    CHECK(!a.customThemeActive());
    CHECK_EQ(a.cfg.theme, 2);
    CHECK_EQ(a.cfg.themeFile[0], 0);
    CHECK(&a.ui.th() == &theme(2));
    CHECK(a.ui.notice() != nullptr);
    CHECK_STR(a.ui.notice(), "Cannot read Themes/Neon.txt");
}

TEST(apply_stock_theme_clears_custom) {
    Harness h;
    App& a = *h.app;
    putFile(h.fs, "Themes/Neon.txt", kNeon);
    CHECK(a.loadCustomTheme("Neon"));
    CHECK(a.customThemeActive());
    pickTheme(h, 1);
    CHECK(!a.customThemeActive());
    CHECK_EQ(a.cfg.theme, 1);
    CHECK_EQ(a.cfg.themeFile[0], 0);
    CHECK(&a.ui.th() == &theme(1));
    // Picking the stock entry the custom theme is based on also clears it.
    CHECK(a.loadCustomTheme("Neon"));
    pickTheme(h, 0);
    CHECK(!a.customThemeActive());
    CHECK(&a.ui.th() == &theme(0));
}

TEST(custom_theme_persists_in_ext_v5_and_reloads_at_boot) {
    FakeNvStore nv;
    Harness h(false);
    putFile(h.fs, "Themes/Paper.txt", kPaper);
    h.app->setNvStore(&nv);
    h.app->boot();
    CHECK(h.app->loadCustomTheme("Paper"));
    h.app->markConfigDirty();
    CHECK(h.app->saveConfig());
    const uint8_t* x = nv.mem.data() + Config::kExtOffset;
    CHECK_BYTES(x, 'O', 'T', 'X', 5, 27);
    CHECK_EQ(Config::kExtSize, 33u);
    CHECK(std::memcmp(x + 11, "Paper\0", 6) == 0);
    CHECK_EQ(nv.mem[0xD5], 1);   // the stock byte holds the base
    Harness h2(false);
    h2.fs.files = h.fs.files;
    h2.fs.dirs = h.fs.dirs;
    h2.app->setNvStore(&nv);
    h2.app->boot();
    CHECK(h2.app->customThemeActive());
    CHECK_STR(h2.app->customThemeName(), "Paper");
    CHECK_EQ(h2.app->cfg.theme, 1);
    CHECK_EQ(h2.app->ui.th().bg, 0xF0EEE8u);
    // The file kept on the card (no INvStore) carries it too.
    Harness h3(false);
    h3.fs.files = h.fs.files;
    h3.fs.dirs = h.fs.dirs;
    h3.app->boot();
    CHECK(h3.app->loadCustomTheme("Paper"));
    CHECK(h3.app->saveConfig());
    Harness h4(false);
    h4.fs.files = h3.fs.files;
    h4.fs.dirs = h3.fs.dirs;
    h4.app->boot();
    CHECK(h4.app->customThemeActive());
    CHECK_STR(h4.app->cfg.themeFile, "Paper");
}

TEST(boot_without_theme_file_falls_back_to_base_and_keeps_name) {
    FakeNvStore nv;
    Harness h(false);
    putFile(h.fs, "Themes/Paper.txt", kPaper);
    h.app->setNvStore(&nv);
    h.app->boot();
    CHECK(h.app->loadCustomTheme("Paper"));
    CHECK(h.app->saveConfig());
    Harness h2(false);   // a card without the file
    h2.app->setNvStore(&nv);
    h2.app->boot();
    CHECK(!h2.app->customThemeActive());
    CHECK_EQ(h2.app->cfg.theme, 1);
    CHECK(&h2.app->ui.th() == &theme(1));
    CHECK_STR(h2.app->cfg.themeFile, "Paper");   // kept: the next card may have it
    h2.app->markConfigDirty();
    CHECK(h2.app->saveConfig());
    CHECK(std::memcmp(nv.mem.data() + Config::kExtOffset + 11, "Paper\0", 6) == 0);
    // The row shows the base while the file is missing.
    char t[48];
    cfgScreen(h2).optionText(kGeneral, kThemeRow, t, sizeof t);
    CHECK_STR(t, "Monochrome");
}

TEST(usb_storage_exit_reloads_theme) {
    Harness h;
    App& a = *h.app;
    putFile(h.fs, "Themes/Neon.txt", kNeon);
    CHECK(a.loadCustomTheme("Neon"));
    CHECK(a.enterUsbStorage() == App::UsbEnter::kOk);
    CHECK(a.exportTheme() == App::ThemeExport::kUnavailable);   // the card is the computer's
    h.run(40);
    // The computer edits the file.
    h.fs.files["Themes/Neon.txt"] = std::vector<uint8_t>();
    const std::string t = std::string(kNeon) + "background = #010101\n";
    h.fs.files["Themes/Neon.txt"].assign(t.begin(), t.end());
    CHECK_EQ(a.ui.th().bg, 0x06030Fu);   // nothing read while the card is away
    a.exitUsbStorage();
    CHECK(a.customThemeActive());
    CHECK_EQ(a.ui.th().bg, 0x010101u);
    CHECK_EQ(h.fs.lockedAccess, 0);
}

TEST(reset_config_clears_custom_theme) {
    Harness h;
    App& a = *h.app;
    putFile(h.fs, "Themes/Paper.txt", kPaper);
    CHECK(a.loadCustomTheme("Paper"));
    h.tap(btn::Config);
    ConfigScreen& c = cfgScreen(h);
    while (c.category < 5) h.tap(btn::Down);   // Firmware
    h.tap(btn::Right);
    for (int i = 0; i < 3; ++i) h.tap(btn::Down);   // Reset config
    h.tap(btn::F8);
    CHECK(a.ui.askOpen());
    h.tap(btn::F8);
    CHECK(!a.customThemeActive());
    CHECK_EQ(a.cfg.theme, 0);
    CHECK_EQ(a.cfg.themeFile[0], 0);
    CHECK(&a.ui.th() == &theme(0));
}

TEST(theme_row_value_shows_custom_name) {
    Harness h;
    App& a = *h.app;
    putFile(h.fs, "Themes/Neon.txt", kNeon);
    ConfigScreen& c = cfgScreen(h);
    char t[48];
    c.optionText(kGeneral, kThemeRow, t, sizeof t);
    CHECK_STR(t, "Original");
    CHECK(a.loadCustomTheme("Neon"));
    c.optionText(kGeneral, kThemeRow, t, sizeof t);
    CHECK_STR(t, "Neon");
    CHECK_EQ(c.optionIndex(kGeneral, kThemeRow), 0);   // the stock model: its base
    gotoGeneralRow(h, kThemeRow);
    h.run(40);
    CHECK(h.disp.lastHasText("Neon"));
    a.clearCustomTheme();
    c.optionText(kGeneral, kThemeRow, t, sizeof t);
    CHECK_STR(t, "Original");
}

TEST(ext_record_v4_reads_as_no_custom_theme) {
    // A v4 record (2.2.x): six bytes, no theme name.
    const uint8_t v4[] = {'O', 'T', 'X', 4, 6, 1, 5, 2, 3, 1, 0, uint8_t(1 + 5 + 2 + 3 + 1 + 0)};
    Config c;
    std::snprintf(c.themeFile, sizeof c.themeFile, "Old");
    CHECK(c.fromExt(v4, sizeof v4));
    CHECK_EQ(c.themeFile[0], 0);
    CHECK_EQ(c.cpuMeter, 1);
    CHECK_EQ(c.synthVoices, 5);
    CHECK_EQ(c.exportRate, 0);
    // v5 round trip; an older build reads its six bytes only.
    std::snprintf(c.themeFile, sizeof c.themeFile, "My theme");
    uint8_t x[Config::kExtSize];
    c.toExt(x);
    CHECK_EQ(x[3], 5);
    CHECK_EQ(x[4], 27);
    Config d;
    CHECK(d.fromExt(x, sizeof x));
    CHECK_STR(d.themeFile, "My theme");
    CHECK_EQ(d.synthVoices, 5);
    Config e;
    CHECK(!e.fromExt(x, 12));   // cut record: refused
    CHECK_EQ(e.themeFile[0], 0);
    // Defaults and setDefaults: no theme file.
    d.setDefaults();
    CHECK_EQ(d.themeFile[0], 0);
}

TEST(ext_v5_name_sanitised) {
    auto withName = [](const char* raw, int n) {
        Config c;
        uint8_t x[Config::kExtSize];
        c.toExt(x);
        std::memset(x + 11, 0, 21);
        std::memcpy(x + 11, raw, size_t(n));
        uint8_t sum = 0;
        for (uint32_t i = 0; i < Config::kExtPayload; ++i) sum = uint8_t(sum + x[5 + i]);
        x[5 + Config::kExtPayload] = sum;
        Config d;
        CHECK(d.fromExt(x, sizeof x));
        return std::string(d.themeFile);
    };
    CHECK(withName("Neon", 4) == "Neon");
    CHECK(withName("a/b", 3).empty());
    CHECK(withName("a\\b", 3).empty());
    CHECK(withName("a:b", 3).empty());
    CHECK(withName("a\x01" "b", 3).empty());
    CHECK(withName("caf\xC3\xA9", 5).empty());
    CHECK(withName("ABCDEFGHIJKLMNOPQRSTU", 21).empty());   // no terminator in 21 bytes
    CHECK(withName("ABCDEFGHIJKLMNOPQRST", 20) == "ABCDEFGHIJKLMNOPQRST");
    Config c;
    std::snprintf(c.themeFile, sizeof c.themeFile, "x/y");
    c.sanitize();
    CHECK_EQ(c.themeFile[0], 0);
}

TEST(export_theme_action_writes_file_and_refuses_overwrite) {
    Harness h;
    App& a = *h.app;
    ConfigScreen::OptionInfo o;
    CHECK(ConfigScreen::optionInfo(kGeneral, kExportRow, &o));
    CHECK_STR(o.name, "Export theme");
    CHECK_EQ(o.kind, 7);
    CHECK(o.help != nullptr);
    gotoGeneralRow(h, kExportRow);
    h.tap(btn::F8);
    CHECK(h.fs.files.count("Themes/Original.txt"));
    CHECK(h.fs.dirs.count("Themes"));
    const std::string t = fileText(h.fs, "Themes/Original.txt");
    CHECK(t.find("base = original\n") != std::string::npos);
    CHECK(t.find("# accent = #CD1818\n") != std::string::npos);
    CHECK_STR(a.ui.notice(), "Saved Themes/Original.txt");
    // Again: refused, the file stays.
    h.fs.files["Themes/Original.txt"] = {'x'};
    h.tap(btn::F8);
    CHECK_STR(a.ui.notice(), "Themes/Original.txt exists");
    CHECK_EQ(h.fs.files["Themes/Original.txt"].size(), 1u);
    // The active custom theme exports under its own name (refused: its file exists).
    putFile(h.fs, "Themes/Neon.txt", kNeon);
    CHECK(a.loadCustomTheme("Neon"));
    CHECK(a.exportTheme() == App::ThemeExport::kExists);
    // The exported file loads back as the same table.
    a.clearCustomTheme();
    a.cfg.theme = 2;
    CHECK(a.exportTheme() == App::ThemeExport::kOk);
    CHECK(a.loadCustomTheme("Anniversary"));
    for (int i = 0; i < kThemeSlotCount; ++i) CHECK_EQ(a.ui.th().slot(i), theme(2).slot(i));
}

TEST(no_theme_files_stock_output_identical) {
    // Without theme files the active theme is the stock table itself, and a
    // file with only a base line draws exactly like that stock theme.
    for (uint8_t t = 0; t < kThemeCount; ++t) {
        Harness h;
        App& a = *h.app;
        a.cfg.theme = t;
        CHECK(&a.ui.th() == &theme(t));
        // The card is the same for both passes (the Loader lists its folders).
        putFile(h.fs, "Themes/Same.txt", std::string("base = ") + themeBaseName(t) + "\n");
        CHECK(!a.customThemeActive());
        std::vector<std::string> shots[2];
        for (int k = 0; k < 2; ++k) {
            if (k == 1) {
                CHECK(a.loadCustomTheme("Same"));
                CHECK(&a.ui.th() != &theme(t));
            }
            for (uint8_t key : {btn::Pattern, btn::Config, btn::Master, btn::SampleLoader}) {
                h.tap(key);
                a.ui.dirty();
                h.run(40);
                shots[k].insert(shots[k].end(), h.disp.last.begin(), h.disp.last.end());
            }
        }
        CHECK(!shots[0].empty());
        CHECK(sameDl(shots[0], shots[1]));
    }
}
