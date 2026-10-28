// 2.x theme files from the card (/Themes, docs/CHANGES_2X.md): the stock
// tables stay bit-identical, the 42 named roles partition slots 0..272, and
// the `key = value` parser / lister / exporter (ui/theme_file.h).
#include "test.h"
#include "mem_storage.h"

#include <string>

#include "formats/crc32.h"
#include "ui/theme.h"
#include "ui/theme_file.h"

using namespace ot;

namespace {

uint32_t gSlots[kThemeSlotCount];

ThemeParse parse(const std::string& s, uint32_t* slots = gSlots) {
    ThemeParse r;
    parseTheme(s.data(), uint32_t(s.size()), slots, &r);
    return r;
}

bool sameAs(const uint32_t* s, uint8_t stock) {
    for (int i = 0; i < kThemeSlotCount; ++i)
        if (s[i] != theme(stock).slots[i]) return false;
    return true;
}

int roleOf(int slotIndex) {
    for (int r = 0; r < kThemeRoleCount; ++r)
        for (int k = 0; k < themeRole(r).count; ++k)
            if (themeRole(r).slots[k] == slotIndex) return r;
    return -1;
}

void putFile(MemStorage& fs, const std::string& path, const std::string& text) {
    fs.files[path] = std::vector<uint8_t>(text.begin(), text.end());
}

std::string fileText(MemStorage& fs, const std::string& path) {
    const auto& v = fs.files[path];
    return std::string(v.begin(), v.end());
}

}  // namespace

TEST(stock_tables_bit_identical) {
    // CRC-32 of the 277 little-endian words of each table (spec 20 §1.4),
    // taken before theme files existed: the stock themes never change.
    const uint32_t want[kThemeCount] = {0x32CBDEE3u, 0xE9D68DB5u, 0x5CE44B85u};
    for (uint8_t t = 0; t < kThemeCount; ++t) {
        uint8_t b[kThemeSlotCount * 4];
        for (int i = 0; i < kThemeSlotCount; ++i)
            for (int k = 0; k < 4; ++k) b[4 * i + k] = uint8_t(theme(t).slots[i] >> (8 * k));
        CHECK_EQ(crc32(b, sizeof(b)), want[t]);
    }
    CHECK_STR(theme(0).name, "Original");
    CHECK_STR(theme(1).name, "Monochrome");
    CHECK_STR(theme(2).name, "Anniversary");
}

TEST(role_table_is_exact_partition) {
    CHECK_EQ(kThemeRoleCount, 42);
    int seen[kThemeSlotCount] = {};
    for (int r = 0; r < kThemeRoleCount; ++r) {
        const ThemeRole& role = themeRole(r);
        CHECK(role.count > 0);
        CHECK_EQ(themeRoleIndex(role.name), r);
        for (int k = 0; k < role.count; ++k) {
            CHECK(role.slots[k] < slot::kFlagBottomFade);
            if (role.slots[k] < kThemeSlotCount) ++seen[role.slots[k]];
        }
    }
    for (int i = 0; i < slot::kFlagBottomFade; ++i) CHECK_EQ(seen[i], 1);
    for (int i = slot::kFlagBottomFade; i < kThemeSlotCount; ++i) CHECK_EQ(seen[i], 0);
    CHECK_EQ(themeRoleIndex("ACCENT"), themeRoleIndex("accent"));
    CHECK_EQ(themeRoleIndex("nope"), -1);
}

