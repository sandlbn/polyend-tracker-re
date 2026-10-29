// SD streaming (docs/PLAN_SD_STREAMING_P1C_P2.md §1.5): the stream reader for
// the host tests and the simulator. It reads a bound instrument's payload
// through IStorage (open, seek, read, close per call: host files are cheap and
// no handle is held). The device uses hal::SdStreamReader (raw sectors through
// the file's cluster map) instead.
#pragma once

#include <cstdint>

#include "audio/stream_bindings.h"
#include "audio/stream_source.h"
#include "hal_api.h"

namespace ot {

class StorageStreamReader final : public audio::IStreamReader {
public:
    explicit StorageStreamReader(IStorage& fs) : fs_(fs) {}
    bool bind(const audio::StreamSource& s, const char* path, uint32_t dataOffset, bool stereo,
              audio::StreamBindInfo* info) override;
    bool streamable(uint8_t instrument, const char* path, audio::StreamBindInfo* info) override;
    void unbind(uint8_t instrument) override;
    void unbindAll() override;
    bool bound(uint8_t instrument) const override;
    bool read(const audio::StreamSource& s, uint32_t frame, uint32_t n, int16_t* l, int16_t* r) override;

    // Test hooks: calls made; failAfter >= 0 makes reads fail after that many
    // more (a card pulled mid-stream); failBind makes binds fail with that
    // reason (the device reader's "fragmented", "map check");
    // failStreamable makes the import pre-check (streamable) fail so.
    uint32_t reads = 0, binds = 0;
    int failAfter = -1;
    const char* failBind = nullptr;
    const char* failStreamable = nullptr;

private:
    struct Binding {
        uint8_t instrument = audio::kStreamFree;
        bool stereo = false;
        uint32_t dataOffset = 0, frames = 0;
        char path[344] = {};
    };
    const Binding* find(uint8_t instrument) const;
    IStorage& fs_;
    Binding b_[audio::kMaxStreams];
};

}  // namespace ot
