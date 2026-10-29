// Sample Loader (spec 06 §5).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "formats/instrument_pti.h"
#include "formats/wav.h"
#include "sdram_bss.h"
#include "ui/screens.h"

namespace ot {

namespace {

// SD Card list preview, streamed like stock (spec 06 §5.4, spec 07 Q7): the
// sound starts after the first chunk and the rest is decoded into the free
// tail of the sample bank while it plays; a file longer than the free memory
// plays its first part. Main loop only.
struct PreviewStream {
    IStorage::Handle h = -1;
    WavImporter imp;
    const int16_t* data = nullptr;   // bank.tail() at the start: the decode target
    uint32_t left = 0;   // data bytes still to read
    bool on = false;
    // .pti: raw 44.1 kHz int16 PCM at dataOff (spec 02 §6.3); planar stereo
    // is read block by block from the L and R halves in step.
    bool pti = false;
    bool stereo = false;
    uint32_t dataOff = 0, srcFrames = 0, total = 0, done = 0;
};
OT_SDRAM_BSS SdramLazy<PreviewStream> g_previewMem;   // SDRAM on the device (stack headroom)
PreviewStream& previewStream() { return g_previewMem.get(); }

void previewStreamStop(App& a) {
    PreviewStream& p = previewStream();
    if (p.h >= 0) a.fs().close(p.h);
    p.h = -1;
    p.on = false;
}

// Reads and decodes up to `budget` data bytes, then tells the player how far
// it may go.
void previewStreamPump(App& a, uint32_t budget) {
    PreviewStream& p = previewStream();
    if (!p.on) return;
    if (p.pti) {
        if (!a.engine.previewBufferActive() || a.bank.tail() > p.data) {
            previewStreamStop(a);
            return;
        }
        int16_t* dst = const_cast<int16_t*>(p.data);
        while (p.done < p.total && budget) {
            uint32_t n = p.total - p.done;
            const uint32_t maxN = budget / (p.stereo ? 4u : 2u);
            if (n > 1024) n = 1024;
            if (n > maxN) n = maxN ? maxN : 1;
            bool ok = a.fs().seek(p.h, p.dataOff + p.done * 2) &&
                      a.fs().read(p.h, dst + p.done, n * 2) == int32_t(n * 2);
            if (ok && p.stereo)
                ok = a.fs().seek(p.h, p.dataOff + (p.srcFrames + p.done) * 2) &&
                     a.fs().read(p.h, dst + p.total + p.done, n * 2) == int32_t(n * 2);
            if (!ok) {
                p.total = p.done;   // short file: play what was read
                break;
            }
            p.done += n;
            budget = budget > n * (p.stereo ? 4u : 2u) ? budget - n * (p.stereo ? 4u : 2u) : 0;
        }
        a.engine.previewAvail(p.done);
        if (p.done >= p.total) previewStreamStop(a);
        return;
    }
    // The decode target is free memory only while the preview plays and no
    // instrument has grown the bank into it (load, replace, trim): stop
    // otherwise, never write into instrument data.
    if (!a.engine.previewBufferActive() || a.bank.tail() > p.data) {
        previewStreamStop(a);
        return;
    }
    uint8_t buf[2048];
    while (p.left && budget) {
        uint32_t want = p.left < sizeof(buf) ? p.left : uint32_t(sizeof(buf));
        if (want > budget) want = budget;
        const int32_t got = a.fs().read(p.h, buf, want);
        if (got <= 0) {
            p.left = 0;
            break;
        }
        p.imp.feed(buf, size_t(got));
        p.left -= uint32_t(got);
        budget -= uint32_t(got);
    }
    if (!p.left) {
        p.imp.finish();
        a.engine.previewAvail(p.imp.plan().outFrames);
        previewStreamStop(a);
        return;
    }
    a.engine.previewAvail(p.imp.availableFrames());
}

// .pti on the SD Card list: the payload of the instrument file (stock
// previews it like a WAV, spec 06 §5.4).
bool previewPtiStart(App& a, const char* path) {
    previewStreamStop(a);
    PreviewStream& p = previewStream();
    p.h = a.fs().open(path, IStorage::kRead);
    if (p.h < 0) return false;
    uint8_t hdr[pti::kHeaderBlockSize];
    const int32_t size = a.fs().size(p.h);
    Instrument ins;
    PtiLoadInfo info;
    bool ok = a.fs().read(p.h, hdr, sizeof(hdr)) == int32_t(sizeof(hdr)) &&
              decodePtiHeader(hdr, sizeof(hdr), ins, &info) == FmtStatus::Ok && size > int32_t(info.dataOffset);
    if (ok) {
        const PtiSampleLayout lay = resolvePtiSample(ins.lengthFrames, uint32_t(size) - info.dataOffset, true);
        const size_t cap = a.bank.freeSamples();
        p.stereo = lay.useStereo;
        p.srcFrames = lay.frames;
        p.dataOff = info.dataOffset;
        const size_t fit = cap / (p.stereo ? 2u : 1u);
        p.total = uint32_t(lay.frames < fit ? lay.frames : fit);   // longer than the memory: the first part
        p.done = 0;
        ok = p.total > 0;
    }
    if (!ok) {
        previewStreamStop(a);
        return false;
    }
    p.pti = true;
    p.on = true;
    p.data = a.bank.tail();
    a.engine.previewBuffer(p.data, p.total, p.stereo, 0);
    previewStreamPump(a, 16 * 1024);
    return true;
}

bool previewStreamStart(App& a, const char* path) {
    previewStreamStop(a);
    PreviewStream& p = previewStream();
    p.pti = false;
    p.h = a.fs().open(path, IStorage::kRead);
    if (p.h < 0) return false;
    WavInfo info;
    StorageReader r(a.fs(), p.h);
    WavImportOptions opt;
    opt.stereo = true;   // the SD streamer plays stereo files in stereo (spec 07 Q1)
    const size_t cap = a.bank.freeSamples();
    bool ok = wavParseHeader(r, info) && wavIsSupported(info);
    if (ok) {
        // Longer than the free memory: preview the part that fits [OT].
        const WavImportPlan full = wavPlanImport(info, opt);
        const uint64_t need = uint64_t(full.outFrames) * full.outChannels;
        if (need > cap && need > 0) {
            const uint64_t m = 65535ull * cap / need;
            opt.endMarker = uint16_t(m > 1 ? m - 1 : 1);
            // Marker rounding: step down until the plan fits.
            for (int i = 0; i < 16 && opt.endMarker > 1; ++i) {
                const WavImportPlan t = wavPlanImport(info, opt);
                if (uint64_t(t.outFrames) * t.outChannels <= cap) break;
                --opt.endMarker;
            }
        }
        ok = p.imp.begin(info, opt, a.bank.tail(), cap) == FmtStatus::Ok && p.imp.plan().outFrames > 0;
    }
    if (!ok) {
        previewStreamStop(a);
        return false;
    }
    p.left = info.dataSize;
    p.on = true;
    // The player starts with nothing decoded (silence, never undecoded
    // memory); the first chunk follows at once.
    p.data = a.bank.tail();
    a.engine.previewBuffer(p.data, p.imp.plan().outFrames, p.imp.plan().outChannels == 2, 0);
    previewStreamPump(a, 16 * 1024);
    return true;
}
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool hasExt(const char* s, const char* ext) {
    const size_t a = std::strlen(s), b = std::strlen(ext);
    if (b > a) return false;
    for (size_t i = 0; i < b; ++i) {
        char x = s[a - b + i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (x != ext[i]) return false;
    }
    return true;
}

// Listing context (spec 06 §5.2, issue #54).
struct ListCtx {
    LoaderList* l;
    int count;
    int cap;          // entries this view may hold
    int part;         // 0 whole folder, k >= 1 part k
    int files;        // .wav / .pti seen so far, in directory order
    int dirs;         // subfolders taken (part view)
    size_t used;      // pool bytes
    size_t dirLen;    // strlen(dir)
};
// [I] The files that count towards a part are those the loader lists by
// extension (.wav / .pti, without the WAV header check, so counting stays
// one directory read). Open: whether stock counts other files.
bool countable(const char* n) { return hasExt(n, ".wav") || hasExt(n, ".pti"); }
bool addName(ListCtx& c, const char* name, bool dir) {
    const size_t n = std::strlen(name) + 1;
    if (c.used + n > LoaderList::kPoolBytes) return false;   // pool full [I]
    std::memcpy(c.l->pool + c.used, name, n);
    c.l->off[c.count] = uint16_t(c.used);
    c.l->part[c.count] = 0;
    c.l->isDir[c.count] = dir;
    c.used += n;
    ++c.count;
    return true;
}
bool listFn(const IStorage::DirEntry& e, void* ctx) {
    auto& c = *static_cast<ListCtx*>(ctx);
    if (e.name[0] == '.') return true;
    if (!e.isDir && !countable(e.name)) return true;
    const size_t len = std::strlen(e.name);
    // The joined path must fit the storage path limit [I: such a name could
    // not be opened]; a name is at most 255 characters (FAT).
    const bool fits = len <= size_t(LoaderList::kNameMax) &&
                      (c.dirLen ? c.dirLen + 1 : 0) + len <= size_t(LoaderScreen::kPathMax);
    if (c.part == 0) {
        // Whole folder: the first 101 names (spec 06 §5.2).
        if (c.count >= c.cap) return false;
        if (fits) addName(c, e.name, e.isDir);
        return true;
    }
    if (e.isDir) {
        // Subfolders are never counted and are listed in part 1 only (at
        // most 100 of them [I]).
        if (c.part == 1 && c.dirs < LoaderScreen::kPartFiles && fits && addName(c, e.name, true)) ++c.dirs;
        return true;
    }
    // Part k: the files (k-1)*100+1 .. k*100 in directory order. Later
    // entries can still be subfolders, so the read goes on (part 1).
    const int idx = c.files++;
    if (idx >= (c.part - 1) * LoaderScreen::kPartFiles && idx < c.part * LoaderScreen::kPartFiles && fits)
        addName(c, e.name, false);
    return c.part == 1 || idx < c.part * LoaderScreen::kPartFiles;
}
bool countFn(const IStorage::DirEntry& e, void* ctx) {
    if (!e.isDir && e.name[0] != '.' && countable(e.name)) ++*static_cast<int*>(ctx);
    return true;
}

// spec 06 §5.2: a .wav is listed only when its header passes the WAV check
// (RIFF/WAVE, 1-2 channels, PCM 8/16/24/32 or float 32; stock 0x70bb0).
// The same read fills the file's import probe (frames / channels as the
// import writes them).
bool wavHeaderOk(IStorage& fs, const char* path, LoaderList::Probe& pr) {
    IStorage::Handle h = fs.open(path, IStorage::kRead);
    if (h < 0) return false;
    StorageReader r(fs, h);
    WavInfo info;
    const bool ok = wavParseHeader(r, info) && wavIsSupported(info);
    fs.close(h);
    if (ok) {
        WavImportOptions opt;
        opt.stereo = true;   // as App::importProbe
        const WavImportPlan p = wavPlanImport(info, opt);
        pr.frames = p.outFrames;
        pr.flags = uint8_t(p.outFrames ? LoaderList::kProbeOk : 0) |
                   uint8_t(p.outChannels == 2 ? LoaderList::kProbeStereo : 0);
    }
    return ok;
}

int cmpNoCase(const char* a, const char* b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y || !x) return x - y;
    }
}
}  // namespace

