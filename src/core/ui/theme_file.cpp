// 2.x theme files (ui/theme_file.h): role table, parser, lister, exporter.
#include "ui/theme_file.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ot {

namespace {

// Role slots (a partition of 0..272; tests/theme_file_test.cpp checks it).
// The first slot is the one the named Theme field reads (theme.cpp).
const uint16_t kBackground[] = {0, 142, 196};
const uint16_t kTitleBg[] = {2, 6};
const uint16_t kTitleText[] = {1, 5};
const uint16_t kText[] = {87,  3,   4,   8,   12,  44,  49,  53,  67,  69,  70,  125, 135, 145, 148, 152, 157,
                          166, 171, 172, 173, 199, 209, 216, 242, 246, 73,  83,  84,  91,  101, 104, 105, 108,
                          115, 118, 119, 122, 141, 220, 230, 234, 238, 243, 266, 60,  161, 164, 200};
const uint16_t kInverseText[] = {45, 48, 52, 137, 212, 262, 265};
const uint16_t kInverseBg[] = {138, 195, 198, 214, 219, 233, 250, 254, 255, 263};
const uint16_t kDim[] = {66, 245, 190, 193, 16, 57};
const uint16_t kScrollThumb[] = {85, 92, 99, 106, 113, 120, 143, 201};
const uint16_t kScrollTrack[] = {89, 96, 103, 110, 117, 124, 147, 205};
const uint16_t kLine2[] = {56, 63, 140, 151, 155, 237, 241};
const uint16_t kLabelText[] = {9, 17, 20, 25, 28, 29, 32, 33, 41, 159};
const uint16_t kLabelTextOff[] = {13, 21, 24, 203};
const uint16_t kLabelBg[] = {10, 14, 18, 22, 34, 62, 68, 71, 160};
const uint16_t kStripSlots[] = {58, 202};
const uint16_t kPanel[] = {86,  50,  77,  82,  93,  100, 107, 114, 121, 144, 149, 153, 156, 224,
                           229, 235, 239, 76,  81,  126, 223, 228, 54,  247, 129, 134, 210, 213};
const uint16_t kPanel2[] = {162, 26, 61, 169, 78, 94, 90, 217, 253, 128, 167, 163, 170, 65, 165, 207, 231};
const uint16_t kAccent[] = {130, 7,   11,  15,  19,  23,  27,  30,  31,  35,  36,  37,  38,  39,  40,  42,
                            43,  47,  59,  88,  95,  98,  112, 116, 123, 127, 136, 139, 146, 154, 158, 184,
                            215, 218, 221, 225, 226, 227, 240, 248, 251, 257, 264, 267, 46,  64,  72,  97,
                            111, 204, 168};
const uint16_t kAlert[] = {51, 74, 79, 150, 197, 211, 232, 55, 261};
const uint16_t kGood[] = {270, 102, 109, 132, 191, 208, 222, 236, 249, 258, 75, 80, 260};
const uint16_t kYellowSlots[] = {269, 194, 256};
const uint16_t kOrangeSlots[] = {268};
const uint16_t kCyanSlots[] = {271, 252};
const uint16_t kNote[] = {176}, kInstrument[] = {177}, kFx1[] = {178}, kFx2[] = {179};
const uint16_t kNoteEmpty[] = {180}, kInstrumentEmpty[] = {181}, kFx1Empty[] = {182}, kFx2Empty[] = {183};
const uint16_t kGridHeader[] = {174}, kGridBg[] = {175}, kGridBeat[] = {185}, kGridMuted[] = {186};
const uint16_t kGridCursorRowSlots[] = {187}, kGridCursorTextSlots[] = {188}, kGridCursorAlt[] = {192};
const uint16_t kGridLines[] = {272, 189};
const uint16_t kWaveSlots[] = {244};
const uint16_t kFocusFill[] = {131, 133};
const uint16_t kMarkerGrey[] = {259};
const uint16_t kMarkerSel[] = {206};

#define R(n, a) {n, a, uint8_t(sizeof(a) / sizeof((a)[0]))}
const ThemeRole kRoles[kThemeRoleCount] = {
    R("background", kBackground),
    R("title_bg", kTitleBg),
    R("title_text", kTitleText),
    R("text", kText),
    R("inverse_text", kInverseText),
    R("inverse_bg", kInverseBg),
    R("dim", kDim),
    R("scroll_thumb", kScrollThumb),
    R("scroll_track", kScrollTrack),
    R("line2", kLine2),
    R("label_text", kLabelText),
    R("label_text_off", kLabelTextOff),
    R("label_bg", kLabelBg),
    R("strip", kStripSlots),
    R("panel", kPanel),
    R("panel2", kPanel2),
    R("accent", kAccent),
    R("alert", kAlert),
    R("good", kGood),
    R("yellow", kYellowSlots),
    R("orange", kOrangeSlots),
    R("cyan", kCyanSlots),
    R("note", kNote),
    R("instrument", kInstrument),
    R("fx1", kFx1),
    R("fx2", kFx2),
    R("note_empty", kNoteEmpty),
    R("instrument_empty", kInstrumentEmpty),
    R("fx1_empty", kFx1Empty),
    R("fx2_empty", kFx2Empty),
    R("grid_header", kGridHeader),
    R("grid_bg", kGridBg),
    R("grid_beat", kGridBeat),
    R("grid_muted", kGridMuted),
    R("grid_cursor_row", kGridCursorRowSlots),
    R("grid_cursor_text", kGridCursorTextSlots),
    R("grid_cursor_alt", kGridCursorAlt),
    R("grid_lines", kGridLines),
    R("wave", kWaveSlots),
    R("focus_fill", kFocusFill),
    R("marker_grey", kMarkerGrey),
    R("marker_sel", kMarkerSel),
};
#undef R

const char* const kBaseNames[kThemeCount] = {"original", "monochrome", "anniversary"};
// flag.<name> -> slots 273..276.
const char* const kFlagNames[4] = {"bottom_fade", "valuebar_gradient", "cursor_row_gradient", "bar_lines"};

// Unreadable-text guard: (text role, its background role) by role index.
const uint8_t kReadable[][2] = {{3, 0}, {10, 12}, {2, 1}};

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
bool sameCi(const char* a, const char* b) {
    while (*a && lower(*a) == lower(*b)) ++a, ++b;
    return lower(*a) == lower(*b);
}
int cmpCi(const char* a, const char* b) {
    while (*a && lower(*a) == lower(*b)) ++a, ++b;
    const int d = int(uint8_t(lower(*a))) - int(uint8_t(lower(*b)));
    return d ? d : std::strcmp(a, b);
}
bool space(char c) { return c == ' ' || c == '\t'; }
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// Colour value: #RRGGBB, #RGB, 0xRRGGBB, RRGGBB.
bool parseColour(const char* v, uint32_t* out) {
    const bool hash = v[0] == '#';
    if (v[0] == '0' && lower(v[1]) == 'x') v += 2;
    else if (hash) ++v;
    const int n = int(std::strlen(v));
    if (n != 6 && !(n == 3 && hash)) return false;
    uint32_t c = 0;
    for (int i = 0; i < n; ++i) {
        const int d = hexDigit(v[i]);
        if (d < 0) return false;
        c = (c << (n == 3 ? 8 : 4)) | uint32_t(n == 3 ? d * 17 : d);
    }
    *out = c & 0xFFFFFFu;
    return true;
}

bool parseFlag(const char* v, uint32_t* out) {
    if (sameCi(v, "1") || sameCi(v, "on") || sameCi(v, "true")) *out = 1;
    else if (sameCi(v, "0") || sameCi(v, "off") || sameCi(v, "false")) *out = 0;
    else return false;
    return true;
}

void copyBase(uint32_t* slots, uint8_t base) { std::memcpy(slots, theme(base).slots, sizeof(uint32_t) * kThemeSlotCount); }

void fillRole(uint32_t* slots, const ThemeRole& r, const uint32_t* from, uint32_t c, bool useFrom) {
    for (int k = 0; k < r.count; ++k) slots[r.slots[k]] = useFrom ? from[r.slots[k]] : c;
}

// Line-by-line parser over a byte stream (stack scratch: one line).
struct Parser {
    uint32_t* slots;
    ThemeParse* r;
    char line[kThemeLineMax + 1];
    int n = 0;
    bool over = false;     // the line is longer than kThemeLineMax
    bool locked = false;   // a colour / flag line was applied: base is final
    uint32_t seen = 0;     // bytes fed (BOM check)