TEST(role_slots_match_spec20) {
    // The named Theme fields (theme.cpp makeTheme) sit in the role of that
    // name, and each role's first slot is the one the field reads.
    struct W {
        const char* role;
        int slot;
    } ws[] = {
        {"background", slot::kFrameClear}, {"text", slot::kList + 4},     {"dim", slot::kGrey},
        {"label_bg", slot::kBottomLabel + 1}, {"panel2", slot::kProgress}, {"panel", slot::kList + 3},
        {"accent", slot::kFocusFrame},     {"alert", slot::kEmptyPanel + 2}, {"good", slot::kGreen},
        {"yellow", slot::kYellow},         {"orange", slot::kOrange},     {"cyan", slot::kCyan},
        {"grid_lines", slot::kGridLine},   {"note", slot::kGrid + 2},     {"instrument", slot::kGrid + 3},
        {"fx1", slot::kGrid + 4},          {"fx2", slot::kGrid + 5},      {"note_empty", slot::kGrid + 6},
        {"fx2_empty", slot::kGrid + 9},    {"title_text", slot::kTitle},  {"title_bg", slot::kTitle + 1},
        {"label_text", slot::kBottomLabel}, {"strip", slot::kStrip + 1},  {"wave", slot::kWave},
        {"grid_cursor_row", slot::kGridCursorRow}, {"grid_cursor_text", slot::kGridCursorText},
    };
    for (const W& w : ws) {
        const int r = themeRoleIndex(w.role);
        CHECK(r >= 0);
        if (r < 0) continue;
        CHECK_EQ(roleOf(w.slot), r);
        CHECK_EQ(int(themeRole(r).slots[0]), w.slot);
    }
}

TEST(parse_empty_is_original) {
    for (uint32_t& s : gSlots) s = 0xABCDEFu;
    ThemeParse r = parse("");
    CHECK(sameAs(gSlots, 0));
    CHECK_EQ(r.base, 0);
    CHECK_EQ(r.badLines, 0);
    CHECK(!r.truncated);
    CHECK(!r.guarded);
    r = parse("# only a comment\n\n");
    CHECK(sameAs(gSlots, 0));
    CHECK_EQ(r.badLines, 0);
}

TEST(parse_base_each_stock) {
    const char* names[] = {"base = original", "base = monochrome", "base = anniversary",
                           "base=0",          "BASE = Monochrome", "base = 2"};
    const uint8_t want[] = {0, 1, 2, 0, 1, 2};
    for (int i = 0; i < 6; ++i) {
        ThemeParse r = parse(names[i]);
        CHECK_EQ(r.base, want[i]);
        CHECK(sameAs(gSlots, want[i]));
        CHECK_EQ(r.badLines, 0);
    }
    ThemeParse r = parse("base = 3\n");
    CHECK_EQ(r.badLines, 1);
    CHECK(sameAs(gSlots, 0));
    CHECK_STR(themeBaseName(0), "original");
    CHECK_STR(themeBaseName(1), "monochrome");
    CHECK_STR(themeBaseName(2), "anniversary");
}

TEST(base_after_colour_ignored) {
    ThemeParse r = parse("accent = #123456\nbase = monochrome\n");
    CHECK_EQ(r.base, 0);
    CHECK_EQ(r.badLines, 1);
    CHECK_EQ(gSlots[slot::kFocusFrame], 0x123456u);
    CHECK_EQ(gSlots[slot::kFrameClear], theme(0).slots[slot::kFrameClear]);
    // A base after a flag line is ignored too; base twice before: the last wins.
    r = parse("flag.bar_lines = 0\nbase = 1\n");
    CHECK_EQ(r.base, 0);
    CHECK_EQ(r.badLines, 1);
    r = parse("base = 1\nbase = 2\nwave = #010101\n");
    CHECK_EQ(r.base, 2);
    CHECK_EQ(r.badLines, 0);
    CHECK_EQ(gSlots[slot::kFrameClear], theme(2).slots[slot::kFrameClear]);
    CHECK_EQ(gSlots[slot::kWave], 0x010101u);
}

