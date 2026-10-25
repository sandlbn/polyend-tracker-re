// FatFs cluster allocation (third_party/fatfs, the OpenTracker scan_clst
// change in create_chain): a file rewritten in its old cluster hole and grown
// past it must not scan the FAT from the hole across every used cluster up to
// free space on each rewrite (device: 2,660 FAT sectors, 0.56 s in one autosave
// unit, a kick grown by Timestretch with 10 GB of samples after the Workspace).
// Also: FSInfo's next free cluster seeds the scan at mount, and random
// rewrites / grows / shrinks / deletes up to a full card keep every file
// intact, the free count right and refuse a write only when the card is full,
// whatever free count FSInfo claims.
// FatFs runs on a FAT32 volume in memory (formatted here: FF_USE_MKFS is 0).
#include "test.h"

#include <cstdlib>
#include <vector>

extern "C" {
#include "ff.h"
#include "diskio.h"
}

namespace {

constexpr uint32_t kSector = 512;
constexpr uint32_t kClusters = 70000;   // > 65525: FAT32, 1 sector per cluster
constexpr uint32_t kReserved = 32;
constexpr uint32_t kFatSectors = (kClusters + 2) * 4 / kSector + 1;
constexpr uint32_t kTotal = kReserved + 2 * kFatSectors + kClusters;

std::vector<uint8_t> gDisk;
uint32_t gFatReads = 0;   // sectors read from the FATs

void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, uint16_t(v)); put16(p + 2, uint16_t(v >> 16)); }

void format() {
    gDisk.assign(size_t(kTotal) * kSector, 0);
    uint8_t* b = gDisk.data();
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    std::memcpy(b + 3, "MSWIN4.1", 8);
    put16(b + 11, kSector);
    b[13] = 1;                         // sectors per cluster
    put16(b + 14, kReserved);
    b[16] = 2;                         // FATs
    b[21] = 0xF8;
    put32(b + 32, kTotal);
    put32(b + 36, kFatSectors);
    put32(b + 44, 2);                  // root directory cluster
    put16(b + 48, 1);                  // FSInfo sector
    put16(b + 50, 6);                  // backup boot sector
    b[66] = 0x29;
    std::memcpy(b + 71, "NO NAME    ", 11);
    std::memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    uint8_t* fi = b + kSector;
    put32(fi, 0x41615252);
    put32(fi + 484, 0x61417272);
    put32(fi + 488, kClusters - 1);    // free count (the root takes one)
    put32(fi + 492, 0xFFFFFFFF);       // next free: unknown
    put32(fi + 508, 0xAA550000);
    for (int f = 0; f < 2; ++f) {
        uint8_t* fat = b + size_t(kReserved + f * kFatSectors) * kSector;
        put32(fat, 0x0FFFFFF8);
        put32(fat + 4, 0x0FFFFFFF);
        put32(fat + 8, 0x0FFFFFFF);    // root: one cluster
    }
}

FATFS gFs;
uint8_t gBuf[8192];

void pattern(uint32_t seed, uint32_t off, uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) p[i] = uint8_t(((off + i) * 2654435761u >> 13) ^ seed);
}

// Writes `bytes` of the seed's pattern; 1 = ok, 0 = card full (file removed), -1 = open failed.
int writeFile(const char* path, uint32_t bytes, uint32_t seed = 0) {
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;
    for (uint32_t o = 0; o < bytes; o += sizeof(gBuf)) {
        const uint32_t k = bytes - o < sizeof(gBuf) ? bytes - o : uint32_t(sizeof(gBuf));
        pattern(seed, o, gBuf, k);
        UINT bw = 0;
        if (f_write(&f, gBuf, k, &bw) != FR_OK || bw != k) {
            f_close(&f);
            f_unlink(path);
            return 0;
        }
    }
    return f_close(&f) == FR_OK ? 1 : -1;
}

bool fileIs(const char* path, uint32_t bytes, uint32_t seed) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    bool ok = f_size(&f) == bytes;
    uint8_t want[sizeof(gBuf)];
    for (uint32_t o = 0; ok && o < bytes; o += sizeof(gBuf)) {
        const uint32_t k = bytes - o < sizeof(gBuf) ? bytes - o : uint32_t(sizeof(gBuf));
        UINT br = 0;
        pattern(seed, o, want, k);
        ok = f_read(&f, gBuf, k, &br) == FR_OK && br == k && std::memcmp(gBuf, want, k) == 0;
    }
    f_close(&f);
    return ok;
}

