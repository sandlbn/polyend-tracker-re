// SD streaming C3 (docs/PLAN_SD_STREAMING_P1C_P2.md §1.5): the host stream
// reader (IStorage-backed) that host tests and the simulator use in place of
// the device's raw-sector reader. Reads the payload of a bound .pti-like file
// at dataOffset: mono, or planar stereo (L block then R block).
#include "test.h"
#include "ui_fakes.h"

#include <vector>

#include "app/storage_stream_reader.h"

using namespace ot;
using namespace ot::audio;

namespace {
// A file: `hdr` bytes of header, then L (and R) frames of int16.
std::vector<uint8_t> payloadFile(uint32_t hdr, uint32_t frames, bool stereo) {
    std::vector<uint8_t> f(hdr + frames * 2u * (stereo ? 2u : 1u), 0xAB);
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t l = int16_t(i * 3), r = int16_t(-int32_t(i) * 5);
        std::memcpy(&f[hdr + i * 2], &l, 2);
        if (stereo) std::memcpy(&f[hdr + frames * 2 + i * 2], &r, 2);
    }
    return f;
}
StreamSource src(uint8_t ins, uint32_t frames) {
    StreamSource s;
    s.instrument = ins;
    s.frames = frames;
    return s;
}
}  // namespace

TEST(storage_reader_bind_and_read_mono) {
    FakeStorage fs;
    fs.files["Workspace/instruments/instrument_00.pti"] = payloadFile(392, 50000, false);
    StorageStreamReader rd(fs);
    StreamSource s = src(0, 50000);
    StreamBindInfo info;
    CHECK(!rd.bound(0));
    CHECK(rd.bind(s, "Workspace/instruments/instrument_00.pti", 392, false, &info));
    CHECK(rd.bound(0));
    int16_t l[100];
    CHECK(rd.read(s, 40000, 100, l, nullptr));
    for (int i = 0; i < 100; ++i) CHECK_EQ(l[i], int16_t((40000 + i) * 3));
    CHECK(!rd.read(s, 49990, 100, l, nullptr));   // past the end
}

TEST(storage_reader_stereo_planar_offsets) {
    FakeStorage fs;
    fs.files["s.pti"] = payloadFile(392, 40000, true);
    StorageStreamReader rd(fs);
    StreamSource s = src(3, 40000);
    CHECK(rd.bind(s, "s.pti", 392, true, nullptr));
    int16_t l[64], r[64];
    CHECK(rd.read(s, 33000, 64, l, r));
    for (int i = 0; i < 64; ++i) {
        CHECK_EQ(l[i], int16_t((33000 + i) * 3));
        CHECK_EQ(r[i], int16_t(-(33000 + i) * 5));
    }
}

TEST(storage_reader_unbind_all) {
    FakeStorage fs;
    fs.files["a.pti"] = payloadFile(392, 1000, false);
    StorageStreamReader rd(fs);
    StreamSource a = src(1, 1000), b = src(2, 1000);
    CHECK(rd.bind(a, "a.pti", 392, false, nullptr));
    CHECK(rd.bind(b, "a.pti", 392, false, nullptr));
    rd.unbind(1);
    CHECK(!rd.bound(1));
    CHECK(rd.bound(2));
    rd.unbindAll();
    CHECK(!rd.bound(2));
    int16_t l[8];
    CHECK(!rd.read(b, 0, 8, l, nullptr));   // unbound: no read
}

TEST(storage_reader_max_streams) {
    FakeStorage fs;
    fs.files["a.pti"] = payloadFile(392, 1000, false);
    StorageStreamReader rd(fs);
    StreamBindInfo info;
    for (int i = 0; i < kMaxStreams; ++i) {
        StreamSource s = src(uint8_t(i), 1000);
        CHECK(rd.bind(s, "a.pti", 392, false, &info));
    }
    StreamSource extra = src(40, 1000);
    CHECK(!rd.bind(extra, "a.pti", 392, false, &info));
    CHECK(std::string(info.reason) == "no slot");
    StreamSource missing = src(0, 1000);
    rd.unbind(0);
    CHECK(!rd.bind(missing, "nope.pti", 392, false, &info));
    CHECK(std::string(info.reason) == "open");
}
