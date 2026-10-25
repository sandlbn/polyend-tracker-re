// USB mass-storage bulk endpoint driver (third_party/teensy3/usb_msc_impl.h,
// included by usb_dev.c on the device) against a simulated Kinetis USB-FS
// controller: buffer descriptor table with EVEN / ODD banks per direction,
// the OWN bit, DATA0/1 with DTS, byte counts, EPSTALL, and the TOKDNE status
// byte (endpoint, TX, ODD) handed to usb_msc_token(). The host side sends and
// takes 64-byte packets the way a USB host does. The last tests run the BOT /
// SCSI layer (src/core/usb/msc_bot.*) on top of the driver.
#include "test.h"

#include <cstring>
#include <vector>

#include "usb/msc_bot.h"

// ---- what usb_dev.c provides to the driver -----------------------------------
extern "C" {
#include "usb_msc.h"
}

typedef volatile struct {
    uint32_t desc;
    void* addr;
} bdt_t;
static bdt_t table[(NUM_ENDPOINTS + 1) * 4];
static volatile uint8_t gEndpt[(NUM_ENDPOINTS + 1) * 4];
static volatile uint8_t usb_configuration = 1;
static int gPended = 0;
#define USB0_ENDPT0 (gEndpt[0])
#define USB_ENDPT_EPSTALL 0x02
#define NVIC_SET_PENDING(n) (++gPended)
#define IRQ_USBOTG 0
#define BDT_OWN 0x80
#define BDT_DATA1 0x40
#define BDT_DATA0 0x00
#define BDT_DTS 0x08
#define BDT_DESC(count, data) (BDT_OWN | BDT_DTS | ((data) ? BDT_DATA1 : BDT_DATA0) | ((count) << 16))
#define TX 1
#define RX 0
#define ODD 1
#define EVEN 0
#define index(endpoint, tx, odd) (((endpoint) << 2) | ((tx) << 1) | (odd))
#define stat2bufferdescriptor(stat) (table + ((stat) >> 2))
#define USB_MSC_DMA_MEM

#include "usb_msc_impl.h"

namespace {

// ---- the controller (SIE) and the host --------------------------------------
struct Sie {
    uint8_t odd[2] = {0, 0};        // bank the SIE uses next, [RX], [TX] (ODDRST: even)
    uint8_t hostToggle[2] = {0, 0};  // DATA0/1 the host sends / expects next
    int overruns = 0;                // OUT packet larger than the armed BC (DMAERR)
    int tokens = 0;
    bool reorderStat = false;        // test: hand the token a stale ODD bit

