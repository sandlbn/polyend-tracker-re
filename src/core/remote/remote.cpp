#include "remote/remote.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ui/buttons.h"

namespace ot {
namespace remote {

namespace {
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// Whole-number arguments: false on missing or trailing garbage.
bool ints(const char* s, int* v, int n) {
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        const long x = std::strtol(s, &end, 10);
        if (end == s) return false;
        v[i] = int(x);
        s = end;
    }
    while (*s == ' ') ++s;
    return *s == 0;
}
}  // namespace

bool parse(const char* line, Msg& m) {
    const char* p = std::strstr(line, "@RC ");
    if (!p) return false;
    p += 4;
    m = Msg{};
    int v[2] = {};
    if (!std::strcmp(p, "HELLO")) m.kind = Msg::kHello;
    else if (!std::strcmp(p, "BYE")) m.kind = Msg::kBye;
    else if (!std::strcmp(p, "ON")) m.kind = Msg::kOn;
    else if (!std::strcmp(p, "OFF")) m.kind = Msg::kOff;
    else if (!std::strncmp(p, "B1 ", 3) || !std::strncmp(p, "B0 ", 3)) {
        if (!ints(p + 3, v, 1) || v[0] < 0 || v[0] > 63) return false;
        m.kind = p[1] == '1' ? Msg::kButtonDown : Msg::kButtonUp;
        m.id = uint8_t(v[0]);
    } else if (!std::strncmp(p, "P1 ", 3)) {
        if (!ints(p + 3, v, 2) || v[0] < 0 || v[0] >= kPads || v[1] < 0 || v[1] > 127) return false;
        m.kind = Msg::kPadDown;
        m.id = uint8_t(v[0]);
        m.velocity = uint8_t(v[1]);
    } else if (!std::strncmp(p, "P0 ", 3)) {
        if (!ints(p + 3, v, 1) || v[0] < 0 || v[0] >= kPads) return false;
        m.kind = Msg::kPadUp;
        m.id = uint8_t(v[0]);
    } else if (!std::strncmp(p, "E ", 2)) {
        if (!ints(p + 2, v, 1) || v[0] < -127 || v[0] > 127 || v[0] == 0) return false;
        m.kind = Msg::kEncoder;
        m.delta = int8_t(v[0]);
    } else if (!std::strncmp(p, "L ", 2)) {
        const char* h = p + 2;
        for (int i = 0; i < kPads; ++i) {
            const int a = hexDigit(h[2 * i]), b = a < 0 ? -1 : hexDigit(h[2 * i + 1]);
            if (a < 0 || b < 0) return false;
            m.levels[i] = uint8_t(a * 16 + b);
        }
        if (h[2 * kPads] != 0) return false;
        m.kind = Msg::kLeds;
    } else {
        return false;
    }
    return true;
}

int formatEvent(const InputEvent& ev, char* out, int cap) {
    int n = 0;
    switch (ev.type) {
        case InputEvent::kButtonDown: n = std::snprintf(out, size_t(cap), "@RC B1 %u", unsigned(ev.id)); break;
        case InputEvent::kButtonUp: n = std::snprintf(out, size_t(cap), "@RC B0 %u", unsigned(ev.id)); break;
        case InputEvent::kPadDown:
            n = std::snprintf(out, size_t(cap), "@RC P1 %u %u", unsigned(ev.id), unsigned(ev.velocity));
            break;
        case InputEvent::kPadUp: n = std::snprintf(out, size_t(cap), "@RC P0 %u", unsigned(ev.id)); break;
        case InputEvent::kEncoder:
            if (ev.delta == 0) return 0;
            n = std::snprintf(out, size_t(cap), "@RC E %d", int(ev.delta));
            break;
        default: return 0;
    }
    return n > 0 && n < cap ? n : 0;
}

int formatLeds(const uint8_t levels[kPads], char* out, int cap) {
    static const char kHex[] = "0123456789ABCDEF";
    if (cap < 6 + 2 * kPads + 1) return 0;
    std::memcpy(out, "@RC L ", 6);
    for (int i = 0; i < kPads; ++i) {
        out[6 + 2 * i] = kHex[levels[i] >> 4];
        out[7 + 2 * i] = kHex[levels[i] & 15];
    }
    out[6 + 2 * kPads] = 0;
    return 6 + 2 * kPads;
}

void LineReader::feed(const char* data, size_t n, void (*fn)(void*, const char*), void* ctx) {
    for (size_t i = 0; i < n; ++i) {
        const char c = data[i];
        if (c == '\n' || c == '\r') {
            if (len_ && !overflow_) {
                buf_[len_] = 0;
                fn(ctx, buf_);
            }
            len_ = 0;
            overflow_ = false;
        } else if (len_ < kLineMax) {
            buf_[len_++] = c;
        } else {
            overflow_ = true;
        }
    }
}

// ---------------------------------------------------------------------------
// DeviceBridge

DeviceBridge::DeviceBridge(IInput& panel, ILeds& pads, const Io& io) : panel_(panel), pads_(pads), io_(io) {}

void DeviceBridge::rx(const char* data, size_t n, uint32_t now) {
    now_ = now;
    reader_.feed(data, n, [](void* self, const char* l) { static_cast<DeviceBridge*>(self)->onLine(l); }, this);
}

void DeviceBridge::onLine(const char* line) {
    Msg m;
    if (!parse(line, m)) return;
    switch (m.kind) {
        case Msg::kHello:
            if (!enabled_) break;
            lastHello_ = now_;
            setActive(true);
            break;
        case Msg::kBye: setActive(false); break;
        case Msg::kLeds:
            if (!active_) break;
            for (int i = 0; i < kPads && i < pads_.padCount(); ++i)
                pads_.setPad(uint8_t(i), m.levels[i], m.levels[i], m.levels[i]);
            pads_.show();
            break;
        default: break;
    }
}

void DeviceBridge::tick(uint32_t now, bool hostOpen) {
    now_ = now;
    if (active_ && (!hostOpen || now - lastHello_ > kTimeoutMs)) setActive(false);
}

void DeviceBridge::setEnabled(bool on) {
    enabled_ = on;
    if (!on) setActive(false);
}

void DeviceBridge::setActive(bool on) {
    if (on == active_) return;
    active_ = on;
    if (on) {
        // Keys the app saw go down are released to the app, not the host.
        localButtons_ = buttonsDown_;
        localPads_ = padsDown_;
        sentButtons_ = sentPads_ = 0;
        pads_.clear();
        pads_.show();
    } else {
        // The host releases whatever it still holds for us (on "@RC OFF");
        // keys still down here are not the app's: their releases are dropped.
        localButtons_ = localPads_ = 0;
        sentButtons_ = sentPads_ = 0;
        for (int i = 0; i < kPads && i < pads_.padCount(); ++i) {
            const uint32_t c = appColor_[i];
            pads_.setPad(uint8_t(i), uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c));
        }
        pads_.show();
    }
    if (io_.send) io_.send(io_.ctx, on ? "@RC ON" : "@RC OFF");
    if (io_.changed) io_.changed(io_.ctx, on);
}

