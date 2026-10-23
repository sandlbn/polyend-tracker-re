// OpenTracker -- board definition for the original hardware (MK66FX1M0).
// Every value cites docs/specs/01-hardware.md ("spec 01 §x"). Items the spec
// tags [I] (inferred) are marked UNVERIFIED.
#pragma once

#include <stdint.h>

namespace ot {
namespace board {

// ---- clocks ---------------------------------------------------------------
constexpr uint32_t kCoreHz    = 180000000;  // spec 01 §1 step 13 (PLL 180 MHz, HSRUN)
constexpr uint32_t kBusHz     = 90000000;   // spec 01 §1 step 14 (SIM_CLKDIV1 0x01060000)
constexpr uint32_t kFlexBusHz = 90000000;   // spec 01 §2 step 1 (OUTDIV3=1)
constexpr uint32_t kSimClkdiv1 = 0x01060000; // spec 01 §1 step 14 (checked at boot)

// ---- port pin helper ------------------------------------------------------
enum Port : uint8_t { PA = 0, PB, PC, PD, PE };
struct Pin { Port port; uint8_t bit; };

// ---- power / misc (spec 01 §10, §3.2) -------------------------------------
constexpr Pin kPinPowerSwitch = {PC, 3};    // spec 01 §10: PTC3 (T#9) input, LOW = on
constexpr uint8_t kTeensyPowerSwitch = 9;   // spec 01 §3.2
// spec 05 power button handler (power-off countdown 1500 ms): held -> shutdown, released earlier -> cancel.
constexpr uint32_t kPowerHoldOffMs = 1500;
constexpr uint32_t kPowerDebounceMs = 20;    // OpenTracker choice
constexpr Pin kPinRailEnable  = {PA, 24};   // spec 01 §10: PTA24 (#80) HIGH at boot. UNVERIFIED [I]
constexpr Pin kPinUpdateReq   = {PA, 1};    // spec 01 §10: PTA1 LOW = fw-update request. Driven only by fw_update.cpp.
constexpr Pin kPinStatusLed   = {PC, 19};   // spec 01 §10: PTC19 (#78), HIGH = off. UNVERIFIED [I] (boot/blink codes)

// ---- SDRAM (spec 01 §2) -----------------------------------------------------
constexpr uint32_t kSdramBase  = 0x70000000; // spec 01 §2 step 7
constexpr uint32_t kSdramSize  = 0x01000000; // spec 01 §2 "Result": 16 MB
constexpr uint16_t kSdramCtrl  = 0x0256;     // spec 01 §2 step 7 (RTIM=1, RC=86)
constexpr uint32_t kSdramAc0   = 0x70003320; // spec 01 §2 step 7 (16-bit, CMDBIT A20, CL3)
constexpr uint32_t kSdramCm0   = 0x00FC0001; // spec 01 §2 step 7
constexpr uint32_t kSdramMrsAddr = 0x70001800; // spec 01 §2 step 8 (mode-register set write)
constexpr uint32_t kSdramPcr   = 0x0500;     // spec 01 §2 step 3 (ALT5)
constexpr uint32_t kFbCspmcrMask = 0xF0000FFF; // spec 01 §2 step 6
constexpr uint32_t kFbCspmcrVal  = 0x02222000; // spec 01 §2 step 6
// spec 01 §2 pin table (39 pins).
constexpr Pin kSdramPins[] = {
    {PA, 6},                                                        // clock
    {PB, 0}, {PB, 1}, {PB, 2}, {PB, 3},                             // control
    {PD, 7},                                                        // CKE
    {PC, 16}, {PC, 17},                                             // DQM
    {PB, 6}, {PB, 7}, {PB, 8}, {PB, 9}, {PB, 10}, {PB, 11},         // data
    {PB, 16}, {PB, 17}, {PB, 20}, {PB, 21}, {PB, 22}, {PB, 23},
    {PC, 12}, {PC, 13}, {PC, 14}, {PC, 15},
    {PB, 18}, {PC, 0}, {PC, 1}, {PC, 2}, {PC, 4}, {PC, 5}, {PC, 6}, // address
    {PC, 7}, {PC, 8}, {PC, 9}, {PC, 10}, {PD, 2}, {PD, 3}, {PD, 4}, {PD, 5},
};
static_assert(sizeof(kSdramPins) / sizeof(kSdramPins[0]) == 39, "spec 01 §2: 39 SDRAM pins");

// ---- I2C (spec 01 §0 step 3, §3.2, §11) ----------------------------------------
constexpr uint32_t kI2cHz = 400000;          // spec 01 §0 step 3
constexpr Pin kI2c0Scl = {PD, 8};            // spec 01 §3.2 (PCR 0x264)
constexpr Pin kI2c0Sda = {PD, 9};            // spec 01 §3.2
constexpr uint32_t kI2c0Pcr = 0x264;         // spec 01 §3.2 (ALT2, DSE, ODE, SRE)
constexpr Pin kI2c2Scl = {PA, 12};           // spec 01 §3.2 (PCR 0x564)
constexpr Pin kI2c2Sda = {PA, 13};           // spec 01 §3.2
constexpr uint32_t kI2c2Pcr = 0x564;         // spec 01 §3.2 (ALT5, DSE, ODE, SRE)
constexpr uint32_t kI2cTimeoutUs = 1000;     // spec 01 §0 step 3 / §7.5 (Wire timeout 1000)
// A per-byte timeout only counts once the wait loop itself has polled at least
// this many times: the audio DMA ISR (56-72 % CPU on hardware) can preempt the
// loop for longer than kI2cTimeoutUs, which made micros()-only timeouts fire
// spuriously (field data: 400-600 errors per bus). OpenTracker choice.
constexpr uint32_t kI2cMinPolls = 2000;      // ~0.3 ms of real polling (micros() each pass)
constexpr uint32_t kI2cBusyRecoverMs = 10;   // spec 01 §7.5: bus busy > 10 ms -> recovery

// Bus carrying the pad TCA8418, the IS31FL3731 and the fuel gauge.
// spec 01 §7.2/§7.3 and §11: I2C2 (0x400E6000, PTA12/PTA13). spec 05 §1/§2.1 calls
// the same bus "I2C3 @0x400E6000, PORTA12/13" -- the base address and pins are those
// of the K66 module I2C2, so both specs describe the same peripheral (naming only).
// The real K66 I2C3 (0x400E7000) can only be muxed onto PTA1/PTA2 (PTA1 must never
// be driven, spec 01 §10) or PTE10/PTE11 (I2S0 TXD/FS, spec 01 §3.2), so it is not probed.
constexpr uint8_t kPadBusIndex = 2;

constexpr uint8_t kAddrSgtl5000 = 0x0A;      // spec 01 §5.1, I2C0
constexpr uint8_t kAddrSi4703   = 0x10;      // spec 01 §5.6, I2C0 (radio_si4703.cpp)
constexpr uint8_t kAddrButtons  = 0x34;      // spec 01 §7.1, TCA8418 on I2C0
constexpr uint8_t kAddrPads     = 0x34;      // spec 01 §7.2, TCA8418 on I2C2
constexpr uint8_t kAddrPadLeds  = 0x74;      // spec 01 §7.3, IS31FL3731 on I2C2
constexpr uint8_t kAddrFuelGauge = 0x36;     // spec 01 §7.6, I2C2 (identity UNVERIFIED [I])

// ---- display: FT81x on SPI2 (spec 01 §4) ------------------------------------
constexpr Pin kSpi2Sck  = {PD, 12};          // spec 01 §3.2 (PCR 0x200, ALT2)
constexpr Pin kSpi2Sout = {PD, 13};          // spec 01 §3.2
constexpr Pin kSpi2Sin  = {PD, 14};          // spec 01 §3.2
constexpr uint32_t kSpi2Pcr = 0x200;         // spec 01 §3.2
constexpr uint8_t kTeensyFtCs = 54;          // spec 01 §3.2: PTD15 CS_N (GPIO)
constexpr uint8_t kTeensyFtPd = 55;          // spec 01 §3.2: PTD11 PD_N
constexpr uint32_t kSpi2CtarSlow = 0x38021001; // spec 01 §4.1: 4.5 MHz
constexpr uint32_t kSpi2CtarFast = 0xB8010000; // spec 01 §4.1: 30 MHz
constexpr uint16_t kLcdWidth  = 800;         // spec 01 §4.2
constexpr uint16_t kLcdHeight = 480;         // spec 01 §4.2
constexpr uint16_t kLcdPwmHz  = 9000;        // spec 01 §4.2
constexpr uint8_t  kLcdRotate = 1;           // spec 01 §4.2 (180 degrees)
constexpr uint8_t  kLcdBacklightOn = 127;    // spec 01 §4.2 ("127/128 on first draw")

// ---- audio (spec 01 §5) -------------------------------------------------------
constexpr uint8_t kTeensyMute = 21;          // spec 01 §5.4: PTD6, LOW = muted. UNVERIFIED [I]
constexpr uint8_t kTeensyRadioSwitch = 24;   // spec 01 §5.4: PTE26, HIGH = radio off. UNVERIFIED [I]
constexpr Pin kPinRadioReset = {PE, 27};     // spec 01 §5.6: Si4703 RST_N (Teensy #67, board-specific)
constexpr Pin kPinRadioInt   = {PE, 28};     // spec 01 §5.6: Si4703 GPIO2 (Teensy #68); not used (polled)
// Recorder input levels: per-source gain, stock scale (core app/input_gain.h, spec 22 §3).
constexpr uint8_t kDmaPrioAudioRx = 0x40;    // spec 01 §5.3 RX ISR priority
constexpr uint32_t kI2sPcr = 0x400;          // spec 01 §3.2 (ALT4) for PTE6/7/10/11/12
constexpr Pin kI2sMclk = {PE, 6};            // spec 01 §3.2
constexpr Pin kI2sRxd0 = {PE, 7};            // spec 01 §3.2
constexpr Pin kI2sTxd0 = {PE, 10};           // spec 01 §3.2
constexpr Pin kI2sTxFs = {PE, 11};           // spec 01 §3.2
constexpr Pin kI2sBclk = {PE, 12};           // spec 01 §3.2
constexpr uint32_t kI2sMcr  = 0x40000000;    // spec 01 §5.2
constexpr uint32_t kI2sMdr  = 0x0000F0FE;    // spec 01 §5.2 (MCLK = 180*16/255 MHz)
constexpr uint32_t kI2sTcr2 = 0x07000001;    // spec 01 §5.2
constexpr uint32_t kI2sRcr2 = 0x47000001;    // spec 01 §5.2
constexpr uint32_t kI2sXcr3 = 0x00010000;    // spec 01 §5.2
constexpr uint32_t kI2sXcr4 = 0x00011F1B;    // spec 01 §5.2
constexpr uint32_t kI2sXcr5 = 0x1F1F1F00;    // spec 01 §5.2
constexpr uint32_t kSampleRateMilliHz = 44117647; // spec 01 §5.2: Fs = MCLK/256
constexpr uint32_t kAudioBlockFrames = 128;  // spec 01 §5.3
constexpr uint8_t  kDmaPrioAudioTx = 0x50;   // spec 01 §5.3

// ---- SD card (spec 01 §6) -----------------------------------------------------
constexpr uint8_t kTeensyCardDetect = 2;     // spec 01 §6: PTD0, pull-up, LOW = inserted
constexpr uint32_t kCardDebounceMs = 500;    // spec 01 §6
constexpr uint32_t kSdPcrInit = 0x103;       // spec 01 §6 (before init)
constexpr uint32_t kSdPcrData = 0x443;       // spec 01 §3.2 (D0-D3, CMD)
constexpr uint32_t kSdPcrClk  = 0x440;       // spec 01 §3.2 (DCLK = PTE2)
constexpr uint32_t kSdWml     = 0x00800080;  // spec 01 §6: read/write watermark 128 words
constexpr uint32_t kSdIrqStatEn = 0x117F000B; // spec 01 §6
// SDCLK = 180 MHz / (2 * SDCLKFS) / (DVS + 1) (K66 RM SDHC SYSCTL, spec 01 §6).
constexpr uint8_t kSdIdClkfs = 0x10, kSdIdDvs = 14;  // 375 kHz identification
constexpr uint8_t kSdHsClkfs = 0x01, kSdHsDvs = 1;   // 45 MHz high speed (stock asks 50 MHz)
constexpr uint8_t kSdDsClkfs = 0x01, kSdDsDvs = 3;   // 22.5 MHz default speed
constexpr uint8_t kSdLoClkfs = 0x02, kSdLoDvs = 3;   // 11.25 MHz last-resort fallback
constexpr uint8_t kSdDtocv = 0xE;            // spec 01 §6 (SDCLK x 2^27 data timeout)
// ADMA2 (spec 01 §6: stock SDK host uses ADMA2). OpenTracker sizes:
constexpr uint32_t kSdAdmaDescs = 8;         // descriptors per command
constexpr uint32_t kSdAdmaDescBytes = 32768; // bytes per descriptor (16-bit length field)
// SRAM bounce buffer. 2.x USB Storage (OT_USB_MSC=1): 16 KB (32 sectors),
// which is also the computer's 4-chunk transfer ring while it owns the card
// (usb_storage.cpp, msc::Bot::kChunk); the 8 KB it adds come from the FatFs
// file objects moved to the SDRAM (sd_storage.cpp). Fallback image: 8 KB.
#if defined(OT_USB_MSC) && OT_USB_MSC
constexpr uint32_t kSdBounceBytes = 16384;
#else
constexpr uint32_t kSdBounceBytes = 8192;
#endif
// UNVERIFIED: whether the SDHC crossbar master (M5) reaches the SDRAM controller
// slave. 2.x: true = try it; sdhc::checkSdramDirect() compares it with the bounce
// path at boot and only then lets SDRAM buffers skip the bounce (else they bounce).
constexpr bool kSdDmaToSdram = true;
// Direct SDRAM->SD writes: verified lazily (first SDRAM-sourced write of the
// boot) through FatFs on a temp file (sd_storage.cpp); never raw sectors.
// false = SDRAM writes always bounce.
constexpr bool kSdDirectWriteCheck = true;
// SDRAM scratch for the boot-time cache + direct-DMA checks: inside the 1 MB the
// quick test just destroyed, before the application owns the SDRAM.
constexpr uint32_t kSdramCheckScratch = kSdramBase + 0x00040000;   // cache test (80 KB)
constexpr uint32_t kSdramSdScratch    = kSdramBase + 0x00060000;   // SD direct check (65 KB)
// `.sdram_bss` (core/sdram_bss.h, docs/OPTIMIZATION_2X.md OP-5): the linker
// region SDRAM_BSS of src/hal/opentracker.ld. The app's SDRAM layout
// (app_main.cpp) ends below it. 2.0.1: 80 KB (was 64 KB) for the FatFs file
// objects; 2.0.7: 92 KB (the synth patches and the 200-entry file lists).
// 156 KB: same base, now ending at the SDRAM's top. Its top 64 KB overlap
// the selftest's SD benchmark buffer (selftest.cpp), which is safe because
// the selftest only runs when the app never started or has returned
// (main.cpp), never alongside it.
constexpr uint32_t kSdramBssBase      = kSdramBase + 0x00FD9000;
constexpr uint32_t kSdramBssSize      = 0x00027000;
static_assert(kSdramBssBase + kSdramBssSize <= kSdramBase + kSdramSize, ".sdram_bss past the SDRAM's top");

// ---- input (spec 01 §7) ---------------------------------------------------------
constexpr uint8_t kTeensyButtonsInt = 14;    // spec 01 §7.1: PTD1
constexpr uint8_t kTeensyPadsInt    = 38;    // spec 01 §7.2: PTC11
constexpr uint8_t kButtonCount = 33;         // spec 01 §7.1
constexpr uint8_t kPadCount = 48;            // spec 01 §7.2 (12 x 4)
constexpr uint8_t kPadColumns = 12;          // spec 01 §7.2
constexpr uint8_t kTcaCfg = 0xB9;            // spec 01 §7.1
constexpr uint8_t kTcaGpio3Buttons = 0x01;   // spec 01 §7.1 (9 columns)
constexpr uint8_t kTcaGpio3Pads    = 0x03;   // spec 01 §7.2 (10 columns)
constexpr uint32_t kEncPcr = 0x612;          // spec 01 §3.2: PTA10/PTA11 ALT6
constexpr Pin kEncA = {PA, 10};              // spec 01 §7.4
constexpr Pin kEncB = {PA, 11};              // spec 01 §7.4
constexpr int kEncCountsPerDetent = 4;       // spec 01 §7.4
constexpr uint32_t kLedFlushMinMs = 30;      // spec 01 §7.3

// ---- MIDI (spec 01 §8) -----------------------------------------------------------
constexpr uint32_t kMidiBaud = 31250;        // spec 01 §8 (UART4 = Teensy Serial5, TX inverted)

}  // namespace board
}  // namespace ot
