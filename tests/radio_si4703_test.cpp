// Si4703 driver register sequences (src/hal/radio_si4703.cpp) against a fake
// chip: the stock 1.9.2 lifecycle and tune handshake of spec 22 §2.
#include "test.h"

#include <cstdarg>
#include <string>
#include <vector>

#include "hal/radio_si4703.cpp"   // platform independent; not part of libcore

using ot::hal::Si4703Radio;

namespace {

// Minimal Si4703 model: 16 registers, 2-wire burst access, TUNE/SEEK -> STC.
struct FakeChip {
    uint16_t r[16] = {};
    uint32_t now = 0;
    int resets = 0, reads = 0, writes = 0;
    bool readSinceWrite = false;     // a full read happened since the last write
    bool rmwViolation = false;       // a write not preceded by a full read
    bool answer = true;              // false: NAK everything
    bool stcNever = false;           // chip never completes a tune
    uint32_t stcDelayMs = 30;
    uint32_t tuneAt = 0;
    bool tuning = false, seeking = false;
    std::vector<std::string> log;

    void reset() {
        resets++;
        for (auto& v : r) v = 0;
        r[0x00] = 0x1242;
        r[0x01] = 0x1253;
        r[0x07] = 0x0100;
        tuning = seeking = false;
    }
    void tick() {
        if ((tuning || seeking) && !stcNever && now - tuneAt >= stcDelayMs) {
            r[0x0A] |= 0x4000;   // STC
            if (tuning) r[0x0B] = uint16_t((r[0x0B] & 0xFC00) | (r[0x03] & 0x3FF));
            else r[0x0B] = uint16_t((r[0x0B] & 0xFC00) | 123);
            r[0x0A] = uint16_t((r[0x0A] & 0xFF00) | 42);   // RSSI 42
        }
    }
    bool read(uint8_t* b, uint32_t n) {
        if (!answer) return false;
        tick();
        reads++;
        if (n == 32) readSinceWrite = true;
        for (uint32_t i = 0; i < n / 2; ++i) {
            const uint16_t v = r[(0x0A + i) & 15];
            b[2 * i] = uint8_t(v >> 8);
            b[2 * i + 1] = uint8_t(v);
        }
        return true;
    }
    bool write(const uint8_t* b, uint32_t n) {
        if (!answer) return false;
        if (!readSinceWrite) rmwViolation = true;
        readSinceWrite = false;
        writes++;
        const bool tuneWas = r[0x03] & 0x8000, seekWas = r[0x02] & 0x0100;
        for (uint32_t i = 0; i < n / 2; ++i) r[0x02 + i] = uint16_t((b[2 * i] << 8) | b[2 * i + 1]);
        const bool tuneNow = r[0x03] & 0x8000, seekNow = r[0x02] & 0x0100;
        const bool enabled = (r[0x02] & 0x0041) == 0x0001;
        if (!tuneWas && tuneNow && enabled) { tuning = true; tuneAt = now; }
        if (!seekWas && seekNow && enabled) { seeking = true; tuneAt = now; }
        if ((tuning && !tuneNow) || (seeking && !seekNow)) {
            tuning = seeking = false;
            r[0x0A] = uint16_t(r[0x0A] & ~0x4000);   // STC clears with TUNE/SEEK
        }
        return true;
    }
};

FakeChip* gChip = nullptr;

void logLine(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    gChip->log.push_back(buf);
}

Si4703Radio::Port port() {
    Si4703Radio::Port p;
    p.read = [](uint8_t* b, uint32_t n) { return gChip->read(b, n); };
    p.write = [](const uint8_t* b, uint32_t n) { return gChip->write(b, n); };
    p.millis = []() { return gChip->now; };
    p.resetPulse = []() { gChip->reset(); };
    p.log = &logLine;
    return p;
}

void run(Si4703Radio& r, FakeChip& c, uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
        c.now++;
        r.service();
    }
}

bool logHas(const FakeChip& c, const char* s) {
    for (const auto& l : c.log)
        if (l.find(s) != std::string::npos) return true;
    return false;
}

// Boot + recorder selection (EU band), until the first tune is done.
void bringUp(Si4703Radio& r, FakeChip& c) {
    r.setPort(port());
    CHECK(r.boot());
    run(r, c, 600);   // the recorder opens later than 500 ms after boot
    CHECK(r.powerOn());
    r.setBand(87.5f, 108.0f, 0.1f);
    r.tune(98.1f);
}

}  // namespace

TEST(boot_resets_once_and_starts_the_crystal) {
    FakeChip c;
    gChip = &c;
    Si4703Radio r;
    r.setPort(port());
    CHECK(r.boot());
    CHECK_EQ(c.resets, 1);
    CHECK_EQ(c.r[0x07], 0x8100);     // spec 22 §2.2 step 9: TEST1 = XOSCEN | bit 8
    CHECK_EQ(c.r[0x02], 0x0000);     // not enabled at boot
    CHECK(!r.powered());
    CHECK_EQ(r.deviceId(), 0x1242);
    CHECK(!c.rmwViolation);
}