bool DeviceBridge::forward(const InputEvent& ev) {
    const uint64_t bit = 1ull << (ev.id & 63);
    const bool button = ev.type == InputEvent::kButtonDown || ev.type == InputEvent::kButtonUp;
    const bool pad = ev.type == InputEvent::kPadDown || ev.type == InputEvent::kPadUp;
    const bool down = ev.type == InputEvent::kButtonDown || ev.type == InputEvent::kPadDown;
    uint64_t& held = button ? buttonsDown_ : padsDown_;
    const bool repeat = down && (held & bit);   // driver auto-repeat: the host makes its own
    if (button || pad) held = down ? (held | bit) : (held & ~bit);
    if (!active_) return false;
    if (button && ev.id == btn::Power) {
        // Power stays local; a press also hands the panel back to the Tracker
        // (the app's power popup closes again on the release).
        if (down && !repeat) setEnabled(false);
        return false;
    }
    uint64_t& local = button ? localButtons_ : localPads_;
    if ((button || pad) && !down && (local & bit)) {
        local &= ~bit;
        return false;   // pressed before remote mode: the app gets its release
    }
    if (repeat) return true;
    uint64_t& sent = button ? sentButtons_ : sentPads_;
    if (button || pad) {
        if (!down && !(sent & bit)) return true;   // never sent down (pressed while off)
        sent = down ? (sent | bit) : (sent & ~bit);
    }
    char line[32];
    if (formatEvent(ev, line, sizeof line) && io_.send) io_.send(io_.ctx, line);
    return true;
}

bool DeviceBridge::Input::poll(InputEvent& ev) {
    while (b_.panel_.poll(ev))
        if (!b_.forward(ev)) return true;
    return false;
}

void DeviceBridge::Leds::setPad(uint8_t pad, uint8_t r, uint8_t g, uint8_t b) {
    if (pad < kPads) b_.appColor_[pad] = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    if (!b_.active_) b_.pads_.setPad(pad, r, g, b);
}

void DeviceBridge::Leds::clear() {
    std::memset(b_.appColor_, 0, sizeof(b_.appColor_));
    if (!b_.active_) b_.pads_.clear();
}

void DeviceBridge::Leds::show() {
    if (!b_.active_) b_.pads_.show();
}

}  // namespace remote
}  // namespace ot
