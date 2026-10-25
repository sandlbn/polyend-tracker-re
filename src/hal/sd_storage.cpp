// OpenTracker -- FatFs disk glue + IStorage (spec 01 §6). FatFs R0.15a is vendored
// in third_party/fatfs (1-clause BSD-style licence).
#include "sd_storage.h"
#include "diag.h"

#include <Arduino.h>
#include <string.h>
#include "board.h"
#include "sdhc.h"
#include "sdram_bss.h"

extern "C" {
#include "ff.h"
#include "diskio.h"
}

// ---- FatFs disk I/O layer (FatFs "diskio.h" contract) ------------------------
extern "C" {

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    // USB storage mode: the computer owns the card (sdhc::Owner).
    if (ot::hal::sdhc::owner() != ot::hal::sdhc::Owner::kApp) return STA_NOINIT;
    if (!ot::hal::sdhc::cardPresent()) return STA_NOINIT | STA_NODISK;
    return ot::hal::sdhc::ready() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (ot::hal::sdhc::owner() != ot::hal::sdhc::Owner::kApp) return STA_NOINIT;
    if (!ot::hal::sdhc::ready()) ot::hal::sdhc::init();
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!ot::hal::sdhc::ready()) return RES_NOTRDY;
    return ot::hal::sdhc::readBlocks(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!ot::hal::sdhc::ready()) return RES_NOTRDY;
    return ot::hal::sdhc::writeBlocks(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0) return RES_PARERR;
    if (!ot::hal::sdhc::ready()) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;   // writes complete synchronously
    case GET_SECTOR_COUNT: *static_cast<LBA_t*>(buff) = ot::hal::sdhc::sectorCount(); return RES_OK;
    case GET_SECTOR_SIZE: *static_cast<WORD*>(buff) = 512; return RES_OK;
    case GET_BLOCK_SIZE: *static_cast<DWORD*>(buff) = 1; return RES_OK;
    default: return RES_PARERR;
    }
}

}  // extern "C"

