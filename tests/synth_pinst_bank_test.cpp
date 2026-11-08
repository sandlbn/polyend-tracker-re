// [3.x] Preset instruments: instruments 67..74 play the synth presets P1..P8
// like samples. A note of P<k> borrows a synth slot for preset k: a slot
// already on k, else an idle slot (S3 first), else it steals the slot whose
// notes are all in their release, else the one whose last note-on is the
// oldest (its voices fade over one block). The slot then plays P<k> with
// its own polyphony, mono / legato rules and note-offs by track. A note of
// the slot's own instrument (S1..S3) takes it back to what Z / the chase
// selected for it; a Z on a borrowed slot wins at once. An empty preset, or
// one whose engine this build lacks, is silent.
#include "test.h"

#include <cmath>
#include <cstring>

#include "audio/audio_tables.h"
#include "audio/synth_bank.h"
#include "model/constants.h"
#include "model/fx.h"
#include "modules/synth_models.h"
#if __has_include("ot_profile.h")
#include "ot_profile.h"
#endif

using namespace ot;
using namespace ot::audio;

namespace {

TEST(preset_instruments_are_67_to_74) {
    CHECK_EQ(kFirstPresetInstrument, 67);
    CHECK_EQ(kLastPresetInstrument, 74);
    CHECK_EQ(kPresetInstrumentCount, kSynthPresetCount);
    CHECK(!isPresetInstrument(66) && isPresetInstrument(67) && isPresetInstrument(74) && !isPresetInstrument(75));
    CHECK_EQ(int(presetOfInstrument(67)), 1);
    CHECK_EQ(int(presetOfInstrument(74)), 8);
    CHECK_EQ(int(presetOfInstrument(66)), 0);
    CHECK_EQ(int(presetOfInstrument(75)), 0);
    CHECK(isSynthInstrument(64) && isSynthInstrument(74) && !isSynthInstrument(75) && !isSynthInstrument(63));
    CHECK(isSynthSlotInstrument(66) && !isSynthSlotInstrument(67));
    CHECK_EQ(synthSlotOf(66), 2);
    CHECK_EQ(synthSlotOf(67), -1);
    CHECK_EQ(synthSlotOf(74), -1);
}

TEST(the_note_record_does_not_grow) {
    // The preset rides in a padding byte of the note record (and of the
    // bank's queued / pending notes).
    CHECK_EQ(int(sizeof(SynthNoteOn)), 12);
    SynthNoteOn n;
    CHECK_EQ(int(n.preset), 0);
}

}  // namespace