TEST(power_up_waits_for_the_crystal_and_writes_stock_values) {
    FakeChip c;
    gChip = &c;
    Si4703Radio r;
    r.setPort(port());
    CHECK(r.boot());
    CHECK(r.powerOn());                 // right after boot
    r.setBand(87.5f, 108.0f, 0.1f);
    r.tune(98.1f);
    run(r, c, 499);
    CHECK_EQ(c.r[0x02], 0x0000);        // crystal has not run 500 ms yet
    run(r, c, 1);
    CHECK_EQ(c.r[0x02], 0xE801);        // spec 22 §2.3 step 3
    CHECK_EQ(c.r[0x05] & 0xFF0F, 0x190A);
    CHECK_EQ(c.r[0x06] & 0x0048, 0x0048);
    CHECK_EQ(c.r[0x03] & 0x8000, 0);    // CHANNEL not written at power-up
    CHECK(r.busy());
    run(r, c, 109);
    CHECK_EQ(c.r[0x05] & 0x00F0, 0x0000);   // region bits only after the 110 ms
    run(r, c, 200);
    CHECK_EQ(c.r[0x05] & 0x00F0, 0x0010);   // EU: SPACE 100 kHz, BAND 87.5-108
    CHECK_EQ(c.r[0x04] & 0x0800, 0x0800);   // DE 50 us
    CHECK(!r.busy());
    CHECK_EQ(c.r[0x0B] & 0x3FF, 106);       // (98.1 - 87.5) / 0.1
    CHECK_NEAR(r.frequency(), 98.1, 1e-3);
    CHECK_EQ(c.resets, 1);
    CHECK(!c.rmwViolation);                 // every write was a read-modify-write
}

TEST(tune_handshake_clears_tune_and_waits_for_stc_low) {
    FakeChip c;
    gChip = &c;
    Si4703Radio r;
    bringUp(r, c);
    run(r, c, 400);
    CHECK(!r.busy());
    CHECK_EQ(r.tunesDone(), 1u);
    CHECK_EQ(c.r[0x03] & 0x8000, 0);        // TUNE cleared after STC (spec 22 §2.5 step 4)
    CHECK_EQ(c.r[0x0A] & 0x4000, 0);        // STC low again
    CHECK(logHas(c, "radio: tune done"));
    CHECK(logHas(c, "POWERCFG 0xE801"));
    CHECK(logHas(c, "READCHAN 0x006A"));
    CHECK(logHas(c, "RSSI 42"));
    CHECK_EQ(r.rssi(), 42);
    // A second tune while ready: TUNE | chan, the old CHANNEL high bits kept.
    r.tune(100.0f);
    CHECK_EQ(c.r[0x03], 0x8000 | 125);
    run(r, c, 100);
    CHECK_EQ(c.r[0x03], 125);
    CHECK_EQ(r.tunesDone(), 2u);
    CHECK(!c.rmwViolation);
}

TEST(tune_without_stc_is_a_logged_failure_after_the_timeout) {
    FakeChip c;
    gChip = &c;
    c.stcNever = true;
    Si4703Radio r;
    bringUp(r, c);
    run(r, c, 110 + Si4703Radio::kTuneTimeoutMs - 50);
    CHECK(r.busy());                        // no 200 ms "done": still waiting
    CHECK_EQ(c.r[0x03] & 0x8000, 0x8000);
    run(r, c, 100);
    CHECK(!r.busy());
    CHECK_EQ(r.tuneFailures(), 1u);
    CHECK_EQ(r.tunesDone(), 0u);
    CHECK_EQ(c.r[0x03] & 0x8000, 0);        // TUNE cleared anyway
    CHECK(logHas(c, "radio: tune FAILED (no STC)"));
}

TEST(power_off_disables_without_reset_and_power_on_reenables) {
    FakeChip c;
    gChip = &c;
    Si4703Radio r;
    bringUp(r, c);
    run(r, c, 400);
    c.r[0x04] |= 0x1000;                    // RDS on (as stock power-up would leave it)
    r.powerOff();
    CHECK(!r.powered());
    CHECK_EQ(c.r[0x02] & 0x0041, 0x0041);   // DISABLE with ENABLE still set (spec 22 §2.7)
    CHECK_EQ(c.r[0x02] & 0xE800, 0xE800);   // other bits kept
    CHECK_EQ(c.r[0x04] & 0x1000, 0);        // RDS off
    CHECK_EQ(c.resets, 1);
    // Next radio selection: no reset, crystal long settled -> enable at once.
    CHECK(r.powerOn());
    r.tune(98.1f);
    run(r, c, 1);
    CHECK_EQ(c.r[0x02], 0xE801);            // DISABLE cleared by the 0xE801 write
    run(r, c, 300);
    CHECK(!r.busy());
    CHECK_EQ(c.resets, 1);                  // RST_N never pulsed again
    CHECK(!c.rmwViolation);
}

