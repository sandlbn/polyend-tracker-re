// OpenTracker -- hardware smoke test. Shows boot results, the 33 button and 48 pad
// states (plus raw TCA8418 codes, to verify the key maps of spec 05 §2.1), encoder
// count, SD/MIDI/power status; plays a tone; lights pads.
#include "selftest.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "audio_sgtl5000.h"
#include "diag.h"
#include "fw_update.h"
#include "board.h"
#include "ft81x.h"
#include "hal.h"
#include "hw_math.h"
#include "i2c_bus.h"
#include "input.h"
#include "midi_ports.h"
#include "pad_leds.h"
#include "sd_storage.h"
#include "sdhc.h"
#include "sdram_cache.h"

namespace ot {
namespace hal {

namespace {
int16_t gSine[256];
volatile uint32_t gPhaseInc = 0;     // 32-bit phase increment, 0 = silent
uint32_t gPhase = 0;

void toneFill(int16_t* out, uint32_t frames, void*) {
    uint32_t inc = gPhaseInc;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t s = inc ? gSine[gPhase >> 24] : 0;
        gPhase += inc;
        out[2 * i] = s;
        out[2 * i + 1] = s;
    }
}

uint32_t incForHz(float hz) {
    return uint32_t(hz * 4294967296.0f / (board::kSampleRateMilliHz / 1000.0f));
}

void label(IDisplay& d, int x, int y, const char* name, bool ok) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: %s", name, ok ? "OK" : "FAIL");
    if (ok) d.colorRGB(120, 230, 120); else d.colorRGB(255, 90, 90);
    d.text(int16_t(x), int16_t(y), 26, 0, buf);
}

// Boot report + live status over USB serial (CDC, Teensy "Serial"): read it with
// `screen /dev/ttyACM0` or `cat /dev/ttyACM0` (docs/HARDWARE_TESTING.md).
// The lines go through the diagnostic log (diag.h), whose interrupt-driven drain
// is the only writer on the CDC port, so they never block the loop.
extern "C" char _etext[], _sdata[], _edata[];   // Teensy mk66fx1m0.ld

void serialLine(const char* s) { diagLog(s); }
void serialReport(const BootReport& br, bool sdramFull, uint32_t sdramErr, bool sdramRan,
                  uint32_t failMask, uint32_t midiDin, uint32_t midiUsb) {
    char b[224];
    auto ok = [](bool v) { return v ? "OK" : "FAIL"; };
    snprintf(b, sizeof b, "--- OpenTracker selftest %s  t=%lus  boot stage %u  blink mask 0x%02lX",
             OT_VERSION, static_cast<unsigned long>(millis() / 1000), unsigned(br.stage),
             static_cast<unsigned long>(failMask));
    serialLine(b);
    snprintf(b, sizeof b, "image: flash 0x00000000..0x%08lX (%lu bytes), built %s %s",
             static_cast<unsigned long>(uint32_t(_etext) + uint32_t(_edata - _sdata)),
             static_cast<unsigned long>(uint32_t(_etext) + uint32_t(_edata - _sdata)), __DATE__, __TIME__);
    serialLine(b);
    snprintf(b, sizeof b, "clocks 180/90 (SIM_CLKDIV1 0x%08lX): %s", static_cast<unsigned long>(SIM_CLKDIV1),
             ok(br.clocksOk));
    serialLine(b);
    snprintf(b, sizeof b, "SDRAM quick 1MB: %s (%lu err)  full 16MB: %s (%lu err)", ok(br.sdramOk),
             static_cast<unsigned long>(br.sdramErrors), sdramRan ? ok(sdramFull) : "SKIPPED",
             static_cast<unsigned long>(sdramErr));
    serialLine(b);
    snprintf(b, sizeof b, "display FT81x: %s  REG_ID 0x%02X (want 0x7C)  attempt %u (0=int osc,1=CLKEXT,2=none)",
             ok(br.displayOk), unsigned(br.displayId), unsigned(br.displayAttempt));
    serialLine(b);
    snprintf(b, sizeof b, "codec SGTL5000: %s  CHIP_ID 0x%04X (want 0xA0xx)  audio blocks %lu",
             ok(br.codecOk), unsigned(br.codecId), static_cast<unsigned long>(audio().blocksRendered()));
    serialLine(b);
    uint32_t cMin, cAvg, cMax;
    audio().renderCycles(cMin, cAvg, cMax);
    snprintf(b, sizeof b, "audio render cycles/block: min %lu avg %lu max %lu (budget 522240)",
             static_cast<unsigned long>(cMin), static_cast<unsigned long>(cAvg),
             static_cast<unsigned long>(cMax));
    serialLine(b);
    FlexRamInfo fr = readFlexRamInfo();   // read-only: the selftest never writes EEPROM
    snprintf(b, sizeof b,
             "FlexRAM: SIM_FCFG1 0x%08lX (EESIZE %u DEPART %u)  FCNFG 0x%02X (RAMRDY %u EEERDY %u)",
             static_cast<unsigned long>(fr.fcfg1), unsigned(fr.eesize), unsigned(fr.depart),
             unsigned(fr.fcnfg), unsigned((fr.fcnfg >> 1) & 1), unsigned(fr.fcnfg & 1));
    serialLine(b);
    int n = snprintf(b, sizeof b, "EEPROM[0..15]:");
    for (int i = 0; i < 16 && n > 0 && n < int(sizeof b) - 4; i++)
        n += snprintf(b + n, sizeof b - size_t(n), " %02X", unsigned(fr.head[i]));
    serialLine(b);
    snprintf(b, sizeof b, "buttons TCA8418 I2C0: %s  pads TCA8418 I2C%u: %s (ack %s)  pad LEDs: %s",
             ok(input().buttonsOk()), unsigned(board::kPadBusIndex), ok(input().padsOk()),
             br.padBusAck ? "yes" : "no", ok(leds().ok()));
    serialLine(b);
    for (I2cBus* bus : {&i2c0(), &i2c2()}) {
        snprintf(b, sizeof b, "I2C%u: timeouts %lu  arb lost %lu  NAKs %lu  recoveries %lu", unsigned(bus->index()),
                 static_cast<unsigned long>(bus->timeoutCount()), static_cast<unsigned long>(bus->arbLostCount()),
                 static_cast<unsigned long>(bus->nakCount()), static_cast<unsigned long>(bus->recoveryCount()));
        serialLine(b);
    }
    const sdhc::Info& si = sdhc::info();
    snprintf(b, sizeof b, "SD: present %d  mounted %s  sectors %lu  last err 0x%08lX  %s %lu Hz%s  retries %lu degrades %lu",
             int(sdhc::cardPresent()), storage().mounted() ? "yes" : "no",
             static_cast<unsigned long>(sdhc::sectorCount()), static_cast<unsigned long>(sdhc::lastError()),
             si.adma ? "ADMA2" : "PIO", static_cast<unsigned long>(si.clockHz), si.highSpeed ? " HS" : "",
             static_cast<unsigned long>(si.retries), static_cast<unsigned long>(si.degrades));
    serialLine(b);
    const cache::Status& cs = cache::status();
    snprintf(b, sizeof b, "cache: SDRAM system cache %s, coherency selftest %s (eDMA reach %d, stale seen %d, "
             "fail step %u words %lu), 64 KB read %lu -> %lu cyc",
             cs.enabled ? "ON write-through" : "OFF", cache::testName(cs.test), int(cs.dmaReach), int(cs.staleSeen),
             unsigned(cs.failStep), static_cast<unsigned long>(cs.failWords),
             static_cast<unsigned long>(cs.streamOff), static_cast<unsigned long>(cs.streamOn));
    serialLine(b);
    const sdhc::Counters& sc = sdhc::counters();
    snprintf(b, sizeof b, "SD direct ADMA2<->SDRAM: read check %s, write check %s -> reads %s, writes %s; "
             "chunks direct %lu bounced %lu, cmds %lu",
             sdhc::directCheckName(sdhc::directCheck()), sdhc::directCheckName(sdhc::directWriteCheck()),
             sdhc::sdramDirectRead() ? "direct" : "bounced", sdhc::sdramDirectWrite() ? "direct" : "bounced",
             static_cast<unsigned long>(sc.direct), static_cast<unsigned long>(sc.bounced),
             static_cast<unsigned long>(sc.cmds));
    serialLine(b);
    snprintf(b, sizeof b, "MIDI out queue drops %lu", static_cast<unsigned long>(midi().dropped()));
    serialLine(b);
    snprintf(b, sizeof b, "enc %ld  btn raw 0x%02X  pad raw 0x%02X  power sw %s  MIDI in din %lu usb %lu",
             static_cast<long>(input().encoderTotal()), input().lastButtonCode(), input().lastPadCode(),
             powerSwitchOn() ? "ON" : "OFF", static_cast<unsigned long>(midiDin),
             static_cast<unsigned long>(midiUsb));
    serialLine(b);
}

// SD throughput (docs/HARDWARE_TESTING.md): 1 MB temp file written and read back
// in 64 KB chunks from SDRAM (the sample-loading path), with audio running so the
// DMA ISR load is realistic. The file is deleted afterwards.
void sdBenchmark(bool sdramOk) {
    if (!storage().mounted() || !sdramOk) {
        diagLogf("sd bench: skipped (mounted %d, SDRAM ok %d)", int(storage().mounted()), int(sdramOk));
        return;
    }
    diagStage("selftest: SD throughput");
    SdStorage::Bench r;
    // Scratch at the top of SDRAM; the selftest owns all of SDRAM (its test just ran).
    uint8_t* buf = reinterpret_cast<uint8_t*>(board::kSdramBase + board::kSdramSize - 65536);
    storage().benchmark(buf, 65536, 1u << 20, r);
    auto kbps = [](uint32_t bytes, uint32_t us) { return us ? uint32_t(uint64_t(bytes) * 1000u / us) : 0u; };
    const uint32_t w = kbps(r.bytes, r.writeUs), rd = kbps(r.bytes, r.readUs);
    const sdhc::Info& si = sdhc::info();
    diagLogf("sd bench: %s, %lu bytes, write %lu.%02lu MB/s, read %lu.%02lu MB/s, verify errors %lu (%s %lu Hz)",
             r.ok ? "OK" : "FAIL", static_cast<unsigned long>(r.bytes), static_cast<unsigned long>(w / 1000),
             static_cast<unsigned long>(w % 1000 / 10), static_cast<unsigned long>(rd / 1000),
             static_cast<unsigned long>(rd % 1000 / 10), static_cast<unsigned long>(r.verifyErrors),
             si.adma ? "ADMA2" : "PIO", static_cast<unsigned long>(si.clockHz));
}

// SD streaming probe (docs/PLAN_SD_STREAMING.md P0), with audio running:
// sequential / random / behind-write reads of a 16 MB temp file. The result
// goes to the serial log, to /OT_SDPROBE.TXT on the card and to the screen.
enum class ProbeVerdict : uint8_t { kNone, kGo, kConditional, kNoGo };
ProbeVerdict gProbeVerdict = ProbeVerdict::kNone;
char gProbeLine[48] = {};

void sdStreamProbe(bool sdramOk) {
    if (!storage().mounted() || !sdramOk) {
        diagLogf("sd probe: skipped (mounted %d, SDRAM ok %d)", int(storage().mounted()), int(sdramOk));
        return;
    }
    diagStage("selftest: SD streaming probe");
    IDisplay& d = display();
    d.dlStart();
    d.clearColorRGB(0, 0, 40);
    d.clear(true, true, true);
    d.colorRGB(255, 255, 255);
    d.text(400, 225, 29, IDisplay::kOptCenter, "Testing SD streaming (16 MB)...");
    d.text(400, 260, 26, IDisplay::kOptCenter, "about 20-60 s, do not remove the card");
    d.swap();
    uint8_t* buf = reinterpret_cast<uint8_t*>(board::kSdramBase + board::kSdramSize - 65536);
    uint32_t cMin, cAvg, cMax;
    audio().renderCyclesWindow(cMin, cAvg, cMax);   // reset the window
    const uint32_t b0 = audio().blocksRendered(), t0 = micros();
    SdStorage::Probe r;
    storage().streamProbe(buf, 65536, 16u << 20, r);
    const uint32_t us = micros() - t0, blocks = audio().blocksRendered() - b0;
    audio().renderCyclesWindow(cMin, cAvg, cMax);
    const uint32_t expect = uint32_t(uint64_t(us) * 44118u / 128u / 1000000u);
    const uint32_t missing = expect > blocks + 1 ? expect - blocks : 0;
    uint32_t stall = r.writeChunkMaxUs;
    for (uint32_t v : {r.seqChunkMaxUs, r.rnd[0].maxUs, r.rnd[1].maxUs, r.rnd[2].maxUs, r.mixReadMaxUs, r.mixWriteMaxUs})
        if (v > stall) stall = v;
    if (!r.ok) gProbeVerdict = ProbeVerdict::kNoGo;
    else if (r.seqKBps >= 6000 && r.rnd[0].p99Us <= 8000 && stall <= 250000 && missing == 0)
        gProbeVerdict = ProbeVerdict::kGo;
    else if (r.seqKBps >= 3000 && stall <= 600000 && missing == 0)
        gProbeVerdict = ProbeVerdict::kConditional;
    else
        gProbeVerdict = ProbeVerdict::kNoGo;
    const char* v = gProbeVerdict == ProbeVerdict::kGo ? "GO" : gProbeVerdict == ProbeVerdict::kConditional ? "COND" : "NO-GO";
    snprintf(gProbeLine, sizeof(gProbeLine), "stream %s %lu.%luMB/s stall %lums", v,
             static_cast<unsigned long>(r.seqKBps / 1024), static_cast<unsigned long>(r.seqKBps % 1024 * 10 / 1024),
             static_cast<unsigned long>(stall / 1000));
    // The report (serial log and the card file get the same lines).
    char lines[12][120];
    int n = 0;
    const sdhc::Info& si = sdhc::info();
    snprintf(lines[n++], sizeof(lines[0]), "sd probe: %s %s (%s), %lu MB file, cluster %lu B, fragments %lu, %s %lu Hz",
             v, r.ok ? "ok" : "FAIL", r.err, static_cast<unsigned long>(r.fileBytes >> 20),
             static_cast<unsigned long>(r.clusterBytes), static_cast<unsigned long>(r.fragments),
             si.adma ? "ADMA2" : "PIO", static_cast<unsigned long>(si.clockHz));
    snprintf(lines[n++], sizeof(lines[0]), "sd probe: write %lu KB/s (32 KB chunk max %lu us)",
             static_cast<unsigned long>(r.writeKBps), static_cast<unsigned long>(r.writeChunkMaxUs));
    snprintf(lines[n++], sizeof(lines[0]), "sd probe: sequential read %lu KB/s (32 KB chunk max %lu us)",
             static_cast<unsigned long>(r.seqKBps), static_cast<unsigned long>(r.seqChunkMaxUs));
    for (const SdStorage::Probe::Rnd& x : r.rnd)
        snprintf(lines[n++], sizeof(lines[0]), "sd probe: random %lu KB x%lu: min %lu avg %lu p99 %lu max %lu us",
                 static_cast<unsigned long>(x.size / 1024), static_cast<unsigned long>(x.n),
                 static_cast<unsigned long>(x.minUs), static_cast<unsigned long>(x.avgUs),
                 static_cast<unsigned long>(x.p99Us), static_cast<unsigned long>(x.maxUs));
    snprintf(lines[n++], sizeof(lines[0]), "sd probe: behind writes: write+sync max %lu us, read avg %lu max %lu us",
             static_cast<unsigned long>(r.mixWriteMaxUs), static_cast<unsigned long>(r.mixReadAvgUs),
             static_cast<unsigned long>(r.mixReadMaxUs));
    snprintf(lines[n++], sizeof(lines[0]), "sd probe: worst single operation %lu us; audio blocks %lu of %lu expected (missing %lu)",
             static_cast<unsigned long>(stall), static_cast<unsigned long>(blocks), static_cast<unsigned long>(expect),
             static_cast<unsigned long>(missing));
    snprintf(lines[n++], sizeof(lines[0]), "sd probe: render cycles during probe min %lu avg %lu max %lu (budget 522240)",
             static_cast<unsigned long>(cMin), static_cast<unsigned long>(cAvg), static_cast<unsigned long>(cMax));
    snprintf(lines[n++], sizeof(lines[0]),
             "sd probe: GO = seq >= 6000 KB/s, random 32 KB p99 <= 8000 us, stall <= 250000 us, no missing audio block");
    for (int i = 0; i < n; ++i) diagLogf("%s", lines[i]);
    IStorage& fs = storage();
    IStorage::Handle h = fs.open("OT_SDPROBE.TXT", IStorage::kWrite);
    if (h >= 0) {
        for (int i = 0; i < n; ++i) {
            fs.write(h, lines[i], uint32_t(strlen(lines[i])));
            fs.write(h, "\r\n", 2);
        }
        fs.close(h);
    }
}

void drawBox(IDisplay& d, int x, int y, int w, int h, bool on, int id) {
    if (on) d.colorRGB(255, 200, 40); else d.colorRGB(60, 60, 70);
    d.rect(int16_t(x), int16_t(y), int16_t(x + w), int16_t(y + h));
    d.colorRGB(on ? 0 : 200, on ? 0 : 200, on ? 0 : 200);
    d.number(int16_t(x + w / 2), int16_t(y + h / 2), 26, IDisplay::kOptCenter, id);
}
}  // namespace