ImportProbe LoaderScreen::probeOf(int i) const {
    const LoaderList::Probe& pr = list_->probe[i];
    ImportProbe p;
    if (!(pr.flags & LoaderList::kProbeOk)) return p;   // frames 0: Unreadable
    p.frames = pr.frames;
    p.stereo = (pr.flags & LoaderList::kProbeStereo) != 0;
    p.pti = (pr.flags & LoaderList::kProbePti) != 0;
    p.playMode = pr.playMode;
    p.loopPoint1 = pr.loop1;
    return p;
}

bool LoaderScreen::entryPath(int i, char* out, int cap) const {
    const int n = dir[0] ? std::snprintf(out, size_t(cap), "%s/%s", dir, entries[i])
                         : std::snprintf(out, size_t(cap), "%s", entries[i]);
    return n >= 0 && n < cap;
}

void LoaderScreen::entryLabel(int i, char* out, int cap) const {
    const int k = entryPart(i);
    if (k > 0)
        std::snprintf(out, size_t(cap), "/%s (%d-%d)", entries[i], (k - 1) * kPartFiles + 1, k * kPartFiles);
    else
        std::snprintf(out, size_t(cap), "%s%s", isDir[i] ? "/" : "", entries[i]);
}

void LoaderScreen::list() {
    // Folders first, then files, alphabetical (spec 06 §5.2). A whole
    // folder lists at most 101 names; part k of a folder with more than 100
    // files lists its files (k-1)*100+1..k*100 (directory order), and part 1
    // also the subfolders. A subfolder with more than 100 files becomes the
    // entries "name (1-100)", "name (101-200)", ... (issue #54).
    LoaderList& L = *list_;
    ListCtx c{&L, 0, part ? kMaxEntries : kMaxUnsplit, part, 0, 0, 0, std::strlen(dir)};
    if (depth_ > 0) addName(c, "..", true);
    const int first = c.count;
    count = 0;   // nothing valid to draw while a progress popup shows
    app_.fs().listDir(dir, listFn, &c);
    // Split the subfolders with more than 100 files: one listDir each.
    const uint32_t t0 = app_.millis();
    uint32_t shown = 0;
    bool popup = false;
    char path[kPathMax + 1];
    int dirsSeen = 0, dirsTotal = 0;
    for (int i = first; i < c.count; ++i) dirsTotal += L.isDir[i] ? 1 : 0;
    for (int i = first; i < c.count; ++i) {
        if (!L.isDir[i] || L.part[i]) continue;
        // [I] A long count on the card shows the progress popup (stock
        // lists in a background job behind it, spec 06 §5.1).
        const uint32_t t = app_.millis();
        if (t - t0 >= 150 && (!popup || t - shown >= 100)) {
            shown = t;
            popup = true;
            ui_.progress("Loading", dirsTotal ? dirsSeen * 100 / dirsTotal : 0);
        }
        ++dirsSeen;
        int files = 0;
        if (!entryPath(i, path, sizeof(path))) continue;
        app_.fs().listDir(path, countFn, &files);
        if (files <= kPartFiles) continue;
        int parts = (files + kPartFiles - 1) / kPartFiles;
        if (parts > c.cap - c.count + 1) parts = c.cap - c.count + 1;   // the view is full
        if (parts <= 1) continue;
        // Entries i+1.. move up by parts-1; the parts share the name.
        const int add = parts - 1;
        for (int j = c.count - 1; j > i; --j) {
            L.off[j + add] = L.off[j];
            L.part[j + add] = L.part[j];
            L.isDir[j + add] = L.isDir[j];
        }
        for (int k = 0; k < parts; ++k) {
            L.off[i + k] = L.off[i];
            L.part[i + k] = uint16_t(k + 1);
            L.isDir[i + k] = true;
        }
        c.count += add;
        i += add;
    }
    if (popup) ui_.progressDone();
    // Insertion sort (records only, the names stay in the pool): folders
    // first, then by name, the parts of a folder in order.
    auto after = [&](int a, uint16_t bo, uint16_t bp, bool bd) {
        if (L.isDir[a] != bd) return !L.isDir[a];
        const int r = cmpNoCase(L.pool + L.off[a], L.pool + bo);
        return r > 0 || (r == 0 && L.part[a] > bp);
    };
    for (int i = first + 1; i < c.count; ++i) {
        const uint16_t to = L.off[i], tp = L.part[i];
        const bool td = L.isDir[i];
        int j = i - 1;
        while (j >= first && after(j, to, tp, td)) {
            L.off[j + 1] = L.off[j];
            L.part[j + 1] = L.part[j];
            L.isDir[j + 1] = L.isDir[j];
            --j;
        }
        L.off[j + 1] = to;
        L.part[j + 1] = tp;
        L.isDir[j + 1] = td;
    }
    // Drop .wav files whose header fails the check; probe the files for the
    // projection (a .pti: one header read).
    int w = first;
    for (int i = first; i < c.count; ++i) {
        LoaderList::Probe pr{0, 0, 0, 0};
        if (!L.isDir[i] && hasExt(entries[i], ".wav")) {
            if (!entryPath(i, path, sizeof(path)) || !wavHeaderOk(app_.fs(), path, pr)) continue;
        } else if (!L.isDir[i] && entryPath(i, path, sizeof(path))) {
            ImportProbe p;
            if (app_.importProbe(path, p))
                pr = {p.frames, p.loopPoint1, p.playMode,
                      uint8_t(LoaderList::kProbeOk | LoaderList::kProbePti | (p.stereo ? LoaderList::kProbeStereo : 0))};
        }
        L.probe[w] = pr;
        if (w != i) {
            L.off[w] = L.off[i];
            L.part[w] = L.part[i];
            L.isDir[w] = L.isDir[i];
        }
        ++w;
    }
    count = w;
    sdCursor = clampi(sdCursor, 0, count > 0 ? count - 1 : 0);
    std::memset(marked_, 0, sizeof(marked_));
    memoryProjection();
}