TEST(colour_syntaxes) {
    struct C {
        const char* v;
        uint32_t want;   // 0xFFFFFFFF: bad line
    } cs[] = {
        {"#ABCDEF", 0xABCDEF}, {"#abcdef", 0xABCDEF}, {"#ABC", 0xAABBCC}, {"#a1c", 0xAA11CC},
        {"0x123456", 0x123456}, {"0X0000FF", 0x0000FF}, {"654321", 0x654321}, {"#12345", 0xFFFFFFFFu},
        {"#GGGGGG", 0xFFFFFFFFu}, {"12345", 0xFFFFFFFFu}, {"#1234567", 0xFFFFFFFFu}, {"red", 0xFFFFFFFFu},
        {"#", 0xFFFFFFFFu},     {"0x", 0xFFFFFFFFu},     {"#ABC DEF", 0xFFFFFFFFu},
    };
    for (const C& c : cs) {
        ThemeParse r = parse(std::string("wave = ") + c.v + "\n");
        if (c.want == 0xFFFFFFFFu) {
            CHECK_EQ(r.badLines, 1);
            CHECK_EQ(gSlots[slot::kWave], theme(0).slots[slot::kWave]);
        } else {
            CHECK_EQ(r.badLines, 0);
            CHECK_EQ(gSlots[slot::kWave], c.want);
        }
    }
}

TEST(role_fills_all_its_slots_only) {
    for (int r = 0; r < kThemeRoleCount; ++r) {
        const ThemeRole& role = themeRole(r);
        ThemeParse p = parse(std::string("base = monochrome\n") + role.name + " = #010203\n");
        CHECK_EQ(p.badLines, 0);
        for (int i = 0; i < kThemeSlotCount; ++i) {
            const bool in = roleOf(i) == r;
            const uint32_t want = in ? 0x010203u : theme(1).slots[i];
            if (gSlots[i] != want && !p.guarded) {
                CHECK_EQ(gSlots[i], want);
                std::printf("       role %s slot %d\n", role.name, i);
            }
        }
    }
}

TEST(later_line_wins) {
    ThemeParse r = parse("accent = #111111\nslot.130 = #222222\n");
    CHECK_EQ(gSlots[slot::kFocusFrame], 0x222222u);
    CHECK_EQ(gSlots[7], 0x111111u);   // the rest of the role keeps the role colour
    r = parse("slot.130 = #222222\naccent = #111111\n");
    CHECK_EQ(gSlots[slot::kFocusFrame], 0x111111u);
    r = parse("accent = #111111\nAccent = #333333\n");
    CHECK_EQ(gSlots[7], 0x333333u);
    CHECK_EQ(r.badLines, 0);
}

TEST(flags_by_name_and_slot) {
    ThemeParse r = parse("flag.bottom_fade = off\nflag.valuebar_gradient = false\nflag.cursor_row_gradient = 0\n"
                         "flag.bar_lines = no\n");
    CHECK_EQ(gSlots[slot::kFlagBottomFade], 0u);
    CHECK_EQ(gSlots[slot::kFlagValueBarGrad], 0u);
    CHECK_EQ(gSlots[slot::kFlagCursorRowGrad], 0u);
    CHECK_EQ(gSlots[slot::kFlagBarLines], 1u);   // "no" is not a flag value
    CHECK_EQ(r.badLines, 1);
    r = parse("base = anniversary\nslot.273 = on\nslot.274 = TRUE\nslot.275 = 1\nflag.bar_lines = On\n");
    CHECK_EQ(r.badLines, 0);
    for (int i = slot::kFlagBottomFade; i <= slot::kFlagBarLines; ++i) CHECK_EQ(gSlots[i], 1u);
    // Colours on flag slots, flags on colour slots, out of range: bad.
    r = parse("slot.273 = #FFFFFF\nslot.12 = on\nslot.277 = #000000\nslot.-1 = #000000\nslot.x = #000000\n"
              "flag.nope = 1\nslot.272 = #0A0B0C\nslot.0 = #010101\n");
    CHECK_EQ(r.badLines, 6);
    CHECK_EQ(gSlots[slot::kFlagBottomFade], 1u);
    CHECK_EQ(gSlots[slot::kGridLine], 0x0A0B0Cu);
    CHECK_EQ(gSlots[0], 0x010101u);
}

