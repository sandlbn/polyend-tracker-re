// OpenTracker 2.x test-song generator (reimpl/tools/make_testsongs).
//
// Builds hardware-test projects through a running App, so every file is
// written by the same code that loads it on the device (like make_demo).
// All audio is synthesised here (sine, saw, noise, FM, simple drum models).
#pragma once

#include <cstdint>
#include <string>

namespace ot {
class App;
class IStorage;
}

namespace ts {

enum class SongId : uint8_t {
    TwelveTracks = 1,   // stock 1.9.2 / 1.9.3 compatible
    SixteenTracks = 2,  // M1
    LoadStress = 3,     // M0 / M1
    Stereo = 4,         // M2
    Synths = 5,         // M3 (+ M1 for tracks 13-16)
    // BLAST GRID remixes on the synth engines (bgsongs.inc): M3 + M1.
    BgTitle = 6,
    BgWorld1 = 7,
    BgWorld4 = 8,
    BgBattle = 9,
    BgBattleFm = 10,
    BgBoss = 11,
    BgFinalBoss = 12,
    // PERC engine only, three kits (percsong.inc): M3 + M1.
    PercOnly = 13,
    // VAP engine only, three patches (vapsong.inc): M3 + M1.
    VapOnly = 14,
    // Track Mixer meter calibration, stock-compatible (metersong.inc).
    MeterCheck = 15,
    // FAT engine only, three patches (fatsong.inc): 2.1 (FAT) + M1.
    FatOnly = 16,
    // WTFM engine only, three patches (wtfmsong.inc): 2.2 (WTFM) + M1.
    WtfmOnly = 17,
    // PERC + ACD + WTFM with dense sequencer FX (idmsong.inc): 2.2 + M1.
    Idm = 18,
};
constexpr int kSongCount = 18;

// Songs 6-12 (the BLAST GRID remixes).
inline bool isBgSong(SongId id) { return int(id) >= int(SongId::BgTitle) && int(id) <= int(SongId::BgFinalBoss); }

struct SongInfo {
    SongId id;
    const char* name;        // project name (<= 32 chars, spec 02 §1.1)
    const char* milestone;   // what the song needs
    bool available;          // this build supports it (features.h)
};

// Song table, index 0..kSongCount-1.
const SongInfo& songInfo(int index);
const SongInfo* findSong(SongId id);

struct GenOptions {
    // Generate a song whose milestone is not built in (only the parts this
    // build can express are written). Used by the host test for the stereo
    // song, whose files are stock-format already.
    bool force = false;
    // Folder for the generated source WAVs on the card.
    const char* samplesRoot = "Samples/OT Test Songs";
};

// Generates one song: new project, source samples under samplesRoot, imported
// instruments, patterns, song, mixer / Performance settings, then Save As
// Projects/<name>. The App must be initialised and booted on `fs`.
bool generateSong(ot::App& app, ot::IStorage& fs, SongId id, const GenOptions& opt, std::string& err);

// Byte-level check of a saved project folder: would the firmware load it
// without changing or rejecting anything? mini = false: stock 1.9.2 rules
// (docs/specs/02: container, load sanitiser ranges, 12-track patterns, step
// encoding, no other files). mini = true: the Tracker Mini format OpenTracker
// 2.x writes (docs/specs/21: 16-track 0x3030 patterns, instruments up to 66,
// valid synth-slot records, the three N.patch files). Returns an empty
// string when clean, else the first problems found.
std::string checkStockProject(ot::IStorage& fs, const char* projectDir, bool mini = false);

}  // namespace ts
