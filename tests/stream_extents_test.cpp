// SD streaming C2 (docs/PLAN_SD_STREAMING_P1C_P2.md §1.2): pure extent
// arithmetic for the device stream reader. A file's FatFs cluster map (CLMT)
// becomes contiguous sector runs; a byte range of one channel becomes a plan
// of partial-sector and whole-sector card reads. Checked against a byte-level
// model of a fragmented file on a fake disk.
#include "test.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "audio/stream_extents.h"

using namespace ot::audio;

TEST(clmt_to_extents_merges_and_caps) {
    // CLMT: size, then (clusters, start cluster) pairs, 0-terminated. Data area
    // at LBA 1000, 8 sectors per cluster; clusters 2.. map to 1000 + (c-2)*8.
    const uint32_t clmt[] = {9, 3, 10, 2, 13, 4, 40, 0};   // 10..12 + 13..14 adjacent, then 40..43
    StreamExtent e[4];
    const int n = clmtToExtents(clmt, 1000, 8, e, 4);
    CHECK_EQ(n, 2);
    CHECK_EQ(e[0].lba, 1000u + 8u * 8u);   // cluster 10
    CHECK_EQ(e[0].sectors, 5u * 8u);       // 10..14 merged
    CHECK_EQ(e[1].lba, 1000u + 38u * 8u);
    CHECK_EQ(e[1].sectors, 4u * 8u);
    StreamExtent one[1];
    CHECK_EQ(clmtToExtents(clmt, 1000, 8, one, 1), -1);   // needs 2
}

TEST(locate_at_every_boundary) {
    const StreamExtent e[3] = {{100, 4}, {500, 2}, {900, 3}};
    int idx;
    uint32_t lba, in;
    CHECK(extentsLocate(e, 3, 0, &idx, &lba, &in) && idx == 0 && lba == 100 && in == 0);
    CHECK(extentsLocate(e, 3, 4 * 512 - 1, &idx, &lba, &in) && idx == 0 && lba == 103 && in == 511);
    CHECK(extentsLocate(e, 3, 4 * 512, &idx, &lba, &in) && idx == 1 && lba == 500 && in == 0);
    CHECK(extentsLocate(e, 3, 6 * 512 + 7, &idx, &lba, &in) && idx == 2 && lba == 900 && in == 7);
    CHECK(!extentsLocate(e, 3, 9 * 512, &idx, &lba, &in));
}

TEST(plan_sector_read_matches_reference) {
    // A 12-extent file of 200 sectors scattered over a 4,000-sector disk.
    std::mt19937 rng(7);
    std::vector<StreamExtent> ext;
    uint32_t lba = 50, total = 0;
    while (total < 200) {
        const uint32_t len = 1 + rng() % 30, take = len < 200 - total ? len : 200 - total;
        ext.push_back({lba, take});
        total += take;
        lba += take + 3 + rng() % 40;   // a gap: the next run is elsewhere
    }
    std::vector<uint8_t> disk(size_t(lba + 10) * 512);
    for (size_t i = 0; i < disk.size(); ++i) disk[i] = uint8_t(rng());
    std::vector<uint8_t> file;
    for (const StreamExtent& e : ext) file.insert(file.end(), disk.begin() + e.lba * 512, disk.begin() + (e.lba + e.sectors) * 512);
    SectorOp ops[64];
    for (int c = 0; c < 10000; ++c) {
        const uint32_t off = rng() % uint32_t(file.size());
        const uint32_t maxLen = uint32_t(file.size()) - off;
        uint32_t len = (c % 4 == 0) ? 32768 : 1 + rng() % 40000;
        if (c % 97 == 0) len = 512 - off % 512;   // ends on a sector boundary
        if (len > maxLen) len = maxLen;
        const int n = planSectorRead(ext.data(), int(ext.size()), off, len, ops, 64);
        CHECK(n > 0);
        if (n <= 0) continue;
        std::vector<uint8_t> out(len, 0xEE);
        uint32_t covered = 0;
        for (int k = 0; k < n; ++k) {
            const SectorOp& o = ops[k];
            if (o.whole) CHECK(o.srcSkip == 0 && o.bytes == o.sectors * 512);
            else CHECK(o.sectors == 1 && o.srcSkip + o.bytes <= 512);
            std::memcpy(out.data() + o.dstOff, disk.data() + size_t(o.lba) * 512 + o.srcSkip, o.bytes);
            covered += o.bytes;
        }
        CHECK_EQ(covered, len);
        CHECK(std::memcmp(out.data(), file.data() + off, len) == 0);
    }
}