    void busReset() {
        odd[0] = odd[1] = 0;
        hostToggle[0] = hostToggle[1] = 0;
        usb_msc_bus_reset();
    }
    void setConfiguration() {
        hostToggle[0] = hostToggle[1] = 0;
        usb_msc_configure();
    }
    enum Tok { kAck, kNak, kStall, kDropped };
    // One OUT packet to endpoint 7.
    Tok out(const uint8_t* p, uint32_t n) {
        if (gEndpt[MSC_RX_ENDPOINT * 4] & USB_ENDPT_EPSTALL) return kStall;
        bdt_t* b = &table[index(MSC_RX_ENDPOINT, RX, odd[RX])];
        if (!(b->desc & BDT_OWN)) return kNak;
        const uint8_t tog = (b->desc & BDT_DATA1) ? 1 : 0;
        if (tog != hostToggle[RX]) {
            // DTS: a packet with the wrong toggle (a retransmission) is ACKed
            // and dropped; the BDT stays armed.
            hostToggle[RX] ^= 1;
            return kDropped;
        }
        const uint32_t bc = b->desc >> 16 & 0x3FF;
        uint32_t k = n;
        if (n > bc) {
            ++overruns;   // DMAERR: the controller writes at most BC bytes
            k = bc;
        }
        std::memcpy(b->addr, p, k);
        b->desc = (b->desc & ~(0x3FFu << 16 | BDT_OWN)) | (k << 16);
        hostToggle[RX] ^= 1;
        token(MSC_RX_ENDPOINT, RX);
        return kAck;
    }
    // One IN packet from endpoint 6.
    Tok in(std::vector<uint8_t>& got, uint32_t* n) {
        *n = 0;
        if (gEndpt[MSC_TX_ENDPOINT * 4] & USB_ENDPT_EPSTALL) return kStall;
        bdt_t* b = &table[index(MSC_TX_ENDPOINT, TX, odd[TX])];
        if (!(b->desc & BDT_OWN)) return kNak;
        const uint8_t tog = (b->desc & BDT_DATA1) ? 1 : 0;
        CHECK_EQ(int(tog), int(hostToggle[TX]));
        const uint32_t bc = b->desc >> 16 & 0x3FF;
        const uint8_t* a = static_cast<const uint8_t*>(b->addr);
        got.insert(got.end(), a, a + bc);
        b->desc &= ~uint32_t(BDT_OWN);
        hostToggle[TX] ^= 1;
        *n = bc;
        token(MSC_TX_ENDPOINT, TX);
        return kAck;
    }
    void token(int ep, int tx) {
        const uint8_t bank = odd[tx];
        odd[tx] ^= 1;
        ++tokens;
        usb_msc_token(uint8_t(ep << 4 | tx << 3 | bank << 2));
    }
    void clearHalt(bool in) {
        gEndpt[(in ? MSC_TX_ENDPOINT : MSC_RX_ENDPOINT) * 4] &= ~USB_ENDPT_EPSTALL;
        hostToggle[in ? TX : RX] = 0;
        usb_msc_clear_halt(in ? 1 : 0);
    }
    void classReset() { usb_msc_class_reset(); }
};

// Driver state lives in file-scope statics: every test starts from a bus reset
// and a new configuration, as after enumeration.
void (*gNext)(uint8_t) = nullptr;
void hook(uint8_t ev) {
    if (gNext) gNext(ev);
}
Sie fresh() {
    std::memset(const_cast<uint8_t*>(gEndpt), 0, sizeof gEndpt);
    gNext = nullptr;
    usb_msc_set_hook(&hook);
    Sie s;
    s.busReset();
    s.setConfiguration();
    return s;
}

std::vector<uint8_t> pattern(uint32_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 13 + i / 64);
    return v;
}

// Sends `data` as 64-byte packets (the last one short); returns packets ACKed.
int sendAll(Sie& s, const std::vector<uint8_t>& data) {
    int acked = 0;
    for (uint32_t off = 0; off < data.size();) {
        const uint32_t n = data.size() - off < 64 ? uint32_t(data.size() - off) : 64;
        const Sie::Tok t = s.out(data.data() + off, n);
        if (t != Sie::kAck) break;
        ++acked;
        off += n;
    }
    return acked;
}

}  // namespace

TEST(rx_segment_by_packets) {
    Sie s = fresh();
    static uint8_t buf[4096];
    CHECK(usb_msc_rx_queue(buf, sizeof buf));
    const std::vector<uint8_t> d = pattern(4096, 1);
    CHECK_EQ(sendAll(s, d), 64);
    uint32_t got = 0;
    CHECK(usb_msc_rx_poll(&got));
    CHECK_EQ(got, 4096u);
    CHECK(std::memcmp(buf, d.data(), d.size()) == 0);
    CHECK(!usb_msc_rx_poll(&got));
    CHECK_EQ(s.overruns, 0);
    // Nothing armed any more: the next packet is NAKed.
    CHECK(s.out(d.data(), 64) == Sie::kNak);
}

TEST(rx_short_packet_ends_segment) {
    Sie s = fresh();
    static uint8_t cbw[64];
    CHECK(usb_msc_rx_queue(cbw, 64));
    const std::vector<uint8_t> d = pattern(31, 7);
    CHECK(s.out(d.data(), 31) == Sie::kAck);
    uint32_t got = 0;
    CHECK(usb_msc_rx_poll(&got));
    CHECK_EQ(got, 31u);
    CHECK(std::memcmp(cbw, d.data(), 31) == 0);
}

TEST(tx_segments_and_short_tail) {
    Sie s = fresh();
    static uint8_t a[200], b[13];
    std::memcpy(a, pattern(200, 3).data(), 200);
    std::memcpy(b, pattern(13, 9).data(), 13);
    CHECK(usb_msc_tx_queue(a, sizeof a));
    CHECK(usb_msc_tx_queue(b, sizeof b));
    std::vector<uint8_t> got;
    uint32_t n = 0, total = 0;
    for (int i = 0; i < 10 && s.in(got, &n) == Sie::kAck; ++i) total += n;
    CHECK_EQ(total, 213u);
    CHECK(std::memcmp(got.data(), a, 200) == 0);
    CHECK(std::memcmp(got.data() + 200, b, 13) == 0);
    CHECK(usb_msc_tx_poll());
    CHECK(usb_msc_tx_poll());
    CHECK(!usb_msc_tx_poll());
}