TEST(bad_lines_ignored_rest_applies) {
    std::string t = "nonsense\naccent\naccent =\n= #123456\nunknown_role = #000000\n9key = #000000\n";
    t += "good = #112233 extra\n";
    t += "panel = " + std::string(200, 'A') + "\n";   // longer than kThemeLineMax
    t += "good = #445566\n";
    ThemeParse r = parse(t);
    CHECK_EQ(r.badLines, 8);
    CHECK_EQ(gSlots[slot::kGreen], 0x445566u);
    CHECK_EQ(gSlots[slot::kList + 3], theme(0).slots[slot::kList + 3]);
    // A key longer than 24 characters is bad.
    r = parse(std::string(25, 'a') + " = #000000\n");
    CHECK_EQ(r.badLines, 1);
    // The last line needs no line end.
    r = parse("good = #010101");
    CHECK_EQ(gSlots[slot::kGreen], 0x010101u);
    CHECK_EQ(r.badLines, 0);
}

TEST(comments_whitespace_crlf_bom) {
    ThemeParse r = parse("\xEF\xBB\xBF# comment\r\n  ; another\r\n\taccent\t=\t#102030  # red\r\n\r\n"
                         "   good=#405060;x\r\n   \r\nyellow = #708090 ; trailing\n");
    CHECK_EQ(r.badLines, 0);
    CHECK_EQ(gSlots[slot::kFocusFrame], 0x102030u);
    CHECK_EQ(gSlots[slot::kGreen], 0x405060u);
    CHECK_EQ(gSlots[slot::kYellow], 0x708090u);
}

TEST(file_cap_8k_sets_truncated) {
    MemStorage fs;
    std::string t = "good = #010101\n";
    while (t.size() < kThemeFileMax - 10) t += "# filler filler filler filler\n";
    t += "yellow = #020202\n";   // crosses the 8 KB cap: cut
    t += "cyan = #030303\n";     // past it: never read
    putFile(fs, "Themes/Big.txt", t);
    ThemeParse r;
    CHECK(loadThemeFile(fs, "Big", gSlots, &r));
    CHECK(r.truncated);
    CHECK_EQ(gSlots[slot::kGreen], 0x010101u);
    CHECK_EQ(gSlots[slot::kCyan], theme(0).slots[slot::kCyan]);
    // A file of exactly the cap is complete.
    std::string e = "good = #010101\n";
    while (e.size() < kThemeFileMax - 16) e += "#\n";
    e += "cyan = #030303\n";
    while (e.size() < kThemeFileMax) e += "\n";
    CHECK_EQ(e.size(), size_t(kThemeFileMax));
    putFile(fs, "Themes/Exact.txt", e);
    CHECK(loadThemeFile(fs, "Exact", gSlots, &r));
    CHECK(!r.truncated);
    CHECK_EQ(gSlots[slot::kCyan], 0x030303u);
}

TEST(load_missing_file_fails_without_touching_table) {
    MemStorage fs;
    for (uint32_t& s : gSlots) s = 0xAAAAAAu;
    ThemeParse r;
    CHECK(!loadThemeFile(fs, "Nope", gSlots, &r));
    for (uint32_t s : gSlots) CHECK_EQ(s, 0xAAAAAAu);
    CHECK(!loadThemeFile(fs, "", gSlots, &r));
    CHECK(!loadThemeFile(fs, "../x", gSlots, &r));
    for (uint32_t s : gSlots) CHECK_EQ(s, 0xAAAAAAu);
    // An empty file opens: Original.
    putFile(fs, "Themes/Empty.txt", "");
    CHECK(loadThemeFile(fs, "Empty", gSlots, &r));
    CHECK(sameAs(gSlots, 0));
}

