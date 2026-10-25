// Global (non-project) settings (spec 05 §5.4, spec 06 §7).
//
// Persisted like the stock firmware: one 0xF4-byte config image in the
// emulated EEPROM app area, EEPROM offset = config offset (spec 01 §10,
// spec 12 §A.2), behind the HAL INvStore. Field offsets in the image are the
// stock RAM offsets from 0x20005C60 given in spec 05 §5.4 / spec 03 §4.1.
// Bytes whose meaning the spec does not give are preserved as read.
//
// Without an INvStore (host tests) the image is kept in a file instead
// (kConfigPath). An older OpenTracker config file found there is imported
// once when the EEPROM holds no valid config (spec 05 §5.4 migration, P15).
#pragma once

#include <cstdint>

#include "hal_api.h"

namespace ot {

struct Config {
    // --- typed fields (order kept: the v1 config file is a copy of them) ---
    // General
    uint8_t radioRegion = 0;       // 0..3
    uint8_t brightness = 2;        // 0 low, 1 medium, 2 high
    uint8_t theme = 0;             // 0..2
    uint8_t font = 0;              // 0..1
    uint8_t padsBrightness = 2;    // 0..2
    uint8_t padsPreview = 2;       // 0 off, 1 track, 2 pattern
    uint8_t recordingOptions = 0;  // 0..3
    uint8_t numbering = 1;         // 0 dec from 0, 1 dec from 1, 2 hex
    uint8_t arrangement = 0;       // 0 revolver, 1 original, 2 horizontal
    uint8_t patternTopInfo = 0;    // 0 info, 1 track names, 2 off
    uint8_t lineInChannel = 0;     // 0 stereo, 1 L, 2 R
    uint8_t perfPresets = 0;       // 0 global, 1 per project
    // Metronome
    uint8_t metronome = 0;
    uint8_t preRoll = 0;
    uint8_t tsNum = 3;             // 0..11 = 1..12
    uint8_t tsDen = 3;
    uint8_t metronomeVolume = 50;  // 0..100
    // MIDI
    uint8_t clockIn = 0;           // 0 internal, 1 USB, 2 DIN
    uint8_t clockOut = 0;          // 0 off, 1 USB, 2 DIN, 3 both
    uint8_t transportIn = 0;
    uint8_t transportOut = 0;
    uint8_t notesIn = 0;
    uint8_t notesInChannel = 0;    // 0 all, 1..16
    uint8_t midiOut = 0;
    uint8_t ccIn = 0;
    uint8_t ccInChannel = 0;
    uint8_t middleC = 2;           // 0..3
    uint8_t clockCorrection = 6;   // 0..12 -> -6..+6
    // Audio / other
    uint8_t masterVolume = 81;     // 0..100 (config 0x1B, default 0x51)
    uint8_t previewVolume = 86;    // 0..100 (config 0xD7, default 0x56)
    uint8_t sliceCount = 8;        // 1..48 (config 0xF2 "No of Slices")
    // Sample Recorder settings. Per project in stock (project.mt 0x134..0x13C,
    // spec 05 appendix C §2.1): these are the working copies the recorder
    // screen edits; App::syncRecorderSettings() keeps them and the project
    // bytes in step. They are not part of the config image.
    uint8_t recSource = 0;         // 0 line in, 1 radio, 2 mic low gain, 3 mic high gain
    uint8_t recGain[4] = {50, 50, 50, 50};   // per source 0..100 (line, radio, mic LG, mic HG)
    uint8_t recMonitor = 1;        // 0 off, 1 on
    uint16_t radioFreq10 = 875;    // MHz x 10, default 87.5 (appendix C §2.8)
    // 2.x (spec 2x-02 §2.2): Config > General "CPU meter". Not in the stock
    // image: kept in the extension record after it (kExtOffset).
    uint8_t cpuMeter = 0;          // 0 Auto, 1 Always, 2 Off
    // 2.x synth voice budget B (OPTIMIZATION_2X item 10 / OP-8): voices the
    // governor starts from, 1..8. Extension record byte 2.
    uint8_t synthVoices = kDefaultSynthVoices;
    static constexpr uint8_t kDefaultSynthVoices = 4;
    static constexpr uint8_t kMaxSynthVoices = 8;
    // [I] 2.x Config > General "Export sample rate": 1 = 44.1 kHz (the render
    // converted, 44100 in the header; default), 0 = the device rate with
    // 44117 in the header, as stock (spec 04 §6.1). Extension record byte 6.
    uint8_t exportRate = 1;
    // Extension record bytes 3..5: options of later 2.x builds this one does
    // not know yet; read and written back unchanged (their defaults).
    uint8_t extKeep[3] = {0, 1, 0};