TEST(stall_and_clear_halt_restart_at_data0) {
    Sie s = fresh();
    static uint8_t buf[128];
    CHECK(usb_msc_rx_queue(buf, 128));
    const std::vector<uint8_t> d = pattern(64, 2);
    CHECK(s.out(d.data(), 64) == Sie::kAck);   // DATA0 used, next DATA1
    usb_msc_stall(0);
    CHECK(usb_msc_stalled(0));
    CHECK(s.out(d.data(), 64) == Sie::kStall);
    s.clearHalt(false);
    CHECK(!usb_msc_stalled(0));
    CHECK(s.out(d.data(), 64) == Sie::kAck);   // DATA0 again on both sides
    uint32_t got = 0;
    CHECK(usb_msc_rx_poll(&got));
    CHECK_EQ(got, 128u);
}

TEST(done_bank_follows_the_token_odd_bit) {
    // Review F3: the bank the controller uses next comes from the ODD bit of
    // the completed token. Even with the driver's own record off by one, the
    // first token puts it right: after a halt is cleared the endpoint is
    // re-armed on the bank the controller reads next.
    Sie s = fresh();
    static uint8_t buf[256];
    CHECK(usb_msc_rx_queue(buf, sizeof buf));
    msc_dir[RX].donebank ^= 1;   // simulated disagreement
    const std::vector<uint8_t> d = pattern(256, 4);
    CHECK(s.out(d.data(), 64) == Sie::kAck);   // completes the EVEN bank
    usb_msc_stall(0);
    s.clearHalt(false);                         // disarm, re-arm from the done bank
    CHECK(s.out(d.data() + 64, 64) == Sie::kAck);
    CHECK(s.out(d.data() + 128, 64) == Sie::kAck);
    CHECK(s.out(d.data() + 192, 64) == Sie::kAck);
    uint32_t got = 0;
    CHECK(usb_msc_rx_poll(&got));
    CHECK_EQ(got, 256u);
    CHECK(std::memcmp(buf, d.data(), 256) == 0);
}

TEST(rx_tail_is_armed_with_a_full_packet_size) {
    // Review B #4: a receive tail (< 64 bytes left in the segment) is armed
    // with BC = 64 into scratch, so a full packet never overruns the BDT.
    Sie s = fresh();
    static uint8_t buf[100 + 8];
    std::memset(buf, 0xEE, sizeof buf);
    CHECK(usb_msc_rx_queue(buf, 100));
    const std::vector<uint8_t> d = pattern(128, 5);
    CHECK(s.out(d.data(), 64) == Sie::kAck);
    CHECK(s.out(d.data() + 64, 64) == Sie::kAck);   // host sends more than asked for
    CHECK_EQ(s.overruns, 0);
    uint32_t got = 0;
    CHECK(usb_msc_rx_poll(&got));
    CHECK_EQ(got, 100u);
    CHECK(std::memcmp(buf, d.data(), 100) == 0);
    CHECK_EQ(int(buf[100]), 0xEE);   // nothing past the segment
    // The normal case: the tail arrives as a short packet.
    CHECK(usb_msc_rx_queue(buf, 100));
    const std::vector<uint8_t> e = pattern(100, 6);
    CHECK_EQ(sendAll(s, e), 2);
    CHECK(usb_msc_rx_poll(&got));
    CHECK_EQ(got, 100u);
    CHECK(std::memcmp(buf, e.data(), 100) == 0);
    CHECK_EQ(s.overruns, 0);
}

