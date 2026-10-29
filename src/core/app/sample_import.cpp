// SD streaming C14: importing a sample that does not fit the free sample
// memory (docs/PLAN_SD_STREAMING_P1C_P2.md, C14). The WAV is converted (a
// .pti is copied) onto the card in 32 KB chunks as the slot's Workspace
// instrument file, through Workspace/instruments/import.tmp laid out with
// IStorage::reserve(); then its head is loaded and the rest streams, as for
// a streamed instrument of a project that opens. The old instrument stays
// until the new file is in place; a cancel (Back), a full card or a read
// error leaves it untouched.
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "app/stream_policy.h"
#include "formats/file_io.h"
#include "formats/instrument_pti.h"
#include "formats/wav.h"
#include "sdram_bss.h"

namespace ot {

namespace {

constexpr uint32_t kChunk = 0x8000;                    // source bytes per job unit
constexpr uint32_t kWinFrames = 0x8000 / 2;            // output window (int16)

struct ImportJob {
    char src[IStorage::kMaxPath + 1] = {};
    char dst[96] = {};
    int slot = 0;
    bool wav = false;
    WavInfo info;
    WavImportOptions opt;
    Instrument ins;
    uint32_t frames = 0;        // per channel
    uint8_t ch = 1;             // output channels
    uint8_t pass = 0;           // output channel being written
    uint32_t dataOff = 0, dataLen = 0, pos = 0;   // source payload bytes; read so far in this pass
    IStorage::Handle in = -1, out = -1;
    WavChunkConverter conv;
    uint64_t done = 0, total = 0;   // output bytes
    bool ioError = false;
    bool finishing = false;         // every pass written: close and finalize next
    bool forcedOneShot = false;
    StreamWhy why = StreamWhy::None;   // for the notice
    void (*userDone)(void*, bool) = nullptr;
    void* userCtx = nullptr;
};
OT_SDRAM_BSS SdramLazy<ImportJob> g_import;   // SDRAM on the device, no static constructor

struct Source {
    bool wav = false;
    WavInfo info;
    Instrument ins;             // .pti: its settings
    uint32_t frames = 0;
    bool stereo = false;
    uint32_t dataOff = 0;       // .pti payload offset
};

const char* baseNameOf(const char* p) {
    const char* s = std::strrchr(p, '/');
    return s ? s + 1 : p;
}

bool endsPti(const char* p) {
    const size_t n = std::strlen(p);
    return n > 4 && (p[n - 4] == '.') && (p[n - 3] | 0x20) == 'p' && (p[n - 2] | 0x20) == 't' &&
           (p[n - 1] | 0x20) == 'i';
}

// Frames / channels of `path` as the import would produce them.
bool probe(IStorage& fs, const char* path, Source& s) {
    IStorage::Handle h = fs.open(path, IStorage::kRead);
    if (h < 0) return false;
    bool ok = false;
    if (endsPti(path)) {
        uint8_t hdr[pti::kHeaderBlockSize] = {};
        const int32_t size = fs.size(h);
        const int32_t got = fs.read(h, hdr, sizeof(hdr));
        PtiLoadInfo li;
        if (got > 0 && size > 0 && decodePtiHeader(hdr, size_t(got), s.ins, &li) == FmtStatus::Ok &&
            uint32_t(size) > li.dataOffset) {
            const PtiSampleLayout lay = resolvePtiSample(s.ins.lengthFrames, uint32_t(size) - li.dataOffset, true);
            s.frames = lay.frames;
            s.stereo = lay.useStereo;
            s.dataOff = li.dataOffset;
            ok = s.frames > 0;
        }
    } else {
        StorageReader r(fs, h);
        if (wavParseHeader(r, s.info) && wavIsSupported(s.info)) {
            WavImportOptions opt;
            opt.stereo = true;   // 2.x: a stereo WAV becomes a planar stereo .pti
            const WavImportPlan p = wavPlanImport(s.info, opt);
            s.wav = true;
            s.frames = p.outFrames;
            s.stereo = p.outChannels == 2;
            ok = s.frames > 0;
        }
    }
    fs.close(h);
    return ok;
}

void sinkWrite(void* ctx, const int16_t* smp, uint32_t n) {
    App& a = *static_cast<App*>(ctx);
    ImportJob& j = g_import.get();
    if (j.ioError) return;
    // The int16 samples are the little-endian file bytes (the device and the
    // hosts are little-endian; saveInstrumentFile does the same).
    const int32_t want = int32_t(n * 2u);
    if (a.fs().write(j.out, smp, uint32_t(want)) != want) j.ioError = true;
    j.done += uint32_t(want);
}

ImportProbe probeOf(const Source& s) {
    ImportProbe p;
    p.frames = s.frames;
    p.stereo = s.stereo;
    p.pti = !s.wav;
    p.playMode = s.ins.playMode;
    p.loopPoint1 = s.ins.loopPoint1;
    return p;
}

}  // namespace

bool App::importProbe(const char* path, ImportProbe& p) {
    Source s;
    if (!path || !probe(fs(), path, s)) return false;
    p = probeOf(s);
    return true;
}

ImportRoute App::importRoute(const char* path, int slot, uint32_t* bankBytes) {
    if (bankBytes) *bankBytes = 0;
    ImportProbe p;
    if (!path || slot < 0 || slot >= kSampleInstrumentCount || !importProbe(path, p)) return ImportRoute::Unreadable;
    // No free-space query (a whole-FAT scan after a mount): the import's
    // reserve() says when the card is full.
    return importRoute(p, slot, bankBytes, nullptr, UINT64_MAX);
}

ImportRoute App::importRoute(const ImportProbe& p, int slot, uint32_t* bankBytes, StreamWhy* why,
                             uint64_t cardFree, int64_t pendingBytes, int pendingStreams) {
    if (bankBytes) *bankBytes = 0;
    if (why) *why = StreamWhy::None;
    if (slot < 0 || slot >= kSampleInstrumentCount || !p.frames) return ImportRoute::Unreadable;
    const uint64_t bytes = uint64_t(p.frames) * (p.stereo ? 2u : 1u) * 2u;
    if (bankBytes) *bankBytes = bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(bytes);
    const int64_t o = int64_t(bank.usedBytes()) - int64_t(bank.samples(slot)) * 2 + pendingBytes;
    const uint64_t others = o > 0 ? uint64_t(o) : 0;
    const uint64_t cap = uint64_t(bank.capacityBytes()) - uint64_t(bank.reserved()) * 2u;
    int inUse = pendingStreams;
    for (int i = 0; i < kSampleInstrumentCount; ++i) inUse += i != slot && stream_.streamOf[i] >= 0;
    // Kept settings (WAV into an existing instrument) or the file's (.pti).
    uint8_t pm = !p.pti ? (instrPresent[slot] ? instruments[slot].playMode : uint8_t(PlayMode::OneShot)) : p.playMode;
    const uint16_t lp1 = !p.pti ? (instrPresent[slot] ? instruments[slot].loopPoint1 : 0) : p.loopPoint1;
    ImportDecisionIn d;
    d.mode = streamingOn() ? StreamMode(cfg.streamMode > 2 ? 0 : cfg.streamMode) : StreamMode::Off;
    d.playMode = pm;
    d.frames = p.frames;
    d.stereo = p.stereo;
    d.thresholdSec = kStreamThresholdSec[cfg.streamThreshold > 3 ? 1 : cfg.streamThreshold];
    d.fitsWhole = others + bytes <= cap;
    d.readerAvailable = hal_.streamReader != nullptr;
    d.streamsInUse = inUse;
    const ImportDecision dec = importDecision(d);
    if (!dec.streams) return d.fitsWhole ? ImportRoute::Fits : ImportRoute::Off;
    if (inUse >= audio::kMaxStreams) return ImportRoute::TooMany;
    // A play mode that cannot stream becomes 1-Shot.
    if (dec.forcedOneShot) pm = uint8_t(PlayMode::OneShot);
    const StreamResident r = streamResident(pm, p.frames, lp1);
    const uint64_t head = uint64_t(r.framesPerChannel()) * (p.stereo ? 2u : 1u) * 2u;
    if (others + head > cap) return ImportRoute::NoMemory;
    if (bankBytes) *bankBytes = uint32_t(head);
    if (cardFree != UINT64_MAX && cardFree < ptiFileSize(p.frames, p.stereo)) return ImportRoute::CardFull;
    if (why) *why = dec.why;
    return ImportRoute::Streams;
}

bool App::startStreamedImport(const char* path, int slot, void (*done)(void*, bool), void* ctx) {
    importNotice_[0] = 0;
    Source s;
    StreamWhy streamWhy = StreamWhy::None;
    const ImportRoute route = !path || !probe(fs(), path, s)
                                  ? ImportRoute::Unreadable
                                  // No free-space query: the first after a mount scans the
                                  // whole FAT (seconds); reserve() below fails on a full card.
                                  : importRoute(probeOf(s), slot, nullptr, &streamWhy, UINT64_MAX);
    if (route != ImportRoute::Streams) {
        const char* why = "Cannot read the file";
        switch (route) {
            case ImportRoute::Off: why = "Not enough memory: turn on Config > Stream from card"; break;
            case ImportRoute::TooMany: why = "16 instruments already stream from the card"; break;
            case ImportRoute::NoMemory: why = "Not enough sample memory"; break;
            case ImportRoute::CardFull: why = "Not enough space on the SD card"; break;
            case ImportRoute::Fits: why = ""; break;
            default: break;
        }
        std::snprintf(importNotice_, sizeof(importNotice_), "%s", why);
        return false;
    }
    finishJob();
    ImportJob& j = g_import.get();
    j = ImportJob{};
    std::snprintf(j.src, sizeof(j.src), "%s", path);
    workspaceInstrumentPath(j.dst, sizeof(j.dst), kWorkspace, slot + 1);
    j.slot = slot;
    j.why = streamWhy;
    j.wav = s.wav;
    j.info = s.info;
    j.opt.stereo = true;
    j.frames = s.frames;
    j.ch = s.stereo ? 2 : 1;
    // The instrument: as importSample (a WAV keeps an existing instrument's
    // settings; a .pti brings its own), at the full length.
    Instrument ins;
    if (s.wav) {
        ins.setDefault();
        if (instrPresent[slot]) {
            ins = instruments[slot];
            ins.sampleType = uint8_t(SampleType::WaveFile);
            ins.reserved173 = 0;
            ins.name[0] = 0;
            if (ins.wavetableWindowSize) ins.wavetableWindowCount = s.frames / ins.wavetableWindowSize;
        }
        const char* bn = baseNameOf(path);
        size_t n = std::strlen(bn);
        const char* dot = std::strrchr(bn, '.');
        if (dot) n = size_t(dot - bn);
        if (n > size_t(kInstrumentNameLen)) n = kInstrumentNameLen;
        std::memcpy(ins.name, bn, n);
        ins.name[n] = 0;
        j.dataOff = s.info.dataOffset;
        j.dataLen = s.info.dataSize;
    } else {
        ins = s.ins;
        j.dataOff = s.dataOff;
        j.dataLen = s.frames * 2u * j.ch;   // planar L (and R) blocks, copied as they are
    }
    ins.lengthFrames = s.frames;
    ins.stereo = s.stereo;
    if (!audio::AudioEngine::streamablePlayMode(ins.playMode)) {
        ins.playMode = uint8_t(PlayMode::OneShot);
        j.forcedOneShot = true;
    }
    j.ins = ins;
    j.total = uint64_t(s.frames) * j.ch * 2u;
    IStorage& f = fs();
    f.remove(kImportTmp);
    j.in = f.open(path, IStorage::kRead);
    j.out = j.in >= 0 ? f.open(kImportTmp, IStorage::kWrite) : -1;
    const uint64_t need = ptiFileSize(s.frames, s.stereo);
    bool reserved = j.out >= 0 && f.reserve(j.out, uint32_t(need));
    if (j.out >= 0 && !reserved) {
        // No contiguous run of that size. A full card is refused; one with
        // room but only in pieces is written unreserved (FatFs places it):
        // a stream may span up to kStreamMaxExtents pieces, and bind() after
        // the conversion refuses a file in more.
        const uint64_t free = f.freeBytes();
        if (free == UINT64_MAX || free < need) {
            importCleanup();
            std::snprintf(importNotice_, sizeof(importNotice_), "Not enough space on the SD card");
            return false;
        }
    }
    // Pre-check: the laid-out file can be bound, before minutes of
    // conversion (the size and the map self-check stay in bind()). An
    // unreserved file has no layout yet: the route counted the free slots.
    audio::StreamBindInfo bi;
    if (reserved && !hal_.streamReader->streamable(uint8_t(slot), kImportTmp, &bi)) {
        importCleanup();
        if (std::strcmp(bi.reason, "no slot") == 0)
            std::snprintf(importNotice_, sizeof(importNotice_), "16 instruments already stream from the card");
        else if (std::strcmp(bi.reason, "fragmented") == 0)
            std::snprintf(importNotice_, sizeof(importNotice_), "Card too fragmented to stream: format the SD card");
        else
            std::snprintf(importNotice_, sizeof(importNotice_), "Cannot stream the file (%s)", bi.reason);
        return false;
    }
    bool ok = j.out >= 0;
    uint8_t hdr[pti::kHeaderBlockSize];
    encodePtiHeader(ins, hdr);
    ok = ok && f.write(j.out, hdr, sizeof(hdr)) == int32_t(sizeof(hdr)) && f.seek(j.in, j.dataOff);
    if (ok && j.wav) {
        int16_t* win = reinterpret_cast<int16_t*>(mem_.scratch + kChunk);
        ok = mem_.scratchBytes >= kChunk + kWinFrames * 2u &&
             j.conv.begin(j.info, j.opt, 0, win, kWinFrames) == FmtStatus::Ok;
    }
    if (!ok) {
        importCleanup();
        std::snprintf(importNotice_, sizeof(importNotice_), "Not enough space on the SD card");
        return false;
    }
    j.userDone = done;
    j.userCtx = ctx;
    if (!startExternalJob("Importing - Delete cancels", &App::importUnitThunk, &App::importPercentThunk, this,
                          &App::importDoneThunk, this)) {
        importCleanup();   // the laid-out file goes too
        return false;
    }
    ui.progressCancel(
        [](void* c) {
            App& a = *static_cast<App*>(c);
            std::snprintf(a.importNotice_, sizeof(a.importNotice_), "Import cancelled");
            a.abortJob();
        },
        this);
    return true;
}

int App::importUnitThunk(void* ctx, uint32_t& cost) { return static_cast<App*>(ctx)->importUnit(cost); }

int App::importPercentThunk(void* ctx) {
    (void)ctx;
    const ImportJob& j = g_import.get();
    return j.total ? int(j.done * 100u / j.total) : 0;
}

void App::importDoneThunk(void* ctx, bool ok) {
    App& a = *static_cast<App*>(ctx);
    ImportJob& j = g_import.get();
    if (!ok) a.importCleanup();   // cancel, card removed, a write / read error
    if (!ok && !a.importNotice_[0]) std::snprintf(a.importNotice_, sizeof(a.importNotice_), "Import cancelled");
    void (*fn)(void*, bool) = j.userDone;
    void* c = j.userCtx;
    j.userDone = nullptr;
    if (fn) fn(c, ok);
}

void App::importCleanup() {
    ImportJob& j = g_import.get();
    IStorage& f = fs();
    if (j.in >= 0) f.close(j.in);
    if (j.out >= 0) f.close(j.out);
    j.in = j.out = -1;
    f.remove(kImportTmp);
}

int App::importUnit(uint32_t& cost) {
    ImportJob& j = g_import.get();
    IStorage& f = fs();
    cost += kChunk;
    if (j.finishing) {
        // The file ends where the conversion did (reserve() may have laid out all of it).
        if (!f.truncate(j.out)) {
            std::snprintf(importNotice_, sizeof(importNotice_), "Cannot write the instrument file");
            return -1;
        }
        f.close(j.out);
        j.out = -1;
        f.close(j.in);
        j.in = -1;
        return importFinalize() ? 0 : -1;
    }
    uint8_t* buf = mem_.scratch;
    const uint32_t left = j.dataLen - j.pos;
    const uint32_t want = left < kChunk ? left : kChunk;
    const int32_t got = want ? f.read(j.in, buf, want) : 0;
    if (got < 0) {
        std::snprintf(importNotice_, sizeof(importNotice_), "Cannot read the file");
        return -1;
    }
    j.pos += uint32_t(got);
    if (j.wav) {
        j.conv.feed(buf, uint32_t(got), &sinkWrite, this);
    } else if (got > 0) {
        if (f.write(j.out, buf, uint32_t(got)) != got) j.ioError = true;
        j.done += uint32_t(got);
    }
    if (j.ioError) {
        std::snprintf(importNotice_, sizeof(importNotice_), "Not enough space on the SD card");
        return -1;
    }
    const bool passEnd = got == 0 || j.pos >= j.dataLen;
    if (!passEnd) return 1;
    if (j.wav) {
        j.conv.finish(&sinkWrite, this);
        if (j.ioError) {
            std::snprintf(importNotice_, sizeof(importNotice_), "Not enough space on the SD card");
            return -1;
        }
        if (j.pass + 1 < j.ch) {
            // The right channel: the data chunk again.
            ++j.pass;
            j.pos = 0;
            int16_t* win = reinterpret_cast<int16_t*>(mem_.scratch + kChunk);
            if (!f.seek(j.in, j.dataOff) || j.conv.begin(j.info, j.opt, j.pass, win, kWinFrames) != FmtStatus::Ok)
                return -1;
            return 1;
        }
    }
    j.finishing = true;
    return 1;
}

bool App::importFinalize() {
    ImportJob& j = g_import.get();
    IStorage& f = fs();
    const int slot = j.slot;
    const bool stereo = j.ch == 2;
    // The converted file must map into a binding (at most kStreamMaxExtents
    // pieces when it was written unreserved) before the slot's instrument
    // goes: a refusal keeps the old one.
    audio::StreamBindInfo bi;
    if (!hal_.streamReader->streamable(uint8_t(slot), kImportTmp, &bi)) {
        f.remove(kImportTmp);
        if (std::strcmp(bi.reason, "fragmented") == 0)
            std::snprintf(importNotice_, sizeof(importNotice_), "Card too fragmented to stream: format the SD card");
        else if (std::strcmp(bi.reason, "no slot") == 0)
            std::snprintf(importNotice_, sizeof(importNotice_), "16 instruments already stream from the card");
        else
            std::snprintf(importNotice_, sizeof(importNotice_), "Cannot stream the file (%s)", bi.reason);
        return false;
    }
    // The new file replaces the slot's Workspace instrument.
    allVoicesOff();
    streamUnbind(slot);
    bank.remove(slot);
    refreshStreamSources();
    instrPresent[slot] = false;
    if (f.exists(j.dst)) f.remove(j.dst);
    if (!f.rename(kImportTmp, j.dst)) {
        std::snprintf(importNotice_, sizeof(importNotice_), "Cannot write the instrument file");
        markInstrumentDirty(slot);
        return false;
    }
    // Head (and loop head) into the bank: [L head | L loop][R head | R loop].
    const StreamResident r = streamResident(j.ins.playMode, j.frames, j.ins.loopPoint1);
    const uint32_t perCh = r.framesPerChannel();
    const bool room = bank.freeSamples() >= size_t(perCh) * j.ch;
    bool ok = room;
    const char* why = "read";   // bindStreamSlot names its own reason
    int16_t* t = bank.tail();
    IStorage::Handle h = ok ? f.open(j.dst, IStorage::kRead) : -1;
    ok = ok && h >= 0;
    for (int c = 0; ok && c < j.ch; ++c) {
        const uint32_t base = pti::kDataOff + uint32_t(c) * j.frames * 2u;
        int16_t* to = t + size_t(c) * perCh;
        const int32_t hb = int32_t(r.headFrames * 2u);
        ok = f.seek(h, base) && f.read(h, to, uint32_t(hb)) == hb;
        if (ok && r.loopFrames) {
            const int32_t lb = int32_t(r.loopFrames * 2u);
            ok = f.seek(h, base + r.loopStart * 2u) && f.read(h, to + r.headFrames, uint32_t(lb)) == lb;
        }
    }
    if (h >= 0) f.close(h);
    if (ok) {
        bank.commitTail(slot, perCh, stereo);
        bank.setPartial(slot, j.frames);
        instruments[slot] = j.ins;
        pj_.fullFrames = j.frames;
        pj_.resHead = r.headFrames;
        pj_.resLoopStart = r.loopStart;
        pj_.resLoopFrames = r.loopFrames;
        pj_.dataOff[slot] = pti::kDataOff;
        ok = bindStreamSlot(slot, j.dst, &why);
        if (!ok) bank.remove(slot);
    }
    if (!ok) {
        // The file stays on the card for nothing: the slot is empty.
        f.remove(j.dst);
        instruments[slot].setDefault();
        refreshStreamSources();
        markInstrumentDirty(slot);
        // Only a fragmented card is fixed by formatting it.
        if (!room)
            std::snprintf(importNotice_, sizeof(importNotice_), "Not enough sample memory");
        else if (std::strcmp(why, "fragmented") == 0)
            std::snprintf(importNotice_, sizeof(importNotice_), "Card too fragmented to stream: format the SD card");
        else
            std::snprintf(importNotice_, sizeof(importNotice_), "Cannot stream the file (%s)", why);
        return false;
    }
    instrPresent[slot] = true;
    refreshStreamSources();
    markInstrumentDirty(slot);   // header only: the file on the card is the sample
    // Why it streams, and a play mode that became 1-Shot.
    const char* oneShot = j.forcedOneShot ? ", 1-Shot" : "";
    if (j.why == StreamWhy::Threshold)
        std::snprintf(importNotice_, sizeof(importNotice_), "Streams from the card (longer than %u s%s)",
                      unsigned(kStreamThresholdSec[cfg.streamThreshold & 3]), oneShot);
    else if (j.why == StreamWhy::All)
        std::snprintf(importNotice_, sizeof(importNotice_), "Streams from the card (%s)",
                      j.forcedOneShot ? "All, 1-Shot" : "Stream from card: All");
    else
        std::snprintf(importNotice_, sizeof(importNotice_), "Streams from the card (does not fit memory%s)", oneShot);
    return true;
}

}  // namespace ot
