// Instrument .pti codec, spec 02 §6.
//
// Note on tracker-lib differences (spec 02 §11): tracker-lib's parser takes
// the audio from 0x184 and the "CRC" from the last 4 bytes of the file; the
// firmware (followed here) stores the CRC at 0x184 and audio at body size +
// 0x14. Overdrive is stored as a raw percent (tracker-lib writes x100).
#include "formats/instrument_pti.h"

#include <cmath>
#include <cstring>

#include "formats/bytes.h"
#include "formats/crc32.h"
#include "formats/legacy_upgrade.h"

namespace ot {

namespace {
BlockUpgradeFn g_upgrade = nullptr;

// Body (struct) offsets, spec 02 §6.2.
enum : uint32_t {
    kActive = 0x000,
    kSampleType = 0x004,
    kName = 0x005,
    kNameField = 32,
    kRamPtr = 0x028,
    kLength = 0x02C,
    kWtWindowSize = 0x030,
    kWtWindowCount = 0x034,
    kReserved38 = 0x038,
    kPlayMode = 0x03C,
    kStart = 0x03E,
    kLoop1 = 0x040,
    kLoop2 = 0x042,
    kEnd = 0x044,
    kWtCurrent = 0x048,
    kEnv = 0x04C,
    kEnvSize = 20,
    kLfo = 0x0C4,
    kLfoSize = 8,
    kCutoff = 0x0F4,
    kResonance = 0x0F8,
    kFilterType = 0x0FC,
    kFilterEnabled = 0x0FD,
    kTune = 0x0FE,
    kFinetune = 0x0FF,
    kVolume = 0x100,
    kPanning = 0x104,
    kDelaySend = 0x106,
    kSlices = 0x108,
    kSliceCnt = 0x168,
    kSelectedSlice = 0x169,
    kGrainLength = 0x16A,
    kGranularPos = 0x16C,
    kGrainShape = 0x16E,
    kGranularLoop = 0x16F,
    kReverbSend = 0x170,
    kOverdrive = 0x171,
    kBitDepth = 0x172,
    kReserved173 = 0x173,
};
static_assert(kEnv + kAutomationCount * kEnvSize == kLfo, "envelope table size");
static_assert(kLfo + kAutomationCount * kLfoSize == kCutoff, "lfo table size");
static_assert(kSlices + 2 * ot::kSliceCount == kSliceCnt, "slice table size");

void encodeBody(const Instrument& ins, uint8_t* b) {
    std::memset(b, 0, pti::kBodySize);
    b[kActive] = ins.active;
    b[kSampleType] = ins.sampleType;
    wrStr(b + kName, kNameField, ins.name);
    wrU32(b + kRamPtr, 0);
    wrU32(b + kLength, ins.lengthFrames);
    wrU16(b + kWtWindowSize, ins.wavetableWindowSize);
    wrU32(b + kWtWindowCount, ins.wavetableWindowCount);
    wrU32(b + kReserved38, ins.reserved38);
    b[kPlayMode] = ins.playMode;
    wrU16(b + kStart, ins.startPoint);
    wrU16(b + kLoop1, ins.loopPoint1);
    wrU16(b + kLoop2, ins.loopPoint2);
    wrU16(b + kEnd, ins.endPoint);
    wrU32(b + kWtCurrent, ins.wavetableCurrentWindow);
    for (int i = 0; i < kAutomationCount; ++i) {
        uint8_t* e = b + kEnv + i * kEnvSize;
        const Envelope& env = ins.env[i];
        wrF32(e + 0, env.amount);
        wrU16(e + 4, env.delayMs);
        wrU16(e + 6, env.attackMs);
        wrU16(e + 8, env.holdMs);
        wrU16(e + 10, env.decayMs);
        wrF32(e + 12, env.sustain);
        wrU16(e + 16, env.releaseMs);
        e[18] = env.loop;
        e[19] = env.enabled;
        uint8_t* l = b + kLfo + i * kLfoSize;
        l[0] = ins.lfo[i].shape;
        l[1] = ins.lfo[i].speed;
        wrF32(l + 4, ins.lfo[i].amount);
    }
    wrF32(b + kCutoff, ins.cutoff);
    wrF32(b + kResonance, ins.resonance);
    b[kFilterType] = ins.filterType;
    b[kFilterEnabled] = ins.filterEnabled;
    b[kTune] = uint8_t(ins.tune);
    b[kFinetune] = uint8_t(ins.finetune);
    b[kVolume] = ins.volume;
    wrU16(b + kPanning, uint16_t(ins.panning));
    b[kDelaySend] = ins.delaySend;
    for (int i = 0; i < ot::kSliceCount; ++i) wrU16(b + kSlices + 2 * i, ins.slices[i]);
    b[kSliceCnt] = ins.sliceCount;
    b[kSelectedSlice] = ins.selectedSlice;
    wrU16(b + kGrainLength, ins.grainLength);
    wrU16(b + kGranularPos, ins.granularPosition);
    b[kGrainShape] = ins.grainShape;
    b[kGranularLoop] = ins.granularLoop;
    b[kReverbSend] = ins.reverbSend;
    b[kOverdrive] = ins.overdrive;
    b[kBitDepth] = ins.bitDepth;
    b[kReserved173] = ins.reserved173;
}

// Float fields of a damaged file: non-finite -> the field's default, else
// clamped to the editor's range (a NaN cutoff/resonance would make the voice
// filter state NaN for good). OpenTracker check; stock loads them raw.
float rdF32Range(const uint8_t* p, float def, float lo, float hi) {
    const float v = rdF32(p);
    if (!std::isfinite(v)) return def;
    return v < lo ? lo : (v > hi ? hi : v);
}

void decodeBody(const uint8_t* b, Instrument& ins) {
    ins.active = b[kActive];
    ins.sampleType = b[kSampleType];
    rdStr(b + kName, kNameField, ins.name, sizeof(ins.name));
    // kRamPtr: stale RAM pointer, ignored (spec 02 §6.2).
    ins.lengthFrames = rdU32(b + kLength);
    ins.wavetableWindowSize = rdU16(b + kWtWindowSize);
    ins.wavetableWindowCount = rdU32(b + kWtWindowCount);
    ins.reserved38 = rdU32(b + kReserved38);
    ins.playMode = b[kPlayMode];
    ins.startPoint = rdU16(b + kStart);
    ins.loopPoint1 = rdU16(b + kLoop1);
    ins.loopPoint2 = rdU16(b + kLoop2);
    ins.endPoint = rdU16(b + kEnd);
    ins.wavetableCurrentWindow = rdU32(b + kWtCurrent);
    for (int i = 0; i < kAutomationCount; ++i) {
        const uint8_t* e = b + kEnv + i * kEnvSize;
        Envelope& env = ins.env[i];
        env.amount = rdF32Range(e + 0, Envelope().amount, 0.0f, 1.0f);
        env.delayMs = rdU16(e + 4);
        env.attackMs = rdU16(e + 6);
        env.holdMs = rdU16(e + 8);
        env.decayMs = rdU16(e + 10);
        env.sustain = rdF32Range(e + 12, Envelope().sustain, 0.0f, 1.0f);
        env.releaseMs = rdU16(e + 16);
        env.loop = e[18];
        env.enabled = e[19];
        const uint8_t* l = b + kLfo + i * kLfoSize;
        ins.lfo[i].shape = l[0];
        ins.lfo[i].speed = l[1];
        ins.lfo[i].amount = rdF32Range(l + 4, Lfo().amount, 0.0f, 1.0f);
    }
    ins.cutoff = rdF32Range(b + kCutoff, Instrument().cutoff, 0.0f, 1.0f);
    ins.resonance = rdF32Range(b + kResonance, Instrument().resonance, 0.0f, 4.3f);   // editor max
    ins.filterType = b[kFilterType];
    ins.filterEnabled = b[kFilterEnabled];
    ins.tune = int8_t(b[kTune]);
    ins.finetune = int8_t(b[kFinetune]);
    ins.volume = b[kVolume];
    ins.panning = rdI16(b + kPanning);
    ins.delaySend = b[kDelaySend];
    for (int i = 0; i < ot::kSliceCount; ++i) ins.slices[i] = rdU16(b + kSlices + 2 * i);
    ins.sliceCount = b[kSliceCnt];
    ins.selectedSlice = b[kSelectedSlice];
    ins.grainLength = rdU16(b + kGrainLength);
    ins.granularPosition = rdU16(b + kGranularPos);
    ins.grainShape = b[kGrainShape];
    ins.granularLoop = b[kGranularLoop];
    ins.reverbSend = b[kReverbSend];
    ins.overdrive = b[kOverdrive];
    ins.bitDepth = b[kBitDepth];
    ins.reserved173 = b[kReserved173];
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// Strip a trailing ".wav" (spec 02 §6.2). Case-insensitive (OpenTracker
// choice; the spec does not state case handling).
void stripWav(char* s) {
    size_t n = std::strlen(s);
    if (n < 4) return;
    char* e = s + n - 4;
    if (e[0] == '.' && lower(e[1]) == 'w' && lower(e[2]) == 'a' && lower(e[3]) == 'v') *e = 0;
}
}  // namespace

void setPtiUpgradeHook(BlockUpgradeFn fn) { g_upgrade = fn; }

void encodePtiHeader(const Instrument& ins, uint8_t out[pti::kHeaderBlockSize]) {
    FileHeader h{{'T', 'I'}, 1, {}, {}, uint16_t(pti::kBodySize)};
    std::memcpy(h.fwVersion, pti::kFwVersion, 4);
    std::memset(h.structVersion, pti::kStructVersion, 4);
    writeFileHeader(out, h);
    encodeBody(ins, out + pti::kBodyOff);
    wrU32(out + pti::kCrcOff, crc32(out, pti::kCrcOff));
}

FmtStatus decodePtiHeader(const uint8_t* file, size_t len, Instrument& ins, PtiLoadInfo* info) {
    if (len < kCommonHeaderSize) return FmtStatus::TooShort;
    // spec 10 §0.1: read min(len, 0x188) bytes into a zero-filled 0x188-byte
    // buffer. The body is always the 0x174 bytes from 0x10 (spec 10 §3.2),
    // so a 368-byte legacy body carries its old CRC in struct 0x170..0x173
    // until the chain overwrites it.
    uint8_t hdr[pti::kHeaderBlockSize];
    std::memset(hdr, 0, sizeof(hdr));
    std::memcpy(hdr, file, len < sizeof(hdr) ? len : sizeof(hdr));
    FileHeader h = readFileHeader(hdr);
    if (h.type != 1) return FmtStatus::WrongType;

    PtiLoadInfo li;
    li.structVersion = h.structVersion[0];
    li.bodySize = h.size;
    li.dataOffset = uint32_t(h.size) + 0x14;  // spec 02 §6.1, spec 10 §3.2

    // spec 10 §3.2 chain (version 0 treated as 1). A hook overrides it.
    if (li.structVersion < pti::kStructVersion) {
        li.upgraded = true;
        if (g_upgrade) g_upgrade(hdr + pti::kBodyOff, pti::kBodySize, li.structVersion);
        else legacy::upgradePtiImage(hdr, li.structVersion);
    }

    decodeBody(hdr + pti::kBodyOff, ins);
    stripWav(ins.name);
    ins.stereo = false;
    if (info) *info = li;
    return FmtStatus::Ok;
}

PtiSampleLayout resolvePtiSample(uint32_t len, uint32_t bytes, bool stereoEnabled) {
    PtiSampleLayout l;
    if (len != 0 && uint64_t(bytes) == uint64_t(len) * 4) {
        l.frames = len;
        l.stereoInFile = true;
        l.useStereo = stereoEnabled;  // stereo off: only the left block is used
    } else if (len != 0 && uint64_t(bytes) == uint64_t(len) * 2) {
        l.frames = len;
    } else {
        l.frames = bytes / 2;  // repair; header must be rewritten
        l.headerDirty = true;
    }
    return l;
}

size_t encodePti(const Instrument& ins, const int16_t* left, const int16_t* right, uint8_t* out, size_t cap) {
    const size_t total = ptiFileSize(ins.lengthFrames, right != nullptr);
    if (cap < total) return 0;
    encodePtiHeader(ins, out);
    uint8_t* p = out + pti::kDataOff;
    for (uint32_t i = 0; i < ins.lengthFrames; ++i, p += 2) wrU16(p, uint16_t(left[i]));
    if (right)
        for (uint32_t i = 0; i < ins.lengthFrames; ++i, p += 2) wrU16(p, uint16_t(right[i]));
    return total;
}

FmtStatus decodePtiSamples(const uint8_t* file, size_t len, const PtiLoadInfo& info,
                           const PtiSampleLayout& layout, Instrument& ins, int16_t* dst, size_t dstCapSamples) {
    const size_t channels = layout.useStereo ? 2 : 1;
    if (size_t(layout.frames) * channels > dstCapSamples) return FmtStatus::NoSpace;
    if (len < info.dataOffset) return layout.frames ? FmtStatus::TooShort : FmtStatus::Ok;
    const uint8_t* src = file + info.dataOffset;
    const size_t avail = (len - info.dataOffset) / 2;
    const size_t needed = size_t(layout.frames) * (layout.useStereo ? 2 : 1);
    // Left block starts at 0, right block at frames (planar on disk too).
    for (size_t i = 0; i < needed; ++i) dst[i] = i < avail ? rdI16(src + 2 * i) : 0;
    ins.lengthFrames = layout.frames;
    ins.stereo = layout.useStereo;
    return FmtStatus::Ok;
}

}  // namespace ot