// ---- BOT / SCSI on top of the driver -----------------------------------------
namespace {

using namespace ot::msc;

class DrvEp final : public IEndpoints {
public:
    bool configured() const override { return usb_msc_configured() != 0; }
    bool rxQueue(uint8_t* buf, uint32_t len) override { return usb_msc_rx_queue(buf, len) != 0; }
    bool rxPoll(uint32_t* got) override { return usb_msc_rx_poll(got) != 0; }
    bool txQueue(const uint8_t* buf, uint32_t len) override { return usb_msc_tx_queue(buf, len) != 0; }
    bool txPoll() override { return usb_msc_tx_poll() != 0; }
    void rxFlush() override { usb_msc_rx_flush(); }
    void txFlush() override { usb_msc_tx_flush(); }
    void stall(bool in) override { usb_msc_stall(in ? 1 : 0); }
    bool stalled(bool in) const override { return usb_msc_stalled(in ? 1 : 0) != 0; }
};

class MemCard final : public IBlockDevice {
public:
    std::vector<uint8_t> data = std::vector<uint8_t>(2048 * 512);
    int reads = 0, writes = 0;
    MemCard() {
        for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 11 + i / 512);
    }
    bool read(uint32_t lba, uint8_t* dst, uint32_t count) override {
        ++reads;
        std::memcpy(dst, &data[lba * 512], count * 512);
        return true;
    }
    bool write(uint32_t lba, const uint8_t* src, uint32_t count) override {
        ++writes;
        std::memcpy(&data[lba * 512], src, count * 512);
        return true;
    }
};

struct Dev {
    static Dev* cur;
    DrvEp ep;
    MemCard card;
    Bot bot;
    alignas(32) uint8_t buf[16384] = {};   // the device's ring: 4 x 4 KB
    alignas(32) uint8_t small[Bot::kSmallBytes] = {};
    Sie sie;
    uint32_t tag = 500;

    Dev() {
        sie = fresh();
        bot.init(&ep, buf, sizeof buf, small);
        cur = this;
        gNext = [](uint8_t ev) {
            if (ev == USB_MSC_EVENT_RESET) cur->bot.onReset();
            cur->bot.service();
        };
        usb_msc_sof();   // first frame: the CBW receive is queued
        bot.setMedium(true, 2048);
    }
    ~Dev() { gNext = nullptr; }
    // Main loop + a frame.
    void step() {
        if (bot.runIo(card)) usb_msc_kick();
        usb_msc_sof();
    }
    Sie::Tok outRetry(const uint8_t* p, uint32_t n) {
        for (int i = 0; i < 100; ++i) {
            const Sie::Tok t = sie.out(p, n);
            if (t != Sie::kNak) return t;
            step();
        }
        return Sie::kNak;
    }
    bool csw(std::vector<uint8_t>& c) {
        for (int i = 0; i < 200; ++i) {
            uint32_t n = 0;
            const Sie::Tok t = sie.in(c, &n);
            if (t == Sie::kStall) {
                sie.clearHalt(true);
                continue;
            }
            if (t == Sie::kAck) return n == 13;
            step();
        }
        return false;
    }
    void cbw(uint32_t len, bool in, const std::vector<uint8_t>& cdb, uint8_t* c) {
        std::memset(c, 0, 31);
        c[0] = 'U'; c[1] = 'S'; c[2] = 'B'; c[3] = 'C';
        ++tag;
        std::memcpy(c + 4, &tag, 4);
        std::memcpy(c + 8, &len, 4);
        c[12] = in ? 0x80 : 0;
        c[14] = uint8_t(cdb.size());
        std::memcpy(c + 15, cdb.data(), cdb.size());
    }
    // A command without data stage; returns the CSW status (0xFF: no CSW).
    int noData(const std::vector<uint8_t>& cdb) {
        uint8_t c[31];
        cbw(0, false, cdb, c);
        if (outRetry(c, 31) != Sie::kAck) return 0xFF;
        std::vector<uint8_t> s;
        return csw(s) ? s[12] : 0xFF;
    }
    static std::vector<uint8_t> rw10(uint8_t opc, uint32_t lba, uint16_t n) {
        return {opc, 0, uint8_t(lba >> 24), uint8_t(lba >> 16), uint8_t(lba >> 8), uint8_t(lba), 0,
                uint8_t(n >> 8), uint8_t(n), 0};
    }
    bool armedInto(const uint8_t* p, uint32_t len) const {
        for (int tx = 0; tx < 2; ++tx)
            for (int odd = 0; odd < 2; ++odd) {
                const bdt_t& b = table[index(tx ? MSC_TX_ENDPOINT : MSC_RX_ENDPOINT, tx, odd)];
                const uint8_t* a = static_cast<const uint8_t*>(b.addr);
                if ((b.desc & BDT_OWN) && a >= p && a < p + len) return true;
            }
        return false;
    }
};
Dev* Dev::cur = nullptr;

}  // namespace

