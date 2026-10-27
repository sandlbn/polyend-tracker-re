// SD streaming (docs/PLAN_SD_STREAMING_P1C_P2.md §1.1): pure extent
// arithmetic for the device stream reader. A streamed .pti is read as raw
// card sectors through its cluster map, captured once at load with FatFs
// fast seek (CLMT); no FatFs state on the stream path. Host-tested
// (tests/stream_extents_test.cpp); no FatFs include.
#pragma once

#include <cstdint>

namespace ot {
namespace audio {

struct StreamExtent {
    uint32_t lba;       // first sector
    uint32_t sectors;   // contiguous run, file order
};
constexpr int kStreamMaxExtents = 32;
constexpr uint32_t kSectorBytes = 512;

// FatFs CLMT (clmt[0] = table size, then (clusters, start cluster) pairs,
// 0-terminated) -> extents, adjacent runs merged. Cluster c lies at
// dataStartLba + (c - 2) * sectorsPerCluster. Returns the count, -1 when more
// than cap would be needed.
int clmtToExtents(const uint32_t* clmt, uint32_t dataStartLba, uint32_t sectorsPerCluster, StreamExtent* out, int cap);

// Byte offset in the file -> extent index, sector and byte inside that sector.
// False past the end.
bool extentsLocate(const StreamExtent* e, int n, uint32_t byteOff, int* idx, uint32_t* lba, uint32_t* inSector);

// One read of [byteOff, byteOff + bytes): a partial head sector, whole-sector
// runs (one per extent crossed) and a partial tail sector. `dstOff` is the
// op's offset in the destination; whole ops have srcSkip 0. Returns the op
// count, -1 past the end or when cap is too small.
struct SectorOp {
    uint32_t lba, sectors, srcSkip, dstOff, bytes;
    bool whole;
};
int planSectorRead(const StreamExtent* e, int n, uint32_t byteOff, uint32_t bytes, SectorOp* ops, int cap);

}  // namespace audio
}  // namespace ot