    Parser(uint32_t* s, ThemeParse* p) : slots(s), r(p) {
        *r = ThemeParse{};
        copyBase(slots, 0);
    }
    void feed(const char* p, uint32_t len) {
        for (uint32_t i = 0; i < len; ++i, ++seen) {
            const char c = p[i];
            if (seen < 3 && n == int(seen) && uint8_t(c) == uint8_t("\xEF\xBB\xBF"[seen])) {
                line[n++] = c;   // possible BOM: dropped once complete
                if (seen == 2) n = 0;
                continue;
            }
            if (c == '\n') {
                endLine();
                continue;
            }
            if (n < kThemeLineMax) line[n++] = c;
            else over = true;
        }
    }
    void endLine() {
        if (n > 0 && line[n - 1] == '\r') --n;
        line[n] = 0;
        if (over || !apply(line)) ++r->badLines;
        n = 0;
        over = false;
    }
    void finish(bool truncated) {
        // A cut last line is dropped; a complete one needs no line end.
        if (truncated) r->truncated = true;
        else if (n > 0 || over) endLine();
        guard();
    }
    bool apply(char* s) {
        while (space(*s)) ++s;
        if (!*s || *s == '#' || *s == ';') return true;
        // key
        char* key = s;
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_')) return false;
        while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' ||
               *s == '.')
            ++s;
        if (s - key > 24) return false;
        char* keyEnd = s;
        while (space(*s)) ++s;
        if (*s != '=') return false;
        *keyEnd = 0;
        ++s;
        while (space(*s)) ++s;
        // value: one token; a comment ('#' or ';') may follow
        char* val = s;
        while (*s && !space(*s) && *s != ';' && !(*s == '#' && s != val)) ++s;
        if (s == val) return false;
        char* valEnd = s;
        while (space(*s)) ++s;
        if (*s && *s != '#' && *s != ';') return false;
        *valEnd = 0;
        return set(key, val);
    }
    bool set(const char* key, const char* val) {
        uint32_t c = 0;
        if (sameCi(key, "base")) {
            if (locked) return false;
            for (uint8_t b = 0; b < kThemeCount; ++b)
                if (sameCi(val, kBaseNames[b]) || (val[0] == char('0' + b) && !val[1])) {
                    r->base = b;
                    copyBase(slots, b);
                    return true;
                }
            return false;
        }
        if (sameCiPrefix(key, "slot.")) {
            const char* d = key + 5;
            int i = 0, k = 0;
            for (; d[k] >= '0' && d[k] <= '9' && k < 4; ++k) i = i * 10 + (d[k] - '0');
            if (k == 0 || d[k] || i >= kThemeSlotCount) return false;
            if (!(i < slot::kFlagBottomFade ? parseColour(val, &c) : parseFlag(val, &c))) return false;
            slots[i] = c;
            locked = true;
            return true;
        }
        if (sameCiPrefix(key, "flag.")) {
            for (int f = 0; f < 4; ++f)
                if (sameCi(key + 5, kFlagNames[f])) {
                    if (!parseFlag(val, &c)) return false;
                    slots[slot::kFlagBottomFade + f] = c;
                    locked = true;
                    return true;
                }
            return false;
        }
        const int ri = themeRoleIndex(key);
        if (ri < 0 || !parseColour(val, &c)) return false;
        fillRole(slots, kRoles[ri], nullptr, c, false);
        locked = true;
        return true;
    }
    static bool sameCiPrefix(const char* s, const char* p) {
        for (; *p; ++s, ++p)
            if (lower(*s) != *p) return false;
        return true;
    }
    void guard() {
        const uint32_t* base = theme(r->base).slots;
        for (const auto& g : kReadable) {
            const ThemeRole& fg = kRoles[g[0]];
            const ThemeRole& bg = kRoles[g[1]];
            if (slots[fg.slots[0]] != slots[bg.slots[0]]) continue;
            r->guarded = true;
            fillRole(slots, fg, base, 0, true);
            if (slots[fg.slots[0]] == slots[bg.slots[0]]) fillRole(slots, bg, base, 0, true);
        }
    }
};

