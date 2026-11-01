// USB descriptors of the device (third_party/teensy3/usb_desc.c, USB_MIDI_SERIAL
// with the OpenTracker identity): OT_USB_MSC=1 is the composite CDC serial +
// MIDI + mass storage device; OT_USB_MSC=0 must be byte for byte the old
// MIDI + serial device (golden below, dumped from 2.0.0 / 87922f1). Built three
// times (tests/Makefile: usb_desc_test_msc, usb_desc_test_nomsc, and
// usb_desc_test_audio with OT_USB_AUDIO=1: [I] #97 the USB audio function after
// mass storage).
#include "test.h"

#include <set>
#include <vector>

extern "C" {
const uint8_t* ot_usb_device_desc(unsigned* n);
const uint8_t* ot_usb_config_desc(unsigned* n);
const uint8_t* ot_usb_ep_config(unsigned* n);
}

namespace {

const uint8_t kGoldenDevice[18] = {0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 0x40, 0x09,
                                   0x12, 0x01, 0x00, 0x77, 0x02, 0x01, 0x02, 0x03, 0x01};
const uint8_t kGoldenConfig[149] = {
    0x09, 0x02, 0x95, 0x00, 0x03, 0x01, 0x00, 0xC0, 0x32, 0x08, 0x0B, 0x00, 0x02, 0x02, 0x02, 0x01, 0x00, 0x09,
    0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00, 0x05, 0x24, 0x00, 0x10, 0x01, 0x05, 0x24, 0x01, 0x01, 0x01,
    0x04, 0x24, 0x02, 0x06, 0x05, 0x24, 0x06, 0x00, 0x01, 0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x40, 0x09, 0x04,
    0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00, 0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00, 0x07, 0x05, 0x83, 0x02,
    0x40, 0x00, 0x00, 0x09, 0x04, 0x02, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x07, 0x24, 0x01, 0x00, 0x01, 0x25,
    0x00, 0x06, 0x24, 0x02, 0x01, 0x01, 0x05, 0x06, 0x24, 0x02, 0x02, 0x02, 0x05, 0x09, 0x24, 0x03, 0x01, 0x03,
    0x01, 0x02, 0x01, 0x05, 0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01, 0x05, 0x09, 0x05, 0x05, 0x02, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x05, 0x25, 0x01, 0x01, 0x01, 0x09, 0x05, 0x84, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x25, 0x01, 0x01, 0x03};
const uint8_t kGoldenEps[5] = {0x15, 0x19, 0x15, 0x15, 0x19};

#if OT_USB_MSC
struct Iface {
    uint8_t num, alt, eps, cls, sub, proto;
};
struct Ep {
    uint8_t addr, attr;
    uint16_t size;
    uint8_t iface;
};

// Walks a configuration descriptor; false on a malformed chain.
bool parse(const uint8_t* d, unsigned n, std::vector<Iface>& ifs, std::vector<Ep>& eps, int& iads) {
    if (n < 9 || d[0] != 9 || d[1] != 2) return false;
    if (unsigned(d[2] | d[3] << 8) != n) return false;
    iads = 0;
    unsigned i = 0;
    uint8_t cur = 0xFF;
    while (i < n) {
        const uint8_t len = d[i], type = d[i + 1];
        if (len < 2 || i + len > n) return false;
        if (type == 4) {
            if (len != 9) return false;
            ifs.push_back({d[i + 2], d[i + 3], d[i + 4], d[i + 5], d[i + 6], d[i + 7]});
            cur = d[i + 2];
        } else if (type == 5) {
            if (len != 7 && len != 9) return false;
            eps.push_back({d[i + 2], d[i + 3], uint16_t(d[i + 4] | d[i + 5] << 8), cur});
        } else if (type == 11) {
            if (len != 8) return false;
            ++iads;
        }
        i += len;
    }
    return i == n;
}
#endif

}  // namespace

#ifndef OT_USB_AUDIO
#define OT_USB_AUDIO 0
#endif