TEST(bot_write_then_read_through_the_driver) {
    Dev v;
    CHECK_EQ(v.noData({op::kTestUnitReady, 0, 0, 0, 0, 0}), 1);   // UNIT ATTENTION
    CHECK_EQ(v.noData({op::kTestUnitReady, 0, 0, 0, 0, 0}), 0);
    const std::vector<uint8_t> d = pattern(72 * 512, 9);   // 9 chunks through the 4-chunk ring
    uint8_t c[31];
    v.cbw(uint32_t(d.size()), false, Dev::rw10(op::kWrite10, 40, 72), c);
    CHECK(v.outRetry(c, 31) == Sie::kAck);
    for (uint32_t off = 0; off < d.size(); off += 64) CHECK(v.outRetry(d.data() + off, 64) == Sie::kAck);
    std::vector<uint8_t> s;
    CHECK(v.csw(s));
    CHECK_EQ(int(s[12]), 0);
    CHECK(std::memcmp(&v.card.data[40 * 512], d.data(), d.size()) == 0);
    v.cbw(uint32_t(d.size()), true, Dev::rw10(op::kRead10, 40, 72), c);
    CHECK(v.outRetry(c, 31) == Sie::kAck);
    std::vector<uint8_t> got;
    for (int i = 0; i < 20000 && got.size() < d.size(); ++i) {
        uint32_t n = 0;
        if (v.sie.in(got, &n) != Sie::kAck) v.step();
    }
    CHECK(got == d);
    s.clear();
    CHECK(v.csw(s));
    CHECK_EQ(int(s[12]), 0);
    CHECK_EQ(v.sie.overruns, 0);
}

TEST(bot_short_packet_mid_write_keeps_the_next_cbw) {
    // Review B #2 / F1: a short packet ends the WRITE's data stage while the
    // other bank is armed further into the same segment. The next CBW must
    // reach the protocol (not an orphan BDT in the data buffer).
    Dev v;
    v.noData({op::kTestUnitReady, 0, 0, 0, 0, 0});
    const std::vector<uint8_t> before = v.card.data;
    const std::vector<uint8_t> d = pattern(4096 + 31, 3);
    uint8_t c[31];
    v.cbw(24 * 512, false, Dev::rw10(op::kWrite10, 0, 24), c);
    CHECK(v.outRetry(c, 31) == Sie::kAck);
    for (uint32_t off = 0; off < 4096; off += 64) CHECK(v.outRetry(d.data() + off, 64) == Sie::kAck);
    CHECK(v.outRetry(d.data() + 4096, 31) == Sie::kAck);
    std::vector<uint8_t> s;
    CHECK(v.csw(s));
    CHECK_EQ(int(s[12]), int(Bot::kStatusPhase));
    CHECK(!v.armedInto(v.buf, sizeof v.buf));
    CHECK(std::memcmp(&v.card.data[0], d.data(), 4096) == 0);
    CHECK(std::memcmp(&v.card.data[4096], &before[4096], 16 * 512) == 0);
    // The next command goes through.
    CHECK_EQ(v.noData({op::kTestUnitReady, 0, 0, 0, 0, 0}), 0);
}

TEST(bot_abort_leaves_nothing_armed_into_the_buffer) {
    // Review B #2 / F2: storage mode ends mid-WRITE (the computer is gone):
    // after abort() no BDT points into the data buffer, which goes back to
    // the app.
    Dev v;
    v.noData({op::kTestUnitReady, 0, 0, 0, 0, 0});
    uint8_t c[31];
    v.cbw(16 * 512, false, Dev::rw10(op::kWrite10, 0, 16), c);
    CHECK(v.outRetry(c, 31) == Sie::kAck);
    const std::vector<uint8_t> d = pattern(128, 1);
    CHECK(v.outRetry(d.data(), 64) == Sie::kAck);
    CHECK(v.armedInto(v.buf, sizeof v.buf));
    CHECK(v.bot.busy());
    v.bot.abort();
    CHECK(!v.armedInto(v.buf, sizeof v.buf));
    CHECK(!v.bot.busy());
    // Host reset recovery: class reset, clear both halts; then commands work.
    v.sie.classReset();
    v.sie.clearHalt(true);
    v.sie.clearHalt(false);
    CHECK(!v.armedInto(v.buf, sizeof v.buf));
    v.step();
    CHECK_EQ(v.noData({op::kTestUnitReady, 0, 0, 0, 0, 0}), 0);
}
