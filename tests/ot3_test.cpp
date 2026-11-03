// OT3 container codec (formats/ot3.*, docs/specs/3x-01-ot3-format.md): header,
// kind, size, CRC and chunk checks; chunk iteration incl. unknown and module
// chunks; padding; writer overflow.
#include "test.h"

#include <cstring>
#include <vector>

#include "formats/crc32.h"
#include "formats/ot3.h"

using namespace ot;

namespace {
const uint8_t kW[4] = {3, 0, 0, 0};

std::vector<uint8_t> sample() {
    std::vector<uint8_t> b(1024);
    ot3::Writer w;
    w.begin(b.data(), uint32_t(b.size()), ot3::kKindProjectSidecar, kW);
    const uint8_t info[4] = {3, 0, 0, 0};
    CHECK(w.chunk(ot3::kTagInfo, ot3::kFlagCritical, info, 4));
    ot3::SlotEngine se{};
    se.slot = 1;
    se.miniFallback = 0;
    se.engineId = 0xF0;
    std::memcpy(se.moduleId, "TENG", 4);
    se.moduleVersion = 100;
    CHECK(w.chunk(ot3::kTagSlotEngine, 0, &se, sizeof se));
    ot3::ModuleChunkHeader mh{};
    std::memcpy(mh.id, "TAPE", 4);
    mh.version = 1;
    const uint8_t pay[5] = {1, 2, 3, 4, 5};   // odd length: padded
    CHECK(w.chunk2(0x4001, 0, &mh, sizeof mh, pay, sizeof pay));
    const uint32_t n = w.end();
    CHECK(n > 0 && (n & 3u) == 0);
    b.resize(n);
    return b;
}
}  // namespace

TEST(ot3_round_trip) {
    const std::vector<uint8_t> b = sample();
    ot3::Reader r;
    CHECK(r.open(b.data(), uint32_t(b.size()), ot3::kKindProjectSidecar) == ot3::Error::kOk);
    CHECK_EQ(int(r.version()), int(ot3::kVersion));
    CHECK_EQ(int(r.writer()[0]), 3);
    ot3::Chunk c;
    CHECK(r.next(c) && c.tag == ot3::kTagInfo && c.len == 4 && (c.flags & ot3::kFlagCritical));
    CHECK(r.next(c) && c.tag == ot3::kTagSlotEngine && c.len == sizeof(ot3::SlotEngine));
    ot3::SlotEngine se;
    std::memcpy(&se, c.payload, sizeof se);
    CHECK_EQ(int(se.slot), 1);
    CHECK_EQ(se.engineId, int32_t(0xF0));
    CHECK(std::memcmp(se.moduleId, "TENG", 4) == 0);
    CHECK(r.next(c) && c.tag == 0x4001 && c.len == 13);
    CHECK(std::memcmp(c.payload, "TAPE", 4) == 0);
    CHECK_EQ(int(c.payload[8 + 4]), 5);
    CHECK(!r.next(c));
    CHECK(r.error() == ot3::Error::kOk);
    // The trailer is the zlib CRC of everything before it.
    uint32_t crc = 0;
    std::memcpy(&crc, b.data() + b.size() - 4, 4);
    CHECK_EQ(crc, crc32Software(b.data(), b.size() - 4));
}

TEST(ot3_refuses_damaged_files) {
    const std::vector<uint8_t> good = sample();
    ot3::Reader r;
    CHECK(r.open(good.data(), 10, ot3::kKindProjectSidecar) == ot3::Error::kTooSmall);
    CHECK(r.open(good.data(), uint32_t(good.size()), 2) == ot3::Error::kKind);
    std::vector<uint8_t> b = good;
    b[0] = 'X';
    CHECK(r.open(b.data(), uint32_t(b.size()), ot3::kKindProjectSidecar) == ot3::Error::kMagic);
    b = good;
    b[20] ^= 0x40;   // a payload bit
    CHECK(r.open(b.data(), uint32_t(b.size()), ot3::kKindProjectSidecar) == ot3::Error::kCrc);
    b = good;
    b.resize(b.size() - 4);   // truncated: the size field no longer matches
    CHECK(r.open(b.data(), uint32_t(b.size()), ot3::kKindProjectSidecar) == ot3::Error::kSize);
    std::vector<uint8_t> big(ot3::kMaxFile + 4, 0);
    CHECK(r.open(big.data(), uint32_t(big.size()), ot3::kKindProjectSidecar) == ot3::Error::kTooLarge);
}

TEST(ot3_chunk_running_past_the_end_stops) {
    std::vector<uint8_t> b = sample();
    // Inflate the first chunk's length and fix the CRC: the reader must stop.
    const uint32_t big = 0x10000;
    std::memcpy(b.data() + 16 + 4, &big, 4);
    const uint32_t crc = crc32Software(b.data(), b.size() - 4);
    std::memcpy(b.data() + b.size() - 4, &crc, 4);
    ot3::Reader r;
    CHECK(r.open(b.data(), uint32_t(b.size()), ot3::kKindProjectSidecar) == ot3::Error::kOk);
    ot3::Chunk c;
    CHECK(!r.next(c));
    CHECK(r.error() == ot3::Error::kChunk);
}

TEST(ot3_newer_version_is_flagged) {
    std::vector<uint8_t> b = sample();
    b[4] = 2;   // version 2
    const uint32_t crc = crc32Software(b.data(), b.size() - 4);
    std::memcpy(b.data() + b.size() - 4, &crc, 4);
    ot3::Reader r;
    CHECK(r.open(b.data(), uint32_t(b.size()), ot3::kKindProjectSidecar) == ot3::Error::kNewer);
    ot3::Chunk c;
    CHECK(r.next(c) && c.tag == ot3::kTagInfo);   // still readable
}

TEST(ot3_writer_overflow_returns_zero) {
    std::vector<uint8_t> b(64);
    ot3::Writer w;
    w.begin(b.data(), uint32_t(b.size()), ot3::kKindProjectSidecar, kW);
    const uint8_t big[64] = {};
    CHECK(!w.chunk(0x4000, 0, big, sizeof big));
    CHECK_EQ(w.end(), 0u);
    // An empty file (header + CRC) is valid.
    w.begin(b.data(), uint32_t(b.size()), ot3::kKindProjectSidecar, kW);
    const uint32_t n = w.end();
    CHECK_EQ(n, 20u);
    ot3::Reader r;
    CHECK(r.open(b.data(), n, ot3::kKindProjectSidecar) == ot3::Error::kOk);
    ot3::Chunk c;
    CHECK(!r.next(c) && r.error() == ot3::Error::kOk);
}