TEST(seek_sets_and_clears_the_seek_bit) {
    FakeChip c;
    gChip = &c;
    Si4703Radio r;
    bringUp(r, c);
    run(r, c, 400);
    CHECK(r.seek(true));
    CHECK_EQ(c.r[0x02] & 0x0700, 0x0300);   // SKMODE 0, SEEKUP, SEEK
    run(r, c, 100);
    CHECK(!r.busy());
    CHECK_EQ(c.r[0x02] & 0x0100, 0);
    CHECK_NEAR(r.frequency(), 87.5 + 12.3, 1e-3);
    CHECK(r.seek(false));
    CHECK_EQ(c.r[0x02] & 0x0700, 0x0100);   // down: SEEKUP 0
    run(r, c, 100);
    CHECK(!r.busy());
    CHECK(logHas(c, "radio: seek done"));
}

TEST(i2c_failure_resets_on_the_next_power_on) {
    FakeChip c;
    gChip = &c;
    Si4703Radio r;
    bringUp(r, c);
    run(r, c, 400);
    c.answer = false;
    r.tune(99.0f);                          // read fails -> dropped
    CHECK(!r.powered());
    CHECK(logHas(c, "radio: I2C failure"));
    c.answer = true;
    CHECK(r.powerOn());                     // stock failure path: reset + power-up
    CHECK_EQ(c.resets, 2);
    run(r, c, 700);
    CHECK(!r.busy());
    CHECK_EQ(c.r[0x02], 0xE801);
}

TEST(region_change_rewrites_band_bits_before_the_tune) {
    FakeChip c;
    gChip = &c;
    Si4703Radio r;
    bringUp(r, c);
    run(r, c, 400);
    r.setRegion(3);
    r.setBand(76.0f, 108.0f, 0.1f);         // Japan (spec 22 §2.4)
    r.tune(80.0f);
    CHECK_EQ(c.r[0x05] & 0x00F0, 0x0050);
    CHECK_EQ(c.r[0x03] & 0x3FF, 40);
    r.setRegion(1);
    r.setBand(87.5f, 108.0f, 0.2f);         // US: 200 kHz, 75 us
    run(r, c, 100);
    r.tune(98.1f);
    run(r, c, 100);
    CHECK_EQ(c.r[0x05] & 0x00F0, 0x0000);
    CHECK_EQ(c.r[0x04] & 0x0800, 0);
    CHECK_EQ(c.r[0x03] & 0x3FF, 53);        // trunc(10.6 / 0.2 + 0.5)
    CHECK(!c.rmwViolation);
}

// OT fork: RDS is enabled with the region bits and the PS name is decoded from
// group 0A blocks (two matching receptions per segment), cleared on a tune.
TEST(rds_ps_name_decoded_and_cleared_on_tune) {
    FakeChip c;
    gChip = &c;
    Si4703Radio r;
    bringUp(r, c);
    run(r, c, 400);
    CHECK(c.r[0x04] & 0x1000);   // SYSCONFIG1.RDS
    CHECK_STR(r.rdsName(), "");
    const char* ps = "JUNGLEFM";
    auto group = [&](int seg, bool bad) {
        c.r[0x0A] = uint16_t((c.r[0x0A] & ~0x0600) | 0x8000);   // RDSR, BLERA 0
        c.r[0x0C] = 0xC204;                                     // PI
        c.r[0x0D] = uint16_t(0x0000 | seg);                     // group 0A, segment
        c.r[0x0F] = uint16_t((uint8_t(ps[2 * seg]) << 8) | uint8_t(ps[2 * seg + 1]));
        c.r[0x0B] = uint16_t((c.r[0x0B] & 0x03FF) | (bad ? 0x4000 : 0));   // BLERB
        run(r, c, 45);
    };
    for (int pass = 0; pass < 2; ++pass)
        for (int seg = 0; seg < 4; ++seg) group(seg, false);
    CHECK_STR(r.rdsName(), "JUNGLEFM");
    CHECK_EQ(r.rdsPi(), 0xC204);
    // A corrupted block B is ignored.
    ps = "XXXXXXXX";
    group(0, true);
    group(0, true);
    CHECK_STR(r.rdsName(), "JUNGLEFM");
    r.tune(99.5f);
    run(r, c, 5);
    CHECK_STR(r.rdsName(), "");
    CHECK(!c.rmwViolation);
}