void themePath(char* out, size_t cap, const char* name) { std::snprintf(out, cap, "%s/%s.txt", kThemesDir, name); }

struct ListCtx {
    char (*names)[kThemeNameMax + 1];
    int max;
    int n;
};
bool listFn(const IStorage::DirEntry& e, void* p) {
    ListCtx& x = *static_cast<ListCtx*>(p);
    if (e.isDir) return true;
    const size_t len = std::strlen(e.name);
    if (len < 5 || len - 4 > size_t(kThemeNameMax) || !sameCi(e.name + len - 4, ".txt")) return true;
    char base[kThemeNameMax + 1];
    std::memcpy(base, e.name, len - 4);
    base[len - 4] = 0;
    if (!themeNameValid(base)) return true;
    // Sorted insertion; when full, the last one in sort order drops out.
    int i = x.n < x.max ? x.n++ : x.max;
    if (i == x.max && (x.max == 0 || cmpCi(base, x.names[x.max - 1]) >= 0)) return true;
    if (i == x.max) --i;
    while (i > 0 && cmpCi(x.names[i - 1], base) > 0) {
        std::memcpy(x.names[i], x.names[i - 1], sizeof(x.names[0]));
        --i;
    }
    std::memcpy(x.names[i], base, sizeof(base));
    return true;
}

