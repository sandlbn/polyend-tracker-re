// App side of the Sample Recorder (spec 05 §2.11, appendix C §2; spec 02
// §8.3 recordings): input routing, output folder and file naming, WAV write.
#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "fmath.h"
#include "formats/wav.h"

namespace ot {

bool App::selectInput(uint8_t source, uint8_t gain) {
    if (source > 3) return false;
    if (source == 1 && !hal_.radio) return false;   // radio: needs a tuner driver
    capture.setChannelMode(cfg.lineInChannel);
    if (hal_.selectInput) return hal_.selectInput(hal_.inputCtx, source, gain);
    return source == 0;   // without a routing hook only the line input exists
}

namespace {
bool countEntry(const IStorage::DirEntry&, void*) { return true; }
}  // namespace

bool App::recordingsFolder(char* out, int cap) {
    IStorage& f = fs();
    // "Recordings", then the highest existing "Recordings N" (N >= 2).
    int n = 1;
    for (int k = 2; k < 1000; ++k) {
        char p[32];
        std::snprintf(p, sizeof(p), "Recordings %d", k);
        if (!f.exists(p)) break;
        n = k;
    }
    auto name = [&](int k) {
        if (k <= 1) std::snprintf(out, size_t(cap), "Recordings");
        else std::snprintf(out, size_t(cap), "Recordings %d", k);
    };
    name(n);
    if (f.exists(out)) {
        // More than 98 entries: start the next folder (spec 02 §8.3).
        const int32_t entries = f.listDir(out, &countEntry, nullptr);
        if (entries > 98) name(n + 1);
    }
    return f.mkdir(out);
}

bool App::recordingName(const char* folder, char* out, int cap) {
    for (int k = 1; k <= 999; ++k) {
        char p[96];
        std::snprintf(p, sizeof(p), "%s/recording%03d.wav", folder, k);
        if (!fs().exists(p)) {
            std::snprintf(out, size_t(cap), "recording%03d", k);
            return true;
        }
    }
    return false;
}

int App::firstFreeSampleSlot() const {
    for (int i = 0; i < kSampleInstrumentCount; ++i)
        if (!instrPresent[i]) return i;
    return -1;
}

bool App::writeWav(const char* path, const int16_t* mono, uint32_t frames) {
    IStorage& f = fs();
    if (f.exists(path)) f.remove(path);
    WavWriter w;
    if (!w.open(f, path, 1)) return false;
    bool ok = true;
    for (uint32_t i = 0; i < frames && ok; i += 4096) {
        const uint32_t n = frames - i < 4096 ? frames - i : 4096;
        ok = w.write(mono + i, n);
    }
    return w.close() && ok;
}

// ---------------------------------------------------------------------------
// [I] Live resampling (app/resample.h)

int App::nextResampleSlot() const {
    int top = bank.lastLoaded();
    for (int i = kSampleInstrumentCount - 1; i > top; --i)
        if (instrPresent[i]) {
            top = i;
            break;
        }
    return top + 1 < kSampleInstrumentCount ? top + 1 : -1;
}

int App::resampleTarget(uint32_t n, bool* inPlace, bool* fits) const {
    if (inPlace) *inPlace = false;
    if (fits) *fits = true;
    const int into = int(resampleCfg.into) - 1;
    if (into >= 0 && into < kSampleInstrumentCount) {
        if (streamed(into)) {
            // Only its head is in the bank: never overwritten in place.
        } else if (bank.loaded(into)) {
            const uint32_t old = bank.frames(into);
            const uint32_t d = n > old ? n - old : old - n;
            if (bank.stereo(into) && (n == 0 || d <= kResampleFitFrames)) {
                if (inPlace) *inPlace = true;
                return into;
            }
        } else if (into > bank.lastLoaded()) {
            return into;   // committing there moves no sample
        }
        if (fits) *fits = false;
    }
    return nextResampleSlot();
}

App::ResampleAction App::resampleToggle() {
    switch (resampler.state()) {
        case Resampler::kIdle: {
            if (sampleBankLent || rendering_ || (jobBusy() && jobOp() != JobOp::Autosave)) return ResampleAction::kBusy;
            // The free bank tail is in use: a Sample Recorder recording or a
            // streamed Sample Loader preview decoding into it.
            if (capture.active() || engine.previewBufferActive()) return ResampleAction::kBusy;
            if (resampleTarget(0, nullptr, nullptr) < 0) return ResampleAction::kNoSlot;
            // All free sample memory is held for the recording (two planar
            // halves); loads meanwhile see none and cannot touch it.
            const size_t free = bank.freeSamples() & ~size_t(1);
            if (free < 2u * 44100u || free > 0xFFFFFFFEu || !bank.setReserved(free)) return ResampleAction::kNoMemory;
            resampler.arm(bank.reservedArea(), uint32_t(free), kResampleBars[resampleCfg.length % ResampleSettings::kLengths],
                          resampleCfg.source);
            // Line in is tapped in App::preBlock, from the input itself.
            if (resampleCfg.source != audio::AudioEngine::kTapLineIn) engine.setResampleTap(&resampler, resampleCfg.source);
            return ResampleAction::kArmed;
        }
        case Resampler::kArmed:
            resampleCancel();
            return ResampleAction::kCancelled;
        case Resampler::kRecording:
            resampler.requestStop();
            return ResampleAction::kStopping;
        default:
            return ResampleAction::kBusy;
    }
}

void App::resampleCancel() {
    engine.setResampleTap(nullptr, 0);
    if (resampler.state() >= Resampler::kRecording) reportResample(ResampleReport::kCancelled, -1);
    resampler.reset();
    bank.setReserved(0);
}

void App::serviceResample() {
    if (!resampler.busy()) return;
    // The bank was cleared (project load / New project) or lent (Games):
    // the reserved memory is gone.
    if (sampleBankLent || bank.reserved() == 0) {
        engine.setResampleTap(nullptr, 0);
        if (resampler.state() >= Resampler::kRecording) reportResample(ResampleReport::kCancelled, -1);
        resampler.reset();
        bank.setReserved(0);
        ui.notify("Resample cancelled");
        return;
    }
    if (resampler.state() == Resampler::kDone) finishResample();
}

// Snapshot of the take for the diag log. Call after the tap is removed (the
// audio side writes no more stats) and before resampler.reset().
void App::reportResample(uint8_t outcome, int slot) {
    ResampleReport& r = lastResample;
    ++r.seq;
    r.source = resampler.source();
    r.end = resampler.end();
    r.outcome = outcome;
    r.slot = int8_t(slot);
    r.frames = resampler.frames();
    r.stats = resampler.stats();
}

void App::finishResample() {
    engine.setResampleTap(nullptr, 0);
    const uint32_t n = resampler.frames();
    const Resampler::End end = resampler.end();
    bool inPlace = false, fits = true;
    const int slot = resampleTarget(n, &inPlace, &fits);
    auto release = [this] {
        resampler.reset();
        bank.setReserved(0);
    };
    if (n < 441) {   // under 10 ms: nothing worth keeping
        reportResample(ResampleReport::kEmpty, -1);
        release();
        ui.notify("Resample: nothing recorded");
        return;
    }
    if (slot < 0) {
        reportResample(ResampleReport::kNoSlot, -1);
        release();
        ui.notify("Resample: no free instrument slot after the last one", 4000);
        return;
    }
    uint32_t frames = n;
    if (inPlace) {
        // Into N, same length: overwrite its audio where it is (voices on it
        // keep their pointers and play on into the new take), trimmed or
        // padded with silence to the slot's length.
        frames = bank.frames(slot);
        const uint32_t m = n < frames ? n : frames;
        int16_t* d = bank.data(slot);
        for (int c = 0; c < 2; ++c) {
            int16_t* lane = d + uint32_t(c) * frames;
            std::memcpy(lane, c ? resampler.right() : resampler.left(), size_t(m) * sizeof(int16_t));
            if (m < frames) std::memset(lane + m, 0, size_t(frames - m) * sizeof(int16_t));
        }
        reportResample(ResampleReport::kOk, slot);
        release();
    } else {
        // Planar stereo at the tail (left block, right block). Moving down never
        // overlaps the right half's source (the area starts at or after tail()).
        int16_t* dst = bank.tail();
        std::memmove(dst, resampler.left(), size_t(n) * sizeof(int16_t));
        std::memmove(dst + n, resampler.right(), size_t(n) * sizeof(int16_t));
        reportResample(ResampleReport::kOk, slot);
        release();
        // Above every loaded instrument: commitTail() moves no playing sample.
        if (!bank.commitTail(slot, n, true)) {
            lastResample.outcome = ResampleReport::kFailed;
            ui.notify("Resample failed");
            return;
        }
    }
    // Into N keeps the slot's settings (an empty slot that has them too);
    // a new instrument starts from the defaults.
    const bool keep = slot == int(resampleCfg.into) - 1 && instrPresent[slot];
    Instrument ins = instruments[slot];
    if (!keep) {
        ins.setDefault();
        const uint8_t src = resampler.source();   // as armed (From may have changed)
        if (src == 0) std::snprintf(ins.name, sizeof(ins.name), "Resample Master");
        else if (src == audio::AudioEngine::kTapLineIn) std::snprintf(ins.name, sizeof(ins.name), "Resample Line in");
        else std::snprintf(ins.name, sizeof(ins.name), "Resample Track %d", src);
    }
    ins.lengthFrames = frames;
    ins.stereo = true;
    // Whole bars when it ended on a bar: placement repeats every that many.
    const uint32_t bars = end == Resampler::kEndBars ? resampler.bars()
                        : end == Resampler::kEndStopped ? resampler.barsDone() : 0;
    // Sliced: slices on the beat / step grid (equal, or where hits start),
    // Beat Slice (note 24 + i plays slice i at its own pitch; the pads play
    // slices 1..48).
    uint16_t sliceSteps[kSliceCount];
    const int slices = resampleSliceSteps(bank.left(slot), bank.right(slot), frames, bars, sliceSteps);
    if (slices > 0) {
        const uint64_t total = uint64_t(bars) * uint64_t(resampleBarSteps());
        ins.playMode = uint8_t(PlayMode::BeatSlice);
        ins.sliceCount = uint8_t(slices);
        for (int i = 0; i < slices; ++i)
            ins.slices[i] = uint16_t((uint64_t(sliceSteps[i]) * 65535u + total / 2) / total);
    }
    instruments[slot] = ins;
    instrPresent[slot] = true;
    markInstrumentDirty(slot, true);
    setCurrentInstrument(slot);
    resampler.reset();
    char msg[64];
    if (!fits)
        std::snprintf(msg, sizeof(msg), "Instr %d is another length: saved as instrument %d", resampleCfg.into, slot + 1);
    else if (bars)
        std::snprintf(msg, sizeof(msg), "Resampled %lu bar%s -> instrument %d%s", static_cast<unsigned long>(bars),
                      bars == 1 ? "" : "s", slot + 1, inPlace ? " (replaced)" : "");
    else
        std::snprintf(msg, sizeof(msg), "Resampled %.1f s -> instrument %d%s", double(n) / 44100.0, slot + 1,
                      end == Resampler::kEndFull ? " (memory full)" : inPlace ? " (replaced)" : "");
    if (fits && slices) {
        const size_t l = std::strlen(msg);
        std::snprintf(msg + l, sizeof(msg) - l, ", %d slices", slices);
    } else if (fits && resampleCfg.slices) {
        const size_t l = std::strlen(msg);
        std::snprintf(msg + l, sizeof(msg) - l, ", not sliced");   // no whole bars
    }
    ui.notify(msg, fits ? 3000 : 4000);
    placeResample(slot, bars, sliceSteps, slices);
    ui.dirty();
}

int App::resampleSliceGrid(uint32_t bars, int& count) const {
    count = 0;
    if (resampleCfg.slices == 0 || bars == 0) return 0;
    const int total = int(bars) * resampleBarSteps();
    const bool beat = resampleCfg.slices == 1 || resampleCfg.slices == 3;
    int unit = beat ? cfg.tsDen + 1 : 1;
    while (total / unit > kSliceCount) unit *= 2;
    count = total / unit;
    return count > 0 ? unit : 0;
}

int App::resampleSliceSteps(const int16_t* l, const int16_t* r, uint32_t n, uint32_t bars, uint16_t* steps) const {
    int count = 0;
    int unit = resampleSliceGrid(bars, count);
    if (!unit) return 0;
    if (resampleCfg.slices <= 2) {   // equal
        for (int i = 0; i < count; ++i) steps[i] = uint16_t(i * unit);
        return count;
    }
    // At hits: the finest grid whose hits fit (more hits than slices: the
    // grid doubles and only the hits on its cells count).
    const int total = int(bars) * resampleBarSteps();
    unit = resampleCfg.slices == 3 ? cfg.tsDen + 1 : 1;
    for (;; unit *= 2) {
        uint16_t cells[kSliceCount];
        const int found = findHitCells(l, r, n, total / unit, cells, kSliceCount);
        if (found <= kSliceCount || total / unit <= kSliceCount) {
            const int c = found < kSliceCount ? found : kSliceCount;
            for (int i = 0; i < c; ++i) steps[i] = uint16_t(cells[i] * unit);
            return c;
        }
    }
}

void App::placeResample(int slot, uint32_t bars, const uint16_t* sliceSteps, int slices) {
    const int t = int(resampleCfg.place) - 1;
    if (t < 0 || t >= Sequencer::kVoiceTracks) return;
    Pattern& p = pattern();
    const int len = int(p.tracks[0].lastStep) + 1;
    const int every = int(bars) * resampleBarSteps();
    pushUndo();
    // Note 60 plays the sample at its own pitch (audio_tables kUnityNote).
    // Sliced: slice i (note 24 + i) on the step where it starts, repeating
    // with the take; the steps in between stay empty (the slice plays on).
    int next = 0;   // the next slice to place within the current repeat
    for (int s = 0; s < len; ++s) {
        const int off = every > 0 ? s % every : s;
        if (off == 0) next = 0;
        int note = -1;
        if (slices > 0) {
            if (next < slices && sliceSteps[next] == off) note = 24 + next++;
        } else if (off == 0) {
            note = 60;
        }
        if (note < 0) continue;
        Step& st = p.tracks[t].steps[s];
        st.note = int8_t(note);
        st.instrument = uint8_t(slot);
    }
    patternEdited();
    if (every > len) ui.notify("Resample is longer than the pattern: placed once", 3000);
}

bool App::writeWav(const char* path, const int16_t* l, const int16_t* r, uint32_t frames) {
    if (!r) return writeWav(path, l, frames);
    IStorage& f = fs();
    if (f.exists(path)) f.remove(path);
    WavWriter w;
    if (!w.open(f, path, 2)) return false;
    // 2.x stereo recording: the planar lanes interleaved (L, R) through a
    // small transient chunk (2 KB of stack, no sample-memory use).
    constexpr uint32_t kChunk = 512;
    int16_t buf[2 * kChunk];
    bool ok = true;
    for (uint32_t i = 0; i < frames && ok; i += kChunk) {
        const uint32_t n = frames - i < kChunk ? frames - i : kChunk;
        for (uint32_t k = 0; k < n; ++k) {
            buf[2 * k] = l[i + k];
            buf[2 * k + 1] = r[i + k];
        }
        ok = w.write(buf, n);
    }
    return w.close() && ok;
}

uint32_t App::previewStep(int note) {
    // 2^((note - 60) / 12) as 16.16: 2048 (note 0) .. ~3.1e6 (note 127).
    const int n = note < 0 ? 0 : (note > 127 ? 127 : note);
    return uint32_t(fm::kNoteRatio[n] * 65536.0 + 0.5);
}

}  // namespace ot
