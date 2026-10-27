// SD streaming: the IStorage-backed stream reader (storage_stream_reader.h).
#include "app/storage_stream_reader.h"

#include <cstdio>

namespace ot {

const StorageStreamReader::Binding* StorageStreamReader::find(uint8_t instrument) const {
    for (const Binding& b : b_)
        if (b.instrument == instrument) return &b;
    return nullptr;
}

bool StorageStreamReader::bind(const audio::StreamSource& s, const char* path, uint32_t dataOffset, bool stereo,
                               audio::StreamBindInfo* info) {
    audio::StreamBindInfo dummy;
    if (!info) info = &dummy;
    Binding* slot = const_cast<Binding*>(find(s.instrument));
    if (!slot) slot = const_cast<Binding*>(find(0xFF));
    if (!slot) {
        info->reason = "no slot";
        return false;
    }
    const IStorage::Handle h = fs_.open(path, IStorage::kRead);
    if (h < 0) {
        info->reason = "open";
        return false;
    }
    const int32_t size = fs_.size(h);
    fs_.close(h);
    const uint64_t need = uint64_t(dataOffset) + uint64_t(s.frames) * 2u * (stereo ? 2u : 1u);
    if (size < 0 || uint64_t(size) < need) {
        info->reason = "short file";
        return false;
    }
    slot->instrument = s.instrument;
    slot->stereo = stereo;
    slot->dataOffset = dataOffset;
    slot->frames = s.frames;
    std::snprintf(slot->path, sizeof(slot->path), "%s", path);
    info->extents = 1;
    ++binds;
    return true;
}

void StorageStreamReader::unbind(uint8_t instrument) {
    if (Binding* b = const_cast<Binding*>(find(instrument))) *b = Binding{};
}

void StorageStreamReader::unbindAll() {
    for (Binding& b : b_) b = Binding{};
}

bool StorageStreamReader::bound(uint8_t instrument) const { return instrument != 0xFF && find(instrument); }

bool StorageStreamReader::read(const audio::StreamSource& s, uint32_t frame, uint32_t n, int16_t* l, int16_t* r) {
    const Binding* b = find(s.instrument);
    if (!b || s.instrument == 0xFF || uint64_t(frame) + n > b->frames) return false;
    if (failAfter == 0) return false;
    if (failAfter > 0) --failAfter;
    ++reads;
    const IStorage::Handle h = fs_.open(b->path, IStorage::kRead);
    if (h < 0) return false;
    bool ok = fs_.seek(h, b->dataOffset + frame * 2u) && fs_.read(h, l, n * 2u) == int32_t(n * 2u);
    if (ok && r && b->stereo)
        ok = fs_.seek(h, b->dataOffset + b->frames * 2u + frame * 2u) && fs_.read(h, r, n * 2u) == int32_t(n * 2u);
    fs_.close(h);
    return ok;
}

}  // namespace ot
