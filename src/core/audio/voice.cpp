// Sample voice (spec 04 §2-§4).
#include "audio/voice.h"
#include "fastrun.h"
#include "audio/ring_io.h"
#include "audio/zero_snap.h"

#include <cmath>
#include <cstring>

namespace ot {
namespace audio {

namespace {

constexpr float kFadeMs = 10.0f;       // NoteOffMode::Fade length (OpenTracker choice; spec 03 §6 "fast fixed fade")
constexpr float kNoEnvReleaseMs = 1.0f; // release when the volume envelope is disabled (OpenTracker choice)

inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Per-block ramps of the output stage (sends, AMP, pan).
struct OutRamps {
    float s0, di, s1, ri, a0, ai, l0, li, r0, rri;
};

inline float clampFast(float v);

// Fused output stage (Voice::render fast path): x = player * envelope,
// sends (kD/kR), AMP clamp, pan, o.amp (+ o.l/o.r when kLR), dry-bus sum.
template <bool kD, bool kR, bool kLR>
void fusedOut(const float* __restrict p, const float* __restrict eg, OutRamps g, float* __restrict ds,
              float* __restrict rs, VoiceOut& o, float* __restrict mL, float* __restrict mR) {
    float* __restrict oa = o.amp;
    float* __restrict ol = o.l;
    float* __restrict orr = o.r;
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float x = p[i] * eg[i];
        if (kD) {
            g.s0 += g.di;
            ds[i] += x * g.s0;
        }
        if (kR) {
            g.s1 += g.ri;
            rs[i] += x * g.s1;
        }
        g.a0 += g.ai;
        g.l0 += g.li;
        g.r0 += g.rri;
        const float a = clampFast(x * g.a0);
        oa[i] = a;
        const float l = a * g.l0, r = a * g.r0;
        if (kLR) {
            ol[i] = l;
            orr[i] = r;
        }
        mL[i] += l;
        mR[i] += r;
    }
}

// Fused chain (Voice::render, OP-3): player * envelope (* headroom), bit
// crusher, overdrive, the SVF (kFilt = type + 1, 0 = off), sends, AMP clamp,
// pan and the dry-bus sum in one pass. Every stage does the same operations
// in the same order as the separate passes of the general path (and the SVF
// as Svf::run), so the result is bit-identical; only o.l / o.r are not
// written (keepLR = false, like fusedOut). kExtra: headroom, crusher or
// overdrive on (else those checks are compiled out). kSends: both sends are
// fed; an inactive one goes to a scratch bus (the real bus is untouched).
struct ChainCfg {
    float hs;          // headroom gain (1 = none)
    int32_t mask;      // crusher AND mask (sign-extended)
    float od;          // overdrive pre-gain (0 = off)
    float fmT, dpT;    // SVF coefficient targets
};
template <int kFilt, bool kSends, bool kExtra>
void fusedChain(const float* __restrict p, const float* __restrict eg, OutRamps g, const ChainCfg& cf, Svf& svf,
                float* __restrict ds, float* __restrict rs, float* __restrict oa, float* __restrict mL,
                float* __restrict mR, float& last) {
    const bool hsOn = kExtra && cf.hs != 1.0f, crush = kExtra && cf.mask != -1, odOn = kExtra && cf.od > 0.0f;
    const float hs = cf.hs, odg = cf.od;
    const int32_t m = cf.mask, half = (((~m) & 0xFFFF) + 1) >> 1;   // mid-tread rounding
    float dfm = 0.0f, ddp = 0.0f, f = 0.0f, d = 0.0f, l = 0.0f, b = 0.0f, ip = 0.0f;
    if (kFilt) {
        dfm = (cf.fmT - svf.fm) / float(kBlock);
        ddp = (cf.dpT - svf.dp) / float(kBlock);
        f = svf.fm;
        d = svf.dp;
        l = svf.lp;
        b = svf.bp;
        ip = svf.inPrev;
    }
    float x = 0.0f;
    for (uint32_t i = 0; i < kBlock; ++i) {
        x = hsOn ? p[i] * (eg[i] * hs) : p[i] * eg[i];
        if (crush) x = float((int32_t(toS16(x)) + half) & m) * (1.0f / 32768.0f);
        if (odOn) {
            const float y = x * odg;
            const float a = y < 0.0f ? -y : y;
            float v;
            if (a < 0.25f) v = 3.0f * a;
            else if (a < 0.5f) {
                const float t = 2.0f - 4.0f * a;
                v = 1.0f - t * t * 0.25f;
            } else v = 1.0f;
            x = y < 0.0f ? -v : v;
        }
        if (kFilt) {
            f += dfm;
            d += ddp;
            const float in = x;
            l += f * b;
            float h = (in + ip) * 0.5f - l - d * b;
            ip = in;
            b += f * h;
            const float l1 = l, b1 = b, h1 = h;
            l += f * b;
            h = in - l - d * b;
            b += f * h;
            const float y = kFilt == 1 ? (l + l1) : (kFilt == 2 ? (h + h1) : (b + b1));
            x = clampUnitFast(y * 0.5f);
        }
        if (kSends) {
            g.s0 += g.di;
            ds[i] += x * g.s0;
            g.s1 += g.ri;
            rs[i] += x * g.s1;
        }
        g.a0 += g.ai;
        g.l0 += g.li;
        g.r0 += g.rri;
        const float a = clampFast(x * g.a0);
        oa[i] = a;
        mL[i] += a * g.l0;
        mR[i] += a * g.r0;
    }
    last = x;
    if (kFilt) {
        svf.lp = l > 16.0f ? 16.0f : (l < -16.0f ? -16.0f : l);
        svf.bp = b > 16.0f ? 16.0f : (b < -16.0f ? -16.0f : b);
        svf.inPrev = ip;
        svf.fm = cf.fmT;
        svf.dp = cf.dpT;
    }
}
typedef void (*FusedChainFn)(const float*, const float*, OutRamps, const ChainCfg&, Svf&, float*, float*, float*,
                             float*, float*, float&);
// [kFilt][kSends * 2 + kExtra]; [0][0] (no filter, no extras) is fusedOut's case.
const FusedChainFn kChain[4][4] = {
    {nullptr, &fusedChain<0, false, true>, &fusedChain<0, true, false>, &fusedChain<0, true, true>},
    {&fusedChain<1, false, false>, &fusedChain<1, false, true>, &fusedChain<1, true, false>, &fusedChain<1, true, true>},
    {&fusedChain<2, false, false>, &fusedChain<2, false, true>, &fusedChain<2, true, false>, &fusedChain<2, true, true>},
    {&fusedChain<3, false, false>, &fusedChain<3, false, true>, &fusedChain<3, true, false>, &fusedChain<3, true, true>},
};

// clampUnit() with the common in-range case decided on the bit pattern
// (|v| <= 1 <=> (bits & 0x7FFFFFFF) <= 0x3F800000; NaN goes to clampUnit).
inline float clampFast(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    return (u & 0x7FFFFFFFu) <= 0x3F800000u ? v : clampUnit(v);
}

// frames = uint(length * x / 65535) (spec 04 §2.3).
inline int32_t pointToFrames(uint32_t len, uint32_t x) { return int32_t(uint64_t(len) * x / 65535u); }

// Play modes that play a stereo instrument in stereo in 2.0.0 (2x-04 §3.1;
// playMode > 7 plays as one-shot). TODO(2.1): wavetable and granular.
inline bool stereoPlayMode(uint8_t m) { return m > 7 || m <= uint8_t(PlayMode::BeatSlice); }

// Linear interpolation with the 16-bit fraction of spec 04 §2.2.
inline int32_t interp(int32_t s0, int32_t s1, uint32_t frac) {
    return int16_t(s0 + (((s1 - s0) * int32_t(frac >> 2)) >> 14));
}

}  // namespace

void Voice::reset() {
    const uint32_t seed = seed_;
    const PerfOffsets perf = perf_;
    *this = Voice();
    seed_ = seed;
    perf_ = perf;
}

void Voice::setPerf(const PerfOffsets& p) {
    const bool flip = playing_ && p.reverse != perf_.reverse && mode_ != uint8_t(PlayMode::Wavetable);
    perf_ = p;
    if (flip) {   // live direction change, as the reverse step FX (spec 04 §2.1 +0xB0)
        reverse_ = !reverse_;
        dir_ = int8_t(-dir_);
    }
}

int Voice::lfoSpeedFor(int slot, int base) const {
    const int n = slot == kAutoVolume ? kRateVCount : kRateCount;
    return clampi(base + perf_.lfoSpeed[slot], 0, n - 1);
}

uint32_t Voice::rnd(uint32_t n) {
    seed_ = seed_ * 1664525u + 1013904223u;
    return n ? (seed_ >> 8) % n : 0;
}

void Voice::stop() {
    playing_ = false;
    if (ring_) ring_->src = nullptr;   // the stream service leaves the ring alone
    env_.stop();
    pending_.valid = false;
    stBlock_ = stereo_;
    for (auto& m : mods_) m.st = 0;
}

// --- FX slots (spec 03 §6 voiceApplyFx / §3.6) ------------------------------

void Voice::applyOne(uint8_t type, uint8_t v) {
    switch (type) {
        case kFxVolume: ov_.volume = v > 100 ? 100 : v; break;
        case kFxGlide: glideFx_ = v; break;
        case kFxPosition: ov_.position = v > 100 ? 100 : v; break;
        case kFxVolumeLfo: ov_.lfoSpeed[kAutoVolume] = int8_t(v < kRateVCount ? v : kRateVCount - 1); break;
        case kFxPanLfo: ov_.lfoSpeed[kAutoPanning] = int8_t(v < kRateCount ? v : kRateCount - 1); break;
        case kFxFilterLfo: ov_.lfoSpeed[kAutoCutoff] = int8_t(v < kRateCount ? v : kRateCount - 1); break;
        case kFxPositionLfo:
            ov_.lfoSpeed[kAutoWavetablePos] = ov_.lfoSpeed[kAutoGranularPos] =
                int8_t(v < kRateCount ? v : kRateCount - 1);
            break;
        case kFxFinetuneLfo: ov_.lfoSpeed[kAutoFinetune] = int8_t(v < kRateCount ? v : kRateCount - 1); break;
        case kFxSlice: sliceSel_ = v; break;
        case kFxReverse: reverseFx_ = v != 0; break;
        case kFxLowPass:
        case kFxHighPass:
        case kFxBandPass:
            ov_.ftype = int8_t(type - kFxLowPass);
            ov_.cutoff = float(v > 100 ? 100 : v) / 100.0f;
            break;
        case kFxDelaySend: ov_.dsend = v > 100 ? 100 : v; break;
        case kFxReverbSend: ov_.rsend = v > 100 ? 100 : v; break;
        case kFxPanning: ov_.pan = v > 100 ? 100 : v; break;
        case kFxMicroTune: ov_.fine = int16_t(clampi(int32_t(v) - 99, -100, 100)); break;
        case kFxOverdrive: ov_.od = v > 100 ? 100 : v; break;
        case kFxBitDepth: ov_.bits = int16_t(clampi(v, 4, 16)); break;
        case kFxTune: ov_.tune = int16_t(clampi(int32_t(v) - 24, -24, 24)); break;
        case kFxSlideUp:
        case kFxSlideDown:
            ov_.slideDir = type == kFxSlideUp ? 1 : -1;
            ov_.slideVal = v;
            break;
        default: break;
    }
}

void Voice::rebuildOverrides() {
    ov_ = Overrides{};
    sliceSel_ = 0xFF;
    glideFx_ = 0xFF;
    const bool prevRev = reverseFx_;
    reverseFx_ = false;
    if (noteVolume_ >= 0) ov_.volume = noteVolume_;
    for (int s = 0; s < 2; ++s) applyOne(slotType_[s], slotValue_[s]);
    // Live reverse toggle flips the running direction (spec 04 §2.1 +0xB0).
    if (playing_ && prevRev != reverseFx_ && reverse_ != reverseFx_ && mode_ != uint8_t(PlayMode::Wavetable)) {
        reverse_ = reverseFx_;
        dir_ = int8_t(-dir_);
    }
}

void Voice::applyFx(uint8_t slot, uint8_t type, uint8_t value) {
    if (slot > 1) return;
    if (type == kFxRandomVolume) {
        // Random volume on an empty step: draw 0..v once (spec 03 §3.6 id 7).
        type = kFxVolume;
        value = uint8_t(rnd(uint32_t(value > 100 ? 100 : value) + 1u));
    }
    slotType_[slot] = type <= kFxOff ? 0 : type;   // 0/1 = undo only
    slotValue_[slot] = value;
    if (type == kFxSlideUp || type == kFxSlideDown) {
        // Started at the next control block (spec 16 §A.10).
        slidePending_ = true;
        slideDirNew_ = type == kFxSlideUp ? 1 : -1;
        slideValNew_ = value;
    }
    // An LFO FX re-latches S_on of its slot(s) (spec 07 Q5).
    switch (type) {
        case kFxPanLfo: mods_[kAutoPanning].stepOn = curStep0_; break;
        case kFxFilterLfo: mods_[kAutoCutoff].stepOn = curStep0_; break;
        case kFxPositionLfo: mods_[kAutoWavetablePos].stepOn = mods_[kAutoGranularPos].stepOn = curStep0_; break;
        case kFxFinetuneLfo: mods_[kAutoFinetune].stepOn = curStep0_; break;
        default: break;
    }
    const int8_t prevVolLfo = ov_.lfoSpeed[kAutoVolume];
    rebuildOverrides();
    // Volume LFO FX on a sounding voice rebuilds the looping envelope (§4.8 slot 0).
    if (ins_ && ov_.lfoSpeed[kAutoVolume] != prevVolLfo && (playing_ || env_.active())) setupVolumeEnvelope(tempo_);
}

void Voice::resetFx() {
    slotType_[0] = slotType_[1] = 0;
    slotValue_[0] = slotValue_[1] = 0;
    noteVolume_ = -1;
    slideSemis_ = 0.0f;
    slideActive_ = slidePending_ = false;
    rebuildOverrides();
}

// --- note on / off ----------------------------------------------------------

void Voice::noteOn(const Instrument* ins, const int16_t* data, uint32_t frames, uint8_t note, int8_t volume,
                   uint8_t aT, uint8_t aV, uint8_t bT, uint8_t bV, const int16_t* dataR,
                   const StreamSource* stream, StreamRing* ring) {
    if (!ins || !data || frames == 0) return;
    noteVolume_ = volume;
    for (auto& m : mods_) m.stepOn = curStep0_;   // S_on latch at note-on (spec 07 Q5)
    slotType_[0] = slotValue_[0] = slotType_[1] = slotValue_[1] = 0;
    rebuildOverrides();
    applyFx(0, aT, aV);
    applyFx(1, bT, bV);
    pending_.ins = ins;
    pending_.data = data;
    pending_.dataR = dataR;
    pending_.stream = stream;
    pending_.ring = ring;
    pending_.frames = frames;
    pending_.note = note;
    pending_.valid = true;
    stBlock_ = stereo_ || (dataR && stereoPlayMode(ins->playMode));
    ins_ = ins;   // envelope / modulator parameters come from the new instrument
    setupVolumeEnvelope(tempo_);
    if (!env_.noteOn()) startNote();
}

void Voice::noteOff(int8_t mode) {
    if (mode == -3) {   // Cut
        stop();
        return;
    }
    if (mode == -2) env_.releaseWith(envMsToCount(kFadeMs));
    else env_.noteOff();
    for (auto& m : mods_) {
        if (m.st != 0 && m.st != 6) {
            m.relStart = m.out;
            m.st = 6;
            m.t = 0.0f;
        }
    }
    pending_.valid = false;
    stBlock_ = stereo_;
}

void Voice::startNote() {
    if (!pending_.valid) return;
    const bool wasActive = playing_;
    ins_ = pending_.ins;
    data_ = pending_.data;
    dataR_ = pending_.dataR;
    frames_ = pending_.frames;
    stream_ = pending_.stream;
    if (pending_.ring) ring_ = pending_.ring;
    pending_.valid = false;
    mode_ = ins_->playMode > 7 ? 0 : ins_->playMode;
    stereo_ = dataR_ != nullptr && stereoPlayMode(mode_);
    stBlock_ = stereo_;
    note_ = mode_ == uint8_t(PlayMode::BeatSlice) ? uint8_t(kUnityNote) : pending_.note;   // §2.3
    sliceNote_ = pending_.note;   // Beat Slice: the note picks the slice (computePoints)
    reverse_ = reverseFx_ != perf_.reverse;
    slideSemis_ = 0.0f;
    frac_ = 0;
    // Modulators restart with the note (spec 03 §6 "resets voice modulators").
    for (int s = 0; s < kAutomationCount; ++s) {
        mods_[s].st = 1;
        mods_[s].t = 0.0f;
        mods_[s].out = 0.0f;
        mods_[s].lastCycle = 0xFFFFFFFFu;
        mods_[s].cyc = 0;
    }
    grainPos_ = ins_->granularPosition;
    if (ov_.position >= 0) grainPos_ = uint16_t(uint32_t(ov_.position) * 65535u / 100u);
    computePoints();
    playing_ = true;
    if (ring_) {
        if (stream_) {   // a new stream: position first, the generation last
            ring_->src = stream_;
            ring_->need = uint32_t(pos_ > 0 ? pos_ : 0);
            streamFadeReset(*ring_);
            ring_->gen = ++streamGen_;
        } else {
            ring_->src = nullptr;
        }
    }
    // Pitch, glide (spec 04 §2.4).
    const float prevRate = targetRate_ + glideOff_;
    glideOff_ = 0.0f;
    glideLeft_ = 0;
    targetRate_ = computeRate();
    const uint32_t gms = glideFx_ != 0xFF ? glideFxMs(glideFx_) : 0u;
    if (gms > 0 && hadNote_) {
        glideLeft_ = glideSamples(gms);
        if (glideLeft_ > 0) {
            glideOff_ = prevRate - targetRate_;
            glideInc_ = -glideOff_ / float(glideLeft_);
        }
    }
    hadNote_ = true;
    if (!wasActive) firstBlock_ = true;
}

void Voice::computePoints() {
    const uint32_t len = frames_;
    start_ = pointToFrames(len, uint32_t(clampi(int32_t(ins_->startPoint) + perf_.start, 0, 65535)));
    end_ = pointToFrames(len, uint32_t(clampi(int32_t(ins_->endPoint) + perf_.end, 0, 65535)));
    loop1_ = pointToFrames(len, ins_->loopPoint1);
    loop2_ = pointToFrames(len, ins_->loopPoint2);
    if (gSnapToZero && !stream_) {   // Snap to zero hook: playback only, markers unchanged (spec 13 §H.2)
        start_ = snapToZero(data_, len, start_);
        loop1_ = snapToZero(data_, len, loop1_);
        loop2_ = snapToZero(data_, len, loop2_);
    }
    if (ov_.position >= 0 && mode_ <= uint8_t(PlayMode::PingPongLoop))
        start_ = pointToFrames(len, uint32_t(ov_.position) * 65535u / 100u);
    if (end_ > int32_t(len)) end_ = int32_t(len);
    if (start_ >= end_) {
        start_ = 0;
        end_ = int32_t(len);
    }
    // Loop points must satisfy start <= loop1 <= loop2 <= end (spec 04 §2.3).
    if (loop2_ > end_) loop2_ = end_;
    if (!(start_ <= loop1_ && loop1_ < loop2_ && loop2_ <= end_)) {
        loop1_ = start_;
        loop2_ = end_;
    }
    if (mode_ == uint8_t(PlayMode::Slice) || mode_ == uint8_t(PlayMode::BeatSlice)) {
        const int count = ins_->sliceCount > kSliceCount ? kSliceCount : ins_->sliceCount;
        if (count > 0) {
            int idx;
            if (sliceSel_ != 0xFF) idx = sliceSel_ % count;   // Slice FX wins and wraps (spec 16 §A.11)
            else if (mode_ == uint8_t(PlayMode::BeatSlice))      // note n plays slice n - 24 (spec 05 §4.1, 06 §1.6)
                idx = clampi(int32_t(sliceNote_) - 24, 0, count - 1);
            else idx = ins_->selectedSlice % count;            // Slice: the selected slice, pitched by the note
            start_ = pointToFrames(len, ins_->slices[idx]);
            // Slice end = next slice start, else the end point (spec 04 §2.3, I).
            int32_t e = idx + 1 < count ? pointToFrames(len, ins_->slices[idx + 1]) : pointToFrames(len, ins_->endPoint);
            if (e <= start_ || e > int32_t(len)) e = int32_t(len);
            end_ = e;
            if (start_ >= end_) start_ = end_ - 1;
        }
    }
    dir_ = reverse_ ? -1 : 1;
    pos_ = reverse_ ? end_ - 1 : start_;
    if (mode_ == uint8_t(PlayMode::Wavetable)) {
        wtSize_ = 32u << wavetableSizeIndex(ins_->wavetableWindowSize);
        if (wtSize_ > len) wtSize_ = 32;
        dir_ = 1;
        pos_ = 0;
        wtOffset_ = 0;   // window chosen in control()
    } else if (mode_ == uint8_t(PlayMode::Granular)) {
        computeGrain();
        grainDirty_ = false;
        int d = ins_->granularLoop == uint8_t(GranularLoop::Backward) ? -1 : 1;
        if (reverse_) d = -d;
        dir_ = int8_t(d);
        pos_ = d > 0 ? gStart_ : gEnd_ - 1;
    }
}

// Granular grain bounds (spec 04 §3.1).
void Voice::computeGrain() {
    const uint32_t len = frames_;
    uint32_t L = ins_->grainLength;
    if (L < 44) L = 44;
    if (L > 44100) L = 44100;
    const int32_t half = int32_t((uint64_t(L) * 65535u / len) % 65536u) / 2;
    int32_t s = int32_t(grainPos_) - half;
    int32_t e = int32_t(grainPos_) + half;
    if (s < 0) {
        e += -s;
        s = 0;
    }
    if (e > 65535) {
        s -= e - 65535;
        e = 65535;
    }
    s = clampi(s, 0, 65535);
    e = clampi(e, 0, 65535);
    gStart_ = pointToFrames(len, uint32_t(s));
    gEnd_ = pointToFrames(len, uint32_t(e));
    if (gStart_ >= int32_t(len)) gStart_ = int32_t(len) - 1;
    if (gEnd_ <= gStart_) gEnd_ = gStart_ + 1;
    if (gEnd_ > int32_t(len)) gEnd_ = int32_t(len);
}

float Voice::computeRate() const {
    // spec 04 §2.4: idx = note + tune (+ transpose byte), offset reduced into 0..119.
    const int tune = ov_.tune != -128 ? ov_.tune : ins_->tune;
    int idx = int(note_) + tune + perf_.tune + int8_t(ins_->reserved173);
    idx = clampi(idx, 0, kPitchTableSize - 1);
    const float* t = mode_ == uint8_t(PlayMode::Wavetable) ? wavetablePitchTable(ins_->wavetableWindowSize)
                                                            : pitchTable();
    float rate = t[idx];
    // Finetune: linear between adjacent semitones; modulation adds mod*100 cents, clamp +-100.
    float c = float(ov_.fine != -128 ? ov_.fine : ins_->finetune) + mods_[kAutoFinetune].out * 100.0f;
    c = clampf(c, -100.0f, 100.0f);
    if (c >= 0.0f) {
        if (idx < kPitchTableSize - 1) rate += (t[idx + 1] - t[idx]) * c / 100.0f;
    } else {
        if (idx > 0) rate += (t[idx - 1] - t[idx]) * (-c) / 100.0f;
    }
    if (slideSemis_ != 0.0f) rate *= std::exp2(slideSemis_ / 12.0f);
    return clampf(rate, t[0], t[kPitchTableSize - 1]);
}

// --- volume envelope / LFO (spec 04 §4.7, §4.8 slot 0) -----------------------

void Voice::setupVolumeEnvelope(float tempo) {
    if (!ins_) return;
    const Envelope& e = ins_->env[kAutoVolume];
    const bool lfo = (e.enabled && e.loop) || ov_.lfoSpeed[kAutoVolume] >= 0;
    if (lfo) {
        const int speed = lfoSpeedFor(kAutoVolume, ov_.lfoSpeed[kAutoVolume] >= 0 ? ov_.lfoSpeed[kAutoVolume] : ins_->lfo[kAutoVolume].speed);
        if (!(tempo > 0.0f)) tempo = 130.0f;
        const float p = volLfoRateSteps(speed) * 15000.0f / tempo;
        volLfoPeriodMs_ = p;
        const uint8_t shape = ins_->lfo[kAutoVolume].shape;
        env_.setNoAttack(false);
        env_.setBypass(false);
        switch (shape) {
            case 0: env_.setTimes(0, 0, 0, p, 0.0f, 0); break;                    // rev-saw: decay = period
            case 1: env_.setTimes(0, p, 0, 0, 0.0f, 0); break;                    // saw: attack = period
            case 2: env_.setTimes(0, p * 0.5f, 0, p * 0.5f, 0.0f, 0); break;      // triangle
            case 3: env_.setTimes(0, 0, p * 0.5f, 0, 0.0f, p * 0.5f); break;      // square: hold = release = P/2
            default: env_.setTimes(0, 0, p, 0, 1.0f, 0); break;                   // random: hold = period
        }
        env_.setRandomSource(&seed_);
        env_.setLoop(true, shape >= 4);
        eAmt_ = clampf(ins_->lfo[kAutoVolume].amount, 0.0f, 1.0f);
    } else if (e.enabled) {
        volLfoPeriodMs_ = -1.0f;
        env_.setNoAttack(false);
        env_.setBypass(false);
        env_.setLoop(false);
        env_.setTimes(e.delayMs, e.attackMs, e.holdMs, e.decayMs, e.sustain, e.releaseMs);
        eAmt_ = clampf(e.amount, 0.0f, 1.0f);
    } else {
        volLfoPeriodMs_ = -1.0f;
        env_.setNoAttack(true);
        env_.setBypass(false);
        env_.setLoop(false);
        env_.setTimes(0, 0, 0, 0, 1.0f, kNoEnvReleaseMs);
        eAmt_ = 1.0f;
    }
}

// --- automation slots 1..5 (spec 04 §4.8, spec 07 Q5) -------------------------

// LFO of slot s. Internal level v in [0, A] follows the shape's envelope
// segments (period T = RATE[speed] steps):
//   0 decay = T (ramp down), 1 attack = T (ramp up), 2 attack/decay T/2
//   (triangle), 3 hold/release T/2 (square; release forced to 0),
//   4 hold = T with a random level random_int(0, floor(A*1000))/1000.
// The output is bipolar: out = 2*clamp(v, -1, 1) - A, i.e. [-A, +A].
// Sequencer playing: phase locked to pos = tick + S_on*576, mod RATE*576.
// Stopped: free-running from the note-on on the µs clock (T = RATE*15e6/BPM
// µs); note-off returns it to idle (output 0).
void Voice::updateLfo(int s, const VoiceControl& c, int speed) {
    Mod& m = mods_[s];
    const float A = ins_->lfo[s].amount;
    const float rate = lfoRateSteps(speed);
    float ph;
    uint32_t cycle;
    if (c.seqPlaying) {
        uint32_t P = uint32_t(rate * float(kLfoUnitsPerStep) + 0.5f);
        if (P == 0) P = 1;
        const uint32_t pos = c.seqTick + uint32_t(int32_t(m.stepOn) * int32_t(kLfoUnitsPerStep));
        cycle = pos / P;
        ph = float(pos % P) / float(P);
    } else {
        if (m.st == 0 || m.st == 6) {   // idle after note-off
            m.out = 0.0f;
            return;
        }
        const float T = rate * 15.0e6f / (tempo_ > 0.0f ? tempo_ : 130.0f);
        while (m.t >= T) {
            m.t -= T;
            ++m.cyc;
        }
        cycle = m.cyc;
        ph = m.t / T;
        m.t += c.blockUs;
    }
    float v;
    switch (ins_->lfo[s].shape) {
        case 0: v = A * (1.0f - ph); break;                                  // decay
        case 1: v = A * ph; break;                                           // attack
        case 2: v = ph < 0.5f ? A * 2.0f * ph : A * (2.0f - 2.0f * ph); break; // attack, decay
        case 3: v = ph < 0.5f ? A : 0.0f; break;                             // hold, release
        default:                                                             // sample & hold
            if (cycle != m.lastCycle) {
                m.lastCycle = cycle;
                const int32_t n = A > 0.0f ? int32_t(A * 1000.0f) : 0;
                m.rnd = float(rnd(uint32_t(n) + 1u)) / 1000.0f;
            }
            v = m.rnd;
            break;
    }
    m.out = 2.0f * clampf(v, -1.0f, 1.0f) - A;
}

void Voice::updateMod(int s, const VoiceControl& c) {
    Mod& m = mods_[s];
    const Envelope& e = ins_->env[s];
    const Lfo& l = ins_->lfo[s];
    const bool lfoOn = ov_.lfoSpeed[s] >= 0 || (e.enabled && e.loop);
    if (lfoOn) {
        updateLfo(s, c, lfoSpeedFor(s, ov_.lfoSpeed[s] >= 0 ? ov_.lfoSpeed[s] : l.speed));
        return;
    }
    // Envelope mode is unipolar: 0..amount (spec 07 Q5).
    if (!e.enabled || m.st == 0) {
        m.out = 0.0f;
        return;
    }
    // Envelope in µs (instrument ms x 1000): delay -> attack -> hold -> decay -> sustain -> release.
    const float amt = e.amount;
    const float D = float(e.delayMs) * 1000.0f, A = float(e.attackMs) * 1000.0f, H = float(e.holdMs) * 1000.0f,
                Dc = float(e.decayMs) * 1000.0f, R = float(e.releaseMs) * 1000.0f;
    m.t += c.blockUs;
    for (int guard = 0; guard < 8; ++guard) {
        if (m.st == 1 && m.t >= D) { m.t -= D; m.st = 2; continue; }
        if (m.st == 2 && m.t >= A) { m.t -= A; m.st = 3; continue; }
        if (m.st == 3 && m.t >= H) { m.t -= H; m.st = 4; continue; }
        if (m.st == 4 && m.t >= Dc) { m.t = 0.0f; m.st = 5; continue; }
        if (m.st == 6 && m.t >= R) { m.st = 0; m.out = 0.0f; return; }
        break;
    }
    float v = 0.0f;
    switch (m.st) {
        case 1: v = 0.0f; break;
        case 2: v = A > 0.0f ? amt * m.t / A : amt; break;
        case 3: v = amt; break;
        case 4: v = amt + (e.sustain * amt - amt) * (Dc > 0.0f ? m.t / Dc : 1.0f); break;
        case 5: v = e.sustain * amt; break;
        case 6: v = R > 0.0f ? m.relStart * (1.0f - m.t / R) : 0.0f; break;
        default: break;
    }
    m.out = clampUnit(v);
}

// --- per-block control (spec 04 §4.9 / FW:0xCD70C behaviour) -----------------

void Voice::control(const VoiceControl& c) {
    tempo_ = c.tempoBpm > 0.0f ? c.tempoBpm : 130.0f;
    aa_ = c.antiAliasing;
    // Silent voice: nothing below is observable until the next note, and
    // startNote() of a silent voice sets firstBlock_ (all ramps / filter
    // coefficients jump to the values computed then). CPU saving.
    if (!ins_ || (!playing_ && !env_.active())) return;
    // Volume LFO period follows the tempo (spec 04 §4.8 slot 0).
    if (volLfoPeriodMs_ > 0.0f) {
        const int speed = lfoSpeedFor(kAutoVolume, ov_.lfoSpeed[0] >= 0 ? ov_.lfoSpeed[0] : ins_->lfo[0].speed);
        const float p = volLfoRateSteps(speed) * 15000.0f / tempo_;
        if (std::fabs(p - volLfoPeriodMs_) > volLfoPeriodMs_ * 0.002f) setupVolumeEnvelope(tempo_);
    }
    if (playing_ || env_.active()) {
        for (int s = 1; s < kAutomationCount; ++s) updateMod(s, c);
        // Slide (spec 16 §A.10): a fixed total of value/16 semitones, reached
        // with a linear playback-rate ramp over exactly one step, then held;
        // value 0 stops the slide where it is.
        if (slidePending_) {
            slidePending_ = false;
            slideActive_ = slideValNew_ != 0;
            slideFrom_ = slideSemis_;
            slideRatio_ = std::exp2(float(slideDirNew_) * float(slideValNew_) / 192.0f) - 1.0f;
            slideProg_ = 0.0f;
        }
        if (slideActive_) {
            slideProg_ += c.blockUs * 1.0e-6f * tempo_ / 15.0f;   // steps
            const float f = slideProg_ >= 1.0f ? 1.0f : slideProg_;
            slideSemis_ = clampf(slideFrom_ + 12.0f * std::log2(1.0f + slideRatio_ * f), -120.0f, 120.0f);
            if (slideProg_ >= 1.0f) slideActive_ = false;
        }
    }
    targetRate_ = computeRate();

    // Filter (spec 04 §4.4).
    bool fOn = ov_.ftype >= 0 || ins_->filterEnabled != 0;
    uint8_t fType = ov_.ftype >= 0 ? uint8_t(ov_.ftype) : (ins_->filterType > 2 ? 0 : ins_->filterType);
    float baseCut = ov_.cutoff >= 0.0f ? ov_.cutoff : ins_->cutoff;
    if (perf_.filterType >= 0) {
        // Performance filter FX: force the type; when the instrument filter
        // was off or of another type, offsets start from the open position
        // (LP 1.0, HP 0.0, BP 0.5) -- OpenTracker choice (SPEC GAP).
        if (!fOn || fType != uint8_t(perf_.filterType)) baseCut = perf_.filterType == 0 ? 1.0f : (perf_.filterType == 1 ? 0.0f : 0.5f);
        fOn = true;
        fType = uint8_t(perf_.filterType);
        baseCut = clampf(baseCut + perf_.cutoff, 0.0f, 1.0f);
    }
    if (!firstBlock_ && (fOn != filtOn_ || (fOn && fType != filtType_))) filtXfade_ = true;
    filtOn_ = fOn;
    filtType_ = fType;
    const float cut = baseCut + mods_[kAutoCutoff].out;
    fmTarget_ = svfFmult(cutoffHz(cut));
    dpTarget_ = svfDamp(ins_->resonance);
    // Overdrive, bit depth (§4.5, §4.6).
    odGain_ = overdrivePreGain(clampi((ov_.od >= 0 ? ov_.od : ins_->overdrive) + perf_.od, 0, 100));
    mask_ = bitMask(clampi((ov_.bits >= 0 ? ov_.bits : ins_->bitDepth) + perf_.bits, 4, 16));

    // Wavetable window / granular position (spec 04 §3.2, §3.3).
    if (mode_ == uint8_t(PlayMode::Wavetable) && frames_ >= wtSize_) {
        const int32_t count = int32_t(ins_->wavetableWindowCount);
        float base = ov_.position >= 0 ? float(ov_.position) * float(count) / 100.0f
                                       : float(ins_->wavetableCurrentWindow);
        base += mods_[kAutoWavetablePos].out * float(count);
        int32_t idx = clampi(int32_t(base), 0, count > 0 ? count - 1 : 0);
        const int32_t last = int32_t(frames_ / wtSize_) - 1;
        if (idx > last) idx = last;
        wtOffset_ = uint32_t(idx) * wtSize_;
    } else if (mode_ == uint8_t(PlayMode::Granular)) {
        float p = ov_.position >= 0 ? float(ov_.position) * 65535.0f / 100.0f : float(ins_->granularPosition);
        p += mods_[kAutoGranularPos].out * 65535.0f;
        const uint16_t gp = uint16_t(clampi(int32_t(p), 0, 65535));
        if (gp != grainPos_) {
            grainPos_ = gp;
            grainDirty_ = true;
        }
    }

    // Gains (spec 04 §4.2, §4.3, §5.5).
    const int vol = clampi((ov_.volume >= 0 ? ov_.volume : ins_->volume) + perf_.volume, 0, 100);
    const float vg = volGain(vol);
    // AMP (spec 07 Q4): VOL * E_amt * TRK[trackLevel] * TRK[Dry Mix].
    const float g = c.muted ? 0.0f : vg * eAmt_ * c.trackGain * c.dryGain;
    float pl, pr;
    const float panv = float(clampi((ov_.pan >= 0 ? ov_.pan : ins_->panning) + perf_.pan, 0, 100)) +
                       mods_[kAutoPanning].out * 100.0f;
    panGains(int(panv + (panv >= 0.0f ? 0.5f : -0.5f)), pl, pr);
    // Sends (spec 07 Q3): SEND[s] * TRK[Global Mixer level] (0 when FX-muted);
    // post-fader also * E_amt * VOL[v] * TRK[trackLevel], 0 for a muted track.
    // Dry Mix is not part of the send gain.
    float gd = sendGain(clampi((ov_.dsend >= 0 ? ov_.dsend : ins_->delaySend) + perf_.dsend, 0, 100)) * c.delayLevel;
    float gr = sendGain(clampi((ov_.rsend >= 0 ? ov_.rsend : ins_->reverbSend) + perf_.rsend, 0, 100)) * c.reverbLevel;
    if (c.postFaderSends) {
        const float post = c.muted ? 0.0f : eAmt_ * vg * c.trackGain;
        gd *= post;
        gr *= post;
    }
    if (firstBlock_) {
        amp_.jump(g);
        panL_.jump(pl);
        panR_.jump(pr);
        sendD_.jump(gd);
        sendR_.jump(gr);
        svf_.fm = fmTarget_;
        svf_.dp = dpTarget_;
        // Stereo note: the right filter starts at the same coefficients (it
        // would otherwise ramp from the previous note's, 2x-04 §3).
        svfR_.fm = fmTarget_;
        svfR_.dp = dpTarget_;
        firstBlock_ = false;
    } else {
        amp_.set(g);
        panL_.set(pl);
        panR_.set(pr);
        sendD_.set(gd);
        sendR_.set(gr);
    }
}

// --- player (spec 04 §2.2, §3) -----------------------------------------------

OT_FASTRUN_FN void Voice::renderPlayer(float* out, uint32_t n) {
    if (!playing_ || !data_ || frames_ == 0) {
        zeroFloats(out, n);
        return;
    }
    const int16_t* d = data_;
    const int32_t last = int32_t(frames_) - 1;
    const float* tab = mode_ == uint8_t(PlayMode::Wavetable) ? wavetablePitchTable(ins_->wavetableWindowSize)
                                                             : pitchTable();
    const float rmin = tab[0], rmax = tab[kPitchTableSize - 1];
    float rate = clampf(targetRate_ + glideOff_, rmin, rmax);
    uint32_t si = uint32_t(rate);
    uint32_t sf = uint32_t((rate - float(si)) * 65535.0f);
    int32_t pos = pos_;
    uint32_t frac = frac_;
    int32_t dir = dir_;

    auto glide = [&]() {
        if (glideLeft_ == 0) return;
        glideOff_ += glideInc_;
        if (--glideLeft_ == 0) glideOff_ = 0.0f;
        rate = clampf(targetRate_ + glideOff_, rmin, rmax);
        si = uint32_t(rate);
        sf = uint32_t((rate - float(si)) * 65535.0f);
    };
    auto advance = [&]() {
        frac += sf;
        pos += dir * int32_t(si);
        if (frac > 65534u) {
            frac -= 65535u;
            pos += dir;
        }
    };
    auto nb = [&](int32_t p) {
        int32_t q = p + dir;
        return q < 0 ? 0 : (q > last ? last : q);
    };
    // Sample reads go through a small window copied from the sample pool
    // (SDRAM on the device) with 32-bit accesses (ring_io.h), refilled when
    // a read leaves it: ~1 SDRAM word per 2 samples at rate 1 instead of 2
    // halfword accesses per sample. High rates read directly.
    constexpr int32_t kWin = 32;
    int16_t win[kWin];
    uint32_t wb = 0x80000000u;   // window start (frame index); invalid
    // A streamed sample always reads through the window (its frames are not
    // all in memory: streamRead takes them from the head, loop head or ring).
    const StreamSource* const st = stream_;
    const bool useWin = int32_t(frames_) >= kWin && (st || (rate < 4.0f && targetRate_ < 4.0f));
    auto fill = [&](int32_t b) {
        if (b > int32_t(frames_) - kWin) b = int32_t(frames_) - kWin;
        if (b < 0) b = 0;
        wb = uint32_t(b);
        OT_SDRAM_ACCESS(kWin / 2 + 1);
        if (st) streamRead(*st, ring_, streamGen_, b, uint32_t(kWin), win, nullptr);
        else spanIn(d + b, win, uint32_t(kWin));
    };
    auto rd = [&](int32_t idx) -> int32_t {
        if (!useWin) {
            OT_SDRAM_ACCESS(1);
            return d[idx];
        }
        uint32_t off = uint32_t(idx) - wb;
        if (off >= uint32_t(kWin)) {
            fill(dir > 0 ? idx - 1 : idx - (kWin - 2));
            off = uint32_t(idx) - wb;
        }
        return win[off];
    };
    // Forward, interpolated, no glide: tight runs (same arithmetic as the
    // per-sample paths below, which handle every other case and the edges).
    const bool fast = useWin && aa_ && glideLeft_ == 0;
    // Produces samples while pos < lim (pos + 1 is then the neighbour).
    auto fastRun = [&](uint32_t& i, int32_t lim) {
        if (uint32_t(pos) - wb >= uint32_t(kWin) - 1u) fill(pos);   // also pos == wb - 1 (loop wrap)
        // Each step advances pos by at most si + 1: k steps stay below stop.
        const int32_t wend = int32_t(wb) + kWin - 1;
        const int32_t stop = lim < wend ? lim : wend;
        uint32_t k = uint32_t(stop - pos) / (si + 1u);
        if (k == 0) k = 1;   // pos < stop holds
        if (k > n - i) k = n - i;
        i += k;
        int32_t q = pos - int32_t(wb);   // index into win
        uint32_t f = frac;
        float* o = out + (i - k);
        do {
            *o++ = s16ToF(interp(win[q], win[q + 1], f));
            f += sf;
            q += int32_t(si);
            if (f > 65534u) {
                f -= 65535u;
                q += 1;
            }
        } while (--k);
        pos = q + int32_t(wb);
        frac = f;
    };

    const uint8_t mode = mode_;
    if (mode == uint8_t(PlayMode::Wavetable)) {
        // Loop one window of W samples; window changes at block boundaries (§2.2, §3.2).
        const int32_t mask = int32_t(wtSize_) - 1;
        const int16_t* base = d + wtOffset_;
        for (uint32_t i = 0; i < n; ++i) {
            glide();
            OT_SDRAM_ACCESS(aa_ ? 2 : 1);
            const int32_t s0 = base[pos & mask];
            out[i] = s16ToF(aa_ ? interp(s0, base[(pos + 1) & mask], frac) : s0);
            frac += sf;
            pos += int32_t(si);
            if (frac > 65534u) {
                frac -= 65535u;
                pos += 1;
            }
            pos &= mask;
        }
    } else if (mode == uint8_t(PlayMode::Granular)) {
        const uint16_t* win = grainWindow(ins_->grainShape);
        const bool pingpong = ins_->granularLoop == uint8_t(GranularLoop::PingPong);
        for (uint32_t i = 0; i < n; ++i) {
            glide();
            if (pos < gStart_ || pos >= gEnd_) pos = clampi(pos, gStart_, gEnd_ - 1);
            const int32_t s0 = rd(pos);
            const int32_t s = aa_ ? interp(s0, rd(nb(pos)), frac) : s0;
            const uint32_t span = uint32_t(gEnd_ - gStart_);
            const uint32_t wi = uint32_t(pos - gStart_) * 999u / span;
            out[i] = s16ToF((int32_t(win[wi]) * s) >> 16);
            advance();
            if (dir > 0 ? pos >= gEnd_ : pos < gStart_) {
                // Grain wrap: apply pending position/length changes here only (§3.1).
                if (grainDirty_) {
                    computeGrain();
                    grainDirty_ = false;
                }
                if (pingpong) {
                    dir = -dir;
                    pos = dir > 0 ? gStart_ : gEnd_ - 1;
                } else {
                    pos = dir > 0 ? gStart_ : gEnd_ - 1;
                }
            }
        }
    } else if (mode >= uint8_t(PlayMode::ForwardLoop) && mode <= uint8_t(PlayMode::PingPongLoop) &&
               loop2_ > loop1_) {
        const int32_t L1 = loop1_, L2 = loop2_, rl = L2 - L1;
        const int32_t lim = L2 < last ? L2 : last;   // pos < lim: pos + 1 is the neighbour
        for (uint32_t i = 0; i < n; ++i) {
            if (fast && dir > 0) {
                while (i < n && pos >= 0 && pos < lim) {
                    fastRun(i, lim);
                    if (pos >= L2) {
                        const int32_t ex = (pos - L2) % rl;
                        if (mode == uint8_t(PlayMode::ForwardLoop)) {
                            pos = L1 + ex;
                        } else {
                            dir = -1;
                            pos = L2 - 1 - ex;
                            break;
                        }
                    }
                }
                if (i >= n) break;
            }
            glide();
            if (pos < 0 || pos > last) {   // defensive (reverse past the start)
                playing_ = false;
                zeroFloats(out + i, n - i);
                break;
            }
            const int32_t s0 = rd(pos);
            out[i] = s16ToF(aa_ ? interp(s0, rd(nb(pos)), frac) : s0);
            advance();
            if (dir > 0) {
                if (pos >= L2) {
                    const int32_t ex = (pos - L2) % rl;
                    if (mode == uint8_t(PlayMode::ForwardLoop)) {
                        pos = L1 + ex;
                    } else {   // backward / ping-pong: reflect at loop2 (§2.2, I)
                        dir = -1;
                        pos = L2 - 1 - ex;
                    }
                }
            } else if (pos < L1) {
                const int32_t ex = (L1 - 1 - pos) % rl;
                if (mode == uint8_t(PlayMode::PingPongLoop)) {
                    dir = 1;
                    pos = L1 + ex;
                } else {       // forward (reversed) / backward loop: wrap to loop2
                    pos = L2 - 1 - ex;
                }
            }
        }
    } else {
        // One-shot / slice / beat-slice (and degenerate loops), §2.2.
        const int32_t s = start_, e = end_;
        for (uint32_t i = 0; i < n; ++i) {
            if (fast && dir > 0) {
                while (i < n && pos >= 0 && pos < e - 1) fastRun(i, e - 1);   // pos + 1 < e: interpolated
                if (i >= n) break;
            }
            glide();
            if (dir > 0 ? pos >= e : pos < s) {
                playing_ = false;
                zeroFloats(out + i, n - i);
                break;
            }
            const int32_t s0 = rd(pos);
            // Interpolate except at the last sample of the region (drop-sample there).
            const int32_t q = pos + dir;
            const bool ip = aa_ && (dir > 0 ? q < e : q >= s);
            out[i] = s16ToF(ip ? interp(s0, rd(q), frac) : s0);
            advance();
        }
    }
    pos_ = pos;
    frac_ = frac;
    dir_ = int8_t(dir);
    if (st && ring_) {
        if (playing_) ring_->need = uint32_t(pos > 0 ? pos : 0);
        else ring_->src = nullptr;
    }
}

// --- block render (spec 04 §4.1 order) ----------------------------------------

OT_FASTRUN_FN bool Voice::render(const VoiceControl& c, VoiceOut& o, float* dsend, float* rsend, float* mixL, float* mixR,
                   bool keepLR) {
    if (!ins_ || (!playing_ && !env_.active())) {
        if (cleanOut_ != &o) {   // zeroed once after the voice went silent
            zeroWords(o.amp, sizeof(o.amp));
            zeroWords(o.l, sizeof(o.l));
            zeroWords(o.r, sizeof(o.r));
            cleanOut_ = &o;
        }
        peak_ = 0.0f;
        sendsTouched_ = 0;
        return false;
    }
    cleanOut_ = nullptr;
    // The stereo chain runs for the whole block when the current or the
    // pending (retriggered in this block) note is stereo (2x-04 §3).
    if (stBlock_) return renderSt(c, o, dsend, rsend, mixL, mixR);
    lastSt_ = false;
    float x[kBlock], eg[kBlock];
    const float hs = c.headroom ? 0.5f : 1.0f;   // extra headroom (§1.4)
    // Envelope in 8-sample groups; the player runs in one call up to each
    // envelope event (retrigger / end), which applies before its group.
    uint32_t done = 0;
    for (uint32_t g = 0; g < kBlock; g += 8) {
        const int ev = env_.group(eg + g);
        if (ev) {
            if (g > done) renderPlayer(x + done, g - done);
            done = g;
            if (ev & VolumeEnvelope::kEvForcedDone) startNote();
            if (ev & VolumeEnvelope::kEvIdle) playing_ = false;
        }
    }
    if (done < kBlock) renderPlayer(x + done, kBlock - done);
    // Sample end (one-shot/slice) with no retrigger pending: the voice is done.
    if (!playing_ && !pending_.valid) env_.stop();
    float* __restrict oa = o.amp;
    if (hs == 1.0f && mask_ == 0xFFFF && !(odGain_ > 0.0f) && !filtOn_ && !filtXfade_ && mixL && mixR) {
        // No crusher / overdrive / filter: envelope, sends, volume, pan and
        // the dry-bus sum in one pass (identical arithmetic to the stages
        // below; x * (eg * 1) == x * eg).
        filtLast_ = x[kBlock - 1] * eg[kBlock - 1];
        OutRamps g;
        g.di = sendD_.begin(g.s0);
        g.ri = sendR_.begin(g.s1);
        g.ai = amp_.begin(g.a0);
        g.li = panL_.begin(g.l0);
        g.rri = panR_.begin(g.r0);
        const float panMax =
            std::fmax(std::fmax(g.l0, g.l0 + g.li * float(kBlock)), std::fmax(g.r0, g.r0 + g.rri * float(kBlock)));
        const bool sd = g.s0 != 0.0f || g.di != 0.0f, sr = g.s1 != 0.0f || g.ri != 0.0f;
        sendsTouched_ = uint8_t((sd ? 1 : 0) | (sr ? 2 : 0));
        const int v = (sd ? 1 : 0) | (sr ? 2 : 0) | (keepLR ? 4 : 0);
        switch (v) {
            case 0: fusedOut<false, false, false>(x, eg, g, dsend, rsend, o, mixL, mixR); break;
            case 1: fusedOut<true, false, false>(x, eg, g, dsend, rsend, o, mixL, mixR); break;
            case 2: fusedOut<false, true, false>(x, eg, g, dsend, rsend, o, mixL, mixR); break;
            case 3: fusedOut<true, true, false>(x, eg, g, dsend, rsend, o, mixL, mixR); break;
            case 4: fusedOut<false, false, true>(x, eg, g, dsend, rsend, o, mixL, mixR); break;
            case 5: fusedOut<true, false, true>(x, eg, g, dsend, rsend, o, mixL, mixR); break;
            case 6: fusedOut<false, true, true>(x, eg, g, dsend, rsend, o, mixL, mixR); break;
            default: fusedOut<true, true, true>(x, eg, g, dsend, rsend, o, mixL, mixR); break;
        }
        float amax = 0.0f;
        for (uint32_t i = 0; i < kBlock; i += 2) {
            const float aa = oa[i] < 0.0f ? -oa[i] : oa[i];
            amax = aa > amax ? aa : amax;
        }
        peak_ = amax * panMax;
        return true;
    }
    if (!filtXfade_ && mixL && mixR && !keepLR) {
        // Crusher / overdrive / filter on: the whole chain in one pass.
        OutRamps g;
        g.di = sendD_.begin(g.s0);
        g.ri = sendR_.begin(g.s1);
        g.ai = amp_.begin(g.a0);
        g.li = panL_.begin(g.l0);
        g.rri = panR_.begin(g.r0);
        const float panMax =
            std::fmax(std::fmax(g.l0, g.l0 + g.li * float(kBlock)), std::fmax(g.r0, g.r0 + g.rri * float(kBlock)));
        const bool sd = g.s0 != 0.0f || g.di != 0.0f, sr = g.s1 != 0.0f || g.ri != 0.0f;
        sendsTouched_ = uint8_t((sd ? 1 : 0) | (sr ? 2 : 0));
        ChainCfg cf;
        cf.hs = hs;
        cf.mask = int32_t(int16_t(mask_));
        cf.od = odGain_;
        cf.fmT = fmTarget_;
        cf.dpT = dpTarget_;
        const int ft = filtOn_ ? (filtType_ == 0 ? 1 : (filtType_ == 1 ? 2 : 3)) : 0;
        const bool extra = hs != 1.0f || mask_ != 0xFFFF || odGain_ > 0.0f;
        // One send off: it feeds a scratch bus with its (all-zero) ramp.
        float scratch[kBlock];
        float* ds = sd ? dsend : scratch;
        float* rs = sr ? rsend : scratch;
        if (sd != sr) zeroFloats(scratch, kBlock);
        kChain[ft][((sd || sr) ? 2 : 0) | (extra ? 1 : 0)](x, eg, g, cf, svf_, ds, rs, oa, mixL, mixR, filtLast_);
        float amax = 0.0f;
        for (uint32_t i = 0; i < kBlock; i += 2) {
            const float aa = oa[i] < 0.0f ? -oa[i] : oa[i];
            amax = aa > amax ? aa : amax;
        }
        peak_ = amax * panMax;
        return true;
    }
    if (hs == 1.0f) {   // x * (eg * 1) == x * eg
        for (uint32_t i = 0; i < kBlock; ++i) x[i] *= eg[i];
    } else {
        for (uint32_t i = 0; i < kBlock; ++i) x[i] *= eg[i] * hs;
    }
    // Bit crusher on the int16 sample (§4.6), on stock's mask grid but rounded
    // to the nearest step (mid-tread): stock's plain AND rounds toward -inf,
    // which turns quiet signals and release tails into a negative pulse train
    // with DC (hiss). OpenTracker deviation, deliberate.
    if (mask_ != 0xFFFF) {
        const int32_t m = int32_t(int16_t(mask_));
        const int32_t half = ((~m & 0xFFFF) + 1) >> 1;
        for (uint32_t i = 0; i < kBlock; ++i) x[i] = float((int32_t(toS16(x[i])) + half) & m) * (1.0f / 32768.0f);
    }
    // Overdrive (§4.5): pre-gain then 3u / 1-(2-4u)^2/4 / hard 1.
    if (odGain_ > 0.0f) {
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float y = x[i] * odGain_;
            const float a = y < 0.0f ? -y : y;
            float v;
            if (a < 0.25f) v = 3.0f * a;
            else if (a < 0.5f) {
                const float t = 2.0f - 4.0f * a;
                v = 1.0f - t * t * 0.25f;
            } else v = 1.0f;
            x[i] = y < 0.0f ? -v : v;
        }
    }
    // Filter (§4.4) with the enable/type change cross-fade.
    if (filtOn_) svf_.process(x, kBlock, filtType_, fmTarget_, dpTarget_);
    if (filtXfade_) {
        const float last = filtLast_;
        for (uint32_t i = 0; i < kBlock; ++i)
            x[i] = (float(i + 1) * x[i] + float(127 - i) * last) * (1.0f / 128.0f);
        filtXfade_ = false;
    }
    filtLast_ = x[kBlock - 1];
    // Sends: post-filter, pre-volume (§1.3). A send ramp that is 0 for the
    // whole block adds nothing and is skipped.
    float s0, s1;
    const float di = sendD_.begin(s0);
    const float ri = sendR_.begin(s1);
    const bool sd = s0 != 0.0f || di != 0.0f, sr = s1 != 0.0f || ri != 0.0f;
    sendsTouched_ = uint8_t((sd ? 1 : 0) | (sr ? 2 : 0));
    if (sd && sr) {
        for (uint32_t i = 0; i < kBlock; ++i) {
            s0 += di;
            s1 += ri;
            dsend[i] += x[i] * s0;
            rsend[i] += x[i] * s1;
        }
    } else if (sd) {
        for (uint32_t i = 0; i < kBlock; ++i) {
            s0 += di;
            dsend[i] += x[i] * s0;
        }
    } else if (sr) {
        for (uint32_t i = 0; i < kBlock; ++i) {
            s1 += ri;
            rsend[i] += x[i] * s1;
        }
    }
    // Volume and pan, ramped (§4.2, §4.3); optionally summed into the
    // engine's dry bus here (same order of additions as a separate pass).
    float a0, l0, r0;
    const float ai = amp_.begin(a0);
    const float li = panL_.begin(l0);
    const float rri = panR_.begin(r0);
    const float panMax = std::fmax(std::fmax(l0, l0 + li * float(kBlock)), std::fmax(r0, r0 + rri * float(kBlock)));
    float amax = 0.0f;
    float* __restrict ol = o.l;
    float* __restrict orr = o.r;
    if (mixL && mixR) {
        for (uint32_t i = 0; i < kBlock; ++i) {
            a0 += ai;
            l0 += li;
            r0 += rri;
            const float a = clampFast(x[i] * a0);
            oa[i] = a;
            const float l = a * l0, r = a * r0;
            ol[i] = l;
            orr[i] = r;
            mixL[i] += l;
            mixR[i] += r;
        }
    } else {
        for (uint32_t i = 0; i < kBlock; ++i) {
            a0 += ai;
            l0 += li;
            r0 += rri;
            const float a = clampFast(x[i] * a0);
            oa[i] = a;
            ol[i] = a * l0;
            orr[i] = a * r0;
        }
    }
    // Block peak of the post-pan output for the UI meters: |amp| peak (every
    // 2nd sample) times the larger pan gain over the block (linear ramps).
    for (uint32_t i = 0; i < kBlock; i += 2) {
        const float aa = oa[i] < 0.0f ? -oa[i] : oa[i];
        amax = aa > amax ? aa : amax;
    }
    peak_ = amax * panMax;
    return true;
}

}  // namespace audio
}  // namespace ot
