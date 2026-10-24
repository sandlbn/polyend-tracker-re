// Step-FX text for the pattern grid (spec 03 §3.5/§3.6, spec 06 §1.2).
#include "ui/fx_text.h"

#include "sequencer/seq_tables.h"

#include <cstdio>
#include <cstring>

#include "model/fx.h"

namespace ot {

namespace {

// LFO rate list, slowest -> fastest (spec 05 automation list 0xa69cc; the
// step-FX tables 0xa13fc (29 entries) and 0xa1474 (24 entries, from "24")
// use the same divisions). Fractions of a step are written "/n".
// The stock 3-character tables are not in the specs; these are
// OpenTracker's own 3-character forms ("3/16" is written ".19").
const char* const kLfoRate[29] = {"128", "96", "64",  "48", "32", "24", "16",  "12",  "8",   "6",
                                  "4",   "3",  "2",   "3/2", "1", "3/4", "/2", "3/8", "/3",  "/4",
                                  ".19", "/6", "/8", "/12", "/16", "/24", "/32", "/48", "/64"};

// MIDI chord interval sets (spec 03 §3.5), root 0 omitted; 10.. = A.., hex-like.
const char* const kChord[30] = {"-",   "27",  "28",  "36",  "37",  "47",  "48",  "57",  "5A",  "5C",
                                "67",  "7C",  "7E",  "279", "27A", "27B", "369", "36A", "378", "379",
                                "37A", "37B", "479", "47A", "47B", "489", "48A", "48B", "57A", "57B"};

// Roll type character after the 'R' symbol: plain, v, V, n, N, random
// (cells read "R  4", "Rv 2", ..., "RR16"; spec 03 §3.5 / 16 §A.8).
const char kRollType[6] = {' ', 'v', 'V', 'n', 'N', 'R'};
const uint8_t kRollHits[8] = {1, 2, 3, 4, 6, 8, 12, 16};
const char kArpDir[3] = {'/', '\\', 'R'};
const char* const kArpRate[11] = {"8", "6", "4", "3", "2", "1", ".2", ".3", ".4", ".6", ".8"};   // manual 7.x
}  // namespace

namespace {
// Indexed by FX id.
const char* const kDescription[kFxCount] = {
    "No effect in this slot. As a value it clears what the slot set before.",   // 0 None
    "Stops a roll or arp started from this column and undoes this slot's earlier change to the voice.",   // 1 Off
    "Delays the note by a fraction of a step. Higher values start it later.",   // 2 Micro-move
    "Repeats the note within the step. The value picks the roll type (plain, fading out, fading in, note variations) and the hits per step.",   // 3 Roll
    "Probability in percent that the step plays.",   // 4 Chance
    "Plays a random note within +/- the value around the step note, kept inside the project scale.",   // 5 Random Note
    "Plays a random instrument within +/- the value around the step instrument.",   // 6 Random Instrument
    "Plays the note at a random volume between 0 and the value.",   // 7 Random Volume
    "Sends the MIDI instrument's CC A with this value.",   // 8 MIDI CC A
    "Sends the MIDI instrument's CC B with this value.",   // 9 MIDI CC B
    "Sends the MIDI instrument's CC C with this value.",   // 10 MIDI CC C
    "Sends the MIDI instrument's CC D with this value.",   // 11 MIDI CC D
    "Sends the MIDI instrument's CC E with this value.",   // 12 MIDI CC E
    "All tracks go back to the start of the pattern after this step.",   // 13 Break Pattern
    "Plays a chord on the step note. The value picks the intervals.",   // 14 MIDI Chord
    "Sets the project tempo (shown in BPM) from this step until playback stops. STP stops playback after the step.",   // 15 Tempo
    "Randomises the value of the other FX slot of this step by up to +/- this value.",   // 16 Random FX Value
    "Sets the swing amount from this step until playback stops.",   // 17 Swing
    "Volume of the note, or the velocity for MIDI instruments.",   // 18 Volume/Velocity
    "Slides the pitch from the previous note to this one. Higher values glide longer.",   // 19 Glide
    "Ends the note after a part of the step: lower values give shorter notes.",   // 20 Gate Length
    "Arpeggiates the chord set in the other FX slot. The value picks the direction (up, down, random) and the rate.",   // 21 Arp
    "Start position in the sample, or the wavetable / granular position.",   // 22 Position
    "Rate of the instrument's volume LFO, in steps, synced to the tempo.",   // 23 Volume LFO
    "Rate of the instrument's panning LFO, in steps, synced to the tempo.",   // 24 Panning LFO
    "Plays this slice of a sliced instrument.",   // 25 Slice
    "Plays the sample forwards (>>>) or backwards (<<<).",   // 26 Reverse Playback
    "Low-pass filter cutoff for this note.",   // 27 Low-pass
    "High-pass filter cutoff for this note.",   // 28 High-pass
    "Band-pass filter cutoff for this note.",   // 29 Band-pass
    "Amount of the track sent to the master delay.",   // 30 Delay Send
    "Stereo position: negative is left, positive is right.",   // 31 Panning
    "Amount of the track sent to the master reverb.",   // 32 Reverb Send
    "Rate of the instrument's finetune LFO, in steps, synced to the tempo.",   // 33 Finetune LFO
    "Fine pitch offset around the note, or pitch bend for MIDI instruments.",   // 34 Micro-tune/Pitchbend
    "Rate of the instrument's filter LFO, in steps, synced to the tempo.",   // 35 Filter LFO
    "Rate of the wavetable / granular position LFO, in steps, synced to the tempo.",   // 36 Position LFO
    "Sends the MIDI instrument's CC F with this value.",   // 37 MIDI CC F
    "Amount of overdrive on the note.",   // 38 Overdrive
    "Reduces the bit depth of the note for a crushed sound.",   // 39 Bit Depth
    "Transposes the note in semitones.",   // 40 Tune
    "Slides the pitch up by value/16 semitones over one step (16 = 1 semitone), then holds.",   // 41 Slide Up (manual p.178)
    "Slides the pitch down by value/16 semitones over one step (16 = 1 semitone), then holds.",   // 42 Slide Down (manual p.179)
};
}  // namespace

const char* fxDescription(uint8_t type) { return type < kFxCount ? kDescription[type] : kDescription[0]; }

const char* fxHelpText(uint8_t type) {
    static char buf[256];
    const char* d = fxDescription(type);
    const FxId id = static_cast<FxId>(type);
    if (type >= kFxCount || id == FxId::None || id == FxId::Off || id == FxId::BreakPattern ||
        seq::fxMin(type) >= seq::fxMax(type)) {
        std::snprintf(buf, sizeof(buf), "%s", d);
        return buf;
    }
    char lo[5], hi[5];
    fxValueText(type, seq::fxMin(type), lo);
    fxValueText(type, seq::fxMax(type), hi);
    const char* a = lo;
    const char* b = hi;
    while (*a == ' ') ++a;
    while (*b == ' ') ++b;
    std::snprintf(buf, sizeof(buf), "%s\n\nFrom %s to %s", d, a, b);
    return buf;
}

void fxValueText(uint8_t type, uint8_t v, char out[5]) {
    // At most 3 characters (spec 16 §A.8: symbol + 3-character value).
    const size_t n = 5;   // every form below is at most 3 characters
    switch (static_cast<FxId>(type)) {
        case FxId::None: std::snprintf(out, n, "---"); return;
        case FxId::Off: std::snprintf(out, n, "OFF"); return;
        case FxId::BreakPattern: std::snprintf(out, n, "%s", v ? "BRK" : "---"); return;
        case FxId::Tempo:
            if (v == 4) std::snprintf(out, n, "STP");
            else std::snprintf(out, n, "%d", 2 * v);
            return;
        case FxId::Roll:
            if (v > 53) break;
            // Type character + 2-digit rate (hits per step; "--" = no roll).
            if (v % 9 == 0) std::snprintf(out, n, "%c--", kRollType[v / 9]);
            else std::snprintf(out, n, "%c%2u", kRollType[v / 9], unsigned(kRollHits[v % 9 - 1]));
            return;
        case FxId::MidiChord:
            if (v >= 30) break;
            std::snprintf(out, n, "%s", kChord[v]);
            return;
        case FxId::Arp:
            if (v == 0) {
                std::snprintf(out, n, "---");
                return;
            }
            if (v > 33) break;
            std::snprintf(out, n, "%c%2s", kArpDir[(v - 1) / 11], kArpRate[(v - 1) % 11]);
            return;
        case FxId::Panning: std::snprintf(out, n, "%d", int(v) - 50); return;
        case FxId::MicroTune: std::snprintf(out, n, "%d", int(v) - 99); return;
        case FxId::Tune: std::snprintf(out, n, "%d", int(v) - 24); return;
        case FxId::Slice: std::snprintf(out, n, "%d", int(v) + 1); return;
        case FxId::ReversePlayback: std::snprintf(out, n, "%s", v ? "<<<" : ">>>"); return;
        case FxId::VolumeLfo: std::snprintf(out, n, "%s", kLfoRate[(v > 23 ? 23 : v) + 5]); return;
        case FxId::PanningLfo:
        case FxId::FinetuneLfo:
        case FxId::FilterLfo:
        case FxId::PositionLfo: std::snprintf(out, n, "%s", kLfoRate[v > 28 ? 28 : v]); return;
        default: break;
    }
    std::snprintf(out, n, "%u", unsigned(v));
}

void fxCellText(uint8_t type, uint8_t value, char out[8]) {
    // spec 16 §A.8: FX symbol + right-aligned 3-character value; an empty
    // slot is "----".
    if (type == 0 || type >= kFxCount) {
        std::snprintf(out, 8, "----");
        return;
    }
    char v[5];
    fxValueText(type, value, v);
    std::snprintf(out, 8, "%c%3.3s", fxInfo(type).symbol, v);
}

}  // namespace ot