void LoaderScreen::start(int opt) {
    trim = false;
    if (opt > 0) {
        insCursor = clampi(opt - 1, 0, 47);
        focus = 1;
        sdCursor = 0;
    } else {
        const int ci = app_.currentInstrument();
        insCursor = ci > 47 ? 0 : ci;
    }
    list();
    refreshBar();
}

void LoaderScreen::stop() {
    previewStreamStop(app_);
    trimPlaying_ = false;
    app_.previewAllOff();
    previewing_ = false;
    trim = false;
}

void LoaderScreen::selectionRange(int* first, int* last) const { listRange(focus != 1, first, last); }

void LoaderScreen::listRange(bool sd, int* first, int* last) const {
    const uint8_t* m = sd ? marked_ : insMarked_;
    const int n = sd ? count : 48;
    int f = -1, l = -1;
    for (int i = 0; i < n; ++i)
        if (m[i]) {
            if (f < 0) f = i;
            l = i;
        }
    if (f < 0) f = l = sd ? sdCursor : insCursor;
    *first = f;
    *last = l;
}

void LoaderScreen::memoryProjection() {
    projectSelection();
    // What the bar and the Memory label show (issue #93). Right after an
    // import (Add, trim Import) the actual use, until a cursor or the
    // selection moves: the slot now holds the file, a trimmed import less
    // than the whole file, so a projection of the whole file again is no
    // longer what the user is looking at.
    shown_ = projected_;
    shownOver_ = over_;
    if (!imported_) return;
    int f, l;
    listRange(true, &f, &l);
    if (trim || sdCursor != importSd_ || insCursor != importIns_ || f != importF_ || l != importL_) {
        imported_ = false;
        return;
    }
    shown_ = app_.bank.usedBytes();
    shownOver_ = false;
}

void LoaderScreen::importDone() {
    imported_ = true;
    importSd_ = sdCursor;
    importIns_ = insCursor;
    listRange(true, &importF_, &importL_);
    memoryProjection();
}