#if OT_USB_MSC && !OT_USB_AUDIO
TEST(msc_composite_layout) {
    unsigned n = 0;
    const uint8_t* dev = ot_usb_device_desc(&n);
    CHECK_EQ(n, 18u);
    CHECK(std::memcmp(dev, kGoldenDevice, 18) == 0);   // same identity, class 0 (per interface)
    const uint8_t* d = ot_usb_config_desc(&n);
    CHECK_EQ(n, unsigned(sizeof kGoldenConfig) + 23);   // + interface (9) + 2 endpoints (7 + 7)
    std::vector<Iface> ifs;
    std::vector<Ep> eps;
    int iads = 0;
    CHECK(parse(d, n, ifs, eps, iads));
    CHECK_EQ(int(d[4]), 4);   // bNumInterfaces
    CHECK_EQ(int(d[8]), 50);  // bMaxPower: 100 mA as before
    CHECK_EQ(iads, 1);        // CDC only: MSC is a single-interface function
    CHECK_EQ(ifs.size(), size_t(4));
    std::set<int> nums;
    for (const Iface& f : ifs) nums.insert(f.num);
    CHECK(nums == std::set<int>({0, 1, 2, 3}));
    // The first three interfaces are untouched (old bytes after the header).
    CHECK(std::memcmp(d + 9, kGoldenConfig + 9, sizeof kGoldenConfig - 9) == 0);
    // Interface 3: Mass Storage, SCSI transparent, Bulk-Only, 2 endpoints.
    const Iface& m = ifs.back();
    CHECK_EQ(int(m.num), 3);
    CHECK_EQ(int(m.cls), 0x08);
    CHECK_EQ(int(m.sub), 0x06);
    CHECK_EQ(int(m.proto), 0x50);
    CHECK_EQ(int(m.eps), 2);
    // Endpoints: unique addresses, numbers within the table, directions match
    // the endpoint configuration table; MSC = bulk IN 0x86 + bulk OUT 0x07, 64 B.
    unsigned ne = 0;
    const uint8_t* cfg = ot_usb_ep_config(&ne);
    CHECK_EQ(ne, 7u);
    std::set<int> addrs;
    int mscIn = 0, mscOut = 0;
    for (const Ep& e : eps) {
        CHECK(addrs.insert(e.addr).second);
        const int num = e.addr & 0x0F;
        CHECK(num >= 1 && num <= int(ne));
        if (num < 1 || num > int(ne)) continue;
        const uint8_t c = cfg[num - 1];
        if (e.addr & 0x80) CHECK(c == 0x15 || c == 0x1D);   // TX enabled
        else CHECK(c == 0x19 || c == 0x1D);                 // RX enabled
        CHECK(e.size <= 64);
        if (e.iface == 3) {
            CHECK_EQ(int(e.attr), 0x02);
            CHECK_EQ(int(e.size), 64);
            if (e.addr == 0x86) ++mscIn;
            if (e.addr == 0x07) ++mscOut;
        }
    }
    CHECK_EQ(mscIn, 1);
    CHECK_EQ(mscOut, 1);
    CHECK_EQ(eps.size(), size_t(7));
    // Endpoints of the old interfaces keep their configuration.
    CHECK(std::memcmp(cfg, kGoldenEps, 5) == 0);
}
#elif OT_USB_AUDIO
// [I] #97: interfaces 0..3 exactly as in the MSC composite (Windows keeps its
// per-interface drivers), then the audio function as 4..6.
TEST(audio_after_msc_layout) {
    unsigned n = 0;
    const uint8_t* dev = ot_usb_device_desc(&n);
    CHECK_EQ(n, 18u);
    CHECK(std::memcmp(dev, kGoldenDevice, 18) == 0);
    const uint8_t* d = ot_usb_config_desc(&n);
    constexpr unsigned kMsc = sizeof kGoldenConfig + 23;
    std::vector<Iface> ifs;
    std::vector<Ep> eps;
    int iads = 0;
    CHECK(parse(d, n, ifs, eps, iads));
    CHECK_EQ(n, kMsc + 182u);   // the Teensy audio block less its Feature Unit
    CHECK_EQ(int(d[4]), 7);     // bNumInterfaces
    CHECK_EQ(iads, 2);          // CDC and audio
    // Interfaces 0..2 byte for byte the old device, interface 3 Mass Storage.
    CHECK(std::memcmp(d + 9, kGoldenConfig + 9, sizeof kGoldenConfig - 9) == 0);
    CHECK_EQ(int(d[sizeof kGoldenConfig + 2]), 3);
    CHECK_EQ(int(d[sizeof kGoldenConfig + 5]), 0x08);
    // In descriptor order the interface numbers never go down.
    for (size_t i = 1; i < ifs.size(); ++i) CHECK(ifs[i].num >= ifs[i - 1].num);
    // The audio IAD: interfaces 4..6, class Audio.
    const uint8_t* iad = d + kMsc;
    CHECK_EQ(int(iad[1]), 11);
    CHECK_EQ(int(iad[2]), 4);
    CHECK_EQ(int(iad[3]), 3);
    CHECK_EQ(int(iad[4]), 1);
    // AudioControl: interface 4; its class-specific descriptors add up to the
    // header's wTotalLength, terminals 1 -> 2 and 3 -> 4, no Feature Unit.
    const uint8_t* ac = iad + 8;
    CHECK_EQ(int(ac[2]), 4);
    CHECK_EQ(int(ac[5]), 1);
    CHECK_EQ(int(ac[6]), 1);
    const uint8_t* cs = ac + 9;
    const unsigned total = unsigned(cs[5] | cs[6] << 8);
    unsigned sum = 0, k = 0;
    bool fu = false;
    int otSource = -1;
    while (cs[k + 1] == 0x24) {
        if (cs[k + 2] == 0x06) fu = true;
        if (cs[k + 2] == 0x03 && cs[k + 3] == 4) otSource = cs[k + 7];
        sum += cs[k];
        k += cs[k];
    }
    CHECK_EQ(sum, total);
    CHECK_EQ(total, 52u);
    CHECK(!fu);
    CHECK_EQ(otSource, 3);
    CHECK_EQ(int(cs[8]), 5);   // baInterfaceNr: 5 (to the computer), 6 (from it)
    CHECK_EQ(int(cs[9]), 6);
    // Streaming interfaces 5 and 6: alternate 0 without endpoints, 1 with.
    int alt1[7] = {};
    for (const Iface& f : ifs)
        if (f.num >= 5) {
            CHECK_EQ(int(f.cls), 1);
            CHECK_EQ(int(f.sub), 2);
            CHECK_EQ(int(f.eps), f.alt ? (f.num == 5 ? 1 : 2) : 0);
            if (f.alt) ++alt1[f.num];
        }
    CHECK_EQ(alt1[5], 1);
    CHECK_EQ(alt1[6], 1);
    // Endpoints: 8 iso IN (data to the computer), 9 iso OUT asynchronous with
    // feedback endpoint 10 (3 bytes, 10.14); table entries match.
    unsigned ne = 0;
    const uint8_t* cfg = ot_usb_ep_config(&ne);
    CHECK_EQ(ne, 10u);
    CHECK(std::memcmp(cfg, kGoldenEps, 5) == 0);
    CHECK_EQ(int(cfg[5]), 0x15);
    CHECK_EQ(int(cfg[6]), 0x19);
    CHECK_EQ(int(cfg[7]), 0x14);
    CHECK_EQ(int(cfg[8]), 0x18);
    CHECK_EQ(int(cfg[9]), 0x14);
    std::set<int> addrs;
    int seen = 0;
    for (const Ep& e : eps) {
        CHECK(addrs.insert(e.addr).second);
        if (e.addr == 0x88) {
            CHECK_EQ(int(e.attr & 0x03), 1);
            CHECK_EQ(int(e.size), 180);
            ++seen;
        } else if (e.addr == 0x09) {
            CHECK_EQ(int(e.attr), 0x05);   // isochronous, asynchronous
            CHECK_EQ(int(e.size), 180);
            ++seen;
        } else if (e.addr == 0x8A) {
            CHECK_EQ(int(e.attr), 0x11);   // isochronous, feedback
            CHECK_EQ(int(e.size), 3);
            ++seen;
        }
    }
    CHECK_EQ(seen, 3);
    // The OUT endpoint names the feedback endpoint (bSynchAddress).
    for (unsigned i = 0; i + 9 <= n; i += d[i])
        if (d[i + 1] == 5 && d[i + 2] == 0x09) CHECK_EQ(int(d[i + 8]), 0x8A);
}
#else
TEST(nomsc_is_the_old_device_exactly) {
    unsigned n = 0;
    const uint8_t* d = ot_usb_device_desc(&n);
    CHECK_EQ(n, 18u);
    CHECK(std::memcmp(d, kGoldenDevice, 18) == 0);
    d = ot_usb_config_desc(&n);
    CHECK_EQ(n, unsigned(sizeof kGoldenConfig));
    CHECK(n == sizeof kGoldenConfig && std::memcmp(d, kGoldenConfig, n) == 0);
    d = ot_usb_ep_config(&n);
    CHECK_EQ(n, 5u);
    CHECK(n == 5 && std::memcmp(d, kGoldenEps, 5) == 0);
}
#endif
