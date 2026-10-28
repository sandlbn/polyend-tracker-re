// 2.x theme files from the card (/Themes/<name>.txt, docs/CHANGES_2X.md):
// a `key = value` text over one of the stock tables (theme.h). The stock
// themes stay in flash and always work; a file only replaces the active
// table while it is chosen.
//
//   # comment (also after a value: "accent = #FF0000  # red")
//   base = original | monochrome | anniversary | 0 | 1 | 2   (before any colour)
//   <role> = #RRGGBB | #RGB | 0xRRGGBB | RRGGBB               (every slot of the role)
//   slot.N = colour (0..272)   slot.N = 0|1|on|off|true|false (273..276)
//   flag.bottom_fade / valuebar_gradient / cursor_row_gradient / bar_lines = flag
//
// Keys are case-insensitive; later lines win; bad lines are skipped and
// counted. Only a file that cannot be opened fails to load.
#pragma once

#include <cstdint>

#include "hal_api.h"
#include "ui/theme.h"

namespace ot {

constexpr const char* kThemesDir = "Themes";
constexpr uint32_t kThemeFileMax = 8192;   // bytes read; the rest is ignored (truncated)
constexpr int kThemeLineMax = 127;         // longer lines are bad
constexpr int kThemeListMax = 32;          // files listed in the Display theme picker
constexpr int kThemeNameMax = 20;          // file basename, printable ASCII

// Named roles: groups of colour slots (a partition of slots 0..272). The
// first slot of a role is the one the named Theme field reads, where there
// is one; Export theme shows each role with that slot's colour.
struct ThemeRole {
    const char* name;
    const uint16_t* slots;
    uint8_t count;
};
constexpr int kThemeRoleCount = 42;
const ThemeRole& themeRole(int i);
int themeRoleIndex(const char* name);   // case-insensitive; -1: none
const char* themeBaseName(uint8_t base);   // "original" / "monochrome" / "anniversary"

struct ThemeParse {
    uint8_t base = 0;          // stock table the file starts from
    uint16_t badLines = 0;
    bool truncated = false;    // longer than kThemeFileMax
    bool guarded = false;      // unreadable text: roles restored from the base
};

// Parses a whole text into `slots` (kThemeSlotCount words, always all written).
void parseTheme(const char* text, uint32_t len, uint32_t* slots, ThemeParse* out);
// Themes/<name>.txt -> slots. False (slots untouched) when the name is not
// valid or the file does not open.
bool loadThemeFile(IStorage& fs, const char* name, uint32_t* slots, ThemeParse* out);

// 1..kThemeNameMax printable ASCII characters, none of / \ :.
bool themeNameValid(const char* name);
// Basenames of Themes/*.txt (extension in any case), sorted case-insensitively;
// at most `max`, the first ones in that order. Returns the count.
int listThemeFiles(IStorage& fs, char (*names)[kThemeNameMax + 1], int max);

// Writes Themes/<name>.txt: base line, every role as a comment, then the
// slots that differ from the base. Never overwrites.
enum class ThemeWrite : uint8_t { kOk = 0, kExists, kFailed };
ThemeWrite writeThemeFile(IStorage& fs, const char* name, const uint32_t* slots, uint8_t base);

}  // namespace ot
