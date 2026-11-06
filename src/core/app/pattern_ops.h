// Pattern edit operations used by the pattern editor (spec 06 §1.6-§1.11).
// Pure functions over a Pattern; the editor owns undo and dirty tracking.
//
// Columns: 0 Note, 1 Instr, 2 Fx1 (step bytes 4/5 = model fx[0]),
// 3 Fx2 (bytes 2/3 = model fx[1]) (spec 06 §1.1).
#pragma once

#include <cstdint>

#include "model/fx.h"
#include "model/pattern.h"

namespace ot {

// Normalised inclusive rectangle of steps x tracks (spec 06 §1.1 "Target rectangle").
struct SelRect {
    int s0 = 0, s1 = 0;   // steps
    int t0 = 0, t1 = 0;   // tracks
    bool single() const { return s0 == s1 && t0 == t1; }
    int rows() const { return s1 - s0 + 1; }
};

// Column mode chosen by the held key (spec 06 §1.11).
enum class ColMode : uint8_t { All = 0, Pattern = 1, Note = 2, Instr = 3, Fx2 = 4, Fx1 = 5 };

inline int fxSlotForColumn(int col) { return col == 2 ? 0 : 1; }
inline bool isMidiTrack(int t) { return t >= 8; }
inline bool emptyNote(int8_t n) { return n == kNoteEmpty || n < kNoteOff; }

namespace pops {

int length(const Pattern& p);
void setLength(Pattern& p, int len);                    // 1..128, all 16 tracks
// Track-type mapping (spec 06 §1.7): audio tracks 0..63, MIDI tracks 48..63.
uint8_t mapInstrument(int track, int instr);

void clearRect(Pattern& p, const SelRect& r, ColMode m);
void insertRows(Pattern& p, const SelRect& r);
bool backspaceRows(Pattern& p, const SelRect& r);
void paste(Pattern& dst, const Pattern& clip, const SelRect& src, int dstStep, int dstTrack, ColMode m,
           uint8_t curInstr);
// One FX column of the copied cells into the other FX column (0 / 1 = FX 1 / 2); the rest stays.
void pasteFxColumn(Pattern& dst, const Pattern& clip, const SelRect& src, int dstStep, int dstTrack, int fromFx, int toFx);
void invert(Pattern& p, const SelRect& r, ColMode m);
bool duplicate(Pattern& p);
bool expand(Pattern& p);
bool shrink(Pattern& p);

// Pad/encoder writes (spec 06 §1.6, §1.7).
void writeNote(Pattern& p, const SelRect& r, int note, uint8_t curInstr);
void writeInstrument(Pattern& p, const SelRect& r, uint8_t instr);
void writeFxType(Pattern& p, const SelRect& r, int col, uint8_t type);
void writeFxValue(Pattern& p, const SelRect& r, int col, uint8_t value);
// Encoder note edit / transpose. Returns the resulting note of a single cell (or -1).
int stepNote(Pattern& p, const SelRect& r, int delta, uint8_t curInstr);
void stepInstrument(Pattern& p, const SelRect& r, int delta);
void stepFxValue(Pattern& p, const SelRect& r, int col, int delta);
void stepFxType(Pattern& p, const SelRect& r, int col, int delta);
// Ordered FX list helpers (kFxMenuOrder).
int fxOrderIndex(uint8_t type);
uint8_t fxValueFromPad(uint8_t type, int pad);

// Fill popup parameters (spec 06 §1.10).
struct FillParams {
    int16_t where = 0;       // 0..6
    int16_t param[7] = {1, 1, 1, 1, 16, 4, 4};   // per where (density / N / events)
    int16_t scaleOrFx = 0;   // note: scale; fx: ordered index, -1 = random
    int16_t type = 0;        // 0 constant, 1 from->to, 2 random
    int16_t from = 60;
    int16_t to = 60;
};
using RandFn = uint32_t (*)(void* ctx);   // uniform 32-bit
void fill(Pattern& p, const SelRect& r, int col, const FillParams& f, uint8_t rootNote, uint8_t curInstr,
          RandFn rnd, void* rndCtx);

}  // namespace pops
}  // namespace ot
