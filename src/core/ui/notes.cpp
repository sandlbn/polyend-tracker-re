#include "ui/notes.h"


namespace ot {

namespace {
// spec 05 §4.1 scale masks (bit 0 = root).
const uint16_t kScaleMasks[kScaleCount] = {
    0xfff, 0x5ad, 0xab5, 0x6ad, 0xad5, 0x5d5, 0x56b, 0x5ab, 0x5b3, 0x6b5, 0xaad, 0x9ad, 0xbb5,
    0x6bd, 0xeb5, 0x4e9, 0x29d, 0x4a9, 0x295, 0x9cd, 0x6cd, 0xad3, 0x5ab, 0x555,
    0xb6d, 0x55b, 0x18d, 0x4a3, 0x2a5, 0x463, 0xb6d, 0x28d, 0x6d5, 0x9b3, 0x5b1,
    0x9cd, 0xaab, 0x9ab, 0xd53};

// Scale names, same order as the masks (spec 16 §B.2, table 0xa7f7c;
// "Phrygian" is stored there with a trailing space).
const char* const kScaleNames[kScaleCount] = {
    "Chromatic", "Minor",     "Major",     "Dorian",    "Lyd Maj",   "Lyd Min",  "Locrian",  "Phrygian",
    "PhrygDom",  "Mixlydian", "Melo Min",  "Harm Min",  "BeBopMaj",  "BeBopDor", "BeBopMix", "Blues Min",
    "Blues Maj", "Penta Min", "Penta Maj", "Hung Min",  "Ukrainian", "Marva",    "Todi",     "Wholetone",
    "Dim",       "SLocrian",  "Hirajoshi", "In Sen",    "Yo",        "Iwato",    "WholeHalf", "Kumoi",
    "Overtone",  "DoubHarm",  "Indian",    "GypsyMin",  "NeapoMaj",  "NeapoMin", "Enigmatic"};
const char* const kSharpNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kFlatNames[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
bool gFlats = false;
}  // namespace

namespace {
// Letter names the scale's notes repeat when spelled from `names` (C D E F G
// A Bb: 0; C D E F G A A#: 1).
int repeatedLetters(int root, uint16_t mask, const char* const* names) {
    int seen = 0, repeats = 0;
    for (int i = 0; i < 12; ++i) {
        if (!((mask >> i) & 1)) continue;
        const int bit = 1 << (names[(root + i) % 12][0] - 'A');
        if (seen & bit) ++repeats;
        seen |= bit;
    }
    return repeats;
}
}  // namespace

bool keyUsesFlats(int root, uint8_t scale) {
    const uint16_t m = scaleMask(scale);
    const int pc = ((root % 12) + 12) % 12;
    // The spelling that names the scale's notes with the fewest repeated
    // letters: C Mixolydian ... A Bb (flats), D melodic minor ... B C#
    // (sharps), whatever the key signature of its major or minor key.
    const int sharps = repeatedLetters(pc, m, kSharpNames), flats = repeatedLetters(pc, m, kFlatNames);
    if (sharps != flats) return flats < sharps;
    // A tie (pentatonic, chromatic, whole-tone, F# / Gb major...): the key
    // signature. Minor-third scales without a major third are spelled like
    // their relative major (D minor -> F major -> flats).
    int major = pc;
    if (((m >> 3) & 1) && !((m >> 4) & 1)) major = (major + 3) % 12;
    // Major keys written with flats: F, Bb, Eb, Ab, Db (Gb / F# spelled with sharps).
    return major == 5 || major == 10 || major == 3 || major == 8 || major == 1;
}

void setNoteSpelling(uint8_t spelling, int root, uint8_t scale) {
    gFlats = spelling == kSpellFlats || (spelling == kSpellAuto && keyUsesFlats(root, scale));
}

bool noteSpellingFlats() { return gFlats; }

const char* pitchClassName(int note) { return (gFlats ? kFlatNames : kSharpNames)[((note % 12) + 12) % 12]; }

void noteName(int8_t note, char out[4]) {
    if (note >= 0) {
        // Stock grid / popup text has no separator: "C5", "C#5" (spec 20
        // §4.1 text colour rules; Part 2 §8.5 "C6", "C#6").
        const char* pc = pitchClassName(note);
        int n = 0;
        while (*pc) out[n++] = *pc++;
        out[n++] = char('0' + (note / 12) % 10);
        out[n] = 0;
        return;
    }
    const char* s = "---";
    if (note == -2) s = "FAD";
    else if (note == -3) s = "CUT";
    else if (note == -4) s = "OFF";
    out[0] = s[0];
    out[1] = s[1];
    out[2] = s[2];
    out[3] = 0;
}

uint16_t scaleMask(uint8_t scale) { return kScaleMasks[scale < kScaleCount ? scale : 0]; }

const char* scaleName(uint8_t scale) { return kScaleNames[scale < kScaleCount ? scale : 0]; }

bool isInScale(int note, int root, uint8_t scale) {
    // spec 05 §4.1 isInScale
    int i = ((note % 12) - (root % 12) + 24) % 12;
    return (scaleMask(scale) >> i) & 1;
}

void PadNotes::build(uint8_t scale, uint8_t layout, uint8_t root) {
    if (scale == scale_ && layout == layout_ && root == root_) return;
    scale_ = scale;
    layout_ = layout;
    root_ = root;
    // spec 05 §4.1 algorithm (bottom-origin logical index).
    int n = root + 12;
    for (int p = 0; p < 48; ++p) {
        if (p > 0 && p % 12 == 0 && layout + 1 < 12) n = notes_[(p - 12) + layout + 1 - 1];
        int guard = 0;
        while (!isInScale(n, root, scale) && guard++ < 12) ++n;
        notes_[p] = uint8_t(n > 120 ? 120 : n);
        ++n;
    }
}

uint8_t PadNotes::note(uint8_t physicalPad, bool raw) const {
    if (physicalPad > 47) physicalPad = 47;
    int lp = logicalPad(physicalPad);
    if (raw) return uint8_t(lp + 24);
    return notes_[lp];
}

int PadNotes::padsForNote(int note, uint8_t* pads, int cap) const {
    int n = 0;
    for (int lp = 0; lp < 48 && n < cap; ++lp)
        if (notes_[lp] == note) pads[n++] = uint8_t(physicalPad(lp));
    return n;
}

}  // namespace ot
