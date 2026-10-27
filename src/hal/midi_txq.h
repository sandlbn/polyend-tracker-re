// OpenTracker -- MIDI TX byte queue of one port (MidiPorts). Platform
// independent (host tests). Producers: MidiPorts::send() from any context, in
// a short IRQ-off section. Consumers: pump(), also with IRQs off, so the audio
// block end and the main loop can both drain the DIN port.
#pragma once

#include <stdint.h>

namespace ot {
namespace hal {

struct MidiTxQueue {
    static constexpr uint32_t kSize = 1024;   // power of two
    uint8_t buf[kSize];
    volatile uint32_t head = 0, tail = 0;

    // MIDI clock (0xF8) latency, queued -> on the wire: the oldest unsent
    // clock byte's position and time, the window maximum and count.
    uint32_t clkPos = 0, clkAtUs = 0;
    bool clkPending = false;
    uint32_t clkLateMaxUs = 0, clkCount = 0;

    uint32_t used() const { return head - tail; }

    // false (nothing queued) when the bytes do not all fit.
    bool push(const uint8_t* d, uint32_t n, uint32_t nowUs) {
        const uint32_t h = head;
        if (n > kSize - (h - tail)) return false;
        for (uint32_t i = 0; i < n; ++i) {
            buf[(h + i) & (kSize - 1)] = d[i];
            if (d[i] == 0xF8 && !clkPending) {
                clkPending = true;
                clkPos = h + i;
                clkAtUs = nowUs;
            }
        }
        head = h + n;
        return true;
    }

    // Hands up to `room` bytes to put(b), oldest first. Returns the count.
    // aheadBytes: bytes already waiting in the port's own buffer, byteUs: one
    // byte's time on the wire; a clock's latency includes the bytes before it.
    template <class Put>
    uint32_t pump(uint32_t room, uint32_t nowUs, Put put, uint32_t aheadBytes = 0, uint32_t byteUs = 0) {
        uint32_t n = 0;
        while (n < room && tail != head) {
            const uint32_t t = tail;
            const uint8_t b = buf[t & (kSize - 1)];
            put(b);
            tail = t + 1;
            ++n;
            if (b == 0xF8) {
                ++clkCount;
                if (clkPending && t == clkPos) {
                    // A clock queued (by a preempting ISR) after the caller
                    // read nowUs has waited 0, not a wrapped negative time.
                    const int32_t waited = int32_t(nowUs - clkAtUs);
                    const uint32_t late = (waited > 0 ? uint32_t(waited) : 0u) + (aheadBytes + n - 1) * byteUs;
                    if (late > clkLateMaxUs) clkLateMaxUs = late;
                    clkPending = false;
                }
            }
        }
        return n;
    }

    // Window statistics for the telemetry line; reset on read.
    void takeClockStats(uint32_t* lateMaxUs, uint32_t* count) {
        *lateMaxUs = clkLateMaxUs;
        *count = clkCount;
        clkLateMaxUs = 0;
        clkCount = 0;
    }
};

}  // namespace hal
}  // namespace ot
