// SD streaming: the stream readers' binding table (audio/stream_bindings.h),
// the device reader's slot type (trivial, in .sdram_bss) and the host's.
#include "test.h"

#include "audio/stream_bindings.h"

using namespace ot::audio;

namespace {
bool bindIns(StreamBinding* t, uint8_t ins) {
    StreamBinding* b = streamBindingSlot(t, kMaxStreams, ins);
    if (!b) return false;
    b->instrument = ins;
    b->frames = 1000;
    return true;
}
}  // namespace

TEST(unbind_all_frees_every_binding_for_any_instrument) {
    StreamBinding t[kMaxStreams];   // trivial: garbage until cleared, as in .sdram_bss
    std::memset(t, 0x5A, sizeof(t));
    streamBindingUnbindAll(t, kMaxStreams);
    for (const StreamBinding& b : t) CHECK_EQ(b.instrument, kStreamFree);
    for (uint8_t i = 0; i < 4; ++i) CHECK(bindIns(t, i));
    // New / project load: every slot free again, not claiming instrument 0.
    streamBindingUnbindAll(t, kMaxStreams);
    for (const StreamBinding& b : t) {
        CHECK_EQ(b.instrument, kStreamFree);
        CHECK_EQ(b.frames, 0u);
    }
    CHECK(!streamBindingFind(t, kMaxStreams, 0));
    CHECK(bindIns(t, 5));
    CHECK(streamBindingFind(t, kMaxStreams, 5));
    // Every slot is usable by any instrument.
    for (uint8_t i = 6; i < 6 + kMaxStreams - 1; ++i) CHECK(bindIns(t, i));
    CHECK(!bindIns(t, 40));   // full
    streamBindingUnbindAll(t, kMaxStreams);
    for (uint8_t i = 0; i < 4; ++i) CHECK(bindIns(t, i));
    streamBindingUnbind(t, kMaxStreams, 2);
    CHECK(!streamBindingFind(t, kMaxStreams, 2));
    CHECK(bindIns(t, 7));
    CHECK(streamBindingFind(t, kMaxStreams, 7) == &t[2]);   // reuses the freed slot
    CHECK(streamBindingFind(t, kMaxStreams, 0) == &t[0]);
}
