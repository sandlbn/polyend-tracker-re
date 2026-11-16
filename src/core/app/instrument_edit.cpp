#include "app/instrument_edit.h"

#include <cmath>


namespace ot {
namespace iedit {

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int toUnits(float v, float unit) { return int(std::lround(v / unit)); }
bool isAuto(InstrParam p) { return p >= InstrParam::AutoType; }
int dclamp(int dest) { return clampi(dest, 0, kAutomationCount - 1); }

const Instrument& defaults() { return defaultInstrument(); }
}  // namespace

// spec 05 appendix C §6: p 0..100 -> 10 ms steps, 100..190 -> 100 ms steps.
int msToKnob(int ms) { return ms <= 1000 ? ms / 10 : clampi(100 + (ms - 1000) / 100, 100, 190); }
int knobToMs(int p) { return p <= 100 ? p * 10 : 1000 + (p - 100) * 100; }

int autoType(const Instrument& in, int dest) {
    const Envelope& e = in.env[dclamp(dest)];
    return e.enabled ? (e.loop ? 2 : 1) : 0;
}

Range range(const Instrument& in, InstrParam p, int dest) {
    (void)in;
    switch (p) {
        case InstrParam::Volume: return {0, 100};
        case InstrParam::Panning: return {0, 100};
        case InstrParam::Tune: return {-24, 24};
        case InstrParam::Finetune: return {-100, 100};
        case InstrParam::FilterType: return {0, 3};
        case InstrParam::Cutoff: return {0, 100};
        case InstrParam::Resonance: return {0, 100};
        case InstrParam::Overdrive: return {0, 100};
        case InstrParam::BitDepth: return {4, 16};
        case InstrParam::ReverbSend: return {0, 100};
        case InstrParam::DelaySend: return {0, 100};
        case InstrParam::AutoType: return {0, 2};
        case InstrParam::AutoAttackShape:
            return autoType(in, dest) == 2 ? Range{0, 4} : Range{0, 190};
        case InstrParam::AutoDecaySpeed:
            // Volume LFO speed is stored 0..23 (shown as list index +5), §6.
            if (autoType(in, dest) == 2) return dclamp(dest) == 0 ? Range{0, 23} : Range{0, 28};
            return {0, 190};
        case InstrParam::AutoSustain: return {0, 100};
        case InstrParam::AutoRelease: return {0, 190};
        case InstrParam::AutoAmount: return {0, 100};
    }
    return {0, 0};
}

int get(const Instrument& in, InstrParam p, int dest) {
    dest = dclamp(dest);
    const Envelope& e = in.env[dest];
    const Lfo& l = in.lfo[dest];
    const bool lfo = autoType(in, dest) == 2;
    switch (p) {
        case InstrParam::Volume: return in.volume;
        case InstrParam::Panning: return in.panning;
        case InstrParam::Tune: return in.tune;
        case InstrParam::Finetune: return in.finetune;
        case InstrParam::FilterType: return in.filterEnabled ? clampi(in.filterType + 1, 1, 3) : 0;
        case InstrParam::Cutoff: return toUnits(in.cutoff, 0.01f);
        case InstrParam::Resonance: return toUnits(in.resonance, 0.043f);
        case InstrParam::Overdrive: return in.overdrive;
        case InstrParam::BitDepth: return in.bitDepth;
        case InstrParam::ReverbSend: return in.reverbSend;
        case InstrParam::DelaySend: return in.delaySend;
        case InstrParam::AutoType: return autoType(in, dest);
        case InstrParam::AutoAttackShape: return lfo ? l.shape : msToKnob(e.attackMs);
        case InstrParam::AutoDecaySpeed: return lfo ? l.speed : msToKnob(e.decayMs);
        case InstrParam::AutoSustain: return toUnits(e.sustain, 0.01f);
        case InstrParam::AutoRelease: return msToKnob(e.releaseMs);
        case InstrParam::AutoAmount: return toUnits(lfo ? l.amount : e.amount, 0.01f);
    }
    return 0;
}

bool editable(const Instrument& in, InstrParam p, int dest) {
    if (p == InstrParam::Cutoff || p == InstrParam::Resonance) return in.filterEnabled != 0;
    if (!isAuto(p) || p == InstrParam::AutoType) return true;
    const int t = autoType(in, dest);
    if (t == 0) return false;
    if (t == 2) return p == InstrParam::AutoAttackShape || p == InstrParam::AutoDecaySpeed || p == InstrParam::AutoAmount;
    return true;
}

bool set(Instrument& in, InstrParam p, int dest, int units) {
    if (!editable(in, p, dest)) return false;
    dest = dclamp(dest);
    const Range r = range(in, p, dest);
    const int v = clampi(units, r.lo, r.hi);
    if (v == get(in, p, dest)) return false;
    Envelope& e = in.env[dest];
    Lfo& l = in.lfo[dest];
    const bool lfo = autoType(in, dest) == 2;
    switch (p) {
        case InstrParam::Volume: in.volume = uint8_t(v); break;
        case InstrParam::Panning: in.panning = int16_t(v); break;
        case InstrParam::Tune: in.tune = int8_t(v); break;
        case InstrParam::Finetune: in.finetune = int8_t(v); break;
        case InstrParam::FilterType:
            // §4 col4: 0 disabled (enable 0, type 0), 1 LP, 2 HP, 3 BP.
            in.filterEnabled = v != 0;
            in.filterType = uint8_t(v ? v - 1 : 0);
            break;
        case InstrParam::Cutoff: in.cutoff = float(v) * 0.01f; break;
        case InstrParam::Resonance: in.resonance = v >= 100 ? 4.3f : float(v) * 0.043f; break;
        case InstrParam::Overdrive: in.overdrive = uint8_t(v); break;
        case InstrParam::BitDepth: in.bitDepth = uint8_t(v); break;
        case InstrParam::ReverbSend: in.reverbSend = uint8_t(v); break;
        case InstrParam::DelaySend: in.delaySend = uint8_t(v); break;
        case InstrParam::AutoType:
            // §6 TYPE: enabled + isLFO.
            e.enabled = v != 0;
            e.loop = v == 2;
            if (v == 2 && l.speed > range(in, InstrParam::AutoDecaySpeed, dest).hi)
                l.speed = uint8_t(range(in, InstrParam::AutoDecaySpeed, dest).hi);
            break;
        case InstrParam::AutoAttackShape:
            if (lfo) l.shape = uint8_t(v);
            else e.attackMs = uint16_t(knobToMs(v));
            break;
        case InstrParam::AutoDecaySpeed:
            if (lfo) l.speed = uint8_t(v);
            else e.decayMs = uint16_t(knobToMs(v));
            break;
        case InstrParam::AutoSustain: e.sustain = float(v) * 0.01f; break;
        case InstrParam::AutoRelease: e.releaseMs = uint16_t(knobToMs(v)); break;
        case InstrParam::AutoAmount:
            if (lfo) l.amount = float(v) * 0.01f;
            else e.amount = float(v) * 0.01f;
            break;
    }
    return true;
}

bool step(Instrument& in, InstrParam p, int dest, int delta, bool wrap) {
    if (!editable(in, p, dest)) return false;
    const Range r = range(in, p, dest);
    int v = get(in, p, dest) + delta;
    if (wrap) {
        if (v > r.hi) v = r.lo;
        else if (v < r.lo) v = r.hi;
    }
    return set(in, p, dest, v);
}

int defaultValue(InstrParam p, int dest) { return get(defaults(), p, dest); }

}  // namespace iedit
}  // namespace ot
