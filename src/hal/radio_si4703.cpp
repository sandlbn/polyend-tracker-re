// OpenTracker -- Si4703 FM tuner register logic (spec 01 §5.6, spec 22 §2).
// Register names and bits from the public Si4702/03-C19 datasheet and Silicon
// Labs AN230. Platform independent: the device glue is radio_si4703_port.cpp.
#include "radio_si4703.h"

namespace ot {
namespace hal {

namespace {
// Register indices (datasheet §6).
constexpr int POWERCFG = 0x02, CHANNEL = 0x03, SYSCONFIG1 = 0x04,
    SYSCONFIG2 = 0x05, SYSCONFIG3 = 0x06, TEST1 = 0x07, STATUSRSSI = 0x0A, READCHAN = 0x0B,
    RDSA = 0x0C, RDSB = 0x0D, RDSD = 0x0F;
// POWERCFG bits.
constexpr uint16_t PWR_DSMUTE = 0x8000, PWR_DMUTE = 0x4000, PWR_MONO = 0x2000, PWR_RDSM = 0x0800,
    PWR_SKMODE = 0x0400, PWR_SEEKUP = 0x0200, PWR_SEEK = 0x0100, PWR_DISABLE = 0x0040, PWR_ENABLE = 0x0001;
constexpr uint16_t CH_TUNE = 0x8000;
constexpr uint16_t SC1_RDS = 0x1000, SC1_DE = 0x0800;   // DE 1 = 50 us, 0 = 75 us
constexpr uint16_t ST_RDSR = 0x8000, ST_STC = 0x4000, ST_SFBL = 0x2000, ST_ST = 0x0100;
// spec 22 §2.2 / §2.3 values.
constexpr uint16_t kTest1Xosc = 0x8100;        // XOSCEN (+ AN230 reserved bit 8)
constexpr uint16_t kPowerCfg = PWR_DSMUTE | PWR_DMUTE | PWR_MONO | PWR_RDSM | PWR_ENABLE;   // 0xE801
static_assert(kPowerCfg == 0xE801, "spec 22 §2.3 POWERCFG");
constexpr uint16_t kSysCfg2Base = 0x190A;      // SEEKTH 0x19, VOLUME 10
constexpr uint16_t kSysCfg3Seek = 0x0048;      // SKSNR 4, SKCNT 8
constexpr uint32_t kStatusPollMs = 500;
// OT fork: RDS groups arrive about every 88 ms; poll the status faster.
constexpr uint32_t kRdsPollMs = 40;
}  // namespace

bool Si4703Radio::readAll() {
    // spec 22 §2.1: always all 16 registers, starting at 0x0A and wrapping
    // 0x0F -> 0x00, ending at 0x09.
    uint8_t b[32];
    if (!port_.read || !port_.read(b, sizeof b)) {
        errors_++;
        return false;
    }
    for (int i = 0; i < 16; ++i) regs_[(STATUSRSSI + i) & 15] = uint16_t((b[2 * i] << 8) | b[2 * i + 1]);
    return true;
}

bool Si4703Radio::writeRegs(bool retry) {
    // spec 22 §2.1: 12 bytes = shadow regs 0x02..0x07 (TEST1 is part of every
    // write); retried up to 10 times unless it is a bring-up write.
    uint8_t b[12];
    for (int i = 0; i < 6; ++i) {
        b[2 * i] = uint8_t(regs_[POWERCFG + i] >> 8);
        b[2 * i + 1] = uint8_t(regs_[POWERCFG + i]);
    }
    for (int n = retry ? kWriteRetries : 1; n > 0; --n) {
        if (port_.write && port_.write(b, sizeof b)) return true;
        errors_++;
    }
    return false;
}

void Si4703Radio::fail(const char* what) {
    // spec 22 §2.1: after a failed read or a write that failed all retries, the
    // stock driver resets and powers up again. OpenTracker drops to kUnreset;
    // the recorder re-selects the radio about once a second while it is the
    // source, and that powerOn() runs the reset + power-up.
    if (port_.log)
        port_.log("radio: I2C failure (%s) in state %u, errors %lu -> reset on the next power on", what,
                  unsigned(state_), static_cast<unsigned long>(errors_));
    state_ = kUnreset;
    pendingSeek_ = 0;
}

bool Si4703Radio::boot() {
    // spec 22 §2.2 steps 2-10 (step 1, PTE26, is board.cpp's). Bring-up
    // writes are not retried (retry flag 1).
    if (port_.resetPulse) port_.resetPulse();
    state_ = kUnreset;
    if (!readAll()) {
        if (port_.log) port_.log("radio: boot: no answer at 0x10 after reset (2-wire mode not entered?)");
        return false;
    }
    // UNVERIFIED: identity check only reported (deviceId()); 0x1242 = Si4702/03.
    if (port_.log)
        port_.log("radio: boot: DEVICEID 0x%04X CHIPID 0x%04X TEST1 0x%04X (expect DEVICEID 0x1242)",
                  unsigned(regs_[0]), unsigned(regs_[1]), unsigned(regs_[TEST1]));
    regs_[TEST1] = kTest1Xosc;
    if (!writeRegs(false)) {
        if (port_.log) port_.log("radio: boot: TEST1 (XOSCEN) write failed");
        return false;
    }
    // spec 22 §2.2 step 10 waits 500 ms here; OpenTracker lets the crystal
    // settle in the background and holds the first power-up back instead.
    oscAt_ = port_.millis ? port_.millis() : 0;
    state_ = kStandby;
    return true;
}

bool Si4703Radio::powerOn() {
    if (powered()) return true;
    if (state_ == kUnreset && !boot()) return false;
    state_ = kOsc;   // service(): power-up once the crystal has run kOscMs
    rssi_ = 0;
    timedOut_ = false;
    return true;
}

void Si4703Radio::powerOff() {
    // spec 22 §2.7: full read; SYSCONFIG1 &= ~RDS; write; full read; POWERCFG
    // |= DISABLE (ENABLE stays set); write. RST_N is not touched.
    const bool enabled = state_ >= kPowerUp;
    const bool midTune = state_ >= kTuning;   // cannot happen in stock (blocking tune)
    pendingSeek_ = 0;
    pendingTune_ = false;
    if (state_ == kOsc) state_ = kStandby;   // never enabled
    if (!enabled) return;
    if (!readAll()) return fail("power off read");
    regs_[SYSCONFIG1] = uint16_t(regs_[SYSCONFIG1] & ~SC1_RDS);
    if (midTune) {   // a tune/seek still running: end it so the next one starts cleanly
        regs_[CHANNEL] = uint16_t(regs_[CHANNEL] & ~CH_TUNE);
        regs_[POWERCFG] = uint16_t(regs_[POWERCFG] & ~PWR_SEEK);
    }
    if (!writeRegs(true)) return fail("power off write");
    if (!readAll()) return fail("power off read");
    regs_[POWERCFG] = uint16_t(regs_[POWERCFG] | PWR_DISABLE);
    if (!writeRegs(true)) return fail("power off write");
    state_ = kStandby;
}

bool Si4703Radio::applyRegion() {
    // spec 22 §2.4: full read; SYSCONFIG2 &= ~0xF0 (BAND, SPACE); SYSCONFIG1
    // &= ~DE; then the region bits; write. Band (datasheet SYSCONFIG2[7:6]):
    // 00 = 87.5-108, 01 = 76-108. Spacing [5:4]: 00 = 200 kHz, 01 = 100 kHz.
    // The region table (EU 87.5/0.1 DE, US 87.5/0.2, AU 87.5/0.2 DE, JP
    // 76/0.1 DE) arrives as setBand() limits plus the region index (DE).
    if (!readAll()) return false;
    const uint16_t band = bandMin_ < 87.0f ? 1 : 0;
    bottom_ = band ? 76.0f : 87.5f;
    const uint16_t space = step_ > 0.15f ? 0 : (step_ > 0.075f ? 1 : 2);
    regs_[SYSCONFIG2] = uint16_t((regs_[SYSCONFIG2] & ~0x00F0u) | (band << 6) | (space << 4));
    uint16_t sc1 = uint16_t(regs_[SYSCONFIG1] & ~SC1_DE);
    if (region_ != 1) sc1 |= SC1_DE;
    // OT fork: RDS on (stock sets it at power-up too, spec 22 §2.3 step 7's
    // 0x9004; the interrupt bits stay off: the status is polled).
    sc1 |= SC1_RDS;
    regs_[SYSCONFIG1] = sc1;
    return writeRegs(true);
}

uint16_t Si4703Radio::channelFor(float mhz) const {
    // spec 22 §2.5: chan = trunc((MHz - base) / spacing + 0.5).
    if (mhz < bandMin_) mhz = bandMin_;
    if (mhz > bandMax_) mhz = bandMax_;
    float c = (mhz - bottom_) / step_ + 0.5f;
    return c < 0 ? 0 : uint16_t(c) & 0x3FF;
}

float Si4703Radio::freqFor(uint16_t chan) const { return bottom_ + float(chan & 0x3FF) * step_; }

void Si4703Radio::setBand(float minMHz, float maxMHz, float stepMHz) {
    if (stepMHz <= 0.0f || maxMHz <= minMHz) return;
    bandMin_ = minMHz;
    bandMax_ = maxMHz;
    step_ = stepMHz;
    bottom_ = bandMin_ < 87.0f ? 76.0f : 87.5f;
    // Applied when the tuner is ready (end of the power-up, or after the
    // running tune/seek), then re-tuned: CHAN is relative to band and spacing.
    regionDirty_ = true;
}

bool Si4703Radio::tune(float mhz) {
    if (!powered()) return false;
    pendingFreq_ = mhz;
    pendingTune_ = true;
    pendingSeek_ = 0;
    freq_ = freqFor(channelFor(mhz));   // shown right away; READCHAN confirms
    if (state_ == kReady) startTune();
    return true;
}

bool Si4703Radio::seek(bool up) {
    if (!powered()) return false;
    pendingSeek_ = up ? 1 : -1;
    if (state_ == kReady) startSeek();
    return true;
}

bool Si4703Radio::busy() const {
    return powered() && (state_ != kReady || pendingTune_ || pendingSeek_ != 0);
}

void Si4703Radio::startTune() {
    clearRds();
    // spec 22 §2.5 step 2: full read; CHANNEL = (CHANNEL & 0xFC00) | chan | TUNE; write.
    pendingTune_ = false;
    if (regionDirty_) {
        regionDirty_ = false;
        if (!applyRegion()) return fail("region");
    }
    const uint16_t chan = channelFor(pendingFreq_);
    if (!readAll()) return fail("tune read");
    regs_[CHANNEL] = uint16_t((regs_[CHANNEL] & 0xFC00) | chan | CH_TUNE);
    if (!writeRegs(true)) return fail("tune write");
    state_ = kTuning;
    timedOut_ = false;
    t0_ = port_.millis ? port_.millis() : 0;
}

void Si4703Radio::startSeek() {
    clearRds();
    // spec 22 §2.6 state 1: up -> SKMODE 0 (wrap) | SEEKUP | SEEK; down ->
    // SKMODE 0, SEEKUP 0, SEEK. Read-modify-write.
    const bool up = pendingSeek_ > 0;
    pendingSeek_ = 0;
    if (regionDirty_) {
        regionDirty_ = false;
        if (!applyRegion()) return fail("region");
    }
    if (!readAll()) return fail("seek read");
    uint16_t p = regs_[POWERCFG];
    p = up ? uint16_t((p & ~PWR_SKMODE) | PWR_SEEKUP | PWR_SEEK)
           : uint16_t((p & ~(PWR_SKMODE | PWR_SEEKUP)) | PWR_SEEK);
    regs_[POWERCFG] = p;
    if (!writeRegs(true)) return fail("seek write");
    state_ = kSeeking;
    timedOut_ = false;
    t0_ = port_.millis ? port_.millis() : 0;
}

void Si4703Radio::logDone(const char* what, uint32_t now) {
    if (!port_.log) return;
    const uint16_t st = regs_[STATUSRSSI];
    port_.log("radio: %s %s after %lu ms: POWERCFG 0x%04X CHANNEL 0x%04X SYSCONFIG1 0x%04X SYSCONFIG2 0x%04X "
              "READCHAN 0x%04X (chan %u = %.1f MHz) STATUSRSSI 0x%04X (RSSI %u ST %u SF/BL %u STC %u)",
              what, timedOut_ ? "FAILED (no STC)" : "done", static_cast<unsigned long>(now - t0_),
              unsigned(regs_[POWERCFG]), unsigned(regs_[CHANNEL]), unsigned(regs_[SYSCONFIG1]),
              unsigned(regs_[SYSCONFIG2]), unsigned(regs_[READCHAN]), unsigned(regs_[READCHAN] & 0x3FF),
              double(freq_), unsigned(st), unsigned(st & 0xFF), unsigned((st & ST_ST) != 0),
              unsigned((st & ST_SFBL) != 0), unsigned((st & ST_STC) != 0));
}

void Si4703Radio::service() {
    if (!powered()) return;
    const uint32_t now = port_.millis ? port_.millis() : 0;
    switch (state_) {
        case kOsc:
            if (now - oscAt_ < kOscMs) return;
            // spec 22 §2.3 steps 1-5: full read; SYSCONFIG3 |= 0x48; POWERCFG =
            // 0xE801 (also clears a DISABLE left by a power-down); SYSCONFIG2 =
            // (x & 0xFFF0) | 0x190A (band/space kept); write. CHANNEL untouched.
            if (!readAll()) return fail("power up read");
            regs_[SYSCONFIG3] = uint16_t(regs_[SYSCONFIG3] | kSysCfg3Seek);
            regs_[POWERCFG] = kPowerCfg;
            regs_[SYSCONFIG2] = uint16_t((regs_[SYSCONFIG2] & 0xFFF0) | kSysCfg2Base);
            if (!writeRegs(false)) return fail("power up write");
            if (port_.log)
                port_.log("radio: enable POWERCFG 0x%04X SYSCONFIG1 0x%04X SYSCONFIG2 0x%04X SYSCONFIG3 0x%04X "
                          "TEST1 0x%04X",
                          unsigned(regs_[POWERCFG]), unsigned(regs_[SYSCONFIG1]), unsigned(regs_[SYSCONFIG2]),
                          unsigned(regs_[SYSCONFIG3]), unsigned(regs_[TEST1]));
            state_ = kPowerUp;
            t0_ = now;
            return;
        case kPowerUp:
            if (now - t0_ < kPowerUpMs) return;
            // spec 22 §2.3 step 7 then sets SYSCONFIG1 |= 0x9004 (RDS, STC/RDS
            // interrupt on GPIO2) and arms the PTE28 interrupt. Not done:
            // OpenTracker shows no RDS and polls STC like the stock tune does
            // (§2.5), so the interrupt is not needed for tuning (README).
            // spec 22 §4 step 5: setRegion right after the power-up.
            if (!applyRegion()) return fail("region");
            regionDirty_ = false;
            state_ = kReady;
            // spec 22 §4 step 6: tune to the saved frequency.
            if (!pendingTune_ && pendingSeek_ == 0) {
                pendingFreq_ = freq_;
                pendingTune_ = true;
            }
            break;
        case kTuning:
        case kSeeking: {
            // spec 22 §2.5 step 3 / §2.6 state 2: full reads until STC.
            if (!readAll()) return fail("STC read");
            const uint32_t limit = state_ == kTuning ? kTuneTimeoutMs : kSeekTimeoutMs;
            if (!(regs_[STATUSRSSI] & ST_STC)) {
                if (now - t0_ < limit) return;
                timedOut_ = true;   // stock waits forever; we give up and say so
            }
            // spec 22 §2.5 step 4 / §2.6 state 3: full read; clear TUNE / SEEK; write.
            freq_ = freqFor(regs_[READCHAN]);
            if (!readAll()) return fail("STC read");
            if (state_ == kTuning) regs_[CHANNEL] = uint16_t(regs_[CHANNEL] & ~CH_TUNE);
            else regs_[POWERCFG] = uint16_t(regs_[POWERCFG] & ~PWR_SEEK);
            if (!writeRegs(true)) return fail("STC clear write");
            state_ = state_ == kTuning ? kTuneEnd : kSeekEnd;
            return;
        }
        case kTuneEnd:
        case kSeekEnd: {
            // spec 22 §2.5 step 4 / §2.6 state 4: full reads until STC clears.
            if (!readAll()) return fail("STC read");
            // The safety limit covers the whole tune/seek (both STC waits).
            const uint32_t limit = state_ == kTuneEnd ? kTuneTimeoutMs : kSeekTimeoutMs;
            if (regs_[STATUSRSSI] & ST_STC) {
                if (now - t0_ < limit) return;
                timedOut_ = true;
            }
            freq_ = freqFor(regs_[READCHAN]);
            rssi_ = regs_[STATUSRSSI] & 0xFF;
            stereo_ = regs_[STATUSRSSI] & ST_ST;
            if (timedOut_) tuneFailures_++;
            else tunesDone_++;
            logDone(state_ == kTuneEnd ? "tune" : "seek", now);
            state_ = kReady;
            lastStatus_ = now;
            break;
        }
        default:
            break;
    }
    if (state_ != kReady) return;
    if (regionDirty_ && !pendingTune_ && pendingSeek_ == 0) {
        pendingFreq_ = freq_;   // startTune() writes the new region first
        pendingTune_ = true;
    }
    if (pendingSeek_ != 0) return startSeek();
    if (pendingTune_) return startTune();
    if (now - lastStatus_ >= kRdsPollMs) {
        lastStatus_ = now;
        if (!readAll()) return fail("status read");
        rssi_ = regs_[STATUSRSSI] & 0xFF;
        stereo_ = regs_[STATUSRSSI] & ST_ST;
        if (regs_[STATUSRSSI] & ST_RDSR) decodeRds();
    }
}

void Si4703Radio::clearRds() {
    for (char& c : psName_) c = 0;
    for (char& c : psCand_) c = 0;
    psSeen_ = 0;
    pi_ = 0;
}

void Si4703Radio::decodeRds() {
    // Verbose mode (POWERCFG RDSM): BLERA in STATUSRSSI[10:9], BLERB / BLERD
    // in READCHAN[15:14] / [11:10]; 0 = no errors.
    const int blerA = (regs_[STATUSRSSI] >> 9) & 3;
    const int blerB = (regs_[READCHAN] >> 14) & 3, blerD = (regs_[READCHAN] >> 10) & 3;
    if (blerA == 0 && regs_[RDSA]) pi_ = regs_[RDSA];
    if (blerB != 0 || blerD != 0) return;
    const uint16_t b = regs_[RDSB];
    if ((b >> 12) != 0) return;   // group 0A / 0B: basic tuning and PS name
    const int seg = b & 3;
    const char c0 = char(regs_[RDSD] >> 8), c1 = char(regs_[RDSD] & 0xFF);
    if (c0 < 0x20 || c0 > 0x7E || c1 < 0x20 || c1 > 0x7E) return;
    if (psCand_[2 * seg] == c0 && psCand_[2 * seg + 1] == c1) {
        psName_[2 * seg] = c0;
        psName_[2 * seg + 1] = c1;
        psSeen_ = uint8_t(psSeen_ | (1u << seg));
        for (int i = 0; i < 8; ++i)
            if (!psName_[i]) psName_[i] = ' ';   // the other segments until they come
        psName_[8] = 0;
    }
    psCand_[2 * seg] = c0;
    psCand_[2 * seg + 1] = c1;
}

}  // namespace hal
}  // namespace ot
