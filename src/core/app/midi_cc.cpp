#include "app/midi_cc.h"

#include "app/project_fields.h"

#include <cstdio>

namespace ot {
namespace midicc {

bool portAccepted(uint8_t ccIn, bool usb) { return (ccIn & (usb ? 1 : 2)) != 0; }

bool channelAccepted(uint8_t ccInChannel, uint8_t channel) {
    return ccInChannel == 0 || ccInChannel == uint8_t((channel & 15) + 1);
}

// spec 05 appendix C §8 CC list, in the stock table order with the stock
// "Name = CC" names (spec 16 §A.13: 47 rows, parameter order, Tune = 5 first,
// Finetune Amount = 119 last; manual p.265 shows rows 1-13). The automation
// row names past "Attack - Shape LFO" follow that pattern [I].
// MANUAL_AUDIT_2026-10-23 #7.
#define OT_AUTO_ROWS(base, d, nm)                                          \
    {uint8_t(base + 0), InstrParam::AutoType, d, nm " Automation Type"},    \
    {uint8_t(base + 1), InstrParam::AutoAttackShape, d, nm " Attack - Shape LFO"}, \
    {uint8_t(base + 2), InstrParam::AutoDecaySpeed, d, nm " Decay - Speed LFO"},   \
    {uint8_t(base + 3), InstrParam::AutoSustain, d, nm " Sustain"},         \
    {uint8_t(base + 4), InstrParam::AutoRelease, d, nm " Release"},         \
    {uint8_t(base + 5), InstrParam::AutoAmount, d, nm " Amount"}

const CcRow kCcList[kCcListCount] = {
    {5, InstrParam::Tune, 0, "Tune"},
    {3, InstrParam::Finetune, 0, "FineTune"},
    {7, InstrParam::Volume, 0, "Volume"},
    {10, InstrParam::Panning, 0, "Panning"},
    {9, InstrParam::FilterType, 0, "Filter type"},
    {1, InstrParam::Cutoff, 0, "Cutoff"},
    {11, InstrParam::Resonance, 0, "Resonance"},
    {12, InstrParam::Overdrive, 0, "Overdrive"},
    {13, InstrParam::BitDepth, 0, "Bit Depth"},
    {14, InstrParam::ReverbSend, 0, "Reverb Send"},
    {15, InstrParam::DelaySend, 0, "Delay Send"},
    OT_AUTO_ROWS(20, 0, "Volume"),
    OT_AUTO_ROWS(26, 1, "Panning"),
    OT_AUTO_ROWS(83, 2, "Cutoff"),
    OT_AUTO_ROWS(102, 3, "Wavetable Position"),
    OT_AUTO_ROWS(108, 4, "Granular Position"),
    OT_AUTO_ROWS(114, 5, "Finetune"),
};
#undef OT_AUTO_ROWS

const CcRow* findRow(uint8_t cc) {
    for (const CcRow& r : kCcList)
        if (r.cc == cc) return &r;
    return nullptr;
}

int toUnits(uint8_t value, int lo, int hi) {
    const int v = value > 127 ? 127 : value;
    return lo + v * (hi - lo) / 127;
}

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Absolute target through the encoder's clamps (spec 17 §1.6 FW:0x2f648).
// OpenTracker fix (README): the exact target is set; the stock encoder step
// quirk (a Tune delta of exactly +-10 applied as +-12) does not apply.
bool applyDelta(Instrument& in, InstrParam p, int target) {
    if (target == iedit::get(in, p, 0)) return false;
    return iedit::set(in, p, 0, target);
}

// Envelope time (attack/decay/release) through the time-position scale P:
// clamp(p_cur + (P(v) - p_cur), 0, 190), back to milliseconds.
bool applyTime(uint16_t& ms, uint8_t v) {
    const int p = clampi(ccTimePosition(v), 0, 190);
    const uint16_t n = uint16_t(iedit::knobToMs(p));
    if (n == ms) return false;
    ms = n;
    return true;
}

bool applyFloat(float& f, uint8_t v) {
    const float n = float(v) / 127.0f;
    if (f == n) return false;
    f = n;
    return true;
}

bool applyU8(uint8_t& f, int target, int lo, int hi) {
    const uint8_t n = uint8_t(clampi(target, lo, hi));
    if (n == f) return false;
    f = n;
    return true;
}
}  // namespace

int ccTimePosition(uint8_t value) {
    const int v = value > 127 ? 127 : value;
    return v <= 64 ? v * 100 / 64 : 100 + (v - 64) * 90 / 63;
}

bool applyInstrumentCc(Instrument& in, uint8_t cc, uint8_t value) {
    const CcRow* r = findRow(cc);
    if (!r) return false;
    const int v = value > 127 ? 127 : value;
    // spec 17 §1.6: the CC value becomes an absolute target in the
    // parameter's units; the delta to the current value is applied through
    // the encoder routine (clamps, step rules, gating).
    switch (r->param) {
        case InstrParam::Cutoff:
        case InstrParam::Resonance:
            if (!in.filterEnabled) return false;   // only while the filter is on
            return applyDelta(in, r->param, v * 100 / 127);
        case InstrParam::FilterType: {
            const int t = 4 * v / 127;             // 4 -> band-pass (FW:0x2d690)
            return applyDelta(in, r->param, t > 3 ? 3 : t);
        }
        case InstrParam::Finetune: return applyDelta(in, r->param, v * 200 / 127 - 100);
        case InstrParam::Tune: return applyDelta(in, r->param, v * 48 / 127 - 24);
        case InstrParam::BitDepth: return applyDelta(in, r->param, v * 12 / 127 + 4);
        case InstrParam::Volume:
        case InstrParam::Panning:
        case InstrParam::Overdrive:
        case InstrParam::ReverbSend:
        case InstrParam::DelaySend: return applyDelta(in, r->param, v * 100 / 127);
        default: break;
    }
    // Automation block (slot = r->dest): E(s) envelope, L(s) LFO.
    const int d = r->dest < kAutomationCount ? r->dest : 0;
    Envelope& e = in.env[d];
    Lfo& l = in.lfo[d];
    const bool lfo = e.loop != 0;
    switch (r->param) {
        case InstrParam::AutoType: return iedit::set(in, InstrParam::AutoType, d, 2 * v / 127);
        case InstrParam::AutoAttackShape:
            if (lfo) return applyU8(l.shape, 4 * v / 127, 0, 4);
            return e.enabled ? applyTime(e.attackMs, uint8_t(v)) : false;
        case InstrParam::AutoDecaySpeed:
            if (lfo) {
                // Clamp 0..S. OpenTracker fix (README): the stock limit of
                // +-5 per message (FW:0x2e8c8) does not apply.
                const int s = d == 0 ? 23 : 28;
                return applyU8(l.speed, s * v / 127, 0, s);
            }
            return e.enabled ? applyTime(e.decayMs, uint8_t(v)) : false;
        case InstrParam::AutoSustain:
            return e.enabled && !lfo ? applyFloat(e.sustain, uint8_t(v)) : false;
        case InstrParam::AutoRelease:
            // Written whenever the slot is enabled, also with an LFO.
            return e.enabled ? applyTime(e.releaseMs, uint8_t(v)) : false;
        case InstrParam::AutoAmount:
            if (lfo) return applyFloat(l.amount, uint8_t(v));
            return e.enabled ? applyFloat(e.amount, uint8_t(v)) : false;
        default: return false;
    }
}

// spec 17 §1.7 (FW:0x53950): CC 16-19 on the Sample Playback parameters of
// the wavetable (6) and granular (7) play modes.
bool applyPlaybackCc(Instrument& in, uint32_t len, uint8_t cc, uint8_t value) {
    const int v = value > 127 ? 127 : value;
    if (in.playMode == uint8_t(PlayMode::Wavetable)) {
        if (cc == 16) {
            const int count = int(in.wavetableWindowCount);
            if (count <= 0) return false;
            const uint32_t n = uint32_t(clampi(v * (count - 1) / 127, 0, count - 1));
            if (n == in.wavetableCurrentWindow) return false;
            in.wavetableCurrentWindow = n;
            return true;
        }
        if (cc == 17) {
            const uint32_t lim = len < 2048 ? len : 2048;
            int big = 0;
            while (big < 11 && (1u << (big + 1)) <= lim) ++big;
            if (big < 5) return false;
            int old = 5;
            while ((1 << old) < in.wavetableWindowSize && old < 11) ++old;
            const int ne = clampi(5 + v * (big - 5) / 127, 5, big);
            if (ne == old && in.wavetableWindowSize == (1u << ne)) return false;
            uint32_t pos = in.wavetableCurrentWindow;
            pos = ne > old ? pos >> (ne - old) : pos << (old - ne);
            in.wavetableWindowSize = uint16_t(1u << ne);
            in.wavetableWindowCount = len / in.wavetableWindowSize;
            if (in.wavetableWindowCount && pos >= in.wavetableWindowCount) pos = in.wavetableWindowCount - 1;
            in.wavetableCurrentWindow = pos;
            return true;
        }
        return false;
    }
    if (in.playMode == uint8_t(PlayMode::Granular)) {
        switch (cc) {
            case 16: {
                // Applied in encoder steps of 65.
                const int cur = in.granularPosition;
                const int n = clampi(cur + (v * 65535 / 127 - cur) / 65 * 65, 0, 65535);
                if (n == cur) return false;
                in.granularPosition = uint16_t(n);
                return true;
            }
            case 17: {
                const int mx = int(len < 44100 ? len : 44100);
                if (mx < 44) return false;
                const int cur = in.grainLength;
                const int n = clampi(cur + (44 + v * (mx - 44) / 127 - cur) / 11 * 11, 44, mx);
                if (n == cur) return false;
                in.grainLength = uint16_t(n);
                return true;
            }
            case 18: return applyU8(in.grainShape, 2 * v / 127, 0, 2);
            case 19: return applyU8(in.granularLoop, 2 * v / 127, 0, 2);
            default: return false;
        }
    }
    return false;
}

// spec 19 (table 0xa6750) / spec 05 appendix C §7: CC A..F value list,
// 131 entries, no "off" entry. 0..127 = CC number, with a short role name for the standard
// controllers; 128..130 = Program Change, Channel / Poly Aftertouch.
void ccValueName(int value, char* out, int cap) {
    if (cap <= 0) return;
    const int v = value < 0 ? 0 : (value > 130 ? 130 : value);
    if (v == 128) { std::snprintf(out, size_t(cap), "Prg Chg"); return; }
    if (v == 129) { std::snprintf(out, size_t(cap), "Chan AT"); return; }
    if (v == 130) { std::snprintf(out, size_t(cap), "Poly AT"); return; }
    // spec 19 §3: the 38 role suffixes (other numbers are bare).
    const char* role = nullptr;
    switch (v) {
        case 0: role = "Bank"; break;
        case 1: role = "ModWh"; break;
        case 2: role = "Brth"; break;
        case 4: role = "Foot"; break;
        case 5: role = "Glide"; break;
        case 6: role = "Data"; break;
        case 7: role = "ChVol"; break;
        case 8: role = "Balnc"; break;
        case 10: role = "Pan"; break;
        case 11: role = "Exp"; break;
        case 32: role = "Bank"; break;
        case 33: role = "Modw"; break;
        case 34: role = "Brth"; break;
        case 36: role = "Foot"; break;
        case 37: role = "Glid"; break;
        case 38: role = "Data"; break;
        case 39: role = "ChVo"; break;
        case 40: role = "Blnc"; break;
        case 42: role = "Pan"; break;
        case 43: role = "Exp"; break;
        case 64: role = "Sust"; break;
        case 65: role = "Port"; break;
        case 66: role = "Sste"; break;
        case 67: role = "SftP"; break;
        case 68: role = "Lgto"; break;
        case 69: role = "Hld2"; break;
        case 84: role = "Porta"; break;
        case 88: role = "HDVel"; break;
        case 96: role = "Dat+"; break;
        case 97: role = "Dat-"; break;
        case 98: role = "NRPNL"; break;
        case 99: role = "NRPNM"; break;
        case 100: role = "RPNL"; break;
        case 101: role = "RPNM"; break;
        case 120: role = "SOff"; break;
        case 121: role = "Rst"; break;
        case 122: role = "Locl"; break;
        case 123: role = "NOff"; break;
        default: break;
    }
    if (role) std::snprintf(out, size_t(cap), "%d %s", v, role);
    else std::snprintf(out, size_t(cap), "%d", v);
}

// spec 12 §B.4 target table.
uint32_t masterOffset(uint8_t cc) {
    if (cc >= 71 && cc <= 78) return pf::kTrackVolume + uint32_t(cc - 71);
    switch (cc) {
        case 79: return pf::kMixDelay;
        case 80: return pf::kMixReverb;
        case 81: return pf::kMixDry;
        case 82: return pf::kLineInLevel;
        default: return 0;
    }
}

}  // namespace midicc
}  // namespace ot