// Buffered text output to an open file.
struct Writer {
    IStorage& fs;
    IStorage::Handle h;
    char buf[256];
    int n = 0;
    bool ok = true;
    void flush() {
        if (n && fs.write(h, buf, uint32_t(n)) != n) ok = false;
        n = 0;
    }
    void line(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char l[96];
        va_list ap;
        va_start(ap, fmt);
        const int k = std::vsnprintf(l, sizeof(l), fmt, ap);
        va_end(ap);
        if (k <= 0) return;
        const int len = k < int(sizeof(l)) ? k : int(sizeof(l)) - 1;
        if (n + len > int(sizeof(buf))) flush();
        std::memcpy(buf + n, l, size_t(len));
        n += len;
    }
};

}  // namespace

const ThemeRole& themeRole(int i) { return kRoles[i >= 0 && i < kThemeRoleCount ? i : 0]; }

int themeRoleIndex(const char* name) {
    for (int i = 0; i < kThemeRoleCount; ++i)
        if (sameCi(name, kRoles[i].name)) return i;
    return -1;
}

const char* themeBaseName(uint8_t base) { return kBaseNames[base < kThemeCount ? base : 0]; }

void parseTheme(const char* text, uint32_t len, uint32_t* slots, ThemeParse* out) {
    Parser p(slots, out);
    const bool cut = len > kThemeFileMax;
    p.feed(text, cut ? kThemeFileMax : len);
    p.finish(cut);
}

bool themeNameValid(const char* name) {
    int n = 0;
    for (; name[n]; ++n) {
        const char c = name[n];
        if (c < 0x20 || c > 0x7E || c == '/' || c == '\\' || c == ':' || n >= kThemeNameMax) return false;
    }
    return n > 0;
}

bool loadThemeFile(IStorage& fs, const char* name, uint32_t* slots, ThemeParse* out) {
    if (!themeNameValid(name)) return false;
    char path[48];
    themePath(path, sizeof(path), name);
    const IStorage::Handle h = fs.open(path, IStorage::kRead);
    if (h < 0) return false;
    const int32_t size = fs.size(h);
    Parser p(slots, out);
    char chunk[512];
    uint32_t total = 0;
    int32_t got;
    while (total < kThemeFileMax) {
        const uint32_t want = kThemeFileMax - total < sizeof(chunk) ? kThemeFileMax - total : uint32_t(sizeof(chunk));
        if ((got = fs.read(h, chunk, want)) <= 0) break;
        p.feed(chunk, uint32_t(got));
        total += uint32_t(got);
    }
    fs.close(h);
    p.finish(size > int32_t(kThemeFileMax));
    return true;
}

int listThemeFiles(IStorage& fs, char (*names)[kThemeNameMax + 1], int max) {
    ListCtx x{names, max, 0};
    fs.listDir(kThemesDir, listFn, &x);
    return x.n;
}

ThemeWrite writeThemeFile(IStorage& fs, const char* name, const uint32_t* slots, uint8_t base) {
    if (!themeNameValid(name) || base >= kThemeCount) return ThemeWrite::kFailed;
    char path[48];
    themePath(path, sizeof(path), name);
    if (fs.exists(path)) return ThemeWrite::kExists;
    fs.mkdir(kThemesDir);
    const IStorage::Handle h = fs.open(path, IStorage::kWrite);
    if (h < 0) return ThemeWrite::kFailed;
    Writer w{fs, h, {}};
    const uint32_t* b = theme(base).slots;
    w.line("# OpenTracker theme \"%s\" (Config > General > Export theme).\n", name);
    w.line("# To change a colour, remove the '#' in front of its line and edit it.\n");
    w.line("base = %s\n\n", kBaseNames[base]);
    for (const ThemeRole& r : kRoles) w.line("# %s = #%06lX\n", r.name, static_cast<unsigned long>(slots[r.slots[0]]));
    bool head = false;
    for (int i = 0; i < kThemeSlotCount; ++i) {
        if (slots[i] == b[i]) continue;
        if (!head) w.line("\n# Colours that differ from the base theme:\n");
        head = true;
        if (i < slot::kFlagBottomFade) w.line("slot.%d = #%06lX\n", i, static_cast<unsigned long>(slots[i] & 0xFFFFFFu));
        else w.line("flag.%s = %d\n", kFlagNames[i - slot::kFlagBottomFade], slots[i] ? 1 : 0);
    }
    w.flush();
    fs.close(h);
    return w.ok ? ThemeWrite::kOk : ThemeWrite::kFailed;
}

}  // namespace ot