void LoaderScreen::projectSelection() {
    // spec 06 §5.4 memory bar.
    App& a = app_;
    uint64_t used = a.bank.usedBytes();
    over_ = false;
    projected_ = uint32_t(used);
    if (trim) {
        // Trimming (issue #93): the Start..End selection at Normal quality
        // replaces the instrument slot (2.x: a stereo file stays stereo).
        const uint64_t n = uint64_t(uint32_t(trimEnd - trimStart)) * trimBytes_ / 65535;
        const uint64_t slot = uint64_t(a.bank.samples(insCursor)) * 2;
        used = used + n > slot ? used + n - slot : 0;
        projected_ = uint32_t(used);
        over_ = used > a.bank.capacityBytes();
        return;
    }
    // The projection always concerns the SD selection loaded at the
    // instrument cursor, whichever list has the focus. From the listing's
    // probes: no file read; the card space is checked at Add (a first
    // f_getfree after a mount scans the whole FAT).
    // 2.x '~' marks (rowRoute_): where each file lands when added at the
    // instrument cursor, a selection row at its slot after the rows before.
    int f, l;
    listRange(true, &f, &l);
    const bool sel = count > 0 && !isDir[sdCursor];
    for (int i = 0; i < count; ++i) {
        rowRoute_[i] = uint8_t(ImportRoute::Unreadable);
        if (isDir[i] || (sel && i >= f && i <= l)) continue;
        rowRoute_[i] = uint8_t(a.importRoute(probeOf(i), insCursor, nullptr, nullptr, UINT64_MAX));
    }
    if (!sel) return;
    int streams = 0;
    for (int i = f; i <= l; ++i) {
        if (isDir[i]) continue;
        const int slot = insCursor + (i - f);
        if (slot >= kSampleInstrumentCount) continue;
        // The whole sample when it fits (or cannot stream: the bar turns
        // red), only its head when it will stream from the card (C14).
        uint32_t b = 0;
        const ImportRoute r = a.importRoute(probeOf(i), slot, &b, nullptr, UINT64_MAX,
                                            int64_t(used) - int64_t(a.bank.usedBytes()), streams);
        rowRoute_[i] = uint8_t(r);
        streams += r == ImportRoute::Streams;
        used += b;
        used -= a.bank.samples(slot) * 2;
    }
    projected_ = uint32_t(used);
    // [I] A resample take holds the free memory it may record into (#7).
    over_ = used + uint64_t(a.bank.reserved()) * 2 > a.bank.capacityBytes();
}

void LoaderScreen::refreshBar() {
    BottomBar& b = ui_.bar;
    App& a = app_;
    b.clear();
    char v[24];
    if (trim) {
        if (trimPlaying_) std::snprintf(v, sizeof(v), "%.3fs", double(trimElapsed_) / 1000.0);
        else std::snprintf(v, sizeof(v), "-");
        b.set(0, "Preview", v);
        std::snprintf(v, sizeof(v), "%.3fs", double(trimStart) / 65535.0 * double(trimLen_ / 44100.0));
        b.set(1, "Start", v);
        std::snprintf(v, sizeof(v), "%.3fs", double(trimEnd) / 65535.0 * double(trimLen_ / 44100.0));
        b.set(2, "End", v);
        b.set(5, "Cancel");
        b.setName(6, "Import", "low quality", importAllowed(true));
        b.setName(7, "Import", "Normal", importAllowed(false));
        b.highlight(trimSel ? 2 : 1, true);
        return;
    }
    const bool onDir = focus == 0 && count > 0 && isDir[sdCursor];
    const bool emptySlot = focus == 1 && !a.instrPresent[insCursor];
    b.set(0, onDir ? "Enter" : "Add", nullptr, focus <= 1);
    b.set(1, "Add next", nullptr, !onDir);
    b.set(2, "Preview", nullptr, !onDir && !emptySlot);
    b.set(3, "Rename", nullptr, focus == 1 && !emptySlot);
    int f, l;
    listRange(true, &f, &l);
    b.set(4, "Import", nullptr, focus <= 1 && count > 0 && f == l && !isDir[sdCursor] && hasExt(entries[sdCursor], ".wav"));
    b.setName(5, "Delete", "unused", focus == 1);
    const float g = audio::trkGain(a.cfg.previewVolume);
    if (g <= 0) std::snprintf(v, sizeof(v), "-inf dB");
    else std::snprintf(v, sizeof(v), "%.2f dB", double(20.0f * std::log10(g)));
    b.set(6, "Preview Vol", v);
    const unsigned pct = unsigned(uint64_t(shown_) * 100 / (a.bank.capacityBytes() ? a.bank.capacityBytes() : 1));
    if (const int n = a.streamedCount()) std::snprintf(v, sizeof(v), "%u%% +%d", pct, n);   // + samples streamed
    else std::snprintf(v, sizeof(v), "%u%%", pct);
    b.set(7, "Memory", v);
    b.highlight(6, focus == 2);
}

bool LoaderScreen::importAllowed(bool lowQuality) const {
    // spec 06 §5.5 trim gating (bytes): n = selected samples, free = capacity
    // - used + 2 * samples of the slot being replaced.
    const App& a = app_;
    const int64_t n = int64_t(uint32_t(trimEnd - trimStart)) * int64_t(trimLen_) / 65535;
    const int64_t fr = int64_t(a.bank.capacityBytes()) - int64_t(a.bank.usedBytes()) +
                       2 * int64_t(a.bank.samples(insCursor));
    return lowQuality ? n < fr - 10000 : 2 * n < fr - 10000;
}

void LoaderScreen::openImport() {
    // §5.5 step 1: F5 with a single non-.pti file; an occupied target slot
    // asks first (F7 Cancel, F8 Yes).
    int f, l;
    listRange(true, &f, &l);
    if (focus > 1 || count == 0 || f != l || isDir[sdCursor] || !hasExt(entries[sdCursor], ".wav")) return;
    {   // The trim view decodes the whole file into sample memory: say so up
        // front when it cannot fit (a long file streams with Add next).
        const App& a = app_;
        char path[kPathMax + 1];
        entryPath(sdCursor, path, sizeof(path));
        const int64_t room = int64_t(a.bank.capacityBytes()) - int64_t(a.bank.usedBytes()) +
                             2 * int64_t(a.bank.samples(insCursor));
        if (int64_t(app_.sampleBytesNeeded(path)) > room) {
            ui_.notify(a.streamingOn() ? "Too long for Import: use Add next (streams)"
                                       : "Too long for Import: not enough sample memory");
            return;
        }
    }
    if (app_.instrPresent[insCursor]) {
        ui_.ask("Replace sample?", "The sample will be replaced", nullptr, "Cancel", "Yes",
                [](void* ctx, int key) {
                    auto* self = static_cast<LoaderScreen*>(ctx);
                    if (key == btn::F8) self->startTrim();
                    self->refreshBar();
                },
                this);
        return;
    }
    startTrim();
}

