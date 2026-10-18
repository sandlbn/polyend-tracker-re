// [I] Remote control (core/remote/remote.h): the line protocol and the
// device-side bridge between the panel drivers and the App.
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "remote/remote.h"
#include "ui/buttons.h"
#include "ui/screens.h"

using namespace ot;
using namespace ot::remote;

namespace {

struct FakePanel : IInput {
    std::deque<InputEvent> q;
    bool poll(InputEvent& ev) override {
        if (q.empty()) return false;
        ev = q.front();
        q.pop_front();
        return true;
    }
    void push(InputEvent::Type t, uint8_t id, uint8_t vel = 0, int8_t d = 0) {
        InputEvent e;
        e.type = t;
        e.id = id;
        e.velocity = vel;
        e.delta = d;
        q.push_back(e);
    }
};

struct PadLedsFake : ILeds {
    uint32_t pending[48] = {}, shown[48] = {};
    int shows = 0;
    uint8_t padCount() const override { return 48; }
    void setPad(uint8_t p, uint8_t r, uint8_t g, uint8_t b) override {
        if (p < 48) pending[p] = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    }
    void clear() override { std::memset(pending, 0, sizeof pending); }
    void show() override {
        std::memcpy(shown, pending, sizeof shown);
        ++shows;
    }
};

struct Rig {
    FakePanel panel;
    PadLedsFake leds;
    std::vector<std::string> sent;
    std::vector<bool> changes;
    DeviceBridge bridge;
    uint32_t now = 1000;
    static DeviceBridge::Io io(Rig* r) {
        DeviceBridge::Io o;
        o.send = [](void* c, const char* l) { static_cast<Rig*>(c)->sent.push_back(l); };
        o.changed = [](void* c, bool on) { static_cast<Rig*>(c)->changes.push_back(on); };
        o.ctx = r;
        return o;
    }
    explicit Rig(bool enabled = true) : bridge(panel, leds, io(this)) { bridge.setEnabled(enabled); }
    void host(const char* line) {
        std::string s = line;
        s += "\n";
        bridge.rx(s.data(), s.size(), now);
    }
    // Events the App receives.
    std::vector<InputEvent> app() {
        std::vector<InputEvent> v;
        InputEvent e;
        while (bridge.input().poll(e)) v.push_back(e);
        return v;
    }
};

}  // namespace

TEST(remote_parse_and_format) {
    Msg m;
    CHECK(parse("@RC HELLO", m) && m.kind == Msg::kHello);
    CHECK(parse("@RC BYE", m) && m.kind == Msg::kBye);
    // Device lines carry the diag timestamp in front.
    CHECK(parse("[   12.345] @RC ON", m) && m.kind == Msg::kOn);
    CHECK(parse("@RC B1 23", m) && m.kind == Msg::kButtonDown && m.id == 23);
    CHECK(parse("@RC B0 0", m) && m.kind == Msg::kButtonUp && m.id == 0);
    CHECK(parse("@RC P1 47 100", m) && m.kind == Msg::kPadDown && m.id == 47 && m.velocity == 100);
    CHECK(parse("@RC P0 5", m) && m.kind == Msg::kPadUp && m.id == 5);
    CHECK(parse("@RC E -12", m) && m.kind == Msg::kEncoder && m.delta == -12);
    // Malformed / out of range.
    CHECK(!parse("RC B1 3", m));
    CHECK(!parse("@RC B1", m));
    CHECK(!parse("@RC B1 3x", m));
    CHECK(!parse("@RC P1 48 10", m));
    CHECK(!parse("@RC P1 3 200", m));
    CHECK(!parse("@RC E 0", m));
    CHECK(!parse("@RC E 300", m));
    CHECK(!parse("@RC NOPE", m));
    // Event round trip.
    InputEvent e;
    char b[40];
    e.type = InputEvent::kPadDown;
    e.id = 13;
    e.velocity = 90;
    CHECK(formatEvent(e, b, sizeof b) > 0);
    CHECK_STR(b, "@RC P1 13 90");
    e.type = InputEvent::kEncoder;
    e.delta = -3;
    formatEvent(e, b, sizeof b);
    CHECK(parse(b, m) && m.kind == Msg::kEncoder && m.delta == -3);
    e.delta = 0;
    CHECK_EQ(formatEvent(e, b, sizeof b), 0);
    // LEDs.
    uint8_t lv[kPads];
    for (int i = 0; i < kPads; ++i) lv[i] = uint8_t(i * 5);
    char l[kLineMax];
    CHECK_EQ(formatLeds(lv, l, sizeof l), 6 + 96);
    CHECK(parse(l, m) && m.kind == Msg::kLeds);
    CHECK(std::memcmp(m.levels, lv, sizeof lv) == 0);
    l[20] = 'g';
    CHECK(!parse(l, m));
    CHECK_EQ(formatLeds(lv, l, 50), 0);
}

