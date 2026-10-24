#include "app/perf_presets.h"

#include <cmath>
#include <cstdio>

#include "app/app.h"
#include "app/config.h"
#include "app/project_fields.h"
#include "fmath.h"
#include "model/project.h"
#include "sequencer/seq_playmodes.h"

namespace ot {
namespace perf {

namespace {
// Label tables (spec 12 §E.2): playback direction, step repeater (Off + 15
// rates in steps), pattern length (none + powers of two).
const char* const kDirection[] = {">>>", "<<<"};
const char* const kRepeater[] = {"Off", "16", "12", "8", "6", "4", "3", "2", "1",
                                 "1/2", "1/3", "1/4", "1/6", "1/8", "1/12", "1/16"};
const char* const kLength[] = {"---", "1", "2", "4", "8", "16", "32", "64", "128"};

// spec 06 §3.2 / spec 12 §E.2 ranges; LP/HP/BP exclude each other (§E.1).
// Names: stock 1.9.2 screen (owner photo 2026-10-23) for the first 12; the rest
// from the manual p.276 list until a stock capture shows them [I].
const FxDesc kFx[kFxCount] = {
    {"---", 0, 0, nullptr, 0, 0, false},
    {"Volume/Velocity", -100, 100, nullptr, 0, 0, false},
    {"Sample panning", -100, 100, nullptr, 0, 0, false},
    {"Tune", -48, 48, nullptr, 0, 0, false},
    {"Sample LP cutoff", -100, 100, nullptr, kHighPass, kBandPass, false},
    {"Sample HP cutoff", -100, 100, nullptr, kLowPass, kBandPass, false},
    {"Sample BP cutoff", -100, 100, nullptr, kLowPass, kHighPass, false},
    {"Sample delay send", -100, 100, nullptr, 0, 0, false},
    {"Sample reverb snd", -100, 100, nullptr, 0, 0, false},
    {"Sample position", -100, 100, nullptr, 0, 0, false},
    {"Sample end", -100, 100, nullptr, 0, 0, false},
    {"Sample playback", 0, 1, kDirection, 0, 0, false},
    {"Volume LFO speed", -23, 23, nullptr, 0, 0, false},
    {"Panning LFO Speed", -28, 28, nullptr, 0, 0, false},
    {"Finetune LFO Speed", -28, 28, nullptr, 0, 0, false},
    {"Filter LFO Speed", -28, 28, nullptr, 0, 0, false},
    {"Grain/WT LFO Speed", -28, 28, nullptr, 0, 0, false},
    {"Step Repeater", 0, 15, kRepeater, 0, 0, false},
    {"Pattern Play Mode", 0, 22, nullptr, 0, 0, false},   // labels: seq::playModeLabel
    {"Pattern Length", 0, 8, kLength, 0, 0, true},
    {"Bit Depth", -12, 12, nullptr, 0, 0, false},
    {"Overdrive", -100, 100, nullptr, 0, 0, false},
};
}  // namespace

const FxDesc& fx(uint8_t id) { return kFx[id < kFxCount ? id : 0]; }

void valueText(uint8_t id, int v, char* out, int cap) {
    const FxDesc& d = fx(id);
    if (id == kPlayMode && v >= 0 && v < seq::kPlayModeCount) {
        std::snprintf(out, size_t(cap), "%s", seq::playModeLabel(uint8_t(v)));
    } else if (d.labels && v >= d.min && v <= d.max) {
        std::snprintf(out, size_t(cap), "%s", d.labels[v - d.min]);
    } else if (v == 0) {
        std::snprintf(out, size_t(cap), "%s", kZeroText);
    } else {
        std::snprintf(out, size_t(cap), "%+d", v);
    }
}

int ccToValue(uint8_t id, uint8_t v) {
    const FxDesc& d = fx(id);
    const int x = v > 127 ? 127 : v;
    return d.min + ((d.max - d.min) * x) / 127;
}

int32_t startOffset(int v) {
    if (v < -100) v = -100;
    if (v > 100) v = 100;
    return int32_t((v + 100) * 131070 / 200) - 65535;
}

int32_t endOffset(int v) {
    if (v < -100) v = -100;
    if (v > 100) v = 100;
    return int32_t(fm::lroundD(double(v + 100) * 131070.0 / 200.0 - 65535.0));
}

int lengthOverride(int v) { return v >= 1 && v <= 8 ? (1 << (v - 1)) - 1 : -1; }

void Source::validate() const {
    if (!valid()) return;
    for (int s = 0; s < kSlots; ++s) {
        if (types[s] >= kFxCount) types[s] = uint8_t(s + 1 < kFxCount ? s + 1 : 0);
        if (rows[s] > 3) rows[s] = 0;
        setValue(s, 0, 0);
        for (int r = 1; r < kRows; ++r)
            if (value(s, r) < -255 || value(s, r) > 255) setValue(s, r, 0);
    }
}

Source globalSource(Config& cfg) {
    Source s;
    s.types = cfg.perfTypes();
    s.values = cfg.perfValues();
    s.rows = cfg.perfRows();
    return s;
}

Source projectSource(Project& p) {
    Source s;
    s.types = &p.values[pf::kPerfTypes - mt::kValuesOff];
    s.values = &p.values[pf::kPerfValues - mt::kValuesOff];
    s.rows = &p.values[pf::kPerfRows - mt::kValuesOff];
    return s;
}

Source select(uint8_t selector, Config& cfg, Project& p) {
    return selector == 1 ? projectSource(p) : globalSource(cfg);
}

uint8_t trackPattern(const Project& p, int t) {
    const uint16_t v = p.u16(pf::kPerfPatterns + 2u * uint32_t(t));
    return v >= 1 && v <= 255 ? uint8_t(v) : 1;
}

void setTrackPattern(Project& p, int t, uint8_t num) { p.setU16(pf::kPerfPatterns + 2u * uint32_t(t), num); }

}  // namespace perf

namespace trk {

bool muted(App& a, int t) { return a.trackMuted(t); }

void setMuted(App& a, int t, bool m) {
    if (t < 0 || t >= kTrackCount || a.trackMuted(t) == m) return;
    a.setTrackMute(t, m);
}

uint8_t level(App& a, int t) {
    if (t < 0 || t >= kMidiTrackStart) return 0;
    return a.project.u8(pf::kTrackVolume + uint32_t(t));
}

void setLevel(App& a, int t, uint8_t v) {
    if (t < 0 || t >= kMidiTrackStart) return;
    if (v > 100) v = 100;
    a.project.setU8(pf::kTrackVolume + uint32_t(t), v);
    a.markProjectDirty();
}

void name(App& a, int t, char* out, int cap) {
    if (cap <= 0) return;
    out[0] = 0;
    if (t < 0 || t >= kTrackCount) return;
    a.project.trackName(t, out, uint32_t(cap));
}

void setName(App& a, int t, const char* s) {
    if (t < 0 || t >= kTrackCount || !s) return;
    a.project.setTrackName(t, s);   // "" clears the name (VERIFY_M1 R5)
    a.markProjectDirty();
}

}  // namespace trk
}  // namespace ot
