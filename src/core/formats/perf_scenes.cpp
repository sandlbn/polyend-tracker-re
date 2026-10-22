#include "formats/perf_scenes.h"

#include <cstring>

namespace ot {

bool perfScenesUsed(const Project& p) {
    for (const auto& sc : p.perfScene)
        for (uint8_t r : sc)
            if (r) return true;
    return false;
}

void encodePerfScenes(const Project& p, uint8_t out[pscn::kFileSize]) {
    std::memset(out, 0, pscn::kFileSize);
    std::memcpy(out, "OTPS", 4);
    out[4] = pscn::kVersion;
    out[5] = uint8_t(Project::kPerfScenes);
    out[6] = uint8_t(Project::kPerfSceneSlots);
    uint8_t sum = 0;
    uint8_t* r = out + pscn::kHeaderSize;
    for (const auto& sc : p.perfScene)
        for (uint8_t v : sc) {
            *r++ = v;
            sum = uint8_t(sum + v);
        }
    out[pscn::kFileSize - 1] = sum;
}

FmtStatus decodePerfScenes(const uint8_t* file, size_t len, Project& p) {
    std::memset(p.perfScene, 0, sizeof(p.perfScene));
    if (len < pscn::kHeaderSize) return FmtStatus::TooShort;
    if (std::memcmp(file, "OTPS", 4) != 0 || file[4] == 0) return FmtStatus::BadMagic;
    const uint32_t scenes = file[5], slots = file[6];
    const size_t body = size_t(scenes) * slots;
    if (len < pscn::kHeaderSize + body + 1) return FmtStatus::TooShort;
    uint8_t sum = 0;
    for (size_t k = 0; k < body; ++k) sum = uint8_t(sum + file[pscn::kHeaderSize + k]);
    if (sum != file[pscn::kHeaderSize + body]) return FmtStatus::BadMagic;
    for (uint32_t sc = 0; sc < scenes && sc < uint32_t(Project::kPerfScenes); ++sc)
        for (uint32_t s = 0; s < slots && s < uint32_t(Project::kPerfSceneSlots); ++s) {
            const uint8_t r = file[pscn::kHeaderSize + sc * slots + s];
            p.perfScene[sc][s] = r <= 3 ? r : 0;
        }
    return FmtStatus::Ok;
}

}  // namespace ot
