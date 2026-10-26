// Live generators (OT fork): what keeps running while the sequencer plays.
//
//  * BBCut play modes B1..B10: new cut tables at every stop, every N loops
//    while playing, or locked (Config > BB play modes); procedure A / B from
//    the Generate popup.
//  * BBCut Mutate (up to kBBSlots selections): new cuts every N loops.
//  * Acid: a track written by Acid stays "live" (up to kAcidSlots tracks):
//      - Density follows the Perform FX "Acid Density" (-7..+7 on top of the
//        generated density, on the tracks the FX is on), changing the notes
//        right away without changing the melody;
//      - Mutate > 0 re-rolls part of the line every "Mut. every" loops
//        (TB-3PO's mutate).
//    A slot ends when its track is edited by hand, when it is generated
//    again by something else, or when the pattern is left.
#pragma once

#include <cstdint>

#include "gen/gen.h"

namespace ot {

class App;

namespace live {

constexpr int kAcidSlots = 4;
constexpr int kBBSlots = 2;

// Main loop, every pass (App::loop).
void service(App& a);
// Takes over the acid line Acid just wrote on target track t.t0.
// v = the Acid parameters, rngSeed = gen::Result::rngSeed of the generate.
void acid(App& a, const int16_t* v, const gen::Target& t, uint32_t rngSeed);
// Drops a live slot on track `track` (another generator wrote there).
void release(int track);
bool acidActive(int track);
// BBCut: A / B of procedure `proc` (Generate popup), also used by the B play
// modes; bbcut() takes over a track BBCut just wrote (live Mutate).
void setBBParams(int proc, int a, int b);
void bbcut(App& a, const int16_t* v, const gen::Target& t, uint32_t rngSeed);
bool bbcutActive(int track);
// Perform FX "Acid Density" offset per track (0 = none); applied by service().
void setAcidDensityOffset(int track, int offset);
void stopAll();

}  // namespace live
}  // namespace ot
