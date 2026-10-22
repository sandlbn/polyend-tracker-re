// MIDI Control Change input (spec 12 §B, spec 05 §2.5 and appendix C §8).
//
// Parsing happens in the main loop (App::serviceMidi), never in the audio
// block. An accepted CC goes, in this order, to:
//   1. the Performance handler (CC 41-48, 51-62), only while the
//      Performance screen is active (spec 12 §B.1);
//   2. the master-level handler (CC 71-82), on any screen (§B.4);
//   3. the instrument CC map (appendix C §8, spec 17 §1.6), on any screen
//      and in any play state (spec 17 §1.6 correction to appendix C §8);
//   4. the Sample Playback map CC 16-19 (spec 17 §1.7).
#pragma once

#include <cstdint>

#include "app/instrument_edit.h"
#include "model/instrument.h"

namespace ot {

// Performance CC 41-48 / 51-62 (spec 12 §B.2, §B.3). The Performance screen
// (task B4) implements this; the default does nothing.
//
// Contract for implementers (spec 12):
// - perfTrackPatternCc(t, v): queue pattern v + 1 (1..128) on track t at
//   that track's loop end, store it in the project per-track pattern table
//   and mark the project dirty (§B.3).
// - perfSlotCc(s, v): latch v for slot s; at the next screen update (>= 50
//   ms) set the slot's live row, live = min + (max - min) * v / 127 from the
//   slot's FX range, and apply it (§B.2). Global FX (Pattern length) must be
//   applied too: OpenTracker fixes the stock quirk that only shows the value
//   (record that in reimpl/README.md when it is implemented). Never store an
//   encoder edit into "row 4" (stock out-of-slot write).
class PerformanceCcSink {
public:
    virtual ~PerformanceCcSink() = default;
    virtual void perfTrackPatternCc(uint8_t track, uint8_t value) { (void)track; (void)value; }
    virtual void perfSlotCc(uint8_t slot, uint8_t value) { (void)slot; (void)value; }
    // [I] CC 63: scene crossfader position 0..127, handled at the next update.
    virtual void perfSceneCc(uint8_t value) { (void)value; }
    // Main-loop tick while another screen is shown: the Performance FX may
    // stay live there (spec 16 §A.15).
    virtual void perfBackground(uint32_t now) { (void)now; }
    // A screen key pressed while another screen is shown (spec 16 §A.15: the
    // stock handler is bound to every screen key while perf mode is on).
    virtual void perfScreenKey(uint8_t id) { (void)id; }
    // Offline render: FX kept live on another screen are reverted first.
    virtual void perfRevertKept() {}
};

namespace midicc {

// ---- reception (spec 12 §B.1) ---------------------------------------------
// Config "CC in": 0 off, 1 USB, 2 DIN (MIDI jack), 3 both.
bool portAccepted(uint8_t ccIn, bool usb);
// Config "CC in channel": 0 all, 1..16. `channel` is the status nibble 0..15.
bool channelAccepted(uint8_t ccInChannel, uint8_t channel);

// ---- instrument CC map (appendix C §8) --------------------------------------
struct CcRow {
    uint8_t cc;
    InstrParam param;
    uint8_t dest;          // automation destination 0..5 (automation rows)
    const char* label;     // reference-list text
};
constexpr int kCcListCount = 47;   // 11 + 6 x 6 rows
extern const CcRow kCcList[kCcListCount];
// Row for CC number `cc`, or nullptr when the CC is not mapped.
const CcRow* findRow(uint8_t cc);
// Absolute CC value 0..127 -> knob units of the parameter's range:
// lo + v * (hi - lo) / 127 (appendix C §8: volume v*100/127, finetune
// v*200/127-100, tune v*48/127-24, cutoff v*100/127).
int toUnits(uint8_t value, int lo, int hi);
// Apply one CC to a sample instrument (spec 17 §1.6): absolute target in
// the parameter's units through the encoder clamps (envelope times on the
// time-position scale). OpenTracker fix: the exact target is set, without
// the stock step limits (Tune +-10 -> +-12, LFO speed +-5 per message).
// Returns true when a parameter changed.
bool applyInstrumentCc(Instrument& in, uint8_t cc, uint8_t value);
// Time-position p 0..190 of a CC value (spec 17 §1.6 scale P).
int ccTimePosition(uint8_t value);
// CC 16-19 on the wavetable / granular parameters (spec 17 §1.7); `len` is
// the sample length in frames. Returns true when a parameter changed.
inline bool isPlaybackCc(uint8_t cc) { return cc >= 16 && cc <= 19; }
bool applyPlaybackCc(Instrument& in, uint32_t len, uint8_t cc, uint8_t value);
// CC A..F value label (spec 05 appendix C §7, 131 entries 0..130).
constexpr int kCcValueCount = 131;
void ccValueName(int value, char* out, int cap);

// ---- master levels (spec 12 §B.4) -------------------------------------------
// Project values offset written by CC 71..82, or 0 when `cc` is not a
// master CC.
uint32_t masterOffset(uint8_t cc);
// Level 0..100 = v * 100 / 127.
inline uint8_t masterLevel(uint8_t v) { return uint8_t((v > 127 ? 127 : v) * 100 / 127); }

// Performance CC ranges (spec 12 §B.2, §B.3).
inline bool isPerfTrackCc(uint8_t cc) { return cc >= 41 && cc <= 48; }
inline bool isPerfSlotCc(uint8_t cc) { return cc >= 51 && cc <= 62; }
// [I] Performance scene crossfader (not in stock): 0 = scene A, 127 = scene B.
constexpr uint8_t kPerfSceneCc = 63;

}  // namespace midicc
}  // namespace ot
