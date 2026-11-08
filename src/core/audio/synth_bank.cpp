// Synth framework (synth_bank.h).
#include "audio/synth_bank.h"

#include <cmath>
#include <cstring>

#include "audio/cpu_meter.h"
#include "audio/ring_io.h"
#include "fastrun.h"
#include "formats/patch_pb.h"
#include "model/fx.h"
#include "modules/module.h"
#include "modules/synth_descriptor.h"
#include "modules/synth_models.h"

namespace ot {
namespace audio {

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Step FX ids (spec 03 §3.6).
constexpr uint8_t kFxDelaySend = 30, kFxPanning = 31, kFxReverbSend = 32, kFxRandVol = 7, kFxVol = 18, kFxGlideId = 19, kFxMicro = 34, kFxTuneId = 40, kFxSlideU = 41,
                  kFxSlideD = 42, kFxCcA = 8, kFxCcF = 37;
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
        heldSeq_[t] = 0;
    }
    heldNext_ = 0;
    for (int s = 0; s < kSynthSlots; ++s) {
        Slot& sl = slots_[s];
        sl.p.held = SynthHeld{};
        if (sl.preset) rebind_[s] = true;
        sl.preset = 0;
        sl.zSel = 0;
        ownMix_[s] = false;
        for (auto& o : sl.ovr) o = -1;
        sl.macroDirty = true;
        sl.ntouched = 0;
        rebind_[s] = true;
        recDirty_[s] = true;   // the current record (default: spec 21 §3.3) is re-applied
        recApplied_[s] = false;
    }
    activeVoices_ = 0;
    tails_ = 0;
    bindNow_ = 0;
#if OT_SLOT_INSERTS
    for (Ins& in : ins_) {   // the binding stays; a pending kind change completes
        if (in.off) {
            in.fx = in.next;
            in.set = in.nextSet;
            in.next = nullptr;
        }
        clearInsert(in);
    }
    insTails_ = 0;
#endif
    aud_.req = 0;
    for (int k = 0; k < kSynthLiveTracks; ++k) liveSeen_[k] = uint8_t(live_[k]);
}

// --- main loop -----------------------------------------------------------------

void SynthBank::bindPatch(int s, const SynthPatch* p) {
    if (s < 0 || s >= kSynthSlots) return;
    slots_[s].patch = p;
    slots_[s].preset = 0;
    slots_[s].zSel = 0;
    ownMix_[s] = false;
    rebind_[s] = true;
}

void SynthBank::patchChanged(int s) {
    if (s < 0 || s >= kSynthSlots) return;
    slots_[s].preset = 0;   // an edit is heard, on the patch being edited
    slots_[s].zSel = 0;
    ownMix_[s] = false;
    rebind_[s] = true;
}
bool SynthBank::presetSelect(int s, uint8_t n) {
    if (s < 0 || s >= kSynthSlots || n > kSynthPresetCount) return false;
    if (n && (!presets_ || presets_[n - 1].engine == 0)) return false;   // empty: the slot plays on
    slots_[s].zSel = n;
    // A repeated Z costs nothing (and keeps an own-mix edit); [3.x] so does a
    // Z of the preset an instrument borrowed the slot for. Another one wins
    // over that borrowing at once.
    if (slots_[s].preset == n) return true;
    slots_[s].preset = n;
    ownMix_[s] = false;   // [3.x] another preset (or Z0) takes back an own-mix edit
    rebind_[s] = true;
    return true;
}

void SynthBank::setSlotRecord(int s, const SynthSlotRecord& r) {
    if (s < 0 || s >= kSynthSlots) return;
    pendRec_[s] = r;
    recDirty_[s] = true;
}

#if OT_SLOT_INSERTS
void SynthBank::setSlotInsert(int s, IEffect* fx, const EffectSettings& set, bool off) {
    if (s < 0 || s >= kSynthSlots) return;
    pendIns_[s] = PendIns{fx, set, off};
    insDirty_[s] = true;
}

void SynthBank::clearInsert(Ins& in) {
    std::memset(in.state, 0, sizeof(in.state));
    in.fresh = true;
    in.off = false;
    in.parked = false;
    in.offBlocks = 0;
    in.quiet = 0;
}

// [3.x] Slot s's insert can still be heard: a voice of the slot, its engine's
// slot effect or the insert itself rings.
bool SynthBank::insAudible(int s) const {
    if (insTailOf(s) || (tails_ & (1u << s))) return true;
    for (const Voice& v : voices_)
        if (v.state != kFree && v.v.slot == s) return true;
    return false;
}

// The insert slot s should run -- its own, or [3.x] the one of the preset it
// plays -- into the audio state (in sync(), after a main-loop change or a
// rebind of the patch).
__attribute__((noinline)) void SynthBank::applyInsert(int s) {
    PendIns p = ownIns_[s];
    const uint8_t z = slots_[s].preset;
    if (z && presetIns_ && presets_ && presets_[z - 1].engine) {
        const PresetInsert& pi = presetIns_[z - 1];
        if (pi.flags & kPresetInsertHas) {
            p.fx = pi.fx;
            p.set = pi.set;
            p.bypass = (pi.flags & kPresetInsertOff) != 0;
        }
    }
    IEffect* fx = p.fx;
    if (fx && fx->workBytes() && !insLines_) fx = nullptr;   // no memory for it: dry
    Ins& in = ins_[s];
    if (fx == in.fx && !in.off && in.bypass == p.bypass && std::memcmp(&in.set, &p.set, sizeof p.set) == 0)
        return;   // nothing changes (a rebind on the same insert)
    in.bypass = p.bypass;
    if (!p.bypass) in.parked = false;
    if (fx == in.fx) {   // the same kind: new settings (the effect ramps them)
        in.set = p.set;
        in.off = false;
        in.next = nullptr;
    } else if (!in.fx || !insAudible(s)) {   // nothing bound, or [3.x] nothing it could still sound
        clearInsert(in);
        in.fx = fx;
        in.set = p.set;
        in.next = nullptr;
        insTails_ = uint8_t(insTails_ & ~(1u << s));
    } else {   // another kind: fade the bound one out first (rendered even on a silent slot)
        in.off = true;
        in.parked = false;
        in.next = fx;
        in.nextSet = p.set;
        in.offBlocks = 0;
        insTails_ = uint8_t(insTails_ | (1u << s));
    }
}

