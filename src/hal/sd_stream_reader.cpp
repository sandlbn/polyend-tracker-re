// OpenTracker -- SD streaming device reader (sd_stream_reader.h).
#include "sd_stream_reader.h"

#include <Arduino.h>
#include <string.h>

#include "diag.h"
#include "sdram_bss.h"

namespace ot {
namespace hal {

namespace {
OT_SDRAM_BSS SdStreamReader::Binding gBindings[audio::kMaxStreams];
alignas(4) uint8_t gEdge[512];   // partial head / tail sectors (SRAM)
}  // namespace

// Constructed in otAppMain (a function-local static), after the SDRAM is up.
SdStreamReader::SdStreamReader(SdStorage& sd) : sd_(sd), b_(gBindings) {
    audio::streamBindingUnbindAll(b_, audio::kMaxStreams);
}

SdStreamReader::Binding* SdStreamReader::find(uint8_t instrument) const {
    return audio::streamBindingFind(b_, audio::kMaxStreams, instrument);
}

bool SdStreamReader::bind(const audio::StreamSource& s, const char* path, uint32_t dataOffset, bool stereo,
                          audio::StreamBindInfo* info) {
    audio::StreamBindInfo dummy;
    if (!info) info = &dummy;
    const uint32_t t0 = micros();
    Binding* slot = audio::streamBindingSlot(b_, audio::kMaxStreams, s.instrument);
    if (!slot) {
        info->reason = "no slot";
        return false;
    }
    Binding b;
    int n = 0;
    uint32_t size = 0;
    if (!sd_.fileExtents(path, b.ext, audio::kStreamMaxExtents, &n, &size)) {
        info->reason = "fragmented";
        return false;
    }
    const uint64_t need = uint64_t(dataOffset) + uint64_t(s.frames) * 2u * (stereo ? 2u : 1u);
    if (uint64_t(size) < need) {
        info->reason = "short file";
        return false;
    }
    b.instrument = s.instrument;
    b.stereo = stereo;
    b.nExt = uint8_t(n);
    b.dataOffset = dataOffset;
    b.frames = s.frames;
    // Self-check: the first whole sector of the first streamed chunk, raw
    // through the map and through FatFs, must be the same bytes.
    const uint32_t off = dataOffset + audio::kStreamHeadFrames * 2u;
    audio::SectorOp ops[4];
    const int k = audio::planSectorRead(b.ext, n, off, 1024, ops, 4);
    int w = -1;
    for (int i = 0; i < k; ++i)
        if (ops[i].whole) {
            w = i;
            break;
        }
    if (w >= 0) {
        uint8_t viaFs[512];
        const bool ok = sd_.readSectors(ops[w].lba, gEdge, 1) && sd_.readBytesAt(path, off + ops[w].dstOff, viaFs) &&
                        memcmp(gEdge, viaFs, 512) == 0;
        if (!ok) {
            ++mismatches_;
            info->reason = "map check";
            diagLogf("sd stream: bind %u \"%s\" extents %d: map check MISMATCH", unsigned(s.instrument), path, n);
            return false;
        }
    }
    *slot = b;
    info->extents = uint16_t(n);
    info->bindUs = micros() - t0;
    diagLogf("sd stream: bind %u \"%s\" extents %d in %lu us, check ok", unsigned(s.instrument), path, n,
             static_cast<unsigned long>(info->bindUs));
    return true;
}

bool SdStreamReader::streamable(uint8_t instrument, const char* path, audio::StreamBindInfo* info) {
    audio::StreamBindInfo dummy;
    if (!info) info = &dummy;
    const uint32_t t0 = micros();
    if (!audio::streamBindingSlot(b_, audio::kMaxStreams, instrument)) {
        info->reason = "no slot";
        return false;
    }
    // The import's reserved file (an allocated chain, SdStorage::reserve):
    // its map must fit a binding, as bind() checks after the conversion.
    audio::StreamExtent ext[audio::kStreamMaxExtents];
    int n = 0;
    if (!sd_.fileExtents(path, ext, audio::kStreamMaxExtents, &n, nullptr)) {
        info->reason = "fragmented";
        diagLogf("sd stream: pre-check \"%s\": fragmented", path);
        return false;
    }
    info->extents = uint16_t(n);
    info->bindUs = micros() - t0;
    diagLogf("sd stream: pre-check \"%s\" extents %d in %lu us", path, n, static_cast<unsigned long>(info->bindUs));
    return true;
}

void SdStreamReader::unbind(uint8_t instrument) { audio::streamBindingUnbind(b_, audio::kMaxStreams, instrument); }

void SdStreamReader::unbindAll() { audio::streamBindingUnbindAll(b_, audio::kMaxStreams); }

bool SdStreamReader::bound(uint8_t instrument) const { return instrument != 0xFF && find(instrument); }

bool SdStreamReader::readChannel(const Binding& b, uint32_t byteOff, uint32_t bytes, uint8_t* dst) {
    audio::SectorOp ops[2 + audio::kStreamMaxExtents];
    const int k = audio::planSectorRead(b.ext, b.nExt, byteOff, bytes, ops, 2 + audio::kStreamMaxExtents);
    if (k <= 0) return false;
    for (int i = 0; i < k; ++i) {
        const audio::SectorOp& o = ops[i];
        if (o.whole) {
            if (!sd_.readSectors(o.lba, dst + o.dstOff, o.sectors)) return false;
        } else {
            if (!sd_.readSectors(o.lba, gEdge, 1)) return false;
            memcpy(dst + o.dstOff, gEdge + o.srcSkip, o.bytes);
        }
    }
    return true;
}

bool SdStreamReader::read(const audio::StreamSource& s, uint32_t frame, uint32_t n, int16_t* l, int16_t* r) {
    const Binding* b = find(s.instrument);
    if (!b || s.instrument == 0xFF || uint64_t(frame) + n > b->frames) return false;
    if (!readChannel(*b, b->dataOffset + frame * 2u, n * 2u, reinterpret_cast<uint8_t*>(l))) return false;
    if (r && b->stereo)
        return readChannel(*b, b->dataOffset + b->frames * 2u + frame * 2u, n * 2u, reinterpret_cast<uint8_t*>(r));
    return true;
}

}  // namespace hal
}  // namespace ot
