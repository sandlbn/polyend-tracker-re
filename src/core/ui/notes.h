// Note naming, scales and the pad -> note map (spec 05 §4.1, spec 06 §1.1).
#pragma once

#include <cstdint>

namespace ot {

constexpr int kScaleCount = 39;

// Note text, at most 3 chars + NUL: "C5", "C#5" (no separator, stock); specials "FAD", "CUT", "OFF"; empty "---".
// Note 0 = C0, so note 60 = C5 (spec 06 §1.1).
void noteName(int8_t note, char out[4]);

uint16_t scaleMask(uint8_t scale);
const char* scaleName(uint8_t scale);   // stock names (spec 16 §B.2)
bool isInScale(int note, int root, uint8_t scale);
// Root note option 0..36 = C1..C4; the root pitch class is root % 12.

// Pad -> note table (spec 05 §4.1). Physical pad index = row*12 + col, row 0 = top.
class PadNotes {
public:
    // [I] a pad that plays nothing (a gap of the piano layout).
    static constexpr uint8_t kNoNote = 0xFF;
    // piano: Config > Pad layout "Piano" (layout is then unused): per row
    // pair the white keys along the lower row (C D E F G A B C D E F G) and
    // the black keys above them, each over the white key above it (C# over
    // D; gaps over C and F); the upper pair an octave above the lower one, which starts at the
    // root note's octave.
    void build(uint8_t scale, uint8_t layout, uint8_t root, bool piano = false);
    // raw: chromatic pad + 24 (spec 05 §4.1 getNote raw).
    uint8_t note(uint8_t physicalPad, bool raw = false) const;
    // Physical pad producing `note`, or -1.
    int padsForNote(int note, uint8_t* pads, int cap) const;
    // [I] the piano layout's key lights (levels 0..31, physical pads):
    // the scale's root brightest, white keys `dim`, black keys dimmer, keys
    // outside the scale barely lit, gaps dark (lv untouched there). False
    // (lv untouched) when the table isn't the piano layout. The levels are
    // built with the table (build) and again only for a new dim / full.
    bool pianoLeds(uint8_t* lv, uint8_t dim, uint8_t full);
    bool piano() const { return piano_; }
    // Times the key-light levels were built (tests).
    uint32_t ledBuilds() const { return ledBuilds_; }

private:
    void buildLeds();
    uint8_t notes_[48] = {};   // logical (bottom-origin) index; kNoNote = gap
    uint8_t scale_ = 0xFF, layout_ = 0xFF, root_ = 0xFF;
    bool piano_ = false;
    // Cached key lights (physical pads; kNoNote = gap) for ledDim_ / ledFull_.
    uint8_t leds_[48] = {};
    uint8_t ledDim_ = 0, ledFull_ = 0;
    uint32_t ledBuilds_ = 0;
};

inline int logicalPad(int physical) { return (3 - physical / 12) * 12 + physical % 12; }
inline int physicalPad(int logical) { return (3 - logical / 12) * 12 + logical % 12; }

}  // namespace ot
