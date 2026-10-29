// OpenTracker -- SD streaming device reader (docs/PLAN_SD_STREAMING_P1C_P2.md
// §1.1-1.2). A streamed instrument's .pti is read as raw card sectors through
// its cluster map, captured once at bind with FatFs fast seek: no FatFs handle
// or state on the stream path. Main loop only (App::serviceStreams).
#pragma once

#include <stdint.h>

#include "audio/stream_bindings.h"
#include "audio/stream_extents.h"
#include "audio/stream_source.h"
#include "sd_storage.h"

namespace ot {
namespace hal {

class SdStreamReader final : public audio::IStreamReader {
public:
    explicit SdStreamReader(SdStorage& sd);
    bool bind(const audio::StreamSource& s, const char* path, uint32_t dataOffset, bool stereo,
              audio::StreamBindInfo* info) override;
    bool streamable(uint8_t instrument, const char* path, audio::StreamBindInfo* info) override;
    void unbind(uint8_t instrument) override;
    void unbindAll() override;
    bool bound(uint8_t instrument) const override;
    bool read(const audio::StreamSource& s, uint32_t frame, uint32_t n, int16_t* l, int16_t* r) override;
    uint32_t mismatches() const { return mismatches_; }   // bind self-check failures (diagnostics)

    // 272 B a slot, in .sdram_bss: trivial (audio/stream_bindings.h).
    using Binding = audio::StreamBinding;

private:
    Binding* find(uint8_t instrument) const;
    bool readChannel(const Binding& b, uint32_t byteOff, uint32_t bytes, uint8_t* dst);
    SdStorage& sd_;
    Binding* b_;
    uint32_t mismatches_ = 0;
};

}  // namespace hal
}  // namespace ot
