#include "panel.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "host_hal.h"
#include "ui/buttons.h"

namespace sim {

namespace b = ot::btn;

namespace {
const ButtonInfo kButtons[] = {
    {b::F1, "F1", "F1"}, {b::F2, "F2", "F2"}, {b::F3, "F3", "F3"}, {b::F4, "F4", "F4"},
    {b::F5, "F5", "F5"}, {b::F6, "F6", "F6"}, {b::F7, "F7", "F7"}, {b::F8, "F8", "F8"},
    {b::InstrParams, "I.Param", "INSTRPARAMS"}, {b::Performance, "Perform", "PERFORMANCE"},
    {b::File, "File", "FILE"}, {b::Config, "Config", "CONFIG"}, {b::Master, "Master", "MASTER"},
    {b::SampleLoader, "S.Load", "SAMPLELOADER"}, {b::SamplePlayback, "S.Play", "SAMPLEPLAYBACK"},
    {b::SampleEditor, "S.Edit", "SAMPLEEDITOR"}, {b::SampleRecorder, "S.Rec", "SAMPLERECORDER"},
    {b::Song, "Song", "SONG"}, {b::Note, "Note", "NOTE"}, {b::Instr, "Instr", "INSTR"},
    {b::Fx1, "FX1", "FX1"}, {b::Fx2, "FX2", "FX2"}, {b::Pattern, "Pattern", "PATTERN"},
    {b::Play, "Play", "PLAY"}, {b::Copy, "Copy", "COPY"}, {b::Insert, "Insert", "INSERT"},
    {b::Up, "Up", "UP"}, {b::Delete, "Delete", "DELETE"}, {b::Rec, "Rec", "REC"},
    {b::Shift, "Shift", "SHIFT"}, {b::Left, "Left", "LEFT"}, {b::Down, "Down", "DOWN"},
    {b::Right, "Right", "RIGHT"}, {b::EncPush, "Push", "ENCPUSH"}, {b::Aux, "Aux", "AUX"},
    {b::Help, "Help", "HELP"}, {b::Power, "Power", "POWER"},
};
constexpr int kNumButtons = int(sizeof(kButtons) / sizeof(kButtons[0]));

// Geometry (unscaled panel pixels).
constexpr int kFY = Panel::kScreenY + 480 + 12, kFW = 80, kFH = 26;
constexpr int kPadX0 = 20, kPadY0 = 560, kPadW = 60, kPadH = 44, kPadPX = 68, kPadPY = 50;
constexpr int kKeyX0 = 848, kKeyY0 = 230, kKeyW = 64, kKeyH = 40, kKeyPX = 72, kKeyPY = 50;
constexpr int kEncCX = 1024, kEncCY = 118, kEncR = 72, kEncPushR = 26;
constexpr int kExtraY = kKeyY0 + 5 * kKeyPY;

struct Rect {
    int x, y, w, h;
    bool in(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

bool buttonRect(uint8_t id, Rect& r) {
    if (id <= b::F8) {
        r = {Panel::kScreenX + 50 + 100 * id - kFW / 2, kFY, kFW, kFH};
        return true;
    }
    if (id >= 0x08 && id <= 0x20) {
        const int k = id - 8;
        r = {kKeyX0 + (k % 5) * kKeyPX, kKeyY0 + (k / 5) * kKeyPY, kKeyW, kKeyH};
        return true;
    }
    if (id == b::Aux) { r = {kKeyX0, kExtraY, kKeyW, kKeyH}; return true; }
    if (id == b::Help) { r = {kKeyX0 + kKeyPX, kExtraY, kKeyW, kKeyH}; return true; }
    if (id == b::Power) { r = {kKeyX0 + 4 * kKeyPX, kExtraY, kKeyW, kKeyH}; return true; }
    return false;
}

Rect padRect(int p) { return {kPadX0 + (p % 12) * kPadPX, kPadY0 + (p / 12) * kPadPY, kPadW, kPadH}; }

void centeredText(Canvas& c, int cx, int cy, uint8_t font, const char* s) {
    const RomFont* f = romFont(font);
    c.text(cx - textWidth(font, s) / 2, cy - f->height / 2, font, s);
}
}  // namespace

namespace {
// strcasecmp without <strings.h> (portable): ASCII letters fold.
bool iequals(const char* a, const char* b) {
    for (;; ++a, ++b) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
        if (!x) return true;
    }
}
}  // namespace

const ButtonInfo* buttonTable(int* count) {
    if (count) *count = kNumButtons;
    return kButtons;
}

int buttonByName(const char* name) {
    if (!name || !*name) return -1;
    for (const auto& bi : kButtons)
        if (iequals(bi.name, name) || iequals(bi.label, name)) return bi.id;
    char* end = nullptr;
    const long v = std::strtol(name, &end, 0);
    if (end && *end == 0 && v >= 0 && v < 64) return int(v);
    return -1;
}

Control Panel::hit(int x, int y) const {
    Control c;
    for (const auto& bi : kButtons) {
        Rect r;
        if (buttonRect(bi.id, r) && r.in(x, y)) {
            c.kind = Control::kButton;
            c.id = bi.id;
            return c;
        }
    }
    for (int p = 0; p < 48; ++p)
        if (padRect(p).in(x, y)) {
            c.kind = Control::kPad;
            c.id = uint8_t(p);
            return c;
        }
    const int dx = x - kEncCX, dy = y - kEncCY;
    const int d2 = dx * dx + dy * dy;
    if (d2 <= kEncPushR * kEncPushR) c.kind = Control::kEncoderPush;
    else if (d2 <= kEncR * kEncR) c.kind = dx < 0 ? Control::kEncoderCcw : Control::kEncoderCw;
    return c;
}

void Panel::draw(Canvas& c, const uint32_t* screen, const SimInput& in, const SimLeds& leds, uint64_t latched,
                 const std::string& status) const {
    c.resetClip();
    c.clearRect(0x1A1A1C);
    // Screen bezel and contents (upright; the device panel's REG_ROTATE is not applied).
    c.setColor(0x050505);
    c.fillRect(kScreenX - 6, kScreenY - 6, kScreenX + 806, kScreenY + 486);
    uint32_t* px = c.pixels();
    for (int y = 0; y < 480; ++y)
        std::memcpy(px + size_t(kScreenY + y) * kW + kScreenX, screen + size_t(y) * 800, 800 * 4);

    // Buttons.
    for (const auto& bi : kButtons) {
        Rect r;
        if (!buttonRect(bi.id, r)) continue;
        const bool down = in.buttonDown(bi.id);
        const bool latch = (latched >> bi.id) & 1;
        uint32_t face = 0x3A3A3E;
        if (bi.id == b::Play) face = 0x2E4A33;
        if (bi.id == b::Rec) face = 0x552A2A;
        if (bi.id == b::Shift) face = 0x40404A;
        if (down) face = 0x8A8A95;
        c.setColor(0x0C0C0C);
        c.fillRect(r.x - 1, r.y - 1, r.x + r.w + 1, r.y + r.h + 1);
        c.setColor(face);
        c.fillRect(r.x, r.y, r.x + r.w, r.y + r.h);
        if (latch) {
            c.setColor(0xF0C000);
            c.fillRect(r.x, r.y, r.x + r.w, r.y + 3);
        }
        c.setColor(down ? 0x000000 : 0xE0E0E0);
        centeredText(c, r.x + r.w / 2, r.y + r.h / 2, 26, bi.label);
    }
    // Pads with LED colours.
    for (int p = 0; p < 48; ++p) {
        const Rect r = padRect(p);
        const uint32_t led = leds.color(p);
        const bool down = in.padDown(uint8_t(p));
        uint32_t base = down ? 0x707078 : 0x2C2C30;
        // Additive LED glow over the rubber colour.
        uint32_t rr = ((base >> 16) & 255) + ((led >> 16) & 255);
        uint32_t gg = ((base >> 8) & 255) + ((led >> 8) & 255);
        uint32_t bb = (base & 255) + (led & 255);
        rr = rr > 255 ? 255 : rr;
        gg = gg > 255 ? 255 : gg;
        bb = bb > 255 ? 255 : bb;
        c.setColor(0x0C0C0C);
        c.fillRoundRect(float(r.x), float(r.y), float(r.x + r.w), float(r.y + r.h), 3.0f);
        c.setColor((rr << 16) | (gg << 8) | bb);
        c.fillRoundRect(float(r.x + 1), float(r.y + 1), float(r.x + r.w - 1), float(r.y + r.h - 1), 3.0f);
        if (down) {
            c.setColor(0xFFFFFF, 160);
            char n[4];
            std::snprintf(n, sizeof(n), "%d", p);
            centeredText(c, r.x + r.w / 2, r.y + r.h / 2, 26, n);
        }
    }
    // Encoder.
    c.setColor(0x0C0C0C);
    c.fillCircle(kEncCX, kEncCY, kEncR + 2);
    c.setColor(0x34343A);
    c.fillCircle(kEncCX, kEncCY, kEncR);
    c.setColor(0x4A4A52);
    c.fillCircle(kEncCX, kEncCY, kEncR - 10);
    c.setColor(in.buttonDown(b::EncPush) ? 0x8A8A95 : 0x2A2A2E);
    c.fillCircle(kEncCX, kEncCY, kEncPushR);
    c.setColor(0xB0B0B0);
    centeredText(c, kEncCX - kEncR + 22, kEncCY, 28, "<");
    centeredText(c, kEncCX + kEncR - 22, kEncCY, 28, ">");
    centeredText(c, kEncCX, kEncCY, 26, "Push");
    centeredText(c, kEncCX, kEncCY + kEncR + 14, 26, "wheel / [ ] / PgUp PgDn");

    // Status lines.
    c.setColor(0x9090A0);
    int y = kExtraY + 56;
    size_t pos = 0;
    while (pos <= status.size() && y < kH - 14) {
        size_t nl = status.find('\n', pos);
        if (nl == std::string::npos) nl = status.size();
        c.text(kKeyX0, y, 26, status.substr(pos, nl - pos).c_str());
        y += 18;
        pos = nl + 1;
    }
}

}  // namespace sim
