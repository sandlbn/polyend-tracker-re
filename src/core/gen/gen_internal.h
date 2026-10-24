// Internals shared by the generator sources (not part of the gen.h API).
#pragma once

#include "gen/gen.h"

namespace ot {
namespace gen {

// Step FX ids used by the generators (model/fx.h numbering, spec 02 §4.3).
constexpr uint8_t kFxOff = 1;
constexpr uint8_t kFxMicroMove = 2;
constexpr uint8_t kFxRoll = 3;
constexpr uint8_t kFxChance = 4;
constexpr uint8_t kFxVolume = 18;
constexpr uint8_t kFxGlide = 19;
constexpr uint8_t kFxGate = 20;
constexpr uint8_t kFxSliceId = 25;
constexpr uint8_t kFxReverse = 26;
constexpr uint8_t kFxLowPass = 27;

// Roll FX value for "hits per step" (sequencer kRollPeriod: 1,2,3,4,6,8,12,16)
// and a roll type (0 constant, 1.. the volume ramps; value = type * 9 + rate).
uint8_t rollValue(float hitsPerStep, int type = 0);

void setFx(Step& s, int slot, uint8_t type, uint8_t value);
void putFx(Step& s, uint8_t type, uint8_t value);

void genAcid(const int16_t* v, Target& t, Rng& rng, Result& r);
void genCallResponse(const int16_t* v, Target& t, Rng& rng, Result& r);
void genBBCut(const int16_t* v, Target& t, Rng& rng, Result& r);
void genTurnaround(const int16_t* v, Target& t, Rng& rng, Result& r);
void genGenotype(const int16_t* v, Target& t, Rng& rng, Result& r);
void genStation(const int16_t* v, Target& t, Rng& rng, Result& r);

constexpr int kCrStyleCount = 46;
constexpr int kCrStrategyCount = 38;
const char* crStyleName(int i);
const char* crStrategyName(int i);

constexpr int kBBProcCount = 10;
const char* bbProcName(int i);
const ParamDesc* bbProcParam(int proc, int i);   // proc parameter A (0) / B (1), nullptr = none

// Slice used for a source offset (0..1 of the loop): the slice whose start is
// closest at or before it (wrapping slice counts), Beat Slice or Slice mode.
int sliceForOffset(const Slices& s, float prop);
// Writes a slice hit on a step: Beat Slice -> note 24 + n; Slice -> unity
// note + Slice FX in the first free FX slot.
void writeSliceHit(Step& st, const Slices& s, int slice, uint8_t instr);

}  // namespace gen
}  // namespace ot
