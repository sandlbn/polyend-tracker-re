// Project model defaults and accessors (spec 02 §3.2, §3.3, §5, §7).
#include "model/project.h"

#include <cstdio>
#include <cstring>

#include "formats/bytes.h"

namespace ot {

uint16_t Project::u16(uint32_t off) const { return rdU16(&values[off - mt::kValuesOff]); }
void Project::setU16(uint32_t off, uint16_t v) { wrU16(&values[off - mt::kValuesOff], v); }
uint32_t Project::u32(uint32_t off) const { return rdU32(&values[off - mt::kValuesOff]); }
void Project::setU32(uint32_t off, uint32_t v) { wrU32(&values[off - mt::kValuesOff], v); }
float Project::f32(uint32_t off) const { return rdF32(&values[off - mt::kValuesOff]); }
void Project::setF32(uint32_t off, float v) { wrF32(&values[off - mt::kValuesOff], v); }

void Project::setDefault() {
    resetUsbAudio();
    // New Project (spec 07 Q2, Q3, Q6; FW:0x234DC): the load pre-fill, then
    // the values the firmware's New Project writes.
    setLoadPrefill();
    setU16(0x120, 100);          // limiter attack, ms
    setF32(0x124, 0.512f);       // limiter release, ms (below the UI minimum; kept)
    setU16(0x128, 30200);        // limiter threshold -> LIMTHR[92] = -6.0 dBFS
    setU8(0x12A, 0x10);          // not limiter (unknown)
    setU8(0x12B, 0x01);
    for (int t = 0; t < 8; ++t) setU8(0x12C + uint32_t(t), 0);   // track mute flags
    setU8(0x309, 86);            // line-in level 0 dB
    for (int i = 0; i < 8; ++i) setU8(mt::kLevels10 + uint32_t(i), 86);   // track levels 0 dB
    setU8(0x4D8, 86);            // Global Mixer Reverb
    setU8(0x4D9, 86);            // Global Mixer Delay
    setU8(0x4DA, 0);             // reverb / delay / dry mute
    setU8(0x4DB, 0);
    setU8(0x4DC, 0);
    setU8(0x4DD, 86);            // Dry Mix
    setU8(0x4FA, 0);             // limiter mode
    setU8(0x4FB, 0);             // side-chain off
    setU8(0x4FC, 1);             // Sends mode = post-fader
    setU16(mt::kU16_5FE, 20000); // delay filter, Hz
    // Tracker Mini (spec 21 §3.2, §3.3): three default synth slots. The
    // "Midi 9".."Midi 16" names come from the load pre-fill.
    for (int s = 0; s < kSynthSlotCount; ++s) resetSynthSlot(s);
    // Owner decision for 2.0: WTFM has no engine yet, so a New Project starts
    // Synth 1 on ACD instead of the Mini's WTFM (a dead slot otherwise).
    // Synth 2 / 3 keep the Mini defaults (ACD / VAP), and the load sanitiser
    // keeps SynthSlotRecord::defaults (WTFM / ACD / VAP, spec 21 §3.3).
    // 2.1: back to WTFM
    SynthSlotRecord s0 = synthSlot(0);
    s0.model = uint32_t(SynthModel::ACD);
    setSynthSlot(0, s0);
}

void Project::setLoadPrefill() {
    // spec 02 §3.2: playlist slot0 = 1, rest 0; position 0.
    std::memset(playlist, 0, sizeof(playlist));
    playlist[0] = 1;
    playlistPos = 0;

    // spec 02 §3.3 "Default" column; unlisted bytes 0.
    std::memset(values, 0, sizeof(values));
    setU8(mt::kCurrentPattern, 1);
    setU8(0x115, 0);
    setU8(0x116, 11);
    setU8(0x117, 24);
    setU8(0x118, 8);
    // Fields with no firmware default but a tracker-lib template value
    // ("tl-tpl" in spec 02 §3.3) take the template value.
    setU8(mt::kDelayFeedback, 50);
    setU16(mt::kDelayTime, 500);
    setU16(0x120, 100);
    setF32(0x124, 0.512f);
    setU32(0x128, 0x01104000);
    setF32(0x13C, 87.5f);
    for (int i = 0; i < 8; ++i) setU16(mt::kTrackWord8 + 2 * i, 1);
    for (int i = 0; i < 12; ++i) setU8(mt::kTrackIndex12 + i, uint8_t(i + 1));
    setTempo(130.0f);
    setU16(mt::kDefaultPatternLength, 32);
    setU16(0x30A, 1);
    setU16(0x30C, 1);
    setU16(0x30E, 1);
    setF32(mt::kReverbSize, 0.5f);
    setF32(mt::kReverbDamp, 0.5f);
    setF32(mt::kReverbPreDelay, 0.5f);
    setF32(mt::kReverbDiffusion, 0.68f);
    char buf[24];
    for (int i = 0; i < 8; ++i) {
        std::snprintf(buf, sizeof(buf), "Track %d", i + 1);
        setTrackName(i, buf);
    }
    for (int i = 0; i < 10; ++i) setU8(mt::kLevels10 + i, 100);
    setU8(0x4DD, 100);
    setU32(0x4F8, 0x00000103);
    setU8(0x4FC, 1);
    for (int i = 0; i < kMidiInstrumentCount; ++i) {
        MidiInstrument m;
        m.channel = uint8_t(i);  // (I) matches the tracker-lib template
        setMidiInstrument(i, m);
    }
    setU16(mt::kU16_5FE, 20000);
    for (int i = 0; i < 8; ++i) {
        std::snprintf(buf, sizeof(buf), "Midi %d", i + 9);
        setTrackName(8 + i, buf);
    }

    setName("New Project");
    setFolder("");
}

bool Project::trackName(int track, char* dst, uint32_t cap) const {
    if (track < 0 || track >= 16) return false;
    if (track < 8)
        rdStr(&values[mt::kTrackNames1to8 + track * mt::kTrackNameLen1to8 - mt::kValuesOff],
              mt::kTrackNameLen1to8, dst, cap);
    else
        rdStr(&values[mt::kTrackNames9to16 + (track - 8) * mt::kTrackNameLen9to16 - mt::kValuesOff],
              mt::kTrackNameLen9to16, dst, cap);
    return true;
}

bool Project::setTrackName(int track, const char* s) {
    if (track < 0 || track >= 16) return false;
    if (track < 8)
        wrStr(&values[mt::kTrackNames1to8 + track * mt::kTrackNameLen1to8 - mt::kValuesOff],
              mt::kTrackNameLen1to8, s);
    else
        wrStr(&values[mt::kTrackNames9to16 + (track - 8) * mt::kTrackNameLen9to16 - mt::kValuesOff],
              mt::kTrackNameLen9to16, s);
    return true;
}

bool Project::trackMuted(int track) const {
    if (track < 0 || track >= kTrackCount) return false;
    return track < 8 ? u8(mt::kTrackMutes1to8 + uint32_t(track)) != 0
                     : u8(mt::kTrackMutes9to16 + uint32_t(track - 8)) != 0;
}

bool Project::setTrackMuted(int track, bool muted) {
    if (track < 0 || track >= kTrackCount) return false;
    if (track < 8) setU8(mt::kTrackMutes1to8 + uint32_t(track), muted ? 1 : 0);
    else setU8(mt::kTrackMutes9to16 + uint32_t(track - 8), muted ? 1 : 0);
    return true;
}

const char* synthModelName(SynthModel m) {
    static const char* const kNames[kSynthModelCount] = {"ACD", "FAT", "VAP", "WTFM", "PERC"};
    return uint8_t(m) < kSynthModelCount ? kNames[uint8_t(m)] : "";
}

bool SynthSlotRecord::valid() const {
    // spec 21 §3.3 "Valid range (load check)"; written so NaN fails.
    if (!(gainDb >= -96.0f && gainDb <= 12.0f)) return false;
    if (polyphony > 8) return false;
    if (model >= uint32_t(kSynthModelCount)) return false;
    if (!(pan >= -100.0f && pan <= 100.0f)) return false;
    if (!(reverbSend >= 0.0f && reverbSend <= 100.0f)) return false;
    if (!(delaySend >= 0.0f && delaySend <= 100.0f)) return false;
    for (uint8_t m : macros)
        if (m & 0x80) return false;
    return true;
}

SynthSlotRecord SynthSlotRecord::defaults(int slot, uint8_t bipolarMask) {
    static const SynthModel kModels[kSynthSlotCount] = {SynthModel::WTFM, SynthModel::ACD, SynthModel::VAP};
    SynthSlotRecord r;
    r.gainDb = -9.0f;
    r.polyphony = 1;
    r.model = uint32_t(kModels[(slot >= 0 && slot < kSynthSlotCount) ? slot : 0]);
    r.pan = 0.0f;
    copyStr(r.patchName, sizeof(r.patchName), "Init");
    r.reverbSend = 0.0f;
    r.delaySend = 0.0f;
    for (uint32_t m = 0; m < mt::kSynthMacroCount; ++m) r.macros[m] = (bipolarMask >> m) & 1 ? 63 : 0;
    return r;
}

void SynthSlotRecord::decode(const uint8_t* rec) {
    gainDb = rdF32(rec + mt::kSynthGain);
    polyphony = rdU32(rec + mt::kSynthPolyphony);
    model = rdU32(rec + mt::kSynthModel);
    pan = rdF32(rec + mt::kSynthPan);
    rdStr(rec + mt::kSynthPatchName, mt::kSynthPatchNameSize, patchName, sizeof(patchName));
    reverbSend = rdF32(rec + mt::kSynthReverbSend);
    delaySend = rdF32(rec + mt::kSynthDelaySend);
    std::memcpy(macros, rec + mt::kSynthMacros, mt::kSynthMacroCount);
    std::memcpy(pad, rec + mt::kSynthPad, 2);
}

void SynthSlotRecord::encode(uint8_t* rec) const {
    wrF32(rec + mt::kSynthGain, gainDb);
    wrU32(rec + mt::kSynthPolyphony, polyphony);
    wrU32(rec + mt::kSynthModel, model);
    wrF32(rec + mt::kSynthPan, pan);
    // The name field holds up to 32 bytes with no NUL (the Mini keeps 32
    // non-NUL bytes; wrStr would cut the 32nd).
    std::memset(rec + mt::kSynthPatchName, 0, mt::kSynthPatchNameSize);
    std::memcpy(rec + mt::kSynthPatchName, patchName, strnlen(patchName, mt::kSynthPatchNameSize));
    wrF32(rec + mt::kSynthReverbSend, reverbSend);
    wrF32(rec + mt::kSynthDelaySend, delaySend);
    std::memcpy(rec + mt::kSynthMacros, macros, mt::kSynthMacroCount);
    std::memcpy(rec + mt::kSynthPad, pad, 2);
}

SynthSlotRecord Project::synthSlot(int slot) const {
    if (slot < 0 || slot >= kSynthSlotCount) return SynthSlotRecord::defaults(0);
    SynthSlotRecord r;
    r.decode(&values[mt::synthSlotOff(slot) - mt::kValuesOff]);
    return r;
}

void Project::setSynthSlot(int slot, const SynthSlotRecord& r) {
    if (slot < 0 || slot >= kSynthSlotCount) return;
    r.encode(&values[mt::synthSlotOff(slot) - mt::kValuesOff]);
}

MidiInstrument Project::midiInstrument(int idx) const {
    MidiInstrument m;
    if (idx < 0 || idx >= kMidiInstrumentCount) return m;
    const uint8_t* p = &values[mt::kMidiRecords + idx * mt::kMidiRecordSize - mt::kValuesOff];
    m.channel = p[0];
    m.velocity = p[1];
    std::memcpy(m.cc, p + 2, 6);
    std::memcpy(m.extra, p + 8, 8);
    return m;
}

void Project::setMidiInstrument(int idx, const MidiInstrument& m) {
    if (idx < 0 || idx >= kMidiInstrumentCount) return;
    uint8_t* p = &values[mt::kMidiRecords + idx * mt::kMidiRecordSize - mt::kValuesOff];
    p[0] = m.channel;
    p[1] = m.velocity;
    std::memcpy(p + 2, m.cc, 6);
    std::memcpy(p + 8, m.extra, 8);
}

void Project::setName(const char* s) { copyStr(name, sizeof(name), s); }
void Project::setFolder(const char* s) { copyStr(folder, sizeof(folder), s); }

void PatternNames::set(int pattern, const char* s) {
    if (pattern < 0 || pattern >= kCount) return;
    copyStr(names[pattern], sizeof(names[pattern]), s);
}

void Recents::displayName(const char* path, char* dst, uint32_t cap) {
    const char* last = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' && p[1]) last = p + 1;
    char tmp[33];
    uint32_t n = 0;
    while (last[n] && last[n] != '/' && n < 32) {
        tmp[n] = last[n];
        ++n;
    }
    tmp[n] = 0;
    copyStr(dst, cap, tmp);
}

void Recents::touch(const char* path) {
    if (!path || !*path) return;
    int found = kSlots - 1;
    for (int i = 0; i < kSlots; ++i) {
        if (std::strncmp(paths[i], path, kSlotSize) == 0) {
            found = i;
            break;
        }
    }
    for (int i = found; i > 0; --i) std::memcpy(paths[i], paths[i - 1], kSlotSize);
    std::memset(paths[0], 0, kSlotSize);
    copyStr(paths[0], kSlotSize, path);
}

}  // namespace ot