void LoaderScreen::startTrim() {
    // §5.5 step 2: read the whole file (here: decoded into the free tail of
    // the sample bank), then the trim state.
    App& a = app_;
    char path[kPathMax + 1];
    entryPath(sdCursor, path, sizeof(path));
    WavImportResult res;
    WavImportOptions opt;
    if (wavImportFile(a.fs(), path, opt, a.bank.tail(), uint32_t(a.bank.freeSamples()), res) != FmtStatus::Ok) {
        ui_.notify("Cannot read file");
        return;
    }
    trimLen_ = res.frames;
    trimBytes_ = a.sampleBytesNeeded(path);
    trimStart = 0;
    trimEnd = 65535;
    trimSel = 0;
    trimPlaying_ = false;
    trim = true;
    memoryProjection();
    refreshBar();
}

void LoaderScreen::enter() {
    if (count == 0 || !isDir[sdCursor]) return;
    if (std::strcmp(entries[sdCursor], "..") == 0) {
        char* s = std::strrchr(dir, '/');
        if (s) *s = 0;
        else dir[0] = 0;
        if (depth_ > 0) --depth_;
        // Back to the listing the folder (or part) was entered from, with
        // the cursor on its entry (issue #54: "name (101-200)").
        part = partHist_[depth_];
        sdCursor = cursorHist_[depth_];
        list();
        sdCursor = clampi(cursorHist_[depth_], 0, count - 1);
    } else if (depth_ < kMaxDepth - 1) {
        // A part entry opens that part of the real folder.
        char nd[kPathMax + 1];
        if (!entryPath(sdCursor, nd, sizeof(nd))) return;
        partHist_[depth_] = part;
        cursorHist_[depth_++] = sdCursor;
        part = entryPart(sdCursor);
        std::snprintf(dir, sizeof(dir), "%s", nd);
        sdCursor = 0;
        list();
    }
}

void LoaderScreen::doAdd(bool next) {
    addNext_ = next;
    int f, l;
    listRange(true, &f, &l);
    addFiles_ = addStreamed_ = 0;
    for (int i = f; i <= l; ++i) addFiles_ += isDir[i] ? 0 : 1;
    addLastFailed_ = false;
    addFrom(f, 0);
}

void LoaderScreen::addFrom(int i, int n) {
    App& a = app_;
    int f, l;
    listRange(true, &f, &l);
    char path[kPathMax + 1];
    for (; i <= l; ++i) {
        if (isDir[i]) continue;
        entryPath(i, path, sizeof(path));
        const int slot = insCursor + n;
        if (slot >= kSampleInstrumentCount) break;
        if (a.importRoute(path, slot) == ImportRoute::Streams) {
            // Too big for the memory: converted onto the card and streamed
            // (a background job; the Add goes on when it is done).
            ui_.progressDone();
            addI_ = i + 1;
            addN_ = n + 1;
            if (a.startStreamedImport(path, slot, &LoaderScreen::streamedDone, this)) return;
            ui_.notify(a.importNotice()[0] ? a.importNotice() : "Import failed", 3000);
            addLastFailed_ = true;
            ++n;
            continue;
        }
        ui_.progress("Loading", (i - f) * 100 / (l - f + 1));
        addLastFailed_ = !a.importSample(path, slot);
        if (addLastFailed_) ui_.notify("Import failed");
        ++n;
    }
    ui_.progressDone();
    // 2.x: several files, some streamed: one summary (a failure of the last
    // file keeps its own notice).
    if (addFiles_ > 1 && addStreamed_ > 0 && !addLastFailed_) {
        char msg[40];
        std::snprintf(msg, sizeof(msg), "%d of %d stream from the card", addStreamed_ & 63, addFiles_ & 63);
        ui_.notify(msg, 3000);
    }
    if (addNext_) insCursor = clampi(insCursor + n, 0, 47);
    a.setCurrentInstrument(insCursor);
    importDone();
    refreshBar();
}

void LoaderScreen::streamedDone(void* ctx, bool ok) {
    LoaderScreen& s = *static_cast<LoaderScreen*>(ctx);
    const char* msg = s.app_.importNotice();
    s.addStreamed_ += ok ? 1 : 0;
    s.addLastFailed_ = !ok;
    // One file: why it streams; several: the summary at the end of the Add.
    if (!ok || s.addFiles_ <= 1) s.ui_.notify(msg[0] ? msg : (ok ? "Streams from the card" : "Import failed"), 3000);
    s.addFrom(s.addI_, s.addN_);
}

void LoaderScreen::add(bool next) {
    // spec 06 §5.4 Add / Add next.
    App& a = app_;
    if (focus == 0 && count > 0 && isDir[sdCursor]) {
        enter();
        return;
    }
    // Add works from the SD list and from the instrument list (F1 is enabled
    // with either focus); it always loads the SD selection.
    if (focus > 1 || count == 0) return;
    int f, l;
    listRange(true, &f, &l);
    int files = 0;
    for (int i = f; i <= l; ++i) files += isDir[i] ? 0 : 1;
    if (files == 0) return;
    if (insCursor + files - 1 > 47) {
        ui_.notify("Too many samples selected");
        return;
    }
    memoryProjection();
    if (over_) {
        // [I] Say why when a resample take holds the memory (#7); otherwise
        // the red memory bar shows it, as stock. SD streaming C14: a file
        // too big to stream names the reason (Stream from card Off, ...).
        if (a.bank.reserved()) {
            ui_.notify("Sample memory is reserved by the resample take", 3000);
            return;
        }
        char path[kPathMax + 1];
        for (int i = f, n = 0; i <= l; ++i) {
            if (isDir[i]) continue;
            entryPath(i, path, sizeof(path));
            const ImportRoute r = a.importRoute(path, insCursor + n++);
            const char* why = r == ImportRoute::Off        ? "Not enough memory: turn on Config > Stream from card"
                              : r == ImportRoute::TooMany  ? "16 instruments already stream from the card"
                              : r == ImportRoute::CardFull ? "Not enough space on the SD card"
                              : r == ImportRoute::NoMemory ? "Not enough sample memory"
                                                           : nullptr;
            if (why) {
                ui_.notify(why, 3000);
                break;
            }
        }
        return;
    }
    bool occupied = false;
    for (int i = 0; i < files; ++i) occupied = occupied || a.instrPresent[insCursor + i];
    addNext_ = next;
    if (occupied) {
        ui_.ask("Replace sample(s)?", "Existing samples will be replaced", nullptr, "Cancel", "Yes",
                [](void* ctx, int key) {
                    auto* self = static_cast<LoaderScreen*>(ctx);
                    if (key == btn::F8) self->doAdd(self->addNext_);
                    self->refreshBar();
                },
                this);
        return;
    }
    doAdd(next);
}

