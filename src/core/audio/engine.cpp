// Audio engine root (spec 04 §1, §5, §6).
#include "audio/engine.h"
#include "fastrun.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

#include "audio/ring_io.h"
#include "audio/zero_snap.h"
#include "formats/wav.h"

namespace ot {
namespace audio {

namespace {
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

// ---------------------------------------------------------------------------
// SampleBank (spec 02 §8.2)
// ---------------------------------------------------------------------------

void SampleBank::init(int16_t* pool, size_t cap) {
    pool_ = pool;
    const size_t maxSamples = kSampleBankBytes / 2;
    cap_ = cap > maxSamples ? maxSamples : cap;
    clear();
}

void SampleBank::clear() {
    for (auto& s : slots_) s = Slot{};
    reserved_ = 0;
}

bool SampleBank::setReserved(size_t samples) {
    if (samples > cap_ - usedSamples()) return false;
    reserved_ = samples;
    return true;
}

int SampleBank::lastLoaded() const {
    for (int j = kSlots - 1; j >= 0; --j)
        if (slots_[j].loaded) return j;
    return -1;
}

uint32_t SampleBank::startOf(int idx) const {
    // Start = end of the nearest lower-index loaded instrument.
    for (int j = idx - 1; j >= 0; --j)
        if (slots_[j].loaded) return slots_[j].off + slots_[j].samples();
    return 0;
}

size_t SampleBank::usedSamples() const {
    for (int j = kSlots - 1; j >= 0; --j)
        if (slots_[j].loaded) return slots_[j].off + slots_[j].samples();
    return 0;
}

int16_t* SampleBank::reserve(int idx, uint32_t frames, bool stereo) {
    if (valid(idx)) slots_[idx].full = 0;   // rewritten: no longer a streamed head
    if (!valid(idx) || !pool_) return nullptr;
    const uint32_t need = frames * (stereo ? 2u : 1u);
    const uint32_t old = slots_[idx].samples();
    const size_t used = usedSamples();
    if (used - old + need > cap_ - reserved_) return nullptr;
    const uint32_t start = startOf(idx);
    const uint32_t oldEnd = start + old;
    const uint32_t newEnd = start + need;
    if (newEnd != oldEnd && used > oldEnd)
        std::memmove(pool_ + newEnd, pool_ + oldEnd, (used - oldEnd) * sizeof(int16_t));
    for (int j = idx + 1; j < kSlots; ++j)
        if (slots_[j].loaded) slots_[j].off = slots_[j].off - oldEnd + newEnd;
    slots_[idx].off = start;
    slots_[idx].frames = frames;
    slots_[idx].stereo = stereo;
    slots_[idx].loaded = true;
    return pool_ + start;
}

bool SampleBank::setPartial(int idx, uint32_t fullFrames) {
    if (!loaded(idx) || fullFrames <= slots_[idx].frames) return false;
    slots_[idx].full = fullFrames;
    return true;
}

bool SampleBank::store(int idx, const int16_t* samples, uint32_t frames, bool stereo) {
    int16_t* dst = reserve(idx, frames, stereo);
    if (!dst) return false;
    if (samples) std::memcpy(dst, samples, size_t(frames) * (stereo ? 2u : 1u) * sizeof(int16_t));
    return true;
}

void SampleBank::remove(int idx) {
    if (!loaded(idx)) return;
    reserve(idx, 0, false);
    slots_[idx] = Slot{};
}

bool SampleBank::commitTail(int idx, uint32_t frames, bool stereo) {
    if (!valid(idx) || !pool_ || slots_[idx].loaded) return false;
    const uint32_t n = frames * (stereo ? 2u : 1u);
    if (n == 0) return true;
    const size_t used = usedSamples();
    if (n > cap_ - reserved_ - used) return false;
    const uint32_t start = startOf(idx);
    // Rotate the tail block down to the slot position; higher instruments move up.
    if (start < used) std::rotate(pool_ + start, pool_ + used, pool_ + used + n);
    for (int j = idx + 1; j < kSlots; ++j)
        if (slots_[j].loaded) slots_[j].off += n;
    slots_[idx].off = start;
    slots_[idx].frames = frames;
    slots_[idx].full = 0;
    slots_[idx].stereo = stereo;
    slots_[idx].loaded = true;
    return true;
}

bool SampleBank::copy(int src, int dst) {
    if (!valid(src) || !valid(dst) || src == dst) return false;
    remove(dst);
    if (!loaded(src)) return true;
    const uint32_t frames = slots_[src].frames;
    const bool st = slots_[src].stereo;
    int16_t* d = reserve(dst, frames, st);
    if (!d) return false;
    // reserve() may have moved src; read its current position.
    std::memcpy(d, pool_ + slots_[src].off, size_t(frames) * (st ? 2u : 1u) * sizeof(int16_t));
    return true;
}

void SampleBank::crop(int idx, uint32_t start, uint32_t n) {
    if (!loaded(idx)) return;
    const Slot s = slots_[idx];
    if (start >= s.frames) start = 0;
    if (n == 0 || start + n > s.frames) n = s.frames - start;
    int16_t* L = pool_ + s.off;
    std::memmove(L, L + start, n * sizeof(int16_t));
    if (s.stereo) std::memmove(L + n, L + s.frames + start, n * sizeof(int16_t));
    reserve(idx, n, s.stereo);   // shrinking keeps the first n*(ch) samples
}

bool SampleBank::replaceFromTail(int idx, uint32_t frames, bool stereo) {
    if (valid(idx)) slots_[idx].full = 0;
    if (!loaded(idx)) return false;
    const uint32_t n = frames * (stereo ? 2u : 1u);
    const size_t used = usedSamples();
    if (n == 0 || n > cap_ - reserved_ - used) return false;
    const uint32_t start = slots_[idx].off;
    const uint32_t old = slots_[idx].samples();
    if (n == old) {
        // Same size (most Sample Editor effects, and their undo): C and N
        // swap places, the rest of the bank stays where it is. Block copies
        // through a small buffer instead of rotating everything after the
        // slot twice (a few hundred ms per apply in a loaded project).
        int16_t buf[128];
        for (uint32_t i = 0; i < n; i += 128) {
            const size_t k = (n - i < 128 ? n - i : 128) * sizeof(int16_t);
            std::memcpy(buf, pool_ + start + i, k);
            std::memcpy(pool_ + start + i, pool_ + used + i, k);
            std::memcpy(pool_ + used + i, buf, k);
        }
    } else {
        // Another size: [C][B][N][free] -> [N][B][C]. With room in the free
        // memory after N, block moves (sequential, the device's memmove):
        // C (and, when the result grows, N) is parked there first, so no move
        // overwrites data it still needs. Otherwise the two rotations.
        int16_t* const s = pool_ + start;
        const size_t b = used - start - old;   // B: the slots after this one
        const size_t room = cap_ - reserved_ - used - n;
        int16_t* const park = pool_ + used + n;
        if (n < old && room >= old) {
            std::memcpy(park, s, old * sizeof(int16_t));                    // C aside
            std::memmove(s, pool_ + used, n * sizeof(int16_t));             // N to the front
            std::memmove(s + n, s + old, b * sizeof(int16_t));              // B left
            std::memmove(s + n + b, park, old * sizeof(int16_t));           // C to the tail
        } else if (n > old && room >= size_t(old) + n) {
            std::memcpy(park, s, old * sizeof(int16_t));                    // C aside
            std::memcpy(park + old, pool_ + used, n * sizeof(int16_t));     // N aside
            std::memmove(s + n, s + old, b * sizeof(int16_t));              // B right
            std::memcpy(s, park + old, n * sizeof(int16_t));                // N to the front
            std::memmove(s + n + b, park, old * sizeof(int16_t));           // C to the tail
        } else {
            // [C][B][N] -> [N][C][B] -> [N][B][C]
            std::rotate(s, pool_ + used, pool_ + used + n);
            std::rotate(s + n, s + n + old, pool_ + used + n);
        }
        for (int j = idx + 1; j < kSlots; ++j)
            if (slots_[j].loaded) slots_[j].off = slots_[j].off - old + n;
    }
    slots_[idx].frames = frames;
    slots_[idx].stereo = stereo;
    return true;
}

const int16_t* SampleBank::left(int idx) const { return loaded(idx) ? pool_ + slots_[idx].off : nullptr; }
const int16_t* SampleBank::right(int idx) const {
    if (!loaded(idx)) return nullptr;
    return pool_ + slots_[idx].off + (slots_[idx].stereo ? slots_[idx].frames : 0u);
}

// ---------------------------------------------------------------------------
// MixerSettings <- project (offsets = spec 04 RAM address - 0x20004780)
// ---------------------------------------------------------------------------

void MixerSettings::loadFromProject(const Project& p) {
    auto lvl = [&](uint32_t off) { return p.u8(off) > 100 ? uint8_t(100) : p.u8(off); };
    delayFeedback = lvl(0x11A);                                        // spec 02 §3.3 [TL]
    delayTimeMs = p.u16(0x11C);                                        // RAM 0x2000489C
    const uint8_t dp = p.u8(0x11E);                                    // RAM 0x2000489E
    delayPingPong = (dp & 0x80) != 0;
    delaySync = (dp & 0x40) != 0;
    delaySyncIndex = uint8_t(dp & 0x3F);
    // Limiter (spec 07 Q2): attack u16 ms at 0x120, release f32 ms at 0x124
    // (New Project 0.512 ms is kept as is), threshold u16 at 0x128, mode
    // 0x4FA, side-chain 0x4FB.
    limiterAttackMs = float(p.u16(0x120));
    limiterReleaseMs = p.f32(0x124);
    if (!(limiterReleaseMs == limiterReleaseMs)) limiterReleaseMs = 10.0f;   // NaN guard
    limiterThreshold = p.u16(0x128) > 32767 ? uint16_t(32767) : p.u16(0x128);
    limiterMode = p.u8(0x4FA) > 1 ? 0 : p.u8(0x4FA);
    sidechain = p.u8(0x4FB) > 10 ? 0 : p.u8(0x4FB);
    for (int t = 0; t < kVoices; ++t) trackMute[t] = p.u8(0x12C + uint32_t(t)) != 0;   // RAM 0x200048AC
    reverbSize = clampf(p.f32(0x418), 0.0f, 1.0f);
    reverbDamping = clampf(p.f32(0x41C), 0.0f, 1.0f);
    reverbPredelay = clampf(p.f32(0x420), 0.0f, 1.0f);
    reverbDiffusion = clampf(p.f32(0x424), 0.0f, 1.0f);
    if (!(reverbSize == reverbSize)) reverbSize = 0.7f;   // NaN guard
    if (!(reverbDamping == reverbDamping)) reverbDamping = 0.2f;
    if (!(reverbPredelay == reverbPredelay)) reverbPredelay = 0.0f;
    if (!(reverbDiffusion == reverbDiffusion)) reverbDiffusion = 0.625f;
    lineInLevel = lvl(0x309);                                          // RAM 0x20004A89
    lineInMute = p.u8(0x30A) != 0;
    lineInDelaySend = lvl(0x30B);
    lineInReverbSend = lvl(0x30D);
    for (int t = 0; t < kVoices; ++t) trackLevel[t] = lvl(0x4D0 + uint32_t(t));
    // Global Mixer (spec 07 Q3, Q4).
    reverbLevel = lvl(0x4D8);
    delayLevel = lvl(0x4D9);
    reverbMute = p.u8(0x4DA) != 0;
    delayMute = p.u8(0x4DB) != 0;
    dryMute = p.u8(0x4DC) != 0;
    dryMix = lvl(0x4DD);
    postFaderSends = p.u8(0x4FC) != 0;
    delayFilterHz = p.u16(0x5FE);                                      // RAM 0x20004D7E
    if (delayFilterHz < 100 || delayFilterHz > 20000) delayFilterHz = 20000;
    extraHeadroom = p.u8(0x600) != 0;
    saturationMode = p.u8(0x601) > 4 ? 0 : p.u8(0x601);
    antiAliasing = p.u8(0x4F9) != 0;                                   // RAM 0x20004C79
    gSnapToZero = p.u8(0x602) == 1;   // Snap to zero hook (zero_snap.h; spec 13 §H.2)
}

// ---------------------------------------------------------------------------
// Built-in metronome click (spec 07 Q10). OpenTracker's own sound, generated
// at start-up: a 256-frame decaying tone burst with a short noise-free
// attack transient. Played at note index 30 (rate ~0.177) it lasts ~33 ms.
// ---------------------------------------------------------------------------

namespace {
constexpr uint32_t kClickFrames = 256;
int16_t gClick[kClickFrames];
bool gClickReady = false;

void buildClick() {
    if (gClickReady) return;
    for (uint32_t n = 0; n < kClickFrames; ++n) {
        const float env = std::exp(-float(n) / 40.0f) * (n < 3 ? float(n + 1) / 3.0f : 1.0f);
        const float tone = 0.7f * std::sin(6.2831853f * float(n) / 5.0f) + 0.3f * std::sin(6.2831853f * float(n) / 2.5f);
        gClick[n] = int16_t(26000.0f * env * tone);
    }
    gClickReady = true;
}
}  // namespace

// ---------------------------------------------------------------------------
// AudioEngine
// ---------------------------------------------------------------------------

AudioEngine::AudioEngine() {
    initTables();
    gov_.setPool(&synth_);   // the governor only ever reduces synth voices (2x-02 §3.1)
    synth_.setGovernor(&gov_);
}

bool AudioEngine::init(int16_t* fx, size_t fxSamples, int16_t* pool, size_t poolSamples, const Instrument* ins) {
    initTables();
    if (!fx || fxSamples < kFxMemorySamples || !ins) return false;
    instruments_ = ins;
    bank_.init(pool, pool ? poolSamples : 0);
    delay_.init(fx);
    reverb_.init(fx + Delay::kMemorySamples);
    for (int v = 0; v <= kVoices; ++v) {
        voices_[v].reset();
        voices_[v].setSeed(0x9E3779B9u * uint32_t(v + 1));
    }
    for (int t = 0; t < kVoices; ++t) head_[t] = -1;
    limL_.reset();
    limR_.reset();
    dynL_.reset();
    dynR_.reset();
    dynActive_ = false;
    master_.jump(trkGain(mix_.masterVolume));
    click_.jump(sendGain(mix_.metronomeVolume));
    buildClick();
    for (int a = 0; a < 2; ++a) {
        // Envelope off (delay = attack = hold = decay = release = 0, sustain 1):
        // the whole sample plays once; VOL[50] = 1, centred (spec 07 Q10).
        Instrument& c = clickIns_[a];
        c.setDefault();
        c.env[kAutoVolume].enabled = 0;
        c.volume = 50;
        c.panning = 50;
        c.finetune = int8_t(a ? 100 : 45);
        c.lengthFrames = kClickFrames;
    }
    lineLvl_.jump(0.0f);
    lineSendD_.jump(0.0f);
    lineSendR_.jump(0.0f);
    cDelayT_ = 0;
    cDelayHz_ = 0;
    cLimA_ = cLimR_ = -1.0f;
    cLimMode_ = 0xFF;
    ready_ = true;
    return true;
}

void AudioEngine::stopAll() {
    raw_.active = false;
    gov_.reset();
    for (auto& v : voices_) v.stop();
    synth_.stopAll();
}

void AudioEngine::clearEffects() {
    delay_.clear();
    reverb_.clear();
    limL_.reset();
    limR_.reset();
    dynL_.reset();
    dynR_.reset();
    dynActive_ = false;
}

// --- ISequencerSink ----------------------------------------------------------

// Note work inside the pre-block hook is attributed to smpOn / synOn of the
// block profile (block_profile.h); main-loop calls (auditions) are not.
namespace {
struct NoteStamp {
    uint32_t* acc;
    uint32_t c0;
    explicit NoteStamp(uint32_t* a) : acc(a), c0(a ? CpuMeter::stampCycles() : 0u) {}
    ~NoteStamp() {
        if (acc) *acc += CpuMeter::stampCycles() - c0;
    }
};
}  // namespace

void AudioEngine::voiceNoteOn(uint8_t track, const VoiceNoteOn& n) {
    NoteStamp st(inHook_ ? &prof_.smpOn : nullptr);
    if (!ready_ || track >= kVoices || n.instrument >= kSampleInstrumentCount) return;
    const Instrument& ins = instruments_[n.instrument];
    if (!ins.active || !bank_.loaded(n.instrument)) return;   // spec 03 §6: ignored if not loaded
    // 2.x: a planar stereo instrument plays both channels (2x-04 §3; stock
    // 1.9.x played channel 0 only, spec 07 Q1). The voice falls back to the
    // left channel for wavetable / granular.
    int16_t step0 = 0;
    uint32_t tick = 0;
    transportPlaying(&tick, &step0);
    voices_[track].setLfoStepLatch(step0);
    if (inHook_) BlockProfile::inc(prof_.nSmpOn);
    const StreamSource* ss = rings_ ? streams_[n.instrument] : nullptr;
    if (ss) {
        // Streamed: the bank holds the head; the voice reads the rest from its
        // track's ring. Other play modes need the whole sample in memory.
        if (!streamablePlayMode(ins.playMode)) return;
        voices_[track].noteOn(&ins, ss->headL, ss->frames, n.note, n.volume, n.fxA.type, n.fxA.value, n.fxB.type,
                              n.fxB.value, ss->headR, ss, &rings_[track]);
    } else {
        // A partial (streamed-head) slot never plays as a whole sample: no
        // source bound yet, streaming turned off, or a bank move in progress.
        if (bank_.partial(n.instrument)) return;
        voices_[track].noteOn(&ins, bank_.left(n.instrument), bank_.frames(n.instrument), n.note, n.volume,
                              n.fxA.type, n.fxA.value, n.fxB.type, n.fxB.value,
                              bank_.stereo(n.instrument) ? bank_.right(n.instrument) : nullptr, nullptr,
                              rings_ ? &rings_[track] : nullptr);
    }
    pendingOff_[track] = 0;
    startedInBlock_[track] = true;
}

void AudioEngine::voiceNoteOff(uint8_t track, NoteOffMode mode) {
    NoteStamp st(inHook_ ? &prof_.smpOn : nullptr);
    if (track >= kVoices) return;
    if (startedInBlock_[track]) {   // applied at the next block's start
        pendingOff_[track] = int8_t(mode);
        return;
    }
    voices_[track].noteOff(int8_t(mode));
}

void AudioEngine::voiceApplyFx(uint8_t track, uint8_t slot, uint8_t type, uint8_t value) {
    NoteStamp st(inHook_ ? &prof_.smpOn : nullptr);
    if (track >= kVoices) return;
    int16_t step0 = 0;
    uint32_t tick = 0;
    transportPlaying(&tick, &step0);
    voices_[track].setLfoStepLatch(step0);   // latched only by LFO FX (spec 07 Q5)
    voices_[track].applyFx(slot, type, value);
}

bool AudioEngine::transportPlaying(uint32_t* tick, int16_t* step0) const {
    *tick = 0;
    *step0 = 0;
    return transport_ ? transport_(transportCtx_, tick, step0) : false;
}

void AudioEngine::resetAllVoiceFx() {
    for (int t = 0; t < kVoices; ++t) voices_[t].resetFx();
    synth_.resetFx();
}

// 2.x synth slots (synth_bank.h): instruments 64..66 on any track.
void AudioEngine::synthNoteOn(uint8_t track, const SynthNoteOn& n) {
    NoteStamp st(inHook_ ? &prof_.synOn : nullptr);
    if (ready_) synth_.noteOn(track, n);
}
void AudioEngine::synthNoteOff(uint8_t track, NoteOffMode mode) {
    NoteStamp st(inHook_ ? &prof_.synOn : nullptr);
    synth_.noteOff(track, mode);
}
void AudioEngine::synthApplyFx(uint8_t track, uint8_t type, uint8_t value) {
    NoteStamp st(inHook_ ? &prof_.synOn : nullptr);
    synth_.applyFx(track, type, value);
}
void AudioEngine::synthMacroFx(uint8_t slot, uint8_t macro, uint8_t value) {
    NoteStamp st(inHook_ ? &prof_.synOn : nullptr);
    synth_.macroFx(slot, macro, value);
}
void AudioEngine::synthTransportStop() { synth_.transportStop(); }

void AudioEngine::metronomeClick(bool accent, uint8_t velocity) {
    // Built-in click on the ninth player (spec 07 Q10): note index 30, accent
    // +100 cents, normal +45 cents (the sink "velocity" is that finetune).
    if (!ready_ || offline_) return;   // not in exports
    Instrument& c = clickIns_[accent ? 1 : 0];
    c.finetune = int8_t(velocity > 100 ? 100 : velocity);
    click_.jump(sendGain(mix_.metronomeVolume));   // written at every click
    voices_[kMetronomeVoice].noteOn(&c, gClick, kClickFrames, 30, -1, 0, 0, 0, 0);
}

void AudioEngine::midiSend(uint8_t portMask, const uint8_t* bytes, uint8_t len) {
    if (offline_ || !midi_) return;   // MIDI is suppressed while rendering (spec 03 §1.1)
    if (inHook_) BlockProfile::inc(prof_.nMidiOut);
    routeMidi(*midi_, portMask, bytes, len);
}

void AudioEngine::previewBuffer(const int16_t* data, uint32_t frames, bool stereo, uint32_t avail, uint32_t step) {
    raw_.active = false;
    if (!data || !frames) return;
    raw_.data = data;
    raw_.right = stereo ? data + frames : nullptr;   // planar: R follows L
    raw_.frames = frames;
    raw_.avail = avail < frames ? avail : frames;   // set before active
    raw_.step = step ? step : 65536u;
    raw_.frac = 0;
    raw_.pos = 0;
    raw_.active = true;
}

void AudioEngine::previewBufferLR(const int16_t* left, const int16_t* right, uint32_t frames, uint32_t step) {
    raw_.active = false;
    if (!left || !frames) return;
    raw_.data = left;
    raw_.right = right;
    raw_.frames = frames;
    raw_.avail = frames;
    raw_.step = step ? step : 65536u;
    raw_.frac = 0;
    raw_.pos = 0;
    raw_.active = true;
}

void AudioEngine::previewAvail(uint32_t frames) { raw_.avail = frames < raw_.frames ? frames : raw_.frames; }

void AudioEngine::previewStop() { raw_.active = false; }

float AudioEngine::busPeak(int ch, int side) {
    if (ch < 0 || ch > 3 || side < 0 || side > 1) return 0.0f;
    const float p = busPeak_[ch][side];
    busPeak_[ch][side] = 0.0f;
    return p;
}

float AudioEngine::trackPeak(int t) {
    if (t < 0 || t >= kVoices) return 0.0f;
    const float p = peak_[t];
    peak_[t] = 0.0f;
    // A track playing a synth meters its synth notes too (2.x).
    const float s = synth_.trackPeak(t);
    return s > p ? s : p;
}

// --- per-block control ---------------------------------------------------------

void AudioEngine::updateParams() {
    const MixerSettings& m = mix_;
    // Delay (§5.1).
    float ms = float(m.delayTimeMs);
    if (m.delaySync) ms = delaySyncMs(tempo_, m.delaySyncIndex);
    const uint32_t t = delayTimeSamples(ms);
    if (t != cDelayT_) {
        delay_.setTimeSamples(t);
        cDelayT_ = t;
    }
    if (m.delayFilterHz != cDelayHz_) {
        delay_.setFilter(float(m.delayFilterHz));
        cDelayHz_ = m.delayFilterHz;
    }
    delay_.setFeedbackK(delayFeedbackK(m.delayFeedback));
    delay_.setPingPong(m.delayPingPong);
    // Reverb (§5.2).
    reverb_.setSize(m.reverbSize);
    reverb_.setDamping(m.reverbDamping);
    reverb_.setDiffusion(m.reverbDiffusion);
    reverb_.setPredelay(m.reverbPredelay);
    // Limiter (§5.3, spec 07 Q2): THR = LIMTHR[min(v, 32767) * 100 / 32767].
    const float thr = limiterThreshold(m.limiterThreshold);
    limL_.setThreshold(thr);
    limR_.setThreshold(thr);
    if (m.limiterAttackMs != cLimA_ || m.limiterReleaseMs != cLimR_ || m.limiterMode != cLimMode_) {
        cLimA_ = m.limiterAttackMs;
        cLimR_ = m.limiterReleaseMs;
        cLimMode_ = m.limiterMode;
        const float ca = limiterCoef(cLimA_, cLimMode_), cr = limiterCoef(cLimR_, cLimMode_);
        limL_.setCoefs(ca, cr);
        limR_.setCoefs(ca, cr);
    }
    // Mixer gains (§5.4, §5.5).
    master_.set(trkGain(m.masterVolume));
    click_.set(sendGain(m.metronomeVolume));   // OUT input 5 (spec 07 Q10)
    const float lineLevel = m.lineInMute ? 0.0f : trkGain(m.lineInLevel);
    // Recorder monitor: MSUM line-in gain = table[0x56] = 1.0 (spec 22 §5).
    // The line sends keep the Global Mixer level (stock writes only MSUM).
    lineLvl_.set(lineMonitor_ ? 1.0f : lineLevel);
    auto lineSend = [&](uint8_t v) {
        float g = 0.5f * trkGain(v);
        if (m.postFaderSends) g *= lineLevel;
        return g < 0.01f ? 0.0f : g;
    };
    lineSendD_.set(m.delayMute ? 0.0f : lineSend(m.lineInDelaySend));
    lineSendR_.set(m.reverbMute ? 0.0f : lineSend(m.lineInReverbSend));
}

void AudioEngine::controlBlock(float blockUs) {
    updateParams();
    // Tempo-locked LFO phase source (spec 07 Q5).
    uint32_t tick = 0;
    int16_t step0 = 0;
    const bool playing = transportPlaying(&tick, &step0);
    playing_ = playing;

    const MixerSettings& m = mix_;
    bool anySolo = false;
    for (int t = 0; t < kVoices; ++t) anySolo |= m.trackSolo[t];
    // Global Mixer (spec 07 Q3/Q4): TRK law, 0 when muted.
    const float dry = m.dryMute ? 0.0f : trkGain(m.dryMix);
    const float dl = m.delayMute ? 0.0f : trkGain(m.delayLevel);
    const float rl = m.reverbMute ? 0.0f : trkGain(m.reverbLevel);
    for (int t = 0; t <= kVoices; ++t) {
        VoiceControl& c = vc_[t];
        c.tempoBpm = tempo_;
        c.blockUs = blockUs;
        c.seqPlaying = playing;
        c.seqTick = tick;
        c.headroom = m.extraHeadroom;
        c.antiAliasing = m.antiAliasing;
        if (t == kMetronomeVoice) {   // click: unity, no sends, no headroom
            c.trackGain = 1.0f;
            c.dryGain = 1.0f;
            c.delayLevel = c.reverbLevel = 0.0f;
            c.postFaderSends = false;
            c.muted = false;
            c.headroom = false;
            c.antiAliasing = true;
        } else {
            c.muted = m.trackMute[t] || (anySolo && !m.trackSolo[t]);
            c.trackGain = trkGain(m.trackLevel[t]);
            c.dryGain = dry;
            c.delayLevel = dl;
            c.reverbLevel = rl;
            c.postFaderSends = m.postFaderSends;
        }
        c.ampTap = t == kMetronomeVoice;   // o.amp of stereo voices: metronome + side-chain track
        voices_[t].control(c);
    }
    synthIn_.delayLevel = dl;   // synth sends scale with the Global Mixer like track sends
    synthIn_.reverbLevel = rl;
    // Synth notes follow their track's Track Mixer fader and mute / solo,
    // like the sample voice of that track (tracks 1-8; 9-16 have no fader).
    for (int t = 0; t < 16; ++t)
        synthIn_.trackGain[t] =
            t < kVoices ? ((m.trackMute[t] || (anySolo && !m.trackSolo[t])) ? 0.0f : trkGain(m.trackLevel[t])) : 1.0f;
    synth_.setTempo(tempo_);
    if (m.sidechain >= 1 && m.sidechain <= kVoices) vc_[m.sidechain - 1].ampTap = true;   // §5.3 source
}

// --- block render (spec 04 §1.3 graph) ----------------------------------------

// Step 6 of renderBlock for one saturation mode (spec 04 §5.4).
template <int kMode>
inline __attribute__((always_inline)) void AudioEngine::masterSum(float l0, float li) {
    for (uint32_t i = 0; i < kBlock; ++i) {
        l0 += li;
        const float dl = saturate(mL_[i], kMode), dr = saturate(mR_[i], kMode);
        mL_[i] = saturate(dl + dL_[i] + rL_[i] + lineL_[i] * l0, kMode);
        mR_[i] = saturate(dr + dR_[i] + rR_[i] + lineR_[i] * l0, kMode);
    }
}

// Saturation mode 0 (the default) runs from SRAM_L (fastrun.h); the other
// modes stay inlined in renderBlock (flash).
OT_FASTRUN_FN void AudioEngine::masterSumClean(float l0, float li) { masterSum<0>(l0, li); }

void AudioEngine::renderBlock(bool offline, int16_t* master, StemBlock* stems) {
    const float blockSeconds = offline ? kRenderBlockSeconds : kLiveBlockSeconds;
    const float blockUs = offline ? kRenderBlockUs : kLiveBlockUs;
    // Phase stamps of the block profile (block_profile.h), live + timed only.
    const bool prof = profOn_ && !offline;
    auto stamp = [prof]() { return prof ? CpuMeter::stampCycles() : 0u; };
    synth_.setProfile(prof ? &prof_ : nullptr);
    const uint32_t pc0 = stamp();
    // 0. note-offs held back from the last block (voiceNoteOff).
    for (int t = 0; t < kVoices; ++t) {
        startedInBlock_[t] = false;
        if (const int8_t m = pendingOff_[t]) {
            pendingOff_[t] = 0;
            voices_[t].noteOff(m);
        }
    }
    // 1. sequencer advance (spec 04 §1.1 step 1 / §6.1)
    inHook_ = prof;
    if (hook_) hook_(hookCtx_, blockSeconds);
    inHook_ = false;
    const uint32_t pc1 = stamp();
    // 2. per-block control (§4.9)
    (void)blockSeconds;
    controlBlock(blockUs);
    const uint32_t pc2 = stamp();
    // 3. voices
    zeroWords(dsend_, sizeof(dsend_));
    zeroWords(rsend_, sizeof(rsend_));
    zeroWords(mL_, sizeof(mL_));
    zeroWords(mR_, sizeof(mR_));
    bool anyVoice = false;
    uint32_t sendsTouched = 0;   // bit 0 dsend_, bit 1 rsend_ (else still all zero)
    // [I] Resample tap on one track: that voice keeps its L/R like a stem.
    IResampleTap* const tap = offline ? nullptr : tap_;
    const int tapTrack = tap && tapSource_ >= 1 && tapSource_ <= kVoices ? tapSource_ - 1 : -1;
    bool tapVoiceOn = false;
    // The Track source accumulates in the track's own post-pan output
    // (vout_, no block buffer on the ISR stack): its voice, then the synth
    // notes it plays. A track that was tapped last block gets its output
    // zeroed once, so it never carries synth data into a later stem.
    if (tapLast_ >= 0 && tapLast_ != tapTrack) {
        zeroFloats(vout_[tapLast_].l, kBlock);
        zeroFloats(vout_[tapLast_].r, kBlock);
    }
    tapLast_ = int8_t(tapTrack);
    float* const tapL = tapTrack >= 0 ? vout_[tapTrack].l : nullptr;
    float* const tapR = tapTrack >= 0 ? vout_[tapTrack].r : nullptr;
    for (int t = 0; t < kVoices; ++t) {
        if (voices_[t].render(vc_[t], vout_[t], dsend_, rsend_, mL_, mR_, stems != nullptr || t == tapTrack)) {
            if (t == tapTrack) tapVoiceOn = true;
            anyVoice = true;
            sendsTouched |= voices_[t].sendsTouched();
            float pk = peak_[t];
            const float b = voices_[t].blockPeak();
            pk = b > pk ? b : pk;
            peak_[t] = pk > 1.0f ? 1.0f : pk;
            if (prof) BlockProfile::inc(prof_.nSmpVoices);
        }
        head_[t] = voices_[t].playhead();
    }
    if (tapTrack >= 0 && !tapVoiceOn) {   // a silent voice zeroes its output only once
        zeroFloats(tapL, kBlock);
        zeroFloats(tapR, kBlock);
    }
    const uint32_t pc3 = stamp();
    // 3b. synth slots (2.x M3): voices -> slot gain / pan -> main bus and
    //     sends; the Synth 1..3 stems are tapped from the slot buses.
    if (synth_.active() || stems) {
        int16_t* synthStems[kSynthSlots] = {};
        if (stems)
            for (int s = 0; s < kSynthSlots; ++s) synthStems[s] = stems->data[StemBlock::kFirstSynthStem + s];
        anyVoice |= synth_.render(synthIn_, mL_, mR_, dsend_, rsend_, synthStems, tapTrack, tapL, tapR);
        sendsTouched = 3;   // (the slots' send taps: not tracked)
    }
    const uint32_t pc4 = stamp();
    // 4. line input (§1.3 IG): channel mode, direct level, sends. A block of
    //    digital silence is treated like no input (identical result).
    const MixerSettings& m = mix_;
    bool line = false;
    if (lineIn_) {
        int32_t nz = 0;
        for (uint32_t i = 0; i < 2 * kBlock; ++i) nz |= lineIn_[i];
        if (nz) {
            constexpr float kS = 1.0f / 32768.0f;
            const uint8_t ch = m.lineInChannel;
            for (uint32_t i = 0; i < kBlock; ++i) {
                const float l = float(lineIn_[2 * i]) * kS, r = float(lineIn_[2 * i + 1]) * kS;
                lineL_[i] = ch == 2 ? r : l;
                lineR_[i] = ch == 1 ? l : r;
            }
            line = true;
            if (lineMonitor_) {   // Recorder monitor diag: mixed peak
                float pk = 0.0f;
                for (uint32_t i = 0; i < kBlock; ++i) {
                    const float a = std::fabs(lineL_[i]), b = std::fabs(lineR_[i]);
                    pk = a > pk ? a : pk;
                    pk = b > pk ? b : pk;
                }
                pk *= lineLvl_.target;
                if (pk > lineMonPeak_) lineMonPeak_ = pk;
            }
        }
        lineIn_ = nullptr;   // consumed
    }
    if (!line) {
        zeroWords(lineL_, sizeof(lineL_));
        zeroWords(lineR_, sizeof(lineR_));
    }
    {
        // Line-in send inputs 8/9 go through DYN0 (L) / DYN1 (R) (§5.6).
        // The dynamics only run while they can reach a send; with no input
        // or both send gains at 0 the line adds nothing to the sends and the
        // dynamics state just relaxes (idle(), free once settled).
        float d0, r0;
        const bool sends = line && (lineSendD_.cur != 0.0f || lineSendD_.target != 0.0f ||
                                    lineSendR_.cur != 0.0f || lineSendR_.target != 0.0f);
        const float di = lineSendD_.begin(d0), ri = lineSendR_.begin(r0);
        if (sends) {
            float dynL[kBlock], dynR[kBlock];
            dynL_.process(lineL_, dynL);
            dynR_.process(lineR_, dynR);
            dynActive_ = true;
            for (uint32_t i = 0; i < kBlock; ++i) {
                d0 += di;
                r0 += ri;
                dsend_[i] = clampUnitFast(dsend_[i] + (dynL[i] + dynR[i]) * d0);
                rsend_[i] = clampUnitFast(rsend_[i] + (dynL[i] + dynR[i]) * r0);
            }
        } else {
            if (dynActive_) {
                dynL_.idle();
                dynR_.idle();
                dynActive_ = !(dynL_.settled() && dynR_.settled());
            }
            // A send no voice added into is still all zero: no clamp pass.
            if (sendsTouched == 3) {
                for (uint32_t i = 0; i < kBlock; ++i) {
                    dsend_[i] = clampUnitFast(dsend_[i]);
                    rsend_[i] = clampUnitFast(rsend_[i]);
                }
            } else if (sendsTouched) {
                float* s = sendsTouched == 1 ? dsend_ : rsend_;
                for (uint32_t i = 0; i < kBlock; ++i) s[i] = clampUnitFast(s[i]);
            }
        }
    }
    // 5. effects (false: bypassed, outputs all zero)
    const uint32_t pc5 = stamp();
    const bool dly = delay_.process(dsend_, dL_, dR_);
    const uint32_t pc6 = stamp();
    const bool rev = reverb_.process(rsend_, rL_, rR_);
    const uint32_t pc7 = stamp();
    // 6. DRY (shaped) + returns (MSUM gains 1/2 fixed at 1.0, spec 07 Q3)
    //    + line-in -> MSUM (shaped) (§5.4). All inputs zero -> zero (every
    //    shaper maps 0 to 0), mL_/mR_ already are.
    {
        float l0;
        const float li = lineLvl_.begin(l0);
        if (anyVoice || dly || rev || line) {
            // Global Mixer meters: the four MSUM inputs before the sum (every
            // 8th frame, a UI reading like the synth slot meters; delay / reverb returns, the dry bus, line in x level).
            auto peak = [](const float* a, float g) {
                float p = 0.0f;
                for (uint32_t i = 0; i < kBlock; i += 8) {
                    const float v = a[i] < 0.0f ? -a[i] : a[i];
                    p = v > p ? v : p;
                }
                return p * g;
            };
            const float* bus[4][2] = {{dL_, dR_}, {rL_, rR_}, {mL_, mR_}, {lineL_, lineR_}};
            for (int ch = 0; ch < 4; ++ch)
                for (int sd = 0; sd < 2; ++sd) {
                    const float p = peak(bus[ch][sd], ch == 3 ? (l0 < 0.0f ? -l0 : l0) : 1.0f);
                    if (p > busPeak_[ch][sd]) busPeak_[ch][sd] = p;
                }
            switch (m.saturationMode) {
                case 1: masterSum<1>(l0, li); break;
                case 2: masterSum<2>(l0, li); break;
                case 3: masterSum<3>(l0, li); break;
                case 4: masterSum<4>(l0, li); break;
                default: masterSumClean(l0, li); break;   // the default mode, from SRAM_L (.fastrun)
            }
        }
    }
    // 7. limiter with optional side-chain (§5.3)
    const float* sc = nullptr;
    if (m.sidechain >= 1 && m.sidechain <= 8) {
        sc = vout_[m.sidechain - 1].amp;   // read-only, valid for this block
    } else if (m.sidechain == 9 || m.sidechain == 10) {
        sc = m.sidechain == 9 ? lineL_ : lineR_;
    }
    float limOutL[kBlock], limOutR[kBlock];
    const bool limL = limL_.process(mL_, sc, limOutL);
    const bool limR = limR_.process(mR_, sc, limOutR);
    if (tap) {
        // [I] Resample tap (see setResampleTap); a source that produced
        // nothing this block taps silence (null: IResampleTap). A Track
        // source is the track's sample voice plus the synth notes it plays
        // (tapL / tapR = its vout_), dry: no delay / reverb.
        const float *tl = tapL, *tr = tapR;
        if (tapTrack < 0) {
            if (limL) tl = limOutL;
            if (limR) tr = limOutR;
        }
        tap->tapBlock(tl, tr, kBlock);
    }
    // 8. OUT mixers (§5.4, mode 0): limiter * master volume + file preview
    //    (input 1) + metronome (input 5, SEND[metronomeVolume], after the
    //    limiter and master volume, centred; spec 07 Q10).
    bool click = false;
    if (!offline && voices_[kMetronomeVoice].active()) {
        float noSendD[kBlock], noSendR[kBlock];
        zeroFloats(noSendD, kBlock);
        zeroFloats(noSendR, kBlock);
        click = voices_[kMetronomeVoice].render(vc_[kMetronomeVoice], vout_[kMetronomeVoice], noSendD, noSendR);
    }
    // WAVPREVIEW (OUT input 1): SD file preview, planar int16 (spec 04 §1.3).
    float rawL[kBlock], rawR[kBlock];
    bool raw = false;
    if (raw_.active && !offline) {
        const float g = trkGain(m.previewVolume) * (1.0f / 32768.0f);
        const int16_t* d = raw_.data;
        const int16_t* dr = raw_.right;
        const uint32_t len = raw_.frames;
        uint32_t pos = raw_.pos;
        const uint32_t lim = raw_.avail;   // streamed: wait (silence) at the decoded end
        const uint32_t step = raw_.step;
        OT_SDRAM_ACCESS(dr ? 2 * kBlock : kBlock);
        if (step == 65536u) {   // own pitch: plain copy (file preview, Preview buttons)
            for (uint32_t i = 0; i < kBlock; ++i) {
                if (pos < lim) {
                    rawL[i] = float(d[pos]) * g;
                    rawR[i] = dr ? float(dr[pos]) * g : rawL[i];
                    ++pos;
                } else {
                    rawL[i] = rawR[i] = 0.0f;
                    if (lim >= len) ++pos;
                }
            }
        } else {
            // 2.x pitched pads: 16.16 step, linear interpolation per channel.
            // The fraction is used as 15 bits so (x1 - x0) * f stays in int32
            // (|x1 - x0| <= 65535, f <= 32767); no 64-bit math in the block.
            uint32_t frac = raw_.frac;
            for (uint32_t i = 0; i < kBlock; ++i) {
                if (pos < lim) {
                    const uint32_t nx = pos + 1 < lim ? pos + 1 : pos;   // last frame: hold
                    const int32_t f = int32_t(frac >> 1);
                    const int32_t l = d[pos] + (((int32_t(d[nx]) - d[pos]) * f) >> 15);
                    rawL[i] = float(l) * g;
                    if (dr) {
                        const int32_t r = dr[pos] + (((int32_t(dr[nx]) - dr[pos]) * f) >> 15);
                        rawR[i] = float(r) * g;
                    } else {
                        rawR[i] = rawL[i];
                    }
                    frac += step;
                    pos += frac >> 16;
                    frac &= 0xFFFFu;
                } else {
                    rawL[i] = rawR[i] = 0.0f;
                    if (lim >= len) ++pos;
                }
            }
            raw_.frac = frac;
        }
        raw_.pos = pos;
        if (pos >= len) raw_.active = false;
        raw = true;
    }
    float g0, c0;
    const float gi = master_.begin(g0), ci = click_.begin(c0);
    if (!limL && !limR && !raw && !click) {
        if (!(reinterpret_cast<uintptr_t>(master) & 3u)) zeroWords(master, kBlock * 2 * sizeof(int16_t));   // silence
        else std::memset(master, 0, kBlock * 2 * sizeof(int16_t));
    } else {
        const float* ca = vout_[kMetronomeVoice].amp;
        for (uint32_t i = 0; i < kBlock; ++i) {
            g0 += gi;
            c0 += ci;
            float l = limOutL[i] * g0, r = limOutR[i] * g0;
            if (raw) {
                l += rawL[i];
                r += rawR[i];
            }
            if (click) {
                const float c = ca[i] * c0;
                l += c;
                r += c;
            }
            master[2 * i] = toS16(l);
            master[2 * i + 1] = toS16(r);
        }
    }
    // 9. stem taps (§1.3 record queues; §6.1). Track stems carry the voice's
    //    post-pan / post-balance o.l / o.r, so stereo instruments keep their
    //    image (2x-04 §6). The Synth 1-3 stems (11..13) were written in 3b.
    if (stems) {
        for (int t = 0; t < kVoices; ++t) {
            int16_t* s = stems->data[t];
            const VoiceOut& o = vout_[t];
            for (uint32_t i = 0; i < kBlock; ++i) {
                s[2 * i] = toS16(o.l[i]);
                s[2 * i + 1] = toS16(o.r[i]);
            }
        }
        for (uint32_t i = 0; i < kBlock; ++i) {
            stems->data[8][2 * i] = toS16(rL_[i]);
            stems->data[8][2 * i + 1] = toS16(rR_[i]);
            stems->data[9][2 * i] = toS16(dL_[i]);
            stems->data[9][2 * i + 1] = toS16(dR_[i]);
        }
        std::memcpy(stems->data[10], master, sizeof(stems->data[10]));
    }
    ++blocks_;
    if (prof) {
        const uint32_t pc8 = stamp();
        const uint32_t notes = prof_.smpOn + prof_.synOn;
        prof_.seq = pc1 - pc0 > notes ? pc1 - pc0 - notes : 0u;
        prof_.ctrl = pc2 - pc1;
        prof_.smp = pc3 - pc2;
        uint32_t eng = 0;
        for (uint32_t c : prof_.slot) eng += c;
        prof_.synFw = pc4 - pc3 > eng ? pc4 - pc3 - eng : 0u;
        prof_.dly = pc6 - pc5;
        prof_.rev = pc7 - pc6;
        prof_.out = (pc5 - pc4) + (pc8 - pc7);   // line / sends; master, limiter, metronome, output
        synth_.setProfile(nullptr);
    }
}

void AudioEngine::renderLive(int16_t* out) {
    if (!ready_ || callbackDisabled_) {
        std::memset(out, 0, kBlock * 2 * sizeof(int16_t));
        return;
    }
    renderBlock(false, out, nullptr);
}

void AudioEngine::fillCallback(int16_t* out, uint32_t frames, void* ctx) {
    AudioEngine* e = static_cast<AudioEngine*>(ctx);
    uint32_t done = 0;
    const bool timed = CpuMeter::cycleSource() != nullptr;
    while (done + kBlock <= frames) {
        if (timed) e->prof_ = BlockProfile{};
        e->profOn_ = timed;
        const uint32_t c0 = timed ? CpuMeter::cycles() : 0u;
        e->renderLive(out + 2 * done);
        done += kBlock;
        if (!timed) continue;
        // 2.x CPU meter + governor (spec 2x-02 §3.2: after render, per block).
        const uint32_t cyc = CpuMeter::cycles() - c0;
        e->cpu_.addBlock(cyc);
        e->prof_.total = cyc;
        e->profWin_.add(e->prof_, e->cpu_.windowEnded());
        if (e->govPlaying_ && !e->playing_) e->gov_.reset();   // Stop
        e->govPlaying_ = e->playing_;
        e->gov_.evaluate(e->cpu_);
    }
    if (done < frames) std::memset(out + 2 * done, 0, (frames - done) * 2 * sizeof(int16_t));
}

// --- offline (spec 04 §6.1) ---------------------------------------------------

void AudioEngine::beginOffline() {
    callbackDisabled_ = true;
    gov_.setEnabled(false);   // spec 2x-02 §3.5
    offline_ = true;
    clearEffects();
    stopAll();
    for (int t = 0; t < kVoices; ++t) voices_[t].resetFx();
}

void AudioEngine::renderOfflineBlock(int16_t* master, StemBlock* stems) {
    if (!ready_) {
        std::memset(master, 0, kBlock * 2 * sizeof(int16_t));
        return;
    }
    renderBlock(true, master, stems);
}

void AudioEngine::endOffline() {
    gov_.reset();
    gov_.setEnabled(true);
    offline_ = false;
    callbackDisabled_ = false;
}

namespace {
// Patch the WAV header sample-rate fields to 44117 Hz (spec 04 §6.1).
bool patchRate(IStorage& fs, const char* path) {
    IStorage::Handle h = fs.open(path, IStorage::kReadWrite);
    if (h < 0) return false;
    const uint32_t rate = 44117, byteRate = 44117u * 4u;
    const uint8_t b[8] = {uint8_t(rate), uint8_t(rate >> 8), uint8_t(rate >> 16), uint8_t(rate >> 24),
                          uint8_t(byteRate), uint8_t(byteRate >> 8), uint8_t(byteRate >> 16), uint8_t(byteRate >> 24)};
    bool ok = fs.seek(h, 24) && fs.write(h, b, 8) == 8;
    fs.close(h);
    return ok;
}

// [I] Convert device-rate audio (180 MHz * 16 / 255 / 256 = 44100 * 2500 /
// 2499 Hz) to 44.1 kHz WAV samples without changing pitch or duration. Stock
// writes the device-rate samples with 44117 in the header (spec 04 §6.1);
// DAWs at 44.1 kHz then resample or play them 0.04 % slow. The position runs
// in exact integer steps of 2500/2499 input frames per output frame; the
// interpolation is the WAV import's (cubicS16, formats/wav.h).
class ExportResampler {
public:
    static constexpr uint32_t kInputPerOutput = 2500;
    static constexpr uint32_t kOutputPerInput = 2499;