uint32_t fatReadsFor(const char* path, uint32_t bytes) {
    gFatReads = 0;
    CHECK_EQ(writeFile(path, bytes), 1);
    return gFatReads;
}

bool mount() { return f_mount(&gFs, "", 1) == FR_OK; }
void remount() {
    f_unmount("");
    CHECK(mount());
}

DWORD freeClusters() {
    DWORD n = 0;
    FATFS* fs = nullptr;
    CHECK(f_getfree("", &n, &fs) == FR_OK);
    return n;
}

constexpr uint32_t kCl = kSector;   // cluster bytes

// The Workspace shape of the device card: an instrument, a big sample library
// written after it (60,000 clusters, ~470 FAT sectors to scan), the project.
void workspaceBeforeLibrary() {
    format();
    CHECK(mount());
    CHECK_EQ(writeFile("ins1.pti", 2 * kCl), 1);
    CHECK_EQ(writeFile("library.wav", 60000 * kCl), 1);
    CHECK_EQ(writeFile("project.mt", 4 * kCl), 1);
}

}  // namespace

extern "C" {
DSTATUS disk_status(BYTE) { return 0; }
DSTATUS disk_initialize(BYTE) { return 0; }
DRESULT disk_read(BYTE, BYTE* buf, LBA_t sector, UINT count) {
    if ((sector + count) * kSector > gDisk.size()) return RES_PARERR;
    std::memcpy(buf, gDisk.data() + size_t(sector) * kSector, size_t(count) * kSector);
    for (UINT i = 0; i < count; ++i)
        if (sector + i >= kReserved && sector + i < kReserved + 2 * kFatSectors) ++gFatReads;
    return RES_OK;
}
DRESULT disk_write(BYTE, const BYTE* buf, LBA_t sector, UINT count) {
    if ((sector + count) * kSector > gDisk.size()) return RES_PARERR;
    std::memcpy(gDisk.data() + size_t(sector) * kSector, buf, size_t(count) * kSector);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE, BYTE cmd, void*) { return cmd == CTRL_SYNC ? RES_OK : RES_PARERR; }
}

TEST(fatfs_same_size_rewrite_stays_in_its_hole) {
    workspaceBeforeLibrary();
    CHECK(fatReadsFor("ins1.pti", 2 * kCl) < 8);
}

TEST(fatfs_grown_rewrite_scans_once_not_on_every_save) {
    workspaceBeforeLibrary();
    // The first growth walks the library (FSInfo had no next free cluster).
    const uint32_t first = fatReadsFor("ins1.pti", 8 * kCl);
    CHECK(first > 400);
    // Every later save of the grown file (bigger again, or the same size: its
    // hole is still 2 clusters) used to repeat that walk.
    CHECK(fatReadsFor("ins1.pti", 9 * kCl) < 8);
    CHECK(fatReadsFor("project.mt", 4 * kCl) < 8);
    CHECK(fatReadsFor("ins1.pti", 9 * kCl) < 8);
    CHECK(fileIs("ins1.pti", 9 * kCl, 0));
    CHECK(fileIs("library.wav", 60000 * kCl, 0));
}

TEST(fatfs_fsinfo_next_free_seeds_the_scan) {
    workspaceBeforeLibrary();
    // Remounted (a boot): FSInfo's next free cluster, written at close, is past
    // the library, so even the first growth after it skips the walk.
    remount();
    CHECK(fatReadsFor("ins1.pti", 8 * kCl) < 8);
    CHECK(fileIs("ins1.pti", 8 * kCl, 0));
}

// R0.15a looked for FSInfo's trailing signature at offset 498 instead of 508
// (fixed in R0.15b, backported): it never loaded FSInfo, so the free count
// took a full FAT scan, the next free cluster was unknown at every mount, and
// FSInfo was never updated (a computer saw the count of its own last write).
TEST(fatfs_fsinfo_at_the_spec_offsets) {
    workspaceBeforeLibrary();
    const uint8_t* fi = gDisk.data() + kSector;
    CHECK_EQ(fi[508], 0x00);
    CHECK_EQ(fi[509], 0x00);
    CHECK_EQ(fi[510], 0x55);
    CHECK_EQ(fi[511], 0xAA);
    // The free count is not trusted at mount (FF_FS_NOFSINFO 1): a stale one
    // could refuse writes. f_getfree walks the FAT once instead.
    const DWORD expect = freeClusters();
    remount();
    gFatReads = 0;
    CHECK_EQ(freeClusters(), expect);
    CHECK(gFatReads > 400);
}