TEST(remote_line_reader) {
    LineReader r;
    std::vector<std::string> got;
    auto fn = [](void* c, const char* l) { static_cast<std::vector<std::string>*>(c)->push_back(l); };
    const char a[] = "@RC HEL";
    const char b[] = "LO\r\n\n@RC B1 2\n";
    r.feed(a, sizeof a - 1, fn, &got);
    CHECK(got.empty());
    r.feed(b, sizeof b - 1, fn, &got);
    CHECK_EQ(got.size(), 2u);
    if (got.size() == 2) {
        CHECK_STR(got[0].c_str(), "@RC HELLO");
        CHECK_STR(got[1].c_str(), "@RC B1 2");
    }
    // An over-long line is dropped whole, the next one is fine.
    std::string big(kLineMax + 10, 'x');
    big += "\n@RC BYE\n";
    got.clear();
    r.feed(big.data(), big.size(), fn, &got);
    CHECK_EQ(got.size(), 1u);
    if (!got.empty()) CHECK_STR(got[0].c_str(), "@RC BYE");
}

TEST(remote_bridge_passes_through_without_host) {
    Rig r;
    r.panel.push(InputEvent::kButtonDown, 5);
    r.panel.push(InputEvent::kPadDown, 7, 100);
    auto ev = r.app();
    CHECK_EQ(ev.size(), 2u);
    CHECK(r.sent.empty());
    r.bridge.leds().setPad(3, 0, 20, 0);
    r.bridge.leds().show();
    CHECK_EQ(r.leds.shown[3], 20u << 8);
    // Not a remote line: ignored.
    r.host("hello");
    r.bridge.tick(r.now, true);
    CHECK(!r.bridge.active());
}

TEST(remote_bridge_forwards_while_active) {
    Rig r;
    r.bridge.leds().setPad(0, 9, 9, 9);   // the app's own frame
    r.bridge.leds().show();
    r.host("@RC HELLO");
    CHECK(r.bridge.active());
    CHECK_EQ(r.changes.size(), 1u);
    CHECK(!r.sent.empty() && r.sent.back() == "@RC ON");
    CHECK_EQ(r.leds.shown[0], 0u);   // pads cleared for the host
    r.sent.clear();
    r.panel.push(InputEvent::kButtonDown, 12);
    r.panel.push(InputEvent::kButtonDown, 12);   // driver auto-repeat: dropped
    r.panel.push(InputEvent::kButtonUp, 12);
    r.panel.push(InputEvent::kPadDown, 30, 77);
    r.panel.push(InputEvent::kPadUp, 30);
    r.panel.push(InputEvent::kEncoder, 0, 0, -2);
    CHECK(r.app().empty());   // all to the host (the power switch: remote_bridge_enable_switch_and_power_exit)
    const std::vector<std::string> want = {"@RC B1 12", "@RC B0 12", "@RC P1 30 77", "@RC P0 30", "@RC E -2"};
    CHECK(r.sent == want);
    // Host LEDs go to the pads; the app's frames are kept aside.
    std::string leds = "@RC L ";
    for (int i = 0; i < kPads; ++i) leds += i == 4 ? "80" : "00";
    r.host(leds.c_str());
    CHECK_EQ(r.leds.shown[4], 0x808080u);
    r.bridge.leds().setPad(0, 0, 31, 0);
    r.bridge.leds().show();
    CHECK_EQ(r.leds.shown[0], 0u);
    // Keepalives hold it; silence times it out and restores the app's LEDs.
    r.now += kTimeoutMs - 100;
    r.host("@RC HELLO");
    r.now += kTimeoutMs - 100;
    r.bridge.tick(r.now, true);
    CHECK(r.bridge.active());
    r.now += 200;
    r.bridge.tick(r.now, true);
    CHECK(!r.bridge.active());
    CHECK(r.sent.back() == "@RC OFF");
    CHECK_EQ(r.leds.shown[0], 31u << 8);
    CHECK_EQ(r.leds.shown[4], 0u);
    // Closing the port (DTR) and BYE also end it.
    r.host("@RC HELLO");
    r.bridge.tick(r.now, false);
    CHECK(!r.bridge.active());
    r.host("@RC HELLO");
    r.host("@RC BYE");
    CHECK(!r.bridge.active());
    CHECK_EQ(r.changes.size(), 6u);   // on, off (timeout), on, off (DTR), on, off (BYE)
}