    uint32_t push(const int16_t* in, uint32_t frames, int16_t* out) {
        const uint32_t start = outputFrames_;
        for (uint32_t i = 0; i < frames; ++i) {
            for (int ch = 0; ch < 2; ++ch) history_[ch][inputFrames_ & 3u] = in[2 * i + ch];
            ++inputFrames_;
            while (sourceIndex_ + 2 < inputFrames_) emit(out, start);
        }
        return outputFrames_ - start;
    }

    uint32_t finish(int16_t* out) {
        const uint32_t start = outputFrames_;
        const uint32_t target = uint32_t(uint64_t(inputFrames_) * kOutputPerInput / kInputPerOutput);
        while (outputFrames_ < target) emit(out, start);
        return outputFrames_ - start;
    }

private:
    int16_t sample(int channel, int64_t frame) const {
        if (frame < 0 || uint64_t(frame) >= inputFrames_) return 0;
        return history_[channel][uint32_t(frame) & 3u];
    }

    void emit(int16_t* out, uint32_t start) {
        const float mu = float(phase_) / float(kOutputPerInput);
        const uint32_t outFrame = outputFrames_ - start;
        for (int ch = 0; ch < 2; ++ch)
            out[2 * outFrame + ch] = cubicS16(float(sample(ch, int64_t(sourceIndex_) - 1)),
                                              float(sample(ch, sourceIndex_)), float(sample(ch, sourceIndex_ + 1)),
                                              float(sample(ch, sourceIndex_ + 2)), mu);
        ++outputFrames_;
        phase_ += kInputPerOutput;
        sourceIndex_ += phase_ / kOutputPerInput;
        phase_ %= kOutputPerInput;
    }

