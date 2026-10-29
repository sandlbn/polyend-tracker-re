// OpenTracker -- IStorage on the SD card via FatFs (spec 01 §6).
#pragma once

#include <stdint.h>
#include "hal_api.h"
#include "audio/stream_extents.h"

namespace ot {
namespace hal {

class SdStorage final : public IStorage {
public:
    // Card-detect watcher (spec 01 §6: 500 ms debounce, insert -> init+mount,
    // removal -> unmount). Call from the main loop.
    void service();
    bool mount();
    // otAppMain(), right after it cleared `.sdram_bss`: file slots that live
    // in the SDRAM may be used from now on (before: one file at a time).
    void enableSdramFiles();
    void unmount();
    bool mounted() const { return mounted_; }
    // USB storage mode (spec 01 §6.1): release the card, stop all SDHC access.
    void setStorageMode(bool on);
    // Layout of the mounted FAT volume (USB Storage diagnostics). Sectors are
    // card blocks. freeClusters: FatFs' count (from FSINFO at mount, kept up
    // to date by FatFs and written back to FSINFO on every f_close / f_sync),
    // 0xFFFFFFFF when unknown. fsinfo: the volume has a valid FSINFO sector.
    struct FatInfo {
        uint8_t type = 0;   // 0 none, 12 / 16 / 32, 64 = exFAT
        uint8_t fats = 0;
        bool fsinfo = false;
        uint32_t clusterBytes = 0, clusters = 0, fatStart = 0, fatSectors = 0, dataStart = 0;
        uint32_t freeClusters = 0xFFFFFFFFu;
    };
    bool fatInfo(FatInfo* out) const;

    // Throughput self-measurement (selftest): writes `totalBytes` to a temp file in
    // `bufLen` chunks from `buf`, reads it back (checking chunk tags), deletes it.
    struct Bench { bool ok = false; uint32_t bytes = 0, writeUs = 0, readUs = 0, verifyErrors = 0; };
    bool benchmark(uint8_t* buf, uint32_t bufLen, uint32_t totalBytes, Bench& r);
    // SD streaming feasibility probe (docs/PLAN_SD_STREAMING.md P0): a temp file
    // of fileBytes written in 32 KB chunks, read back sequentially, then random
    // reads of 32 / 16 / 4 KB through the FatFs fast-seek cluster map, then
    // reads right behind 32 KB writes + f_sync (the autosave case). buf: at
    // least 32 KB in SDRAM (the ADMA2 path the streamer would use).
    struct Probe {
        struct Rnd { uint32_t size = 0, n = 0, minUs = 0, avgUs = 0, p99Us = 0, maxUs = 0; };
        bool ok = false;
        const char* err = "";
        uint32_t fileBytes = 0, clusterBytes = 0, fragments = 0;
        uint32_t writeKBps = 0, writeChunkMaxUs = 0;
        uint32_t seqKBps = 0, seqChunkMaxUs = 0;
        Rnd rnd[3];
        uint32_t mixWriteMaxUs = 0, mixReadMaxUs = 0, mixReadAvgUs = 0;
    };
    bool streamProbe(uint8_t* buf, uint32_t bufLen, uint32_t fileBytes, Probe& r);
    // Boot: sdhc::checkSdramDirect() on the first FAT sectors of the mounted
    // volume (real data). `scratch`: >= 65 KB of free SDRAM. True = direct reads on.
    bool checkDirectDma(uint8_t* scratch);

    // SD streaming (docs/PLAN_SD_STREAMING_P1C_P2.md §1.2). The cluster map of
    // `path` as sector extents (FatFs fast seek on a private FIL, not a slot);
    // false when not mounted, the open fails or more than cap extents are
    // needed (too fragmented).
    bool fileExtents(const char* path, audio::StreamExtent* out, int cap, int* count, uint32_t* fileSize);
    // Raw sectors (main loop only); false when unmounted, in USB Storage mode
    // or on a transfer error.
    bool readSectors(uint32_t lba, uint8_t* dst, uint32_t count);
    // 512 bytes of `path` at byte offset `off` through FatFs (bind self-check).
    bool readBytesAt(const char* path, uint32_t off, uint8_t* dst512);

    // SD streaming C14: f_expand (opt 0: find a contiguous run and point the
    // allocation at it; the file stays empty) / f_getfree.
    bool reserve(Handle h, uint32_t bytes) override;
    bool truncate(Handle h) override;
    uint64_t freeBytes() override;

    Handle open(const char* path, OpenMode mode) override;
    int32_t read(Handle h, void* dst, uint32_t len) override;
    int32_t write(Handle h, const void* src, uint32_t len) override;
    bool seek(Handle h, uint32_t pos) override;
    int32_t size(Handle h) override;
    void close(Handle h) override;
    bool exists(const char* path) override;
    bool mkdir(const char* path) override;
    bool remove(const char* path) override;
    bool rename(const char* from, const char* to) override;
    int32_t listDir(const char* path, DirFn fn, void* ctx) override;

private:
    // Once per boot, before the first SDRAM-sourced write (sd_storage.cpp).
    void maybeCheckDirectWrite(const void* src, uint32_t len);
    bool mounted_ = false;
    bool storageMode_ = false;
    bool lastPresent_ = false;
    uint32_t changeMs_ = 0;
    bool pendingPresent_ = false;
};

}  // namespace hal
}  // namespace ot
