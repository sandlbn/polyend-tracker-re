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
#include "audio/engines/wtfm.h"
#include "audio/synth_params.h"
#include "formats/patch_pb.h"
#include "ui/notes.h"
#include "ui/screens.h"

namespace ot {
namespace synthui {

namespace {

// --- enum names [MAN] --------------------------------------------------------
const char* const kSubMode[6] = {"-1 Oct Square", "-1 Oct Pulse", "-1 Oct Triangle",
                                 "-2 Oct Square", "-2 Oct Pulse", "-2 Oct Triangle"};
const char* const kAcdFilter[3] = {"LP SVF 12dB", "LP SVF 24dB", "LP RD3"};
const char* const kFatFilter[3] = {"LP MG 24dB", "LP OB 24dB", "LP OB 12dB"};
const char* const kFilter15[15] = {"LP MG 24dB",  "LP OB 24dB",  "LP OB 12dB",   "LP SVF 24dB",   "LP SVF 12dB",
                                   "HP OB 24dB",  "HP OB 12dB",  "HP SVF 24dB",  "HP SVF 12dB",   "BP OB 24dB",
                                   "BP OB 12dB",  "BP SVF 24dB", "BP SVF 12dB",  "Notch SVF 24dB", "Notch SVF 12dB"};
const char* const kWave[6] = {"Triangle", "Sine", "Ramp Up", "Ramp Down", "Square", "Random S&H"};
const char* const kRetrig[3] = {"Free Running", "One Shot", "Note"};
const char* const kGlide[4] = {"Always", "Overlap", "Legato", "Legato Overlap"};
const char* const kModSrc[5] = {"Amp Env", "Env 1 (Fltr)", "Env 2", "LFO 1", "LFO 2"};
const char* const kVapDest[14] = {"Osc1 PWM",   "Osc2 PWM",         "Osc Mix",    "Filter Cutoff", "Amp Env Sustain",
                                  "Flt Env Sustain", "Wave Env Sustain", "Volume", "Osc1 Pitch", "Osc2 Pitch",
                                  "Flt Env Decay", "Noise Volume",  "Osc1 Shape", "Osc2 Shape"};
const char* const kWtfmDest[14] = {"Osc1 Pitch",  "Osc2 Pitch",   "Op1 Shape",       "Op2 Shape",       "Op1 Feedback",
                                   "Op2 Feedback", "Op21 Feedback", "FM",             "Filter Cutoff",   "Amp Env Sustain",
                                   "Flt Env Sustain", "Wave Env Sustain", "Volume",   "Flt Env Decay"};
const char* const kCharacter[9] = {"Smoother", "Sharper", "Wilder", "Add 1", "Add 2", "Add 3", "Add 5", "Add 7", "Add 11"};
const char* const kKickModel[3] = {"Classic8", "Synced8", "Classic9"};
const char* const kTomModel[3] = {"Classic", "Classic+", "Resonant"};
const char* const kSnareModel[5] = {"Classic8", "Classic9", "Classic9+", "Modern", "Modern+"};
const char* const kTwoModel[2] = {"Classic", "FM"};
const char* const kPercModel[5] = {"Clap", "Clap+", "Maracas", "Shaker", "Cowbell"};
const char* const kOutput[3] = {"Main", "Post Overdrive", "Post EQ"};
const char* const kShaper[5] = {"Saturate", "Overdrive", "Overdrive+", "Bitcrush", "Bitcrush+"};
// PERC Body Wave families (manual p.166), 11 positions each (0 % .. 100 %).
const char* const kBodyWave[16] = {"Sat-Sin", "Triangle", "Square", "Square FM", "FM1",    "FM3",
                                   "FM7",     "FM11",     "tzFM1",  "tzFM3",     "tzFM7",  "tzFM11",
                                   "Glitch 1", "Glitch 2", "Glitch 3", "Glitch 4"};

#define E(n) kFmtEnum, n, uint8_t(sizeof(n) / sizeof(n[0]))
#define F(f) f, nullptr, 0

// --- ACD (29) ------------------------------------------------------------------
const ParamUi kAcd[29] = {
    {"Saw Mix", F(kFmtPct)},       {"Square Mix", F(kFmtPct)},   {"Sub Mix", F(kFmtPct)},
    {"Noise Mix", F(kFmtPct)},     {"PW", F(kFmtPct)},           {"Sub Mode", E(kSubMode)},
    {"PW LFO", F(kFmtPct)},        {"PW Env", F(kFmtPct)},       {"Pitch LFO", F(kFmtPctMax)},
    {"Cutoff", F(kFmtHz)},         {"Resonance", F(kFmtPctMax)}, {"Type", E(kAcdFilter)},
    {"Note Track", F(kFmtPct)},    {"LFO Amt", F(kFmtPct)},      {"Env Amt", F(kFmtPct)},
    {"Attack", F(kFmtSec)},        {"Decay", F(kFmtSec)},        {"Sustain", F(kFmtPct)},
    {"Release", F(kFmtSec)},       {"Attack", F(kFmtSec)},       {"Decay", F(kFmtSec)},
    {"Sustain", F(kFmtPct)},       {"Release", F(kFmtSec)},      {"Frequency", F(kFmtLfoHz)},
    {"Waveform", E(kWave)},        {"Retrigger", E(kRetrig)},    {"Glide Mode", E(kGlide)},
    {"Glide Time", F(kFmtSec)},    {"Volume", F(kFmtPct)},
};
const uint8_t kAcdOsc[] = {0, 1, 2, 3, 4, 5, 8, 7, 6};
const uint8_t kAcdFlt[] = {9, 10, 11, 14, 12, 13};
const uint8_t kAcdAmp[] = {15, 16, 17, 18};
const uint8_t kAcdEnv1[] = {19, 20, 21, 22};
const uint8_t kAcdLfo[] = {23, 24, 25};
const uint8_t kAcdVoice[] = {26, 27, 28};

// --- FAT (25) ------------------------------------------------------------------
const ParamUi kFat[25] = {
    {"Fatness", F(kFmtVal100)},    {"Brightness", F(kFmtVal100)}, {"LFO Amt", F(kFmtPct)},
    {"Timbre", F(kFmtVal100)},     {"Noise", F(kFmtPct)},         {"Reset", F(kFmtOnOff)},
    {"Type", E(kFatFilter)},       {"Cutoff", F(kFmtHz)},         {"Resonance", F(kFmtPctMax)},
    {"LFO Amt", F(kFmtPct)},       {"Env Amt", F(kFmtPct)},       {"Attack", F(kFmtSec)},
    {"Decay", F(kFmtSec)},         {"Sustain", F(kFmtPct)},       {"Release", F(kFmtSec)},
    {"Attack", F(kFmtSec)},        {"Decay", F(kFmtSec)},         {"Sustain", F(kFmtPct)},
    {"Release", F(kFmtSec)},       {"Frequency", F(kFmtLfoHz)},   {"Waveform", E(kWave)},
    {"Retrigger", E(kRetrig)},     {"Glide Mode", E(kGlide)},     {"Glide Time", F(kFmtSec)},
    {"Volume", F(kFmtPct)},
};
const uint8_t kFatOsc[] = {0, 1, 3, 2, 4, 5};
const uint8_t kFatFlt[] = {7, 8, 6, 10, 9};
const uint8_t kFatAmp[] = {11, 12, 13, 14};
const uint8_t kFatEnv1[] = {15, 16, 17, 18};
const uint8_t kFatLfo[] = {19, 20, 21};
const uint8_t kFatVoice[] = {22, 23, 24};

// --- VAP (60) ------------------------------------------------------------------
#define MODROW(k) {"Src " #k, E(kModSrc)}, {"Dest " #k, E(kVapDest)}, {"Amt " #k, F(kFmtPctMax)}
const ParamUi kVap[60] = {
    {"Shape 1", F(kFmtVal100)},   {"Shape 2", F(kFmtVal100)},  {"PW 1", F(kFmtVal100)},
    {"PW 2", F(kFmtVal100)},      {"Mix", F(kFmtPct)},         {"Tune 1", F(kFmtSemi)},
    {"Tune 2", F(kFmtSemi)},      {"Detune", F(kFmtCents)},    {"Finetune", F(kFmtCents)},
    {"Reset 1", F(kFmtOnOff)},    {"Reset 2", F(kFmtOnOff)},   {"Type", E(kFilter15)},
    {"Cutoff", F(kFmtHz)},        {"Resonance", F(kFmtPctMax)}, {"Env Amt", F(kFmtPct)},
    {"Attack", F(kFmtSec)},       {"Decay", F(kFmtSec)},       {"Sustain", F(kFmtPct)},
    {"Release", F(kFmtSec)},      {"Attack", F(kFmtSec)},      {"Decay", F(kFmtSec)},
    {"Sustain", F(kFmtPct)},      {"Release", F(kFmtSec)},     {"Attack", F(kFmtSec)},
    {"Decay", F(kFmtSec)},        {"Sustain", F(kFmtPct)},     {"Release", F(kFmtSec)},
    {"Ratio", F(kFmtLfoHz)},      {"Waveform", E(kWave)},      {"Retrigger", E(kRetrig)},
    {"Sync", F(kFmtOnOff)},       {"Ratio", F(kFmtLfoHz)},     {"Waveform", E(kWave)},
    {"Retrigger", E(kRetrig)},    {"Sync", F(kFmtOnOff)},      {"Glide Mode", E(kGlide)},
    {"Glide Time", F(kFmtSec)},   {"Note Track", F(kFmtPct)},  {"Mix Env Amt", F(kFmtPct)},
    {"Volume", F(kFmtPct)},       {"Sync", F(kFmtOnOff)},      {"Noise", F(kFmtPct)},
    MODROW(1), MODROW(2), MODROW(3), MODROW(4), MODROW(5), MODROW(6),
};
#undef MODROW
const uint8_t kVapOsc[] = {4, 0, 1, 2, 3, 5, 6, 8, 7, 41, 40, 9, 10, 38};
const uint8_t kVapFlt[] = {12, 13, 11, 14, 37};
const uint8_t kVapAmp[] = {15, 16, 17, 18};
const uint8_t kVapEnv1[] = {19, 20, 21, 22};
const uint8_t kVapEnv2[] = {23, 24, 25, 26};
const uint8_t kVapLfo1[] = {27, 28, 29, 30};
const uint8_t kVapLfo2[] = {31, 32, 33, 34};
const uint8_t kVapMod[] = {42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59};
const uint8_t kVapVoice[] = {35, 36, 39};

// --- WTFM (63) -----------------------------------------------------------------
#define MODROW(k) {"Src " #k, E(kModSrc)}, {"Dest " #k, E(kWtfmDest)}, {"Amt " #k, F(kFmtPctMax)}
const ParamUi kWtfm[63] = {
    {"Ratio 1", F(kFmtRatio)},     {"Ratio 2", F(kFmtRatio)},     {"Finetune 1", F(kFmtCents)},
    {"Finetune 2", F(kFmtCents)},  {"Shape 1", F(kFmtVal100)},    {"Shape 2", F(kFmtVal100)},
    {nullptr, F(kFmtVal100)},      {nullptr, F(kFmtVal100)},      {"Oversampling", F(kFmtOnOff)},
    {"Character 1", E(kCharacter)}, {"Character 2", E(kCharacter)}, {"FM", F(kFmtPct)},
    {"Feedback 1", F(kFmtPct)},    {"Feedback 2", F(kFmtPct)},    {"Feedback 2>1", F(kFmtPct)},
    {"Type", E(kFilter15)},        {"Cutoff", F(kFmtHz)},         {"Resonance", F(kFmtPctMax)},
    {"Env Amt", F(kFmtPct)},       {"Note Track", F(kFmtPct)},    {"Attack", F(kFmtSec)},
    {"Decay", F(kFmtSec)},         {"Sustain", F(kFmtPct)},       {"Release", F(kFmtSec)},
    {"Attack", F(kFmtSec)},        {"Decay", F(kFmtSec)},         {"Sustain", F(kFmtPct)},
    {"Release", F(kFmtSec)},       {"Attack", F(kFmtSec)},        {"Decay", F(kFmtSec)},
    {"Sustain", F(kFmtPct)},       {"Release", F(kFmtSec)},       {"Ratio", F(kFmtLfoHz)},
    {"Waveform", E(kWave)},        {"Retrigger", E(kRetrig)},     {"Sync", F(kFmtOnOff)},
    {"Ratio", F(kFmtLfoHz)},       {"Waveform", E(kWave)},        {"Retrigger", E(kRetrig)},
    {"Sync", F(kFmtOnOff)},        {"Glide Mode", E(kGlide)},     {"Glide Time", F(kFmtSec)},
    {"Volume", F(kFmtPct)},
    MODROW(1), MODROW(2), MODROW(3), MODROW(4), MODROW(5), MODROW(6),
    {"Reset 1", F(kFmtOnOff)},     {"Reset 2", F(kFmtOnOff)},
};
#undef MODROW
const uint8_t kWtfmOsc[] = {11, 0, 1, 4, 5, 9, 10, 12, 13, 14, 2, 3, 61, 62, 8};
const uint8_t kWtfmFlt[] = {16, 17, 15, 18, 19};
const uint8_t kWtfmAmp[] = {20, 21, 22, 23};
const uint8_t kWtfmEnv1[] = {24, 25, 26, 27};
const uint8_t kWtfmEnv2[] = {28, 29, 30, 31};
const uint8_t kWtfmLfo1[] = {32, 33, 34, 35};
const uint8_t kWtfmLfo2[] = {36, 37, 38, 39};
const uint8_t kWtfmMod[] = {43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60};
const uint8_t kWtfmVoice[] = {40, 41, 42};

// --- PERC (84) -----------------------------------------------------------------
#define MIXROW(d) {d " Level", F(kFmtPct)}, {d " Pan", F(kFmtPan)}, {d " Delay", F(kFmtVal100)}, \
                  {d " Reverb", F(kFmtVal100)}, {d " Output", E(kOutput)}
const ParamUi kPerc[84] = {
    // Kick 0..8
    {"Body Wave", F(kFmtBodyWave)}, {"Pitch", F(kFmtPercNote)},    {"Body Attack", F(kFmtMs10)},
    {"Body Decay", F(kFmtSec)},     {"Trans Timbre", F(kFmtVal100)}, {"Trans Level", F(kFmtPct)},
    {"Mod Decay", F(kFmtSec)},      {"Mod Amount", F(kFmtVal100)}, {"Model", E(kKickModel)},
    // Tom 9..17: 12 holds the transient level (0..1, 0-100 %), 13 the
    // transient timbre (0..2, 0-200): the manual's ranges (p.166) match the
    // stored ranges that way round.
    {"Body Wave", F(kFmtBodyWave)}, {"Body Attack", F(kFmtMs10)},  {"Body Decay", F(kFmtSec)},
    {"Trans Level", F(kFmtPct)},    {"Trans Timbre", F(kFmtVal100)}, {"Mod Decay", F(kFmtSec)},
    {"Pitch Mod", F(kFmtVal100)},   {"Wave Mod", F(kFmtVal100)},   {"Model", E(kTomModel)},
    // Snare 18..26
    {"Body Wave", F(kFmtBodyWave)}, {"Pitch", F(kFmtPercNote)},    {"Tone", F(kFmtPct)},
    {"Model", E(kSnareModel)},      {"Body Decay", F(kFmtSec)},    {"Snap Attack", F(kFmtMs10)},
    {"Snap Decay", F(kFmtSec)},     {"Body/Snap Mix", F(kFmtVal100)}, {"Snap Timbre", F(kFmtPct)},
    // Hi-hat 27..35
    {"Decay Shape", F(kFmtPct)},    {"Closed Decay", F(kFmtSec)},  {"Open Decay", F(kFmtSec)},
    {"HPF Cutoff", F(kFmtHz)},      {"LPF Cutoff", F(kFmtHz)},     {"Tone Pitch", F(kFmtHz)},
    {"Tone/Noise Mix", F(kFmtVal100)}, {"Timbre", F(kFmtInt)},     {"Model", E(kTwoModel)},
    // Cymbal 36..44
    {"Body Filter", F(kFmtHz)},     {"Impact Filter", F(kFmtHz)},  {"Body Decay", F(kFmtSec)},
    {"Impact Decay", F(kFmtSec)},   {"Tone Pitch", F(kFmtHz)},     {"Tone/Noise Mix", F(kFmtVal100)},
    {"Impact/Body Mix", F(kFmtVal100)}, {"Model", E(kTwoModel)},   {"Timbre", F(kFmtInt)},
    // Perc 45..48
    {"Model", E(kPercModel)},       {"Variation", F(kFmtVal100)},  {"Timbre", F(kFmtVal100)},
    {"Decay", F(kFmtSec)},
    // Mixer 49..78
    MIXROW("Kick"), MIXROW("Tom"), MIXROW("Snare"), MIXROW("Hi Hat"), MIXROW("Cymbal"), MIXROW("Perc"),
    // FX 79..83
    {"Waveshaper Level", F(kFmtVal100)}, {"Waveshaper Type", E(kShaper)}, {"EQ Low 80Hz", F(kFmtDb)},
    {"EQ Mid 2.5kHz", F(kFmtDb)},   {"EQ High 12kHz", F(kFmtDb)},
};
#undef MIXROW
const uint8_t kPercKick[] = {8, 1, 0, 2, 3, 6, 7, 4, 5};
const uint8_t kPercTom[] = {17, 9, 10, 11, 14, 15, 16, 13, 12};
const uint8_t kPercSnare[] = {21, 19, 20, 18, 22, 23, 24, 26, 25};
const uint8_t kPercHat[] = {35, 34, 28, 29, 27, 30, 31, 32, 33};
const uint8_t kPercCym[] = {43, 44, 36, 37, 38, 39, 40, 41, 42};
const uint8_t kPercPerc[] = {45, 46, 47, 48};
const uint8_t kPercMix[] = {49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
                            64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78};
const uint8_t kPercFx[] = {80, 79, 81, 82, 83};   // Type, Level (manual p.169)
#undef E
#undef F

#define S(name, longName, arr) {name, longName, arr, uint8_t(sizeof(arr)), -1}
#define MACROS                                                                                      \
    {"M1", "Macro 1", nullptr, 0, 0}, {"M2", "Macro 2", nullptr, 0, 1}, {"M3", "Macro 3", nullptr, 0, 2}, \
        {"M4", "Macro 4", nullptr, 0, 3}, {"M5", "Macro 5", nullptr, 0, 4}, {"M6", "Macro 6", nullptr, 0, 5}, \
        {"M7", "Macro 7", nullptr, 0, 6}
const Section kAcdSec[] = {S("Osc", "Oscillator", kAcdOsc), S("Filter", "Filter", kAcdFlt),
                           S("Amp Env", "Amp Env", kAcdAmp), S("Env 1", "Mod Env", kAcdEnv1),
                           S("LFO 1", "LFO", kAcdLfo),       S("Voice", "Voice", kAcdVoice), MACROS};
const Section kFatSec[] = {S("Osc", "Oscillator", kFatOsc), S("Filter", "Filter", kFatFlt),
                           S("Amp Env", "Amp Env", kFatAmp), S("Env 1", "Filter Env", kFatEnv1),
                           S("LFO 1", "LFO", kFatLfo),       S("Voice", "Voice", kFatVoice), MACROS};
const Section kVapSec[] = {S("Osc", "Oscillators", kVapOsc), S("Filter", "Filter", kVapFlt),
                           S("Amp Env", "Amp Env", kVapAmp),  S("Env 1", "Envelope 1", kVapEnv1),
                           S("Env 2", "Envelope 2", kVapEnv2), S("LFO 1", "LFO 1", kVapLfo1),
                           S("LFO 2", "LFO 2", kVapLfo2),     S("Mod", "Modulation", kVapMod),
                           S("Voice", "Voice", kVapVoice),    MACROS};
const Section kWtfmSec[] = {S("Osc", "Oscillator", kWtfmOsc), S("Filter", "Filter", kWtfmFlt),
                            S("Amp Env", "Amp Env", kWtfmAmp), S("Env 1", "Envelope 1", kWtfmEnv1),
                            S("Env 2", "Envelope 2", kWtfmEnv2), S("LFO 1", "LFO 1", kWtfmLfo1),
                            S("LFO 2", "LFO 2", kWtfmLfo2),   S("Mod", "Modulation", kWtfmMod),
                            S("Voice", "Voice", kWtfmVoice),  MACROS};
const Section kPercSec[] = {S("Kick", "Kick", kPercKick),     S("Tom", "Toms", kPercTom),
                            S("Snare", "Snare", kPercSnare),  S("Hi Hat", "Hi Hat", kPercHat),
                            S("Cymbal", "Cymbal", kPercCym),  S("Perc", "Percussion", kPercPerc),
                            S("Mixer", "Mixer", kPercMix),    S("FX", "FX", kPercFx), MACROS};
#undef S
#undef MACROS

template <class T, int N>
constexpr int countOf(const T (&)[N]) { return N; }

const ParamUi* table(int32_t engine, int* n) {
    switch (engine) {
        case kSynthEngineAcd: *n = countOf(kAcd); return kAcd;
        case kSynthEngineFat: *n = countOf(kFat); return kFat;
        case kSynthEngineVap: *n = countOf(kVap); return kVap;
        case kSynthEngineWtfm: *n = countOf(kWtfm); return kWtfm;
        case kSynthEnginePerc: *n = countOf(kPerc); return kPerc;
        default: *n = 0; return nullptr;
    }
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
    switch (engine) {
        case kSynthEngineAcd: *out = kAcdSec; return countOf(kAcdSec);
        case kSynthEngineFat: *out = kFatSec; return countOf(kFatSec);
        case kSynthEngineVap: *out = kVapSec; return countOf(kVapSec);
        case kSynthEngineWtfm: *out = kWtfmSec; return countOf(kWtfmSec);
        case kSynthEnginePerc: *out = kPercSec; return countOf(kPercSec);
        default: *out = nullptr; return 0;
    }
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
        case kFmtRatio: std::snprintf(out, c, "%.2f", double(audio::wtfm::ratioOf(roundi(v)))); break;
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
        case kFmtBodyWave: {
            const int i = int(clampf(std::floor(v + 0.5f), 0.0f, 175.0f));
            std::snprintf(out, c, "%s %d %%", kBodyWave[i / 11], (i % 11) * 10);
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

int targetChoices(int32_t engine) {
    const Section* sec = nullptr;
    const int ns = sections(engine, &sec);
    int n = 1;
    for (int s = 0; s < ns; ++s) n += sec[s].count;
    return n;
}

int targetChoice(int32_t engine, int k) {
    if (k <= 0) return -1;
    const Section* sec = nullptr;
    const int ns = sections(engine, &sec);
    int i = 1;
    for (int s = 0; s < ns; ++s)
        for (int j = 0; j < sec[s].count; ++j, ++i)
            if (i == k) return sec[s].params[j];
    return -1;
}

int targetChoiceIndex(int32_t engine, int param) {
    if (param < 0) return 0;
    const Section* sec = nullptr;
    const int ns = sections(engine, &sec);
    int i = 1;
    for (int s = 0; s < ns; ++s)
        for (int j = 0; j < sec[s].count; ++j, ++i)
            if (sec[s].params[j] == param) return i;
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
