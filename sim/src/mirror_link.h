// [I] Display mirror, host side (core/remote/display_tap.h): shows a
// Tracker's screen live from its display commands ("@RC MIRROR 1" in drive
// mode) and drives it with the simulator's keys, mouse and wheel.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "host_hal.h"
#include "remote/remote.h"

namespace sim {

class MirrorLink {
public:
    ~MirrorLink() { close(); }
    // recordDir: also save every frame there as frame-<seq>.otf ("" = no).
    bool open(const std::string& path, const std::string& recordDir);
    void close();   // "@RC BYE"
    bool isOpen() const { return fd_ >= 0; }
    bool driving() const { return driving_; }
    const std::string& path() const { return path_; }
    // Reads the Tracker's lines (a completed frame is drawn on `display`),
    // sends keepalives and the input polled from `in`.
    void service(ot::IInput& in, ot::IDisplay& display, uint32_t nowMs);
    // Frames a second over the last second, the last frame's size.
    float fps() const { return fps_; }
    uint32_t lastBytes() const { return lastBytes_; }
    uint32_t damaged() const { return damaged_; }

private:
    void send(const char* line);
    void onLine(const char* line, ot::IDisplay& display);

    int fd_ = -1;
    std::string path_, record_;
    ot::remote::LineReader reader_;
    bool driving_ = false;
    uint32_t now_ = 0, lastHello_ = 0;
    bool inFrame_ = false;
    std::vector<uint8_t> frame_;
    uint32_t seq_ = 0;
    uint32_t lastBytes_ = 0, damaged_ = 0, frames_ = 0, fpsStart_ = 0;
    float fps_ = 0.0f;
};

}  // namespace sim