namespace ot {
namespace hal {

namespace {
FATFS gFs;
constexpr int kMaxFiles = 14;   // stem export keeps 11 WAVs open (spec 04 §6.1)
#if defined(OT_USB_MSC) && OT_USB_MSC
// 2.0.1 (USB Storage ring, board.h kSdBounceBytes): slot 0 stays in the SRAM
// for the boot code (restore trigger, firmware info: one file at a time,
// before the SDRAM is up); slots 1..13 live in `.sdram_bss` (main-loop file
// I/O only; sdhc.cpp handles SDRAM buffers: direct ADMA2 with cache
// maintenance when verified at boot, else the bounce buffer). They are
// handed out only after otAppMain() cleared `.sdram_bss`
// (SdStorage::enableSdramFiles()).
constexpr int kSramFiles = 1;
FIL gFilesLo[kSramFiles];
OT_SDRAM_BSS FIL gFilesHi[kMaxFiles - kSramFiles];
FIL& fil(int i) { return i < kSramFiles ? gFilesLo[i] : gFilesHi[i - kSramFiles]; }
int gSlots = kSramFiles;
#else
FIL gFiles[kMaxFiles];
FIL& fil(int i) { return gFiles[i]; }
constexpr int gSlots = kMaxFiles;
#endif
bool gUsed[kMaxFiles];
// FF_USE_FASTSEEK cluster link maps for read-only handles: a small static pool
// (kClmtSlots x kClmtWords DWORDs = 1 KB SRAM). A table holds (kClmtWords-1)/2
// fragments; a more fragmented file (FR_NOT_ENOUGH_CORE) or a full pool just
// reads without a map (normal FAT chain walking).
constexpr int kClmtSlots = 4;
constexpr UINT kClmtWords = 64;
DWORD gClmt[kClmtSlots][kClmtWords];
int8_t gClmtOf[kMaxFiles];   // pool slot of a file handle, -1 = none (set on open)
bool gClmtUsed[kClmtSlots];
uint32_t gClmtMaps = 0, gClmtFallbacks = 0;

void attachLinkMap(int file) {
    gClmtOf[file] = -1;
    int t = -1;
    for (int i = 0; i < kClmtSlots; i++) if (!gClmtUsed[i]) { t = i; break; }
    if (t < 0) { gClmtFallbacks++; return; }
    FIL& f = fil(file);
    gClmt[t][0] = kClmtWords;
    f.cltbl = gClmt[t];
    if (f_lseek(&f, CREATE_LINKMAP) != FR_OK) {   // too fragmented (or empty file): no map
        f.cltbl = nullptr;
        f_lseek(&f, 0);
        gClmtFallbacks++;
        return;
    }
    gClmtUsed[t] = true;
    gClmtOf[file] = int8_t(t);
    gClmtMaps++;
}
void detachLinkMap(int file) {
    if (gClmtOf[file] >= 0) gClmtUsed[gClmtOf[file]] = false;
    gClmtOf[file] = -1;
}
char gPath[FF_MAX_LFN + 4];

// IStorage paths are relative to the card root; FatFs wants "/..." (no drive prefix needed).
// nullptr for a path longer than IStorage::kMaxPath: cutting it would name
// another file, or the folder itself (issue #55).
static_assert(sizeof(gPath) - 2 == IStorage::kMaxPath, "kMaxPath is the path buffer");
const char* fix(const char* p) {
    const char* s = (p[0] == '/') ? p + 1 : p;
    size_t n = strlen(s);
    if (n + 2 > sizeof(gPath)) return nullptr;
    gPath[0] = '/';
    memcpy(gPath + 1, s, n);
    gPath[n + 1] = 0;
    return gPath;
}
FIL* fileFor(IStorage::Handle h) {
    if (h < 0 || h >= kMaxFiles || !gUsed[h]) return nullptr;
    return &fil(h);
}
}  // namespace

void SdStorage::enableSdramFiles() {
#if defined(OT_USB_MSC) && OT_USB_MSC
    gSlots = kMaxFiles;
#endif
}

bool SdStorage::mount() {
    if (storageMode_) return false;
    if (mounted_) return true;
    if (!sdhc::cardPresent()) return false;
    diagLog("sd: SDHC init (CMD0/8/ACMD41/2/3/9/7/ACMD6)");
    if (!sdhc::init()) {
        diagLogf("sd: SDHC init failed, last err 0x%08lX", static_cast<unsigned long>(sdhc::lastError()));
        return false;
    }
    const sdhc::Info& in = sdhc::info();
    diagLogf("sd: SDHC ready, %lu sectors, %s, %s timing, SDCLK %lu Hz%s%s; f_mount",
             static_cast<unsigned long>(sdhc::sectorCount()), in.adma ? "ADMA2" : "PIO",
             in.highSpeed ? "high-speed" : "default-speed", static_cast<unsigned long>(in.clockHz),
             sdhc::initNote()[0] ? " -- " : "", sdhc::initNote());
    FRESULT fr = f_mount(&gFs, "", 1);
    diagLogf("sd: f_mount result %d", int(fr));
    mounted_ = fr == FR_OK;
    return mounted_;
}

bool SdStorage::checkDirectDma(uint8_t* scratch) {
    if (!mounted_) return false;
    const bool on = sdhc::checkSdramDirect(scratch, uint32_t(gFs.fatbase));
    diagLogf("sd: ADMA2 direct to SDRAM: read check %s -> reads %s (writes bounce until the first-write check)",
             sdhc::directCheckName(sdhc::directCheck()), sdhc::sdramDirectRead() ? "direct" : "bounced");
    return on;
}

namespace {
bool gWriteCheckDone = false;   // at most one direct-write trial per boot
}

// Lazy direct-write check, before the first write sourced from SDRAM: up to
// 4 KB of the caller's own data go to a FatFs temp file over the direct path,
// are read back in 64-byte pieces (FatFs reads each sector into the FIL's own
// SRAM buffer: the proven path), compared and the file is deleted. Uses a free
// file slot, no new buffers and only ~100 B of stack. Never touches sectors
// outside FatFs.
void SdStorage::maybeCheckDirectWrite(const void* src, uint32_t len) {
    if (gWriteCheckDone || !mounted_) return;
    const uint32_t a = reinterpret_cast<uint32_t>(src);
    const bool inSdram = a >= board::kSdramBase && a - board::kSdramBase < board::kSdramSize;
    if (!inSdram || len < 512 || (a & 3u)) return;   // would bounce anyway; wait for a real candidate
    gWriteCheckDone = true;
    if (!board::kSdDirectWriteCheck || !sdhc::sdramDirectRead()) { sdhc::skipDirectWriteCheck(); return; }
    static const char kTmp[] = "/OpenTracker/dmacheck.tmp";
    int slot = -1;
    for (int i = 0; i < gSlots; i++) if (!gUsed[i]) { slot = i; break; }
    if (slot < 0) { gWriteCheckDone = false; return; }   // all slots busy: try at a later write
    uint32_t n = len & ~511u;
    if (n > 4096) n = 4096;
    if (a - board::kSdramBase + n > board::kSdramSize) n = (board::kSdramBase + board::kSdramSize - a) & ~511u;
    f_mkdir("/OpenTracker");                         // FR_EXIST is fine
    FIL& f = fil(slot);                              // not marked used: never visible to callers
    bool ok = f_open(&f, kTmp, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
    bool used = false;
    if (ok) {
        const uint32_t before = sdhc::counters().direct;
        sdhc::beginDirectWriteTrial();
        UINT bw = 0;
        ok = f_write(&f, src, n, &bw) == FR_OK && bw == n;
        used = sdhc::counters().direct != before;
        sdhc::endDirectWriteTrial(false);            // read back through the proven path first
        ok = (f_close(&f) == FR_OK) && ok;
    }
    if (ok && f_open(&f, kTmp, FA_READ | FA_OPEN_EXISTING) == FR_OK) {
        uint8_t piece[64];                           // < 512 B: FatFs goes through f.buf
        const uint8_t* p = static_cast<const uint8_t*>(src);
        for (uint32_t off = 0; ok && off < n; off += sizeof piece) {
            UINT br = 0;
            ok = f_read(&f, piece, sizeof piece, &br) == FR_OK && br == sizeof piece &&
                 memcmp(piece, p + off, sizeof piece) == 0;
        }
        f_close(&f);
    } else {
        ok = false;
    }
    f_unlink(kTmp);
    sdhc::endDirectWriteTrial(ok && used);
    diagLogf("sd: ADMA2 direct from SDRAM: write check %s (%lu B via %s, temp file) -> writes %s",
             sdhc::directCheckName(sdhc::directWriteCheck()), static_cast<unsigned long>(n),
             used ? "direct path" : "bounce only", sdhc::sdramDirectWrite() ? "direct" : "bounced");
}

void SdStorage::unmount() {
    for (int i = 0; i < kMaxFiles; i++) {
        if (gUsed[i] && mounted_) f_close(&fil(i));
        if (gUsed[i]) detachLinkMap(i);
        gUsed[i] = false;
    }
    if (mounted_) f_mount(nullptr, "", 0);
    mounted_ = false;
    sdhc::release();
}

bool SdStorage::fatInfo(FatInfo* out) const {
    *out = FatInfo{};
    if (!mounted_ || !gFs.fs_type) return false;
    switch (gFs.fs_type) {
    case FS_FAT12: out->type = 12; break;
    case FS_FAT16: out->type = 16; break;
    case FS_FAT32: out->type = 32; break;
    default: out->type = 64; break;
    }
    out->fats = gFs.n_fats;
    out->fsinfo = gFs.fs_type == FS_FAT32 && !(gFs.fsi_flag & 0x80);
    out->clusterBytes = uint32_t(gFs.csize) * 512u;
    out->clusters = gFs.n_fatent - 2;
    out->fatStart = uint32_t(gFs.fatbase);
    out->fatSectors = gFs.fsize;
    out->dataStart = uint32_t(gFs.database);
    if (gFs.free_clst <= gFs.n_fatent - 2) out->freeClusters = gFs.free_clst;
    return true;
}

void SdStorage::setStorageMode(bool on) {
    // spec 01 §6.1: storage mode = release the card. 2.x: usb_storage.cpp then
    // gives it to the computer (sdhc::Owner::kHost); mount() is refused until
    // the mode ends, and every IStorage call fails while unmounted.
    storageMode_ = on;
    if (on) unmount();
}

void SdStorage::service() {
    // spec 01 §6 card detect, 500 ms debounce.
    bool present = sdhc::cardPresent();
    uint32_t now = millis();
    if (present != pendingPresent_) { pendingPresent_ = present; changeMs_ = now; }
    if (present != lastPresent_ && now - changeMs_ >= board::kCardDebounceMs) {
        lastPresent_ = present;
        if (present) mount(); else unmount();
    }
}

IStorage::Handle SdStorage::open(const char* path, OpenMode mode) {
    if (!mounted_) return -1;
    int slot = -1;
    for (int i = 0; i < gSlots; i++) if (!gUsed[i]) { slot = i; break; }
    if (slot < 0) return -1;
    BYTE m = 0;
    switch (mode) {
    case kRead: m = FA_READ | FA_OPEN_EXISTING; break;
    case kWrite: m = FA_WRITE | FA_CREATE_ALWAYS; break;
    case kReadWrite: m = FA_READ | FA_WRITE | FA_OPEN_ALWAYS; break;
    default: return -1;
    }
    const char* fp = fix(path);
    if (!fp || f_open(&fil(slot), fp, m) != FR_OK) return -1;
    gUsed[slot] = true;
    if (mode == kRead) attachLinkMap(slot); else gClmtOf[slot] = -1;
    return slot;
}

int32_t SdStorage::read(Handle h, void* dst, uint32_t len) {
    FIL* f = fileFor(h);
    if (!f) return -1;
    UINT br = 0;
    return f_read(f, dst, len, &br) == FR_OK ? int32_t(br) : -1;
}

int32_t SdStorage::write(Handle h, const void* src, uint32_t len) {
    FIL* f = fileFor(h);
    if (!f) return -1;
    maybeCheckDirectWrite(src, len);
    UINT bw = 0;
    return f_write(f, src, len, &bw) == FR_OK ? int32_t(bw) : -1;
}

bool SdStorage::seek(Handle h, uint32_t pos) {
    FIL* f = fileFor(h);
    return f && f_lseek(f, pos) == FR_OK;
}

int32_t SdStorage::size(Handle h) {
    FIL* f = fileFor(h);
    return f ? int32_t(f_size(f)) : -1;
}

void SdStorage::close(Handle h) {
    FIL* f = fileFor(h);
    if (!f) return;
    f_close(f);
    detachLinkMap(h);
    gUsed[h] = false;
}

bool SdStorage::exists(const char* path) {
    if (!mounted_) return false;
    const char* p = fix(path);
    if (!p) return false;
    if (p[1] == 0) return true;   // root
    FILINFO fi;
    return f_stat(p, &fi) == FR_OK;
}

bool SdStorage::mkdir(const char* path) {
    if (!mounted_) return false;
    const char* p = fix(path);
    if (!p) return false;
    FRESULT r = f_mkdir(p);
    return r == FR_OK || r == FR_EXIST;
}

bool SdStorage::remove(const char* path) {
    if (!mounted_) return false;
    const char* p = fix(path);
    return p && f_unlink(p) == FR_OK;
}

bool SdStorage::rename(const char* from, const char* to) {
    if (!mounted_) return false;
    static char fromBuf[FF_MAX_LFN + 4];
    const char* pf = fix(from);
    if (!pf) return false;
    strncpy(fromBuf, pf, sizeof(fromBuf) - 1);
    fromBuf[sizeof(fromBuf) - 1] = 0;
    const char* pt = fix(to);
    return pt && f_rename(fromBuf, pt) == FR_OK;
}

bool SdStorage::benchmark(uint8_t* buf, uint32_t bufLen, uint32_t totalBytes, Bench& r) {
    r = Bench();
    if (!mounted_ || !buf || bufLen < 512 || totalBytes < bufLen) return false;
    static const char kPath[] = "/OT_BENCH.TMP";
    FIL f;
    maybeCheckDirectWrite(buf, bufLen);   // the selftest bench is the first SDRAM write there
    if (f_open(&f, kPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    const uint32_t chunks = totalBytes / bufLen;
    bool ok = true;
    memset(buf, 0x5A, bufLen);
    uint32_t t0 = micros();
    for (uint32_t i = 0; i < chunks && ok; i++) {
        memcpy(buf, &i, sizeof i);   // chunk tag, checked on read-back
        UINT bw = 0;
        ok = f_write(&f, buf, bufLen, &bw) == FR_OK && bw == bufLen;
    }
    ok = (f_sync(&f) == FR_OK) && ok;
    r.writeUs = micros() - t0;
    f_close(&f);
    if (ok && f_open(&f, kPath, FA_READ | FA_OPEN_EXISTING) == FR_OK) {
        t0 = micros();
        for (uint32_t i = 0; i < chunks && ok; i++) {
            UINT br = 0;
            ok = f_read(&f, buf, bufLen, &br) == FR_OK && br == bufLen;
            uint32_t tag;
            memcpy(&tag, buf, sizeof tag);
            if (ok && (tag != i || buf[bufLen - 1] != 0x5A)) { r.verifyErrors++; }
        }
        r.readUs = micros() - t0;
        f_close(&f);
    } else {
        ok = false;
    }
    f_unlink(kPath);
    r.bytes = chunks * bufLen;
    r.ok = ok && r.verifyErrors == 0;
    return r.ok;
}

namespace {
// Timings of one random-read series: sorted in place for the percentile.
void rndStats(uint32_t* t, uint32_t n, SdStorage::Probe::Rnd& r) {
    for (uint32_t i = 1; i < n; ++i)
        for (uint32_t j = i; j > 0 && t[j] < t[j - 1]; --j) {
            const uint32_t x = t[j];
            t[j] = t[j - 1];
            t[j - 1] = x;
        }
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) sum += t[i];
    r.n = n;
    r.minUs = n ? t[0] : 0;
    r.maxUs = n ? t[n - 1] : 0;
    r.avgUs = n ? uint32_t(sum / n) : 0;
    r.p99Us = n ? t[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1] : 0;
}
uint32_t kbps(uint64_t bytes, uint32_t us) { return us ? uint32_t(bytes * 1000u / 1024u * 1000u / us) : 0; }
}  // namespace

bool SdStorage::streamProbe(uint8_t* buf, uint32_t bufLen, uint32_t fileBytes, Probe& r) {
    r = Probe();
    constexpr uint32_t kChunk = 32768;
    if (!mounted_ || !buf || bufLen < kChunk) {
        r.err = "not mounted / no buffer";
        return false;
    }
    static const char kPath[] = "/OT_PROBE.TMP";
    static const char kPath2[] = "/OT_PROBE2.TMP";
    const uint32_t chunks = fileBytes / kChunk;
    r.fileBytes = chunks * kChunk;
    r.clusterBytes = uint32_t(gFs.csize) * 512u;
    FIL f;
    maybeCheckDirectWrite(buf, kChunk);
    // 1. Write the test file (sequential, 32 KB chunks).
    if (f_open(&f, kPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        r.err = "create failed";
        return false;
    }
    bool ok = true;
    uint32_t t0 = micros();
    for (uint32_t i = 0; i < chunks && ok; ++i) {
        memset(buf, int(i & 0xFF), kChunk);
        const uint32_t c0 = micros();
        UINT bw = 0;
        ok = f_write(&f, buf, kChunk, &bw) == FR_OK && bw == kChunk;
        const uint32_t dt = micros() - c0;
        if (dt > r.writeChunkMaxUs) r.writeChunkMaxUs = dt;
    }
    ok = f_sync(&f) == FR_OK && ok;
    r.writeKBps = kbps(r.fileBytes, micros() - t0);
    f_close(&f);
    if (!ok) {
        f_unlink(kPath);
        r.err = "write failed (card full?)";
        return false;
    }
    // 2. Sequential read + the cluster map (fast seek), as a streamed instrument would.
    DWORD clmt[128];   // on the stack: the probe runs once at boot (no .bss in the app image)
    if (f_open(&f, kPath, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        r.err = "open failed";
        return false;
    }
    clmt[0] = 128;
    f.cltbl = clmt;
    if (f_lseek(&f, CREATE_LINKMAP) == FR_OK) {
        r.fragments = (clmt[0] - 1) / 2;
    } else {
        f.cltbl = nullptr;   // too fragmented for 63 entries: plain FAT walk
        r.fragments = 0xFFFF;
    }
    t0 = micros();
    for (uint32_t i = 0; i < chunks && ok; ++i) {
        const uint32_t c0 = micros();
        UINT br = 0;
        ok = f_read(&f, buf, kChunk, &br) == FR_OK && br == kChunk && buf[0] == uint8_t(i & 0xFF) &&
             buf[kChunk - 1] == uint8_t(i & 0xFF);
        const uint32_t dt = micros() - c0;
        if (dt > r.seqChunkMaxUs) r.seqChunkMaxUs = dt;
    }
    r.seqKBps = kbps(r.fileBytes, micros() - t0);
    // 3. Random reads: 64 each of 32, 16 and 4 KB, size-aligned offsets.
    uint32_t t[64];
    const uint32_t sizes[3] = {32768, 16384, 4096};
    uint32_t rng = 0x1234567u;
    for (int k = 0; k < 3 && ok; ++k) {
        r.rnd[k].size = sizes[k];
        const uint32_t slots = r.fileBytes / sizes[k];
        for (uint32_t i = 0; i < 64 && ok; ++i) {
            rng = rng * 1664525u + 1013904223u;
            const uint32_t off = ((rng >> 8) % slots) * sizes[k];
            const uint32_t c0 = micros();
            UINT br = 0;
            ok = f_lseek(&f, off) == FR_OK && f_read(&f, buf, sizes[k], &br) == FR_OK && br == sizes[k];
            t[i] = micros() - c0;
        }
        rndStats(t, 64, r.rnd[k]);
    }
    // 4. Reads right behind writes: 16 x (32 KB write + f_sync to a second
    //    file, then one random 32 KB read of the first): the autosave case.
    FIL w;
    if (ok && f_open(&w, kPath2, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        uint64_t sum = 0;
        for (uint32_t i = 0; i < 16 && ok; ++i) {
            memset(buf, 0xA5, kChunk);
            uint32_t c0 = micros();
            UINT bw = 0;
            ok = f_write(&w, buf, kChunk, &bw) == FR_OK && bw == kChunk && f_sync(&w) == FR_OK;
            uint32_t dt = micros() - c0;
            if (dt > r.mixWriteMaxUs) r.mixWriteMaxUs = dt;
            rng = rng * 1664525u + 1013904223u;
            const uint32_t off = ((rng >> 8) % chunks) * kChunk;
            c0 = micros();
            UINT br = 0;
            ok = ok && f_lseek(&f, off) == FR_OK && f_read(&f, buf, kChunk, &br) == FR_OK && br == kChunk;
            dt = micros() - c0;
            sum += dt;
            if (dt > r.mixReadMaxUs) r.mixReadMaxUs = dt;
        }
        r.mixReadAvgUs = uint32_t(sum / 16);
        f_close(&w);
        f_unlink(kPath2);
    } else if (ok) {
        ok = false;
        r.err = "second file failed";
    }
    f_close(&f);
    f_unlink(kPath);
    if (!ok && !r.err[0]) r.err = "read failed";
    r.ok = ok;
    return ok;
}

int32_t SdStorage::listDir(const char* path, DirFn fn, void* ctx) {
    if (!mounted_) return -1;
    DIR d;
    const char* p = fix(path);
    if (!p || f_opendir(&d, p) != FR_OK) return -1;
    int32_t n = 0;
    FILINFO fi;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        n++;
        DirEntry e{fi.fname, (fi.fattrib & AM_DIR) != 0, uint32_t(fi.fsize)};
        if (fn && !fn(e, ctx)) break;
    }
    f_closedir(&d);
    return n;
}

}  // namespace hal
}  // namespace ot
