// [I] Remote control (not in stock): a Tracker running OpenTracker as the
// front panel of the desktop simulator. Text lines over the device's USB
// serial (CDC) port, which it shares with the diag log (hal/diag.h):
//
//   host -> device   "@RC HELLO"            enter remote mode / keepalive (every kKeepaliveMs)
//                    "@RC BYE"              leave remote mode
//                    "@RC L <96 hex>"       pad LED levels 0..255, pads 0..47
//   device -> host   "@RC ON" / "@RC OFF"   remote mode entered / left
//                    "@RC B1 <id>" / "@RC B0 <id>"            button down / up
//                    "@RC P1 <pad> <velocity>" / "@RC P0 <pad>"  pad down / up
//                    "@RC E <delta>"        encoder detents (device acceleration applied)
//
// Device lines arrive inside diag log lines (timestamp prefix), so a parser
// looks for "@RC " anywhere in the line. While remote mode is on the device
// forwards its input instead of handing it to its own app (the power switch
// stays local) and shows the host's pad LEDs; it falls back to normal after
// "@RC BYE", kTimeoutMs without a keepalive, or the host closing the port.
//
// The device only answers while remote control is enabled (Config > USB >
// Remote control, Off after power-on). A short press of the power switch
// while the host drives the panel disables it again.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hal_api.h"

namespace ot {
namespace remote {

constexpr uint32_t kKeepaliveMs = 500;
constexpr uint32_t kTimeoutMs = 2000;
constexpr int kPads = 48;
constexpr int kLineMax = 128;   // longest line: "@RC L " + 96 hex digits

struct Msg {
    enum Kind : uint8_t { kNone, kHello, kBye, kOn, kOff, kButtonDown, kButtonUp, kPadDown, kPadUp, kEncoder, kLeds };
    Kind kind = kNone;
    uint8_t id = 0;
    uint8_t velocity = 0;
    int8_t delta = 0;
    uint8_t levels[kPads] = {};
};

// One line (without the line end) -> message; false for anything else.
bool parse(const char* line, Msg& m);
// Input event -> "@RC ..." (no line end); 0 for events with no remote form.
int formatEvent(const InputEvent& ev, char* out, int cap);
int formatLeds(const uint8_t levels[kPads], char* out, int cap);

// Splits a byte stream into lines (CR / LF ends, over-long lines dropped).
class LineReader {
public:
    // Feeds bytes; calls fn(ctx, line) for each complete line.
    void feed(const char* data, size_t n, void (*fn)(void*, const char*), void* ctx);

private:
    char buf_[kLineMax + 1] = {};
    int len_ = 0;
    bool overflow_ = false;
};

// Device side: wraps the panel input and pad LEDs. Feed received bytes with
// rx(), call tick() every main-loop pass; the App gets `input` / `leds`.
class DeviceBridge {
public:
    struct Io {
        void (*send)(void* ctx, const char* line) = nullptr;   // one line, no line end
        void (*changed)(void* ctx, bool active) = nullptr;     // remote mode on / off
        void* ctx = nullptr;
    };
    DeviceBridge(IInput& panel, ILeds& pads, const Io& io);

    void rx(const char* data, size_t n, uint32_t now);
    // hostOpen: the host holds the serial port (DTR). Times out a silent host.
    void tick(uint32_t now, bool hostOpen);
    bool active() const { return active_; }
    // Config > USB > Remote control. Disabling ends an active session.
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }

    IInput& input() { return in_; }
    ILeds& leds() { return leds_; }

private:
    class Input final : public IInput {
    public:
        explicit Input(DeviceBridge& b) : b_(b) {}
        bool poll(InputEvent& ev) override;

    private:
        DeviceBridge& b_;
    };
    class Leds final : public ILeds {
    public:
        explicit Leds(DeviceBridge& b) : b_(b) {}
        uint8_t padCount() const override { return b_.pads_.padCount(); }
        void setPad(uint8_t pad, uint8_t r, uint8_t g, uint8_t b) override;
        void clear() override;
        void show() override;

    private:
        DeviceBridge& b_;
    };

    void onLine(const char* line);
    void setActive(bool on);
    // True: the event goes to the host (or is a dropped repeat), not to the app.
    bool forward(const InputEvent& ev);

    IInput& panel_;
    ILeds& pads_;
    Io io_;
    Input in_{*this};
    Leds leds_{*this};
    LineReader reader_;
    bool active_ = false;
    bool enabled_ = false;
    uint32_t now_ = 0, lastHello_ = 0;
    uint64_t buttonsDown_ = 0, padsDown_ = 0;     // panel state, always tracked
    uint64_t localButtons_ = 0, localPads_ = 0;   // held when remote mode began: their release stays local
    uint64_t sentButtons_ = 0, sentPads_ = 0;     // pressed on the host
    uint32_t appColor_[kPads] = {};               // the app's own LED frame, shown again after remote mode
};

}  // namespace remote
}  // namespace ot