// The slot insert on the slot bus (busL_ / busR_); true while it rings. Its
// work memory comes in from the slow line into the voice scratch and goes back
// out after the block (one span each way).
__attribute__((noinline)) bool SynthBank::runInsert(int s, bool voiced) {
    Ins& in = ins_[s];
    if (in.parked) return false;   // Off and faded out: not run
    IEffect* fx = in.fx;
    if (voiced) in.quiet = 0;
    else if (in.quiet < 0xF000u) in.quiet = uint16_t(in.quiet + kBlock);
    const uint32_t n16 = fx->workBytes() / sizeof(int16_t);
    int16_t* const line = n16 ? insLines_ + s * (kEffectWorkMax / 2) : nullptr;
    int16_t* const win = n16 ? vs_.w : nullptr;
    if (n16) {
        if (in.fresh) {
            spanZero(line, n16);
            std::memset(win, 0, n16 * sizeof(int16_t));
        } else {
            spanIn(line, win, n16);
        }
    }
    if (in.fresh) {
        fx->reset(in.state, win);
        in.fresh = false;
    }
    const bool live = fx->process(in.state, win, in.set, !in.off && !in.bypass, busL_, busR_, kBlock);
    if (n16) spanOut(line, win, n16);
    if (in.bypass && !in.off && !live) {   // Off: faded out, parked until on again
        in.parked = true;
        return false;
    }
    if (in.off && (!live || ++in.offBlocks >= 4)) {   // faded out: the next kind (or none)
        IEffect* const next = in.next;
        const EffectSettings set = in.nextSet;
        clearInsert(in);
        in.fx = next;
        in.set = set;
        in.next = nullptr;
        return false;
    }
    return live && in.quiet < fx->tailFrames();
}
#endif

void SynthBank::setSlotEngine(int s, int32_t engineId) {
    if (s < 0 || s >= kSynthSlots) return;
    const int32_t v = engineId >= 0x80 ? engineId : 0;
    if (engineOf_[s] == v) return;
    engineOf_[s] = v;
    rebind_[s] = true;
}

int32_t SynthBank::engineFor(int s, const SynthSlotRecord& r) const {
    if (const int32_t e = engineOf_[clampSlot(s)]) return e;
    return r.model < uint32_t(kSynthModelCount) ? synthEngineId(SynthModel(r.model)) : 0;
}

void SynthBank::releaseMacro(int s, int m) {
    if (s < 0 || s >= kSynthSlots || m < 0 || m >= kSynthSeqMacros) return;
    ovrRelease_[s][m] = true;
}

bool SynthBank::slotAvailable(int s) const {
    const int32_t id = engineFor(s, slotRecord(s));
    return id != 0 && synthEngineFor(id) != nullptr;
}

SynthSlotView SynthBank::slotView(int s) const {
    s = clampSlot(s);
    const Slot& sl = slots_[s];
    SynthSlotView v;
    const SynthSlotRecord& r = recDirty_[s] ? pendRec_[s] : sl.rec;
    v.engine = engineFor(s, r);
    v.model = v.engine && patchEngineName(v.engine) ? synthEngineDisplayName(v.engine) : "";
    v.available = v.engine != 0 && synthEngineFor(v.engine) != nullptr;
    v.patchBound = sl.patch != nullptr;
    v.patchMismatch = !sl.patch || sl.patch->engine != v.engine;
    v.polyphony = int(r.polyphony > 8 ? 8 : r.polyphony);
    const int c = cap();
    v.playable = v.polyphony < c ? v.polyphony : c;
    v.limited = c < v.polyphony;
    v.sounding = voicesOf(s);
    for (int k = 0; k < kSynthSeqMacros; ++k) {
        v.overridden[k] = sl.ovr[k] >= 0 && !ovrRelease_[s][k];
        v.macro[k] = v.overridden[k] ? uint8_t(sl.ovr[k]) : (r.macros[k] > 127 ? 127 : r.macros[k]);
    }
    v.lastVelocity = sl.lastVel;
    return v;
}

void SynthBank::auditionOn(int s, uint8_t note, int8_t volume, uint8_t preset) {
    if (s < 0 || s >= kSynthSlots || preset > kSynthPresetCount) return;
    aud_.slot = uint8_t(s | preset << 4);   // [3.x] the preset in the high nibble
    aud_.note = note > 127 ? 127 : note;
    aud_.vol = volume;
    aud_.req = 1;
}

void SynthBank::auditionOff() { aud_.req = 2; }

void SynthBank::liveOn(int k, int s, uint8_t note, int8_t volume, uint8_t preset) {
    if (k < 0 || k >= kSynthLiveTracks || s < 0 || s >= kSynthSlots || preset > kSynthPresetCount) return;
    const uint8_t ser = ++liveSerial_[k];
    // Serial 0..7, on 8, slot 9..10, note 11..17, [3.x] preset 18..21, volume 24..31.
    live_[k] = uint32_t(ser) | 1u << 8 | uint32_t(s & 3) << 9 | uint32_t(note & 127) << 11 | uint32_t(preset & 15) << 18 |
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
            n.preset = uint8_t((w >> 18) & 15);
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

// [3.x] Spread: in flash, not with render(): .fastrun comes out of the stack
// headroom. Moves the note's mid to a side, ramping a change of the slot's
// Spread across the block; L + R (mono, the sends) stays.
__attribute__((noinline)) void SynthBank::spreadVoice(const Slot& sl, uint8_t note, float* l, float* r) {
    const float place = 0.5f / 255.0f * synthSpreadPlace(note);
    float sp = float(sl.spreadCur) * place;
    const float d = float(int(sl.spreadTo) - int(sl.spreadCur)) * place * (1.0f / float(kBlock));
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float m = sp * (l[i] + r[i]);
        l[i] -= m;
        r[i] += m;
        sp += d;
    }
}