// A stale FSInfo free count of 0 (files deleted on a computer, the card pulled
// without eject) on a card with 60,000 free clusters: trusted, FatFs refused
// every write for good and wrote the 0 back. Now the card's space is used.
TEST(fatfs_stale_zero_free_count_does_not_refuse_writes) {
    workspaceBeforeLibrary();
    CHECK(f_unlink("library.wav") == FR_OK);
    const DWORD real = freeClusters();
    f_unmount("");
    put32(gDisk.data() + kSector + 488, 0);
    CHECK(mount());
    CHECK_EQ(writeFile("song.pti", 8 * kCl, 7), 1);
    CHECK(fileIs("song.pti", 8 * kCl, 7));
    CHECK_EQ(freeClusters(), real - 8);
    remount();
    CHECK_EQ(freeClusters(), real - 8);
}

TEST(fatfs_free_clusters_below_the_scan_are_still_used) {
    workspaceBeforeLibrary();
    CHECK(fatReadsFor("ins1.pti", 8 * kCl) > 400);   // scan now past the library
    // Free a block before it, then fill the card: the wrap-around finds it.
    CHECK(f_unlink("library.wav") == FR_OK);
    const DWORD freeNow = freeClusters();
    CHECK_EQ(writeFile("fill.bin", (freeNow - 4) * kCl), 1);
    CHECK(fileIs("fill.bin", (freeNow - 4) * kCl, 0));
    CHECK(freeClusters() <= 4);
    CHECK_EQ(writeFile("over.bin", 8 * kCl), 0);     // full: refused, not corrupt
    CHECK(fileIs("ins1.pti", 8 * kCl, 0));
}

TEST(fatfs_random_rewrites_up_to_a_full_card) {
    for (unsigned seed = 1; seed <= 3; ++seed) {
        std::srand(seed);
        format();
        CHECK(mount());
        constexpr int kFiles = 24;
        uint32_t size[kFiles] = {}, data[kFiles] = {};
        bool present[kFiles] = {};
        int fulls = 0;
        for (int it = 0; it < 1500; ++it) {
            const int i = std::rand() % kFiles;
            char path[24];
            std::snprintf(path, sizeof(path), "f%02d.pti", i);
            const int r = std::rand() % 10;
            if (present[i] && r == 0) {
                CHECK(f_unlink(path) == FR_OK);
                present[i] = false;
                continue;
            }
            uint32_t bytes;
            if (present[i] && r < 5) bytes = size[i] + uint32_t(std::rand() % 600) * kCl;   // grow
            else if (present[i] && r < 7) bytes = size[i];                                // same size
            else bytes = uint32_t(1 + std::rand() % 7000) * kCl - uint32_t(std::rand()) % kCl;
            const uint32_t s = uint32_t(std::rand());
            const int ok = writeFile(path, bytes, s);
            CHECK(ok >= 0);
            if (ok == 1) {
                present[i] = true;
                size[i] = bytes;
                data[i] = s;
            } else {
                present[i] = false;
                ++fulls;
                // Refused only when the card really can't hold it.
                CHECK(uint64_t(freeClusters()) * kCl < uint64_t(bytes) + kCl);
            }
            if (it % 250 == 249) remount();   // FSInfo seeding on the way
        }
        CHECK(fulls > 20);   // the card did run full
        for (int i = 0; i < kFiles; ++i)
            if (present[i]) {
                char path[24];
                std::snprintf(path, sizeof(path), "f%02d.pti", i);
                CHECK(fileIs(path, size[i], data[i]));
            }
        // FatFs' running free count matches a walk of the FAT.
        const DWORD counted = freeClusters();
        gFs.free_clst = 0xFFFFFFFF;
        CHECK_EQ(freeClusters(), counted);
        f_unmount("");
    }
}