void LoaderScreen::moveCursor(int d) {
    const bool sh = ui_.shift();
    imported_ = false;   // a cursor move: the projection again (#93)
    if (focus == 0) {
        if (count == 0) return;
        const int old = sdCursor;
        int nc = clampi(sdCursor + d, 0, count - 1);
        if (sh) {
            // Multi-select: mark rows passed, folders stop the range (spec 06 §5.3).
            int marks = 0;
            for (int i = 0; i < count; ++i) marks += marked_[i];
            if (!marks && !isDir[old]) marked_[old] = 1;
            const int step = nc > old ? 1 : -1;
            for (int i = old + step; nc != old && i != nc + step; i += step) {
                if (isDir[i]) {
                    nc = i - step;
                    break;
                }
                if (marked_[i] && marked_[i - step]) marked_[i - step] = 0;   // moving back un-marks
                else if (marks < 48) marked_[i] = 1;
            }
        } else {
            std::memset(marked_, 0, sizeof(marked_));
        }
        sdCursor = nc;
        pendingProjAt_ = ui_.now() + 300;
        // Preview held: the next file auditions at once, as the instrument
        // list does (user report: Up / Down went quiet while previewing).
        if (previewing_ && nc != old) {
            previewStreamStop(app_);
            app_.previewAllOff();
            if (!isDir[nc]) {
                char path[kPathMax + 1];
                entryPath(nc, path, sizeof(path));
                if (hasExt(path, ".wav")) previewStreamStart(app_, path);
                else if (hasExt(path, ".pti")) previewPtiStart(app_, path);
            }
        }
    } else if (focus == 1) {
        const int old = insCursor;
        insCursor = clampi(insCursor + d, 0, 47);
        if (sh) {
            int marks = 0;
            for (int i = 0; i < 48; ++i) marks += insMarked_[i];
            if (!marks) insMarked_[old] = 1;
            const int step = insCursor > old ? 1 : -1;
            for (int i = old + step; insCursor != old && i != insCursor + step; i += step) {
                if (insMarked_[i] && insMarked_[i - step]) insMarked_[i - step] = 0;   // moving back un-marks
                else if (marks < 48) insMarked_[i] = 1;
            }
        } else {
            std::memset(insMarked_, 0, sizeof(insMarked_));
        }
        app_.setCurrentInstrument(insCursor);
        if (previewing_ && app_.instrPresent[insCursor]) app_.previewOn(App::kAuditionLoader, uint8_t(insCursor), 60, 127);
    } else {
        app_.cfg.previewVolume = uint8_t(clampi(app_.cfg.previewVolume + d, 0, 100));
        app_.markConfigDirty();
    }
}

