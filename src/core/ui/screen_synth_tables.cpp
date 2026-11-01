// Synth parameter labels, sections and value texts for the synth screens
// (2.x WP-G). Indices and ranges: spec 21 §8 with the docs/MINI_PLAN.md
// corrections (formats/patch_pb.cpp holds the ranges); labels, sections,
// enum names and display units: Tracker Mini manual p.153-170 [MAN]; WTFM
// Ratio shows its value 0.25 .. 12.00 (Polyend Synth manual WTFM range,
// spec 2x-08 §1). Display units only scale the shown value; the stored values
// stay those of spec 21 §8.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio/audio_tables.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "modules/module.h"
#include "ui/notes.h"
#include "ui/screens.h"

namespace ot {
namespace synthui {

namespace {

// The engine's descriptor (its module), or nullptr.
const mod::SynthDescriptor* desc(int32_t engine) { return mod::ModuleHost::instance().synthDescriptor(engine); }

const ParamUi* table(int32_t engine, int* n) {
    const mod::SynthDescriptor* d = desc(engine);
    *n = d ? int(d->paramCount) + int(d->extCount) : 0;   // [OT] extension rows after the Mini's
    return d ? d->ui : nullptr;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
int roundi(float v) { return int(std::lround(v)); }
// 100 % of a kFmtPctMax parameter: the larger end of its stored range.
float pctScale(int32_t engine, int index) {
    const PatchParamRange* r = audio::synthParamRange(engine, index);
    const float m = r ? (std::fabs(r->max) > std::fabs(r->min) ? std::fabs(r->max) : std::fabs(r->min)) : 1.0f;
    return m > 0.0f ? m : 1.0f;
}
// Log axis for the frequency kinds (min may be 0: LFO Ratio 0..100 Hz).
float logLo(float lo) { return lo > 0.1f ? lo : 0.1f; }
}  // namespace

const ParamUi* paramUi(int32_t engine, int index) {
    int n = 0;
    const ParamUi* t = table(engine, &n);
    if (!t || index < 0 || index >= n || !t[index].label) return nullptr;
    return &t[index];
}

int sections(int32_t engine, const Section** out) {
    const mod::SynthDescriptor* d = desc(engine);
    *out = d ? d->sections : nullptr;
    return d ? int(d->sectionCount) : 0;
}

void paramFullName(int32_t engine, int index, char* out, int cap) {
    const ParamUi* u = paramUi(engine, index);
    if (!u) {
        std::snprintf(out, size_t(cap), "None");
        return;
    }
    const Section* sec = nullptr;
    const int ns = sections(engine, &sec);
    for (int s = 0; s < ns; ++s)
        for (int k = 0; k < sec[s].count; ++k)
            if (sec[s].params[k] == index) {
                // Mod rows and the PERC mixer already name their group.
                if (std::strcmp(sec[s].name, "Mod") == 0 || std::strcmp(sec[s].name, "Mixer") == 0 ||
                    std::strcmp(sec[s].name, "FX") == 0)
                    std::snprintf(out, size_t(cap), "%s", u->label);
                else std::snprintf(out, size_t(cap), "%s %s", sec[s].name, u->label);
                return;
            }
    std::snprintf(out, size_t(cap), "%s", u->label);
}

void formatValue(int32_t engine, int index, float v, char* out, int cap) {
    const ParamUi* u = paramUi(engine, index);
    const size_t c = size_t(cap);
    if (!u) {
        std::snprintf(out, c, "%.2f", double(v));
        return;
    }
    switch (u->fmt) {
        case kFmtPct: std::snprintf(out, c, "%d %%", roundi(v * 100.0f)); break;
        case kFmtVal100: std::snprintf(out, c, "%d", roundi(v * 100.0f)); break;
        case kFmtSec: std::snprintf(out, c, "%.2f s", double(v)); break;
        case kFmtHz:
            if (v >= 1000.0f) std::snprintf(out, c, "%.2f kHz", double(v) / 1000.0);
            else std::snprintf(out, c, "%d Hz", roundi(v));
            break;
        case kFmtLfoHz: std::snprintf(out, c, "%.2f Hz", double(v)); break;
        case kFmtEnum: {
            const int i = roundi(v);
            if (u->names && i >= 0 && i < u->nameCount) std::snprintf(out, c, "%s", u->names[i]);
            else std::snprintf(out, c, "%d", i);
            break;
        }
        case kFmtOnOff: std::snprintf(out, c, "%s", v >= 0.5f ? "On" : "Off"); break;
        case kFmtSemi: std::snprintf(out, c, "%d st", roundi(v)); break;
        case kFmtCents: std::snprintf(out, c, "%d c", roundi(v)); break;
        case kFmtInt: std::snprintf(out, c, "%d", roundi(v)); break;
        case kFmtRatio: {   // index 0..47 -> (i + 1) / 4
            const int i = roundi(v);
            std::snprintf(out, c, "%.2f", 0.25 * double((i < 0 ? 0 : (i > 47 ? 47 : i)) + 1));
            break;
        }
        case kFmtMs: std::snprintf(out, c, "%.1f ms", double(v)); break;
        case kFmtDb: std::snprintf(out, c, "%.1f dB", double(v)); break;
        case kFmtNote: {
            char n[4];
            noteName(int8_t(clampf(std::floor(v + 0.5f), 0, 127)), n);
            std::snprintf(out, c, "%s", n);
            break;
        }
        case kFmtPan: {
            const int p = roundi(v * 100.0f);
            if (p == 0) std::snprintf(out, c, "0");
            else std::snprintf(out, c, "%c%d", p < 0 ? 'L' : 'R', p < 0 ? -p : p);
            break;
        }
        case kFmtPctMax: std::snprintf(out, c, "%d %%", roundi(v / pctScale(engine, index) * 100.0f)); break;
        case kFmtMs10: std::snprintf(out, c, "%.2f ms", double(v) * 0.1); break;
        case kFmtPercNote: {
            // Manual p.166: 12..119 = C0..B8 (kick C4 = 60).
            static const char* const kL[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            const int n = int(clampf(std::floor(v + 0.5f), 12.0f, 127.0f));
            std::snprintf(out, c, "%s%d", kL[n % 12], n / 12 - 1);
            break;
        }
        case kFmtBodyWave: {   // names[] = the wave families, 11 positions each (0 % .. 100 %)
            const int max = u->nameCount > 0 ? 11 * u->nameCount - 1 : 0;
            const int i = int(clampf(std::floor(v + 0.5f), 0.0f, float(max)));
            if (u->names && u->nameCount > 0) std::snprintf(out, c, "%s %d %%", u->names[i / 11], (i % 11) * 10);
            else std::snprintf(out, c, "%d", i);
            break;
        }
        default: std::snprintf(out, c, "%.2f", double(v)); break;
    }
}

float valueFrac(int32_t engine, int index, float v) {
    const PatchParamRange* r = audio::synthParamRange(engine, index);
    const ParamUi* u = paramUi(engine, index);
    if (!r || !(r->max > r->min)) return 0.0f;
    if (u && (u->fmt == kFmtHz || u->fmt == kFmtLfoHz)) {
        const float lo = logLo(r->min);
        if (v <= lo) return 0.0f;
        return clampf(std::log(v / lo) / std::log(r->max / lo), 0.0f, 1.0f);
    }
    return clampf((v - r->min) / (r->max - r->min), 0.0f, 1.0f);
}

bool valueBipolar(int32_t engine, int index) {
    const PatchParamRange* r = audio::synthParamRange(engine, index);
    const ParamUi* u = paramUi(engine, index);
    if (!r || (u && u->fmt == kFmtEnum)) return false;
    return r->min < 0.0f && r->max > 0.0f && std::fabs(r->min + r->max) < 1e-3f;
}

float stepValue(int32_t engine, int index, float v, int d) {
    const PatchParamRange* r = audio::synthParamRange(engine, index);
    const ParamUi* u = paramUi(engine, index);
    if (!r || !u || d == 0) return v;
    const float lo = r->min, hi = r->max;
    float nv = v;
    switch (u->fmt) {
        case kFmtEnum:
        case kFmtOnOff:
        case kFmtInt:
        case kFmtRatio:
        case kFmtSemi:
        case kFmtCents:
        case kFmtNote:
        case kFmtPercNote:
        case kFmtBodyWave: nv = std::floor(v + 0.5f) + float(d); break;
        case kFmtPctMax: {   // 1 shown % per detent, on the shown grid
            const float st = pctScale(engine, index) / 100.0f;
            nv = std::round(v / st + float(d)) * st;
            break;
        }
        case kFmtMs10: nv = std::round(v + float(d)); break;   // 0.1 ms
        case kFmtHz:
        case kFmtLfoHz: {
            const float l = logLo(lo);
            const float base = v < l ? l : v;
            nv = base * std::exp2(float(d) / 24.0f);
            if (d < 0 && nv <= l) nv = lo;   // reaches the minimum (0 Hz Ratio)
            break;
        }
        case kFmtSec: {
            // 10 ms steps below 1 s, 50 ms to 3 s, 100 ms above.
            const int dir = d > 0 ? 1 : -1;
            for (int i = 0; i < (d > 0 ? d : -d); ++i) {
                const float a = dir > 0 ? nv : nv - 1e-4f;
                const float st = a < 1.0f ? 0.01f : (a < 3.0f ? 0.05f : 0.1f);
                nv += float(dir) * st;
            }
            nv = std::round(nv * 100.0f) / 100.0f;
            break;
        }
        case kFmtMs: nv = v + 0.1f * float(d); break;
        case kFmtDb: nv = v + 0.5f * float(d); break;
        default: {
            // 1 % of the range per detent (1 unit of the shown x100 value for a 0..1 range).
            const float st = (hi - lo) / ((hi - lo) > 2.5f ? 200.0f : 100.0f);
            nv = v + st * float(d);
            // Snap the shown value to whole units.
            nv = std::round(nv * 100.0f) / 100.0f;
            break;
        }
    }
    if (r->flags & kParamFlagNoRange) return nv;
    return clampf(nv, lo, hi);
}

// Macro targets: the Mini's parameters only ([OT] extension rows skipped).
int targetChoices(int32_t engine) {
    const Section* sec = nullptr;
    const int ns = sections(engine, &sec);
    const int mini = audio::synthParamCount(engine);
    int n = 1;
    for (int s = 0; s < ns; ++s)
        for (int j = 0; j < sec[s].count; ++j) n += sec[s].params[j] < mini;
    return n;
}

int targetChoice(int32_t engine, int k) {
    if (k <= 0) return -1;
    const Section* sec = nullptr;
    const int ns = sections(engine, &sec);
    const int mini = audio::synthParamCount(engine);
    int i = 1;
    for (int s = 0; s < ns; ++s)
        for (int j = 0; j < sec[s].count; ++j) {
            if (sec[s].params[j] >= mini) continue;
            if (i++ == k) return sec[s].params[j];
        }
    return -1;
}

int targetChoiceIndex(int32_t engine, int param) {
    if (param < 0 || param >= audio::synthParamCount(engine)) return 0;
    const Section* sec = nullptr;
    const int ns = sections(engine, &sec);
    int i = 1;
    for (int s = 0; s < ns; ++s)
        for (int j = 0; j < sec[s].count; ++j) {
            if (sec[s].params[j] >= audio::synthParamCount(engine)) continue;
            if (sec[s].params[j] == param) return i;
            ++i;
        }
    return 0;
}

void gainText(float db, char* out, int cap) {
    if (!(db > -96.0f)) std::snprintf(out, size_t(cap), "-inf dB");
    else std::snprintf(out, size_t(cap), "%.2f dB", double(db));
}

void sendText(float v, char* out, int cap) {
    // SEND law (audio_tables.h): 0 -> silent, i -> -40 + 0.4 i dB.
    const int i = int(std::lround(v));
    if (i <= 0) std::snprintf(out, size_t(cap), "-inf dB");
    else std::snprintf(out, size_t(cap), "%.2f dB", -40.0 + 0.4 * double(i > 100 ? 100 : i));
}

}  // namespace synthui
}  // namespace ot
