// [I] Remote control, host side (core/remote/remote.h): a Tracker on its USB
// serial port (e.g. /dev/ttyACM0) as the simulator's front panel. Its
// buttons, pads and encoder feed SimInput; the simulator's pad LEDs go back
// to the Tracker's pads.
#pragma once

#include <cstdint>
#include <string>

#include "host_hal.h"
#include "remote/remote.h"

namespace sim {

class RemoteLink {
public:
    ~RemoteLink() { close(); }
    bool open(const std::string& path);   // false (with a message on stderr) if the port cannot be used
    void close();                          // "@RC BYE"; releases what the Tracker still holds
    bool isOpen() const { return fd_ >= 0; }
    bool active() const { return active_; }   // the Tracker confirmed remote mode
    const std::string& path() const { return path_; }
    // Read the Tracker's lines into `in`, send keepalives and changed LEDs.
    void service(SimInput& in, const SimLeds& leds, uint32_t nowMs);

private:
    void send(const char* line);
    void onLine(const char* line);
    void releaseAll();

    int fd_ = -1;
    std::string path_;
    SimInput* in_ = nullptr;
    ot::remote::LineReader reader_;
    bool active_ = false;
    uint32_t lastHello_ = 0, lastLeds_ = 0;
    bool helloSent_ = false;
    uint8_t sentLevels_[ot::remote::kPads] = {};
    bool ledsValid_ = false;
    uint64_t buttons_ = 0, pads_ = 0;   // held on the Tracker, i.e. down in SimInput
};

}  // namespace sim
