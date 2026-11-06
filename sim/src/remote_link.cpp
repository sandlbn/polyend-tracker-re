#include "remote_link.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace sim {

namespace {
constexpr uint32_t kLedIntervalMs = 40;   // at most 25 LED frames a second
}

bool RemoteLink::open(const std::string& path) {
    close();
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::fprintf(stderr, "remote: cannot open %s: %s\n", path.c_str(), std::strerror(errno));
        return false;
    }
    // Raw bytes (USB CDC: the baud rate is ignored). Opening raises DTR, which
    // tells the Tracker a host is listening.
    termios t{};
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &t);
    }
    fd_ = fd;
    path_ = path;
    helloSent_ = false;
    ledsValid_ = false;
    std::fprintf(stderr, "remote: %s open, waiting for the Tracker\n", path.c_str());
    return true;
}

void RemoteLink::close() {
    if (fd_ < 0) return;
    send("@RC BYE");
    releaseAll();
    ::close(fd_);
    fd_ = -1;
    active_ = false;
}

void RemoteLink::send(const char* line) {
    if (fd_ < 0) return;
    char buf[ot::remote::kLineMax + 2];
    const int n = std::snprintf(buf, sizeof buf, "%s\n", line);
    if (n <= 0 || ::write(fd_, buf, size_t(n)) != n) {
        if (errno != EAGAIN) {
            std::fprintf(stderr, "remote: %s: write failed (%s), closing\n", path_.c_str(), std::strerror(errno));
            releaseAll();
            ::close(fd_);
            fd_ = -1;
            active_ = false;
        }
    }
}

void RemoteLink::releaseAll() {
    if (!in_) return;
    for (uint8_t i = 0; i < 64; ++i) {
        if ((buttons_ >> i) & 1) in_->button(i, false);
        if ((pads_ >> i) & 1) in_->pad(i, false);
    }
    buttons_ = pads_ = 0;
}

void RemoteLink::onLine(const char* line) {
    ot::remote::Msg m;
    if (!ot::remote::parse(line, m)) return;
    switch (m.kind) {
        case ot::remote::Msg::kOn:
            if (!active_) std::fprintf(stderr, "remote: Tracker panel connected\n");
            active_ = true;
            ledsValid_ = false;   // resend the LEDs
            break;
        case ot::remote::Msg::kOff:
            if (active_) std::fprintf(stderr, "remote: Tracker panel released\n");
            active_ = false;
            releaseAll();
            break;
        case ot::remote::Msg::kButtonDown:
            buttons_ |= 1ull << m.id;
            in_->button(m.id, true);
            break;
        case ot::remote::Msg::kButtonUp:
            buttons_ &= ~(1ull << m.id);
            in_->button(m.id, false);
            break;
        case ot::remote::Msg::kPadDown:
            pads_ |= 1ull << m.id;
            in_->pad(m.id, true, m.velocity ? m.velocity : 100);
            break;
        case ot::remote::Msg::kPadUp:
            pads_ &= ~(1ull << m.id);
            in_->pad(m.id, false);
            break;
        case ot::remote::Msg::kEncoder: in_->encoder(m.delta); break;
        default: break;
    }
}

void RemoteLink::service(SimInput& in, const SimLeds& leds, uint32_t now) {
    if (fd_ < 0) return;
    in_ = &in;
    char buf[512];
    for (;;) {
        const ssize_t n = ::read(fd_, buf, sizeof buf);
        if (n > 0) {
            reader_.feed(buf, size_t(n), [](void* self, const char* l) { static_cast<RemoteLink*>(self)->onLine(l); },
                         this);
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            std::fprintf(stderr, "remote: %s: read failed (%s), closing\n", path_.c_str(), std::strerror(errno));
            releaseAll();
            ::close(fd_);
            fd_ = -1;
            active_ = false;
            return;
        }
        break;
    }
    if (!helloSent_ || now - lastHello_ >= ot::remote::kKeepaliveMs) {
        helloSent_ = true;
        lastHello_ = now;
        send("@RC HELLO");
    }
    if (!active_ || now - lastLeds_ < kLedIntervalMs) return;
    // The Tracker's pads have one colour: the brightest channel is the level.
    uint8_t lv[ot::remote::kPads];
    for (int i = 0; i < ot::remote::kPads; ++i) {
        const uint32_t c = leds.color(i);
        const uint8_t r = uint8_t(c >> 16), g = uint8_t(c >> 8), b = uint8_t(c);
        lv[i] = r > g ? (r > b ? r : b) : (g > b ? g : b);
    }
    if (ledsValid_ && std::memcmp(lv, sentLevels_, sizeof lv) == 0) return;
    char line[ot::remote::kLineMax];
    if (!ot::remote::formatLeds(lv, line, sizeof line)) return;
    send(line);
    std::memcpy(sentLevels_, lv, sizeof lv);
    ledsValid_ = true;
    lastLeds_ = now;
}

}  // namespace sim