void LoaderScreen::onButton(uint8_t id, uint8_t state) {
    App& a = app_;
    if (trim) {
        if (id == btn::F1 || id == btn::EncPush) {
            if (state == kPress && !trimPlaying_) {
                const uint32_t s0 = uint32_t(uint64_t(trimStart) * trimLen_ / 65535);
                const uint32_t s1 = uint32_t(uint64_t(trimEnd) * trimLen_ / 65535);
                a.previewFile(a.bank.tail() + s0, s1 > s0 ? s1 - s0 : 0, false);
                trimPlaying_ = true;
                trimPlayAt_ = now();
                trimElapsed_ = 0;
            } else if (state == kRelease) {
                a.previewFile(nullptr, 0, false);
                trimPlaying_ = false;
            }
        } else if (state == kPress) {
            switch (id) {
                case btn::F2: trimSel = 0; break;
                case btn::F3: trimSel = 1; break;
                case btn::F6:
                    trim = false;
                    trimPlaying_ = false;
                    a.previewAllOff();
                    memoryProjection();   // back to the whole file
                    break;
                case btn::F7:
                case btn::F8: {
                    if (!importAllowed(id == btn::F7)) break;   // disabled key does nothing
                    a.previewAllOff();
                    trimPlaying_ = false;
                    char path[kPathMax + 1];
                    entryPath(sdCursor, path, sizeof(path));
                    trim = false;
                    ui_.progress("Importing", 50);
                    if (!a.importSample(path, insCursor, trimStart, trimEnd, id == btn::F7)) ui_.notify("Import failed");
                    ui_.progressDone();
                    importDone();
                    break;
                }
                default: break;
            }
        }
        refreshBar();
        return;
    }
    if (ui_.kb.isOpen()) return;
    switch (id) {
        case btn::F1:
        case btn::Insert:
        case btn::EncPush:
            if (state == kPress) add(false);
            break;
        case btn::F2:
            if (state == kPress) add(true);
            break;
        case btn::F3:
            if (state == kPress) {
                const int fcs = focus == 2 ? prevFocus_ : focus;
                if (fcs == 1 && a.instrPresent[insCursor]) {
                    // One track voice, fixed id 0x60 and note (spec 07 Q7).
                    a.previewOn(App::kAuditionLoader, uint8_t(insCursor), 60, 127);
                    previewing_ = true;
                } else if (fcs == 0 && count > 0 && !isDir[sdCursor]) {
                    // Streamed into the free tail of the sample bank.
                    char path[kPathMax + 1];
                    entryPath(sdCursor, path, sizeof(path));
                    if (hasExt(path, ".wav")) previewStreamStart(a, path);
                    else if (hasExt(path, ".pti")) previewPtiStart(a, path);
                    previewing_ = true;
                }
            } else if (state == kRelease) {
                previewStreamStop(a);
                a.previewAllOff();
                previewing_ = false;
            }
            break;
        case btn::F4:
            if (state == kPress && focus == 1 && a.instrPresent[insCursor]) {
                ui_.kb.open("Rename instrument", a.instruments[insCursor].name, kInstrumentNameLen, "Rename",
                            [](void* ctx, bool ok, const char* text) {
                                auto* self = static_cast<LoaderScreen*>(ctx);
                                bool blank = true;
                                for (const char* p = text; *p; ++p) blank = blank && *p == ' ';
                                if (!ok) return;
                                if (blank) {
                                    self->ui_.notify("Name cannot be empty");
                                    return;
                                }
                                self->app_.renameInstrument(self->insCursor, text);
                            },
                            this);
            }
            break;
        case btn::F5:
            if (state == kPress) openImport();
            break;
        case btn::F6:
            if (state == kPress && focus == 1) {
                ui_.ask("Delete unused instruments?", "Instruments not used in any pattern", nullptr, "Cancel", "Yes",
                        [](void* ctx, int key) {
                            if (key != btn::F8) return;
                            auto* self = static_cast<LoaderScreen*>(ctx);
                            App& ap = self->app_;
                            bool used[kSampleInstrumentCount] = {};
                            ap.commitCurrentPattern();
                            for (int n = 1; n <= kPatternCount; ++n) {
                                if (!ap.patternPresent(n)) continue;
                                // All 16 tracks (2.x): tracks 12..15 only when they hold a pool block.
                                for (int t = 0; t < kTrackCount; ++t) {
                                    const Track* tr = ap.findStoredTrack(n, t);
                                    if (!tr) continue;
                                    for (const auto& s : tr->steps)
                                        if (s.note >= 0 && s.instrument < kSampleInstrumentCount) used[s.instrument] = true;
                                }
                            }
                            int removed = 0;
                            for (int i = 0; i < kSampleInstrumentCount; ++i)
                                if (ap.instrPresent[i] && !used[i]) {
                                    ap.deleteInstrument(i);
                                    ++removed;
                                }
                            char msg[32];
                            std::snprintf(msg, sizeof(msg), "%d deleted", removed);
                            self->ui_.notify(msg);
                            self->refreshBar();
                        },
                        this);
            }
            break;
        case btn::F7:
            // §5.3: press focuses the volume bar; after a hold tick the release
            // returns to the SD list (momentary use).
            if (state == kPress) {
                if (focus != 2) prevFocus_ = focus;
                focus = 2;
                f7Hold_ = false;
            } else if (state == kHold) {
                f7Hold_ = true;
            } else if (state == kRelease) {
                if (f7Hold_ && prevFocus_ == 0) focus = 0;
                f7Hold_ = false;
            }
            break;
        case btn::Delete:
            if (state != kPress) break;
            if (focus == 2) {
                a.cfg.previewVolume = 86;
                a.markConfigDirty();
            } else if (focus == 1 && [&] {
                           int f, l, n = 0;
                           listRange(false, &f, &l);
                           for (int i = f; i <= l; ++i) n += a.instrPresent[i] ? 1 : 0;
                           return n > 0;
                       }()) {
                ui_.ask("Delete instrument(s)?", "Selected instruments will be deleted", nullptr, "Cancel", "Yes",
                        [](void* ctx, int key) {
                            if (key != btn::F8) return;
                            auto* self = static_cast<LoaderScreen*>(ctx);
                            int f, l;
                            self->selectionRange(&f, &l);
                            for (int i = f; i <= l; ++i) self->app_.deleteInstrument(i);
                            self->memoryProjection();
                            self->refreshBar();
                        },
                        this);
            }
            break;
        case btn::Copy:
            if (state != kPress || focus != 1) break;
            if (!ui_.shift()) {
                int f, l;
                selectionRange(&f, &l);
                copySrc_ = f;
                copyCount_ = l - f + 1;
                ui_.notify("Copied");
            } else if (copySrc_ >= 0) {
                int ok = 0;
                for (int i = 0; i < copyCount_ && insCursor + i < kSampleInstrumentCount; ++i)
                    if (a.instrPresent[copySrc_ + i] && a.copyInstrument(copySrc_ + i, insCursor + i)) ++ok;
                if (ok < copyCount_) ui_.notify("Not all instruments copied");
            }
            break;
        case btn::Left:
            if (state != kPress) break;
            a.previewAllOff();
            if (focus == 1) {
                focus = 0;
                std::memset(insMarked_, 0, sizeof(insMarked_));
            } else if (focus == 0) {
                sdCursor = 0;
                std::memset(marked_, 0, sizeof(marked_));
            } else {
                focus = 1;
            }
            memoryProjection();
            break;
        case btn::Right:
            if (state != kPress) break;
            if (focus < 2) {
                prevFocus_ = focus;
                ++focus;
            }
            memoryProjection();
            break;
        case btn::Up:
        case btn::Down:
            // Lists: Up = cursor - 1; volume bar: Up = volume + 1 (§5.3).
            if (state == kPress) moveCursor((id == btn::Up) == (focus == 2) ? 1 : -1);
            break;
        case btn::Shift:
            break;
        default:
            Screen::onButton(id, state);
            break;
    }
    refreshBar();
}

void LoaderScreen::onPad(uint8_t pad, uint8_t state) {
    (void)pad;
    (void)state;   // pads are only used by the keyboard here (spec 06 §5.4)
}

#if !(defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE)
bool LoaderScreen::encoderOnList() const {
    return !trim;   // SD and instrument lists (trim markers are values)
}
#endif

void LoaderScreen::onEncoder(int d) {
    if (trim) {
        // 109 units per detent; Start <= End - 2 (spec 06 §5.5).
        if (trimSel == 0) trimStart = uint16_t(clampi(int(trimStart) + d * 109, 0, int(trimEnd) - 2));
        else trimEnd = uint16_t(clampi(int(trimEnd) + d * 109, int(trimStart) + 2, 65535));
        memoryProjection();
        refreshBar();
        return;
    }
    if (focus == 0) d = clampi(d, -5, 5);
    moveCursor(d);
    refreshBar();
}

void LoaderScreen::update(uint32_t now) {
    // SD preview: decode ahead of the player (32 KB per pass, about 0.2 s
    // of stereo audio; the player waits if it ever catches up).
    previewStreamPump(app_, 32 * 1024);
    if (trim && trimPlaying_ && now - trimPlayAt_ >= trimElapsed_ + 11) {
        // §5.5: the playhead advances every >= 11 ms; playback stops at End.
        trimElapsed_ = now - trimPlayAt_;
        const uint32_t s0 = uint32_t(uint64_t(trimStart) * trimLen_ / 65535);
        const uint32_t s1 = uint32_t(uint64_t(trimEnd) * trimLen_ / 65535);
        const uint32_t durMs = uint32_t(uint64_t(s1 > s0 ? s1 - s0 : 0) * 1000 / 44100);
        if (trimElapsed_ >= durMs) {
            trimElapsed_ = durMs;
            app_.previewFile(nullptr, 0, false);
            trimPlaying_ = false;
        }
        refreshBar();
        ui_.dirty();
    }
    if (pendingProjAt_ && int32_t(now - pendingProjAt_) >= 0) {
        pendingProjAt_ = 0;
        memoryProjection();
        refreshBar();
        ui_.dirty();
    }
}