TEST(remote_bridge_keys_held_across_the_switch) {
    Rig r;
    // Held by the app when remote mode starts: its release goes to the app.
    r.panel.push(InputEvent::kButtonDown, 3);
    CHECK_EQ(r.app().size(), 1u);
    r.host("@RC HELLO");
    r.sent.clear();
    r.panel.push(InputEvent::kButtonDown, 3);   // repeat while held
    r.panel.push(InputEvent::kButtonUp, 3);
    auto ev = r.app();
    CHECK_EQ(ev.size(), 1u);
    if (!ev.empty()) CHECK(ev[0].type == InputEvent::kButtonUp && ev[0].id == 3);
    CHECK(r.sent.empty());
    // Held on the host when remote mode ends: the app never sees its release.
    r.panel.push(InputEvent::kPadDown, 9, 50);
    r.app();
    r.host("@RC BYE");
    r.sent.clear();
    r.panel.push(InputEvent::kPadUp, 9);
    ev = r.app();
    CHECK_EQ(ev.size(), 1u);   // the app gets an unmatched release (harmless: pads ignore it)
    CHECK(r.sent.empty());
    // Pressed while off, released while on: nothing to the host.
    r.panel.push(InputEvent::kButtonDown, 6);
    r.app();
    r.host("@RC HELLO");
    r.sent.clear();
    r.panel.push(InputEvent::kButtonUp, 6);
    ev = r.app();
    CHECK_EQ(ev.size(), 1u);
    CHECK(r.sent.empty());
}

TEST(remote_bridge_enable_switch_and_power_exit) {
    Rig r(false);
    CHECK(!r.bridge.enabled());
    r.host("@RC HELLO");   // Config > USB > Remote control is Off: ignored
    CHECK(!r.bridge.active());
    CHECK(r.sent.empty());
    r.bridge.setEnabled(true);
    r.host("@RC HELLO");
    CHECK(r.bridge.active());
    // Turning it off in Config ends the session.
    r.bridge.setEnabled(false);
    CHECK(!r.bridge.active());
    CHECK(r.sent.back() == "@RC OFF");
    r.host("@RC HELLO");
    CHECK(!r.bridge.active());
    // A short power press while the host drives the panel hands it back and
    // keeps it back (the host's keepalives no longer re-enter).
    r.bridge.setEnabled(true);
    r.host("@RC HELLO");
    r.panel.push(InputEvent::kButtonDown, btn::Power);
    r.panel.push(InputEvent::kButtonUp, btn::Power);
    auto ev = r.app();
    CHECK_EQ(ev.size(), 2u);   // the app still sees the switch (power popup, cancelled on release)
    CHECK(!r.bridge.active());
    CHECK(!r.bridge.enabled());
    r.host("@RC HELLO");
    CHECK(!r.bridge.active());
    r.panel.push(InputEvent::kButtonDown, 12);
    CHECK_EQ(r.app().size(), 1u);   // the panel works locally again
}

TEST(remote_config_row_switches_the_bridge) {
    Harness h;
    App& a = *h.app;
    FakePanel panel;
    PadLedsFake leds;
    DeviceBridge b(panel, leds, DeviceBridge::Io{});
    a.setRemoteBridge(&b);
    // Config > USB (4th category) > Remote control (2nd row).
    auto open = [&] {
        h.tap(btn::Config);
        for (int i = 0; i < 3; ++i) h.tap(btn::Down);
        h.tap(btn::Right);
        h.tap(btn::Down);
    };
    open();
    ConfigScreen& c = *static_cast<ConfigScreen*>(a.ui.screen(kModConfig));
    char t[48];
    c.optionText(3, 1, t, sizeof t);
    CHECK_STR(t, "Off");
    h.tap(btn::F8);
    h.tap(btn::Down);   // On
    h.tap(btn::F8);
    CHECK(b.enabled());
    c.optionText(3, 1, t, sizeof t);
    CHECK_STR(t, "On");
    h.tap(btn::F8);
    h.tap(btn::Up);     // Off
    h.tap(btn::F8);
    CHECK(!b.enabled());
    // Without a bridge (simulator): stays Off with a notice.
    a.setRemoteBridge(nullptr);
    h.tap(btn::F8);
    h.tap(btn::Down);
    h.tap(btn::F8);
    c.optionText(3, 1, t, sizeof t);
    CHECK_STR(t, "Off");
}
