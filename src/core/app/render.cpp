// Offline render to WAV (spec 05 §2.12 File > Export, spec 06 §1.11 Render
// selection, spec 04 §6.1 render jobs). The engine renders faster than real
// time (AudioEngine::exportWav): before every block it calls App::preBlock,
// which advances the sequencer by kRenderBlockSeconds; the real-time callback
// outputs silence meanwhile. Master files or 14 stems (track1..8, reverb,
// delay, master, synth_1..3; Mini names, spec 21 §2).
#include <cstdio>
#include <cstring>

#include "app/render.h"

#include "app/app.h"
#include "formats/wav.h"
#include "sdram_bss.h"
#include "ui/buttons.h"

namespace ot {

namespace {
constexpr uint32_t kBlock = audio::kBlock;
constexpr uint32_t kMaxSeconds = 30 * 60;
constexpr uint32_t kTailBlocks = 44100 / kBlock;   // ~1 s release tail
constexpr uint32_t kPollBlocks = 16;                 // input poll interval (~46 ms of audio)

// ---- Cancel (spec 05 appendix E1.4: while rendering every label is blank
// except btn7 "Cancel"; also during "Samples"). The render blocks the main
// loop, so the input queue is polled here: an F8 press cancels, releases are
// kept and handed to the UI afterwards (keys held when the render started
// see their release), everything else is dropped.
constexpr const char* kMasterName = "master.wav";   // spec 16 §A.12
// Progress title naming the export type (stock storage-state titles,
// docs/STRINGS_DIFF_1.9.3.md item 6).
const char* g_renderTitle = kExportingPattern;

// Folder part of a "<folder>/master.wav" export path, or nullptr when `path`
// is not one (a Selection file).
bool masterFolder(const char* path, char* out, size_t cap) {
    const char* slash = std::strrchr(path, '/');
    if (!slash || std::strcmp(slash + 1, kMasterName) != 0) return false;
    std::snprintf(out, cap, "%.*s", int(slash - path), path);
    return true;
}

bool g_cancel = false;
InputEvent g_deferred[16];
int g_deferredCount = 0;
OT_SDRAM_BSS SdramLazy<BottomBar> g_savedBarMem;   // SDRAM on the device (CPU round 3)

void cancelBegin(App& a) {
    g_cancel = false;
    g_deferredCount = 0;
    g_savedBarMem.get() = a.ui.bar;
    a.ui.bar.clear();
    a.ui.bar.set(btn::F8, "Cancel");
}

bool pollCancel(App& a) {
    if (g_cancel) return true;
    const HalRefs& h = a.hal();
    if (h.service) h.service();
    if (!h.input) return false;
    InputEvent ev;
    int guard = 0;
    while (guard++ < 32 && h.input->poll(ev)) {
        if (ev.type == InputEvent::kButtonDown && ev.id == btn::F8) {
            g_cancel = true;
        } else if ((ev.type == InputEvent::kButtonUp || ev.type == InputEvent::kPadUp) &&
                   g_deferredCount < int(sizeof(g_deferred) / sizeof(g_deferred[0]))) {
            g_deferred[g_deferredCount++] = ev;
        }
    }
    return g_cancel;
}

void cancelEnd(App& a) {
    a.ui.bar = g_savedBarMem.get();
    a.ui.dirty();
    const int n = g_deferredCount;
    g_deferredCount = 0;
    for (int i = 0; i < n; ++i) {
        const InputEvent& ev = g_deferred[i];
        const bool held = ev.type == InputEvent::kButtonUp ? a.ui.isDown(ev.id) : a.ui.padDown(ev.id);
        if (held) a.ui.handle(ev, a.millis());
    }
}

// Remove a cancelled render's output: the .wav, or the stems folder with
// its files (OpenTracker; spec 05 appendix E1.4 does not say what the stock
// abort 0x1f864 leaves on the card).
void removeOutput(IStorage& f, const char* path, bool stems) {
    if (!stems) {
        f.remove(path);
        char dir[256];
        if (masterFolder(path, dir, sizeof(dir))) f.remove(dir);   // the per-export folder
        return;
    }
    static const char* const kStemNames[] = {"track1.wav", "track2.wav", "track3.wav", "track4.wav",
                                             "track5.wav", "track6.wav", "track7.wav", "track8.wav",
                                             "reverb.wav", "delay.wav",  "master.wav", "synth_1.wav",
                                             "synth_2.wav", "synth_3.wav"};
    char p[320];
    for (const char* n : kStemNames) {
        std::snprintf(p, sizeof(p), "%s/%s", path, n);
        if (f.exists(p)) f.remove(p);
    }
    f.remove(path);
}
}  // namespace

bool exportWasCancelled() { return g_cancel; }

bool App::exportName(ExportKind kind, char* out, int cap, bool stems) {
    IStorage& f = fs();
    f.mkdir(kExportDir);
    char dir[128];
    const char* proj = project.name[0] ? project.name : "Project";
    std::snprintf(dir, sizeof(dir), "%s/%s", kExportDir, proj);
    f.mkdir(dir);
    if (kind == ExportKind::Selection) {
        std::snprintf(dir, sizeof(dir), "%s/%s/Selection", kExportDir, proj);
        f.mkdir(dir);
    }
    // spec 16 §A.12 (spec 02 §8.3, spec 05 appendix E1.4): every Song /
    // Pattern export gets a new folder, NNNN = first free 0001..9999, the
    // pattern number unpadded (_P3_). A master export writes master.wav into
    // it, a stems export track1..8, reverb, delay, master and synth_1..3.wav.
    char folder[256];
    for (int n = 1; n <= 9999; ++n) {
        switch (kind) {
            case ExportKind::Song:
                std::snprintf(folder, sizeof(folder), "%s/%.60s_Song_%s%04d", dir, proj, stems ? "S" : "", n);
                break;
            case ExportKind::Pattern:
                std::snprintf(folder, sizeof(folder), "%s/%.60s_P%d_%s%04d", dir, proj, patternNumber(), stems ? "S" : "", n);
                break;
            case ExportKind::Selection:
                // Pattern editor "Render selection": "Selection" + 4 digits (spec 06 §1.11).
                std::snprintf(out, size_t(cap), "%s/Selection%04d%s", dir, n, stems ? "" : ".wav");
                if (!f.exists(out)) return true;
                continue;
        }
        if (f.exists(folder)) continue;
        if (stems) std::snprintf(out, size_t(cap), "%s", folder);
        else std::snprintf(out, size_t(cap), "%s/%s", folder, kMasterName);
        return true;
    }
    return false;
}

int App::exportSamples() {
    // spec 05 appendix E1.4 "Samples": <folder>/<name>/export/samples/<..>.wav
    // (format "%s/%s/export/samples/%s%s.wav", spec 02 §1), 16-bit 44.1 kHz
    // (spec 02 §8.3). SPEC GAP: the two name parts are not identified;
    // OpenTracker writes "<NN>_<instrument name>" (NN = slot 01..48).
    g_cancel = false;
    IStorage& f = fs();
    char base[256], path[320];
    const char* folder = project.folder[0] ? project.folder : kProjectsDir;
    const char* name = project.name[0] ? project.name : "Project";
    f.mkdir(folder);
    std::snprintf(base, sizeof(base), "%.150s/%.60s", folder, name);
    f.mkdir(base);
    std::snprintf(base, sizeof(base), "%.150s/%.60s/export", folder, name);
    f.mkdir(base);
    std::snprintf(base, sizeof(base), "%.150s/%.60s/export/samples", folder, name);
    if (!f.mkdir(base)) return -1;
    int written = 0, used = 0;
    for (int i = 0; i < kSampleInstrumentCount; ++i)
        if (instrPresent[i] && bank.loaded(i)) ++used;
    cancelBegin(*this);
    ui.progress("Exporting Samples", 0);
    int result = 0;
    for (int i = 0; i < kSampleInstrumentCount && result == 0; ++i) {
        if (!instrPresent[i] || !bank.loaded(i)) continue;
        if (pollCancel(*this)) break;
        char nm[kInstrumentNameLen + 1];
        std::snprintf(nm, sizeof(nm), "%s", instruments[i].name[0] ? instruments[i].name : "instrument");
        for (char* c = nm; *c; ++c)
            if (*c == '/' || *c == '\\' || *c == ':' || *c == '*' || *c == '?' || *c == '"' || *c == '<' || *c == '>' ||
                *c == '|')
                *c = '_';
        char pre[8];
        std::snprintf(pre, sizeof(pre), "%02d_", i + 1);
        std::snprintf(path, sizeof(path), "%s/%s%s.wav", base, pre, nm);
        if (f.exists(path)) f.remove(path);
        const uint32_t frames = bank.frames(i);
        const bool st = bank.stereo(i);
        const int16_t* L = bank.left(i);
        const int16_t* R = bank.right(i);
        WavWriter w;
        if (!w.open(f, path, st ? 2 : 1)) {
            result = -1;
            break;
        }
        bool ok = true;
        int16_t buf[2 * 1024];
        for (uint32_t pos = 0; pos < frames && ok && !pollCancel(*this); pos += 1024) {
            const uint32_t n = frames - pos < 1024 ? frames - pos : 1024;
            if (st) {
                for (uint32_t k = 0; k < n; ++k) {
                    buf[2 * k] = L[pos + k];
                    buf[2 * k + 1] = R[pos + k];
                }
                ok = w.write(buf, n);
            } else {
                ok = w.write(L + pos, n);
            }
        }
        const bool closed = w.close();
        if (g_cancel) {
            f.remove(path);   // partial file of a cancelled export
            break;
        }
        if (!closed || !ok) {
            result = -1;
            break;
        }
        ++written;
        ui.progress("Exporting Samples", written * 100 / (used ? used : 1));
    }
    ui.progressDone();
    cancelEnd(*this);
    if (g_cancel) return -1;
    return result < 0 ? result : written;
}

bool App::exportRunning(void* ctx) {
    App& a = *static_cast<App*>(ctx);
    ExportJob& j = a.job_;
    if (!j.started) {
        // Start inside the render loop: beginOffline() has reset the voices.
        j.started = true;
        a.patternEnds_ = 0;
        switch (j.kind) {
            case ExportKind::Song: a.seq.playSong(0); break;
            case ExportKind::Pattern: a.seq.play(0); break;
            case ExportKind::Selection:
                if (j.sel) a.seq.playSelection(j.sel->s0, j.sel->s1, j.sel->t0, j.sel->t1, false);
                else a.seq.play(0);
                break;
        }
        return true;
    }
    ++j.blocks;
    if ((j.blocks % kPollBlocks) == 0 && pollCancel(a)) return false;
    if ((j.blocks & 1023) == 0) {
        const int pct = j.kind == ExportKind::Song && j.songLen ? a.seq.songPosition() * 100 / j.songLen
                                                                : int(j.blocks / 1024 % 100);
        a.ui.progress(g_renderTitle, pct);
    }
    if (j.tail == 0) {
        bool done;
        if (j.kind == ExportKind::Song) done = a.patternEnds_ > 0 && a.seq.songPosition() == 0;
        else done = a.patternEnds_ > 0 || !a.seq.isPlaying();
        if (done) {
            a.seq.stop();   // release tail follows
            j.tail = 1;
        }
        return true;
    }
    return ++j.tail <= kTailBlocks;
}

bool App::exportAudio(ExportKind kind, const char* path, const SelRect* sel, bool stems) {
    seq.stop();
    if (perfCc) perfCc->perfRevertKept();   // never render with live Performance FX (spec 16 §A.15)
    allVoicesOff();
    if (engineDirty_) applyEngineSettings();
    rendering_ = true;
    commitCurrentPattern();
    const uint8_t startPattern = patternNumber();
    job_ = ExportJob{};
    job_.kind = kind;
    job_.sel = sel;
    while (job_.songLen < kSongSlots && project.playlist[job_.songLen]) ++job_.songLen;
    if (!stems) {
        char dir[256];
        if (masterFolder(path, dir, sizeof(dir))) fs().mkdir(dir);   // spec 16 §A.12
    }
    seq.setRendering(true);
    cancelBegin(*this);
    g_renderTitle = kind == ExportKind::Song      ? (stems ? kExportingSongStems : kExportingSong)
                    : kind == ExportKind::Pattern ? (stems ? kExportingPatternStems : kExportingPattern)
                                                  : kExportingSelection;
    ui.progress(g_renderTitle, 0);
    const uint32_t maxBlocks = uint32_t(uint64_t(kMaxSeconds) * 44100 / kBlock);
    const int32_t n = engine.exportWav(fs(), path, stems, maxBlocks, &App::exportRunning, this,
                                       /*resampleTo44100=*/cfg.exportRate != 0);   // Config > Export sample rate
    seq.stop();
    seq.setRendering(false);
    allVoicesOff();
    engine.clearEffects();
    // Restore the edited pattern (song render moves through the playlist).
    if (patternNumber() != startPattern) {
        project.setCurrentPattern(startPattern);
        loadPattern(startPattern, pattern(), true);
        attachSequencer();
    }
    rendering_ = false;
    ui.progressDone();
    if (g_cancel) removeOutput(fs(), path, stems);
    cancelEnd(*this);
    return n > 0 && !g_cancel;
}

}  // namespace ot