TEST(list_themes_sorted_capped_txt_only) {
    MemStorage fs;
    putFile(fs, "Themes/neon.txt", "");
    putFile(fs, "Themes/Amber.TXT", "");
    putFile(fs, "Themes/paper.Txt", "");
    putFile(fs, "Themes/notes.json", "");
    putFile(fs, "Themes/.txt", "");
    putFile(fs, "Themes/txt", "");
    putFile(fs, "Themes/abcdefghijklmnopqrstu.txt", "");   // 21 characters: too long
    putFile(fs, "Themes/abcdefghijklmnopqrst.txt", "");    // 20: fine
    putFile(fs, "Themes/a:b.txt", "");
    putFile(fs, "Themes/a\\b.txt", "");
    putFile(fs, "Themes/caf\xC3\xA9.txt", "");             // not printable ASCII
    putFile(fs, "Other/x.txt", "");
    char names[kThemeListMax][kThemeNameMax + 1];
    int n = listThemeFiles(fs, names, kThemeListMax);
    CHECK_EQ(n, 4);
    CHECK_STR(names[0], "abcdefghijklmnopqrst");
    CHECK_STR(names[1], "Amber");
    CHECK_STR(names[2], "neon");
    CHECK_STR(names[3], "paper");
    CHECK(themeNameValid("Neon"));
    CHECK(themeNameValid("My theme 2"));
    CHECK(!themeNameValid(""));
    CHECK(!themeNameValid("a/b"));
    CHECK(!themeNameValid("abcdefghijklmnopqrstu"));
    // At most kThemeListMax: the first ones in sort order.
    MemStorage many;
    for (int i = 39; i >= 0; --i) {
        char p[32];
        std::snprintf(p, sizeof p, "Themes/t%02d.txt", i);
        putFile(many, p, "");
    }
    n = listThemeFiles(many, names, kThemeListMax);
    CHECK_EQ(n, kThemeListMax);
    CHECK_STR(names[0], "t00");
    CHECK_STR(names[kThemeListMax - 1], "t31");
    CHECK_EQ(listThemeFiles(fs, names, 2), 2);
    CHECK_STR(names[1], "Amber");
    MemStorage none;
    CHECK_EQ(listThemeFiles(none, names, kThemeListMax), 0);
}

TEST(export_round_trips) {
    MemStorage fs;
    // A stock theme: base line, every role as a comment, no slot lines.
    CHECK(writeThemeFile(fs, "Monochrome", theme(1).slots, 1) == ThemeWrite::kOk);
    const std::string t = fileText(fs, "Themes/Monochrome.txt");
    CHECK(t.find("base = monochrome\n") != std::string::npos);
    CHECK(t.find("# accent = #DD2020\n") != std::string::npos);
    CHECK(t.find("# background = #4B4B4B\n") != std::string::npos);
    CHECK(t.find("slot.") == std::string::npos);
    CHECK(t.find("flag.") == std::string::npos);
    for (int r = 0; r < kThemeRoleCount; ++r)
        CHECK(t.find(std::string("# ") + themeRole(r).name + " = #") != std::string::npos);
    ThemeParse r;
    CHECK(loadThemeFile(fs, "Monochrome", gSlots, &r));
    CHECK(sameAs(gSlots, 1));
    CHECK_EQ(r.badLines, 0);
    // Exists: refused, the file stays.
    CHECK(writeThemeFile(fs, "Monochrome", theme(0).slots, 0) == ThemeWrite::kExists);
    CHECK(fileText(fs, "Themes/Monochrome.txt") == t);
    // Every slot changed (flags flipped): a complete round trip, under the cap.
    uint32_t mine[kThemeSlotCount];
    for (int i = 0; i < kThemeSlotCount; ++i)
        mine[i] = i >= slot::kFlagBottomFade ? (theme(2).slots[i] ? 0u : 1u) : ((theme(2).slots[i] + 0x010203u * uint32_t(i + 1)) & 0xFFFFFFu);
    mine[5] = theme(2).slots[5];   // one slot as the base: not written
    CHECK(writeThemeFile(fs, "Mine", mine, 2) == ThemeWrite::kOk);
    const std::string m = fileText(fs, "Themes/Mine.txt");
    CHECK(m.size() <= kThemeFileMax);
    CHECK(m.find("base = anniversary\n") != std::string::npos);
    CHECK(m.find("slot.5 =") == std::string::npos);
    CHECK(m.find("slot.6 =") != std::string::npos);
    CHECK(loadThemeFile(fs, "Mine", gSlots, &r));
    CHECK_EQ(r.base, 2);
    CHECK_EQ(r.badLines, 0);
    CHECK(!r.truncated);
    for (int i = 0; i < kThemeSlotCount; ++i) CHECK_EQ(gSlots[i], mine[i]);
    CHECK(writeThemeFile(fs, "a/b", mine, 0) == ThemeWrite::kFailed);
}

