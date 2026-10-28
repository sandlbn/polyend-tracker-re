// Workspace / project management (spec 02 §1, §1.1, §7, §8.2; spec 05 §5.5
// and appendix E; spec 10 §1.5, §3.3, §4 legacy projects) and instrument
// import/delete (spec 06 §5.4).
#include <cstdio>
#include <cstring>
#include <new>

#include "app/app.h"
#include "app/project_fields.h"
#include "app/project_io.h"
#include "app/stream_policy.h"
#include "audio/synth_params.h"
#include "formats/bytes.h"
#include "formats/crc32.h"
#include "formats/file_io.h"
#include "formats/pattern_mtp.h"
#include "formats/patterns_metadata.h"
#include "formats/instrument_pti.h"
#include "formats/legacy_upgrade.h"
#include "formats/patch_pb.h"
#include "formats/project_mt.h"
#include "formats/wav.h"
#include "ui/gfx.h"
#include "sdram_bss.h"

namespace ot {

namespace {
// Slots whose own N.patch was refused at load (damaged or too large): the
// slot plays the fallback, the Workspace keeps the original file, and Save
// never overwrites the project's file while the slot still holds that same
// fallback (CRC of its encoding at load; any later change of the slot's
// patch is the user's and is saved normally).
uint8_t g_patchKept = 0;
uint32_t g_patchKeptCrc[kSynthSlotCount];

struct FirstEntry {
    char name[256];   // a FAT long name (255) in full: a cut name is another file (#55)
    bool isDir;
    bool found;
};
bool firstEntryFn(const IStorage::DirEntry& e, void* ctx) {
    auto* f = static_cast<FirstEntry*>(ctx);
    if (!std::strcmp(e.name, ".") || !std::strcmp(e.name, "..")) return true;
    std::snprintf(f->name, sizeof(f->name), "%s", e.name);
    f->isDir = e.isDir;
    f->found = true;
    return false;
}

bool endsWithNoCase(const char* s, const char* suf) {
    size_t a = std::strlen(s), b = std::strlen(suf);
    if (b > a) return false;
    for (size_t i = 0; i < b; ++i) {
        char x = s[a - b + i], y = suf[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

const char* baseName(const char* path) {
    const char* s = std::strrchr(path, '/');
    return s ? s + 1 : path;
}

struct NameList {
    char (*names)[64];
    int cap;
    int count;
    bool dirs;   // collect directories instead of files
};
bool collectFn(const IStorage::DirEntry& e, void* ctx) {
    auto* l = static_cast<NameList*>(ctx);
    if (e.isDir != l->dirs || e.name[0] == '.') return true;
    if (l->count >= l->cap) return false;
    std::snprintf(l->names[l->count++], 64, "%s", e.name);
    return true;
}

// Saved instrument "<N> <name>.pti" / "%02d %s.pti" -> N (1..48), else 0.
// SPEC GAP (spec 10 §7 item 1): the stock scan is unresolved; a leading
// number 1..48 followed by a space and a ".pti" suffix is accepted.
int savedPtiSlot(const char* n) {
    if (!endsWithNoCase(n, ".pti")) return 0;
    int num = 0, digits = 0;
    while (*n >= '0' && *n <= '9' && digits < 3) {
        num = num * 10 + (*n++ - '0');
        ++digits;
    }
    if (!digits || *n != ' ' || num < 1 || num > kSampleInstrumentCount) return 0;
    return num;
}

// "instrument_NN.mti" -> NN (1..48), else 0 (spec 02 §1 legacy layout).
int legacyMtiSlot(const char* n) {
    if (std::strncmp(n, "instrument_", 11) != 0 || !endsWithNoCase(n, ".mti")) return 0;
    if (std::strlen(n) != 17) return 0;
    const char a = n[11], b = n[12];
    if (a < '0' || a > '9' || b < '0' || b > '9') return 0;
    const int num = (a - '0') * 10 + (b - '0');
    return num >= 1 && num <= kSampleInstrumentCount ? num : 0;
}

// "instrNN.wav" -> NN (1..48), else 0.
int legacyWavSlot(const char* n) {
    if (std::strncmp(n, "instr", 5) != 0 || std::strlen(n) != 11 || !endsWithNoCase(n, ".wav")) return 0;
    const char a = n[5], b = n[6];
    if (a < '0' || a > '9' || b < '0' || b > '9') return 0;
    const int num = (a - '0') * 10 + (b - '0');
    return num >= 1 && num <= kSampleInstrumentCount ? num : 0;
}

void mtiPath(char* out, size_t cap, const char* base, int n) {
    std::snprintf(out, cap, "%s/instruments/instrument_%02d.mti", base, n);
}
void legacyWavPath(char* out, size_t cap, const char* base, int n) {
    std::snprintf(out, cap, "%s/samples/instr%02d.wav", base, n);
}

bool eqNoCase(const char* a, const char* b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
        if (!x) return true;
    }
}

// "pattern_NN<ext>" as patternFilePath() spells pattern NN (1..255) with
// ext ".mtp" (or ".mtp.tmp", an interrupted atomic write), compared without
// case like FatFs lookups; else 0.
int patternFileNumber(const char* n, const char* ext = ".mtp") {
    const size_t el = std::strlen(ext);
    if (std::strlen(n) < 10 + el || std::strlen(n) > 11 + el) return 0;
    char pre[9];
    std::memcpy(pre, n, 8);
    pre[8] = 0;
    if (!eqNoCase(pre, "pattern_")) return 0;
    int num = 0, digits = 0;
    const char* p = n + 8;
    while (*p >= '0' && *p <= '9') {
        num = num * 10 + (*p++ - '0');
        ++digits;
    }
    if (!digits || digits > 3 || !eqNoCase(p, ext) || num < 1 || num > kPatternCount) return 0;
    char canon[24];
    std::snprintf(canon, sizeof(canon), "pattern_%02d%s", num, ext);
    return eqNoCase(canon, n) ? num : 0;
}

// Workspace "instrument_NN.pti" / ".mti" (case-insensitive, as the FatFs
// existence checks they replace): slot NN (1..48) and *mti, else 0.
int workspaceInstrumentSlot(const char* n, bool* mti) {
    if (std::strlen(n) != 17) return 0;
    char pre[12];
    std::memcpy(pre, n, 11);
    pre[11] = 0;
    if (!eqNoCase(pre, "instrument_")) return 0;
    const char a = n[11], b = n[12];
    if (a < '0' || a > '9' || b < '0' || b > '9') return 0;
    const int num = (a - '0') * 10 + (b - '0');
    if (num < 1 || num > kSampleInstrumentCount) return 0;
    if (eqNoCase(n + 13, ".pti")) *mti = false;
    else if (eqNoCase(n + 13, ".mti")) *mti = true;
    else return 0;
    return num;
}

// One scan of a project folder's instruments/, samples/ and patterns/
// (2.x: replaces the per-file existence probes; each probe walked the whole
// directory on the card). For a saved project: "<N> <name>.pti" and
// instrument_NN.mti per slot, samples/instrNN.wav, and the pattern files;
// for the Workspace: instrument_NN.pti / .mti and the pattern files.
struct SlotScan {
    uint32_t ptiPayload[kSampleInstrumentCount];
    uint32_t wavBytes[kSampleInstrumentCount];
    bool hasPti[kSampleInstrumentCount];
    bool hasMti[kSampleInstrumentCount];
    bool hasWav[kSampleInstrumentCount];
    int8_t ptiName[kSampleInstrumentCount];   // collected-name index of the slot's .pti, -1 none
    uint8_t patBits[(kPatternCount + 8) / 8];
    uint8_t patTmp[(kPatternCount + 8) / 8];   // pattern_NN.mtp.tmp (interrupted write)
    // Synth slot patches in the folder root (spec 21 §2): N.patch, N.patch.tmp.
    bool patch[kSynthSlotCount], patchTmp[kSynthSlotCount];
    bool namesFile;
    bool collect;      // collect instrument file names into `names`
    bool workspace;    // Workspace naming (instrument_NN.pti)
    char (*names)[64];
    int namesCap;
    int listCount;
    uint64_t instrBytes, patBytes, sampleBytes;
    uint32_t files;
};
bool scanInstrFn(const IStorage::DirEntry& e, void* ctx) {
    auto* s = static_cast<SlotScan*>(ctx);
    if (e.isDir) return true;
    s->instrBytes += e.size;
    ++s->files;
    if (s->workspace) {
        bool mti = false;
        if (int n = workspaceInstrumentSlot(e.name, &mti)) {
            (mti ? s->hasMti : s->hasPti)[n - 1] = true;
            // The payload too (the memory checks and Auto streaming's
            // over-budget test counted 0 for every Workspace instrument).
            if (!mti) s->ptiPayload[n - 1] = e.size > pti::kDataOff ? e.size - pti::kDataOff : 0;
        }
        return true;
    }
    if (int n = savedPtiSlot(e.name)) {
        s->hasPti[n - 1] = true;
        s->ptiPayload[n - 1] = e.size > pti::kDataOff ? e.size - pti::kDataOff : 0;
    } else if (int m = legacyMtiSlot(e.name)) {
        s->hasMti[m - 1] = true;
    }
    // Open copies the first "<N> <name>.pti" per slot in listing order, from
    // the first namesCap file names (the stored name is what gets opened).
    if (s->collect && e.name[0] != '.' && s->listCount < s->namesCap) {
        const int k = s->listCount++;
        std::snprintf(s->names[k], 64, "%s", e.name);
        const int n = savedPtiSlot(s->names[k]);
        if (n && s->ptiName[n - 1] < 0) s->ptiName[n - 1] = int8_t(k);
    }
    return true;
}
bool scanWavFn(const IStorage::DirEntry& e, void* ctx) {
    auto* s = static_cast<SlotScan*>(ctx);
    if (e.isDir) return true;
    s->sampleBytes += e.size;
    ++s->files;
    if (int n = legacyWavSlot(e.name)) {
        s->hasWav[n - 1] = true;
        s->wavBytes[n - 1] = e.size > wav::kHeaderSize ? e.size - wav::kHeaderSize : 0;
    }
    return true;
}
bool scanPatFn(const IStorage::DirEntry& e, void* ctx) {
    auto* s = static_cast<SlotScan*>(ctx);
    if (e.isDir) return true;
    s->patBytes += e.size;
    ++s->files;
    if (int n = patternFileNumber(e.name)) s->patBits[n >> 3] |= uint8_t(1u << (n & 7));
    else if (int m = patternFileNumber(e.name, ".mtp.tmp")) s->patTmp[m >> 3] |= uint8_t(1u << (m & 7));
    else if (eqNoCase(e.name, "patternsMetadata")) s->namesFile = true;
    return true;
}
// "N.patch" / "N.patch.tmp" (N = 0..2) -> slot N and *tmp, else -1.
int patchFileSlot(const char* n, bool* tmp) {
    if (n[0] < '0' || n[0] >= char('0' + kSynthSlotCount)) return -1;
    if (eqNoCase(n + 1, ".patch")) *tmp = false;
    else if (eqNoCase(n + 1, ".patch.tmp")) *tmp = true;
    else return -1;
    return n[0] - '0';
}
bool scanRootFn(const IStorage::DirEntry& e, void* ctx) {
    auto* s = static_cast<SlotScan*>(ctx);
    if (e.isDir) return true;
    bool tmp = false;
    const int k = patchFileSlot(e.name, &tmp);
    if (k < 0) return true;
    (tmp ? s->patchTmp : s->patch)[k] = true;
    s->patBytes += e.size;
    ++s->files;
    return true;
}
inline bool bitOf(const uint8_t* b, int n) { return (b[n >> 3] >> (n & 7)) & 1; }
inline void setBit(uint8_t* b, int n, bool on) {
    if (on) b[n >> 3] |= uint8_t(1u << (n & 7));
    else b[n >> 3] &= uint8_t(~(1u << (n & 7)));
}
void scanSlots(IStorage& f, const char* dir, SlotScan& s, bool workspace = false, char (*names)[64] = nullptr,
               int namesCap = 0, bool patterns = false) {
    std::memset(&s, 0, sizeof(s));
    for (auto& k : s.ptiName) k = -1;
    s.workspace = workspace;
    s.collect = names != nullptr;
    s.names = names;
    s.namesCap = namesCap;
    char p[300];
    std::snprintf(p, sizeof(p), "%s/instruments", dir);
    f.listDir(p, scanInstrFn, &s);
    if (!workspace) {
        std::snprintf(p, sizeof(p), "%s/samples", dir);
        f.listDir(p, scanWavFn, &s);
    }
    if (patterns) {
        std::snprintf(p, sizeof(p), "%s/patterns", dir);
        f.listDir(p, scanPatFn, &s);
        // Folder root: the synth slot patches (N.patch).
        f.listDir(dir, scanRootFn, &s);
    }
}

// Atomic small-file write: "<path>.tmp", then remove "<path>", then rename.
// A power loss leaves the old file, or a complete .tmp next to no file
// (restored on load when it validates); a partial .tmp is dropped.
bool writeAtomic(IStorage& f, const char* path, const uint8_t* buf, uint32_t len) {
    char tmp[340];
    std::snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (writeWholeFile(f, tmp, buf, len) != FmtStatus::Ok) {
        f.remove(tmp);
        return false;
    }
    if (f.exists(path) && !f.remove(path)) return false;
    return f.rename(tmp, path);
}

void patchPath(char* out, size_t cap, const char* dir, int s, const char* suffix = "") {
    std::snprintf(out, cap, "%s/%d.patch%s", dir, s, suffix);
}

// A complete 16-track pattern image (restoring an orphan .mtp.tmp): the
// size field and length are 0x3030 and the CRC matches.
bool completeMiniPattern(const uint8_t* b, uint32_t len) {
    return len == mtp::kMiniFileSize && rdU16(b + 0x0C) == mtp::kMiniFileSize &&
           crc32(b, mtp::kMiniCrcOff) == rdU32(b + mtp::kMiniCrcOff);
}
// <path> written through <path>.tmp for a .patch: the Extra field is
// zero-filled in the first pass and written last (seek back), so a .tmp cut
// at any point has no valid Extra. Extra is not the last field of every
// file (unknown fields 10 / 11 follow it in some factory patches), so a
// content check alone could not tell a cut right after Extra from a whole
// file; with this write order a decodable Extra means every other byte is
// already on the card.
// work: len bytes of scratch for the first-pass image.
bool writePatchAtomic(IStorage& f, const char* path, const uint8_t* buf, uint32_t len, uint8_t* work) {
    size_t beg = 0, end = 0;
    if (!patchFieldSpan(buf, len, 9, &beg, &end)) return false;
    std::memcpy(work, buf, len);
    std::memset(work + beg, 0, end - beg);
    char tmp[352];
    std::snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (f.exists(tmp) && !f.remove(tmp)) return false;
    IStorage::Handle h = f.open(tmp, IStorage::kWrite);
    if (h < 0) return false;
    const bool ok = f.write(h, work, len) == int32_t(len) && f.seek(h, uint32_t(beg)) &&
                    f.write(h, buf + beg, uint32_t(end - beg)) == int32_t(end - beg);
    f.close(h);
    if (!ok) {
        f.remove(tmp);
        return false;
    }
    if (f.exists(path) && !f.remove(path)) return false;
    return f.rename(tmp, path);
}

// A complete .patch (restoring an orphan .patch.tmp): it decodes with the
// Extra field (written last by writePatchAtomic) and has the full expected
// size: it re-encodes to exactly its own `len` bytes (encBuf: scratch of
// kPatchMaxFileSize bytes). A file cut right after Extra loses only the [OT]
// extension parameters (field 16, after Extra): it plays mono.
bool completePatch(const uint8_t* b, uint32_t len, SynthPatch* scratchPatch, uint8_t* encBuf) {
    if (!len || decodePatch(b, len, scratchPatch) != PatchStatus::kOk || !scratchPatch->extraPresent) return false;
    size_t n = 0;
    return encodePatch(*scratchPatch, encBuf, kPatchMaxFileSize, &n) == PatchStatus::kOk && n == len &&
           std::memcmp(encBuf, b, n) == 0;
}

// Scratch layout of the pattern / patch units: the file read at 0, the
// split ext tracks at kExtOff, the encoded output at kOutOff, a decode
// SynthPatch at kPatchOff.
constexpr uint32_t kExtOff = 0x3100;
constexpr uint32_t kOutOff = 0x4000;
constexpr uint32_t kPatchOff = 0x8000;
static_assert(kExtOff >= mtp::kMiniFileSize && kExtOff + sizeof(Track) * kExtTrackCount <= kOutOff, "ext slot");
static_assert(kOutOff + mtp::kMiniFileSize <= kPatchOff, "output slot");
static_assert(kOutOff + 2 * kPatchMaxFileSize <= kPatchOff, "patch output + its write image");
static_assert(kPatchMaxFileSize + 1 <= kExtOff, "patch read fits the file slot");
static_assert(kPatchOff + sizeof(SynthPatch) <= 0x10000, "pattern scratch fits 64 KB");

// spec 02 §8.2 / spec 10 §4.2 step 0: sample data of a scanned saved project.
// SPEC GAP: file headers (0x188 / 44 bytes) are not counted.
uint64_t scanSampleBytes(const SlotScan& s) {
    uint64_t sum = 0;
    for (int i = 0; i < kSampleInstrumentCount; ++i) {
        if (s.hasPti[i]) sum += s.ptiPayload[i];
        else if (s.hasMti[i] && s.hasWav[i]) sum += s.wavBytes[i];
    }
    return sum;
}

// Open pre-check (startOpenProject and the File screen's projectFitsMemory):
// the sample payloads fit the bank, or, with Stream from card on, they fit
// with the long samples streamed (their heads are counted, not the payloads).
bool scanFitsMemory(const App& a, const SlotScan& s) {
    if (scanSampleBytes(s) <= a.bank.capacityBytes()) return true;
    return a.cfg.streamMode != 0 && a.hal().streamReader &&
           streamProjectedBytes(s.ptiPayload, s.hasPti, kSampleInstrumentCount, StreamMode(a.cfg.streamMode),
                                kStreamThresholdSec[a.cfg.streamThreshold & 3]) <= a.bank.capacityBytes();
}

bool stopAtFirst(const IStorage::DirEntry&, void*) { return false; }

// removeTree (public issue #55): every path under a folder must fit the
// card's limit (IStorage::kMaxPath) before anything is deleted.
constexpr uint32_t kTreeSteps = 1u << 20;   // safety stop for a corrupt directory
struct FitScan {
    size_t base;   // length of the folder path
    bool ok;
};
bool fitScanFn(const IStorage::DirEntry& e, void* ctx) {
    auto* s = static_cast<FitScan*>(ctx);
    if (!std::strcmp(e.name, ".") || !std::strcmp(e.name, "..")) return true;
    if (s->base + 1 + std::strlen(e.name) > IStorage::kMaxPath) {
        s->ok = false;
        return false;
    }
    return true;
}
struct NthDir {
    int want, seen;
    bool found;
    char name[256];
};
bool nthDirFn(const IStorage::DirEntry& e, void* ctx) {
    auto* d = static_cast<NthDir*>(ctx);
    if (!e.isDir || !std::strcmp(e.name, ".") || !std::strcmp(e.name, "..")) return true;
    if (d->seen++ != d->want) return true;
    std::snprintf(d->name, sizeof(d->name), "%s", e.name);
    d->found = true;
    return false;
}
// Walks the tree without recursion (one path buffer, the index of the next
// subfolder per level) and deletes nothing.
bool treeFits(IStorage& f, const char* root) {
    char p[IStorage::kMaxPath + 1];
    if (std::strlen(root) > IStorage::kMaxPath) return false;
    std::snprintf(p, sizeof(p), "%s", root);
    int next[IStorage::kMaxPath / 2 + 2];   // each level adds at least 2 characters
    int depth = 0;
    next[0] = 0;
    for (uint32_t step = 0; step < kTreeSteps; ++step) {
        const size_t n = std::strlen(p);
        if (next[depth] == 0) {   // first visit: every entry's path fits
            FitScan s{n, true};
            if (f.listDir(p, fitScanFn, &s) < 0 || !s.ok) return false;
        }
        NthDir d{next[depth], 0, false, {}};
        if (f.listDir(p, nthDirFn, &d) < 0) return false;
        if (d.found) {
            ++next[depth];
            if (depth + 1 >= int(sizeof(next) / sizeof(next[0]))) return false;
            std::snprintf(p + n, sizeof(p) - n, "/%s", d.name);
            next[++depth] = 0;
        } else {
            if (depth == 0) return true;
            *std::strrchr(p, '/') = 0;
            --depth;
        }
    }
    return false;
}
}  // namespace

bool App::removeTree(const char* path) {
    IStorage& f = fs();
    if (!f.exists(path)) return true;
    {
        FirstEntry e{};
        if (f.listDir(path, firstEntryFn, &e) < 0) return f.remove(path);   // a file
    }
    // Refused before anything is deleted (no half-deleted project).
    if (!treeFits(f, path)) return false;
    // Depth first without recursion: descend into the first subfolder,
    // delete files, delete a folder once it is empty, then go back up.
    char p[IStorage::kMaxPath + 1];
    std::snprintf(p, sizeof(p), "%s", path);
    const size_t rootLen = std::strlen(p);
    for (uint32_t step = 0; step < kTreeSteps; ++step) {
        FirstEntry e{};
        if (f.listDir(p, firstEntryFn, &e) < 0) return false;
        const size_t n = std::strlen(p);
        if (e.found) {
            if (n + 1 + std::strlen(e.name) > IStorage::kMaxPath) return false;   // appeared after the check
            std::snprintf(p + n, sizeof(p) - n, "/%s", e.name);
            if (e.isDir) continue;
            if (!f.remove(p)) return false;
            p[n] = 0;
            continue;
        }
        if (!f.remove(p)) return false;   // empty now
        if (n == rootLen) return true;
        *std::strrchr(p, '/') = 0;
    }
    return false;
}

bool App::copyFile(const char* from, const char* to) {
    IStorage& f = fs();
    IStorage::Handle src = f.open(from, IStorage::kRead);
    if (src < 0) return false;
    if (f.exists(to)) f.remove(to);
    IStorage::Handle dst = f.open(to, IStorage::kWrite);
    if (dst < 0) {
        f.close(src);
        return false;
    }
    bool ok = true;
    uint8_t* buf = mem_.scratch;
    const uint32_t cap = mem_.scratchBytes > kIoChunk ? kIoChunk : mem_.scratchBytes;
    for (;;) {
        int32_t n = f.read(src, buf, cap);
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) break;
        if (f.write(dst, buf, uint32_t(n)) != n) {
            ok = false;
            break;
        }
    }
    f.close(src);
    f.close(dst);
    return ok;
}

bool App::projectExists(const char* folder, const char* name) {
    char p[280];
    std::snprintf(p, sizeof(p), "%.221s/%.32s/project.mt", folder && folder[0] ? folder : kProjectsDir, name);
    return fs().exists(p);
}

bool App::newProject() {
    // spec 05 appendix E3: flush a running job first.
    finishJob();
    // spec 05 appendix E3 NEW PROJECT: update recents with the current project.
    recentsLeaveCurrent(*this);
    // spec 02 §1.1 "New project".
    seq.stop();
    allVoicesOff();
    IStorage& f = fs();
    removeTree(kWorkspace);
    if (!f.mkdir(kWorkspace)) return false;
    f.mkdir("Workspace/instruments");
    f.mkdir("Workspace/patterns");
    f.mkdir("Workspace/samples");
    project.setDefault();
    project.setName("New Project");
    project.setFolder("");
    project.setCurrentPattern(1);
    names.clear();
    engineDirty_ = true;
    applyStreamOption();   // clears the bank; carves the stream rings when on
    for (int i = 0; i < kSampleInstrumentCount; ++i) {
        instruments[i].setDefault();
        instrPresent[i] = false;
        instrHeaderDirty_[i] = instrSampleDirty_[i] = false;
    }
    std::memset(present_, 0, sizeof(present_));
    std::memset(patDirty_, 0, sizeof(patDirty_));
    resetPatternStore();
    std::memset(extKeep_, 0, sizeof(extKeep_));
    loadNotice_[0] = 0;
    setPresent(1, true);
    assemblePattern(1, pattern());
    attachSequencer();
    char path[96];
    patternFilePath(path, sizeof(path), kWorkspace, 1);
    bool ok = savePatternFile(f, path, stored(1)) == FmtStatus::Ok;
    projectFilePath(path, sizeof(path), kWorkspace);
    ok = saveProjectFile(f, path, project) == FmtStatus::Ok && ok;
    // Synth slots: the default records' patches (library "Init", else the
    // built-in Init), written to Workspace/N.patch like the Mini.
    g_patchKept = 0;
    for (int s = 0; s < kSynthSlotCount; ++s) {
        resolvePatch(s, nullptr, 0, false);
        patchDirty_[s] = !savePatch(s, kWorkspace);
    }
    undo.clear();
    clipValid_ = false;
    projectDirty_ = namesDirty_ = false;
    unsaved_ = false;
    neverSaved_ = true;
    refreshPadNotes();
    ui.dirty();
    return ok;
}

// ---------------------------------------------------------------------------
// Background project operations (spec 05 appendix E3/E4 state machine
// 0x1d5e0; spec 10 §4.2/§4.3 open/save of legacy projects).
//
// A job is a list of stages; each call of jobUnit() does one unit of work:
// one small file (project.mt, one pattern, one header, ...), one directory
// scan, or one kJobChunk piece of a large file (instrument copy, sample
// payload, WAV import). serviceProjectJob() runs units for kJobPassUs of wall
// time per main-loop pass (field 1.9.3: a 64 KB-per-pass byte budget held the
// load to ~34 Hz main-loop passes and ~45 s per project).
//
// Load order (spec 10 §2.2 ordering dependency): project.mt -> instruments
// (header, then the sample data: the play modes the pattern upgrade needs are
// taken from the headers before any sample is processed) -> pattern names ->
// patterns. Stock reads every header, then the patterns, then the sample
// data; the slots are still filled in index order, so the sample bank layout
// is the same and no instrument ever moves (single layout pass).
//
// 2.x loading (docs/PLAN_2X.md "SD and loading"):
//  - one directory scan per folder instead of an existence probe per file;
//  - Open loads while it copies: each "<project>/instruments" file is read
//    once, its payload straight into the sample bank and from there written
//    to Workspace/instruments/instrument_NN.pti (stock: copy as
//    instrument_NN.mti, rename to .pti, read again); each pattern is decoded
//    from the bytes being copied;
//  - reads and writes end on kJobChunk-aligned file offsets (whole clusters).

namespace {
enum Stage : uint8_t {
    kLProject = 1, kLSlots, kLNames, kLPatterns, kLFinish,
    kOWipe, kODirs,
    kABegin, kAPatterns, kANames, kAInstr, kAProject, kAFinish,
    kSDirs, kSPatterns, kSNames, kSCleanList, kSClean, kSInstr, kSFinish,
    kLPatch, kAPatch, kSPatch,   // Tracker Mini synth slot patches (spec 21 §7)
};
// Patterns of a Workspace load that were not in the 16-track layout: the
// autosave rewrites them as 0x3030 (not "unsaved": the content is the same).
uint8_t g_upgrade[(kPatternCount + 8) / 8];

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
constexpr bool kLittleEndian = true;   // .pti payload bytes are the int16 samples
#else
constexpr bool kLittleEndian = false;
#endif

// Instrument file names of the project being opened / saved.
OT_SDRAM_BSS char g_list[kSampleInstrumentCount + 16][64];   // SDRAM on the device (OP-5)
OT_SDRAM_BSS SlotScan g_scan;   // folder scanned by the running job (Open: source; load: Workspace)

struct DirSum {
    uint64_t bytes = 0;
    uint32_t files = 0;
};
bool sumFn(const IStorage::DirEntry& e, void* ctx) {
    auto* s = static_cast<DirSum*>(ctx);
    if (!e.isDir) {
        s->bytes += e.size;
        ++s->files;
    }
    return true;
}
DirSum dirSum(IStorage& f, const char* base, const char* sub) {
    char p[340];
    std::snprintf(p, sizeof(p), "%s/%s", base, sub);
    DirSum s;
    f.listDir(p, sumFn, &s);
    return s;
}

// ---- job profile (telemetry) ----
struct ProfTicks {
    uint64_t open, read, write, dir, decode, commit, total;
};
JobProfile g_prof;
ProfTicks g_ticks;
IClock* g_clock = nullptr;
uint32_t (*g_cycles)() = nullptr;
uint32_t g_cyclesPerUs = 1;

inline uint32_t profNow() { return g_cycles ? g_cycles() : (g_clock ? g_clock->micros() : 0u); }
inline uint32_t ticksToUs(uint64_t t) { return uint32_t(g_cycles ? t / g_cyclesPerUs : t); }

struct ProfScope {
    uint64_t& acc;
    uint32_t t0;
    explicit ProfScope(uint64_t& a) : acc(a), t0(profNow()) {}
    ~ProfScope() { acc += uint32_t(profNow() - t0); }
};

void profBegin(JobOp op) {
    g_prof = JobProfile{};
    g_ticks = ProfTicks{};
    g_prof.op = uint8_t(op);
    g_prof.running = true;
}

// IStorage pass-through that books the job's card time per category.
class ProfStorage : public IStorage {
public:
    IStorage* s = nullptr;
    Handle open(const char* path, OpenMode mode) override {
        ProfScope p(g_ticks.open);
        ++g_prof.opens;
        return s->open(path, mode);
    }
    int32_t read(Handle h, void* dst, uint32_t len) override {
        ProfScope p(g_ticks.read);
        ++g_prof.reads;
        const int32_t r = s->read(h, dst, len);
        if (r > 0) g_prof.bytesRead += uint32_t(r);
        return r;
    }
    int32_t write(Handle h, const void* src, uint32_t len) override {
        ProfScope p(g_ticks.write);
        ++g_prof.writes;
        const int32_t r = s->write(h, src, len);
        if (r > 0) g_prof.bytesWritten += uint32_t(r);
        return r;
    }
    bool seek(Handle h, uint32_t pos) override {
        ProfScope p(g_ticks.read);
        return s->seek(h, pos);
    }
    int32_t size(Handle h) override { return s->size(h); }
    void close(Handle h) override {
        ProfScope p(g_ticks.open);
        s->close(h);
    }
    bool exists(const char* path) override { return dirOp([&] { return s->exists(path); }); }
    bool mkdir(const char* path) override { return dirOp([&] { return s->mkdir(path); }); }
    bool remove(const char* path) override { return dirOp([&] { return s->remove(path); }); }
    bool rename(const char* from, const char* to) override { return dirOp([&] { return s->rename(from, to); }); }
    int32_t listDir(const char* path, DirFn fn, void* ctx) override {
        ProfScope p(g_ticks.dir);
        ++g_prof.dirOps;
        return s->listDir(path, fn, ctx);
    }

private:
    template <class F>
    bool dirOp(F f) {
        ProfScope p(g_ticks.dir);
        ++g_prof.dirOps;
        return f();
    }
};
ProfStorage g_pfs;

// ---- time-based unit sizing ----
// Card time per KB (x16, moving average) measured from the job's own reads
// and writes, and the time left in the current pass: a transfer unit moves
// as many bytes as fit, 4 KB .. kJobChunk, ending on 4 KB file offsets and
// never crossing a kJobChunk (cluster) boundary.
// Start values until measured: 20 MB/s reads, 8 MB/s writes.
uint32_t g_readUs16PerKB = 16u * 1024u / 20u, g_writeUs16PerKB = 16u * 1024u / 8u;
uint32_t g_unitBudgetUs = kJobPassUs;
bool g_unitSized = false;   // the running unit sized itself with unitBytes()
bool g_unitClose = false;   // the running unit closed a written file
uint32_t g_closeUs = 3000;  // measured duration of such a close

uint32_t unitBytes(bool read, bool write) {
    g_unitSized = true;
    const uint32_t per = (read ? g_readUs16PerKB : 0u) + (write ? g_writeUs16PerKB : 0u);
    if (!per) return kJobChunk;
    const uint64_t b = uint64_t(g_unitBudgetUs) * 16u * 1024u / per;
    if (b >= kJobChunk) return kJobChunk;
    return b < 0x1000 ? 0x1000u : uint32_t(b) & ~0xFFFu;
}
uint32_t chunkEnd(uint32_t pos, uint32_t cap) {
    uint32_t e = (pos + cap) & ~0xFFFu;
    if (e <= pos) e = (pos & ~0xFFFu) + 0x1000u;
    const uint32_t c = (pos / kJobChunk + 1) * kJobChunk;
    return e < c ? e : c;
}
void learnRate(uint32_t& ema, uint64_t ticks, uint64_t bytes) {
    if (bytes < 0x1000) return;
    const uint64_t v = uint64_t(ticksToUs(ticks)) * 16u * 1024u / bytes;
    const uint32_t x = v > 0xFFFFFFu ? 0xFFFFFFu : uint32_t(v);
    ema = ema ? (ema * 3u + x) / 4u : x;
}

// Whole small file src -> dst through buf (cap bytes); the first
// min(size, cap) bytes stay in buf for decoding. 1 = copied, 0 = failed.
int copySmallFile(IStorage& f, const char* src, const char* dst, uint8_t* buf, uint32_t cap, uint32_t keep,
                  uint32_t& size, uint32_t& cost) {
    size = 0;
    IStorage::Handle in = f.open(src, IStorage::kRead);
    if (in < 0) return 0;
    const int32_t sz = f.size(in);
    IStorage::Handle out = sz >= 0 ? f.open(dst, IStorage::kWrite) : -1;
    if (out < 0) {
        f.close(in);
        return 0;
    }
    bool ok = true;
    uint32_t pos = 0;
    const uint32_t total = uint32_t(sz);
    while (ok && pos < total) {
        // The first chunk fills buf; later ones (files over cap) reuse the
        // part of buf past `keep` so the decoded prefix survives.
        uint8_t* d = pos == 0 ? buf : buf + keep;
        const uint32_t room = pos == 0 ? cap : cap - keep;
        const uint32_t want = total - pos < room ? total - pos : room;
        int32_t r = f.read(in, d, want);
        if (r < 0 && f.seek(in, pos)) r = f.read(in, d, want);   // one retry
        if (r <= 0 || f.write(out, d, uint32_t(r)) != r) ok = false;
        if (r > 0) {
            pos += uint32_t(r);
            cost += 2u * uint32_t(r);
        }
    }
    f.close(in);
    f.close(out);
    size = pos;
    return ok ? 1 : 0;
}
}  // namespace

const JobProfile& jobProfile() {
    g_prof.openUs = ticksToUs(g_ticks.open);
    g_prof.readUs = ticksToUs(g_ticks.read);
    g_prof.writeUs = ticksToUs(g_ticks.write);
    g_prof.dirUs = ticksToUs(g_ticks.dir);
    g_prof.decodeUs = ticksToUs(g_ticks.decode);
    g_prof.commitUs = ticksToUs(g_ticks.commit);
    g_prof.totalUs = ticksToUs(g_ticks.total);
    return g_prof;
}

void setJobProfileCycles(uint32_t (*cycles)(), uint32_t cyclesPerUs) {
    g_cycles = cycles;
    g_cyclesPerUs = cyclesPerUs ? cyclesPerUs : 1;
}

int App::jobPercent() const {
    if (pj_.op == JobOp::None) return 100;
    if (pj_.op == JobOp::External) {
        const int p = pj_.extPercent ? pj_.extPercent(pj_.extCtx) : 0;
        return p < 0 ? 0 : (p > 99 ? 99 : p);
    }
    const uint64_t t = pj_.total ? pj_.total : 1;
    const uint64_t p = pj_.done * 100 / t;
    return p > 99 ? 99 : int(p);
}

void App::estimateJobTotal() {
    IStorage& f = fs();
    uint64_t t = 0;
    auto add = [&](const DirSum& s, uint64_t mul) { t += s.bytes * mul + uint64_t(s.files) * kJobSmallCost; };
    switch (pj_.op) {
        case JobOp::Open:
            // From the source scan: patterns and instruments are read once
            // and written once, legacy WAVs read and written as .pti.
            t = 2 * (g_scan.patBytes + g_scan.instrBytes + g_scan.sampleBytes) + uint64_t(g_scan.files) * kJobSmallCost;
            break;
        case JobOp::LoadWorkspace:
            t = g_scan.patBytes + g_scan.instrBytes + uint64_t(g_scan.files) * kJobSmallCost +
                2 * dirSum(f, kWorkspace, "samples").bytes;
            break;
        case JobOp::Save:
            add(dirSum(f, kWorkspace, "patterns"), 1);
            add(dirSum(f, kWorkspace, "instruments"), 1);
            break;
        default:
            break;
    }
    pj_.total = t ? t : 1;
}

void App::jobProgress() {
    if (!pj_.popup || pj_.op == JobOp::None) return;
    ui.setProgress(pj_.title, jobPercent());
}

void App::jobCloseFiles() {
    IStorage& f = fs();
    if (pj_.in >= 0) f.close(pj_.in);
    if (pj_.out >= 0) f.close(pj_.out);
    pj_.in = pj_.out = -1;
}

void App::jobEnd() {
    jobCloseFiles();
    const bool ok = pj_.ok;
    const bool popup = pj_.popup;
    JobDoneFn fn = pj_.onDone;
    void* ctx = pj_.ctx;
    lastJobOk_ = ok;
    lastJobErrors_ = pj_.loadErrors;
    pj_ = ProjectJob{};
    g_prof.running = false;
    if (popup) ui.progressDone();
    if (fn) fn(ctx, ok);
}

void App::jobFail() {
    pj_.ok = false;
    jobEnd();
}

void App::abortJob() {
    if (pj_.op != JobOp::None) jobFail();
}

bool App::serviceProjectJob() {
    if (pj_.op == JobOp::None) return false;
    g_clock = hal_.clock;
    g_pfs.s = &fs();
    if (!pj_.profiled) {
        profBegin(pj_.op);
        pj_.profiled = true;
    }
    IClock* clk = hal_.clock;
    const uint32_t t0 = clk ? clk->micros() : 0;
    const uint32_t p0 = profNow();
    const uint8_t stage0 = pj_.stage, phase0 = pj_.phase;
    uint32_t cost = 0;
    g_unitBudgetUs = kJobPassUs;
    while (pj_.op != JobOp::None) {
        const uint32_t u0 = clk ? clk->micros() : 0;
        const uint32_t pu = profNow();
        const uint8_t stage = pj_.stage;
        g_unitSized = g_unitClose = false;
        const uint64_t r0 = g_ticks.read, w0 = g_ticks.write, br0 = g_prof.bytesRead, bw0 = g_prof.bytesWritten;
        const bool more = jobUnit(cost);
        const uint32_t unitUs = ticksToUs(uint32_t(profNow() - pu));
        ++g_prof.units;
        if (unitUs > g_prof.maxUnitUs) {
            g_prof.maxUnitUs = unitUs;
            g_prof.maxUnitStage = stage;
        }
        learnRate(g_readUs16PerKB, g_ticks.read - r0, g_prof.bytesRead - br0);
        learnRate(g_writeUs16PerKB, g_ticks.write - w0, g_prof.bytesWritten - bw0);
        if (!more) break;
        const uint32_t now = clk ? clk->micros() : 0;
        const uint32_t unit = now - u0, elapsed = now - t0;
        pj_.lastUnitUs = unit;
        if (elapsed >= kJobPassUs || cost >= kJobPassBytes) break;
        const uint32_t left = kJobPassUs - elapsed;
        // Transfer units size themselves to the time left; any other unit is
        // expected to take as long as the last one, and a new stage starts
        // in a pass of its own (its first unit has no history).
        if (g_unitClose) g_closeUs = (g_closeUs * 3u + unit) / 4u;
        if (!g_unitSized && unit > left) break;
        const bool closeNext = pj_.stage == kLSlots && ((pj_.phase == 5 && pj_.wphase == 2) ||
                                                        (pj_.phase == 1 && pj_.pos >= pj_.len && pj_.out >= 0));
        if (closeNext && g_closeUs > left) break;
        if ((elapsed && pj_.stage != stage) || left < 500) break;
        // A file create in a large folder is the longest unit: give it a pass
        // of its own.
        const bool create = pj_.fromProject && ((pj_.stage == kLPatterns && pj_.phase == 1) ||
                                                (pj_.stage == kLSlots && pj_.phase == 6));
        if (create && elapsed) break;
        g_unitBudgetUs = left;
    }
    const uint32_t passTicks = uint32_t(profNow() - p0);
    g_ticks.total += passTicks;
    g_prof.lastPassUs = ticksToUs(passTicks);
    if (g_prof.lastPassUs > g_prof.maxPassUs) {
        g_prof.maxPassUs = g_prof.lastPassUs;
        g_prof.maxPassStage = stage0;
        g_prof.maxPassPhase = phase0;
    }
    ++g_prof.passes;
    jobProgress();
    return pj_.op != JobOp::None;
}

void App::finishJob() {
    while (serviceProjectJob()) {
        // Blocking callers (boot, power-off): keep the popup moving
        // (spec 05 §1.3: progress refresh every > 50 ms).
        if (pj_.popup && hal_.display && millis() - jobProgressMs_ > 50) {
            jobProgressMs_ = millis();
            ui.render();
        }
    }
}

// One chunk of a file copy. Returns true when the copy is finished (or
// failed: *failed set). The target is created or truncated (FatFs
// FA_CREATE_ALWAYS); chunks end on kJobChunk-aligned offsets.
bool App::jobCopyUnit(const char* from, const char* to, uint32_t& cost, bool* failed) {
    IStorage& f = g_pfs;
    if (pj_.phase == 0) {
        pj_.in = f.open(from, IStorage::kRead);
        if (pj_.in < 0) {
            *failed = true;
            cost += kJobSmallCost;
            return true;
        }
        pj_.out = f.open(to, IStorage::kWrite);
        if (pj_.out < 0) {
            jobCloseFiles();
            *failed = true;
            cost += kJobSmallCost;
            return true;
        }
        const int32_t sz = f.size(pj_.in);
        pj_.len = sz > 0 ? uint32_t(sz) : 0;
        pj_.pos = 0;
        pj_.phase = 1;
        cost += kJobSmallCost;
        pj_.done += kJobSmallCost;
    }
    uint32_t want = chunkEnd(pj_.pos, unitBytes(true, true)) - pj_.pos;
    if (want > mem_.scratchBytes) want = mem_.scratchBytes;
    int32_t n = pj_.pos < pj_.len ? f.read(pj_.in, mem_.scratch, want) : 0;
    if (n < 0 && f.seek(pj_.in, pj_.pos)) n = f.read(pj_.in, mem_.scratch, want);   // one retry
    bool bad = n < 0;
    if (n > 0 && f.write(pj_.out, mem_.scratch, uint32_t(n)) != n) bad = true;
    if (n > 0) {
        pj_.pos += uint32_t(n);
        cost += 2u * uint32_t(n);
        pj_.done += uint32_t(n);
    }
    if (bad || n <= 0 || pj_.pos >= pj_.len) {
        jobCloseFiles();
        pj_.phase = 0;
        if (bad) *failed = true;
        return true;
    }
    return false;
}

// One chunk of writing instrument `slot` from the sample bank as a full
// .pti (header + payload, spec 02 §6). Returns true when finished. A bank
// change while writing (sample edit, import) abandons the file and marks the
// slot dirty again so the next autosave rewrites it. The payload is written
// straight from the bank (little-endian int16 = the file bytes), in pieces
// that end on kJobChunk-aligned file offsets.
bool App::jobPtiWriteUnit(int slot, const char* path, uint32_t& cost, bool* failed) {
    IStorage& f = g_pfs;
    if (pj_.wphase == 0) {
        Instrument ins = instruments[slot];
        ins.lengthFrames = bank.frames(slot);
        ins.stereo = bank.stereo(slot);   // planar L+R payload (2x-04 §8)
        pj_.src = bank.left(slot);
        pj_.srcFrames = ins.lengthFrames;
        pj_.len = ins.lengthFrames * (ins.stereo ? 2u : 1u) * 2u;   // payload bytes
        pj_.pos = 0;
        pj_.out = f.open(path, IStorage::kWrite);   // created or truncated
        cost += kJobSmallCost;
        if (pj_.out < 0) {
            *failed = true;
            return true;
        }
        uint8_t hdr[pti::kHeaderBlockSize];
        encodePtiHeader(ins, hdr);
        if (f.write(pj_.out, hdr, sizeof(hdr)) != int32_t(sizeof(hdr))) {
            jobCloseFiles();
            *failed = true;
            return true;
        }
        pj_.wphase = 1;
        if (!pj_.src) pj_.len = 0;
        if (pj_.len) return false;   // payload from the next unit on
    }
    if (pj_.wphase == 2) {
        // Close (directory entry + FAT + FSINFO writes) in a unit of its own.
        g_unitClose = true;
        jobCloseFiles();
        pj_.wphase = 0;
        return true;
    }
    if (bank.left(slot) != pj_.src || bank.frames(slot) != pj_.srcFrames) {
        jobCloseFiles();
        pj_.wphase = 0;
        instrSampleDirty_[slot] = true;
        return true;
    }
    if (pj_.pos < pj_.len) {
        const uint32_t fo = pti::kDataOff + pj_.pos;
        uint32_t n = chunkEnd(fo, unitBytes(false, true)) - fo;
        if (n > pj_.len - pj_.pos) n = pj_.len - pj_.pos;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pj_.src) + pj_.pos;
        if (!kLittleEndian) {
            if (n > mem_.scratchBytes) n = mem_.scratchBytes & ~1u;
            for (uint32_t k = 0; k < n / 2; ++k) wrU16(mem_.scratch + 2 * k, uint16_t(pj_.src[pj_.pos / 2 + k]));
            bytes = mem_.scratch;
        }
        if (f.write(pj_.out, bytes, n) != int32_t(n)) {
            jobCloseFiles();
            pj_.wphase = 0;
            *failed = true;
            return true;
        }
        pj_.pos += n;
        cost += n;
        pj_.done += n;
        if (pj_.pos >= pj_.len) pj_.wphase = 2;
        return false;
    }
    jobCloseFiles();
    pj_.wphase = 0;
    return true;
}

// Instrument `slot` (spec 10 §3.3), one unit per call; true when the slot is
// finished. Phase 0 opens the file and decodes its 0x188-byte header (the
// play mode goes to pj_.modes for the pattern upgrade). A .pti payload is
// then read in cluster-aligned chunks straight into the sample bank tail and
// committed in index order; a legacy payload-less header imports
// samples/instrNN.wav with the normal importer and writes a v9 .pti.
// Open (pj_.fromProject): the source is "<dir>/instruments/<N> <name>.pti" or
// instrument_NN.mti, and every byte read is also written to
// Workspace/instruments/instrument_NN.pti (stock: copy as .mti, rename, load).
// Load: Workspace/instrument_NN.pti (a lone .mti is renamed first).
// ---------------------------------------------------------------------------
// SD streaming: head-only loading (docs/PLAN_SD_STREAMING_P1C_P2.md §2.2)

uint32_t App::streamWouldResident(int slot, uint32_t frames, bool stereo) {
    pj_.streamThis = false;
    const uint32_t whole = frames * (stereo ? 2u : 1u);
    if (!stream_.on || ((pj_.noStream[slot / 8] >> (slot % 8)) & 1u)) return whole;
    int inUse = 0;
    for (int i = 0; i < kSampleInstrumentCount; ++i) inUse += stream_.streamOf[i] >= 0;
    StreamDecisionIn d;
    d.mode = StreamMode(cfg.streamMode > 2 ? 0 : cfg.streamMode);
    d.playMode = instruments[slot].playMode;
    d.frames = frames;
    d.stereo = stereo;
    d.thresholdSec = kStreamThresholdSec[cfg.streamThreshold > 3 ? 1 : cfg.streamThreshold];
    d.overBudget = pj_.overBudget;
    d.readerAvailable = hal_.streamReader != nullptr;
    d.streamsInUse = inUse;
    if (!streamDecision(d)) return whole;
    const StreamResident r = streamResident(instruments[slot].playMode, frames, instruments[slot].loopPoint1);
    pj_.streamThis = true;
    pj_.fullFrames = frames;
    pj_.resHead = r.headFrames;
    pj_.resLoopStart = r.loopStart;
    pj_.resLoopFrames = r.loopFrames;
    return r.framesPerChannel() * (stereo ? 2u : 1u);
}

void App::planStreamRanges(int slot, uint32_t frames, bool stereo, uint32_t off) {
    (void)slot;
    if (!pj_.streamThis) return;
    const uint32_t perCh = pj_.resHead + pj_.resLoopFrames;
    int n = 0;
    auto add = [&](uint32_t fileOff, uint32_t frm, uint32_t bankFrame) {
        if (frm) pj_.ranges[n++] = {fileOff, frm * 2u, bankFrame * 2u};
    };
    add(off, pj_.resHead, 0);                                              // L head
    add(off + pj_.resLoopStart * 2u, pj_.resLoopFrames, pj_.resHead);      // L loop head
    if (stereo) {
        const uint32_t r0 = off + frames * 2u;                             // planar: R after L
        add(r0, pj_.resHead, perCh);                                       // R head
        add(r0 + pj_.resLoopStart * 2u, pj_.resLoopFrames, perCh + pj_.resHead);   // R loop head
    }
    pj_.nRanges = uint8_t(n);
    pj_.capLen = perCh * (stereo ? 2u : 1u) * 2u;
}

bool App::bindStreamSlot(int slot, const char* path, const char** reason) {
    if (reason) *reason = "no reader";
    if (!hal_.streamReader) return false;
    int k = -1;
    bool used[audio::kMaxStreams] = {};
    for (int i = 0; i < kSampleInstrumentCount; ++i)
        if (stream_.streamOf[i] >= 0) used[stream_.streamOf[i]] = true;
    for (int i = 0; i < audio::kMaxStreams && k < 0; ++i)
        if (!used[i]) k = i;
    if (reason) *reason = "no slot";
    if (k < 0) return false;
    audio::StreamSource& s = stream_.src[k];
    s = audio::StreamSource{};
    s.instrument = uint8_t(slot);
    s.frames = pj_.fullFrames;
    s.headL = bank.left(slot);
    s.headR = bank.stereo(slot) ? bank.right(slot) : nullptr;
    s.headFrames = pj_.resHead;
    if (pj_.resLoopFrames) {
        s.loopStart = pj_.resLoopStart;
        s.loopFrames = pj_.resLoopFrames;
        s.loopL = s.headL + pj_.resHead;
        s.loopR = s.headR ? s.headR + pj_.resHead : nullptr;
    }
    audio::StreamBindInfo info;
    if (!hal_.streamReader->bind(s, path, pj_.dataOff[slot], s.headR != nullptr, &info)) {
        if (reason) {
            *reason = info.reason;
        } else if (!loadNotice_[0]) {
            // The slot as a byte: the compiler sees the notice fits (no -Wformat-truncation).
            std::snprintf(loadNotice_, sizeof(loadNotice_), "Instrument %u: %s, loaded into memory",
                          unsigned(uint8_t(slot + 1)),
                          std::strcmp(info.reason, "fragmented") == 0 ? "too fragmented to stream" : "cannot stream");
        }
        return false;
    }
    stream_.streamOf[slot] = int8_t(k);
    engine.setStreamSource(slot, &stream_.src[k]);
    return true;
}

bool App::jobSamplesUnit(int slot, uint32_t& cost) {
    IStorage& f = g_pfs;
    char path[340];
    workspaceInstrumentPath(path, sizeof(path), kWorkspace, slot + 1);
    auto clearSlot = [&](bool error) {
        jobCloseFiles();
        instruments[slot].setDefault();
        instrPresent[slot] = false;
        if (error) ++pj_.loadErrors;
        pj_.phase = pj_.wphase = 0;
        return true;
    };
    auto endSlot = [&]() {
        jobCloseFiles();
        pj_.phase = pj_.wphase = 0;
        pj_.slotFromWorkspace = false;
        return true;
    };
    // Open copies the project's file into the Workspace while loading; a slot
    // whose stream bind fell back restarts from that finished copy instead.
    const bool fromProject = pj_.fromProject && !pj_.slotFromWorkspace;
    uint8_t* bankBytes = reinterpret_cast<uint8_t*>(bank.tail());

    if (pj_.phase == 0) {
        // spec 10 §3.3 steps 1-2.
        pj_.modes[slot] = instruments[slot].playMode;
        pj_.kind[slot] = 0;
        char src[340];
        if (fromProject) {
            // spec 10 §4.2 step 4: the slot's first "<N> <name>.pti", else
            // the legacy instrument_NN.mti.
            if (g_scan.ptiName[slot] >= 0) std::snprintf(src, sizeof(src), "%s/instruments/%.63s", pj_.dir, g_list[g_scan.ptiName[slot]]);
            else if (g_scan.hasMti[slot]) mtiPath(src, sizeof(src), pj_.dir, slot + 1);
            else return true;
        } else {
            if (!g_scan.hasPti[slot]) {
                cost += kJobProbeCost;
                if (!g_scan.hasMti[slot]) return true;
                // spec 10 §3.3 step 1: instrument_NN.mti -> .pti when no .pti exists.
                mtiPath(src, sizeof(src), kWorkspace, slot + 1);
                if (!f.rename(src, path)) {
                    ++pj_.loadErrors;
                    return true;
                }
            }
            std::snprintf(src, sizeof(src), "%s", path);
        }
        cost += kJobSmallCost;
        pj_.done += kJobSmallCost;
        pj_.in = f.open(src, IStorage::kRead);
        if (pj_.in < 0) {
            ++pj_.loadErrors;
            return true;
        }
        // spec 10 §3.3 step 2: zero-filled 0x188-byte header, chain, data offset.
        const int32_t size = f.size(pj_.in);
        uint8_t* hdr = mem_.scratch;
        std::memset(hdr, 0, pti::kHeaderBlockSize);
        const int32_t got = size > 0 ? f.read(pj_.in, hdr, pti::kHeaderBlockSize) : 0;
        if (size < 0 || got < 0) {
            ++pj_.loadErrors;
            return endSlot();
        }
        pj_.fileSize[slot] = uint32_t(size);
        pj_.pos = uint32_t(got);
        pj_.len = fromProject ? uint32_t(size) : uint32_t(got);
        pj_.capOff = pj_.capLen = 0;
        {
            ProfScope d(g_ticks.decode);
            Instrument ins;
            PtiLoadInfo li;
            if (decodePtiHeader(hdr, size_t(got), ins, &li) == FmtStatus::Ok &&
                legacy::ptiSlotAccepted(size_t(size), li.dataOffset)) {   // else: slot cleared
                pj_.kind[slot] = legacy::ptiNeedsWav(size_t(size), li.dataOffset) ? 2 : 1;
                pj_.dataOff[slot] = li.dataOffset;
                instruments[slot] = ins;   // header only; present once its samples are in
                pj_.modes[slot] = ins.playMode;
            }
        }
        if (pj_.kind[slot] == 1) {
            const uint32_t off = pj_.dataOff[slot];
            const uint32_t payload = uint32_t(size) > off ? uint32_t(size) - off : 0;
            const PtiSampleLayout lay = resolvePtiSample(instruments[slot].lengthFrames, payload, true);   // 2.x: stereo kept
            const uint32_t count = streamWouldResident(slot, lay.frames, lay.useStereo);
            if (count > bank.freeSamples()) {
                // Checked before reading (spec 02 §8.2): the slot is cleared.
                instruments[slot].setDefault();
                instrPresent[slot] = false;
                ++pj_.loadErrors;
                pj_.kind[slot] = 0;
                if (!fromProject) return endSlot();
            } else {
                pj_.srcFrames = lay.frames;
                pj_.srcStereo = lay.useStereo;
                pj_.capOff = off;
                pj_.capLen = count * 2;
                pj_.nRanges = 1;
                pj_.ranges[0] = {off, count * 2, 0};
                planStreamRanges(slot, lay.frames, lay.useStereo, off);   // SD streaming: head-only ranges
                if (fromProject) {
                    // Payload bytes already in the header block (short header).
                    for (int r = 0; r < pj_.nRanges; ++r) {
                        const ProjectJob::BankRange& g = pj_.ranges[r];
                        if (!g.len || g.fileOff >= pj_.pos) continue;
                        const uint32_t e = pj_.pos < g.fileOff + g.len ? pj_.pos : g.fileOff + g.len;
                        std::memcpy(bankBytes + g.bankOff, hdr + g.fileOff, e - g.fileOff);
                    }
                } else if (pj_.capLen) {
                    const uint32_t first = pj_.ranges[0].fileOff;
                    const ProjectJob::BankRange& last = pj_.ranges[pj_.nRanges - 1];
                    if (pj_.pos != first && !f.seek(pj_.in, first)) return clearSlot(true);
                    pj_.pos = first;
                    pj_.len = last.fileOff + last.len;
                }
            }
        }
        // Open: the Workspace copy is created in a unit of its own (a FatFs
        // create scans the folder once per "~N" short-name candidate).
        pj_.phase = fromProject ? 6 : 1;
        return false;
    }

    if (pj_.phase == 6) {
        pj_.out = f.open(path, IStorage::kWrite);
        const uint32_t hb = pj_.pos;   // header bytes read in phase 0 (still in scratch)
        if (pj_.out < 0 || (hb && f.write(pj_.out, mem_.scratch, hb) != int32_t(hb))) {
            // No Workspace copy: the slot stays empty (as a failed stock copy).
            ++pj_.loadErrors;
            instruments[slot].setDefault();
            pj_.modes[slot] = instruments[slot].playMode;
            pj_.kind[slot] = 0;
            return endSlot();
        }
        cost += kJobSmallCost;
        pj_.phase = 1;
        return false;
    }

    if (pj_.phase == 1) {
        // Transfer: one aligned chunk per unit.
        bool moved = false;
        if (pj_.pos < pj_.len) {
            moved = true;
            // The bank range holding pos (or the next one after it).
            const ProjectJob::BankRange* in = nullptr;
            const ProjectJob::BankRange* next = nullptr;
            for (int r = 0; r < pj_.nRanges && pj_.capLen; ++r) {
                const ProjectJob::BankRange& g = pj_.ranges[r];
                if (!g.len) continue;
                if (pj_.pos >= g.fileOff && pj_.pos < g.fileOff + g.len) in = &g;
                else if (pj_.pos < g.fileOff && !next) next = &g;
            }
            if (!in && next && pj_.out < 0) {
                // Head-only load (no copy to write): skip to the next range.
                if (!f.seek(pj_.in, next->fileOff)) {
                    ++pj_.loadErrors;
                    pj_.pos = pj_.len;
                    return false;
                }
                pj_.pos = next->fileOff;
                in = next;
                next = nullptr;
            }
            const bool inCap = in != nullptr;
            const uint32_t cs = in ? in->fileOff : 0, ce = in ? in->fileOff + in->len : 0;
            uint32_t end = chunkEnd(pj_.pos, unitBytes(true, pj_.out >= 0));
            if (end > pj_.len) end = pj_.len;
            if (!inCap && next && end > next->fileOff) end = next->fileOff;
            if (inCap && end > ce) end = ce;
            uint32_t want = end - pj_.pos;
            if (!inCap && want > mem_.scratchBytes) want = mem_.scratchBytes;
            uint8_t* d = inCap ? bankBytes + in->bankOff + (pj_.pos - cs) : mem_.scratch;
            int32_t r = f.read(pj_.in, d, want);
            if (r < 0 && f.seek(pj_.in, pj_.pos)) r = f.read(pj_.in, d, want);   // one retry
            if (r < 0) ++pj_.loadErrors;
            const uint32_t got = r > 0 ? uint32_t(r) : 0;
            if (pj_.out >= 0 && got && f.write(pj_.out, d, got) != int32_t(got)) {
                ++pj_.loadErrors;
                f.close(pj_.out);
                pj_.out = -1;
            }
            pj_.pos += got;
            cost += fromProject ? 2 * got : got;
            pj_.done += fromProject ? 2 * got : got;
            if (got < want) {
                // Short file / read error: the rest of the samples reads as
                // zero (as loadInstrumentFile; a half sample counts as missing).
                for (int r = 0; r < pj_.nRanges && pj_.capLen; ++r) {
                    const ProjectJob::BankRange& g = pj_.ranges[r];
                    const uint32_t gs = g.fileOff, ge = g.fileOff + g.len;
                    if (pj_.pos >= ge) continue;
                    const uint32_t z = pj_.pos > gs ? gs + ((pj_.pos - gs) & ~1u) : gs;
                    std::memset(bankBytes + g.bankOff + (z - gs), 0, ge - z);
                }
                pj_.pos = pj_.len;
            }
            if (pj_.pos < pj_.len) return false;
        }
        if (moved && pj_.out >= 0) return false;   // close the copy in the next unit
        if (pj_.out >= 0) g_unitClose = true;
        jobCloseFiles();
        if (pj_.kind[slot] == 1) {
            ProfScope c(g_ticks.commit);
            if (!kLittleEndian) {
                // Little-endian payload -> native int16 in place (spec 02 §6.3).
                int16_t* s = bank.tail();
                for (uint32_t k = 0; k < pj_.capLen / 2; ++k) s[k] = rdI16(bankBytes + 2 * k);
            }
            Instrument ins = instruments[slot];
            ins.lengthFrames = pj_.srcFrames;
            ins.stereo = pj_.srcStereo;
            if (pj_.streamThis) {
                // Head-only: the bank holds [L head | L loop][R head | R loop].
                const uint32_t perCh = pj_.resHead + pj_.resLoopFrames;
                bank.commitTail(slot, perCh, ins.stereo);
                bank.setPartial(slot, pj_.fullFrames);
                ins.lengthFrames = pj_.fullFrames;
                instruments[slot] = ins;
                pj_.phase = 7;   // bind the stream (a unit of its own)
                return false;
            }
            bank.commitTail(slot, ins.lengthFrames, ins.stereo);
            instruments[slot] = ins;
            instrPresent[slot] = true;
            pj_.phase = 0;
            return true;
        }
        if (pj_.kind[slot] != 2) return endSlot();
        pj_.phase = 3;
        return false;
    }

    // SD streaming: bind the head-only slot's stream (FAT chain walk + self-check).
    if (pj_.phase == 7) {
        cost += kJobSmallCost;
        if (bindStreamSlot(slot, path)) {
            instrPresent[slot] = true;
            pj_.streamThis = false;
            return endSlot();
        }
        // Fallback: the slot reloads whole from its Workspace copy.
        bank.remove(slot);
        pj_.noStream[slot / 8] = uint8_t(pj_.noStream[slot / 8] | (1u << (slot % 8)));
        pj_.streamThis = false;
        pj_.slotFromWorkspace = true;
        pj_.phase = 0;
        return false;
    }

    // Legacy slot: import <wavBase>/samples/instrNN.wav (spec 10 §3.3 steps 3-6).
    if (pj_.phase == 3) {
        char wavPath[340];
        legacyWavPath(wavPath, sizeof(wavPath), pj_.wavBase, slot + 1);
        cost += kJobSmallCost;
        pj_.in = f.open(wavPath, IStorage::kRead);
        // spec 10 §7 unknown 3: a missing WAV clears the slot.
        if (pj_.in < 0) return clearSlot(f.exists(wavPath));
        WavInfo info;
        StorageReader r(f, pj_.in);
        if (!wavParseHeader(r, info)) return clearSlot(false);
        WavImportOptions opt;
        if (pj_.wav.begin(info, opt, bank.tail(), bank.freeSamples()) != FmtStatus::Ok) return clearSlot(true);
        pj_.len = info.dataSize;
        pj_.pos = 0;
        pj_.capOff = info.dataOffset;   // file offset of the data (chunk alignment)
        pj_.phase = 4;
        return false;
    }
    if (pj_.phase == 4) {
        const uint32_t fo = pj_.capOff + pj_.pos;
        uint32_t want = pj_.len - pj_.pos;
        if (want > chunkEnd(fo, unitBytes(true, false)) - fo) want = chunkEnd(fo, unitBytes(true, false)) - fo;
        uint32_t maxb = 0;
        uint8_t* direct = pj_.wav.directBuffer(maxb);
        int32_t got = 0;
        if (direct) {
            // 16-bit mono 44.1 kHz: the data bytes are the samples.
            if (want > maxb) want = maxb;
            got = want ? f.read(pj_.in, direct, want) : 0;
            if (got > 0) pj_.wav.directCommit(uint32_t(got));
        } else {
            if (want > mem_.scratchBytes) want = mem_.scratchBytes;
            got = want ? f.read(pj_.in, mem_.scratch, want) : 0;
            if (got > 0) {
                ProfScope d(g_ticks.decode);
                pj_.wav.feed(mem_.scratch, size_t(got));
            }
        }
        if (got > 0) {
            pj_.pos += uint32_t(got);
            cost += uint32_t(got);
            pj_.done += uint32_t(got);
        }
        if (got > 0 && pj_.pos < pj_.len && !pj_.wav.complete()) return false;
        // End of data (a missing tail is zero-filled by the importer).
        jobCloseFiles();
        ProfScope c(g_ticks.commit);
        const uint32_t frames = pj_.wav.finish();
        Instrument ins = instruments[slot];
        legacy::applyImportedLength(ins, frames, false);
        bank.commitTail(slot, frames, false);
        ins.lengthFrames = bank.frames(slot);
        instruments[slot] = ins;
        instrPresent[slot] = true;
        pj_.phase = 5;
        pj_.wphase = 0;
        return false;
    }
    // SPEC GAP: stock writes the payload now and the v9 header at the next
    // autosave (dirty flag); both are written here at once.
    bool failed = false;
    if (!jobPtiWriteUnit(slot, path, cost, &failed)) return false;
    if (failed) ++pj_.loadErrors;
    pj_.phase = 0;
    return true;
}

namespace {
// Wipe batch: names of one folder listing, kWipeEntry bytes each in the
// scratch buffer (byte 0 = is-directory).
constexpr uint32_t kWipeEntry = 256;
struct WipeList {
    uint8_t* buf;
    uint32_t cap, count;
};
bool wipeCollectFn(const IStorage::DirEntry& e, void* ctx) {
    auto* w = static_cast<WipeList*>(ctx);
    if (!std::strcmp(e.name, ".") || !std::strcmp(e.name, "..")) return true;
    if (w->count >= w->cap) return false;
    uint8_t* d = w->buf + size_t(w->count++) * kWipeEntry;
    d[0] = e.isDir ? 1 : 0;
    std::snprintf(reinterpret_cast<char*>(d + 1), kWipeEntry - 1, "%s", e.name);
    return true;
}
}  // namespace

// One step of the recursive Workspace delete. 0 = done. A folder is listed
// once per batch of entries (not once per entry: every listing walks the
// folder from its first slot, deleted ones included).
int App::jobWipeUnit(uint32_t& cost) {
    IStorage& f = g_pfs;
    if (pj_.idx >= int(pj_.wipeCount)) {
        WipeList wl{mem_.scratch, mem_.scratchBytes / kWipeEntry, 0};
        cost += kJobProbeCost;
        if (f.listDir(pj_.wipe, wipeCollectFn, &wl) < 0) return 0;   // gone
        if (wl.count == 0) {
            cost += kJobSmallCost;
            const bool removed = f.remove(pj_.wipe);
            if (!removed || std::strlen(pj_.wipe) <= pj_.wipeRootLen) return 0;
            *std::strrchr(pj_.wipe, '/') = 0;
            pj_.wipeCount = 0;
            pj_.idx = 0;
            return 1;
        }
        pj_.wipeCount = wl.count;
        pj_.idx = 0;
    }
    const uint8_t* e = mem_.scratch + size_t(pj_.idx++) * kWipeEntry;
    char child[sizeof(pj_.wipe)];
    if (std::snprintf(child, sizeof(child), "%s/%s", pj_.wipe, reinterpret_cast<const char*>(e + 1)) >= int(sizeof(child)))
        return 0;
    if (e[0]) {
        std::memcpy(pj_.wipe, child, sizeof(child));
        pj_.wipeCount = 0;
        pj_.idx = 0;
        return 1;
    }
    cost += kJobSmallCost;
    // SPEC GAP: a file that cannot be deleted stops the wipe; the copies
    // below overwrite what they need.
    return f.remove(child) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Tracker Mini pattern and patch files (spec 21 §4, §7; docs/MINI_PLAN.md §1)
//
// Patterns are 16-track files (0x3030): tracks 0..11 go to the resident
// stock store, tracks 12..15 to the ext-track pool when they hold data.
// Autosave writes them atomically (.tmp, remove, rename; an orphan .tmp
// with a valid CRC is restored on load). Synth slot s has its patch in
// <folder>/N.patch (spec 21 §2), written the same way.

namespace {
inline void keepBit(uint8_t* keep, int num, int e, bool on) {
    if (on) keep[num] = uint8_t(keep[num] | (1u << e));
    else keep[num] = uint8_t(keep[num] & ~(1u << e));
}
}  // namespace

// Pattern file image (any layout; spec 21 §4.2 upgrades) -> store. `out`
// (may be null) receives the 0x3030 image of everything decoded, also the
// ext tracks that did not fit a full pool (they are remembered in extKeep_
// and kept from the Workspace file by the autosave). *upgrade: the file
// was not a current 16-track file. False = the pool was full.
bool App::loadPatternBytes(int num, const uint8_t* file, uint32_t len, uint8_t* out, bool* upgrade) {
    PatternUpgradeContext ctx;
    ctx.instrumentPlayModes = pj_.modes;
    ctx.beatSliceReference = project.beatSliceReference();
    Track* ext = reinterpret_cast<Track*>(mem_.scratch + kExtOff);
    PatternLoadInfo li;
    clearExtTracks(num);
    PatternStock& st = storedStock(num);
    decodePattern16(file, len, st, ext, &ctx, &li);
    bool fit = true;
    for (int e = 0; e < kExtTrackCount; ++e) {
        if (extTrackIsDefault(ext[e], st.tracks[0].lastStep) || ext_.alloc(num, e, ext[e])) continue;
        keepBit(extKeep_, num, e, true);
        fit = false;
    }
    if (!fit && !loadNotice_[0]) std::snprintf(loadNotice_, sizeof(loadNotice_), "Track memory full");
    if (upgrade) *upgrade = li.sizeField != mtp::kMiniFileSize || li.upgraded;
    if (out) {
        const Track* ep[kExtTrackCount] = {&ext[0], &ext[1], &ext[2], &ext[3]};
        encodePattern16(st, ep, out);
    }
    return fit;
}

// Autosave: Workspace/patterns/pattern_NN.mtp as 0x3030 from the store.
bool App::savePatternUnit(int num) {
    IStorage& f = g_pfs;
    char path[96];
    patternFilePath(path, sizeof(path), kWorkspace, num);
    const Track* ep[kExtTrackCount];
    for (int e = 0; e < kExtTrackCount; ++e) ep[e] = ext_.find(num, e);
    if (extKeep_[num]) {
        // Tracks that did not fit the pool at load come from the file.
        uint32_t got = 0;
        Track* file = reinterpret_cast<Track*>(mem_.scratch + kPatchOff);
        static_assert(kPatchOff + sizeof(Track) * kTrackCount <= 0x10000, "keep tracks fit the scratch");
        if (readWholeFile(f, path, mem_.scratch, mtp::kMiniFileSize, &got) == FmtStatus::Ok) {
            decodePattern16(mem_.scratch, got, file);
            for (int e = 0; e < kExtTrackCount; ++e)
                if (!ep[e] && ((extKeep_[num] >> e) & 1)) ep[e] = &file[kStockTrackCount + e];
        }
    }
    uint8_t* out = mem_.scratch + kOutOff;
    const size_t n = encodePattern16(storedStock(num), ep, out);
    return writeAtomic(f, path, out, uint32_t(n));
}

// Patch of slot s: `buf` (a .patch file, `len` bytes) when haveFile, else
// (or when it is refused) the library Patches/<ENGINE>/<record name>.patch,
// else the engine's Init patch (spec 21 §7.2 slot load order). A patch of
// another engine is refused silently, a damaged or too large file with a
// notice. True = a file was used.
bool App::resolvePatch(int s, const uint8_t* buf, uint32_t len, bool haveFile) {
    engine.synth().patchChanged(s);   // the audio side re-reads the patch next block
    const SynthSlotRecord r = project.synthSlot(s);
    const SynthModel model = SynthModel(r.model < uint32_t(kSynthModelCount) ? r.model : 0);
    const int32_t engine = synthEngineId(model);
    auto* tmp = reinterpret_cast<SynthPatch*>(mem_.scratch + kPatchOff);
    patchDirty_[s] = false;
    if (haveFile) {
        const PatchStatus st = len > kPatchMaxFileSize ? PatchStatus::kTooLarge : decodePatch(buf, len, tmp);
        if (st == PatchStatus::kOk && tmp->engine == engine) {
            patches_[s] = *tmp;
            patchSrc_[s] = PatchSource::File;
            return true;
        }
        if (st != PatchStatus::kOk && !loadNotice_[0])
            std::snprintf(loadNotice_, sizeof(loadNotice_), st == PatchStatus::kTooLarge ? "Synth %d patch is too large" : "Synth %d patch is damaged", s + 1);
        if (st != PatchStatus::kOk) g_patchKept = uint8_t(g_patchKept | (1u << s));
    }
    if (r.patchName[0]) {
        char path[96];
        std::snprintf(path, sizeof(path), "Patches/%s/%s.patch", synthModelName(model), r.patchName);
        uint32_t got = 0;
        if (readWholeFile(fs(), path, mem_.scratch, kPatchMaxFileSize + 1, &got) == FmtStatus::Ok && got <= kPatchMaxFileSize &&
            decodePatch(mem_.scratch, got, tmp) == PatchStatus::kOk && tmp->engine == engine) {
            patches_[s] = *tmp;
            patchSrc_[s] = PatchSource::Library;
            return true;
        }
    }
    patchInit(engine, &patches_[s]);
    patchSrc_[s] = PatchSource::Init;
    return false;
}

// CRC-32 of the slot's patch as savePatch would write it (0: not encodable).
uint32_t App::patchCrc(int s) {
    uint8_t* out = mem_.scratch + kOutOff;
    size_t n = 0;
    if (encodePatch(patches_[s], out, kPatchMaxFileSize, &n) != PatchStatus::kOk) return 0;
    return crc32(out, n);
}

// The slot still plays the fallback of a refused project N.patch (see
// g_patchKept): Save keeps the project's file.
bool App::patchKeptOriginal(int s) {
    return ((g_patchKept >> s) & 1) && patchCrc(s) == g_patchKeptCrc[s];
}

// <dir>/N.patch from the slot's patch (atomic).
bool App::savePatch(int s, const char* dir) {
    uint8_t* out = mem_.scratch + kOutOff;
    size_t n = 0;
    if (encodePatch(patches_[s], out, kPatchMaxFileSize, &n) != PatchStatus::kOk) return false;
    char path[340];
    patchPath(path, sizeof(path), dir, s);
    return writePatchAtomic(fs(), path, out, uint32_t(n), out + kPatchMaxFileSize);
}

// kLPatch: slot s. Open copies <project>/N.patch to Workspace/N.patch and
// decodes the copied bytes; a Workspace load reads Workspace/N.patch. An
// orphan N.patch.tmp that is complete stands in for a missing file (and is
// renamed back in the Workspace); any other leftover .tmp is deleted.
void App::loadPatchUnit(int s, uint32_t& cost) {
    IStorage& f = g_pfs;
    const char* dir = pj_.fromProject ? pj_.dir : kWorkspace;
    char src[340], tmp[340], dst[340];
    patchPath(src, sizeof(src), dir, s);
    patchPath(tmp, sizeof(tmp), dir, s, ".tmp");
    patchPath(dst, sizeof(dst), kWorkspace, s);
    uint8_t* b = mem_.scratch;
    constexpr uint32_t kCap = kPatchMaxFileSize + 1;   // one byte more: "too large" is seen
    uint32_t len = 0;
    bool have = false;
    if (g_scan.patch[s]) {
        if (pj_.fromProject) {
            have = copySmallFile(f, src, dst, b, kExtOff, kCap, len, cost) == 1;
            if (len > kCap) len = kCap;
        } else {
            have = readWholeFile(f, src, b, kCap, &len) == FmtStatus::Ok;
            if (!have) have = readWholeFile(f, src, b, kCap, &len) == FmtStatus::Ok;   // one retry
        }
        if (!have) ++pj_.loadErrors;
        if (g_scan.patchTmp[s] && !pj_.fromProject) f.remove(tmp);   // the old file is intact
    } else if (g_scan.patchTmp[s]) {
        auto* chk = reinterpret_cast<SynthPatch*>(mem_.scratch + kPatchOff);
        if (readWholeFile(f, tmp, b, kCap, &len) == FmtStatus::Ok && len <= kPatchMaxFileSize && completePatch(b, len, chk, mem_.scratch + kOutOff)) {
            have = pj_.fromProject ? writeWholeFile(f, dst, b, len) == FmtStatus::Ok : f.rename(tmp, src);
        } else if (!pj_.fromProject) {
            f.remove(tmp);
        }
    }
    g_patchKept = uint8_t(g_patchKept & ~(1u << s));
    resolvePatch(s, b, len, have);
    if ((g_patchKept >> s) & 1) g_patchKeptCrc[s] = patchCrc(s);
    if ((pj_.slotReset >> s) & 1) {
        // spec 21 §3.3: a record reset at load takes its default macro values
        // from the bipolar flags of the patch then loaded (63 bipolar, else
        // 0). The project.mt check ran before the patch existed.
        SynthSlotRecord r = project.synthSlot(s);
        for (uint32_t k = 0; k < mt::kSynthMacroCount; ++k) r.macros[k] = audio::synthMacroDefault(patches_[s].macros[k].bipolar);
        project.setSynthSlot(s, r);
    }
    cost += kJobSmallCost + len;
}

// One notice per load (Ui has one notice slot): the first problem.
void App::loadNotice() {
    if (!loadNotice_[0]) synthUnavailableNotice(loadNotice_, sizeof(loadNotice_));   // synth_slots.cpp
    // SD streaming (§2.7): with nothing else to say, how many stream.
    if (const int n = streamedCount(); !loadNotice_[0] && n > 0)
        std::snprintf(loadNotice_, sizeof(loadNotice_), n == 1 ? "1 instrument streams from the card" :
                      "%d instruments stream from the card", n);
    if (loadNotice_[0]) ui.notify(loadNotice_, 4000);
}

void App::jobBeginLoad() {
    pj_.stage = kLProject;
    pj_.idx = 0;
    pj_.phase = pj_.wphase = 0;
}

// One unit of the current job. Returns false when the job has ended.
bool App::jobUnit(uint32_t& cost) {
    IStorage& f = g_pfs;
    char src[340], dst[340];
    if (pj_.op == JobOp::External) {
        const int r = pj_.extUnit(pj_.extCtx, cost);
        if (r > 0) return true;
        if (r < 0) jobFail();
        else jobEnd();
        return false;
    }
    switch (pj_.stage) {
        // ---- workspace load (op 8 / tail of op 9; spec 05 E3 stage 1) ----
        case kLProject: {
            seq.stop();
            allVoicesOff();
            projectFilePath(src, sizeof(src), kWorkspace);
            Project p;
            ProjectLoadInfo pinfo;
            // spec 10 §1: structure 1..16 upgraded in RAM; type != 0 / version 0 fail.
            if (loadProjectFile(f, src, p, &pinfo) != FmtStatus::Ok) {
                jobFail();
                return false;
            }
            project = p;
            pj_.sizeMismatch = pinfo.sizeMismatch;
            pj_.slotReset = uint8_t(pj_.slotReset | pinfo.synthSlotsReset);   // Open: also the source file's check
            std::memset(extKeep_, 0, sizeof(extKeep_));
            std::memset(g_upgrade, 0, sizeof(g_upgrade));
            loadNotice_[0] = 0;
            for (int s = 0; s < kSynthSlotCount; ++s) {
                patchDirty_[s] = false;
                patchSrc_[s] = PatchSource::None;
            }
            engineDirty_ = true;
            applyStreamOption();   // clears the bank; carves the stream rings when on
            for (int i = 0; i < kSampleInstrumentCount; ++i) {
                instruments[i].setDefault();
                instrPresent[i] = false;
                pj_.kind[i] = 0;
            }
            std::memset(present_, 0, sizeof(present_));
            cost += kJobSmallCost;
            pj_.done += kJobSmallCost;
            pj_.stage = kLSlots;
            pj_.idx = 0;
            pj_.phase = pj_.wphase = 0;
            return true;
        }
        case kLSlots: {
            const int i = pj_.idx;
            if (i >= kSampleInstrumentCount) {
                pj_.stage = kLNames;
                return true;
            }
            if (jobSamplesUnit(i, cost)) ++pj_.idx;
            return true;
        }
        case kLNames:
            patternNamesPath(dst, sizeof(dst), kWorkspace);
            if (pj_.fromProject) {
                // spec 10 §2.3 / §4.2 step 6: copy patternsMetadata, or create
                // a zeroed one; the copied bytes are decoded as loaded.
                patternNamesPath(src, sizeof(src), pj_.dir);
                if (!g_scan.namesFile) {
                    names.clear();
                    savePatternNamesFile(f, dst, names);
                } else {
                    uint32_t size = 0;
                    const uint32_t cap = mem_.scratchBytes;
                    if (cap >= 2 * pamd::kFileSize &&
                        copySmallFile(f, src, dst, mem_.scratch, cap, pamd::kFileSize, size, cost)) {
                        ProfScope d(g_ticks.decode);
                        const uint32_t got = size < pamd::kFileSize ? size : pamd::kFileSize;
                        std::memset(mem_.scratch + got, 0, pamd::kFileSize - got);
                        decodePatternNames(mem_.scratch, got, names);
                    } else {
                        ++pj_.loadErrors;
                        loadPatternNamesFile(f, dst, names);
                    }
                }
            } else if (g_scan.namesFile) {
                loadPatternNamesFile(f, dst, names);
            } else {
                names.clear();   // missing file (spec 02 §5)
            }
            cost += kJobSmallCost;
            pj_.stage = kLPatch;
            pj_.idx = 0;
            return true;
        case kLPatch:
            // Synth slot patches (spec 21 §7.2), after project.mt (records).
            if (pj_.idx < kSynthSlotCount) {
                loadPatchUnit(pj_.idx++, cost);
                return true;
            }
            pj_.stage = kLPatterns;
            pj_.idx = 1;
            pj_.phase = 0;
            return true;
        case kLPatterns: {
            const int n = pj_.idx++;
            if (n > kPatternCount) {
                pj_.stage = kLFinish;
                pj_.idx = 0;
                pj_.phase = 0;
                return true;
            }
            patternFilePath(dst, sizeof(dst), kWorkspace, n);
            if (!bitOf(g_scan.patBits, n)) {
                cost += kJobProbeCost;
                defaultStored(n);
                // An interrupted autosave can leave only pattern_NN.mtp.tmp:
                // complete (size + CRC) = the new file, renamed back.
                if (!pj_.fromProject && bitOf(g_scan.patTmp, n)) {
                    std::snprintf(src, sizeof(src), "%s/patterns/pattern_%02d.mtp.tmp", kWorkspace, n);
                    uint32_t got = 0;
                    if (readWholeFile(f, src, mem_.scratch, mtp::kMiniFileSize, &got) == FmtStatus::Ok &&
                        completeMiniPattern(mem_.scratch, got) && f.rename(src, dst)) {
                        ProfScope d(g_ticks.decode);
                        loadPatternBytes(n, mem_.scratch, got, nullptr, nullptr);
                        setPresent(n, true);
                    } else {
                        f.remove(src);
                    }
                    cost += kJobSmallCost + got;
                }
                return true;
            }
            if (!pj_.fromProject && bitOf(g_scan.patTmp, n)) {
                std::snprintf(src, sizeof(src), "%s/patterns/pattern_%02d.mtp.tmp", kWorkspace, n);   // the file is intact: drop the leftover
                f.remove(src);
            }
            // 2.x (Tracker Mini, spec 21 §4.2): any layout loads with the
            // stock upgrades; Open writes the Workspace copy as 0x3030 (a
            // 1.9.x project becomes a Mini project), a Workspace load marks
            // older files for the autosave to rewrite.
            FmtStatus st = FmtStatus::IoError;
            if (pj_.fromProject) {
                // Step 3, in three units: read + decode the source, create the
                // copy (a FatFs create scans the folder per "~N" candidate),
                // write the 16-track image.
                patternFilePath(src, sizeof(src), pj_.dir, n);
                if (pj_.phase == 0) {
                    pj_.in = f.open(src, IStorage::kRead);
                    const int32_t sz = pj_.in >= 0 ? f.size(pj_.in) : -1;
                    if (sz >= 0) {
                        const uint32_t want = uint32_t(sz) < mtp::kMiniFileSize ? uint32_t(sz) : mtp::kMiniFileSize;
                        int32_t r = want ? f.read(pj_.in, mem_.scratch, want) : 0;
                        if (r != int32_t(want) && f.seek(pj_.in, 0)) r = want ? f.read(pj_.in, mem_.scratch, want) : 0;
                        jobCloseFiles();
                        cost += kJobSmallCost + want;
                        if (r == int32_t(want)) {
                            ProfScope d(g_ticks.decode);
                            loadPatternBytes(n, mem_.scratch, want, mem_.scratch + kOutOff, nullptr);
                            pj_.phase = 1;
                            --pj_.idx;   // same pattern, next unit
                            return true;
                        }
                    }
                    jobCloseFiles();
                } else if (pj_.phase == 1) {
                    pj_.out = f.open(dst, IStorage::kWrite);
                    cost += kJobSmallCost;
                    if (pj_.out >= 0) {
                        pj_.phase = 2;
                        --pj_.idx;
                        return true;
                    }
                } else if (pj_.phase == 2) {
                    const bool wrote = f.write(pj_.out, mem_.scratch + kOutOff, mtp::kMiniFileSize) == int32_t(mtp::kMiniFileSize);
                    jobCloseFiles();
                    cost += mtp::kMiniFileSize;
                    if (wrote) st = FmtStatus::Ok;
                }
                pj_.phase = 0;
            } else {
                uint32_t got = 0;
                st = readWholeFile(f, dst, mem_.scratch, mtp::kMiniFileSize, &got);
                if (st == FmtStatus::IoError) st = readWholeFile(f, dst, mem_.scratch, mtp::kMiniFileSize, &got);   // one retry
                if (st == FmtStatus::Ok) {
                    ProfScope d(g_ticks.decode);
                    bool upgrade = false;
                    loadPatternBytes(n, mem_.scratch, got, nullptr, &upgrade);
                    if (upgrade) setBit(g_upgrade, n, true);
                }
            }
            if (st == FmtStatus::Ok) {
                setPresent(n, true);
            } else {
                // The file is there but unreadable: report it instead of
                // silently showing an empty pattern (field bug #1).
                defaultStored(n);
                ++pj_.loadErrors;
            }
            cost += kJobSmallCost + mtp::kMiniFileSize;
            pj_.done += kJobSmallCost + mtp::kLegacyFileSize;
            return true;
        }
        case kLFinish: {
            uint8_t cur = project.currentPattern();
            if (cur < 1) cur = 1;
            project.setCurrentPattern(cur);
            loadPattern(cur, pattern(), true);
            attachSequencer();
            undo.clear();
            std::memcpy(patDirty_, g_upgrade, sizeof(patDirty_));   // older layouts -> 0x3030
            projectDirty_ = namesDirty_ = false;
            for (int i = 0; i < kSampleInstrumentCount; ++i) instrHeaderDirty_[i] = instrSampleDirty_[i] = false;
            unsaved_ = false;
            // A slot record reset at load: the autosave writes the corrected
            // record (with its patch's macro defaults) to Workspace/project.mt.
            if (pj_.slotReset) projectDirty_ = true;
            // spec 05 appendix E2: an empty folder marks a never-saved (new) project.
            neverSaved_ = !project.name[0] || !project.folder[0] || !projectExists(project.folder, project.name);
            if (pj_.sizeMismatch) {
                // spec 10 §1.3 / §4.4: a legacy Workspace/project.mt loads as
                // "Unsaved Project" with status flags 3 (never saved + unsaved).
                neverSaved_ = true;
                unsaved_ = true;
            }
            if (pj_.opened) {
                // spec 05 appendix E2: opening sets the status flags to 0.
                neverSaved_ = false;
                unsaved_ = false;
            }
            refreshPadNotes();
            loadNotice();
            ui.dirty();
            jobEnd();
            return false;
        }

        // ---- open project (op 9 stage 3; spec 10 §4.2) -------------------
        case kOWipe:
            // Step 0: wipe and recreate the Workspace.
            if (jobWipeUnit(cost) == 0) {
                pj_.stage = kODirs;
                pj_.idx = 0;
            }
            return true;
        case kODirs: {
            // One folder per unit: FatFs clears a new folder's whole cluster.
            static const char* const kDirs[] = {kWorkspace, "Workspace/instruments", "Workspace/patterns",
                                                "Workspace/samples"};
            if (pj_.idx < 4) {
                f.mkdir(kDirs[pj_.idx++]);
                cost += kJobSmallCost;
                return true;
            }
            // spec 10 §1.5 / §4.2 step 2: Workspace/project.mt as v17 with the
            // selected name and parent folder (also for legacy files).
            projectFilePath(dst, sizeof(dst), kWorkspace);
            if (saveProjectFile(f, dst, pj_.proj) != FmtStatus::Ok) {
                jobFail();
                return false;
            }
            cost += kJobSmallCost;
            // Steps 3-6 run inside the load (instruments, names, patterns).
            jobBeginLoad();
            return true;
        }

        case kABegin:
            lastAutosave_ = millis();
            commitCurrentPattern();
            pj_.stage = kAPatterns;
            pj_.idx = 1;
            return true;
        case kAPatterns: {
            int n = pj_.idx;
            while (n <= kPatternCount && !((patDirty_[n >> 3] >> (n & 7)) & 1)) ++n;
            if (n > kPatternCount) {
                pj_.stage = kANames;
                return true;
            }
            pj_.idx = n + 1;
            patDirty_[n >> 3] &= uint8_t(~(1u << (n & 7)));
            // A kept track (pool full at load) the store now holds is the
            // store's from here on.
            for (int e = 0; e < kExtTrackCount; ++e)
                if (ext_.find(n, e)) extKeep_[n] = uint8_t(extKeep_[n] & ~(1u << e));
            compactExtTracks(n);
            if (!savePatternUnit(n)) {
                patDirty_[n >> 3] |= uint8_t(1u << (n & 7));
                ++pj_.loadErrors;
            }
            cost += kJobSmallCost + mtp::kMiniFileSize;
            return true;
        }
        case kANames:
            if (namesDirty_) {
                patternNamesPath(dst, sizeof(dst), kWorkspace);
                namesDirty_ = false;
                if (savePatternNamesFile(f, dst, names) != FmtStatus::Ok) {
                    namesDirty_ = true;
                    ++pj_.loadErrors;
                }
                cost += kJobSmallCost;
            }
            pj_.stage = kAInstr;
            pj_.idx = 0;
            return true;
        case kAInstr: {
            int i = pj_.idx;
            if (pj_.wphase == 0) {
                while (i < kSampleInstrumentCount && !instrHeaderDirty_[i] && !instrSampleDirty_[i]) ++i;
                pj_.idx = i;
                if (i >= kSampleInstrumentCount) {
                    pj_.stage = kAProject;
                    return true;
                }
                pj_.sampleDirty = instrSampleDirty_[i];
                // SD streaming: the bank holds only the head of a streamed
                // slot; its .pti payload is never rewritten (header only).
                if (streamed(i)) pj_.sampleDirty = false;
                instrHeaderDirty_[i] = instrSampleDirty_[i] = false;
            }
            workspaceInstrumentPath(dst, sizeof(dst), kWorkspace, i + 1);
            if (pj_.wphase == 0 && !instrPresent[i]) {
                if (f.exists(dst)) f.remove(dst);
                cost += kJobSmallCost;
                ++pj_.idx;
                return true;
            }
            if (pj_.wphase == 0 && !pj_.sampleDirty && f.exists(dst)) {
                if (saveInstrumentHeader(f, dst, instruments[i]) != FmtStatus::Ok) {
                    instrHeaderDirty_[i] = true;
                    ++pj_.loadErrors;
                }
                cost += kJobSmallCost;
                ++pj_.idx;
                return true;
            }
            bool failed = false;
            if (jobPtiWriteUnit(i, dst, cost, &failed)) {
                if (failed) {
                    instrSampleDirty_[i] = true;
                    ++pj_.loadErrors;
                }
                ++pj_.idx;
            }
            return true;
        }
        case kAProject: {
            if (projectDirty_) {
                project.setCurrentPattern(patternNumber());
                projectFilePath(dst, sizeof(dst), kWorkspace);
                projectDirty_ = false;
                if (saveProjectFile(f, dst, project) != FmtStatus::Ok) {
                    projectDirty_ = true;
                    ++pj_.loadErrors;
                }
                cost += kJobSmallCost;
            }
            pj_.stage = kAPatch;
            pj_.idx = 0;
            return true;
        }
        case kAPatch: {
            // Workspace/N.patch of each slot whose patch changed.
            int k = pj_.idx;
            while (k < kSynthSlotCount && !patchDirty_[k]) ++k;
            if (k >= kSynthSlotCount) {
                pj_.stage = kAFinish;
                return true;
            }
            pj_.idx = k + 1;
            patchDirty_[k] = false;
            if (!savePatch(k, kWorkspace)) {
                patchDirty_[k] = true;
                ++pj_.loadErrors;
            }
            cost += kJobSmallCost + kPatchMaxFileSize / 4;
            return true;
        }
        case kAFinish:
            if (pj_.op == JobOp::Save) {
                // spec 05 appendix E3: op 0x0d continues with stage 4.
                if (pj_.loadErrors) {
                    jobFail();
                    return false;
                }
                pj_.stage = kSDirs;
                return true;
            }
            jobEnd();
            return false;

        // ---- copy Workspace -> "<folder>/<name>" (stage 4; spec 10 §4.3) ----
        case kSDirs: {
            // Create every level of the folder path.
            char folder[sizeof(pj_.dir)];
            std::snprintf(folder, sizeof(folder), "%s", pj_.dir);
            for (char* p = folder; *p; ++p) {
                if (*p != '/' || p == folder) continue;
                *p = 0;
                f.mkdir(folder);
                *p = '/';
            }
            if (!f.mkdir(pj_.dir)) {
                jobFail();
                return false;
            }
            // spec 10 §4.3 step 0: patterns/ and instruments/. OpenTracker
            // deviation (README): the project's samples/ folder is NOT deleted.
            std::snprintf(dst, sizeof(dst), "%s/patterns", pj_.dir);
            f.mkdir(dst);
            std::snprintf(dst, sizeof(dst), "%s/instruments", pj_.dir);
            f.mkdir(dst);
            projectFilePath(dst, sizeof(dst), pj_.dir);
            if (saveProjectFile(f, dst, project) != FmtStatus::Ok) ++pj_.loadErrors;
            cost += 4 * kJobSmallCost;
            pj_.stage = kSPatterns;
            pj_.idx = 1;
            return true;
        }
        case kSPatterns: {
            // spec 10 §2.3: patterns are copied raw (unedited legacy ones stay legacy).
            const int n = pj_.idx;
            if (n > kPatternCount) {
                pj_.stage = kSNames;
                return true;
            }
            patternFilePath(src, sizeof(src), kWorkspace, n);
            patternFilePath(dst, sizeof(dst), pj_.dir, n);
            if (pj_.phase == 0 && !(patternPresent(n) && f.exists(src))) {
                cost += kJobProbeCost;
                if (f.exists(dst)) {
                    f.remove(dst);
                    cost += kJobSmallCost;
                }
                ++pj_.idx;
                return true;
            }
            bool failed = false;
            if (jobCopyUnit(src, dst, cost, &failed)) {
                if (failed) ++pj_.loadErrors;
                ++pj_.idx;
            }
            return true;
        }
        case kSNames: {
            patternNamesPath(src, sizeof(src), kWorkspace);
            patternNamesPath(dst, sizeof(dst), pj_.dir);
            if (pj_.phase == 0 && !f.exists(src)) {
                if (f.exists(dst)) f.remove(dst);
                cost += kJobSmallCost;
                pj_.stage = kSCleanList;
                return true;
            }
            bool failed = false;
            if (jobCopyUnit(src, dst, cost, &failed)) {
                if (failed) ++pj_.loadErrors;
                pj_.stage = kSCleanList;
            }
            return true;
        }
        case kSCleanList: {
            // spec 10 §4.3 step 0 "empty instruments/": stale "NN <old name>.pti"
            // files (renamed or deleted instruments) and legacy instrument_NN.mti
            // headers are removed. SPEC GAP: other files there are kept.
            NameList nl{g_list, int(sizeof(g_list) / sizeof(g_list[0])), 0, false};
            std::snprintf(dst, sizeof(dst), "%s/instruments", pj_.dir);
            f.listDir(dst, collectFn, &nl);
            pj_.listCount = nl.count;
            cost += kJobSmallCost;
            pj_.stage = kSClean;
            pj_.idx = 0;
            return true;
        }
        case kSClean: {
            const int k = pj_.idx++;
            if (k >= pj_.listCount) {
                pj_.stage = kSInstr;
                pj_.idx = 0;
                return true;
            }
            if (!endsWithNoCase(g_list[k], ".pti") && !endsWithNoCase(g_list[k], ".mti")) return true;
            std::snprintf(dst, sizeof(dst), "%s/instruments/%.63s", pj_.dir, g_list[k]);
            f.remove(dst);
            cost += kJobSmallCost;
            return true;
        }
        case kSInstr: {
            const int i = pj_.idx;
            if (i >= kSampleInstrumentCount) {
                pj_.stage = kSPatch;
                pj_.idx = 0;
                pj_.phase = 0;
                return true;
            }
            if (!instrPresent[i]) {
                ++pj_.idx;
                return true;
            }
            workspaceInstrumentPath(src, sizeof(src), kWorkspace, i + 1);
            savedInstrumentPath(dst, sizeof(dst), pj_.dir, i + 1, instruments[i].name[0] ? instruments[i].name : "instrument");
            bool failed = false;
            if (jobCopyUnit(src, dst, cost, &failed)) {
                if (failed) ++pj_.loadErrors;
                ++pj_.idx;
            }
            return true;
        }
        case kSPatch:
            // Save writes all three slot patches (spec 21 §2), except a
            // project patch that was refused at load and is still replaced
            // by its fallback: the project's file stays as it is (a new
            // folder gets the original bytes from the Workspace).
            if (pj_.idx < kSynthSlotCount) {
                const int s = pj_.idx;
                if (pj_.phase != 0 || patchKeptOriginal(s)) {
                    patchPath(dst, sizeof(dst), pj_.dir, s);
                    patchPath(src, sizeof(src), kWorkspace, s);
                    if (pj_.keptPatch < 0) pj_.keptPatch = int8_t(s);
                    if (pj_.phase == 0 && (f.exists(dst) || !f.exists(src))) {
                        ++pj_.idx;
                        cost += kJobSmallCost;
                        return true;
                    }
                    bool failed = false;
                    if (jobCopyUnit(src, dst, cost, &failed)) {
                        if (failed) ++pj_.loadErrors;
                        ++pj_.idx;
                    }
                    return true;
                }
                if (!savePatch(pj_.idx, pj_.dir)) ++pj_.loadErrors;
                ++pj_.idx;
                cost += kJobSmallCost + kPatchMaxFileSize / 4;
                return true;
            }
            pj_.stage = kSFinish;
            return true;
        case kSFinish:
            if (pj_.loadErrors) {
                jobFail();
                return false;
            }
            // spec 05 appendix E2: a completed save sets the status flags to 0.
            unsaved_ = false;
            neverSaved_ = false;
            if (pj_.keptPatch >= 0) {
                char msg[48];
                std::snprintf(msg, sizeof(msg), "Synth %d patch kept unchanged", pj_.keptPatch + 1);
                ui.notify(msg, 4000);
            }
            jobEnd();
            return false;
        default:
            jobFail();
            return false;
    }
}

// ---- job starters ----------------------------------------------------------

bool App::startLoadWorkspace(JobDoneFn done, void* ctx) {
    finishJob();
    if (!fs().exists("Workspace/project.mt")) return false;
    // SD streaming C14: an import cut by a power loss / card removal.
    fs().remove(kImportTmp);
    pj_ = ProjectJob{};
    pj_.op = JobOp::LoadWorkspace;
    g_clock = hal_.clock;
    g_pfs.s = &fs();
    profBegin(pj_.op);
    pj_.profiled = true;
    pj_.popup = true;
    pj_.title = "Opening last project";
    std::snprintf(pj_.wavBase, sizeof(pj_.wavBase), "%s", kWorkspace);
    pj_.onDone = done;
    pj_.ctx = ctx;
    // One scan of Workspace/instruments and Workspace/patterns.
    scanSlots(g_pfs, kWorkspace, g_scan, true, nullptr, 0, true);
    pj_.overBudget = scanSampleBytes(g_scan) > bank.capacityBytes();   // SD streaming (Auto)
    estimateJobTotal();
    jobBeginLoad();
    seq.stop();
    allVoicesOff();
    jobProgress();
    return true;
}

bool App::startExternalJob(const char* title, JobUnitFn unit, JobPercentFn percent, void* unitCtx, JobDoneFn done,
                           void* ctx) {
    if (!unit) return false;
    finishJob();
    pj_ = ProjectJob{};
    pj_.op = JobOp::External;
    pj_.popup = true;
    pj_.title = title;
    pj_.extUnit = unit;
    pj_.extPercent = percent;
    pj_.extCtx = unitCtx;
    pj_.onDone = done;
    pj_.ctx = ctx;
    jobProgress();
    return true;
}

bool App::loadWorkspace() {
    if (!startLoadWorkspace()) return false;
    finishJob();
    return lastJobOk_;
}

bool App::startOpenProject(const char* folder, const char* name, JobDoneFn done, void* ctx) {
    // spec 05 appendix E3 OPEN (0x1d2e0): name <= 32 and project.mt must exist.
    if (!name || !name[0] || std::strlen(name) > mt::kNameMax) return false;
    const char* fold = folder && folder[0] ? folder : kProjectsDir;
    if (std::strlen(fold) > mt::kFolderMax) return false;
    finishJob();
    IStorage& f = fs();
    char src[340], dir[sizeof(pj_.dir)];
    std::snprintf(dir, sizeof(dir), "%s/%s", fold, name);
    projectFilePath(src, sizeof(src), dir);
    if (!f.exists(src)) return false;
    // spec 10 §4.2 step 1: load + upgrade the project first. SPEC GAP: done
    // before the Workspace is wiped, so a rejected file (type != 0, version
    // 0) or a project too big for the sample bank leaves the current one.
    Project p;
    ProjectLoadInfo pinfo;
    if (loadProjectFile(f, src, p, &pinfo) != FmtStatus::Ok) return false;
    g_clock = hal_.clock;
    g_pfs.s = &f;
    profBegin(JobOp::Open);
    // The one scan of the project folder: memory check, copy list, patterns.
    scanSlots(g_pfs, dir, g_scan, false, g_list, int(sizeof(g_list) / sizeof(g_list[0])), true);
    const bool overBudget = scanSampleBytes(g_scan) > bank.capacityBytes();
    if (!scanFitsMemory(*this, g_scan)) return false;
    recentsLeaveCurrent(*this);
    seq.stop();
    allVoicesOff();
    pj_ = ProjectJob{};
    pj_.op = JobOp::Open;
    pj_.profiled = true;
    pj_.popup = true;
    pj_.opened = true;
    pj_.fromProject = true;
    pj_.title = "Opening project";
    std::memcpy(pj_.dir, dir, sizeof(dir));
    std::memcpy(pj_.wavBase, dir, sizeof(dir));
    std::snprintf(pj_.wipe, sizeof(pj_.wipe), "%s", kWorkspace);
    pj_.wipeRootLen = uint16_t(std::strlen(kWorkspace));
    pj_.proj = p;
    pj_.slotReset = pinfo.synthSlotsReset;   // the Workspace copy is written with the reset record
    pj_.proj.setName(name);
    pj_.proj.setFolder(fold);
    pj_.onDone = done;
    pj_.ctx = ctx;
    pj_.stage = kOWipe;
    pj_.overBudget = overBudget;
    estimateJobTotal();
    jobProgress();
    return true;
}

bool App::openProject(const char* folder, const char* name) {
    if (!startOpenProject(folder, name)) return false;
    finishJob();
    return lastJobOk_ && lastJobErrors_ == 0;
}

bool App::startAutosave() {
    if (pj_.op != JobOp::None) return false;
    lastAutosave_ = millis();
    bool any = projectDirty_ || namesDirty_;
    for (int s = 0; s < kSynthSlotCount && !any; ++s) any = patchDirty_[s];
    for (int i = 0; i < 32 && !any; ++i) any = patDirty_[i] != 0;
    for (int i = 0; i < kSampleInstrumentCount && !any; ++i) any = instrHeaderDirty_[i] || instrSampleDirty_[i];
    // The edited pattern may differ from the store without a dirty mark yet.
    if (!any) {
        commitCurrentPattern();
        return false;
    }
    pj_ = ProjectJob{};
    pj_.op = JobOp::Autosave;
    pj_.stage = kABegin;
    return true;
}

void App::autosave() {
    // Blocking flush (power-off, Config, tests): finish whatever runs, then
    // write every dirty item now.
    finishJob();
    if (startAutosave()) finishJob();
    lastAutosave_ = millis();
}

bool App::startSaveProjectAs(const char* nameOrPath, JobDoneFn done, void* ctx) {
    // A plain name saves into the browser root "Projects" (spec 05 appendix
    // E1.2); "<folder>/<name>" (saveProjectInto: the Save browser's current
    // folder; keyboard names cannot contain '/') saves into that folder.
    if (!nameOrPath || !nameOrPath[0]) return false;
    char folder[mt::kFolderMax + 1];
    const char* name = nameOrPath;
    if (const char* s = std::strrchr(nameOrPath, '/')) {
        const size_t fl = size_t(s - nameOrPath);
        if (fl == 0 || fl > mt::kFolderMax) return false;
        std::memcpy(folder, nameOrPath, fl);
        folder[fl] = 0;
        name = s + 1;
    } else {
        std::snprintf(folder, sizeof(folder), "%s", kProjectsDir);
    }
    if (!name[0] || std::strlen(name) > mt::kNameMax) return false;
    finishJob();
    // spec 05 appendix E3: update recents (the project being left), set
    // folder + name, mark the project dirty, flush the Workspace (stage 2),
    // then copy Workspace -> "<folder>/<name>" (stage 4).
    recentsLeaveCurrent(*this);
    project.setName(name);
    project.setFolder(folder);
    projectDirty_ = true;
    pj_ = ProjectJob{};
    pj_.op = JobOp::Save;
    pj_.popup = true;
    pj_.title = "Saving project";
    std::snprintf(pj_.dir, sizeof(pj_.dir), "%s/%s", folder, name);
    pj_.onDone = done;
    pj_.ctx = ctx;
    pj_.stage = kABegin;
    estimateJobTotal();
    jobProgress();
    return true;
}

bool App::saveProjectAs(const char* nameOrPath) {
    if (!startSaveProjectAs(nameOrPath)) return false;
    finishJob();
    return lastJobOk_;
}

bool App::startSaveProject(JobDoneFn done, void* ctx) {
    // spec 05 appendix E1.3 / E3: Save goes to the project's own "<folder>/<name>"
    // (the folder stored in project.mt or the one it was opened from).
    if (neverSaved_ || !project.name[0]) return false;
    return startSaveProjectInto(*this, project.folder[0] ? project.folder : kProjectsDir, project.name, done, ctx);
}

bool App::saveProject() {
    if (!startSaveProject()) return false;
    finishJob();
    return lastJobOk_;
}

bool canDirectSave(App& a) {
    // spec 05 appendix E1.3 + E2: a brand-new project has an empty folder.
    return !a.neverSaved() && a.project.name[0] && a.project.folder[0] &&
           a.projectExists(a.project.folder, a.project.name);
}

bool startSaveProjectInto(App& a, const char* folder, const char* name, void (*done)(void*, bool), void* ctx) {
    if (!name || !name[0] || std::strchr(name, '/')) return false;
    if (!folder || !folder[0]) return a.startSaveProjectAs(name, done, ctx);
    if (std::strlen(folder) > mt::kFolderMax) return false;
    char path[mt::kFolderMax + mt::kNameMax + 2];
    std::snprintf(path, sizeof(path), "%s/%s", folder, name);
    return a.startSaveProjectAs(path, done, ctx);
}

bool saveProjectInto(App& a, const char* folder, const char* name) {
    if (!startSaveProjectInto(a, folder, name, nullptr, nullptr)) return false;
    a.finishJob();
    return a.lastJobOk();
}

// ---------------------------------------------------------------------------
// Memory pre-check, recents, card removal

namespace {
// The folder scan of the memory pre-check (one copy in SDRAM).
const SlotScan& scanForMemory(App& a, const char* dir) {
    OT_SDRAM_BSS static SlotScan s;
    scanSlots(a.fs(), dir, s);
    return s;
}
}  // namespace

uint64_t projectSampleBytes(App& a, const char* dir) {
    // spec 02 §8.2 / spec 10 §4.2 step 0: sum the sample data sizes.
    return scanSampleBytes(scanForMemory(a, dir));
}

bool projectFitsMemory(App& a, const char* folder, const char* name) {
    char dir[mt::kFolderMax + mt::kNameMax + 2];
    std::snprintf(dir, sizeof(dir), "%s/%s", folder && folder[0] ? folder : kProjectsDir, name);
    // SPEC GAP: the stock reaction to an over-budget project is not
    // specified; OpenTracker refuses to open it. SD streaming: the same rule
    // as the Open job (scanFitsMemory).
    return scanFitsMemory(a, scanForMemory(a, dir));
}

bool workspaceFitsMemory(App& a) {
    OT_SDRAM_BSS static SlotScan s;   // the Workspace names its files instrument_NN.pti
    scanSlots(a.fs(), kWorkspace, s, true);
    const uint32_t bank = a.mem().sampleBankSamples;
    const bool on = a.cfg.streamMode != 0 && a.hal().streamReader && bank >= 2u * audio::kStreamRegionSamples;
    const uint64_t cap = uint64_t(bank - (on ? audio::kStreamRegionSamples : 0u)) * sizeof(int16_t);
    if (scanSampleBytes(s) <= cap) return true;
    return on && streamProjectedBytes(s.ptiPayload, s.hasPti, kSampleInstrumentCount, StreamMode(a.cfg.streamMode),
                                      kStreamThresholdSec[a.cfg.streamThreshold & 3]) <= cap;
}

namespace {
// The Recents list (10 x 500 B) the functions below work on: one copy in
// SDRAM instead of a 5,000 B local in each. None holds it across a call into
// another user (Open Recent copies its path out first), so one is enough;
// as locals, Open Recent nested two on the stack (startOpenRecent ->
// startOpenProject -> recentsLeaveCurrent).
OT_SDRAM_BSS Recents g_recents;
}  // namespace

Recents& recentsBuffer() { return g_recents; }

int loadRecents(App& a, Recents& r) {
    loadRecentsFile(a.fs(), kRecentsPath, r);
    int n = 0;
    char p[Recents::kSlotSize + 16];
    for (int i = 0; i < Recents::kSlots; ++i) {
        r.paths[i][Recents::kSlotSize - 1] = 0;
        if (!r.paths[i][0]) continue;
        std::snprintf(p, sizeof(p), "%s/project.mt", r.paths[i]);
        if (!a.fs().exists(p)) continue;   // stale entry removed (0x236e0)
        if (n != i) std::memcpy(r.paths[n], r.paths[i], Recents::kSlotSize);
        ++n;
    }
    for (int i = n; i < Recents::kSlots; ++i) std::memset(r.paths[i], 0, Recents::kSlotSize);
    return n;
}

void recentsLeaveCurrent(App& a) {
    if (!canDirectSave(a)) return;   // only a project that exists on the card
    Recents& r = recentsBuffer();
    loadRecents(a, r);
    char p[mt::kFolderMax + mt::kNameMax + 2];
    std::snprintf(p, sizeof(p), "%s/%s", a.project.folder, a.project.name);
    r.touch(p);   // MRU, de-duplicated, 10 entries
    saveRecentsFile(a.fs(), kRecentsPath, r);
}

bool splitProjectPath(const char* path, char* folder, int folderCap, char* name, int nameCap) {
    if (!path || !path[0]) return false;
    const char* s = std::strrchr(path, '/');
    if (!s) {
        std::snprintf(folder, size_t(folderCap), "%s", "");
        std::snprintf(name, size_t(nameCap), "%s", path);
    } else {
        std::snprintf(folder, size_t(folderCap), "%.*s", int(s - path), path);
        std::snprintf(name, size_t(nameCap), "%s", s + 1);
    }
    return name[0] != 0;
}

bool openRecent(App& a, int index) {
    Recents& r = recentsBuffer();
    const int n = loadRecents(a, r);
    if (index < 0 || index >= n) return false;
    char folder[Recents::kSlotSize], name[Recents::kSlotSize];
    if (!splitProjectPath(r.paths[index], folder, sizeof(folder), name, sizeof(name))) return false;
    return a.openProject(folder, name);
}

bool startOpenRecent(App& a, int index, void (*done)(void*, bool), void* ctx) {
    Recents& r = recentsBuffer();
    const int n = loadRecents(a, r);
    if (index < 0 || index >= n) return false;
    char folder[Recents::kSlotSize], name[Recents::kSlotSize];
    if (!splitProjectPath(r.paths[index], folder, sizeof(folder), name, sizeof(name))) return false;
    return a.startOpenProject(folder, name, done, ctx);
}

namespace {
bool g_cardMissing = false;
bool g_cardProbed = false;
uint32_t g_cardProbeMs = 0;
constexpr uint32_t kCardProbeMs = 500;   // spec 05 §1.7 card-detect debounce
}  // namespace

bool cardMissing() { return g_cardMissing; }

bool serviceCardRemoved(App& a, uint32_t now) {
    if (g_cardProbed && now - g_cardProbeMs < kCardProbeMs) return g_cardMissing;
    g_cardProbed = true;
    g_cardProbeMs = now;
    const bool present = a.fs().listDir("", stopAtFirst, nullptr) >= 0;
    if (present) {
        if (g_cardMissing) {
            // spec 05 §1.5: state 3 -> 2 -> start-up load (reopen the last project).
            g_cardMissing = false;
            a.boot();
            a.ui.dirty();
        }
        return false;
    }
    if (!g_cardMissing) {
        g_cardMissing = true;
        a.abortJob();   // its files are gone with the card
        a.streamUnbindAll();
        a.seq.stop();
        a.allVoicesOff();
    }
    if (a.hal().display) {
        Gfx g(*a.hal().display);
        g.beginFrame(0);
        g.text(400, 240, kFontLarge, 0xFFFFFF, "Please insert SD card to continue", IDisplay::kOptCenter);
        g.endFrame();
    }
    return true;
}


// ---------------------------------------------------------------------------
// Instruments

uint32_t App::sampleBytesNeeded(const char* path) {
    IStorage& f = fs();
    if (endsWithNoCase(path, ".pti")) {
        IStorage::Handle h = f.open(path, IStorage::kRead);
        if (h < 0) return 0;
        int32_t sz = f.size(h);
        f.close(h);
        return sz > int32_t(pti::kDataOff) ? uint32_t(sz) - pti::kDataOff : 0;
    }
    IStorage::Handle h = f.open(path, IStorage::kRead);
    if (h < 0) return 0;
    StorageReader r(f, h);
    WavInfo info;
    const bool ok = wavParseHeader(r, info);
    f.close(h);
    if (!ok || !wavIsSupported(info)) return 0;
    // 2.x: stereo WAVs load as planar stereo, 4 bytes per frame (2x-04 §2).
    return wavResampledFrames(wavSourceFrames(info), info.sampleRate) * (info.channels == 2 ? 4u : 2u);
}

bool App::importSample(const char* path, int slot, uint16_t startMarker, uint16_t endMarker, bool lowQuality) {
    if (slot < 0 || slot >= kSampleInstrumentCount) return false;
    finishJob();   // the bank moves under a running instrument write
    IStorage& f = fs();
    allVoicesOff();   // bank moves invalidate voice sample pointers
    // A WAV loaded into an existing instrument keeps its settings (stock:
    // copy an instrument, then load another sample into the copy; user
    // report / Polyend forum); only the audio and its name change.
    const bool keep = instrPresent[slot];
    const Instrument prev = instruments[slot];
    streamUnbind(slot);
    bank.remove(slot);
    refreshStreamSources();
    instrPresent[slot] = false;
    Instrument ins;
    if (endsWithNoCase(path, ".pti")) {
        PtiSampleLayout lay;
        if (loadInstrumentFile(f, path, ins, bank.tail(), uint32_t(bank.freeSamples()), true, &lay) != FmtStatus::Ok) {
            markInstrumentDirty(slot);
            return false;
        }
        bank.commitTail(slot, ins.lengthFrames, ins.stereo);
    } else {
        WavImportOptions opt;
        opt.stereo = true;   // 2.x: a stereo WAV becomes a planar stereo .pti (2x-04 §4)
        opt.startMarker = startMarker;
        opt.endMarker = endMarker;
        WavImportResult res;
        if (wavImportFile(f, path, opt, bank.tail(), uint32_t(bank.freeSamples()), res) != FmtStatus::Ok) {
            markInstrumentDirty(slot);
            return false;
        }
        uint32_t frames = res.frames;
        if (lowQuality) {
            // spec 06 §5.5: store at 22.05 kHz, quality byte -12.
            int16_t* d = bank.tail();
            for (uint32_t i = 0; i < frames / 2; ++i) d[i] = int16_t((int32_t(d[2 * i]) + d[2 * i + 1]) / 2);
            if (res.stereo) {   // right block: from d + frames to d + frames / 2
                const int16_t* rs = d + frames;
                int16_t* rd = d + frames / 2;
                for (uint32_t i = 0; i < frames / 2; ++i) rd[i] = int16_t((int32_t(rs[2 * i]) + rs[2 * i + 1]) / 2);
            }
            frames /= 2;
            ins.reserved173 = uint8_t(int8_t(-12));
        }
        ins.setDefault();
        if (keep) {
            ins = prev;
            ins.sampleType = uint8_t(SampleType::WaveFile);
            ins.stereo = false;
            ins.reserved173 = 0;
            ins.name[0] = 0;
            if (ins.wavetableWindowSize) ins.wavetableWindowCount = frames / ins.wavetableWindowSize;
        }
        if (lowQuality) ins.reserved173 = uint8_t(int8_t(-12));
        // Name = file name without extension, max 31 chars (spec 06 §5.4).
        const char* bn = baseName(path);
        size_t n = std::strlen(bn);
        const char* dot = std::strrchr(bn, '.');
        if (dot) n = size_t(dot - bn);
        if (n > size_t(kInstrumentNameLen)) n = kInstrumentNameLen;
        std::memcpy(ins.name, bn, n);
        ins.name[n] = 0;
        ins.lengthFrames = frames;
        ins.stereo = res.stereo;
        bank.commitTail(slot, frames, res.stereo);
    }
    ins.lengthFrames = bank.frames(slot);
    instruments[slot] = ins;
    instrPresent[slot] = true;
    refreshStreamSources();
    markInstrumentDirty(slot, true);
    return true;
}

bool App::deleteInstrument(int slot) {
    if (slot < 0 || slot >= kSampleInstrumentCount) return false;
    finishJob();   // the bank moves under a running instrument write
    allVoicesOff();
    streamUnbind(slot);
    bank.remove(slot);
    refreshStreamSources();
    instruments[slot].setDefault();
    instrPresent[slot] = false;
    markInstrumentDirty(slot, true);
    return true;
}

bool App::copyInstrument(int src, int dst) {
    if (src < 0 || dst < 0 || src >= kSampleInstrumentCount || dst >= kSampleInstrumentCount || src == dst) return false;
    finishJob();   // the bank moves under a running instrument write
    if (!instrPresent[src]) return false;
    allVoicesOff();
    streamUnbind(dst);
    if (streamed(src)) {
        // A streamed source: a full RAM copy from its Workspace file, never
        // a copy of the head.
        bank.remove(dst);
        refreshStreamSources();
        char path[340];
        workspaceInstrumentPath(path, sizeof(path), kWorkspace, src + 1);
        Instrument ins;
        PtiSampleLayout lay;
        if (loadInstrumentFile(fs(), path, ins, bank.tail(), uint32_t(bank.freeSamples()), true, &lay) != FmtStatus::Ok) {
            ui.notify("Not enough memory to copy the sample");
            return false;
        }
        bank.commitTail(dst, ins.lengthFrames, ins.stereo);
        refreshStreamSources();
        Instrument copy = instruments[src];
        copy.lengthFrames = bank.frames(dst);
        copy.stereo = bank.stereo(dst);
        instruments[dst] = copy;
        instrPresent[dst] = true;
        markInstrumentDirty(dst, true);
        return true;
    }
    if (!bank.copy(src, dst)) return false;
    refreshStreamSources();
    instruments[dst] = instruments[src];
    instrPresent[dst] = true;
    markInstrumentDirty(dst, true);
    return true;
}

bool App::loadStreamedIntoMemory(int slot) {
    if (!streamed(slot)) return true;
    finishJob();
    allVoicesOff();
    char path[340];
    workspaceInstrumentPath(path, sizeof(path), kWorkspace, slot + 1);
    // Read the whole sample after the last instrument first (the head stays
    // until the read succeeded), then drop the head and move it down.
    int16_t* at = bank.tail();
    Instrument ins;
    PtiSampleLayout lay;
    if (loadInstrumentFile(fs(), path, ins, at, uint32_t(bank.freeSamples()), true, &lay) != FmtStatus::Ok) {
        ui.notify("Not enough memory to load the sample");
        return false;
    }
    const uint32_t n = ins.lengthFrames * (ins.stereo ? 2u : 1u);
    const Instrument prev = instruments[slot];
    streamUnbind(slot);
    bank.remove(slot);
    std::memmove(bank.tail(), at, size_t(n) * sizeof(int16_t));
    bank.commitTail(slot, ins.lengthFrames, ins.stereo);
    refreshStreamSources();
    Instrument keep = prev;
    keep.lengthFrames = bank.frames(slot);
    keep.stereo = bank.stereo(slot);
    instruments[slot] = keep;
    markInstrumentDirty(slot);   // header only: the file is unchanged
    return true;
}

void App::renameInstrument(int slot, const char* name) {
    if (slot < 0 || slot >= kSampleInstrumentCount || !name) return;
    std::snprintf(instruments[slot].name, sizeof(instruments[slot].name), "%s", name);
    markInstrumentDirty(slot);
}

}  // namespace ot
