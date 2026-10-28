// SD streaming: the stream readers' binding table (hal::SdStreamReader on
// the device, StorageStreamReader on the host). One slot per streamed
// instrument; instrument 0xFF marks a free slot. Pure: host-tested
// (tests/stream_bindings_test.cpp).
#pragma once

#include <cstdint>
#include <type_traits>

#include "audio/stream_extents.h"
#include "audio/stream_source.h"

namespace ot {
namespace audio {

constexpr uint8_t kStreamFree = 0xFF;

// The device reader's slot: 16 + 32 x 8 = 272 B, in .sdram_bss. No default
// member values (a static constructor would write SDRAM before boardInit()
// starts it and the Tracker stays black at power on); streamBindingClear()
// frees the slots at run time.
struct StreamBinding {
    uint8_t instrument;   // kStreamFree: free
    bool stereo;
    uint8_t nExt;
    uint32_t dataOffset, frames;
    StreamExtent ext[kStreamMaxExtents];
};
static_assert(std::is_trivially_default_constructible<StreamBinding>::value,
              "SDRAM object: no static constructor (tools/check_ctors.py)");

// Frees a slot. Value-initialising alone would leave instrument 0, a slot
// that claims instrument 0 instead of being free.
template <class B>
void streamBindingClear(B& b) {
    b = B{};
    b.instrument = kStreamFree;
}

template <class B>
B* streamBindingFind(B* t, int n, uint8_t instrument) {
    for (int i = 0; i < n; ++i)
        if (t[i].instrument == instrument) return &t[i];
    return nullptr;
}

// The instrument's own slot, else a free one; null when the table is full.
template <class B>
B* streamBindingSlot(B* t, int n, uint8_t instrument) {
    B* b = streamBindingFind(t, n, instrument);
    return b ? b : streamBindingFind(t, n, kStreamFree);
}

template <class B>
void streamBindingUnbind(B* t, int n, uint8_t instrument) {
    if (B* b = streamBindingFind(t, n, instrument)) streamBindingClear(*b);
}

template <class B>
void streamBindingUnbindAll(B* t, int n) {
    for (int i = 0; i < n; ++i) streamBindingClear(t[i]);
}

}  // namespace audio
}  // namespace ot