// The start of a block, before sync(): what the last block reached, and the
// target; an edit that sync() brings moves the target again.
__attribute__((noinline)) void SynthBank::blockSync() {
    if (spreadLive_) spreadSettle();
    sync();
}

void SynthBank::spreadSettle() {
    bool live = false;
    for (int s = 0; s < kSynthSlots; ++s) {
        Slot& sl = slots_[s];
        const uint8_t reached = sl.spreadTo;
        sl.spreadTo = playable(s) > 1 ? sl.spreadEnd : uint8_t(0);
        // An idle slot has no note to ramp: the next one starts at its place.
        sl.spreadCur = voicesOf(s) > 0 ? reached : sl.spreadTo;
        live = live || sl.spreadCur || sl.spreadTo || sl.spreadEnd;
    }
    spreadLive_ = live;
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

// [3.x] The mix slot s plays with -- its record's, or the one of the preset
// it is bound to when that preset carries one -- into the cached mixer
// targets (in sync(), after a record change or a rebind of the patch).
__attribute__((noinline)) void SynthBank::applyMix(int s) {
    Slot& sl = slots_[s];
    float gainDb = sl.rec.gainDb, panV = sl.rec.pan, del = sl.rec.delaySend, rev = sl.rec.reverbSend;
    if (!ownMix_[s] && presetMix_ && presets_ && sl.bound && sl.bound != sl.patch) {
        const uintptr_t b = uintptr_t(sl.bound), t = uintptr_t(presets_);
        const uintptr_t n = (b - t) / sizeof(SynthPatch);
        if (b >= t && n < uintptr_t(kSynthPresetCount) && (presetMix_[n].flags & kPresetMixHas)) {
            const PresetMix& m = presetMix_[n];
            gainDb = m.gainDb;
            panV = m.pan;
            del = m.delaySend;
            rev = m.reverbSend;
        }
    }
    sl.gainLin = std::isfinite(gainDb) && gainDb > -96.0f ? dbToGain(clampf(gainDb, -96.0f, 12.0f)) : 0.0f;
    // Balance (linear, spec 04 §4.3) and SEND law, cached per mix; the send
    // gains include the (L + R) / 2 of the mono effect input.
    const float pan = std::isfinite(panV) ? clampf(panV, -100.0f, 100.0f) : 0.0f;
    sl.mixL = sl.gainLin * (pan > 0.0f ? 1.0f - pan * 0.01f : 1.0f);
    sl.mixR = sl.gainLin * (pan < 0.0f ? 1.0f + pan * 0.01f : 1.0f);
    sl.sendD = uint8_t(std::isfinite(del) ? int(clampf(del, 0.0f, 100.0f) + 0.5f) : 0);
    sl.sendR = uint8_t(std::isfinite(rev) ? int(clampf(rev, 0.0f, 100.0f) + 0.5f) : 0);
    sl.mixD = 0.5f * sendGain(sl.sendD);
    sl.mixE = 0.5f * sendGain(sl.sendR);
}

void SynthBank::sync() {
    for (int s = 0; s < kSynthSlots; ++s) {
        Slot& sl = slots_[s];
        bool mixWant = false;
#if OT_SLOT_INSERTS
        bool insWant = false;
        if (insDirty_[s]) {   // the slot's own insert, as the main loop set it
            insDirty_[s] = false;
            ownIns_[s] = pendIns_[s];
            insWant = true;
        }
#endif
        for (int k = 0; k < kSynthSeqMacros; ++k) {
            if (!ovrRelease_[s][k]) continue;
            ovrRelease_[s][k] = false;
            if (sl.ovr[k] >= 0) {
                sl.ovr[k] = -1;
                sl.macroDirty = true;
            }
        }
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
            mixWant = true;
            }
        }
        if (rebind_[s]) {
            rebind_[s] = false;
            bindEngine(s);
            mixWant = true;   // [3.x] a preset switch (Z, Z0, Stop, an edit) brings its mix
#if OT_SLOT_INSERTS
            insWant = true;   // and its insert
#endif
        }
        if (mixWant) applyMix(s);
#if OT_SLOT_INSERTS
        if (insWant) applyInsert(s);
#endif
    }
    if (aud_.req) {
        const uint8_t req = aud_.req;
        aud_.req = 0;
        if (req == 1) {
            SynthNoteOn n;
            n.slot = uint8_t(aud_.slot & 15);
            n.preset = uint8_t(aud_.slot >> 4);
            n.note = aud_.note;
            n.volume = aud_.vol;
            noteOnNow(kSynthAuditionTrack, n);
        } else {
            noteOff(kSynthAuditionTrack, NoteOffMode::Off);
        }
    }
}

void SynthBank::bindEngine(int s, bool cut) {
    Slot& sl = slots_[s];
    const SynthPatch* patch = patchOf(sl);
    sl.bound = patch;
    const int32_t id = patch != sl.patch ? patch->engine : engineFor(s, sl.rec);   // a preset brings its engine
    ISynthEngine* e = id ? synthEngineFor(id) : nullptr;
    if (e && override_) e = override_;
    sl.mismatch = !patch || patch->engine != id;
    if (sl.mismatch) e = nullptr;
    const bool restart = cut || e != sl.eng || id != sl.engineId;   // [3.x] cut: another sound on the same engine too
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
        tails_ = uint8_t(tails_ & ~(1u << s));
    }
    reloadParams(s, restart);
}

float SynthBank::paramBase(const Slot& sl, int i) const { return synthParamGet(*sl.bound, i); }