void runSelftest() {
    const BootReport& br = bootReport();
    IDisplay& d = display();

    for (int i = 0; i < 256; i++) gSine[i] = int16_t(8000.0f * sinf(6.2831853f * i / 256.0f));

    // Full SDRAM test (spec 01 §2 factory test covers all 16 MB). Skipped when the
    // 1 MB boot test already failed (nothing to learn, and it takes seconds).
    diagStage("selftest: full SDRAM test");
    statusLed(true);
    d.dlStart();
    d.clearColorRGB(0, 0, 40);
    d.clear(true, true, true);
    d.colorRGB(255, 255, 255);
    d.text(400, 240, 29, IDisplay::kOptCenter, "Testing SDRAM (16 MB)...");
    d.swap();
    uint32_t sdramErr = 0;
    bool sdramRan = br.sdramOk;
    bool sdramFull = sdramRan && sdramTest(board::kSdramSize, &sdramErr);
    statusLed(false);
    diagLogf("sdram: full test %s, %s, %lu errors", sdramRan ? "ran" : "skipped", sdramFull ? "OK" : "FAIL",
             static_cast<unsigned long>(sdramErr));

    diagStage("selftest: audio start");
    audio().setCallback(toneFill, nullptr);
    audio().start();
    audio().setOutputEnabled(true);   // the selftest plays its tones at once
    audio().setHeadphoneVolume(0.5f);
    diagLogf("audio: started, blocks %lu", static_cast<unsigned long>(audio().blocksRendered()));
    sdBenchmark(sdramFull);
    sdStreamProbe(sdramFull);
    diagStage("selftest: main loop");
    diagFreezeBoot();
    gPhaseInc = incForHz(440.0f);
    uint32_t t0 = millis();

    uint32_t midiDin = 0, midiUsb = 0;
    uint32_t lastFrame = 0, lastReport = 0;
    for (;;) {
        diagBeat();
        service();
        uint32_t now = millis();

        // Status LED blink code (hw_math.h / docs/HARDWARE_TESTING.md), from the
        // boot report updated with the live driver state.
        BootReport live = br;
        live.sdramOk = br.sdramOk && sdramFull;
        live.buttonsOk = input().buttonsOk();
        live.padsOk = input().padsOk();
        live.ledsOk = leds().ok();
        live.sdPresent = sdhc::cardPresent();
        live.sdMounted = storage().mounted();
        uint32_t failMask = bootFailMask(live);
        statusLed(hw::statusLedOn(failMask, now - t0));
        // Only while a terminal holds the port open (DTR set: screen, cat, minicom
        // all assert it on Linux), so an unread port never costs write timeouts.
        if (now - lastReport >= 2000 && Serial.dtr()) {
            lastReport = now;
            serialReport(br, sdramFull, sdramErr, sdramRan, failMask, midiDin, midiUsb);
        }

        InputEvent ev;
        while (input().poll(ev)) {
            if (ev.type == InputEvent::kPadDown || ev.type == InputEvent::kPadUp) {
                bool on = ev.type == InputEvent::kPadDown;
                uint8_t msg[3] = {uint8_t(on ? 0x90 : 0x80), uint8_t(36 + ev.id), uint8_t(on ? 100 : 0)};
                midi().send(MidiPortId::kDin, msg, 3);
                midi().send(MidiPortId::kUsb, msg, 3);
            }
        }
        uint8_t rx[32];
        midiDin += midi().receive(MidiPortId::kDin, rx, sizeof(rx));
        midiUsb += midi().receive(MidiPortId::kUsb, rx, sizeof(rx));

        // Tone: 440 Hz for 3 s after start, then the lowest held pad's pitch.
        int held = -1;
        for (uint8_t p = 0; p < board::kPadCount; p++) if (input().padDown(p)) { held = p; break; }
        if (held >= 0) gPhaseInc = incForHz(110.0f * powf(2.0f, held / 12.0f));
        else gPhaseInc = (now - t0 < 3000) ? incForHz(440.0f) : 0;

        // LEDs: chaser for the first 48 x 50 ms, then held pads full + encoder base level.
        uint32_t chase = (now - t0) / 50;
        int base = input().encoderTotal();
        base = base < 0 ? 0 : (base > 31 ? 31 : base);
        for (uint8_t p = 0; p < board::kPadCount; p++) {
            uint8_t lvl = uint8_t(base);
            if (chase < board::kPadCount && p == chase) lvl = 31;
            if (input().padDown(p)) lvl = 31;
            leds().setLevel(p, lvl);
        }
        leds().show();

        if (now - lastFrame < 33) continue;
        lastFrame = now;

        char buf[96];
        d.dlStart();
        d.clearColorRGB(10, 10, 20);
        d.clear(true, true, true);
        d.colorRGB(255, 255, 255);
        d.text(10, 4, 29, 0, "OpenTracker hardware selftest");

        label(d, 10, 44, "clocks 180/90", br.clocksOk);
        snprintf(buf, sizeof(buf), "SDRAM 1MB %lu err, 16MB %s %lu err",
                 static_cast<unsigned long>(br.sdramErrors), sdramRan ? "ran" : "skipped",
                 static_cast<unsigned long>(sdramErr));
        label(d, 10, 66, buf, br.sdramOk && sdramFull);
        snprintf(buf, sizeof(buf), "codec SGTL5000 id 0x%04X", unsigned(br.codecId));
        label(d, 10, 88, buf, br.codecOk);
        label(d, 10, 110, "buttons TCA8418 I2C0", br.buttonsOk && input().buttonsOk());
        snprintf(buf, sizeof(buf), "pads TCA8418 I2C%u 0x34 ack", unsigned(board::kPadBusIndex));
        label(d, 10, 132, buf, br.padBusAck);
        label(d, 10, 154, "pads init", input().padsOk());
        label(d, 10, 176, "pad LEDs IS31FL3731", leds().ok());
        label(d, 10, 198, storage().mounted() ? "SD mounted" : "SD card", storage().mounted());
        {
            const cache::Status& cs = cache::status();
            snprintf(buf, sizeof(buf), "SDRAM cache %s (%s)", cs.enabled ? "on" : "off", cache::testName(cs.test));
            label(d, 300, 176, buf, cs.test == cache::Test::Ok || cs.test == cache::Test::BuildOff);
            const sdhc::DirectCheck rc = sdhc::directCheck(), wc = sdhc::directWriteCheck();
            snprintf(buf, sizeof(buf), "SD DMA->SDRAM rd %s wr %s", sdhc::directCheckName(rc),
                     sdhc::directCheckName(wc));
            label(d, 300, 198, buf, rc == sdhc::DirectCheck::Ok || rc == sdhc::DirectCheck::Skipped);
        }
        if (gProbeVerdict != ProbeVerdict::kNone) label(d, 300, 154, gProbeLine, gProbeVerdict == ProbeVerdict::kGo);

        d.colorRGB(200, 200, 255);
        snprintf(buf, sizeof(buf), "enc %ld  btn raw 0x%02X  pad raw 0x%02X",
                 static_cast<long>(input().encoderTotal()), input().lastButtonCode(), input().lastPadCode());
        d.text(10, 224, 26, 0, buf);
        snprintf(buf, sizeof(buf), "power sw %s  audio blk %lu  MIDI in din %lu usb %lu",
                 powerSwitchOn() ? "ON" : "OFF", static_cast<unsigned long>(audio().blocksRendered()),
                 static_cast<unsigned long>(midiDin), static_cast<unsigned long>(midiUsb));
        d.text(10, 246, 26, 0, buf);
        FlexRamInfo fr = readFlexRamInfo();
        snprintf(buf, sizeof(buf), "SD present %d  err 0x%08lX  EE: sz %u dp %u fcnfg 0x%02X b0 %u",
                 int(sdhc::cardPresent()), static_cast<unsigned long>(sdhc::lastError()),
                 unsigned(fr.eesize), unsigned(fr.depart), unsigned(fr.fcnfg), unsigned(fr.head[0]));
        d.text(10, 268, 26, 0, buf);

        // Buttons 8..32: 5x5 grid (TCA rows 0-4 x cols 0-4, spec 05 §2.1.1).
        for (int id = 8; id <= 32; id++) {
            int k = id - 8;
            drawBox(d, 560 + (k % 5) * 46, 40 + (k / 5) * 40, 42, 36, input().buttonDown(uint8_t(id)), id);
        }
        // Buttons 0..7: F1..F8 row (screen function buttons).
        for (int id = 0; id < 8; id++)
            drawBox(d, 10 + id * 100, 440, 92, 34, input().buttonDown(uint8_t(id)), id);
        // Pads: 12 x 4 grid, index = row*12 + col, row 0 = top (spec 05 §2.1.2).
        for (int p = 0; p < board::kPadCount; p++)
            drawBox(d, 10 + (p % 12) * 66, 290 + (p / 12) * 36, 62, 32, input().padDown(uint8_t(p)), p);
        d.swap();
    }
}

}  // namespace hal
}  // namespace ot