TEST(unreadable_text_guard) {
    // Text in the background colour: the text role comes back from the base.
    ThemeParse r = parse("base = monochrome\ntext = #4B4B4B\naccent = #00FF00\n");
    CHECK(r.guarded);
    CHECK_EQ(gSlots[slot::kList + 4], theme(1).slots[slot::kList + 4]);
    CHECK_EQ(gSlots[3], theme(1).slots[3]);
    CHECK_EQ(gSlots[slot::kFocusFrame], 0x00FF00u);   // the rest applies
    // Background set to the base's text colour: the background comes back.
    r = parse("background = #FFFFFF\n");
    CHECK(r.guarded);
    CHECK(gSlots[slot::kList + 4] != gSlots[slot::kFrameClear]);
    // Label text on its own background, title text on the title band.
    r = parse("label_text = #323132\n");
    CHECK(r.guarded);
    CHECK_EQ(gSlots[slot::kBottomLabel], theme(0).slots[slot::kBottomLabel]);
    r = parse("title_text = #080808\n");
    CHECK(r.guarded);
    CHECK_EQ(gSlots[slot::kTitle], theme(0).slots[slot::kTitle]);
    // Readable themes are left alone.
    r = parse("text = #EEEEEE\nbackground = #101010\n");
    CHECK(!r.guarded);
    CHECK_EQ(gSlots[slot::kList + 4], 0xEEEEEEu);
}

TEST(theme_view_reads_its_slots) {
    uint32_t s[kThemeSlotCount];
    parse("background = #010203\naccent = #040506\nnote = #070809\n", s);
    Theme t = themeView("Mine", s);
    CHECK_STR(t.name, "Mine");
    CHECK_EQ(t.bg, 0x010203u);
    CHECK_EQ(t.accent, 0x040506u);
    CHECK_EQ(t.track[0], 0x070809u);
    CHECK(t.slots == s);
    Theme o = themeView("O", theme(0).slots);
    CHECK_EQ(o.text, theme(0).text);
    CHECK_EQ(o.panelDark, theme(0).panelDark);
    CHECK_EQ(o.grid, theme(0).grid);
}

TEST(shipped_example_themes_parse_cleanly) {
    // reimpl/themes/*.txt (the release's Themes folder): every file parses
    // with no bad lines, keeps its text readable and differs from its base.
    const char* names[] = {"Neon",      "Amber", "Paper", "Bubblegum", "Barbie", "Matrix",
                           "GameBoy",   "Vaporwave", "Lava", "Ocean", "C64"};
    MemStorage fs;
    for (const char* n : names) {
        const std::string path = std::string("../themes/") + n + ".txt";
        FILE* f = std::fopen(path.c_str(), "rb");
        CHECK(f != nullptr);
        if (!f) {
            std::printf("       missing %s\n", path.c_str());
            continue;
        }
        std::string t;
        char b[512];
        size_t k;
        while ((k = std::fread(b, 1, sizeof b, f)) > 0) t.append(b, k);
        std::fclose(f);
        putFile(fs, std::string("Themes/") + n + ".txt", t);
        ThemeParse r;
        CHECK(loadThemeFile(fs, n, gSlots, &r));
        if (r.badLines || r.guarded || r.truncated) std::printf("       %s: %d bad lines%s\n", n, r.badLines, r.guarded ? ", guarded" : "");
        CHECK_EQ(r.badLines, 0);
        CHECK(!r.guarded);
        CHECK(!r.truncated);
        CHECK(!sameAs(gSlots, r.base));
        CHECK(gSlots[slot::kList + 4] != gSlots[slot::kFrameClear]);
    }
    char list[kThemeListMax][kThemeNameMax + 1];
    CHECK_EQ(listThemeFiles(fs, list, kThemeListMax), int(sizeof(names) / sizeof(names[0])));
}
