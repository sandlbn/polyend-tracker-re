// Synth framework (synth_bank.h).
#include "audio/synth_bank.h"

#include <cmath>
#include <cstring>

#include "audio/cpu_meter.h"
#include "fastrun.h"
#include "formats/patch_pb.h"

namespace ot {
namespace audio {

namespace {
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Step FX ids (spec 03 §3.6).
constexpr uint8_t kFxDelaySend = 30, kFxPanning = 31, kFxReverbSend = 32, kFxRandVol = 7, kFxVol = 18, kFxGlideId = 19, kFxMicro = 34, kFxTuneId = 40, kFxSlideU = 41,
                  kFxSlideD = 42;
}  // namespace

static_assert(sizeof(SynthVoice) <= 160, "engine-visible voice record");

SynthBank::SynthBank() { reset(); }

void SynthBank::reset() {
    for (auto& v : voices_) v = Voice{};
    for (auto& o : owner_) o = -1;
    for (auto& p : pend_) p = Pending{};
    for (auto& d : defer_) d = Deferred{};
    nDefer_ = 0;
    starts_ = 0;
    flushDue_ = false;
    for (int t = 0; t < kSynthAllTracks; ++t) {
        trackVoice_[t] = -1;
        trackPitch_[t] = -1.0f;
    }
    for (int s = 0; s < kSynthSlots; ++s) {
        Slot& sl = slots_[s];
        for (auto& o : sl.ovr) o = -1;
        sl.macroDirty = true;
        sl.ntouched = 0;
        rebind_[s] = true;
        recDirty_[s] = true;   // the current record (default: spec 21 §3.3) is re-applied
        recApplied_[s] = false;
    }
    activeVoices_ = 0;
    aud_.req = 0;
    for (int k = 0; k < kSynthLiveTracks; ++k) liveSeen_[k] = uint8_t(live_[k]);
}

// --- main loop -----------------------------------------------------------------

void SynthBank::bindPatch(int s, const SynthPatch* p) {
    if (s < 0 || s >= kSynthSlots) return;
    slots_[s].patch = p;
    rebind_[s] = true;
}

void SynthBank::patchChanged(int s) {
    if (s >= 0 && s < kSynthSlots) rebind_[s] = true;
}

void SynthBank::setSlotRecord(int s, const SynthSlotRecord& r) {
    if (s < 0 || s >= kSynthSlots) return;
    pendRec_[s] = r;
    recDirty_[s] = true;
}

bool SynthBank::slotAvailable(int s) const {
    const SynthSlotRecord& r = slotRecord(s);
    if (r.model >= uint32_t(kSynthModelCount)) return false;
    return synthEngineFor(synthEngineId(SynthModel(r.model))) != nullptr;
}

SynthSlotView SynthBank::slotView(int s) const {
    s = clampSlot(s);
    const Slot& sl = slots_[s];
    SynthSlotView v;
    const SynthSlotRecord& r = recDirty_[s] ? pendRec_[s] : sl.rec;
    const bool okModel = r.model < uint32_t(kSynthModelCount);
    v.engine = okModel ? synthEngineId(SynthModel(r.model)) : 0;
    v.model = okModel ? synthModelName(SynthModel(r.model)) : "";
    v.available = okModel && synthEngineFor(v.engine) != nullptr;
    v.patchBound = sl.patch != nullptr;
    v.patchMismatch = !sl.patch || sl.patch->engine != v.engine;
    v.polyphony = int(r.polyphony > 8 ? 8 : r.polyphony);
    const int c = cap();
    v.playable = v.polyphony < c ? v.polyphony : c;
    v.limited = c < v.polyphony;
    v.sounding = voicesOf(s);
    for (int k = 0; k < kSynthSeqMacros; ++k) {
        v.overridden[k] = sl.ovr[k] >= 0;
        v.macro[k] = v.overridden[k] ? uint8_t(sl.ovr[k]) : (r.macros[k] > 127 ? 127 : r.macros[k]);
    }
    v.lastVelocity = sl.lastVel;
    return v;
}

void SynthBank::auditionOn(int s, uint8_t note, int8_t volume) {
    if (s < 0 || s >= kSynthSlots) return;
    aud_.slot = uint8_t(s);
    aud_.note = note > 127 ? 127 : note;
    aud_.vol = volume;
    aud_.req = 1;
}

void SynthBank::auditionOff() { aud_.req = 2; }

void SynthBank::liveOn(int k, int s, uint8_t note, int8_t volume) {
    if (k < 0 || k >= kSynthLiveTracks || s < 0 || s >= kSynthSlots) return;
    const uint8_t ser = ++liveSerial_[k];
    live_[k] = uint32_t(ser) | 1u << 8 | uint32_t(s & 3) << 9 | uint32_t(note & 127) << 11 |
               uint32_t(uint8_t(volume)) << 24;
}

void SynthBank::liveOff(int k) {
    if (k < 0 || k >= kSynthLiveTracks) return;
    const uint8_t ser = ++liveSerial_[k];
    live_[k] = uint32_t(ser);
}

// Audio block (render): start / release what the main loop asked for.
void SynthBank::liveDrain() {
    for (int k = 0; k < kSynthLiveTracks; ++k) {
        const uint32_t w = live_[k];
        if (uint8_t(w) == liveSeen_[k]) continue;
        liveSeen_[k] = uint8_t(w);
        const int track = kSynthLiveTrack0 + k;
        if (w & (1u << 8)) {
            SynthNoteOn n;
            n.slot = uint8_t((w >> 9) & 3);
            n.note = uint8_t((w >> 11) & 127);
            n.volume = int8_t(uint8_t(w >> 24));
            noteOnNow(track, n);
        } else {
            noteOff(track, NoteOffMode::Off);
        }
    }
}

// --- internal state ---------------------------------------------------------------

int SynthBank::cap() const {
    const int c = gov_ ? gov_->cap() : kSynthDefaultVoices;
    return c < 0 ? 0 : (c > kSynthMaxVoices ? kSynthMaxVoices : c);
}

int SynthBank::playable(int s) const {
    const Slot& sl = slots_[s];
    if (!sl.eng) return 0;
    const int poly = int(sl.rec.polyphony > 8 ? 8 : sl.rec.polyphony);
    const int c = cap();
    return poly < c ? poly : c;
}

float SynthBank::trackPeak(int t) {
    if (t < 0 || t >= kSynthTracks) return 0.0f;
    const float p = trackPeak_[t];
    trackPeak_[t] = 0.0f;
    return p;
}

float SynthBank::slotPeak(int s) {
    if (s < 0 || s >= kSynthSlots) return 0.0f;
    const float p = peak_[s];
    peak_[s] = 0.0f;
    return p;
}

int SynthBank::voicesOf(int s) const {
    int n = 0;
    for (const Voice& v : voices_) n += (v.state == kOn && v.v.slot == s) ? 1 : 0;
    return n;
}

int SynthBank::sounding() const {
    int n = 0;
    for (const Voice& v : voices_) n += v.state == kOn ? 1 : 0;
    return n;
}

int SynthBank::busyRecords() const {
    int n = 0;
    for (const Voice& v : voices_) n += v.state != kFree ? 1 : 0;
    return n;
}

void SynthBank::sync() {
    for (int s = 0; s < kSynthSlots; ++s) {
        Slot& sl = slots_[s];
        if (recDirty_[s]) {
            recDirty_[s] = false;
            // The App re-sends every record after any project edit
            // (applyEngineSettings): an unchanged record changes nothing
            // (no macro recompute, no engine refresh in this block).
            if (!recApplied_[s] || std::memcmp(&sl.rec, &pendRec_[s], sizeof(SynthSlotRecord)) != 0) {
            recApplied_[s] = true;
            const uint32_t oldModel = sl.rec.model;
            sl.rec = pendRec_[s];
            if (sl.rec.model != oldModel) rebind_[s] = true;
            for (int k = 0; k < kSynthSeqMacros; ++k) sl.page[k] = sl.rec.macros[k] > 127 ? 127 : sl.rec.macros[k];
            sl.macroDirty = true;
            sl.gainLin = std::isfinite(sl.rec.gainDb) ? dbToGain(clampf(sl.rec.gainDb, -96.0f, 12.0f)) : 0.0f;
            if (sl.rec.gainDb <= -96.0f) sl.gainLin = 0.0f;
            // Balance (linear, spec 04 §4.3) and SEND law, cached per record;
            // the send gains include the (L + R) / 2 of the mono effect input.
            const float pan = std::isfinite(sl.rec.pan) ? clampf(sl.rec.pan, -100.0f, 100.0f) : 0.0f;
            sl.mixL = sl.gainLin * (pan > 0.0f ? 1.0f - pan * 0.01f : 1.0f);
            sl.mixR = sl.gainLin * (pan < 0.0f ? 1.0f + pan * 0.01f : 1.0f);
            auto send = [](float v) { return std::isfinite(v) ? sendGain(int(clampf(v, 0.0f, 100.0f) + 0.5f)) : 0.0f; };
            sl.mixD = 0.5f * send(sl.rec.delaySend);
            sl.mixE = 0.5f * send(sl.rec.reverbSend);
            }
        }
        if (rebind_[s]) {
            rebind_[s] = false;
            bindEngine(s);
        }
    }
    if (aud_.req) {
        const uint8_t req = aud_.req;
        aud_.req = 0;
        if (req == 1) {
            SynthNoteOn n;
            n.slot = aud_.slot;
            n.note = aud_.note;
            n.volume = aud_.vol;
            noteOnNow(kSynthAuditionTrack, n);
        } else {
            noteOff(kSynthAuditionTrack, NoteOffMode::Off);
        }
    }
}

void SynthBank::bindEngine(int s) {
    Slot& sl = slots_[s];
    const bool okModel = sl.rec.model < uint32_t(kSynthModelCount);
    const int32_t id = okModel ? synthEngineId(SynthModel(sl.rec.model)) : 0;
    ISynthEngine* e = synthEngineFor(id);
    if (e && override_) e = override_;
    sl.mismatch = !sl.patch || sl.patch->engine != id;
    if (sl.mismatch) e = nullptr;
    const bool restart = e != sl.eng || id != sl.engineId;
    if (restart) {
        // A new engine starts clean: the slot's voices stop (fade) first.
        for (int r = 0; r < kSynthMaxVoices; ++r)
            if (voices_[r].state == kOn && voices_[r].v.slot == s) fade(r);
        sl.eng = e;
        sl.engineId = id;
        // A new engine starts from zeroed scratch, not the previous engine's
        // state (PERC keeps its seed across slotReset and read leftovers).
        for (uint8_t& b : sl.scratch.bytes) b = 0;
        if (e) e->slotReset(sl.scratch);
    }
    reloadParams(s, restart);
}

float SynthBank::paramBase(const Slot& sl, int i) const { return synthParamGet(*sl.patch, i); }

void SynthBank::reloadParams(int s, bool jump) {
    Slot& sl = slots_[s];
    if (prof_) BlockProfile::inc(prof_->nReload);
    SynthParams& p = sl.p;
    p.engine = sl.engineId;
    sl.ntgt = sl.ntgt7 = 0;
    sl.volK = 0.0f;
    sl.volIdx = -1;
    if (!sl.patch || sl.mismatch) {
        p.count = 0;
        p.moving = false;
        ++p.serial;
        return;
    }
    int n = 0;
    const PatchParamRange* t = patchEngineParams(sl.engineId, &n);
    // A live edit ramps from where the last block ended (like a macro move).
    if (!jump) catchUp(sl);
    p.count = n;
    for (int i = 0; i < n; ++i) {
        p.end[i] = paramBase(sl, i);
        if (jump) p.start[i] = p.end[i];
    }
    p.moving = !jump;
    sl.fullCopy = !jump;
    sl.fresh = !jump;
    ++p.serial;
    sl.volIdx = patchVolumeParam(sl.engineId);
    if (sl.volIdx >= 0 && sl.volIdx < n) {
        sl.volMin = t[sl.volIdx].min;
        sl.volMax = t[sl.volIdx].max;
    }
    // Compile the macro targets (empty targets -1 inside count are skipped).
    for (int k = 0; k < kSynthSeqMacros; ++k) {
        const PatchMacro& m = sl.patch->macros[k];
        for (int j = 0, c = synthMacroTargets(m); j < c; ++j) {
            const int i = m.targets[j].param;
            const float a = patchBitsToFloat(m.targets[j].amountBits);
            if (i < 0 || i >= n || !std::isfinite(a)) continue;
            sl.tgt[sl.ntgt++] = Slot::Tgt{uint8_t(i), uint8_t(k), a * (t[i].max - t[i].min)};
        }
    }
    const PatchMacro& m7 = sl.patch->macros[kPatchMacroVelocity];
    for (int j = 0, c = synthMacroTargets(m7); j < c; ++j) {
        const int i = m7.targets[j].param;
        const float a = patchBitsToFloat(m7.targets[j].amountBits);
        if (i < 0 || i >= n || !std::isfinite(a)) continue;
        if (i == sl.volIdx) sl.volK += a;   // per voice (voiceGain)
        else sl.tgt7[sl.ntgt7++] = Slot::Tgt{uint8_t(i), uint8_t(kPatchMacroVelocity), a};
    }
    sl.macroDirty = true;
    applyMacros(s);
}

// M1..M6 (and slot-level M7 targets other than Volume) into p.end[] of the
// targeted parameters; untargeted ones keep the patch value.
void SynthBank::applyMacros(int s) {
    Slot& sl = slots_[s];
    if (!sl.patch || sl.mismatch || (!sl.macroDirty)) return;
    sl.macroDirty = false;
    uint8_t val[kSynthSeqMacros];
    float m[kSynthSeqMacros];
    for (int k = 0; k < kSynthSeqMacros; ++k) {
        val[k] = sl.ovr[k] >= 0 ? uint8_t(sl.ovr[k]) : sl.page[k];
        m[k] = synthMacroM(val[k], sl.patch->macros[k].bipolar);
        sl.applied[k] = val[k];
    }
    if (sl.ntgt == 0 && sl.ntgt7 == 0) return;
    if (prof_) BlockProfile::inc(prof_->nMacro);
    SynthParams& p = sl.p;
    catchUp(sl);   // a move between blocks ramps from the last block's end
    const int32_t e = sl.engineId;
    // The targets' values before: an unchanged result moves nothing (no
    // ramp, no serial change, so the engines keep their cached coefficients).
    float before[kSynthSeqMacros * kPatchMacroTargets + kPatchMacroTargets];
    int nb = 0;
    for (int j = 0; j < sl.ntgt; ++j) before[nb++] = p.end[sl.tgt[j].param];
    for (int j = 0; j < sl.ntgt7; ++j) before[nb++] = p.end[sl.tgt7[j].param];
    for (int j = 0; j < sl.ntgt; ++j) p.end[sl.tgt[j].param] = paramBase(sl, sl.tgt[j].param);
    for (int j = 0; j < sl.ntgt7; ++j) p.end[sl.tgt7[j].param] = paramBase(sl, sl.tgt7[j].param);
    for (int j = 0; j < sl.ntgt; ++j) p.end[sl.tgt[j].param] += sl.tgt[j].k * m[sl.tgt[j].macro];
    for (int j = 0; j < sl.ntgt; ++j) p.end[sl.tgt[j].param] = synthParamClamp(e, sl.tgt[j].param, p.end[sl.tgt[j].param]);
    if (sl.ntgt7) {
        float mul[kPatchMacroTargets];
        for (int j = 0; j < sl.ntgt7; ++j) {
            mul[j] = 1.0f;
            for (int q = 0; q < sl.ntgt7; ++q)
                if (sl.tgt7[q].param == sl.tgt7[j].param) mul[j] += sl.tgt7[q].k * (sl.lastVel - 1.0f);
        }
        for (int j = 0; j < sl.ntgt7; ++j) {
            const int i = sl.tgt7[j].param;
            bool first = true;   // apply each parameter once
            for (int q = 0; q < j; ++q) first &= sl.tgt7[q].param != i;
            if (first) p.end[i] = synthParamClamp(e, i, p.end[i] * mul[j]);
        }
    }
    bool changed = false;
    nb = 0;
    for (int j = 0; j < sl.ntgt; ++j) changed |= before[nb++] != p.end[sl.tgt[j].param];
    for (int j = 0; j < sl.ntgt7; ++j) changed |= before[nb++] != p.end[sl.tgt7[j].param];
    if (!changed) return;
    // Ramp the moved parameters across this block; start[] catches up next block.
    sl.ntouched = 0;
    for (int j = 0; j < sl.ntgt; ++j) sl.touched[sl.ntouched++] = sl.tgt[j].param;
    for (int j = 0; j < sl.ntgt7 && int(sl.ntouched) < int(sizeof(sl.touched)); ++j) sl.touched[sl.ntouched++] = sl.tgt7[j].param;
    p.moving = true;
    sl.fresh = true;
    ++p.serial;
}

void SynthBank::catchUp(Slot& sl) {
    SynthParams& p = sl.p;
    if (!p.moving || sl.fresh) return;
    if (sl.fullCopy) {
        if (prof_) BlockProfile::inc(prof_->nFullCopy);
        std::memcpy(p.start, p.end, sizeof(float) * uint32_t(p.count));
        sl.fullCopy = false;
    } else {
        for (int j = 0; j < sl.ntouched; ++j) p.start[sl.touched[j]] = p.end[sl.touched[j]];
    }
    p.moving = false;
}

// Framework gain of a voice: the patch Volume (after M1..M6) with Macro 7.
float SynthBank::voiceGain(const Slot& sl, const Voice& v) const {
    if (v.state != kOn) return 0.0f;
    if (sl.volIdx < 0 || sl.volIdx >= sl.p.count) return v.vel;   // PERC: velocity scales the voice
    const float vol = sl.p.end[sl.volIdx];
    if (sl.volK == 0.0f) return vol;
    return clampf(vol * (1.0f + sl.volK * (v.vel - 1.0f)), sl.volMin, sl.volMax);
}

int SynthBank::freeRecord() const {
    for (int r = 0; r < kSynthMaxVoices; ++r)
        if (voices_[r].state == kFree) return r;
    return -1;
}

// Inside slot s: a voice on the same note, else the quietest voice in its
// release, else the oldest (2x-05 §3.2).
int SynthBank::victimInSlot(int s, uint8_t note) const {
    int same = -1, rel = -1, old = -1;
    for (int r = 0; r < kSynthMaxVoices; ++r) {
        const Voice& v = voices_[r];
        if (v.state != kOn || v.v.slot != s) continue;
        if (v.v.note == note && (same < 0 || v.age < voices_[same].age)) same = r;
        if (!v.v.gate && (rel < 0 || v.v.level < voices_[rel].v.level ||
                          (v.v.level == voices_[rel].v.level && v.age < voices_[rel].age)))
            rel = r;
        if (old < 0 || v.age < voices_[old].age) old = r;
    }
    return same >= 0 ? same : (rel >= 0 ? rel : old);
}

// Governor rule (2x-02 §3.3): release phase quietest first, then the
// quietest overall, ties by age. Quietest = envelope level x gain x slot gain.
int SynthBank::victimGlobal() const {
    int best = -1;
    bool bestRel = false;
    float bestQ = 0.0f;
    for (int r = 0; r < kSynthMaxVoices; ++r) {
        const Voice& v = voices_[r];
        if (v.state != kOn) continue;
        const bool rel = !v.v.gate;
        const float q = v.v.level * v.g * slots_[v.v.slot].gainLin;
        bool take = best < 0;
        if (!take) {
            if (rel != bestRel) take = rel;
            else if (q != bestQ) take = q < bestQ;
            else take = v.age < voices_[best].age;
        }
        if (take) {
            best = r;
            bestRel = rel;
            bestQ = q;
        }
    }
    return best;
}

void SynthBank::fade(int r, bool steal) {
    Voice& v = voices_[r];
    if (v.state != kOn) return;
    if (prof_) BlockProfile::inc(prof_->nSteal);
    // A steal victim that has not rendered yet (started in this block) ends
    // at once: no fade blip, no wasted render. Not PERC: its kit outlives
    // the voice and is released in the voice's last render.
    if (steal && v.fresh && v.pending < 0 && slots_[v.v.slot].engineId != kSynthEnginePerc) {
        if (v.track >= 0 && trackVoice_[v.track] == r) trackVoice_[v.track] = -1;
        v.track = -1;
        v.state = kFree;
        v.fresh = 0;
        --activeVoices_;
        return;
    }
    v.state = kFading;
    if (v.track >= 0 && trackVoice_[v.track] == r) trackVoice_[v.track] = -1;
    v.track = -1;
}

bool SynthBank::stealOne() {
    const int r = victimGlobal();
    if (r < 0) return false;
    fade(r);
    ++steals_;
    return true;
}

void SynthBank::startOn(int r, int track, int s, const SynthNote& n, float bend, float target, float step) {
    Voice& V = voices_[r];
    Slot& sl = slots_[s];
    if (V.state == kFree) ++activeVoices_;
    std::memset(V.v.state, 0, sizeof(V.v.state));
    V.v.slotScratch = &sl.scratch;
    V.v.slot = uint8_t(s);
    V.v.note = n.note;
    V.v.gate = true;
    V.v.fading = false;
    V.v.level = 0.0f;
    V.v.bend = V.v.bendStart = bend;
    V.bendTarget = target;
    V.bendStep = step;
    V.vel = n.velocity;
    V.sendD = noteSendD_;
    V.sendR = noteSendR_;
    V.pan = notePan_;
    V.age = ++serial_;
    V.state = kOn;
    V.pending = -1;
    V.fresh = 1;
    V.track = int8_t(track);
    owner_[r] = int8_t(track);
    trackVoice_[track] = int8_t(r);
    if (prof_) BlockProfile::inc(prof_->nSynOn[s]);
    V.g = voiceGain(sl, V);   // start at the target (the engine envelope attacks)
    sl.eng->noteOn(V.v, sl.p, n);
}

void SynthBank::releaseTrack(int track) {
    const int r = trackVoice_[track];
    if (r < 0) return;
    trackVoice_[track] = -1;
    Voice& V = voices_[r];
    if (V.state != kOn) return;
    V.track = -1;
    if (V.v.gate) {
        V.v.gate = false;
        const Slot& sl = slots_[V.v.slot];
        if (sl.eng) sl.eng->noteOff(V.v, sl.p);
    }
}

// Pitch FX of a note step (spec 03 §3.6 as for samples): Tune (40, -24..+24
// st), Micro-tune (34, -99..+99 cents), Glide (19, from the track's previous
// note over glideFxMs), Slide up / down (41 / 42, v/16 semitones over a step).
void SynthBank::notePitchFx(const SynthNoteOn& n, float prevPitch, bool hasPrev, float& bend, float& target,
                            float& step) const {
    float off = 0.0f;
    uint8_t glide = 0, slideV = 0;
    int slideDir = 0;
    const StepFx fx[2] = {n.fxA, n.fxB};
    for (const StepFx& f : fx) {
        switch (f.type) {
            case kFxTuneId: off += float(clampf(float(f.value) - 24.0f, -24.0f, 24.0f)); break;
            case kFxMicro: off += clampf(float(f.value) - 99.0f, -99.0f, 99.0f) * 0.01f; break;
            case kFxGlideId: glide = f.value; break;
            case kFxSlideU: case kFxSlideD:
                slideV = f.value;
                slideDir = f.type == kFxSlideU ? 1 : -1;
                break;
            default: break;
        }
    }
    bend = target = off;
    step = 0.0f;
    if (glide && hasPrev) {
        bend = prevPitch - float(n.note) + off;
        const float blocks = float(glideFxMs(glide)) * 1000.0f / kLiveBlockUs;
        step = blocks > 1.0f ? std::fabs(bend - target) / blocks : 0.0f;
    }
    if (slideV) {
        target = bend + float(slideDir) * float(slideV) / 16.0f;
        const float blocksPerStep = (15.0f / tempo_) * 1.0e6f / kLiveBlockUs;
        step = std::fabs(target - bend) / (blocksPerStep > 1.0f ? blocksPerStep : 1.0f);
    }
}

// --- sequencer sink ------------------------------------------------------------------

void SynthBank::noteOn(int track, const SynthNoteOn& n) {
    if (track < 0 || track >= kSynthTracks) return;
    sync();
    if (flushDue_) flushDeferred();
    noteOnNow(track, n, false);
}

SynthBank::Deferred* SynthBank::deferredOf(int track) {
    for (int i = 0; i < nDefer_; ++i)
        if (defer_[i].track == track) return &defer_[i];
    return nullptr;
}

// The note-ons queued in the previous block start first, in their order,
// each followed by its FX and note-off of that block.
void SynthBank::flushDeferred() {
    flushDue_ = false;
    const int n = nDefer_;
    nDefer_ = 0;
    for (int i = 0; i < n; ++i) {
        const Deferred d = defer_[i];
        defer_[i] = Deferred{};
        if (d.track < 0) continue;
        noteOnNow(d.track, d.n, true);
        for (int k = 0; k < d.nfx; ++k) applyFx(d.track, d.fx[k][0], d.fx[k][1]);
        if (d.off) noteOff(d.track, NoteOffMode(d.off));
    }
}

void SynthBank::noteOnNow(int track, const SynthNoteOn& n, bool forced) {
    if (n.slot >= kSynthSlots) return;
    const int s = n.slot;
    Slot& sl = slots_[s];
    // Note-on spreading (header): a later note of a track whose note waits
    // replaces it; past the per-block start cap a note that needs a new
    // voice record waits one block.
    if (!forced) {
        Deferred* d = deferredOf(track);
        bool wait = d != nullptr;
        if (!wait && startCap_ > 0 && starts_ >= startCap_ && playable(s) > 0) {
            bool reuse = false;
            if (playable(s) == 1)
                for (int q = 0; q < kSynthMaxVoices && !reuse; ++q)
                    reuse = voices_[q].state == kOn && voices_[q].v.slot == s;
            wait = !reuse;
        }
        if (wait) {
            if (!d) d = &defer_[nDefer_++];   // one entry per track: never full
            *d = Deferred{};
            d->track = int8_t(track);
            d->n = n;
            if (prof_) BlockProfile::inc(prof_->nDeferred);
            return;
        }
    }
    const int P = playable(s);
    if (P == 0) {   // slot off, unavailable model or no patch: silent
        releaseTrack(track);
        return;
    }
    applyMacros(s);
    SynthNote note;
    note.note = n.note > 127 ? 127 : n.note;
    note.pitch = float(note.note);
    note.velocity = n.volume < 0 ? 1.0f : clampf(float(n.volume) * 0.01f, 0.0f, 1.0f);
    if (sl.lastVel != note.velocity && sl.ntgt7) sl.macroDirty = true;
    sl.lastVel = note.velocity;
    applyMacros(s);
    float bend, target, step;
    const bool hasPrev = trackPitch_[track] >= 0.0f;
    notePitchFx(n, trackPitch_[track], hasPrev, bend, target, step);
    // The step's Delay / Reverb Send FX: this note's own sends.
    noteSendD_ = noteSendR_ = notePan_ = -1;
    const StepFx stepFx[2] = {n.fxA, n.fxB};
    for (const StepFx& f : stepFx) {
        if (f.type == kFxDelaySend) noteSendD_ = int8_t(f.value > 100 ? 100 : f.value);
        if (f.type == kFxReverbSend) noteSendR_ = int8_t(f.value > 100 ? 100 : f.value);
        if (f.type == kFxPanning) notePan_ = int8_t(f.value > 100 ? 100 : f.value);
    }
    // A Glide FX starts at the previous pitch through the bend; the engine's
    // patch glide would start there too and the two would add up.
    note.fxGlide = hasPrev && ((n.fxA.type == kFxGlideId && n.fxA.value) || (n.fxB.type == kFxGlideId && n.fxB.value));
    trackPitch_[track] = note.pitch + target;

    if (P == 1) {   // mono: reuse the slot's voice (legato / glide in the engine)
        int r = -1;
        for (int q = 0; q < kSynthMaxVoices; ++q)
            if (voices_[q].state == kOn && voices_[q].v.slot == s && (r < 0 || voices_[q].age > voices_[r].age)) r = q;
        if (r >= 0) {
            Voice& V = voices_[r];
            if (trackVoice_[track] >= 0 && trackVoice_[track] != r) releaseTrack(track);
            if (V.track >= 0 && V.track != track && trackVoice_[V.track] == r) trackVoice_[V.track] = -1;
            note.overlap = V.v.gate;
            note.reused = true;
            V.v.note = note.note;
            V.v.gate = true;
            V.v.bend = V.v.bendStart = bend;
            V.bendTarget = target;
            V.bendStep = step;
            V.vel = note.velocity;
            V.sendD = noteSendD_;
            V.sendR = noteSendR_;
            V.pan = notePan_;
            V.age = ++serial_;
            V.track = int8_t(track);
            owner_[r] = int8_t(track);   // [I] the resample tap follows the note
            trackVoice_[track] = int8_t(r);
            if (prof_) BlockProfile::inc(prof_->nSynOn[s]);
            sl.eng->noteOn(V.v, sl.p, note);
            return;
        }
    }
    releaseTrack(track);
    ++starts_;
    int victim = -1;
    if (voicesOf(s) >= P) victim = victimInSlot(s, note.note);
    else if (sounding() >= cap()) victim = victimGlobal();
    if (victim >= 0) {
        fade(victim, true);
        ++steals_;
    }
    const int r = freeRecord();
    if (r >= 0) {
        startOn(r, track, s, note, bend, target, step);
        return;
    }
    // Every record busy: start on a fading record after its fade.
    int host = victim;
    if (host < 0 || voices_[host].pending >= 0) {
        host = -1;
        for (int q = 0; q < kSynthMaxVoices && host < 0; ++q)
            if (voices_[q].state == kFading && voices_[q].pending < 0) host = q;
    }
    if (host < 0) return;   // dropped
    for (int e = 0; e < kPending; ++e) {
        Pending& pe = pend_[e];
        if (pe.record >= 0) continue;
        pe.record = int8_t(host);
        pe.track = int8_t(track);
        pe.slot = uint8_t(s);
        pe.note = note;
        pe.bend = bend;
        pe.bendTarget = target;
        pe.bendStep = step;
        pe.sendD = noteSendD_;
        pe.sendR = noteSendR_;
        pe.pan = notePan_;
        voices_[host].pending = int8_t(e);
        return;
    }
}

void SynthBank::noteOff(int track, NoteOffMode mode) {
    if (track < 0 || track >= kSynthAllTracks) return;
    if (Deferred* d = deferredOf(track)) {   // after the queued note starts
        d->off = int8_t(mode);
        return;
    }
    // A note waiting for a record is dropped.
    for (auto& pe : pend_)
        if (pe.record >= 0 && pe.track == track) pe.track = -1;
    if (mode == NoteOffMode::Cut) {
        const int r = trackVoice_[track];
        if (r >= 0) fade(r);
        trackVoice_[track] = -1;
        return;
    }
    releaseTrack(track);   // Off / Fade: the patch release
}

void SynthBank::applyFx(int track, uint8_t type, uint8_t value) {
    if (track < 0 || track >= kSynthAllTracks) return;
    if (Deferred* d = deferredOf(track)) {   // after the queued note starts
        if (d->nfx < 2) {
            d->fx[d->nfx][0] = type;
            d->fx[d->nfx][1] = value;
            ++d->nfx;
        }
        return;
    }
    const int r = trackVoice_[track];
    if (r < 0 || voices_[r].state != kOn) return;
    Voice& V = voices_[r];
    Slot& sl = slots_[V.v.slot];
    switch (type) {
        case kFxVol:
        case kFxRandVol:
            V.vel = clampf(float(value > 100 ? 100 : value) * 0.01f, 0.0f, 1.0f);
            if (sl.ntgt7 && sl.lastVel != V.vel) sl.macroDirty = true;
            sl.lastVel = V.vel;
            break;
        case kFxSlideU:
        case kFxSlideD: {
            const float blocksPerStep = (15.0f / tempo_) * 1.0e6f / kLiveBlockUs;
            V.bendTarget = V.v.bend + (type == kFxSlideU ? 1.0f : -1.0f) * float(value) / 16.0f;
            V.bendStep = std::fabs(V.bendTarget - V.v.bend) / (blocksPerStep > 1.0f ? blocksPerStep : 1.0f);
            break;
        }
        case kFxTuneId:
            V.bendTarget = clampf(float(value) - 24.0f, -24.0f, 24.0f);
            V.bendStep = 0.0f;
            break;
        case kFxMicro:
            V.bendTarget = clampf(float(value) - 99.0f, -99.0f, 99.0f) * 0.01f;
            V.bendStep = 0.0f;
            break;
        // Step FX Delay / Reverb Send set this note's send (user report:
        // the step FX left synth tracks dry); render() adds the difference
        // to the slot's send.
        case kFxDelaySend: V.sendD = int8_t(value > 100 ? 100 : value); break;
        case kFxReverbSend: V.sendR = int8_t(value > 100 ? 100 : value); break;
        // Panning: this note's balance (0 left, 50 centre, 100 right), on
        // top of the slot's pan (exact at the slot's default centre).
        case kFxPanning: V.pan = int8_t(value > 100 ? 100 : value); break;
        default:
            break;   // filter FX: not on synth notes
    }
}

void SynthBank::macroFx(int slot, int macro, uint8_t value) {
    if (slot < 0 || slot >= kSynthSlots || macro < 0 || macro >= kSynthSeqMacros) return;
    Slot& sl = slots_[slot];
    const int16_t v = int16_t(value > 127 ? 127 : value);
    if (sl.ovr[macro] != v) {
        sl.ovr[macro] = v;
        sl.macroDirty = true;
    }
}

void SynthBank::transportStop() {
    for (auto& sl : slots_) {
        for (auto& o : sl.ovr) {
            if (o >= 0) sl.macroDirty = true;
            o = -1;
        }
    }
}

void SynthBank::resetFx() {
    for (int t = 0; t < kSynthAllTracks; ++t) trackPitch_[t] = -1.0f;
}

void SynthBank::stopAll() {
    // Engine state that outlives a voice (PERC's kit and its owner) goes too.
    for (auto& sl : slots_)
        if (sl.eng) sl.eng->slotReset(sl.scratch);
    for (auto& v : voices_) {
        v.state = kFree;
        v.track = -1;
        v.pending = -1;
    }
    for (auto& p : pend_) p.record = -1;
    for (auto& d : defer_) d = Deferred{};
    nDefer_ = 0;
    flushDue_ = false;
    for (int t = 0; t < kSynthAllTracks; ++t) trackVoice_[t] = -1;
    for (auto& o : owner_) o = -1;
    activeVoices_ = 0;
}

// --- render ----------------------------------------------------------------------------

namespace {
// [I] Resample tap (only while a Track take runs): one note's signal x the
// slot's gain / pan (block-end values) into tl / tr, dry. In flash, not
// .fastrun, to keep SRAM for the stack.
__attribute__((noinline)) void addTap(float* tl, float* tr, const float* vl, const float* vr, float gl, float gr) {
    for (uint32_t i = 0; i < kBlock; ++i) {
        tl[i] += vl[i] * gl;
        tr[i] += vr[i] * gr;
    }
}
}  // namespace

OT_FASTRUN_FN bool SynthBank::render(const SynthMixInputs& in, float* mL, float* mR, float* dsend, float* rsend,
                       int16_t* const stems[kSynthSlots], int tapTrack, float* tapL, float* tapR) {
    sync();
    liveDrain();   // MIDI input (audio side only)
    if (flushDue_) flushDeferred();   // no note-on came first in this block
    // Records per slot (one pass).
    uint8_t busy[kSynthSlots] = {};
    for (const Voice& v : voices_)
        if (v.state != kFree) ++busy[v.v.slot];
    bool any = false;
    for (int s = 0; s < kSynthSlots; ++s) {
        Slot& sl = slots_[s];
        SynthParams& p = sl.p;
        // Parameters: start[] catches up with last block's ramp (unless a
        // patch edit / macro move since then already did), then the macro
        // changes of this block ramp.
        catchUp(sl);
        if (sl.macroDirty) applyMacros(s);
        sl.fresh = false;   // the next block catches up
        // Mixer targets (cached per record in sync()).
        const float gl = sl.mixL, gr = sl.mixR;
        const float sd = sl.mixD * in.delayLevel, sr = sl.mixE * in.reverbLevel;
        int16_t* stem = stems ? stems[s] : nullptr;
        if (!busy[s]) {
            sl.gl.jump(gl);
            sl.gr.jump(gr);
            sl.sd.jump(sd);
            sl.sr.jump(sr);
            if (stem) std::memset(stem, 0, kBlock * 2 * sizeof(int16_t));
            continue;
        }
        sl.gl.set(gl);
        sl.gr.set(gr);
        sl.sd.set(sd);
        sl.sr.set(sr);
        // busL_ / busR_ are zero here (cleared by the previous mixSlot).
        // Engine hooks: song tempo, per-voice send tap (PERC per-drum sends
        // straight into the send buses, scaled by slot gain x Global Mixer
        // level; a muted or zero level passes no bus), then the block setup.
        bool tapped = false;
        // Engine cycles of this slot (block profile): prepare + voice renders.
        uint32_t engCyc = 0;
        const bool prof = prof_ != nullptr;
        if (sl.eng) {
            const uint32_t c0 = prof ? CpuMeter::stampCycles() : 0u;
            sl.eng->setTempo(tempo_);
            const float td = sl.gainLin * in.delayLevel, te = sl.gainLin * in.reverbLevel;
            tapped = sl.eng->setSendTap(s, td > 0.0f ? dsend : nullptr, te > 0.0f ? rsend : nullptr, td, te);
            sl.eng->prepare(sl.scratch, p);
            if (prof) {
                engCyc = CpuMeter::stampCycles() - c0;
                prof_->slotEngine[s] = sl.engineId;
            }
        }
        for (int r = 0; r < kSynthMaxVoices; ++r) {
            Voice& V = voices_[r];
            if (V.state == kFree || V.v.slot != s) continue;
            // Step-FX pitch: move bend towards its target.
            V.v.bendStart = V.v.bend;
            if (V.v.bend != V.bendTarget) {
                const float d = V.bendTarget - V.v.bend;
                V.v.bend = (V.bendStep <= 0.0f || std::fabs(d) <= V.bendStep) ? V.bendTarget
                                                                               : V.v.bend + (d > 0.0f ? V.bendStep : -V.bendStep);
            }
            V.v.fading = V.state == kFading;
            V.fresh = 0;
            // Voice gain x the Track Mixer fader of the voice's track (user
            // report: synth tracks ignored the Track Mixer volume).
            const float tg = V.track >= 0 && V.track < kSynthTracks ? in.trackGain[V.track] : 1.0f;
            const float target = voiceGain(sl, V) * tg;   // 0 while fading
            const float g0 = V.g;
            const float gInc = (target - g0) * (1.0f / float(kBlock));
            V.g = target;
            bool alive = false;
            // A note with its own send renders into a scratch pair, joins
            // the bus, and adds (own send - slot send) x its signal to the
            // send buses (same law as mixSlot: (L + R) x 0.5 x SEND x level).
            // [I] A note of the resample-tapped track does too: only its
            // own signal goes to the tap.
            const bool tapV = tapL && owner_[r] == tapTrack;
            const bool own = tapV || V.sendD >= 0 || V.sendR >= 0 || V.pan >= 0;
            float* vl = busL_;
            float* vr = busR_;
            if (own) {
                for (uint32_t i = 0; i < kBlock; ++i) voiceL_[i] = voiceR_[i] = 0.0f;
                vl = voiceL_;
                vr = voiceR_;
            }
            if (sl.eng) {
                if (prof) {
                    const uint32_t c0 = CpuMeter::stampCycles();
                    alive = sl.eng->render(V.v, p, g0, gInc, vl, vr);
                    const uint32_t d = CpuMeter::stampCycles() - c0;
                    engCyc += d;
                    if (d > prof_->voiceMax) prof_->voiceMax = d;
                    BlockProfile::inc(prof_->nSynVoices);
                } else {
                    alive = sl.eng->render(V.v, p, g0, gInc, vl, vr);
                }
            }
            if (own) {
                if (V.pan >= 0 && V.pan != 50) {
                    float pl, pr;
                    panGains(V.pan, pl, pr);
                    for (uint32_t i = 0; i < kBlock; ++i) {
                        voiceL_[i] *= pl;
                        voiceR_[i] *= pr;
                    }
                }
                const float xd = V.sendD >= 0 ? (0.5f * sendGain(V.sendD) - sl.mixD) * in.delayLevel : 0.0f;
                const float xr = V.sendR >= 0 ? (0.5f * sendGain(V.sendR) - sl.mixE) * in.reverbLevel : 0.0f;
                for (uint32_t i = 0; i < kBlock; ++i) {
                    const float m = voiceL_[i] + voiceR_[i];
                    busL_[i] += voiceL_[i];
                    busR_[i] += voiceR_[i];
                    if (dsend) dsend[i] += m * xd;
                    if (rsend) rsend[i] += m * xr;
                }
                if (tapV) addTap(tapL, tapR, voiceL_, voiceR_, gl, gr);
            }
            if (V.state == kFading || !alive) {
                if (V.track >= 0 && trackVoice_[V.track] == r) trackVoice_[V.track] = -1;
                V.track = -1;
                owner_[r] = -1;
                V.state = kFree;
                --activeVoices_;
                const int pe = V.pending;
                V.pending = -1;
                if (pe >= 0) {
                    Pending& q = pend_[pe];
                    if (q.track >= 0 && playable(q.slot) > 0) {
                        // Other notes may have filled the slot / the cap since
                        // this one was queued: steal by the normal order first.
                        int victim = -1;
                        if (voicesOf(q.slot) >= playable(q.slot)) victim = victimInSlot(q.slot, q.note.note);
                        else if (sounding() >= cap()) victim = victimGlobal();
                        if (victim >= 0) {
                            fade(victim, true);
                            ++steals_;
                        }
                        releaseTrack(q.track);
                        if (prof_) BlockProfile::inc(prof_->nPendStart);
                        noteSendD_ = q.sendD;
                        noteSendR_ = q.sendR;
                        notePan_ = q.pan;
                        startOn(r, q.track, q.slot, q.note, q.bend, q.bendTarget, q.bendStep);
                    }
                    q.record = -1;
                }
            }
        }
        if (tapped) sl.eng->setSendTap(s, nullptr, nullptr, 0.0f, 0.0f);
        if (prof) prof_->slot[s] += engCyc;
        const float pk = mixSlot(sl, mL, mR, dsend, rsend, stem);
        // Track Mixer meters: the slot's block peak shared by its sounding
        // voices by envelope level x gain (exact for one voice).
        if (pk > 0.0f) {
            float wsum = 0.0f;
            for (const Voice& V : voices_)
                if (V.state != kFree && V.v.slot == s) wsum += V.v.level * V.g;
            if (wsum > 0.0f)
                for (const Voice& V : voices_) {
                    if (V.state == kFree || V.v.slot != s || V.track < 0 || V.track >= kSynthTracks) continue;
                    const float share = pk * (V.v.level * V.g) / wsum;
                    if (share > trackPeak_[V.track]) trackPeak_[V.track] = share;
                }
        }
        any = true;
    }
    // The next block begins: its voice starts count from 0 and the notes
    // queued so far start first in it.
    starts_ = 0;
    flushDue_ = true;
    return any;
}

// Slot bus -> main bus (gain, balance) and the sends ((L + R) / 2 folded
// into the cached send gains). Variants: ramped or steady gains, each send
// only when it is non-zero. The bus is cleared as it is read (it starts the
// next slot zeroed); with a stem it keeps the post gain / pan signal and the
// stem pass clears it.
namespace {
template <bool kRamp, bool kD, bool kE, bool kKeep>
OT_FASTRUN_FN void mixLoop(float* bl, float* br, float* mL, float* mR, float* ds, float* rs, float l0, float r0, float d0, float e0,
             float li, float ri, float di, float ei) {
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (kRamp) {
            l0 += li;
            r0 += ri;
            d0 += di;
            e0 += ei;
        }
        const float l = bl[i] * l0, r = br[i] * r0;
        mL[i] += l;
        mR[i] += r;
        if (kD || kE) {
            const float m = l + r;
            if (kD) ds[i] += m * d0;
            if (kE) rs[i] += m * e0;
        }
        bl[i] = kKeep ? l : 0.0f;
        br[i] = kKeep ? r : 0.0f;
    }
}
using MixFn = void (*)(float*, float*, float*, float*, float*, float*, float, float, float, float, float, float, float,
                       float);
// [ramp][delay][reverb]
const MixFn kMix[2][2][2] = {{{mixLoop<false, false, false, false>, mixLoop<false, false, true, false>},
                              {mixLoop<false, true, false, false>, mixLoop<false, true, true, false>}},
                             {{mixLoop<true, false, false, false>, mixLoop<true, false, true, false>},
                              {mixLoop<true, true, false, false>, mixLoop<true, true, true, false>}}};
}  // namespace

OT_FASTRUN_FN float SynthBank::mixSlot(Slot& sl, float* mL, float* mR, float* dsend, float* rsend, int16_t* stem) {
    float l0, r0, d0, e0;
    const float li = sl.gl.begin(l0), ri = sl.gr.begin(r0), di = sl.sd.begin(d0), ei = sl.sr.begin(e0);
    const bool dl = d0 != 0.0f || di != 0.0f, rv = e0 != 0.0f || ei != 0.0f;
    const bool ramp = li != 0.0f || ri != 0.0f || di != 0.0f || ei != 0.0f;
    float* bl = busL_;
    float* br = busR_;
    float blockPk = 0.0f;
    // Meter peak (Global Mixer 3/3, a UI reading): every 8th frame of the
    // bus at the block-end gains is enough and keeps the mix loops as they were.
    {
        const int s = int(&sl - slots_);
        const float gL = l0 + li * float(kBlock), gR = r0 + ri * float(kBlock);
        float pl = 0.0f, pr = 0.0f;
        for (uint32_t i = 0; i < kBlock; i += 8) {
            const float a = bl[i] < 0.0f ? -bl[i] : bl[i], b = br[i] < 0.0f ? -br[i] : br[i];
            pl = a > pl ? a : pl;
            pr = b > pr ? b : pr;
        }
        const float pk = pl * (gL < 0.0f ? -gL : gL) > pr * (gR < 0.0f ? -gR : gR) ? pl * (gL < 0.0f ? -gL : gL)
                                                                                    : pr * (gR < 0.0f ? -gR : gR);
        if (pk > peak_[s]) peak_[s] = pk;
        blockPk = pk;
    }
    if (!stem) {
        kMix[ramp][dl][rv](bl, br, mL, mR, dsend, rsend, l0, r0, d0, e0, li, ri, di, ei);
        return blockPk;
    }
    mixLoop<true, true, true, true>(bl, br, mL, mR, dsend, rsend, l0, r0, d0, e0, li, ri, di, ei);
    for (uint32_t i = 0; i < kBlock; ++i) {
        stem[2 * i] = toS16(bl[i]);
        stem[2 * i + 1] = toS16(br[i]);
        bl[i] = br[i] = 0.0f;
    }
    return blockPk;
}

}  // namespace audio
}  // namespace ot
