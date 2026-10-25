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
const char* kNoteLetters[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
}  // namespace

void noteName(int8_t note, char out[4]) {
    if (note >= 0) {
        // Stock grid / popup text has no separator: "C5", "C#5" (spec 20
        // §4.1 text colour rules; Part 2 §8.5 "C6", "C#6").
        int n = 0;
        out[n++] = kNoteLetters[note % 12][0];
        if (kNoteLetters[note % 12][1] == '#') out[n++] = '#';
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

void PadNotes::build(uint8_t scale, uint8_t layout, uint8_t root, bool piano) {
    if (scale == scale_ && layout == layout_ && root == root_ && piano == piano_) return;
    scale_ = scale;
    layout_ = layout;
    root_ = root;
    piano_ = piano;
    if (piano) {
        // [I] #42: logical rows 0 / 2 (bottom of each pair) white keys, 1 / 3
        // black keys; rows 0-1 the lower octave. Root option 0..36 = C1..C4:
        // the bottom C is the root's octave (stock starts at root + 12).
        static const bool kBlack[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
        const int base = 12 + 12 * (root / 12);
        for (int p = 0; p < 48; ++p) {
            const int row = p / 12, col = p % 12;
            const bool blackRow = (row & 1) != 0;
            const int n = base + 12 * (row / 2) + col;
            notes_[p] = kBlack[col] == blackRow && n <= 120 ? uint8_t(n) : kNoNote;
        }
        return;
    }
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

bool PadNotes::pianoLeds(uint8_t* lv, uint8_t dim, uint8_t full) const {
    if (!piano_) return false;
    static const bool kBlack[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
    const uint8_t rootLv = uint8_t((dim + full) / 2);
    const uint8_t blackLv = uint8_t(dim * 6 / 10 > 0 ? dim * 6 / 10 : 1);
    const uint8_t outLv = uint8_t(dim / 4 > 0 ? dim / 4 : 1);
    for (int lp = 0; lp < 48; ++lp) {
        const uint8_t n = notes_[lp];
        if (n == kNoNote) continue;
        uint8_t v = kBlack[n % 12] ? blackLv : dim;
        if (!isInScale(n, root_, scale_)) v = outLv;
        else if (n % 12 == root_ % 12) v = rootLv;
        lv[physicalPad(lp)] = v;
    }
    return true;
}

int PadNotes::padsForNote(int note, uint8_t* pads, int cap) const {
    int n = 0;
    for (int lp = 0; lp < 48 && n < cap; ++lp)
        if (notes_[lp] == note) pads[n++] = uint8_t(physicalPad(lp));
    return n;
}

}  // namespace ot
