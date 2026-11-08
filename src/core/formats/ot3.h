// OT3 container (docs/specs/3x-01-ot3-format.md): a little-endian TLV file
// with a 16-byte header and a CRC-32 trailer. 3.0 uses one kind, the project
// sidecar `project.ot3x` next to the Tracker Mini project files. Pure: no
// storage, no allocation.
//
//   header  "OT3\0" | u16 version | u16 kind | u8 writer[4] | u32 total size
//   chunk   u16 tag | u16 flags | u32 len | payload | zero pad to 4 bytes
//   trailer u32 CRC-32 (formats/crc32.h) over bytes [0, size - 4)
#pragma once

#include <cstddef>
#include <cstdint>

namespace ot {
namespace ot3 {

constexpr uint16_t kVersion = 1;
constexpr uint16_t kKindProjectSidecar = 1;
constexpr uint32_t kHeaderSize = 16;
constexpr uint32_t kChunkHeaderSize = 8;
constexpr uint32_t kMaxFile = 16384;   // a larger file is refused

// Chunk flags.
enum : uint16_t {
    kFlagCritical = 1u << 0,   // a reader that does not know the tag must not re-save the file
    kFlagOpaque = 1u << 1,     // written back unchanged by a build without the owner
};

// Tags (a tag never changes meaning; readers take known leading fields).
enum : uint16_t {
    kTagInfo = 0x0001,        // u8 synthSlots, u8 rsv[3]
    kTagSlotEngine = 0x0007,  // SlotEngine (16 B)
    kTagSlotInsert = 0x0008,  // SlotInsert (16 B), not critical
    kTagSlotArp = 0x0009,     // SlotArp (4 B), not critical
    kTagPresetInsert = 0x000A, // PresetInsert (16 B), not critical
    kTagPresetMix = 0x000B,   // PresetMix (20 B), not critical
    kTagPresetName = 0x000C,  // PresetName (36 B), not critical
    kTagMods = 0x0010,        // n x ModEntry (8 B)
    kTagModuleFirst = 0x4000, // module chunks: ModuleChunkHeader + payload
    kTagModuleLast = 0x7FFF,
};

struct SlotEngine {   // kTagSlotEngine payload, little endian
    uint8_t slot;
    uint8_t miniFallback;   // Tracker Mini model index 0..4 written to project.mt
    uint16_t rsv;
    int32_t engineId;
    char moduleId[4];
    uint16_t moduleVersion;
    uint16_t rsv2;
};
static_assert(sizeof(SlotEngine) == 16, "SlotEngine layout");

constexpr uint8_t kSlotInsertOff = 0x01;   // SlotInsert::rsv bit 0: the insert is Off (settings kept)
struct SlotInsert {   // kTagSlotInsert payload: a synth slot's insert effect
    uint8_t slot;
    uint8_t rsv;            // bit 0 kSlotInsertOff; other bits 0
    char effectId[4];       // IEffect::id()
    uint8_t set[6];         // its settings (audio::EffectSettings)
    uint8_t rsv2[4];
};
static_assert(sizeof(SlotInsert) == 16, "SlotInsert layout");

struct SlotArp {   // kTagSlotArp payload: a synth slot's step FX Arp settings (a slot at 100 has none)
    uint8_t slot;
    uint8_t gate;           // Arp Gate, 1..99
    uint8_t rsv[2];
};
static_assert(sizeof(SlotArp) == 4, "SlotArp layout");

// kTagPresetInsert payload: the insert a synth preset carries (one chunk per
// preset that has insert data; a preset without one keeps the slot's insert).
constexpr uint8_t kPresetInsertOff = 0x01;    // flags bit 0: the insert is Off (settings kept)
constexpr uint8_t kPresetInsertNone = 0x02;   // flags bit 1: stored with no insert (it removes the slot's)
struct PresetInsert {
    uint8_t preset;         // 1..8
    uint8_t flags;
    char effectId[4];       // IEffect::id(); all zero with kPresetInsertNone
    uint8_t set[6];         // its settings (audio::EffectSettings)
    uint8_t rsv[4];
};
static_assert(sizeof(PresetInsert) == 16, "PresetInsert layout");

// kTagPresetMix payload: the mix a synth preset carries (one chunk per
// preset that has one; a preset without one keeps the slot's mix). The
// values have the slot record's ranges (spec 21 §3.3); a chunk outside them
// is ignored.
// [3.x] kPresetMixPoly: polyphony (0..8) is the preset's; without it (an
// older chunk: flags 0) the preset keeps the slot's.
constexpr uint8_t kPresetMixPoly = 0x01;
struct PresetMix {
    uint8_t preset;         // 1..8
    uint8_t flags;          // kPresetMixPoly; other bits 0
    uint8_t polyphony;      // 0..8 with kPresetMixPoly, else 0
    uint8_t rsv;
    float gainDb;           // -96..12
    float pan;              // -100..100
    float reverbSend;       // 0..100
    float delaySend;        // 0..100
};
static_assert(sizeof(PresetMix) == 20, "PresetMix layout");

// [3.x] kTagPresetName payload: the patch name of a synth preset (one chunk
// per preset that has one), NUL-padded.
struct PresetName {
    uint8_t preset;         // 1..8
    uint8_t rsv[3];
    char name[32];
};
static_assert(sizeof(PresetName) == 36, "PresetName layout");

struct ModEntry {   // kTagMods entry
    char id[4];
    uint16_t version;
    uint8_t enabled;
    uint8_t rsv;
};
static_assert(sizeof(ModEntry) == 8, "ModEntry layout");

struct ModuleChunkHeader {   // first 8 bytes of a module chunk payload
    char id[4];
    uint16_t version;
    uint16_t rsv;
};
static_assert(sizeof(ModuleChunkHeader) == 8, "ModuleChunkHeader layout");

enum class Error : uint8_t { kOk = 0, kTooSmall, kTooLarge, kMagic, kKind, kSize, kCrc, kChunk, kNewer };
const char* errorText(Error e);

struct Chunk {
    uint16_t tag;
    uint16_t flags;
    uint32_t len;
    const uint8_t* payload;
};

class Reader {
public:
    // Checks magic, kind, size and CRC. kNewer: a newer format version (the
    // chunks are still readable; the caller must not re-save).
    Error open(const uint8_t* file, uint32_t len, uint16_t kind);
    // The next chunk; false at the end or on a malformed chunk (error() says).
    bool next(Chunk& c);
    Error error() const { return err_; }
    uint16_t version() const { return version_; }
    const uint8_t* writer() const { return writer_; }

private:
    const uint8_t* p_ = nullptr;
    uint32_t pos_ = 0, end_ = 0;
    uint16_t version_ = 0;
    uint8_t writer_[4] = {};
    Error err_ = Error::kOk;
};

class Writer {
public:
    void begin(uint8_t* buf, uint32_t cap, uint16_t kind, const uint8_t writer[4]);
    bool chunk(uint16_t tag, uint16_t flags, const void* payload, uint32_t len);
    // Two-part payload (a module chunk header + the module's bytes).
    bool chunk2(uint16_t tag, uint16_t flags, const void* a, uint32_t alen, const void* b, uint32_t blen);
    // Size with the CRC trailer; 0 when anything overflowed.
    uint32_t end();

private:
    uint8_t* buf_ = nullptr;
    uint32_t cap_ = 0, pos_ = 0;
    bool ok_ = false;
};

}  // namespace ot3
}  // namespace ot