namespace {
// Column header label (spec 20 §4.5): 080808 band x 3..297 (+ 300 k),
// y 32..58 with 1-px soft edges, bold font-4 caption centred [S snap 009].
void headerLabel(Gfx& g, const Theme& th, int x, const char* text) {
    const uint32_t c = 0x080808;
    wg::panel(g, th, x + 2, 32, 294, 27, c, false);
    wg::blendFill(g, x + 2, 31, 295, 1, c, wg::kEdgeA);
    wg::blendFill(g, x + 2, 59, 295, 1, c, wg::kEdgeA);
    g.text(x + 150, 45, kFont4, th.slot(slot::kList + 4), text, IDisplay::kOptCenter);   // ink y 40..51 [S]
}
// List box (x, 61, 297, 362) under a header, with its soft top row.
void listBox(Gfx& g, const Theme& th, int x) {
    const uint32_t c = th.slot(slot::kListRed + 3);
    wg::panel(g, th, x, 61, 297, 362, c);
    wg::blendFill(g, x, 60, 298, 1, c, wg::kEdgeA);
}
int gTop[2] = {0, 0};
}  // namespace

void LoaderScreen::draw(Gfx& g) {
    App& a = app_;
    const Theme& th = ui_.th();
    ui_.drawTitle(g, "Sample Loader");   // no path [S snap 009]
    if (trim) {
        // Waveform of the decoded file (min/max per pixel column).
        g.fill(100, 116, 600, 218, th.panelDark);
        const int16_t* d = a.bank.tail();
        if (trimLen_ > 0) {
            g.rectsBegin(th.slot(slot::kWave));   // white wave (spec 20 §3.12)
            for (int x = 0; x < 600; x += 2) {
                const uint32_t s0 = uint32_t(uint64_t(x) * trimLen_ / 600), s1 = uint32_t(uint64_t(x + 2) * trimLen_ / 600);
                int mn = 0, mx = 0;
                const uint32_t stride = (s1 - s0) / 32 + 1;
                for (uint32_t s = s0; s < s1 && s < trimLen_; s += stride) {
                    if (d[s] < mn) mn = d[s];
                    if (d[s] > mx) mx = d[s];
                }
                g.rectsAdd(100 + x, 225 - mx * 100 / 32768, 1, (mx - mn) * 100 / 32768 + 1);
            }
            g.rectsEnd();
        }
        const int xs = 100 + int(uint32_t(trimStart) * 600 / 65536), xe = 100 + int(uint32_t(trimEnd) * 600 / 65536);
        g.vline(xs, 116, 218, trimSel == 0 ? th.accent : th.text);
        g.vline(xe, 116, 218, trimSel == 1 ? th.accent : th.text);
        if (trimPlaying_ && trimLen_) {
            const uint64_t f0 = uint64_t(trimStart) * trimLen_ / 65535 + uint64_t(trimElapsed_) * 44100 / 1000;
            g.vline(100 + int(f0 * 600 / trimLen_), 116, 218, th.good);
        }
        return;
    }
    // spec 20 §4.5: header labels (font 4, bg 080808, y 32), lists
    // (1 / 301, 61, 297, 362); browser selection red, instrument selection
    // white; folders "/name" white; value bars (601 / 701, 29) with white
    // fills [S snap 009].
    headerLabel(g, th, 1, "SD Card");
    headerLabel(g, th, 301, "Instruments");
    for (int l = 0; l < 2; ++l) {
        const int x = l ? 301 : 1;
        listBox(g, th, x);
        wg::ListView v;
        v.x = x;
        v.y = 61;
        v.w = 297;
        v.h = 362;
        v.background = false;
        v.set = slot::kListRed;   // marked (multi-selection) text green
        v.count = l ? 48 : count;
        v.sel = l ? insCursor : sdCursor;
        v.selStyle = l ? wg::ListSel::kWhite : wg::ListSel::kRed;
        gTop[l] = wg::listFollow(v.sel, v.count, wg::listVisibleRows(v.h), gTop[l]);
        v.top = gTop[l];
        v.ctx = this;
        if (l) {
            v.item = [](void* c, int i, char* out, int cap) {
                const App& ap = static_cast<LoaderScreen*>(c)->app_;
                // [S snap 009]; 2.x: '~' marks a sample streamed from the card.
                if (ap.instrPresent[i]) std::snprintf(out, size_t(cap), "%d.%s%s", i + 1, ap.streamed(i) ? "~" : "", ap.instruments[i].name);
                else std::snprintf(out, size_t(cap), "%d.", i + 1);
            };
            v.marked = [](void* c, int i) { return static_cast<LoaderScreen*>(c)->insMarked_[i] != 0; };
        } else {
            v.item = [](void* c, int i, char* out, int cap) {
                const LoaderScreen& ls = *static_cast<LoaderScreen*>(c);
                ls.entryLabel(i, out, cap);
            };
            v.marked = [](void* c, int i) { return static_cast<LoaderScreen*>(c)->marked_[i] != 0; };
            // 2.x: '~' at the right, a file that will stream from the card.
            v.tag = [](void* c, int i) -> const char* {
                return static_cast<LoaderScreen*>(c)->rowRoute_[i] == uint8_t(ImportRoute::Streams) ? "~" : nullptr;
            };
        }
        wg::list(g, th, v);
    }
    if (focus <= 1) wg::panelFocus(g, th, focus ? 301 : 1, 297);
    // Preview volume and memory bars (white fills; the memory bar shows the
    // projected use above the current one in the "above reference" colour,
    // red when it does not fit [I]).
    wg::valueBar(g, th, 601, 29, 97, 394, a.cfg.previewVolume / 100.0f);
    if (focus == 2) wg::panelFocus(g, th, 601, 97);
    const float cap = float(a.bank.capacityBytes() ? a.bank.capacityBytes() : 1);
    const float cur = float(a.bank.usedBytes()) / cap;
    const float proj = float(shown_) / cap;
    wg::ValueBarStyle ms;
    if (shownOver_) {
        ms.fill = th.slot(slot::kFocusFrame);
        wg::valueBar(g, th, 701, 29, 97, 394, 1.0f, ms);
    } else {
        // The bar shows the smaller of the two; the difference is a red band
        // when loading adds memory and a green one when it frees memory
        // (manual p.44-45; MANUAL_AUDIT_2026-10-23 #20). Fill geometry of
        // valueBar: x + 11 .. x + w - 11, from row y + h - 15 up by
        // round((h - 24) v) rows.
        const float lo = proj < cur ? proj : cur, hi = proj < cur ? cur : proj;
        wg::valueBar(g, th, 701, 29, 97, 394, lo, ms);
        auto row = [](float v) { return 29 + 394 - 15 - int((394 - 24) * (v > 1 ? 1 : v) + 0.5f); };
        const int top = row(hi), bot = row(lo);
        if (bot > top) g.fill(701 + 11, top + 1, 97 - 22, bot - top, proj > cur ? th.alert : th.good);
    }
}

}  // namespace ot
