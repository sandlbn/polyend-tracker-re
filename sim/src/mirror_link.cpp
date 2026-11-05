#include "mirror_link.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include "formats/crc32.h"
#include "remote/display_tap.h"

namespace sim {

namespace {
int b64(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
// Appends the bytes of base64 text `s` to out; false on a bad character.
bool unbase64(const char* s, std::vector<uint8_t>& out) {
    uint32_t acc = 0;
    int bits = 0;
    for (; *s && *s != '='; ++s) {
        const int v = b64(*s);
        if (v < 0) return false;
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
        }
    }
    return true;
}
}  // namespace

bool MirrorLink::open(const std::string& path, const std::string& recordDir) {
    close();
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::fprintf(stderr, "mirror: cannot open %s: %s\n", path.c_str(), std::strerror(errno));
        return false;
    }
    termios t{};
    if (tcgetattr(fd, &t) == 0) {   // raw bytes; opening raises DTR
        cfmakeraw(&t);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &t);
    }
    fd_ = fd;
    path_ = path;
    record_ = recordDir;
    driving_ = false;
    lastHello_ = 0;
    std::fprintf(stderr, "mirror: %s open, waiting for the Tracker (Config > USB > Remote control On)\n",
                 path.c_str());
    return true;
}

void MirrorLink::close() {
    if (fd_ < 0) return;
    send("@RC BYE");
    ::close(fd_);
    fd_ = -1;
    driving_ = false;
}

void MirrorLink::send(const char* line) {
    if (fd_ < 0) return;
    char buf[ot::remote::kLineMax + 2];
    const int n = std::snprintf(buf, sizeof buf, "%s\n", line);
    if (n > 0 && ::write(fd_, buf, size_t(n)) != n && errno != EAGAIN) {
        std::fprintf(stderr, "mirror: %s: write failed (%s), closing\n", path_.c_str(), std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        driving_ = false;
    }
}

void MirrorLink::onLine(const char* line, ot::IDisplay& display) {
    const char* p = std::strstr(line, "@RC ");
    if (!p) return;
    p += 4;
    if (!std::strcmp(p, "DRIVING")) {
        if (!driving_) std::fprintf(stderr, "mirror: driving the Tracker\n");
        driving_ = true;
        send("@RC MIRROR 1");
    } else if (!std::strcmp(p, "OFF")) {
        driving_ = false;
        inFrame_ = false;
    } else if (!std::strcmp(p, "F FAIL")) {
        std::fprintf(stderr, "mirror: the Tracker cannot export its display\n");
    } else if (!std::strncmp(p, "F END ", 6)) {
        if (!inFrame_) return;
        inFrame_ = false;
        unsigned long bytes = 0, crc = 0;
        if (std::sscanf(p + 6, "%lu %lx", &bytes, &crc) != 2 || bytes != frame_.size() ||
            ot::crc32Final(ot::crc32Update(ot::crc32Init(), frame_.data(), frame_.size())) != uint32_t(crc)) {
            ++damaged_;
            return;
        }
        lastBytes_ = uint32_t(bytes);
        ++frames_;
        ot::remote::replayFrame(frame_.data(), frame_.size(), display);
        if (!record_.empty()) {
            char name[64];
            std::snprintf(name, sizeof name, "/frame-%06u.otf", unsigned(seq_));
            if (FILE* f = std::fopen((record_ + name).c_str(), "wb")) {
                std::fwrite(frame_.data(), 1, frame_.size(), f);
                std::fclose(f);
            }
        }
    } else if (!std::strncmp(p, "F ", 2)) {
        unsigned long seq = 0;
        std::sscanf(p + 2, "%lu", &seq);
        seq_ = uint32_t(seq);
        frame_.clear();
        inFrame_ = true;
    } else if (!std::strncmp(p, "D ", 2)) {
        if (inFrame_ && !unbase64(p + 2, frame_)) inFrame_ = false;
    }
}

void MirrorLink::service(ot::IInput& in, ot::IDisplay& display, uint32_t nowMs) {
    now_ = nowMs;
    if (fd_ < 0) return;
    char buf[4096];
    for (ssize_t n; fd_ >= 0 && (n = ::read(fd_, buf, sizeof buf)) > 0;) {
        struct Ctx {
            MirrorLink* self;
            ot::IDisplay* d;
        } ctx{this, &display};
        reader_.feed(buf, size_t(n),
                     [](void* c, const char* l) {
                         auto* x = static_cast<Ctx*>(c);
                         x->self->onLine(l, *x->d);
                     },
                     &ctx);
    }
    if (nowMs - lastHello_ >= ot::remote::kKeepaliveMs) {
        lastHello_ = nowMs;
        send("@RC DRIVE");
    }
    ot::InputEvent ev;
    while (in.poll(ev)) {
        char line[32];
        if (driving_ && ot::remote::formatEvent(ev, line, sizeof line)) send(line);
    }
    if (nowMs - fpsStart_ >= 1000) {
        fps_ = float(frames_) * 1000.0f / float(nowMs - fpsStart_);
        frames_ = 0;
        fpsStart_ = nowMs;
    }
}

}  // namespace sim