void SynthBank::reloadParams(int s, bool jump) {
    Slot& sl = slots_[s];
    if (prof_) BlockProfile::inc(prof_->nReload);
    SynthParams& p = sl.p;
    p.engine = sl.engineId;
    sl.ntgt = sl.ntgt7 = 0;
    sl.volK = 0.0f;
    sl.volIdx = -1;
    // [3.x] C1: Poly 1 note priority when the engine asks for it.
    const mod::SynthDescriptor* d = mod::ModuleHost::instance().synthDescriptor(sl.engineId);
    sl.prio = d && (d->flags & mod::kSynthFlagNotePriority);
    sl.prioIdx = sl.prio ? d->priorityParam : int8_t(-1);
    sl.kit = d && (d->flags & mod::kSynthFlagKitState);
    if (!sl.bound || sl.mismatch) {
        p.count = 0;
        p.moving = false;
        ++p.serial;
        return;
    }
    int n = 0;
    const PatchParamRange* t = patchEngineParams(sl.engineId, &n);
    // A live edit ramps from where the last block ended (like a macro move).
    if (!jump) catchUp(sl);
    // The Mini's parameters, then the [OT] extension ones (Width, FAT
    // Stereo; synth_params.h) for the engines that read them.
    const int total = synthParamTotal(sl.engineId);
    p.count = total;
    for (int i = 0; i < total; ++i) {
        p.end[i] = paramBase(sl, i);
        if (jump) p.start[i] = p.end[i];
    }
    p.moving = !jump;
    sl.fullCopy = !jump;
    sl.fresh = !jump;
    ++p.serial;
    // [OT] Width: not a macro target; a live edit ramps over one block.
    const int wi = synthWidthParam(sl.engineId);
    sl.widthEnd = wi >= 0 ? paramBase(sl, wi) : 0.0f;
    if (jump) sl.widthCur = sl.widthEnd;
    // [3.x] Spread: read per note in render(), which ramps a change.
    const int si = synthSpreadParam(sl.engineId);
    sl.spreadEnd = si >= 0 ? uint8_t(std::lround(255.0f * paramBase(sl, si))) : uint8_t(0);
    sl.spreadTo = playable(s) > 1 ? sl.spreadEnd : uint8_t(0);
    if (jump) sl.spreadCur = sl.spreadTo;
    spreadLive_ = spreadLive_ || sl.spreadEnd || sl.spreadCur;
    sl.volIdx = patchVolumeParam(sl.engineId);
    if (sl.volIdx >= 0 && sl.volIdx < n) {
        sl.volMin = t[sl.volIdx].min;
        sl.volMax = t[sl.volIdx].max;
    }
    // Compile the macro targets (empty targets -1 inside count are skipped).
    for (int k = 0; k < kSynthSeqMacros; ++k) {
        const PatchMacro& m = sl.bound->macros[k];
        for (int j = 0, c = synthMacroTargets(m); j < c; ++j) {
            const int i = m.targets[j].param;
            const float a = patchBitsToFloat(m.targets[j].amountBits);
            if (i < 0 || i >= n || !std::isfinite(a)) continue;
            sl.tgt[sl.ntgt++] = Slot::Tgt{uint8_t(i), uint8_t(k), a * (t[i].max - t[i].min)};
        }
    }
    const PatchMacro& m7 = sl.bound->macros[kPatchMacroVelocity];
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
    if (!sl.bound || sl.mismatch || (!sl.macroDirty)) return;
    sl.macroDirty = false;
    uint8_t val[kSynthSeqMacros];
    float m[kSynthSeqMacros];
    for (int k = 0; k < kSynthSeqMacros; ++k) {
        val[k] = sl.ovr[k] >= 0 ? uint8_t(sl.ovr[k]) : sl.page[k];
        m[k] = synthMacroM(val[k], sl.bound->macros[k].bipolar);
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
    // at once: no fade blip, no wasted render. Not a kit engine (PERC,
    // T-Type: kSynthFlagKitState): its kit outlives the voice and is released
    // in the voice's last render.
    if (steal && v.fresh && v.pending < 0 && !slots_[v.v.slot].kit) {
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
    V.live = track >= kSynthLiveTrack0 && track < kSynthAllTracks;
    V.liveBend = 0.0f;
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
        if (slots_[V.v.slot].prio && returnToHeld(r)) return;   // [3.x] C1
        V.v.gate = false;
        const Slot& sl = slots_[V.v.slot];
        if (sl.eng) sl.eng->noteOff(V.v, sl.p);
    }
}

// --- [3.x] preset instruments ----------------------------------------------------------

bool SynthBank::presetPlays(uint8_t k) const {
    if (!presets_ || k < 1 || k > kSynthPresetCount) return false;
    const int32_t e = presets_[k - 1].engine;
    return e != 0 && synthEngineFor(e) != nullptr;
}

int SynthBank::presetSlot(uint8_t k) const {
    if (!presetPlays(k)) return -1;
    for (int s = 0; s < kSynthSlots; ++s)
        if (slots_[s].preset == k && playable(s) > 0) return s;
    return -1;
}

// A slot for preset k (none plays it): an idle one, S3 first (songs start
// on S1); else steal the slot whose notes are all in their release, else
// the one whose last note-on is the oldest. One pass over the voices.
int SynthBank::acquire(uint8_t k) {
    if (!presetPlays(k) || cap() == 0) return -1;
    uint8_t on[kSynthSlots] = {}, gated[kSynthSlots] = {};
    uint32_t last[kSynthSlots] = {};
    for (const Voice& v : voices_) {
        if (v.state != kOn) continue;
        const int s = v.v.slot;
        ++on[s];
        gated[s] = uint8_t(gated[s] | v.v.gate);
        if (v.age > last[s]) last[s] = v.age;
    }
    // A slot bound in this block keeps its waiting note: the last choice.
    int idle = -1, best = -1;
    for (int s = kSynthSlots - 1; s >= 0; --s) {
        if (slots_[s].rec.polyphony == 0) continue;   // the slot is off
        const bool resv = (bindNow_ >> s) & 1u;
        if (!resv && !on[s] && !(tails_ & (1u << s)) && !insTailOf(s)) {
            idle = s;
            break;
        }
        const bool bresv = best >= 0 && ((bindNow_ >> best) & 1u);
        if (best < 0 || (resv != bresv ? !resv : (!gated[s] != !gated[best] ? !gated[s] : last[s] < last[best]))) best = s;
    }
    const int s = idle >= 0 ? idle : best;
    if (s >= 0) bindTo(s, k);
    return s;
}

// Slot s plays preset k (0: its own patch) from this note on: its notes and
// held notes go (a jump, not a morph), its tails and insert follow the new
// sound, and an own-mix edit is dropped (the bound preset's mix plays).
void SynthBank::bindTo(int s, uint8_t k) {
    for (int r = 0; r < kSynthMaxVoices; ++r)
        if (voices_[r].state == kOn && voices_[r].v.slot == s) {
            fade(r, true);
            ++steals_;
        }
    bool held = false;
    for (int t = 0; t < kSynthAllTracks; ++t)
        if (heldSeq_[t] && heldSlot_[t] == s) {
            heldSeq_[t] = 0;
            held = true;
        }
    if (held) heldUpdate(s);
    slots_[s].preset = k;
    ownMix_[s] = false;
    bindNow_ = uint8_t(bindNow_ | (1u << s));
    bindEngine(s, true);
    applyMix(s);
#if OT_SLOT_INSERTS
    applyInsert(s);
#endif
}

// --- [3.x] C1: held notes, note priority, return-to-held -------------------------

void SynthBank::holdSet(int track, int s, uint8_t note, float velocity) {
    const int was = heldSeq_[track] ? int(heldSlot_[track]) : -1;
    if (heldNext_ == 255) {   // renumber 1..n in key order before the order wraps
        uint8_t rank[kSynthAllTracks];
        heldNext_ = 0;
        for (int t = 0; t < kSynthAllTracks; ++t) {
            rank[t] = 0;
            if (!heldSeq_[t]) continue;
            ++heldNext_;
            for (int q = 0; q < kSynthAllTracks; ++q) rank[t] = uint8_t(rank[t] + (heldSeq_[q] && heldSeq_[q] <= heldSeq_[t]));
        }
        for (int t = 0; t < kSynthAllTracks; ++t) heldSeq_[t] = rank[t];
    }
    heldNote_[track] = note;
    heldVel_[track] = uint8_t(clampf(velocity, 0.0f, 1.0f) * 255.0f + 0.5f);
    heldSlot_[track] = uint8_t(s);
    heldSeq_[track] = ++heldNext_;
    heldUpdate(s);
    if (was >= 0 && was != s) heldUpdate(was);
}

void SynthBank::holdClear(int track) {
    if (!heldSeq_[track]) return;
    heldSeq_[track] = 0;
    heldUpdate(heldSlot_[track]);
}

void SynthBank::heldUpdate(int s) {
    SynthHeld h;
    for (int t = 0; t < kSynthAllTracks; ++t) {
        if (!heldSeq_[t] || heldSlot_[t] != s) continue;
        const uint8_t n = heldNote_[t];
        if (!h.count || n < h.low) h.low = n;
        if (!h.count || n > h.high) h.high = n;
        ++h.count;
    }
    h.mono = playable(s) == 1;
    slots_[s].p.held = h;
}

// Last: the most recent key; Low / High: the lowest / highest note, ties to
// the most recent.
int SynthBank::heldWinner(int s) const {
    const Slot& sl = slots_[s];
    const int mode = sl.prioIdx >= 0 && sl.prioIdx < sl.p.count ? int(clampf(sl.p.end[sl.prioIdx], 0.0f, 2.0f) + 0.5f) : 0;
    int w = -1;
    for (int t = 0; t < kSynthAllTracks; ++t) {
        if (!heldSeq_[t] || heldSlot_[t] != s) continue;
        if (w < 0) {
            w = t;
            continue;
        }
        const int d = mode == 1 ? int(heldNote_[w]) - int(heldNote_[t]) : (mode == 2 ? int(heldNote_[t]) - int(heldNote_[w]) : 0);
        if (d > 0 || (d == 0 && heldSeq_[t] > heldSeq_[w])) w = t;
    }
    return w;
}

bool SynthBank::returnToHeld(int r) {
    const int s = voices_[r].v.slot;
    const Slot& sl = slots_[s];
    if (!sl.prio || !sl.eng || playable(s) != 1) return false;
    const int w = heldWinner(s);
    if (w < 0) return false;
    handTo(r, w);
    return true;
}

void SynthBank::handTo(int r, int track) {
    Voice& V = voices_[r];
    Slot& sl = slots_[V.v.slot];
    if (trackVoice_[track] >= 0 && trackVoice_[track] != r) releaseTrack(track);
    if (V.track >= 0 && trackVoice_[V.track] == r) trackVoice_[V.track] = -1;
    SynthNote note;
    note.note = heldNote_[track];
    note.pitch = float(note.note);
    note.velocity = float(heldVel_[track]) * (1.0f / 255.0f);
    note.overlap = true;
    note.reused = true;
    // The held track's Tune / Micro-tune (its pitch FX offset) as the bend.
    const float bend = trackPitch_[track] >= 0.0f ? trackPitch_[track] - note.pitch : 0.0f;
    V.v.note = note.note;
    V.v.gate = true;
    V.v.bend = V.v.bendStart = bend;
    V.bendTarget = bend;
    V.bendStep = 0.0f;
    V.live = track >= kSynthLiveTrack0 && track < kSynthAllTracks;
    V.liveBend = 0.0f;
    V.vel = note.velocity;
    V.age = ++serial_;
    V.track = int8_t(track);
    owner_[r] = int8_t(track);
    trackVoice_[track] = int8_t(r);
    if (prof_) BlockProfile::inc(prof_->nSynOn[V.v.slot]);
    sl.eng->noteOn(V.v, sl.p, note);
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
    if (n.preset > kSynthPresetCount || (!n.preset && n.slot >= kSynthSlots)) return;
    // [3.x] A preset instrument's note: the slot on that preset, if any (else
    // a bind below); a slot instrument's: its slot, back on what Z selected.
    int s = n.preset ? presetSlot(n.preset) : int(n.slot);
    // Note-on spreading (header): a later note of a track whose note waits
    // replaces it; past the per-block start cap a note that needs a new
    // voice record (or [3.x] a bind) waits one block.
    if (!forced) {
        Deferred* d = deferredOf(track);
        bool wait = d != nullptr;
        if (!wait && startCap_ > 0 && starts_ >= startCap_) {
            if (s < 0) {
                wait = presetPlays(n.preset);
            } else if (playable(s) > 0) {
                bool reuse = false;
                const bool bind = !n.preset && slots_[s].preset != slots_[s].zSel;
                if (playable(s) == 1 && !bind)
                    for (int q = 0; q < kSynthMaxVoices && !reuse; ++q)
                        reuse = voices_[q].state == kOn && voices_[q].v.slot == s;
                wait = !reuse;
            }
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
    bool bound = false;
    if (n.preset) {
        if (s < 0) {
            s = acquire(n.preset);
            bound = s >= 0;
        }
    } else if (slots_[s].preset != slots_[s].zSel) {
        bindTo(s, slots_[s].zSel);   // [3.x] the slot's own note: the latest note-on decides its sound
        bound = true;
    }
    // [3.x] Spreading: the bind costs about a note start, so a sequencer note
    // that binds counts as one and starts first in the next block.
    if (bound && !forced && startCap_ > 0) {
        ++starts_;
        Deferred* d = &defer_[nDefer_++];   // (no entry of this track: it would have waited above)
        *d = Deferred{};
        d->track = int8_t(track);
        d->n = n;
        if (prof_) BlockProfile::inc(prof_->nDeferred);
        return;
    }
    const int P = s < 0 ? 0 : playable(s);
    if (P == 0) {   // slot off, unavailable model or no patch ([3.x] an empty or unavailable preset): silent
        holdClear(track);
        releaseTrack(track);
        return;
    }
    Slot& sl = slots_[s];
    if (n.preset) {   // [3.x] the note's CC a..f: the slot it plays on (as the sequencer's for a slot)
        const StepFx cc[2] = {n.fxB, n.fxA};
        for (const StepFx& f : cc)
            if ((f.type >= kFxCcA && f.type <= kFxCcA + 4) || f.type == kFxCcF)
                macroFx(s, f.type == kFxCcF ? 5 : f.type - kFxCcA, f.value);
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
    holdSet(track, s, note.note, note.velocity);

    if (P == 1) {   // mono: reuse the slot's voice (legato / glide in the engine)
        int r = -1;
        for (int q = 0; q < kSynthMaxVoices; ++q)
            if (voices_[q].state == kOn && voices_[q].v.slot == s && (r < 0 || voices_[q].age > voices_[r].age)) r = q;
        // [3.x] C1: a note that does not win is held silently (also with no
        // voice sounding); a sounding voice plays the winner (it may be
        // another held note now).
        if (sl.prio) {
            const int w = heldWinner(s);
            if (w != track) {
                if (trackVoice_[track] >= 0 && trackVoice_[track] != r) releaseTrack(track);
                if (r >= 0 && voices_[r].v.gate && voices_[r].track != w) handTo(r, w);
                return;
            }
        }
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
            V.live = track >= kSynthLiveTrack0 && track < kSynthAllTracks;
            V.liveBend = 0.0f;
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
        pe.preset = sl.preset;
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
    holdClear(track);   // [3.x] C1 (Cut: no return to a held note)
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
        // [3.x] CC a..f on an FX-only step after a preset instrument's note:
        // the slot it plays on (a slot instrument's go through macroFx).
        case kFxCcA: case kFxCcA + 1: case kFxCcA + 2: case kFxCcA + 3: case kFxCcA + 4: case kFxCcF:
            macroFx(V.v.slot, type == kFxCcF ? 5 : type - kFxCcA, value);
            break;
        default:
            break;   // filter step FX: not on synth notes (the Performance filters are, #23)
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
    // The sequencer's notes are no longer held (no return to them later).
    for (int t = 0; t < kSynthTracks; ++t) holdClear(t);
    for (int s = 0; s < kSynthSlots; ++s) {
        Slot& sl = slots_[s];
        for (auto& o : sl.ovr) {
            if (o >= 0) sl.macroDirty = true;
            o = -1;
        }
        if (sl.preset) {   // Z (and [3.x] a preset instrument's binding) lasts until Stop
            sl.preset = 0;
            rebind_[s] = true;
        }
        sl.zSel = 0;
        ownMix_[s] = false;
    }
}

namespace {
constexpr float kPerfOctPerUnit = 477.0f / 5000.0f;   // filter value 1 = 0.0954 octaves
int8_t perfByte(float v) { return int8_t(v < -100.0f ? -100 : (v > 100.0f ? 100 : int(std::lround(v)))); }
}  // namespace

void SynthBank::setTrackPerf(int t, const SynthPerf& p) {
    if (t < 0 || t >= kSynthTracks) return;
    const uint8_t b[4] = {uint8_t(perfByte(p.cutoffOct / kPerfOctPerUnit)), uint8_t(perfByte(p.pan)),
                          uint8_t(perfByte(p.dsend)), uint8_t(perfByte(p.rsend))};
    perfWord_[t] = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    uint8_t n = 0;
    for (uint32_t w : perfWord_) n = uint8_t(n + (w ? 1 : 0));
    perfActive_ = n;
}

SynthPerf SynthBank::trackPerf(int t) const {
    SynthPerf p;
    if (t < 0 || t >= kSynthTracks) return p;
    const uint32_t w = perfWord_[t];
    p.cutoffOct = float(int8_t(w)) * kPerfOctPerUnit;
    p.pan = int8_t(w >> 8);
    p.dsend = int8_t(w >> 16);
    p.rsend = int8_t(w >> 24);
    return p;
}

// Performance FX of a sounding note's track (issue #23): its filter offset
// in octaves, and its pan / sends added to the note's own (centre / the
// slot's send). In flash, not in render's FASTRUN copy: it runs once per
// voice per block, only while a track has Performance FX on.
__attribute__((noinline)) float SynthBank::perfNote(uint32_t w, const Slot& sl, int8_t& pan, int8_t& sendD,
                                                    int8_t& sendR) {
    const int dp = int8_t(w >> 8), dd = int8_t(w >> 16), dr = int8_t(w >> 24);
    if (dp) pan = int8_t(clampi((pan >= 0 ? pan : 50) + dp, 0, 100));
    if (dd) sendD = int8_t(clampi((sendD >= 0 ? sendD : int(sl.sendD)) + dd, 0, 100));
    if (dr) sendR = int8_t(clampi((sendR >= 0 ? sendR : int(sl.sendR)) + dr, 0, 100));
    return float(int8_t(w)) * kPerfOctPerUnit;
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
    for (int t = 0; t < kSynthAllTracks; ++t) {
        trackVoice_[t] = -1;
        heldSeq_[t] = 0;
    }
    heldNext_ = 0;
    for (auto& sl : slots_) sl.p.held = SynthHeld{};
    for (auto& o : owner_) o = -1;
    activeVoices_ = 0;
    // (Also reached from the main loop: a render racing this store keeps or
    // drops one slot's tail for one block; its scratch is zeroed either way.)
    tails_ = 0;
    bindNow_ = 0;
#if OT_SLOT_INSERTS
    for (Ins& in : ins_) {   // the inserts' tails go too; the next note starts clean
        if (in.off) {
            in.fx = in.next;
            in.set = in.nextSet;
            in.next = nullptr;
        }
        clearInsert(in);
    }
    insTails_ = 0;
#endif
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
    blockSync();
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
        // A tapping engine (PERC: per-drum sends) sends through its tap only,
        // so the slot's own sends stay 0 (see the per-voice tap below).
        const bool taps = sl.eng && sl.eng->setSendTap(s, nullptr, nullptr, 0.0f, 0.0f);
        const float sd = taps ? 0.0f : sl.mixD * in.delayLevel, sr = taps ? 0.0f : sl.mixE * in.reverbLevel;
        int16_t* stem = stems ? stems[s] : nullptr;
        // A slot without voices renders on while its engine's slot effect
        // rings (slotPost on the silent bus).
        if (!busy[s] && !(sl.eng && (tails_ & (1u << s))) && !insTailOf(s)) {
            sl.gl.jump(gl);
            sl.gr.jump(gr);
            sl.sd.jump(sd);
            sl.sr.jump(sr);
            if (stem) std::memset(stem, 0, kBlock * 2 * sizeof(int16_t));
            continue;
        }
        p.held.mono = playable(s) == 1;   // [3.x] the cap or the polyphony may have moved since the last note
        sl.gl.set(gl);
        sl.gr.set(gr);
        // busL_ / busR_ are zero here (cleared by the previous mixSlot).
        // Engine hooks: song tempo, per-voice send tap (PERC per-drum sends
        // straight into the send buses, scaled by slot gain x Global Mixer
        // level; a muted or zero level passes no bus), then the block setup.
        // A tapping engine's sends are its per-voice amounts x the
        // instrument's send (or the note's Delay / Reverb Send FX), set per
        // voice below; the slot itself then sends nothing on its own.
        bool tapped = false;
        float td = 0.0f, te = 0.0f;
        // Engine cycles of this slot (block profile): prepare + voice renders.
        uint32_t engCyc = 0;
        const bool prof = prof_ != nullptr;
        if (sl.eng) {
            const uint32_t c0 = prof ? CpuMeter::stampCycles() : 0u;
            sl.eng->setTempo(tempo_);
            td = sl.gainLin * in.delayLevel;
            te = sl.gainLin * in.reverbLevel;
            tapped = taps;
            sl.eng->prepare(sl.scratch, p);
            if (prof) {
                engCyc = CpuMeter::stampCycles() - c0;
                prof_->slotEngine[s] = sl.engineId;
            }
        }
        sl.sd.set(sd);
        sl.sr.set(sr);
        for (int r = 0; r < kSynthMaxVoices; ++r) {
            Voice& V = voices_[r];
            if (V.state == kFree || V.v.slot != s) continue;
            // Live MIDI pitch bend (live tracks): added to bend and its
            // target; a note started this block takes it at once, a sounding
            // one ramps to it over the block like the step-FX pitch.
            float lb = 0.0f;
            if (V.live && (lb = liveBend_ - V.liveBend) != 0.0f) {
                V.liveBend += lb;
                V.bendTarget += lb;
                if (V.fresh) {
                    V.v.bend += lb;
                    lb = 0.0f;
                }
            }
            // Step-FX pitch: move bend towards its target.
            V.v.bendStart = V.v.bend;
            V.v.bend += lb;
            if (V.v.bend != V.bendTarget) {
                const float d = V.bendTarget - V.v.bend;
                V.v.bend = (V.bendStep <= 0.0f || std::fabs(d) <= V.bendStep) ? V.bendTarget
                                                                               : V.v.bend + (d > 0.0f ? V.bendStep : -V.bendStep);
            }
            V.v.fading = V.state == kFading;
            V.fresh = 0;
            // Voice gain x the Track Mixer fader of the voice's track (user
            // report: synth tracks ignored the Track Mixer volume). owner_
            // survives the note-off, so a release tail keeps the fader and
            // the track's Performance FX (V.track is cleared at release).
            const int ot = owner_[r];
            const bool seqTrack = ot >= 0 && ot < kSynthTracks;
            const float tg = seqTrack ? in.trackGain[ot] : 1.0f;
            // Performance pan / sends / filter of the note's track (#23):
            // the pan / sends stand in for the note's own for this block
            // and are put back after the mix below.
            const bool perfV = perfActive_ && seqTrack && perfWord_[ot];
            const int8_t keepD = V.sendD, keepR = V.sendR, keepP = V.pan;
            V.v.cutoffOct = perfV ? perfNote(perfWord_[ot], sl, V.pan, V.sendD, V.sendR) : 0.0f;
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
            const bool own = tapV || V.sendD >= 0 || V.sendR >= 0 || V.pan >= 0 || (sl.spreadCur | sl.spreadTo);
            float* vl = busL_;
            float* vr = busR_;
            if (own) {
                for (uint32_t i = 0; i < kBlock; ++i) vs_.f[0][i] = vs_.f[1][i] = 0.0f;
                vl = vs_.f[0];
                vr = vs_.f[1];
            }
            if (tapped) {
                // User report: the init PERC kit's snare and hats reached the
                // delay whatever the instrument and step FX sent. A drum's
                // send is its patch send x this note's send (FX) or else the
                // instrument's (2 x mixD = SEND law of the record), with the
                // track's Performance send (#23).
                const float kd = td * (V.sendD >= 0 ? sendGain(V.sendD) : 2.0f * sl.mixD);
                const float ke = te * (V.sendR >= 0 ? sendGain(V.sendR) : 2.0f * sl.mixE);
                sl.eng->setSendTap(s, kd > 0.0f ? dsend : nullptr, ke > 0.0f ? rsend : nullptr, kd, ke);
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
                        vs_.f[0][i] *= pl;
                        vs_.f[1][i] *= pr;
                    }
                }
                if (sl.spreadCur | sl.spreadTo) spreadVoice(sl, V.v.note, vs_.f[0], vs_.f[1]);
                // (A tapping engine sent this note's share through the tap.)
                const float xd = V.sendD >= 0 && !tapped ? (0.5f * sendGain(V.sendD) - sl.mixD) * in.delayLevel : 0.0f;
                const float xr = V.sendR >= 0 && !tapped ? (0.5f * sendGain(V.sendR) - sl.mixE) * in.reverbLevel : 0.0f;
                for (uint32_t i = 0; i < kBlock; ++i) {
                    const float m = vs_.f[0][i] + vs_.f[1][i];
                    busL_[i] += vs_.f[0][i];
                    busR_[i] += vs_.f[1][i];
                    if (dsend) dsend[i] += m * xd;
                    if (rsend) rsend[i] += m * xr;
                }
                if (tapV) addTap(tapL, tapR, vs_.f[0], vs_.f[1], gl, gr);
            }
            if (perfV) {
                V.sendD = keepD;
                V.sendR = keepR;
                V.pan = keepP;
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
                    if (q.track >= 0 && playable(q.slot) > 0 && slots_[q.slot].preset == q.preset) {
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
        // The engine's slot effect on the voices' sum (counted as engine time).
        if (sl.eng) {
            const uint32_t c0 = prof ? CpuMeter::stampCycles() : 0u;
            const bool tail = sl.eng->slotPost(s, sl.scratch, p, busL_, busR_);
            tails_ = uint8_t(tail ? tails_ | (1u << s) : tails_ & ~(1u << s));
            if (prof) engCyc += CpuMeter::stampCycles() - c0;
        }
#if OT_SLOT_INSERTS
        // The slot insert: after the engine's slot effect, before Width / gain /
        // pan / sends, so they carry the effected sound. A note's own sends and
        // the resample tap took the voice before it (dry). Counted as slot time.
        if (ins_[s].fx) {
            const uint32_t c0 = prof ? CpuMeter::stampCycles() : 0u;
            const bool ring = runInsert(s, busy[s] != 0 || (tails_ & (1u << s)) != 0);
            insTails_ = uint8_t(ring ? insTails_ | (1u << s) : insTails_ & ~(1u << s));
            if (prof) engCyc += CpuMeter::stampCycles() - c0;
        } else {
            insTails_ = uint8_t(insTails_ & ~(1u << s));
        }
#endif
        if (prof) prof_->slot[s] += engCyc;
        widen(sl);
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
    bindNow_ = 0;
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

// [OT] Width: S = w * (AP(M) - M) / 2 with M = (L + R) / 2 and AP four
// first-order allpasses (breaks ~120 / 400 / 1300 / 4300 Hz at 44117.6 Hz:
// a = (tan(pi f / fs) - 1) / (tan(pi f / fs) + 1)); L += S, R -= S. L + R
// is unchanged (mono, the sends), S = 0 at DC (the bass stays centred) and
// |S| <= w |M|: frequency bands lean left or right. Width 0 skips it all.
OT_FASTRUN_FN void SynthBank::widen(Slot& sl) {
    if (sl.widthCur == 0.0f && sl.widthEnd == 0.0f) return;
    static constexpr float kAp[4] = {-0.983054128f, -0.944595595f, -0.83009807f, -0.51959139f};
    float* bl = busL_;
    float* br = busR_;
    float w = sl.widthCur * 0.5f;
    const float wi = (sl.widthEnd - sl.widthCur) * 0.5f / float(kBlock);
    float x0 = sl.ap[0][0], y0 = sl.ap[0][1], x1 = sl.ap[1][0], y1 = sl.ap[1][1];
    float x2 = sl.ap[2][0], y2 = sl.ap[2][1], x3 = sl.ap[3][0], y3 = sl.ap[3][1];
    for (uint32_t i = 0; i < kBlock; ++i) {
        w += wi;
        const float m = 0.5f * (bl[i] + br[i]);
        // y = a (x - y1) + x1 per stage (transposed direct form).
        const float a0 = kAp[0] * (m - y0) + x0;
        x0 = m;
        y0 = a0;
        const float a1 = kAp[1] * (a0 - y1) + x1;
        x1 = a0;
        y1 = a1;
        const float a2 = kAp[2] * (a1 - y2) + x2;
        x2 = a1;
        y2 = a2;
        const float a3 = kAp[3] * (a2 - y3) + x3;
        x3 = a2;
        y3 = a3;
        const float s = w * (a3 - m);
        bl[i] += s;
        br[i] -= s;
    }
    sl.widthCur = sl.widthEnd;
    if (sl.widthEnd == 0.0f) {
        std::memset(sl.ap, 0, sizeof(sl.ap));   // off: the next start begins clean
        return;
    }
    // No denormals in silence.
    const float st[8] = {x0, y0, x1, y1, x2, y2, x3, y3};
    for (int k = 0; k < 8; ++k) sl.ap[k / 2][k % 2] = std::fabs(st[k]) < 1.0e-20f ? 0.0f : st[k];
}

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
