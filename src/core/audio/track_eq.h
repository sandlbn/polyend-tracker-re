// Per-track 4-band parametric EQ (OpenTracker extension; stock 1.9.2 has no
// per-track EQ).
//
// One insert per sample track (tracks 1..8), run on the voice signal right
// after the SVF filter and BEFORE the delay/reverb sends, so the sends carry
// the EQ'd sound like an insert effect on a mixer channel. A stereo sample
// runs the same settings on both channels with separate filter state. Each
// band is an RBJ ("Audio EQ Cookbook") biquad in transposed direct form II,
// float.
//
// Band types follow the useful subset of Ableton's EQ Eight: low cut and high
// cut (12 dB/oct, no gain), low shelf, high shelf, peak (bell) and notch.
// Cut and notch have no gain control (as in EQ Eight); their gain is ignored.
//
// Parameters are kept in integer "stored units" that are also the on-disk
// units (formats/track_eq_file.h): Hz, 0.1 dB and 0.01 of Q. This keeps the
// file format, the UI steps and the tests free of float rounding questions.
#pragma once

#include <cstdint>

#include "audio/audio_tables.h"

namespace ot {
namespace audio {

constexpr int kEqBands = 4;

enum EqType : uint8_t {
    kEqPeak = 0,
    kEqLowShelf,
    kEqHighShelf,
    kEqLowCut,    // 12 dB/oct high-pass
    kEqHighCut,   // 12 dB/oct low-pass
    kEqNotch,
    kEqTypeCount,
};

constexpr uint16_t kEqFreqMin = 20;
constexpr uint16_t kEqFreqMax = 20000;
constexpr int16_t kEqGainMax = 150;     // +-15.0 dB in 0.1 dB units
constexpr uint16_t kEqQMin = 10;        // 0.10
constexpr uint16_t kEqQMax = 1800;      // 18.00

struct EqBand {
    uint8_t type = kEqPeak;
    bool on = false;
    uint16_t freqHz = 1000;      // kEqFreqMin..kEqFreqMax
    int16_t gainDb10 = 0;        // -kEqGainMax..kEqGainMax
    uint16_t q100 = 71;          // kEqQMin..kEqQMax
    uint8_t slope = 0;           // cuts of bands 1 and 4 only: 0 = 12 dB/oct, 1 = 48 dB/oct (eqSlopeAllowed)

    bool operator==(const EqBand& o) const {
        return type == o.type && on == o.on && freqHz == o.freqHz && gainDb10 == o.gainDb10 && q100 == o.q100 &&
               slope == o.slope;
    }
    bool operator!=(const EqBand& o) const { return !(*this == o); }
};

struct TrackEqParams {
    EqBand band[kEqBands];
    bool operator==(const TrackEqParams& o) const {
        for (int i = 0; i < kEqBands; ++i)
            if (band[i] != o.band[i]) return false;
        return true;
    }
    bool operator!=(const TrackEqParams& o) const { return !(*this == o); }
};

// All eight sample tracks.
struct TrackEqSet {
    TrackEqParams track[kVoices];
    TrackEqSet() { setDefault(); }
    void setDefault();   // every band off, factory layout
};

inline bool eqUsesGain(uint8_t type) { return type == kEqPeak || type == kEqLowShelf || type == kEqHighShelf; }
const char* eqTypeName(uint8_t type);
// The 48 dB/oct option exists for the cut types of bands 1 and 4 (index 0 and 3).
inline bool eqSlopeAllowed(int bandIndex, uint8_t type) {
    return (bandIndex == 0 || bandIndex == 3) && (type == kEqLowCut || type == kEqHighCut);
}
// Biquad stages the band needs: 4 for a 48 dB/oct cut (8th-order Butterworth), else 1.
inline int eqStageCount(const EqBand& b) {
    return b.slope && (b.type == kEqLowCut || b.type == kEqHighCut) ? 4 : 1;
}

// Factory layout of band i (all bands off): low cut 80 Hz, bells at 500 Hz
// and 2.5 kHz, high cut 12 kHz.
EqBand eqDefaultBand(int index);
// Clamp every field into its legal range (type to a valid value).
void eqSanitize(EqBand& b);

// Encoder steps. `d` is the signed number of detents; `coarse` is the Shift
// step. Results are clamped; a non-zero `d` always moves the value by at least
// one stored unit unless it is already at the limit.
uint16_t eqStepFreq(uint16_t hz, int d, bool coarse);      // 1/24 octave, coarse 1/6 octave
int16_t eqStepGain(int16_t gainDb10, int d, bool coarse);  // 0.5 dB, coarse 3 dB
uint16_t eqStepQ(uint16_t q100, int d, bool coarse);       // x1.06, coarse x1.5
uint8_t eqStepType(uint8_t type, int d);                   // wraps

// RBJ biquad coefficients of one band, normalised (a0 = 1):
// c[0..4] = b0, b1, b2, a1, a2. The frequency is limited to 0.45 fs.
// Pure function of the stored units (used by the DSP and the UI curve).
// This is the single-stage (12 dB/oct) form; see eqStageCoefficients.
void eqCoefficients(const EqBand& b, float c[5]);
// Coefficients of stage `stage` (0 .. eqStageCount-1) of a band. A 48 dB/oct cut
// is four biquads with the Butterworth Q of an 8th-order filter; the Q control
// scales the resonant (last) stage by sqrt(q / 0.7071), so Q 0.71 = plain
// Butterworth and higher Q adds resonance at the corner.
void eqStageCoefficients(const EqBand& b, int stage, float c[5]);
// True when the band changes the signal at all (cut / notch always; bell and
// shelves only with a non-zero gain).
bool eqBandActive(const EqBand& b);
// Magnitude response in dB of the enabled bands of `p` at frequency `hz`.
float eqResponseDb(const TrackEqParams& p, float hz);

// Runtime processor of one channel of one track.
class TrackEq {
public:
    // Take new parameters (cheap when nothing changed). A band whose
    // coefficients change keeps its filter state; a band that is switched off
    // or turned neutral clears it.
    void set(const TrackEqParams& p);
    // Clear every filter state (voice went silent).
    void reset();
    // True when at least one band processes audio.
    bool active() const { return n_ != 0; }
    // In place, `n` samples (normalised floats).
    void process(float* x, uint32_t n);

private:
    struct Stage {
        float c[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        float z1 = 0.0f, z2 = 0.0f;
        EqBand cfg;
        bool live = false;
    };
    // Band 1 and band 4 can use four stages (48 dB/oct cut), bands 2 and 3 one.
    static constexpr int kStages = 10;
    Stage st_[kStages];
    uint8_t n_ = 0;   // stages with live == true
};

}  // namespace audio
}  // namespace ot
