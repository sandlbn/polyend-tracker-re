// Project save/open helpers on top of App (spec 05 appendix E1.3, E2, E3;
// spec 02 §1.1, §7, §8.2; spec 10 §4).
#pragma once

#include <cstdint>

#include "formats/wav.h"
#include "hal_api.h"
#include "model/instrument.h"
#include "model/project.h"

namespace ot {

class App;
struct Recents;

// ---- background project operations (spec 05 appendix E3/E4) -------------
// Stock runs open / save / workspace load / autosave as the asynchronous
// project-manager state machine 0x1d5e0: one bounded step per main-loop pass
// while a progress popup is shown (spec 05 §1.3 "progress popup"), so the UI,
// the audio and the sequencer clock keep running. OpenTracker does the same:
// App::serviceProjectJob() runs job units until kJobPassUs of wall time
// (IClock) are used, and does not start a unit the previous one says would
// overrun the budget. A unit is one small file, one directory scan, or one
// kJobChunk piece of a large file (aligned to kJobChunk file offsets, i.e. to
// FAT clusters of up to 32 KB, so FatFs reads/writes whole clusters with one
// multi-block command straight to/from the sample bank). kJobPassBytes only
// backs the time budget up when the clock does not advance (host tests).
enum class JobOp : uint8_t { None = 0, LoadWorkspace, Open, Save, Autosave, External };

// External job (File > Import Mod / Export As .it, spec 11 §0.1): unit()
// does one bounded piece of work and adds its byte cost; it returns 1 while
// more work remains, 0 when finished, -1 on failure. percent() feeds the
// progress popup.
using JobUnitFn = int (*)(void* ctx, uint32_t& cost);
using JobPercentFn = int (*)(void* ctx);

constexpr uint32_t kJobChunk = 0x8000;        // bytes per chunked read/write unit (cluster aligned)
constexpr uint32_t kJobPassBytes = 0x40000;   // backstop per main-loop pass (frozen clock)
constexpr uint32_t kJobPassUs = 8000;         // wall-time budget per pass
constexpr uint32_t kJobSmallCost = 0x800;     // a small-file operation
constexpr uint32_t kJobProbeCost = 0x40;      // an existence check of a missing file

// Per-job profile (performance workstream telemetry): wall time per
// category, from IClock::micros() or, when the HAL installs one with
// setJobProfileCycles(), a cycle counter (DWT CYCCNT). Reset when a job
// starts; jobProfile() keeps the last job's numbers after it ends.
struct JobProfile {
    uint8_t op = 0;               // JobOp of the profiled job
    bool running = false;
    uint32_t passes = 0;
    uint32_t units = 0;
    uint32_t totalUs = 0;         // sum of pass times
    uint32_t maxPassUs = 0;
    uint8_t maxPassStage = 0;     // job stage / phase the longest pass started in
    uint8_t maxPassPhase = 0;
    uint32_t lastPassUs = 0;
    uint32_t maxUnitUs = 0;
    uint8_t maxUnitStage = 0;     // job stage of the longest unit
    // Time per category (us) and call counts.
    uint32_t openUs = 0, readUs = 0, writeUs = 0, dirUs = 0, decodeUs = 0, commitUs = 0;
    uint32_t opens = 0, reads = 0, writes = 0, dirOps = 0;
    uint64_t bytesRead = 0, bytesWritten = 0;
};
const JobProfile& jobProfile();
// Optional high-resolution time source for the profile (device: DWT CYCCNT
// and the core clock in MHz). Null restores IClock::micros().
void setJobProfileCycles(uint32_t (*cycles)(), uint32_t cyclesPerUs);

struct ProjectJob {
    JobOp op = JobOp::None;
    uint8_t stage = 0;
    int idx = 0;                   // item index inside the stage
    uint8_t phase = 0;             // sub-step of a chunked item (0 = not started)
    uint8_t wphase = 0;            // sub-step of a chunked .pti write
    bool ok = true;
    bool popup = false;            // progress popup shown (not for autosave)
    bool opened = false;           // Open: status flags := 0 at the end (E2)
    bool fromProject = false;      // Open: load while copying "<dir>" -> Workspace
    bool sizeMismatch = false;     // legacy Workspace project.mt (spec 10 §1.3)
    uint8_t slotReset = 0;
    int8_t keptPatch = -1;         // Save: first slot whose refused project N.patch was kept         // bit s: slot record s was reset by the load check (spec 21 §3.3)
    bool profiled = false;         // profile reset for this job
    int loadErrors = 0;            // files that exist but could not be read
    const char* title = "";
    char dir[mt::kFolderMax + mt::kNameMax + 2] = {};   // open source / save target
    char wavBase[mt::kFolderMax + mt::kNameMax + 2] = {};   // legacy samples/instrNN.wav root
    char wipe[300] = {};           // current folder of the incremental Workspace wipe
    uint16_t wipeRootLen = 0;      // the wipe stops after removing this prefix of `wipe`
    Project proj;                  // Open: the upgraded project to write (spec 10 §4.2 step 2)
    uint8_t kind[kSampleInstrumentCount] = {};      // 0 none, 1 .pti payload, 2 legacy WAV
    uint32_t fileSize[kSampleInstrumentCount] = {};
    uint32_t dataOff[kSampleInstrumentCount] = {};
    uint8_t modes[kSampleInstrumentCount] = {};
    int listCount = 0;
    uint32_t wipeCount = 0;        // names of the current wipe batch (in the scratch buffer)
    // Chunked item state.
    IStorage::Handle in = -1, out = -1;
    uint32_t pos = 0, len = 0;
    uint32_t capOff = 0, capLen = 0;   // file range read straight into the sample bank
    const int16_t* src = nullptr;  // autosave/import: samples being written
    uint32_t srcFrames = 0;
    bool srcStereo = false;
    bool sampleDirty = false;
    WavImporter wav;
    Instrument ins;
    // Progress (bytes of work, estimated at the start).
    uint64_t done = 0, total = 1;
    // Time-based pass budget: duration of the last unit.
    uint32_t lastUnitUs = 0;
    // JobOp::External callbacks.
    JobUnitFn extUnit = nullptr;
    JobPercentFn extPercent = nullptr;
    void* extCtx = nullptr;
    // Called once when the job ends (after the job state is cleared).
    void (*onDone)(void* ctx, bool ok) = nullptr;
    void* ctx = nullptr;
};

constexpr const char* kRecentsPath = "Projects/recents";

// spec 05 appendix E1.3: Save writes straight into "<folder>/<name>" only when
// the project is not flagged never-saved AND "<folder>/<name>/project.mt"
// exists; otherwise Save behaves like Save As.
bool canDirectSave(App& a);

// spec 05 appendix E3 SAVE AS (0x1d428 with a (folder, name) pair): set folder
// and name, flush the Workspace and copy it into "<folder>/<name>".
// App::saveProjectAs(name) = this with the browser root "Projects";
// App::saveProject() = this with the project's own folder and name.
bool saveProjectInto(App& a, const char* folder, const char* name);
// Background variant (File screen): returns false if it cannot start; else
// `done(ctx, ok)` runs when the save has finished.
bool startSaveProjectInto(App& a, const char* folder, const char* name, void (*done)(void*, bool), void* ctx);
inline bool saveProjectAs(App& a, const char* folder, const char* name) { return saveProjectInto(a, folder, name); }

// ---- memory pre-check (spec 02 §1.1 / §8.2, spec 10 §4.2 step 0) --------
// Sample bytes the saved project "<dir>" (= "<folder>/<name>") needs: the
// payload of each "<N> <name>.pti", or for a legacy slot (no .pti for N but
// instrument_NN.mti) the data of samples/instrNN.wav.
uint64_t projectSampleBytes(App& a, const char* dir);
// True when that sum fits the sample bank (0xB40000 bytes on the device).
bool projectFitsMemory(App& a, const char* folder, const char* name);

// ---- Projects/recents (spec 02 §7, spec 05 appendix E2) ------------------
// Load and compact: entries whose "<path>/project.mt" no longer exists are
// removed (0x236e0). Returns the number of valid entries (slots 0..n-1).
int loadRecents(App& a, Recents& r);
// The one Recents list for loadRecents() callers (SDRAM, not the stack):
// load, use, done; never held across a call that may load it again.
Recents& recentsBuffer();
// When a project is left (new / open / save as): the CURRENT project's path,
// if it exists on the card, moves to the top; file rewritten (0x23784).
void recentsLeaveCurrent(App& a);
// Open Recent entry `index` (0x238fc). False if it is gone or fails.
bool openRecent(App& a, int index);
bool startOpenRecent(App& a, int index, void (*done)(void*, bool), void* ctx);
// Split a recents path into folder and name.
bool splitProjectPath(const char* path, char* folder, int folderCap, char* name, int nameCap);

// ---- SD card removed while running (spec 05 §1.5 state 3, appendix A) ----
// Call once per main-loop pass before input/UI. Probes the card every
// 500 ms (spec 05 §1.7 card-detect debounce); while it is missing the
// sequencer is stopped and "Please insert SD card to continue" is shown, and
// true is returned (the caller skips the rest of the pass). When the card
// comes back the last project (Workspace) is reopened like at boot.
bool serviceCardRemoved(App& a, uint32_t now);
bool cardMissing();

}  // namespace ot
