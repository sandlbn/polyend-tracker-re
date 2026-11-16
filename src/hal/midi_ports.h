// OpenTracker -- MIDI DIN (UART4 = Teensy Serial5) and USB-MIDI device (spec 01 §8, §9).
#pragma once

#include <stdint.h>
#include "hal_api.h"
#include "midi_txq.h"

namespace ot {
namespace hal {

class MidiPorts final : public IMidiPort {
public:
    void init();
    // Main-loop drain of the TX queues (called from hal::service()).
    void service();
    // DIN only, as much as the UART ring takes (never waits). Also called at
    // the end of every audio block (audio TX ISR), so MIDI clock out keeps
    // the block timing while the main loop is busy (a screen's first frame,
    // a sample loaded for a pad audition). Any context.
    void pumpDin();
    // DIN MIDI clock (0xF8) latency since the last call: the longest time
    // from queued to handed to the UART (us) and the clocks sent.
    void takeDinClockStats(uint32_t* lateMaxUs, uint32_t* count);
    // Safe from any context incl. the audio DMA ISR (App::preBlock -> sequencer ->
    // engine MIDI out): only copies into a queue. false + drop count on overflow.
    bool send(MidiPortId port, const uint8_t* data, uint32_t len) override;
    uint32_t dropped() const { return dropped_; }
    uint32_t receive(MidiPortId port, uint8_t* dst, uint32_t cap) override;

private:
    // Raw byte stream -> USB-MIDI event packets (USB Device Class for MIDI 1.0 §4).
    void usbEncode(uint8_t b);
    void usbEmit(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2);

    uint8_t txStatus_ = 0;      // running status of the stream being encoded
    uint8_t txBuf_[3] = {};
    uint8_t txLen_ = 0;
    uint8_t txNeed_ = 0;
    bool txSysex_ = false;

    // TX queues, one per port: multi-producer (ISR + main) via a short IRQ-off
    // section. DIN: pumpDin() (block end + main loop, IRQs off); USB: service().
    MidiTxQueue txq_[2];
    static constexpr uint32_t kDinByteUs = 320;   // 10 bits at 31250 Bd
    volatile uint32_t dropped_ = 0;

    uint8_t rxPend_[3] = {};    // bytes of a received USB packet not yet handed out
    uint8_t rxPendLen_ = 0, rxPendPos_ = 0;
};

}  // namespace hal
}  // namespace ot
