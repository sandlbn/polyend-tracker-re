// Note naming, scales and the pad -> note map (spec 05 §4.1, spec 06 §1.1).
#pragma once

#include <cstdint>

namespace ot {

constexpr int kScaleCount = 39;

// Note text, at most 3 chars + NUL: "C5", "C#5" (no separator, stock); specials "FAD", "CUT", "OFF"; empty "---".
// Note 0 = C0, so note 60 = C5 (spec 06 §1.1). Black keys follow the current spelling ("Db5" with flats).
void noteName(int8_t note, char out[4]);

// [I] Config > General > Note names (not in stock): how black keys are spelled.
enum NoteSpelling : uint8_t { kSpellSharps = 0, kSpellFlats = 1, kSpellAuto = 2 };
constexpr int kNoteSpellingCount = 3;
// Auto: flats when the key (root pitch class, scale) is a flat key.
bool keyUsesFlats(int root, uint8_t scale);
// Sets the spelling noteName() and pitchClassName() use; root/scale are the project's (Auto only).
void setNoteSpelling(uint8_t spelling, int root, uint8_t scale);
bool noteSpellingFlats();
// Pitch class text without octave: "C", "C#" / "Db", ... (current spelling).
const char* pitchClassName(int note);

uint16_t scaleMask(uint8_t scale);
const char* scaleName(uint8_t scale);   // stock names (spec 16 §B.2)
bool isInScale(int note, int root, uint8_t scale);
// Root note option 0..36 = C1..C4; the root pitch class is root % 12.

// Pad -> note table (spec 05 §4.1). Physical pad index = row*12 + col, row 0 = top.
class PadNotes {
public:
    void build(uint8_t scale, uint8_t layout, uint8_t root);
    // raw: chromatic pad + 24 (spec 05 §4.1 getNote raw).
    uint8_t note(uint8_t physicalPad, bool raw = false) const;
    // Physical pad producing `note`, or -1.
    int padsForNote(int note, uint8_t* pads, int cap) const;

private:
    uint8_t notes_[48] = {};   // logical (bottom-origin) index
    uint8_t scale_ = 0xFF, layout_ = 0xFF, root_ = 0xFF;
};

inline int logicalPad(int physical) { return (3 - physical / 12) * 12 + physical % 12; }
inline int physicalPad(int logical) { return (3 - logical / 12) * 12 + logical % 12; }

}  // namespace ot
