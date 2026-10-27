// SD streaming: pure extent arithmetic (stream_extents.h).
#include "audio/stream_extents.h"

namespace ot {
namespace audio {

int clmtToExtents(const uint32_t* clmt, uint32_t dataStartLba, uint32_t sectorsPerCluster, StreamExtent* out, int cap) {
    if (!clmt || !out || cap <= 0 || sectorsPerCluster == 0) return -1;
    int n = 0;
    for (const uint32_t* p = clmt + 1; p[0] != 0; p += 2) {
        const uint32_t clusters = p[0], start = p[1];
        if (start < 2) return -1;
        const uint32_t lba = dataStartLba + (start - 2) * sectorsPerCluster, sectors = clusters * sectorsPerCluster;
        if (n > 0 && out[n - 1].lba + out[n - 1].sectors == lba) {
            out[n - 1].sectors += sectors;   // physically adjacent: one run
            continue;
        }
        if (n >= cap) return -1;
        out[n].lba = lba;
        out[n].sectors = sectors;
        ++n;
    }
    return n;
}

bool extentsLocate(const StreamExtent* e, int n, uint32_t byteOff, int* idx, uint32_t* lba, uint32_t* inSector) {
    uint32_t sector = byteOff / kSectorBytes;
    for (int i = 0; i < n; ++i) {
        if (sector < e[i].sectors) {
            *idx = i;
            *lba = e[i].lba + sector;
            *inSector = byteOff % kSectorBytes;
            return true;
        }
        sector -= e[i].sectors;
    }
    return false;
}

int planSectorRead(const StreamExtent* e, int n, uint32_t byteOff, uint32_t bytes, SectorOp* ops, int cap) {
    int k = 0, idx;
    uint32_t lba, in, dst = 0;
    if (!extentsLocate(e, n, byteOff, &idx, &lba, &in)) return -1;
    uint32_t rem = bytes;
    // Partial head sector.
    if (in != 0 || rem < kSectorBytes) {
        const uint32_t take = kSectorBytes - in < rem ? kSectorBytes - in : rem;
        if (k >= cap) return -1;
        ops[k++] = {lba, 1, in, dst, take, false};
        dst += take;
        rem -= take;
        byteOff += take;
        if (rem && !extentsLocate(e, n, byteOff, &idx, &lba, &in)) return -1;
    }
    // Whole sectors, one op per extent run.
    while (rem >= kSectorBytes) {
        const uint32_t left = e[idx].lba + e[idx].sectors - lba;   // sectors left in this extent
        uint32_t want = rem / kSectorBytes;
        if (want > left) want = left;
        if (k >= cap) return -1;
        ops[k++] = {lba, want, 0, dst, want * kSectorBytes, true};
        dst += want * kSectorBytes;
        rem -= want * kSectorBytes;
        byteOff += want * kSectorBytes;
        if (rem && !extentsLocate(e, n, byteOff, &idx, &lba, &in)) return -1;
    }
    // Partial tail sector.
    if (rem) {
        if (k >= cap) return -1;
        ops[k++] = {lba, 1, 0, dst, rem, false};
    }
    return k;
}

}  // namespace audio
}  // namespace ot