    int16_t history_[2][4] = {};
    uint32_t inputFrames_ = 0;
    uint32_t outputFrames_ = 0;
    uint32_t sourceIndex_ = 0;
    uint32_t phase_ = 0;
};

void joinPath(char* dst, size_t cap, const char* dir, const char* name) {
    size_t n = 0;
    for (const char* p = dir; *p && n + 1 < cap; ++p) dst[n++] = *p;
    if (n > 0 && dst[n - 1] != '/' && n + 1 < cap) dst[n++] = '/';
    for (const char* p = name; *p && n + 1 < cap; ++p) dst[n++] = *p;
    dst[n] = 0;
}
}  // namespace

int32_t AudioEngine::exportWav(IStorage& fs, const char* path, bool stems, uint32_t maxBlocks,
                               bool (*running)(void*), void* ctx, bool resampleTo44100) {
    static const char* const kNames[StemBlock::kStems] = {"track1.wav", "track2.wav", "track3.wav", "track4.wav",
                                                          "track5.wav", "track6.wav", "track7.wav", "track8.wav",
                                                          "reverb.wav", "delay.wav",  "master.wav", "synth_1.wav",
                                                          "synth_2.wav", "synth_3.wav"};
    const int nw = stems ? StemBlock::kStems : 1;
    StemBlock* stemBuf = exportScratch_;
    std::unique_ptr<StemBlock> ownStems;
    if (stems && !stemBuf) {
        ownStems.reset(new (std::nothrow) StemBlock);
        stemBuf = ownStems.get();
        if (!stemBuf) return -1;
    }
    WavWriter writers[StemBlock::kStems];
    char names[StemBlock::kStems][160];
    if (stems) fs.mkdir(path);
    for (int i = 0; i < nw; ++i) {
        if (stems) joinPath(names[i], sizeof(names[i]), path, kNames[i]);
        else joinPath(names[i], sizeof(names[i]), "", path);
        if (!writers[i].open(fs, names[i], 2)) {
            for (int j = 0; j < i; ++j) writers[j].close();
            return -1;
        }
    }
    beginOffline();
    int16_t masterBuf[kBlock * 2];
    int16_t rateBuf[kBlock * 2];
    ExportResampler resamplers[StemBlock::kStems];
    uint32_t n = 0;
    bool ok = true;
    while (n < maxBlocks && (!running || running(ctx))) {
        renderOfflineBlock(masterBuf, stems ? stemBuf : nullptr);
        for (int i = 0; i < nw && ok; ++i) {
            const int16_t* src = stems ? stemBuf->data[i] : masterBuf;
            if (!resampleTo44100) {
                ok = writers[i].write(src, kBlock);
                continue;
            }
            const uint32_t outFrames = resamplers[i].push(src, kBlock, rateBuf);
            ok = !outFrames || writers[i].write(rateBuf, outFrames);
        }
        if (!ok) break;
        ++n;
    }
    endOffline();
    for (int i = 0; i < nw; ++i) {
        if (resampleTo44100) {
            const uint32_t outFrames = resamplers[i].finish(rateBuf);
            if (outFrames) ok = writers[i].write(rateBuf, outFrames) && ok;
        }
        ok = writers[i].close() && ok;
        if (!resampleTo44100) ok = patchRate(fs, names[i]) && ok;
    }
    return ok ? int32_t(n) : -1;
}

}  // namespace audio
}  // namespace ot
