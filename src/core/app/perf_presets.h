// Performance FX catalogue and preset sources (spec 12 §A, §E; spec 06 §3.2).
//
// The FX engine owns no preset memory: it works on one of two sources, the
// global presets in the config image or the per-project presets in
// project.mt, selected by the Config option "Performance presets" at screen
// start (spec 12 §A.1-§A.3). Both sources have the same byte layout, so a
// Source is three pointers into little-endian byte arrays.
#pragma once

#include <cstdint>

namespace ot {

class App;
struct Config;
struct Project;

namespace perf {

constexpr int kSlots = 12;
constexpr int kRows = 4;          // stored rows 0..3 (0 = off)
constexpr int kLiveRow = 4;       // MIDI-CC live value (spec 12 §B.2)
constexpr int kFxCount = 23;      // 0 = empty, 1..21 stock, 22 OT fork (Acid Density)

// FX ids (spec 12 §E.2).
enum Fx : uint8_t {
    kNone = 0, kVolume, kPanning, kTune, kLowPass, kHighPass, kBandPass, kDelaySend, kReverbSend,
    kStart, kEnd, kDirection, kVolLfo, kPanLfo, kFineLfo, kFilterLfo, kPosLfo, kRepeater, kPlayMode,
    kPatternLength, kBitDepth, kOverdrive,
    kAcidDensity,   // OT fork: density offset of the live Acid tracks (live_gen.h)
};

// Value descriptor (spec 12 §E.1): min, max, label table, exclusive-with
// ids, global flag. Names are OpenTracker's.
struct FxDesc {
    const char* name;
    int16_t min, max;
    const char* const* labels;   // value v -> labels[v - min]; null = numeric
    uint8_t exclA, exclB;        // other ids this one excludes (LP/HP/BP)
    bool global;                 // applied once, not per track
};
const FxDesc& fx(uint8_t id);

// Value text (spec 06 §3.2): the label when the FX has a label table,
// "%+d" for non-zero values, kZeroText for 0.
constexpr const char* kZeroText = "0";
void valueText(uint8_t id, int v, char* out, int cap);

// CC 51..62 value -> slot value (spec 12 §B.2): min + (max - min) * v / 127,
// truncating toward zero.
int ccToValue(uint8_t id, uint8_t v);

// Sample position / end offset in 1/65535 marker units (spec 12 §E.2 ids 9/10).
int32_t startOffset(int v);
int32_t endOffset(int v);
// Pattern length FX (id 19): last-step index override, -1 = none.
int lengthOverride(int v);

// One preset source (spec 12 §A.2).
struct Source {
    uint8_t* types = nullptr;    // u8[12]
    uint8_t* values = nullptr;   // i16[12][4] little endian, slot-major
    uint8_t* rows = nullptr;     // u8[12]

    bool valid() const { return types && values && rows; }
    uint8_t type(int s) const { return types[s]; }
    void setType(int s, uint8_t t) const { types[s] = t; }
    int16_t value(int s, int r) const {
        const uint8_t* p = values + 8 * s + 2 * r;
        return int16_t(uint16_t(p[0] | (p[1] << 8)));
    }
    void setValue(int s, int r, int16_t v) const {
        uint8_t* p = values + 8 * s + 2 * r;
        p[0] = uint8_t(uint16_t(v));
        p[1] = uint8_t(uint16_t(v) >> 8);
    }
    uint8_t row(int s) const { return rows[s]; }
    void setRow(int s, uint8_t r) const { rows[s] = r; }
    // Engine-side repeat of the load checks (spec 12 §A.2 [LG:0x43404]).
    void validate() const;
};

Source globalSource(Config& cfg);
Source projectSource(Project& p);
// Selector byte: 0 global, 1 per project (spec 12 §A.3).
Source select(uint8_t selector, Config& cfg, Project& p);

// Per-track pattern overrides PRJ 0x142 (spec 12 §A.4), 1..255.
uint8_t trackPattern(const Project& p, int t);
void setTrackPattern(Project& p, int t, uint8_t num);

}  // namespace perf

// Per-track settings across both banks (docs/specs/2x-01 §1.1, §4), as
// the Tracker Mini (docs/MINI_PLAN.md §1, spec 21 §3.2), t = 0..15:
// - level: tracks 1-8 only (0x4D0); the Mini has no levels for 9-16, so
//   level() reads 0 and setLevel() ignores them;
// - mute: project.mt 0x12C (1-8) and 0x742[0..7] (9-16, App::trackMuted);
// - name: project.mt 0x428 (1-8, 20 chars) and 0x603 slots 0..7 (9-16,
//   7 chars); "" is stored as written.
namespace trk {
bool muted(App& a, int t);
void setMuted(App& a, int t, bool m);
uint8_t level(App& a, int t);
void setLevel(App& a, int t, uint8_t v);
void name(App& a, int t, char* out, int cap);
void setName(App& a, int t, const char* s);
}  // namespace trk
}  // namespace ot
