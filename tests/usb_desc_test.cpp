// USB descriptors of the device (third_party/teensy3/usb_desc.c, USB_MIDI_SERIAL
// with the OpenTracker identity): OT_USB_MSC=1 is the composite CDC serial +
// MIDI + mass storage device; OT_USB_MSC=0 must be byte for byte the old
// MIDI + serial device (golden below, dumped from 2.0.0 / 87922f1). Built twice
// (tests/Makefile: usb_desc_test_msc, usb_desc_test_nomsc), one per OT_USB_MSC.
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

#if OT_USB_MSC
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
