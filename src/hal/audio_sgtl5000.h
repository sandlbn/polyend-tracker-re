// OpenTracker -- SGTL5000 codec + I2S0 master + DMA double buffer (spec 01 §5).
#pragma once

#include <stdint.h>
#include "hal_api.h"
#include "audio_sgtl5000_dap.h"

namespace ot {
namespace hal {

class Sgtl5000Audio final : public IAudioOut {
public:
    // Brings up I2S0 clocks (MCLK must run before the codec answers on I2C),
    // the TX DMA ring, then the codec enable sequence. Output stays muted
    // (mute pin LOW) until start().
    bool init();
    bool codecOk() const { return codecOk_; }
    uint16_t chipId() const { return chipId_; }   // CHIP_ID read at init (0xA0xx), 0 = no answer

    // spec 01 §5.1 wrapper operations.
    void setHeadphoneVolume(float v);       // 0..1, 0 = HP muted
    bool codecWrite(uint16_t reg, uint16_t val);
    bool codecRead(uint16_t reg, uint16_t& val);
    void powerDown();                        // spec 01 §5.1 shutdown: ANA_POWER = 0

    // spec 01 §5.1 wrapper inputSelect/lineInLevel/micGain. ADC from the line
    // input at `level` (0..15, CHIP_ANA_ADC_CTRL both channels).
    bool selectLineIn(uint8_t level);
    // ADC from the mic input with bias on and `gainDb` split into the MIC_CTRL
    // preamp step + ADC analog gain (spec 01 §5.1 micGain).
    bool selectMic(uint8_t gainDb);
    // The gain calls alone (spec 22 §3 0x56640, on a recorder gain change).
    bool lineInLevel(uint8_t level);
    bool micGain(uint8_t gainDb);

    // Line input (I2S RX, spec 01 §5.3): the most recently completed 128-frame
    // block, interleaved L/R int16, copied into a stable buffer. Call from the
    // audio TX callback context (HalRefs::lineIn); nullptr before the first block.
    static const int16_t* lineInBlock(void* ctx);
    uint32_t rxBlocks() const;
    // RX liveness (rx_watch.h): main loop; restarts a halted receiver and logs it.
    static void serviceRx();
    static uint32_t rxRecoveries();    // receiver restarts since boot
    static uint32_t rxStaleBlocks();   // audio blocks given no input while halted
    static uint32_t rxStatus();        // I2S0_RCSR
    // Diagnostics (board.cpp inputDiag): while enabled, lineInBlock() also
    // tracks the peak |sample| per channel of the raw RX data (before any app
    // gain); takeRxPeak() returns it and the number of blocks seen, then resets.
    static void setRxPeak(bool on);
    static void takeRxPeak(int& peakL, int& peakR, uint32_t& blocks);

    uint32_t blocksRendered() const;
    // CPU cycles (DWT, 180 MHz) per 128-frame render callback since start; the
    // budget is 128 / 44117.6 Hz * 180 MHz = 522 240 cycles.
    void renderCycles(uint32_t& minC, uint32_t& avgC, uint32_t& maxC) const;
    // Same statistics since the previous call (app telemetry window); resets
    // the window and returns its block count. Main-loop context.
    uint32_t renderCyclesWindow(uint32_t& minC, uint32_t& avgC, uint32_t& maxC);

    // Called at the end of every audio block in the TX ISR, after the fill
    // callback (MIDI DIN out keeps the block timing: MidiPorts::pumpDin).
    void setBlockEndHook(void (*fn)());

    // IAudioOut
    void setCallback(FillFn fn, void* ctx) override;
    bool start() override;
    void stop() override;
    void setOutputEnabled(bool on) override;
    // spec 18 §1: Master EQ / Bass Boost / Space on the codec DAP (I2C writes,
    // main-loop context only; never from the audio ISR).
    void applyMasterDsp(const float eq[5], uint8_t bassBoost, uint8_t space) override;

private:
    sgtl_dap::Bus dapBus();
    sgtl_dap::Dap dap_;
    bool codecOk_ = false;
    uint16_t chipId_ = 0;
};

}  // namespace hal
}  // namespace ot
