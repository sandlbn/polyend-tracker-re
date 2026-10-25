#include "ui/ui.h"

#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "ui/screens.h"
#include "ui/widgets.h"

namespace ot {

namespace {
void copyStr(char* dst, const char* src, size_t cap) {
    if (!cap) return;
    if (!src) src = "";
    size_t n = 0;
    while (n + 1 < cap && src[n]) ++n;
    std::memmove(dst, src, n);
    dst[n] = 0;
}

// spec 05 §2.1.4 Shift-fast table (0xa521c).
int fastDelta(int d) {
    int a = d < 0 ? -d : d;
    int m;
    if (a <= 1) m = 1;
    else if (a == 2) m = 4;
    else if (a == 3) m = 12;
    else if (a == 4) m = 20;
    else if (a == 5) m = 32;
    else if (a == 6) m = 64;
    else if (a < 40) m = 100;
    else m = 200;
    return d < 0 ? -m : m;
}

constexpr uint32_t kHoldMs = 333;        // spec 05 §2.1.1 hold threshold
constexpr uint32_t kMinFrameMs = 16;     // render rate limit
constexpr uint32_t kReadyPollMs = 1;     // frameReady() poll interval while the display is busy
}  // namespace

int moduleForButton(uint8_t id) {
    switch (id) {
        case btn::File: return kModFile;
        case btn::SampleLoader: return kModLoader;
        case btn::Pattern: return kModPattern;
        case btn::SamplePlayback: return kModPlayback;
        case btn::Song: return kModSong;
        case btn::InstrParams: return kModInstrument;
        case btn::SampleEditor: return kModEditor;
        case btn::Config: return kModConfig;
        case btn::SampleRecorder: return kModRecorder;
        case btn::Performance: return kModPerformance;
        case btn::Master: return kModMaster;
        default: return kModNone;
    }
}

// ---------------------------------------------------------------------------
// Screen defaults

void Screen::onButton(uint8_t id, uint8_t state) {
    if (id == btn::Play && state == kPress) app_.togglePlay(ui_.shift());
}

void Screen::onPad(uint8_t pad, uint8_t state) {
    // spec 06 §0.3: pads audition the current instrument.
    // One track voice per held pad, voiceId = pad number (spec 07 Q7).
    if (state == kPress) app_.previewOn(pad, app_.currentInstrument(), app_.padNote(pad), 127);
    else if (state == kRelease) app_.previewOff(pad);
}

// ---------------------------------------------------------------------------
// BottomBar

void BottomBar::clear() {
    for (auto& l : label) l = BarLabel{};
    separators = 0xFE;
    stripBg = wg::kThemeColour;
    stripStyle = wg::StripStyle::kNormal;
    labelStrip = false;
}

void BottomBar::set(int i, const char* top, const char* bottom, bool enabled) {
    if (i < 0 || i >= 8) return;
    BarLabel& l = label[i];
    copyStr(l.top, top, sizeof(l.top));
    copyStr(l.bottom, bottom, sizeof(l.bottom));
    l.enabled = enabled;
    l.wide = false;
    l.hidden = false;
    l.highlight = false;
    l.name = false;
}

void BottomBar::setName(int i, const char* line1, const char* line2, bool enabled) {
    set(i, line1, line2, enabled);
    if (i >= 0 && i < 8) label[i].name = true;
}

void BottomBar::setWide(int i, const char* top, const char* bottom, bool enabled) {
    set(i, top, bottom, enabled);
    if (i < 7) {
        label[i].wide = true;
        label[i + 1] = BarLabel{};
        label[i + 1].hidden = true;
    }
}

void BottomBar::setBottom(int i, const char* bottom) {
    if (i >= 0 && i < 8) copyStr(label[i].bottom, bottom, sizeof(label[i].bottom));
}

void BottomBar::highlightOnly(int i) {
    for (int k = 0; k < 8; ++k) label[k].highlight = k == i;
}

uint8_t BottomBar::separatorMask() const {
    uint8_t m = separators;
    for (int i = 0; i < 7; ++i)
        if (label[i].wide && !label[i].hidden) m = uint8_t(m & ~(1u << (i + 1)));
    return m;
}

uint32_t BottomBar::stripColour(const Theme& th) const {
    return stripBg == wg::kThemeColour ? wg::stripColour(th, stripStyle) : stripBg;
}

void BottomBar::drawStrip(Gfx& g, const Theme& th) const { wg::bottomStrip(g, th, separatorMask(), stripColour(th)); }

void BottomBar::drawLabels(Gfx& g, const Theme& th) const {
    // spec 20 §1.2: text-only labels centred at 50 + 100 i (wide: 100 + 100 i)
    // on the strip; the focus frames go last so neighbours never cover them.
    for (int i = 0; i < 8; ++i) {
        const BarLabel& l = label[i];
        if (l.hidden) continue;
        wg::BarLabelStyle st;
        st.enabled = l.enabled;
        st.fill = l.fill;
        st.underline = l.underline;
        st.pages = l.pages;
        st.page = l.page;
        st.dotOff = l.dotOff;
        st.nameLines = l.name;
        st.topAligned = labelStrip;
        wg::barLabel(g, th, i, l.wide, l.top, l.bottom, st);
    }
    uint8_t hl = 0, wide = 0;
    for (int i = 0; i < 8; ++i)
        if (label[i].highlight && !label[i].hidden) {
            hl |= uint8_t(1u << i);
            if (label[i].wide) wide |= uint8_t(1u << i);
        }
    wg::barFocusMask(g, th, hl, wide);
}

// ---------------------------------------------------------------------------
// Picker

void Picker::open(int kind, const char* title, int count, int cursor, ItemFn fn, void* ctx) {
    open_ = true;
    kind_ = kind;
    count_ = count > 0 ? count : 1;
    cursor_ = cursor < 0 ? 0 : (cursor >= count_ ? count_ - 1 : cursor);
    copyStr(title_, title, sizeof(title_));
    fn_ = fn;
    ctx_ = ctx;
    descFn_ = nullptr;   // descOpen_ persists across opens (spec 16 §B.4)
}

void Picker::move(int d) {
    int c = cursor_ + d;
    if (c < 0) c = 0;
    if (c >= count_) c = count_ - 1;
    cursor_ = c;
}

void Picker::pick(int pad) {
    int c = (cursor_ / 48) * 48 + pad;
    if (c < count_) cursor_ = c;
}

namespace {
// `s` cut to the characters that fit `maxW` px in `font` (a cell line).
void fitText(char* out, int cap, uint8_t font, const char* s, int maxW) {
    int n = 0;
    while (s[n] && n < cap - 1 && textWidth(font, s, n + 1) <= maxW) ++n;
    std::memcpy(out, s, size_t(n));
    out[n] = 0;
}
// Glyph run centred on (cx, cy) (one word per glyph with custom fonts).
void centredRun(Gfx& g, int cx, int cy, uint8_t font, uint32_t c, const char* s) {
    if (!s || !*s) return;
    g.glyphRun(cx - textWidth(font, s) / 2, cy - fontHeight(font) / 2, font, s, c);
}
}  // namespace

void Picker::draw(Gfx& g, const Theme& th) const {
    if (!open_) return;
    // Instrument chooser (spec 06 §6.2 Instr hold, popup kind 2). No stock
    // screenshot shows it [I]: an opaque page under the title bar (Ui::render
    // draws only the screen's title under it, docs/VERIFY_1.9.5.md §6) with
    // the 12 x 4 grid of the Notes popup (Part 2 §8.5) mirroring the pads.
    // The cells are a visible grey (VERIFY R8: 0a0a0a keys vanished on the
    // black page), the selection a red 1.5-px frame; names are cut to their
    // cell (two lines, font 1) instead of running into the neighbours.
    const int x0 = 10, y0 = 64, cw = 65, ch = 80;
    g.fill(0, wg::kTitleBandH, kScreenW, wg::kStripY - wg::kTitleBandH, th.bg);
    g.text(x0 + 3, y0 - 16, kFont4, th.slot(slot::kTitle), title_, IDisplay::kOptCenterY);
    const int page = cursor_ / 48;
    const int shown = count_ - page * 48 < 48 ? count_ - page * 48 : 48;
    g.rectsBegin(wg::mix(th.bg, th.text, 40));
    for (int i = 0; i < shown; ++i)
        g.rectsAdd(x0 + (i % 12) * cw + 1, y0 + (i / 12) * ch + 1, cw - 2, ch - 2);
    g.rectsEnd();
    const int sel = cursor_ - page * 48;
    if (sel >= 0 && sel < shown) {
        const int sx = x0 + (sel % 12) * cw, sy = y0 + (sel / 12) * ch;
        wg::stroke(g, sx + 2, sy + 2, sx + cw - 3, sy + ch - 3, th.slot(slot::kFocusFrame));
    }
    const int maxW = cw - 8;
    char buf[24], line[24];
    for (int i = 0; i < shown; ++i) {
        const int idx = page * 48 + i;
        const int cx = x0 + (i % 12) * cw + cw / 2, cy = y0 + (i / 12) * ch;
        buf[0] = 0;
        if (fn_) fn_(ctx_, idx, buf, sizeof(buf));
        // "V - Volume/Velocity" (FX list): the letter on its own line above
        // the name.
        const char* name = buf;
        int my = cy + ch / 2;
        if (buf[0] && buf[1] == ' ' && buf[2] == '-' && buf[3] == ' ') {
            const char letter[2] = {buf[0], 0};
            centredRun(g, cx, cy + 16, kFont1, th.accent, letter);
            name = buf + 4;
            my += 10;
        }
        // Two lines: split at the first space (or a '/', which is kept on
        // the first line) when the text is wider than the cell.
        const char* sp = std::strpbrk(name, " /");
        if (sp && textWidth(kFont1, name) > maxW) {
            char a[24];
            size_t n = size_t(sp - name) + (*sp == '/' ? 1 : 0);
            std::memcpy(a, name, n);
            a[n] = 0;
            fitText(line, sizeof(line), kFont1, a, maxW);
            centredRun(g, cx, my - 10, kFont1, th.text, line);
            fitText(line, sizeof(line), kFont1, sp + 1, maxW);
            centredRun(g, cx, my + 10, kFont1, th.text, line);
        } else {
            fitText(line, sizeof(line), kFont1, name, maxW);
            centredRun(g, cx, my, kFont1, th.text, line);
        }
    }
    if (descriptionOpen()) {
        // Description window over the half of the grid away from the cursor.
        const bool top = (cursor_ % 48) / 12 >= 2;
        const int by = top ? y0 - 30 : y0 + 2 * ch, bh = 2 * ch - 4;
        wg::popupBox(g, th, x0, by, 12 * cw, bh);
        wg::stroke(g, x0 + 1, by + 1, x0 + 12 * cw - 2, by + bh - 2, th.accent);
        buf[0] = 0;
        if (fn_) fn_(ctx_, cursor_, buf, sizeof(buf));
        g.text(x0 + 12, by + 18, kFontText, th.text, buf, IDisplay::kOptCenterY);
        // Word wrap at about 70 characters per line.
        const char* t = descFn_(ctx_, cursor_);
        char line[80];
        int ly = by + 46;
        while (t && *t && ly < by + bh - 8) {
            int n = int(std::strlen(t));
            if (n > 70) {
                n = 70;
                while (n > 0 && t[n] != ' ') --n;
                if (n == 0) n = 70;
            }
            std::memcpy(line, t, size_t(n));
            line[n] = 0;
            g.text(x0 + 12, ly, kFontSmall, th.text, line, IDisplay::kOptCenterY);
            ly += 22;
            t += n;
            while (*t == ' ') ++t;
        }
    }
}

void Picker::leds(uint8_t* levels, uint8_t dim, uint8_t full) const {
    if (!open_) return;
    const int page = cursor_ / 48;
    for (int i = 0; i < 48; ++i) {
        const int idx = page * 48 + i;
        levels[i] = idx == cursor_ ? full : (idx < count_ ? dim : 0);
    }
}

// ---------------------------------------------------------------------------
// Keyboard (spec 05 §5.2; key layout from the 1.9.2 manual's naming-keyboard
// figure, wide keys at the special pads of spec 05 §1.5: Backspace 10-11,
// Caps Lock 34-35, Space 43-47; the f/j pads 27/30 are the home markers).

namespace {
constexpr char kBs = '\b', kCaps = '\x01';
const char kKeys[2][4][13] = {
    {"1234567890\b\b", "qwertyuiop-+", "asdfghjkl@\x01\x01", "zxcvbnm     "},
    {"1234567890\b\b", "QWERTYUIOP-+", "ASDFGHJKL@\x01\x01", "ZXCVBNM     "},
};
const char* kWordsA[] = {"red", "blue", "silent", "lazy", "bright", "deep", "warm", "cold", "tiny", "wild",
                         "soft", "dark", "golden", "rusty", "late", "early"};
const char* kWordsB[] = {"river", "engine", "garden", "signal", "planet", "machine", "forest", "echo",
                         "pulse", "harbor", "valley", "circuit", "lantern", "comet", "tide", "orbit"};

// First and last pad of the key under pad k (wide keys span several pads).
void keySpan(int k, int& lo, int& hi) {
    lo = hi = k;
    if (k == 10 || k == 11) lo = 10, hi = 11;
    else if (k == 34 || k == 35) lo = 34, hi = 35;
    else if (k >= 43 && k <= 47) lo = 43, hi = 47;
}
}  // namespace

void Keyboard::autoName(char* out, int cap, uint32_t seed) {
    seed = seed * 1103515245u + 12345u;
    const char* a = kWordsA[(seed >> 16) % 16];
    seed = seed * 1103515245u + 12345u;
    const char* b = kWordsB[(seed >> 16) % 16];
    std::snprintf(out, size_t(cap), "%s %s", a, b);
}

void Keyboard::open(const char* title, const char* initial, int maxLen, const char* okLabel, DoneFn fn,
                    void* ctx) {
    open_ = true;
    caps_ = false;
    maxLen_ = maxLen > 0 && maxLen < int(sizeof(text_)) ? maxLen : int(sizeof(text_)) - 1;
    // spec 16 §B.1 open state: text mode, cursor at the end, the whole
    // name selected (the first typed key replaces it), key highlight on
    // Backspace.
    hl_ = 10;
    nameFocus_ = true;
    copyStr(title_, title, sizeof(title_));
    copyStr(text_, initial, size_t(maxLen_) + 1);
    cursor_ = int(std::strlen(text_));
    selAll_ = cursor_ > 0;
    copyStr(okLabel_, okLabel ? okLabel : "Enter", sizeof(okLabel_));
    altLabel_[0] = 0;
    alt_ = false;
    fn_ = fn;
    ctx_ = ctx;
}

void Keyboard::setAlt(const char* altLabel) { copyStr(altLabel_, altLabel, sizeof(altLabel_)); }

void Keyboard::clearSelection() {
    if (!selAll_) return;
    selAll_ = false;
    text_[0] = 0;
    cursor_ = 0;
}

void Keyboard::backspace() {
    // Deletes the selection, else the character before the text cursor.
    if (selAll_) {
        clearSelection();
        return;
    }
    if (cursor_ <= 0) return;
    std::memmove(text_ + cursor_ - 1, text_ + cursor_, std::strlen(text_ + cursor_) + 1);
    --cursor_;
}

void Keyboard::typeKey(int k) {
    if (k < 0 || k >= 48) return;
    const char c = kKeys[caps_ ? 1 : 0][k / 12][k % 12];
    if (c == kBs) {
        backspace();
    } else if (c == kCaps) {
        caps_ = !caps_;
    } else {
        clearSelection();   // a typed key replaces the selection
        typeChar(c);
    }
}

void Keyboard::typeChar(char c) {
    const int len = int(std::strlen(text_));
    if (len < maxLen_) {
        // Inserted at the text cursor.
        std::memmove(text_ + cursor_ + 1, text_ + cursor_, size_t(len - cursor_) + 1);
        text_[cursor_++] = c;
    }
}

void Keyboard::finish(bool ok) {
    open_ = false;
    DoneFn fn = fn_;
    fn_ = nullptr;
    if (fn) fn(ctx_, ok, text_);
}

bool Keyboard::autoNameAllowed() const { return maxLen_ > 7; }   // not for 7-char track names (manual)

void Keyboard::moveKey(int dir) {
    // dir: 0 up, 1 down, 2 right, 3 left (spec 16 §B.1). Text mode:
    // Left/Right move the text cursor and drop the selection, Down enters
    // the key grid on '1'. Grid: Up from the top row returns to text mode.
    int lo, hi;
    keySpan(hl_, lo, hi);
    if (nameFocus_) {
        if (dir == 1) {
            nameFocus_ = false;
            hl_ = 0;
        } else if (dir == 2 || dir == 3) {
            selAll_ = false;
            if (dir == 2 && text_[cursor_]) ++cursor_;
            if (dir == 3 && cursor_ > 0) --cursor_;
        }
        return;
    }
    switch (dir) {
        case 0:
            if (hl_ < 12) nameFocus_ = true;
            else hl_ -= 12;
            break;
        case 1:
            if (hl_ < 36) hl_ += 12;
            break;
        case 2: hl_ = hi % 12 == 11 ? hi - 11 : hi + 1; break;
        case 3:
            hl_ = lo % 12 == 0 ? lo + 11 : lo - 1;
            keySpan(hl_, lo, hi);
            hl_ = lo;
            break;
    }
    keySpan(hl_, lo, hi);
    hl_ = lo;
}

bool Keyboard::onButton(uint8_t id, uint8_t state) {
    if (!open_) return false;
    if (state == kRelease) return !btn::isModuleKey(id);
    switch (id) {
        case btn::F1:
        case btn::EncPush:
        case btn::Insert:
            if (!nameFocus_) typeKey(hl_);   // grid mode only (spec 16 §B.1)
            return true;
        case btn::F2:
            if (autoNameAllowed()) {
                autoName(text_, maxLen_ + 1, seed_ += 7919);
                cursor_ = int(std::strlen(text_));
                selAll_ = false;
            }
            return true;
        case btn::F6:
            if (altLabel_[0]) finish(false);
            return true;
        case btn::F7:
            if (altLabel_[0]) {
                alt_ = true;
                finish(true);
            } else {
                finish(false);
            }
            return true;
        case btn::F8:
            alt_ = false;
            finish(true);
            return true;
        case btn::Delete:
            backspace();
            return true;
        case btn::Up: moveKey(0); return true;
        case btn::Down: moveKey(1); return true;
        case btn::Right: moveKey(2); return true;
        case btn::Left: moveKey(3); return true;
        default:
            return !btn::isModuleKey(id);
    }
}

void Keyboard::onPad(uint8_t pad, uint8_t state) {
    if (state != kPress) return;
    // A pad types its key and moves the highlight there.
    int lo, hi;
    keySpan(pad, lo, hi);
    hl_ = lo;
    typeKey(pad);
}

void Keyboard::onEncoder(int d) {
    nameFocus_ = false;
    hl_ = ((hl_ + d) % 48 + 48) % 48;
    int lo, hi;
    keySpan(hl_, lo, hi);
    hl_ = lo;
}

void Keyboard::draw(Gfx& g, const Theme& th) const { draw(g, th, false); }

void Keyboard::draw(Gfx& g, const Theme& th, bool keepTitle) const {
    if (!open_) return;
    // Snapshot_020 (VISUAL_DIFF Part 2 §8.9): the screen's title bar stays
    // (status group, right text) with the keyboard title at x 9; name field
    // (1, 31)-(798, 123); 42 keys of 61 x 61 at (13 + 65 c, 143 + 65 r), wide
    // keys 65 n - 4; f / j white home keys; class-0x20 label strip.
    g.fill(0, wg::kTitleBandH, kScreenW, kScreenH - wg::kTitleBandH, th.bg);
    if (keepTitle) {
        g.fill(0, 0, wg::kStatusX, wg::kHairlineY, th.slot(slot::kTitle + 1));
        g.clip(wg::kTitleTextX, 0, wg::kTitleClipW, wg::kHairlineY - 1);
        g.text(wg::kTitleTextX, wg::kTitleCentreY, kFont4, th.slot(slot::kTitle), title_, IDisplay::kOptCenterY);
        g.noClip();
    } else {
        wg::titleBar(g, th, title_, nullptr, nullptr);
    }
    wg::nameField(g, th, 1, 31, 798, 93, text_, cursor_, selAll_, nameFocus_);
    const int x0 = 13, y0 = 143, pitch = 65, kw = 61;
    int hlo, hhi;
    keySpan(hl_, hlo, hhi);
    for (int k = 0; k < 48; ++k) {
        int lo, hi;
        keySpan(k, lo, hi);
        if (k != lo) continue;   // wide keys are drawn once
        const char c = kKeys[caps_ ? 1 : 0][k / 12][k % 12];
        const int x = x0 + (k % 12) * pitch, y = y0 + (k / 12) * pitch;
        const int w = (hi - lo) * pitch + kw;
        const bool sel = !nameFocus_ && lo == hlo;
        char s[12] = {c, 0};
        if (c == kBs) std::strcpy(s, "Backspace");
        else if (c == kCaps) std::strcpy(s, "Caps Lock");
        else if (c == ' ') std::strcpy(s, "Space");
        const bool home = k == 27 || k == 30;   // f / j (spec 16 §B.1 home markers)
        wg::keyboardKey(g, th, x, y, w, kw, s, sel, home, c == kCaps && caps_ ? th.yellow : wg::kThemeColour);
    }
    BottomBar b;
    b.labelStrip = true;
    b.set(0, "Enter");
    if (autoNameAllowed()) b.set(1, "Auto Name");
    // Labels with '\n' take two lines ("Render" / "Selection").
    auto twoLine = [&b](int i, const char* l) {
        char top[24];
        std::snprintf(top, sizeof(top), "%s", l);
        char* nl = std::strchr(top, '\n');
        if (nl) *nl = 0;
        b.set(i, top, nl ? nl + 1 : nullptr);
    };
    if (altLabel_[0]) {
        b.set(5, "Cancel");
        twoLine(6, altLabel_);
    } else {
        b.set(6, "Cancel");
    }
    twoLine(7, okLabel_);
    b.draw(g, th);
}

void Keyboard::leds(uint8_t* levels, uint8_t dim, uint8_t full) const {
    if (!open_) return;
    // spec 16 §B.1: F (27) and J (30) always lit as home markers, Caps
    // (34-35) while Caps is on, the highlighted key (all its pads) full.
    for (int i = 0; i < 48; ++i) levels[i] = 0;
    levels[27] = levels[30] = dim;
    if (caps_) levels[34] = levels[35] = dim;
    int lo, hi;
    keySpan(hl_, lo, hi);
    for (int p = lo; p <= hi; ++p) levels[p] = full;
}

// ---------------------------------------------------------------------------
// Ui

Ui::Ui(App& a) : app(a), factory(*this, a) {}

Ui::~Ui() { destroyScreens(screenOwner_, screens_); }

#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
bool Ui::encoderOnList() const {
    if (askOpen_ || progressOpen_ || powerOpen_ || kb.isOpen()) return false;
    if (picker.isOpen()) return true;
    const Screen* s = screen(active_);
    return s && s->encoderOnList();
}
#endif

const Theme& Ui::th() const { return theme(app.cfg.theme); }

void Ui::start() {
    if (!screenOwner_) screenOwner_ = createScreens(*this, app, screens_);
    // spec 06 §7.2: the developer overlay needs /.develop at boot.
    develop_ = app.fs().exists(".develop");
    active_ = kModNone;
    switchTo(kModPattern, 0);
}

void Ui::startFactoryTest() {
    // spec 05 §1.5 event 11: the active module is suspended (no stop/start).
    kb.close();
    picker.close();
    askOpen_ = false;
    ++gen_;   // releases of keys held at entry do not reach the module
    factory.start(now_);
    dirty();
}

void Ui::closeFactoryTest() {
    if (factory.active()) return;
    ++gen_;
    switchTo(kModConfig, 0);
}

void Ui::setUsbStorageMode(bool on) {
    // spec 01 §6.1: 0x284f0(on) shows / hides the page. 2.x: the page is the
    // real USB Storage mode (Tracker Mini manual §13.10): the app hands the
    // card to the computer before the page shows and takes it back (and
    // reloads the Workspace) when it closes.
    if (on == usb.active()) return;
    if (on) {
        switch (app.enterUsbStorage()) {
        case App::UsbEnter::kOk: break;
        case App::UsbEnter::kBusy: notify("Wait for the current operation to finish"); return;
        case App::UsbEnter::kNoCard: notify("No SD card"); return;
        default: notify("USB storage not available"); return;
        }
        kb.close();
        picker.close();
        askOpen_ = false;
        ++gen_;   // releases of keys held at entry do not reach the module
        usb.show(true);
        usb.setHost(app.usbHost());
    } else {
        usb.show(false);
        app.exitUsbStorage();
    }
    dirty();
}

void Ui::switchTo(int module, int opt) {
    // [I] Issue #52 (stock 1.9.2, checked on a Tracker): from the Pattern
    // screen with Rec off, Instrument Params, Sample Playback and Sample
    // Editor open the instrument of the step under the cursor and make it the
    // current instrument. A step without a note (empty, OFF, CUT, FADE) keeps
    // the current one.
    if (active_ == kModPattern && !app.editMode &&
        (module == kModInstrument || module == kModPlayback || module == kModEditor)) {
        if (const auto* p = static_cast<const PatternScreen*>(screen(kModPattern))) {
            const Step& st = app.pattern().tracks[p->track < 0 ? 0 : (p->track >= kTrackCount ? kTrackCount - 1 : p->track)]
                                 .steps[p->step < 0 ? 0 : (p->step >= kStepCount ? kStepCount - 1 : p->step)];
            if (st.note >= 0) app.setCurrentInstrument(st.instrument);
        }
    }
    // 2.x: Instrument Params on a synth step / synth instrument opens the
    // synth slot page (Tracker Mini manual p.146).
    if (module == kModInstrument) {
        const int s = SynthScreen::entrySlot(app, *this, active_);
        if (s >= 0) {
            module = kModSynth;
            opt = s + 1;
        }
    }
    Screen* next = screen(module);
    if (!next) return;
    // spec 05 §1.5 switching procedure.
    if (Screen* old = current()) {
        if (shift()) old->onButton(btn::Shift, kRelease);
        old->stop();
        previous_ = active_;
    }
    kb.close();
    picker.close();
    askOpen_ = false;
    bar.clear();
    popupKind_ = PopupHeader::kNone;
    statusHidden_ = false;
    hairline_ = wg::kThemeColour;
    cursorStep_ = -1;
    active_ = module;
    ++gen_;
    next->start(opt);
    dirty();
}

void Ui::notify(const char* text, uint32_t ms) {
    copyStr(note_, text, sizeof(note_));
    if (platform.notified) platform.notified(note_);
    noteUntil_ = now_ + ms;
    if (!noteUntil_) noteUntil_ = 1;
    dirty();
}

void Ui::ask(const char* title, const char* text, const char* l5, const char* l6, const char* l7, AskFn fn,
             void* ctx) {
    askOpen_ = true;
    copyStr(askTitle_, title, sizeof(askTitle_));
    copyStr(askText_, text, sizeof(askText_));
    copyStr(askLabel_[0], l5, sizeof(askLabel_[0]));
    copyStr(askLabel_[1], l6, sizeof(askLabel_[1]));
    copyStr(askLabel_[2], l7, sizeof(askLabel_[2]));
    askFn_ = fn;
    askCtx_ = ctx;
    dirty();
}

void Ui::progress(const char* title, int percent) {
    progressOpen_ = true;
    copyStr(progTitle_, title, sizeof(progTitle_));
    progPct_ = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    render();
}

void Ui::setProgress(const char* title, int percent) {
    const int pct = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    if (progressOpen_ && pct == progPct_ && std::strncmp(progTitle_, title, sizeof(progTitle_) - 1) == 0) return;
    progressOpen_ = true;
    copyStr(progTitle_, title, sizeof(progTitle_));
    progPct_ = pct;
    dirty();
    // Before the first screen starts (boot: "Opening last project", run
    // synchronously by App::boot) nothing else renders: draw the boot splash
    // (Snapshot_035) here, at most every 50 ms.
    if (active_ == kModNone && display_) {
        const uint32_t t = app.millis();
        if (!bootDrawn_ || t - bootDrawnAt_ >= 50 || pct == 100) {
            bootDrawn_ = true;
            bootDrawnAt_ = t;
            render();
        }
    }
}

void Ui::openPopupHeader(const char* title, PopupHeader kind) {
    popupKind_ = kind;
    copyStr(popupTitle_, title, sizeof(popupTitle_));
    dirty();
}

void Ui::closePopupHeader() {
    popupKind_ = PopupHeader::kNone;
    dirty();
}

void Ui::handle(const InputEvent& ev, uint32_t now) {
    now_ = now;
    Screen* s = current();
    switch (ev.type) {
        case InputEvent::kButtonDown: {
            const uint8_t id = ev.id;
            if (id >= 64) return;
            const bool repeat = isDown(id);
            if (repeat && !btn::autoRepeats(id)) return;
            if (!repeat) {
                down_ |= 1ull << id;
                downAt_[id] = now;
                holdSent_ &= ~(1ull << id);
            }
            dispatchButton(id, kPress);
            break;
        }
        case InputEvent::kButtonUp: {
            const uint8_t id = ev.id;
            if (id >= 64 || !isDown(id)) return;
            down_ &= ~(1ull << id);
            dispatchButton(id, kRelease);
            break;
        }
        case InputEvent::kPadDown:
        case InputEvent::kPadUp: {
            if (ev.id >= 48) return;
            const bool press = ev.type == InputEvent::kPadDown;
            if (press) pads_ |= 1ull << ev.id;
            else pads_ &= ~(1ull << ev.id);
            if (powerOpen_ || usb.active()) break;
            if (factory.active()) {
                factory.onPad(ev.id, press ? kPress : kRelease);
                break;
            }
            if (progressOpen_ || askOpen_) break;
            if (kb.isOpen()) {
                kb.onPad(ev.id, press ? kPress : kRelease);
                break;
            }
            if (picker.isOpen() && press) picker.pick(ev.id);
            if (s) s->onPad(ev.id, press ? kPress : kRelease);
            break;
        }
        case InputEvent::kEncoder: {
            if (powerOpen_ || usb.active()) break;
            if (factory.active()) {
                factory.onEncoder(ev.delta);
                break;
            }
            if (progressOpen_ || askOpen_ || !s) break;
            int d = ev.delta;
            if (kb.isOpen()) {
                kb.onEncoder(d);
                break;
            }
            if (picker.isOpen()) {
                picker.move(shift() ? fastDelta(d) : d);
            } else {
                // Jog while Master is held acts on its hold state (main page,
                // volume). Song gets its tempo focus only at the hold tick
                // (spec 16 §A.7).
                if (enterKey_ == btn::Master) deliverEnteringKey(true);
                s = current();
                if (s->encoderFast() && shift()) d = fastDelta(d);
                s->onEncoder(d);
            }
            break;
        }
        default:
            break;
    }
    // Telemetry: the oldest event not shown yet dates the next frame's
    // input latency (ev.timeMs = capture time in the driver, 0 = unknown).
    // The USB storage page takes every event; it is redrawn only when
    // something on it changes (UsbStorageMode::Action::kRedraw / exit): in
    // the mode the main loop belongs to the computer's block I/O.
    if (usb.active() && !factory.active()) return;
    if (!inputPending_) {
        inputPending_ = true;
        inputAtMs_ = ev.timeMs ? ev.timeMs : now;
    }
    dirty();
}

void Ui::dispatchButton(uint8_t id, uint8_t state) {
    Screen* s = current();
    if (!s || progressOpen_) return;
    // Up / Down while Master is held act on its hold state like the Jog
    // (manual p.16; MANUAL_AUDIT_2026-10-23 #3).
    if (state == kPress && id != enterKey_)
        deliverEnteringKey(enterKey_ == btn::Master && (id == btn::Up || id == btn::Down));
    if (state == kRelease && id == btn::Song) songHandedEarly_ = false;
    if (state == kRelease && id == enterKey_) enterKey_ = 0;
    // Releases only go to the screen that received the press.
    if (state == kPress) pressGen_[id] = gen_;
    else if (pressGen_[id] != gen_) return;

    if (id == btn::Power) {
        // spec 05 §2.1.1: Shift + Power takes a screenshot (§3.4).
        if (state == kPress && shift()) {
            render();   // the shown frame is current
            char p[48];
            const bool shot = app.screenshot(p, sizeof(p));
            // The capture blocks for ~2.6 s on the device: start the notice
            // time after it, not at the key press, or it expires unseen.
            now_ = app.millis();
            notify(shot ? p : "Screenshot failed");
        } else if (state == kPress) {
            // spec 05 §1.5 state 5: popup at the press; the board powers off
            // after the 1.5 s hold, a release before that cancels it.
            powerOpen_ = true;
            powerAt_ = powerDrawn_ = now_;
            dirty();
        } else if (state == kRelease && powerOpen_) {
            powerOpen_ = false;
            dirty();
        }
        return;
    }
    if (powerOpen_) return;   // state 5 blocks input
    if (usb.active()) {
        // The storage page takes every key; only its Exit is bound.
        switch (usb.onButton(id, state)) {
        case UsbStorageMode::Action::kExit: setUsbStorageMode(false); break;
        case UsbStorageMode::Action::kRedraw: dirty(); break;
        default: break;
        }
        return;
    }
    if (factory.active()) {
        factory.onButton(id, state);
        return;
    }
    if (askOpen_) {
        if (state != kPress) return;
        int key = -1;
        if (id >= btn::F6 && id <= btn::F8 && askLabel_[id - btn::F6][0]) key = id;
        else if (id == btn::EncPush && askLabel_[2][0]) key = btn::F8;
        else return;
        askOpen_ = false;
        AskFn fn = askFn_;
        askFn_ = nullptr;
        if (fn) fn(askCtx_, key);
        return;
    }
    if (kb.isOpen()) {
        if (kb.onButton(id, state)) return;
    }
    // [I] Shift + Sample Recorder: live resampling from any screen (arm /
    // stop at the next bar / cancel while armed), see App::resampleToggle.
    if (id == btn::SampleRecorder && shift() && state != kHold) {
        if (state == kPress) resampleKey();
        return;
    }
    if (btn::isModuleKey(id)) {
        const int m = moduleForButton(id);
        if (m == active_ || m == kModNone || s->ownsModuleKey(id)) {
            if (s->ownsModuleKey(id)) s->onButton(id, state);
            if (id == btn::Song && state == kRelease && songTemporary_) {
                songTemporary_ = false;
                if (active_ == kModSong) switchTo(previous_);
            }
            return;
        }
        // The entering press is consumed by the switch (spec 06 §0.3). For
        // the hold keys (Master, Song) it is handed to the new screen once
        // the key turns out to be held: hold tick, encoder or another key
        // while it is down (manual: Hold Master/Song + Jog from any page).
        if (state == kPress) {
            songTemporary_ = false;
            if (app.perfCc && active_ != kModPerformance) app.perfCc->perfScreenKey(id);   // spec 16 §A.15
            switchTo(m, 0);
            if (routesEnteringKey(id) && current() && current()->ownsModuleKey(id)) {
                enterKey_ = id;
                enterGen_ = gen_;
            }
        }
        return;
    }
    if (picker.isOpen() && state == kPress) {
        switch (id) {
            case btn::Left: picker.move(-1); return;
            case btn::Right: picker.move(1); return;
            case btn::Up: picker.move(-12); return;
            case btn::Down: picker.move(12); return;
            default: break;
        }
    }
    s->onButton(id, state);
}

bool Ui::routesEnteringKey(uint8_t id) { return id == btn::Master || id == btn::Song; }

void Ui::deliverEnteringKey(bool asHold) {
    const uint8_t id = enterKey_;
    if (!id) return;
    enterKey_ = 0;
    Screen* s = current();
    if (enterGen_ != gen_ || !isDown(id) || !s || !s->ownsModuleKey(id) || askOpen_ || kb.isOpen() ||
        powerOpen_ || factory.active())
        return;
    pressGen_[id] = gen_;   // its hold tick and release now reach the screen
    s->onButton(id, kPress);
    // Song entered from another page and handed over early (another key
    // inside the hold window): its hold tick still makes it temporary.
    if (id == btn::Song && !asHold) songHandedEarly_ = true;
    if (asHold && !((holdSent_ >> id) & 1)) {
        holdSent_ |= 1ull << id;
        s->onButton(id, kHold);
    }
}

void Ui::resampleKey() {
    // The Sample Recorder's take and the Sample Editor's undo live in the
    // free bank tail a take would record over: no new take from there.
    if (app.resampler.state() == Resampler::kIdle && (active_ == kModRecorder || active_ == kModEditor)) {
        notify(active_ == kModRecorder ? "Resample: not from the Sample Recorder" : "Resample: not from the Sample Editor",
               3000);
        return;
    }
    // Into: Instrument N holding a sample: ask once before the first take
    // that may replace it (App::resampleIntoOk; reset when Into changes).
    const int into = int(app.resampleCfg.into) - 1;
    if (app.resampler.state() == Resampler::kIdle && into >= 0 && app.bank.loaded(into) &&
        app.resampleIntoOk != into) {
        char title[40];
        std::snprintf(title, sizeof(title), "Resample into instrument %d?", into + 1);
        ask(title, "Same-length takes replace it", nullptr, "Cancel", "Yes",
            [](void* ctx, int key) {
                if (key != btn::F8) return;
                auto* self = static_cast<Ui*>(ctx);
                self->app.resampleIntoOk = int8_t(self->app.resampleCfg.into - 1);
                self->resampleKey();
            },
            this);
        return;
    }
    switch (app.resampleToggle()) {
        case App::ResampleAction::kArmed: break;   // update() shows the state
        case App::ResampleAction::kCancelled: notify("Resample cancelled"); break;
        case App::ResampleAction::kStopping: break;
        case App::ResampleAction::kNoSlot: notify("Resample: no free instrument slot after the last one", 4000); break;
        case App::ResampleAction::kNoMemory: notify("Resample: not enough sample memory", 3000); break;
        default: notify("Resample: busy"); break;
    }
    resampleShown_ = 0xFF;   // show the new state now
}

void Ui::update(uint32_t now) {
    now_ = now;
    // [I] Live resampling status: one notice per state change (armed,
    // armed and playing, recording, stopping), so other notices stay up;
    // the Resample screen shows the bar count.
    {
        const Resampler& r = app.resampler;
        const Resampler::State st = r.state();
        const uint8_t key = st == Resampler::kArmed && app.seq.isPlaying() ? 0x80 | st : uint8_t(st);
        if (key != resampleShown_) {
            resampleShown_ = key;
            if (r.busy() && st != Resampler::kDone && active_ != kModResample) {
                char t[64];
                if (st == Resampler::kArmed)
                    std::snprintf(t, sizeof(t), app.seq.isPlaying() ? "Resample armed: starts on the next bar"
                                                                    : "Resample armed: starts with Play");
                else if (st == Resampler::kStopping)
                    std::snprintf(t, sizeof(t), "Resampling: stops at the next bar");
                else if (r.bars())
                    std::snprintf(t, sizeof(t), "Resampling %u bar%s", r.bars(), r.bars() == 1 ? "" : "s");
                else
                    std::snprintf(t, sizeof(t), "Resampling (Shift+Sample Rec stops)");
                notify(t);
            }
        }
    }
    if (noteUntil_ && int32_t(now - noteUntil_) >= 0) {
        noteUntil_ = 0;
        note_[0] = 0;
        dirty();
    }
    // Hold ticks for keys without auto-repeat (spec 05 §2.1.1 state 2).
    for (uint8_t id = 0; id < 64; ++id) {
        if (!isDown(id) || btn::autoRepeats(id) || ((holdSent_ >> id) & 1)) continue;
        if (now - downAt_[id] >= kHoldMs) {
            holdSent_ |= 1ull << id;
            const bool entering = id == enterKey_;
            if (entering) deliverEnteringKey();
            // spec 16 §A.7: Song held from another page shows it only
            // temporarily; the release returns to the previous page (also
            // when another key handed the press over early; MANUAL_AUDIT #21).
            if (id == btn::Song && (entering || songHandedEarly_) && pressGen_[id] == gen_) songTemporary_ = true;
            if (id == btn::Song) songHandedEarly_ = false;
            if (pressGen_[id] == gen_ && !askOpen_ && !progressOpen_ && !kb.isOpen() && !powerOpen_ &&
                !factory.active() && !usb.active()) {
                Screen* s = current();
                if (s && (!btn::isModuleKey(id) || s->ownsModuleKey(id))) s->onButton(id, kHold);
            }
        }
    }
    // Shutdown popup progress (spec 05 §1.5: refresh every >= 30 ms).
    if (powerOpen_ && now - powerDrawn_ >= kPowerRedrawMs && now - powerAt_ <= kPowerHoldMs + kPowerRedrawMs) {
        powerDrawn_ = now;
        dirty();
    }
    if (usb.active() && !factory.active()) {
        // The storage page covers the screen and the CPU cell: only its own
        // state (the computer's view of the drive) redraws it.
        if (usb.setHost(app.usbHost())) dirty();
        updateLeds();
        return;
    }
    if (usb.active() && usb.setHost(app.usbHost())) dirty();
    if (factory.active()) factory.update(now);
    else if (Screen* s = current()) s->update(now);   // suspended during the factory test
    pollCpuCell(now);
    updateLeds();
}

void Ui::pollCpuCell(uint32_t now) {
    if (now - cpuPollAt_ < kCpuPollMs && cpuPollAt_ != 0) return;
    cpuPollAt_ = now ? now : 1;
    const audio::CpuMeter& m = app.engine.cpu();
    const wg::CpuCell c = wg::cpuCell(app.cfg.cpuMeter, m.hasData(), m.peak(), m.overrunRecent(),
                                      app.engine.governor().cutRecent());
    if (c.level != cpuCell_.level || c.percent != cpuCell_.percent || c.overrun != cpuCell_.overrun) {
        cpuCell_ = c;
        dirty();
    }
}

void Ui::updateLeds() {
    if (!leds_) return;
    uint8_t lv[48] = {};
    const uint8_t dim = app.cfg.padDim(), full = app.cfg.padFull();
    if (factory.active()) factory.padLeds(lv);
    else if (kb.isOpen()) kb.leds(lv, dim, full);
    else if (picker.isOpen()) picker.leds(lv, dim, full);
    else if (Screen* s = current()) s->padLeds(lv);
    for (int p = 0; p < 48; ++p)
        if (padDown(p)) lv[p] = full;   // spec 06 §1.6 rule 8
    bool changed = false;
    for (int p = 0; p < 48; ++p) {
        if (!ledInit_ || lv[p] != ledLast_[p]) {
            const uint8_t v = uint8_t(lv[p] > 31 ? 255 : lv[p] * 255 / 31);
            leds_->setPad(uint8_t(p), v, v, v);
            ledLast_[p] = lv[p];
            changed = true;
        }
    }
    ledInit_ = true;
    if (changed) leds_->show();
}

void Ui::dirty() {
    if (dirty_) return;
    dirty_ = true;
    dirtyAtUs_ = app.micros();
}

bool Ui::renderIfDirty(uint32_t now) {
    if (!dirty_ || !display_) return false;
    if (now - lastRender_ < kMinFrameMs && frames_ > 0) return false;
    // Non-blocking swap: while the previous frame is still being consumed or
    // swapped in, try again on the next main-loop pass instead of spinning.
    // The display is polled at most once per millisecond (kReadyPollMs):
    // each poll is SPI traffic (REG_CMD_READ, REG_DLSWAP), and a busy
    // display stays busy until the next vsync (up to 18.5 ms at 54 Hz).
    if (readyRetry_ && int32_t(now - readyRetryAt_) < 0) return false;
    if (!display_->frameReady()) {
        ++deferred_;
        readyRetry_ = true;
        readyRetryAt_ = now + kReadyPollMs;
        return false;
    }
    readyRetry_ = false;
    lastRender_ = now;
    const uint32_t t0 = app.micros();
    // Telemetry: dirty -> frame start, frame -> frame gap, input -> frame.
    const uint32_t lat = t0 - dirtyAtUs_;
    if (lat > dirtyLatMaxUs_) dirtyLatMaxUs_ = lat;
    if (lastFrameAtMs_ && now - lastFrameAtMs_ > gapMaxMs_) gapMaxMs_ = now - lastFrameAtMs_;
    lastFrameAtMs_ = now ? now : 1;
    if (inputPending_) {
        inputPending_ = false;
        if (now - inputAtMs_ > inputLatMaxMs_) inputLatMaxMs_ = now - inputAtMs_;
    }
    render();
    const uint32_t us = app.micros() - t0;
    renderUsSum_ += us;
    if (us > renderUsMax_) renderUsMax_ = us;
    return true;
}

void Ui::drawTitle(Gfx& g, const char* title, const char* right) {
    // The Sample Editor hides the status group [S snap 006].
    drawTitle(g, title, right, active_ != kModEditor);
}

void Ui::drawTitle(Gfx& g, const char* title, const char* right, bool status) {
    // spec 20 §1.2: status "%3im:%02is <transport> P%i.%03i <rec>".
    Sequencer& sq = app.seq;
    wg::TitleStatus st;
    st.seconds = sq.isPlaying() ? (now_ - app.playStartMs()) / 1000 : 0;
    const auto* song = static_cast<const SongScreen*>(screen(kModSong));
    st.transport = !sq.isPlaying()                         ? wg::Transport::kStopped
                   : song && song->loopingInSong()         ? wg::Transport::kLoopingInSong
                   : sq.isSongMode()                       ? wg::Transport::kPlayingSong
                                                           : wg::Transport::kPlayingPattern;
    st.recording = sq.isRecording();
    st.pattern = sq.currentPatternNumber();
    const int step0 = !sq.isPlaying() && cursorStep_ >= 0 ? cursorStep_ : sq.step(0);
    st.step = step0 + (app.cfg.numbering == 0 ? 0 : 1);
    st.hex = app.cfg.numbering == 2;
    // The band, title and right text go through a cached RAM_G fragment
    // (2.x M0); the status group changes with every step and is drawn inline.
    const bool showStatus = status && !statusHidden();
    // 2.x CPU cell next to the play time (hidden: the stock chrome).
    st.cpu = cpuCell_;
    st.cpu.right = int16_t(wg::cpuCellRight(st.seconds));
    g.cached(kFragTitle, [&] { wg::titleBarChrome(g, th(), title, right, showStatus, &st.cpu); });
    if (showStatus) wg::titleBarStatus(g, th(), st);
    titleDrawn_ = true;
    // Remembered for the keyboard frames (render()); title may already be
    // the remembered copy.
    if (title != lastTitle_) copyStr(lastTitle_, title ? title : "", sizeof(lastTitle_));
    if (right != lastRight_) copyStr(lastRight_, right ? right : "", sizeof(lastRight_));
    lastHasRight_ = right != nullptr;
    lastStatus_ = status;
    lastTitleGen_ = gen_;
    lastTitleValid_ = true;
}

uint8_t Ui::overlayBits() const {
    return uint8_t((kb.isOpen() ? 1 : 0) | (picker.isOpen() ? 2 : 0) | (usb.active() ? 4 : 0) |
                   (factory.active() ? 8 : 0) | (askOpen_ ? 16 : 0) | (progressOpen_ ? 32 : 0) |
                   (popupKind_ != PopupHeader::kNone ? 64 : 0));
}

void Ui::render() {
    if (!display_) return;
    // Low-detail fallback stays on while the screen and its overlays stay
    // the same (set by an overflowing frame below).
    const uint8_t ov = overlayBits();
    if (lowDetail_ && (lowDetailGen_ != gen_ || lowDetailOverlays_ != ov)) lowDetail_ = false;
    dirty_ = false;
    titleDrawn_ = false;
    const Theme& t = th();
    Gfx g(*display_, &fragments);
    // RAM_DL guard (docs/VERIFY_1.9.5.md §6): never send more than the
    // FT81x can hold next to the driver's per-frame words (appended RAM_G
    // fragments included, Gfx::dlTotalWords).
    // Device feedback (FT81x). A frame the coprocessor faulted on is lost
    // (the driver recovers); drawing the same frame again would fault again
    // and the screen would look frozen (user report: Master -> Track Mixer).
    // So: low detail for this screen and a tighter guard. The measured
    // RAM_DL size of finished frames calibrates the estimate (gradients and
    // other coprocessor commands expand on the device).
    // Per screen: text-heavy and bar-heavy pages expand differently.
    if (scaleGen_ != gen_) {
        scaleGen_ = gen_;
        dlScale_ = 64;
    }
    const uint32_t hwFaults = display_->frameFaultCount();
    if (hwFaults != hwFaultsSeen_) {
        hwFaultsSeen_ = hwFaults;
        ++hwFaultFrames_;
        lowDetail_ = true;
        lowDetailGen_ = gen_;
        lowDetailOverlays_ = ov;
        dlScale_ = uint8_t(dlScale_ + 8 > 128 ? 128 : dlScale_ + 8);
        if (platform.dlOverflow) platform.dlOverflow(current() ? current()->name() : "-", lastWords_, 0, true);
    }
    const uint32_t meas = display_->measuredDlBytes();
    if (meas != hwMeasSeen_) {
        hwMeasSeen_ = meas;
        // Only a steady screen pairs the measurement with its estimate (the
        // last two frames within 1/16 of each other).
        const uint32_t mw = meas / 4 > kDriverDlWords ? meas / 4 - kDriverDlWords : 0;
        const uint32_t d = lastWords_ > prevWords_ ? lastWords_ - prevWords_ : prevWords_ - lastWords_;
        if (lastWords_ >= 400 && d * 16 <= lastWords_ && mw > lastWords_) {
            const uint32_t sc = mw * 64 / lastWords_;
            if (sc > dlScale_) dlScale_ = uint8_t(sc > 128 ? 128 : sc);
        }
    }
    g.setWordLimit(dlFrameLimit_ ? uint32_t(uint64_t(dlFrameLimit_) * 64 / dlScale_) : 0);
    g.setLowDetail(lowDetail_);
    g.beginFrame(t.bg);
    Screen* s = current();
    if (factory.active()) s = nullptr;
    if (!s && !factory.active() && progressOpen_) {
        // Boot (no screen started yet): the splash, not the popup box.
#ifdef OT_VERSION
        wg::bootSplash(g, t, progTitle_, progPct_, "v" OT_VERSION);
#else
        wg::bootSplash(g, t, progTitle_, progPct_, nullptr);
#endif
        finishFrame(g, nullptr);
        return;
    }
    // Opaque overlays cover everything under the title bar (keyboard, the
    // pad-grid chooser) or the whole screen (USB storage page): the screen
    // itself is not drawn under them, only its remembered title bar. A
    // screen plus such an overlay overflows RAM_DL (1.9.5: New Folder,
    // Track Mixer / Sample Loader rename, the instrument chooser).
    const bool usbPage = usb.active() && !factory.active();
    const bool covered = !factory.active() && (kb.isOpen() || picker.isOpen()) && s;
    const bool titleOnly = covered && lastTitleValid_ && lastTitleGen_ == gen_;
    const PopupHeader ph = (factory.active() || kb.isOpen()) ? PopupHeader::kNone : popupKind_;
    // The bottom strip goes under the screen (stock draws the strip control
    // first: panel fades end on top of its edge row); labels go on top.
    const bool barShown = !factory.active() && !usbPage && !kb.isOpen() && ph != PopupHeader::kFull &&
                          (askOpen_ || !s || !s->fullScreen());
    stripUnder_ = barShown;
    if (barShown) {
        const BottomBar& sb = askOpen_ ? BottomBar{} : bar;
        stripUnderC_ = sb.stripColour(t);
        sb.drawStrip(g, t);
    }
    if (factory.active()) {
        factory.draw(g);
    } else if (usbPage) {
        // usb.draw (below) fills the whole frame.
    } else if (titleOnly) {
        drawTitle(g, lastTitle_, lastHasRight_ ? lastRight_ : nullptr, lastStatus_);
    } else if (s) {
        s->draw(g);
    }
    if (factory.active() || usbPage) {
        // full screen; only the popups below
    } else if (kb.isOpen()) {
        kb.draw(g, t, titleDrawn_);
        titleDrawn_ = true;
    } else {
        picker.draw(g, t);
        if (askOpen_) {
            BottomBar b;
            for (int i = 0; i < 3; ++i)
                if (askLabel_[i][0]) b.set(5 + i, askLabel_[i]);
            g.cached(kFragBar, [&] { b.drawLabels(g, t); });
        } else if (barShown) {
            if (ph == PopupHeader::kRight) {
                // F7 / F8 lie under the popup panel (015 / 018); the x 600
                // separator stays.
                BottomBar b = bar;
                for (int i = 6; i < 8; ++i) b.label[i].hidden = true;
                if (b.label[5].wide) b.label[5].hidden = true;
                b.drawLabels(g, t);
                g.fill(wg::kRightPopupX + 1, kBarY, kScreenW - wg::kRightPopupX - 1, kScreenH - kBarY,
                       t.slot(slot::kEmptyPanel + 1));
            } else {
                g.cached(kFragBar, [&] { bar.drawLabels(g, t); });
            }
        }
    }
    if (titleDrawn_ && !factory.active() && !usbPage && !(s && s->fullScreen() && !kb.isOpen()) &&
        ph != PopupHeader::kFull)
        wg::titleHairline(g, t, 0,
                          ph == PopupHeader::kRight || ph == PopupHeader::kRightBar ? wg::kRightPopupX - 1 : kScreenW - 1,
                          hairline_);
    if (!usbPage) {
        if (ph == PopupHeader::kRight || ph == PopupHeader::kRightBar) wg::rightPopupHeader(g, t, popupTitle_);
        else if (ph == PopupHeader::kFull) wg::fullPopupHeader(g, t, popupTitle_);
    }
    usb.draw(g, t);   // full screen over the module and its popups
    if (askOpen_ && !usb.active()) wg::messagePopup(g, t, askTitle_, askText_, 150, 150);
    if (progressOpen_) wg::progressPopup(g, t, progTitle_, progPct_);
    // "Shutting down" (stock string, STRINGS_DIFF) with the remaining hold;
    // the stock progress-popup style (spec 20 §3.7). The bar shrinks as on
    // stock (device check).
    if (powerOpen_) wg::progressPopup(g, t, "Shutting down", powerPopupPercent());
    if (note_[0]) {
        wg::popupBox(g, t, wg::kProgressX - 40, 360, wg::kProgressW + 80, 44);
        g.text(400, 382, kFont3, t.slot(slot::kProgress + 2), note_, IDisplay::kOptCenter);
    }
    finishFrame(g, factory.active() ? "Factory test" : usbPage ? "USB storage" : s ? s->name() : nullptr);
}

void Ui::finishFrame(Gfx& g, const char* screenName) {
    // RAM_DL words including appended fragments; the command-FIFO upload.
    prevWords_ = frameGen_ == gen_ ? lastWords_ : 0;   // pairs only frames of one screen
    frameGen_ = gen_;
    lastWords_ = g.dlTotalWords();
    lastUpload_ = g.dlWords();
    if (lastWords_ > maxWords_) maxWords_ = lastWords_;
    if (g.dropped()) {
        // The frame did not fit RAM_DL: the guard dropped its last
        // primitives (the device would have faulted). Count it, log the
        // first one per screen, and redraw at once in low detail.
        ++overBudget_;
        const int mod = active_ >= 0 && active_ < kModCount ? active_ : kModCount;
        if (!(overLogged_ & (1u << mod))) {
            overLogged_ |= 1u << mod;
            if (platform.dlOverflow) platform.dlOverflow(screenName ? screenName : "-", lastWords_, g.dropped(),
                                                         g.lowDetail());
        }
        if (!g.lowDetail()) {
            lowDetail_ = true;
            lowDetailGen_ = gen_;
            lowDetailOverlays_ = overlayBits();
            dirty();
        }
    }
    if (g.lowDetail()) ++lowDetailFrames_;
    g.endFrame();
    ++frames_;
}

}  // namespace ot
