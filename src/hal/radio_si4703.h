// OpenTracker -- Si4703 FM tuner on I2C0 implementing IRadio (spec 01 §5.6,
// spec 22 §2). Register model from the public Si4702/03 datasheet and AN230.
//
// Lifecycle as stock 1.9.2 (spec 22 §2.2-§2.7):
//  * boot(): once, from boardInit after the codec -- reset into 2-wire mode,
//    TEST1 = 0x8100 (crystal on). RST_N then stays HIGH for the whole session.
//  * powerOn(): every radio selection -- once the crystal has run 500 ms,
//    POWERCFG 0xE801 etc., 110 ms, band/spacing/de-emphasis, tune.
//  * powerOff(): RDS off, POWERCFG |= DISABLE. RST_N is not touched.
// Every register write is a read-modify-write that starts with the full
// 32-byte read, as stock. The waits run as a state machine from service(), so
// the main loop never blocks; tune()/seek() issued meanwhile are queued and
// busy() is true until they finish.
//
// The register logic is platform independent: the device glue (I2C0, pins,
// millis, diag log) comes in through Port (radio_si4703_port.cpp), and the host
// tests (tests/radio_si4703_test.cpp) drive it against a fake chip.
#pragma once

#include <stdint.h>
#include "hal_api.h"

namespace ot {
namespace hal {

class Si4703Radio final : public IRadio {
public:
    struct Port {
        // Burst read of n bytes starting at register 0x0A (2-wire mode).
        bool (*read)(uint8_t* data, uint32_t n) = nullptr;
        // Burst write of n bytes starting at register 0x02.
        bool (*write)(const uint8_t* data, uint32_t n) = nullptr;
        uint32_t (*millis)() = nullptr;
        // spec 22 §2.2 steps 2-7: SDIO and RST_N LOW, 1 ms, RST_N HIGH (SDIO
        // still LOW selects 2-wire mode), 1 ms, I2C0 begin.
        void (*resetPulse)() = nullptr;
        void (*log)(const char* fmt, ...) __attribute__((format(printf, 1, 2))) = nullptr;
    };

    // Safety limits. Stock waits for STC without any timeout (spec 22 §2.5);
    // OpenTracker gives up after these and logs the tune/seek as FAILED
    // (README "Differences").
    static constexpr uint32_t kTuneTimeoutMs = 1000;
    static constexpr uint32_t kSeekTimeoutMs = 15000;   // full band at ~60 ms/channel
    static constexpr uint32_t kOscMs = 500;             // spec 22 §2.2 step 10
    static constexpr uint32_t kPowerUpMs = 110;         // spec 22 §2.3 step 6
    static constexpr int kWriteRetries = 10;            // spec 22 §2.1

    void setPort(const Port& p) { port_ = p; }

    // Boot bring-up (spec 22 §2.2): reset + crystal. false = no answer at 0x10
    // (the next powerOn() retries the reset, as the stock failure path).
    bool boot();
    // Starts the asynchronous power-up (spec 22 §2.3). false = the chip did not
    // answer. Already powered: no-op.
    bool powerOn();
    // spec 22 §2.7 power-down; RST_N stays HIGH.
    void powerOff();
    bool powered() const { return state_ >= kOsc; }
    // Region index of the config "Radio region" (spec 05 config table:
    // 0 EU, 1 US, 2 AU, 3 JP). Selects de-emphasis; band/spacing come from
    // setBand(). Applied on the next power-up or setBand().
    void setRegion(uint8_t region) { region_ = region & 3; }
    // Last STATUSRSSI.RSSI (dBuV, 0..75), refreshed by service() about every
    // 500 ms while ready; -1 when off.
    int rssi() const { return powered() ? rssi_ : -1; }
    bool stereo() const { return stereo_; }
    uint16_t deviceId() const { return regs_[0]; }   // 0x1242 expected (datasheet DEVICEID)
    uint32_t errorCount() const { return errors_; }
    uint32_t tuneFailures() const { return tuneFailures_; }   // STC never came (timeout)
    uint32_t tunesDone() const { return tunesDone_; }
    // Diagnostics (board.cpp inputDiag, 1 Hz while the recorder is open): the
    // cached registers as last read/written, no I2C traffic.
    uint8_t stateCode() const { return state_; }
    uint16_t reg(int i) const { return regs_[i & 15]; }

    void service();   // main loop

    // IRadio
    void setBand(float minMHz, float maxMHz, float stepMHz) override;
    bool tune(float mhz) override;
    bool seek(bool up) override;
    bool busy() const override;
    float frequency() const override { return freq_; }
    const char* rdsName() const override { return psName_; }
    uint16_t rdsPi() const override { return pi_; }

    // kUnreset: never reset, or a reset is due after an I2C failure.
    // kStandby: crystal running, not enabled (after boot and after powerOff).
    enum State : uint8_t {
        kUnreset, kStandby, kOsc, kPowerUp, kReady, kTuning, kTuneEnd, kSeeking, kSeekEnd
    };

private:
    bool readAll();                 // spec 22 §2.1: 32 bytes from 0x0A, wrapping
    bool writeRegs(bool retry);     // regs 0x02..0x07 (12 bytes)
    bool applyRegion();             // spec 22 §2.4 (read-modify-write)
    uint16_t channelFor(float mhz) const;
    float freqFor(uint16_t chan) const;
    void startTune();
    void startSeek();
    void logDone(const char* what, uint32_t now);
    void fail(const char* what);
    void decodeRds();
    void clearRds();

    Port port_;
    uint16_t regs_[16] = {};
    State state_ = kUnreset;
    uint32_t t0_ = 0, oscAt_ = 0, lastStatus_ = 0;
    float bandMin_ = 87.5f, bandMax_ = 108.0f, step_ = 0.1f;
    float bottom_ = 87.5f;          // datasheet band bottom (87.5 or 76.0)
    float freq_ = 87.5f;
    float pendingFreq_ = 87.5f;
    bool pendingTune_ = false;
    int8_t pendingSeek_ = 0;        // +1 up, -1 down
    bool timedOut_ = false;         // the STC wait of this tune/seek gave up
    bool regionDirty_ = false;      // setBand() not yet written to the chip
    uint8_t region_ = 0;
    int rssi_ = 0;
    bool stereo_ = false;
    uint32_t errors_ = 0, tuneFailures_ = 0, tunesDone_ = 0;
    // OT fork: RDS programme service name (two matching receptions per segment).
    char psName_[9] = {};
    char psCand_[8] = {};
    uint8_t psSeen_ = 0;            // bit n: segment n confirmed
    uint16_t pi_ = 0;
};

Si4703Radio& radio();

}  // namespace hal
}  // namespace ot
