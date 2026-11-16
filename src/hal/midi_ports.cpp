// OpenTracker -- MIDI ports (spec 01 §8 DIN on UART4, §9 USB-MIDI via the Teensy core).
#include "midi_ports.h"

#include <Arduino.h>
#include "board.h"
#include "hw_math.h"

// spec 01 §8: UART4 (Serial5) is clocked from the bus clock; Teensy's
// HardwareSerial5 divides F_BUS (BAUD2DIV3) -> SBR 180, BRFA 0 at 90 MHz.
static_assert(F_BUS == ot::board::kBusHz, "Teensy F_BUS must match the spec 01 bus clock");
static_assert(ot::hal::hw::uartDivisor32(F_BUS, ot::board::kMidiBaud) == 5760, "spec 01 §8 divisor");

extern "C" {
void usb_midi_write_packed(uint32_t n);
void usb_midi_flush_output(void);
uint32_t usb_midi_read_message(void);
extern volatile uint8_t usb_configuration;
}

namespace ot {
namespace hal {

void MidiPorts::init() {
    // spec 01 §8: 31250 Bd 8N1, TX inverted (UART4_C3.TXINV, format flag 0x20).
    // UNVERIFIED [O/I]: inversion is what the stock firmware programs; the reason
    // (inverting driver stage) is inferred.
    Serial5.begin(board::kMidiBaud, SERIAL_8N1_TXINV);
}

void MidiPorts::usbEmit(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2) {
    // cable 0
    usb_midi_write_packed(uint32_t(cin & 0x0F) | (uint32_t(b0) << 8) | (uint32_t(b1) << 16) |
                          (uint32_t(b2) << 24));
}

void MidiPorts::usbEncode(uint8_t b) {
    if (b >= 0xF8) {                       // real-time: single byte, may interleave anything
        usbEmit(0x0F, b, 0, 0);
        return;
    }
    if (b == 0xF0) {
        txSysex_ = true;
        txBuf_[0] = b;
        txLen_ = 1;
        return;
    }
    if (txSysex_) {
        if (b == 0xF7) {
            txBuf_[txLen_++] = b;
            static const uint8_t endCin[4] = {0, 0x5, 0x6, 0x7};
            usbEmit(endCin[txLen_], txBuf_[0], txLen_ > 1 ? txBuf_[1] : 0, txLen_ > 2 ? txBuf_[2] : 0);
            txSysex_ = false;
            txLen_ = 0;
            return;
        }
        if (b & 0x80) {                    // aborted sysex: drop and fall through
            txSysex_ = false;
            txLen_ = 0;
        } else {
            txBuf_[txLen_++] = b;
            if (txLen_ == 3) { usbEmit(0x4, txBuf_[0], txBuf_[1], txBuf_[2]); txLen_ = 0; }
            return;
        }
    }
    if (b & 0x80) {
        txLen_ = 0;
        if (b < 0xF0) {
            txStatus_ = b;
            txNeed_ = ((b & 0xE0) == 0xC0) ? 1 : 2;
        } else {
            txStatus_ = 0;                 // system common clears running status
            switch (b) {
            case 0xF1: case 0xF3: txStatus_ = b; txNeed_ = 1; break;
            case 0xF2: txStatus_ = b; txNeed_ = 2; break;
            case 0xF6: usbEmit(0x5, b, 0, 0); break;
            default: break;                // 0xF4/0xF5/stray 0xF7: ignore
            }
        }
        return;
    }
    if (!txStatus_) return;                // data without status
    txBuf_[txLen_++] = b;
    if (txLen_ < txNeed_) return;
    uint8_t cin;
    if (txStatus_ < 0xF0) cin = uint8_t(txStatus_ >> 4);
    else cin = (txNeed_ == 1) ? 0x2 : 0x3;
    usbEmit(cin, txStatus_, txBuf_[0], txNeed_ > 1 ? txBuf_[1] : 0);
    txLen_ = 0;
    if (txStatus_ >= 0xF0) txStatus_ = 0;
}

bool MidiPorts::send(MidiPortId port, const uint8_t* data, uint32_t len) {
    // Never touches the UART or USB here: usb_midi_write_packed() can wait (and call
    // yield()) for up to tens of ms, which must not happen in the audio ISR.
    if (port != MidiPortId::kDin && !usb_configuration) return false;
    MidiTxQueue& q = txq_[port == MidiPortId::kDin ? 0 : 1];
    // micros() re-enables IRQs (Teensy core): read the time before masking.
    const uint32_t now = micros();
    uint32_t primask;
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(primask)::"memory");
    const bool ok = q.push(data, len, now);
    if (!ok) dropped_ = dropped_ + 1;
    __asm__ volatile("msr primask, %0" ::"r"(primask) : "memory");
    return ok;
}

void MidiPorts::pumpDin() {
    // micros() re-enables IRQs (Teensy core): read the time before masking.
    const uint32_t now = micros();
    uint32_t primask;
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(primask)::"memory");
    MidiTxQueue& d = txq_[0];
    if (d.tail != d.head) {
        // Only what the UART ring has room for: Serial5.write() never waits.
        // The bytes still in the ring go out first (320 us each at 31250 Bd).
        const int room = Serial5.availableForWrite();
        if (room > 0)
            d.pump(uint32_t(room), now, [](uint8_t b) { Serial5.write(b); },
                   uint32_t(SERIAL5_TX_BUFFER_SIZE - 1 - room), kDinByteUs);
    }
    __asm__ volatile("msr primask, %0" ::"r"(primask) : "memory");
}

void MidiPorts::takeDinClockStats(uint32_t* lateMaxUs, uint32_t* count) {
    uint32_t primask;
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(primask)::"memory");
    txq_[0].takeClockStats(lateMaxUs, count);
    __asm__ volatile("msr primask, %0" ::"r"(primask) : "memory");
}

void MidiPorts::service() {
    pumpDin();
    // USB: encode to event packets; discarded while the host is not configured.
    MidiTxQueue& u = txq_[1];
    if (u.tail == u.head) return;
    bool configured = usb_configuration;
    while (u.tail != u.head) {
        uint8_t b = u.buf[u.tail & (MidiTxQueue::kSize - 1)];
        u.tail = u.tail + 1;
        if (configured) usbEncode(b);
    }
    if (configured) usb_midi_flush_output();
}

uint32_t MidiPorts::receive(MidiPortId port, uint8_t* dst, uint32_t cap) {
    uint32_t n = 0;
    if (port == MidiPortId::kDin) {
        while (n < cap && Serial5.available() > 0) dst[n++] = uint8_t(Serial5.read());
        return n;
    }
    // CIN -> byte count (USB MIDI 1.0 table 4-1); 0 = reserved/ignored.
    static const uint8_t kLen[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    while (n < cap) {
        if (rxPendPos_ < rxPendLen_) { dst[n++] = rxPend_[rxPendPos_++]; continue; }
        uint32_t p = usb_midi_read_message();
        if (!p) break;
        uint8_t len = kLen[p & 0x0F];
        rxPend_[0] = uint8_t(p >> 8);
        rxPend_[1] = uint8_t(p >> 16);
        rxPend_[2] = uint8_t(p >> 24);
        rxPendLen_ = len;
        rxPendPos_ = 0;
    }
    return n;
}

}  // namespace hal
}  // namespace ot