    // --- the stock config image --------------------------------------------
    static constexpr uint32_t kImageSize = 0xF4;   // spec 05 §5.4 version word 0xf4
    // Image offsets (spec 05 §5.4 table / spec 03 §4.1 / spec 12 §A.2).
    static constexpr uint32_t kOffVersion = 0x08;   // u16 0x0A00 (written on a fresh image)
    static constexpr uint32_t kOffSize = 0x0C;      // u32 0xF4 (written on a fresh image)
    static constexpr uint32_t kOffPerfPatterns = 0x38;   // u16[8], vestigial (spec 12 §A.4)
    static constexpr uint32_t kOffPerfTypes = 0x48;      // u8[12]
    static constexpr uint32_t kOffPerfValues = 0x54;     // i16[12][4]
    static constexpr uint32_t kOffPerfRows = 0xB4;       // u8[12]
    static constexpr uint32_t kOffPerfReserved = 0xC0;   // u8[8]
    uint8_t image[kImageSize];

    // --- 2.x extension record (M1 plan, spec gap 7) -------------------------
    // Stored right after the stock image (store offset kImageSize, file: after
    // the image), so the stock bytes stay byte-exact and a stock firmware
    // ignores it: "OTX" magic, version, payload length, payload, 8-bit sum of
    // the payload. A missing or bad record leaves the defaults.
    static constexpr uint32_t kExtOffset = kImageSize;
    static constexpr uint8_t kExtVersion = 4;
    static constexpr uint32_t kExtPayload = 6;               // cpuMeter, synthVoices, extKeep[3], exportRate
    static constexpr uint32_t kExtSize = 5 + kExtPayload + 1;
    void toExt(uint8_t* out) const;                          // kExtSize bytes
    bool fromExt(const uint8_t* in, uint32_t len);           // false: none / bad (fields unchanged)

    Config();
    void setDefaults();
    void sanitize();

    // Pad LED levels (dim, full) for the pads-brightness setting (spec 05 §2.1.3).
    uint8_t padDim() const;
    uint8_t padFull() const;

    // Image <-> fields. fromImage() returns false (and changes nothing) when
    // the "EEPROM" signature is missing; otherwise it validates the image like
    // configValidate (spec 05 §5.4, spec 12 §A.2) and loads the fields.
    void toImage();
    bool fromImage(const uint8_t* img);

    // Non-volatile store (device EEPROM / sim file). save() writes only the
    // bytes that differ from what the store holds.
    bool load(INvStore& nv);
    bool save(INvStore& nv);

    // File fallback (no INvStore) and the one-time import of an older
    // OpenTracker config file (format v1 = typed fields only).
    bool load(IStorage& fs);
    bool save(IStorage& fs);

    // Global Performance presets (spec 12 §A.2) live in the image.
    uint8_t* perfTypes() { return image + kOffPerfTypes; }
    uint8_t* perfValues() { return image + kOffPerfValues; }
    uint8_t* perfRows() { return image + kOffPerfRows; }
};

constexpr const char* kConfigDir = "OpenTracker";
constexpr const char* kConfigPath = "OpenTracker/config.bin";

}  // namespace ot
