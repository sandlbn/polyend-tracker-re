// OpenTracker application object: owns the project model, sequencer, audio
// engine (spec 04), storage/workspace handling and the UI (spec 05 §1.1-§1.5,
// spec 02 §1.1).
#pragma once

#include <cstdint>

#include "app/capture.h"
#include "app/resample.h"
#include "audio/stream_source.h"
#include "app/config.h"
#include "app/midi_cc.h"
#include "app/pattern_ops.h"
#include "app/project_io.h"
#include "app/stream_policy.h"
#include "app/undo.h"
#include "audio/engine.h"
#include "hal_api.h"
#include "model/instrument.h"
#include "model/pattern.h"
#include "model/pattern_store.h"
#include "model/project.h"
#include "model/synth_patch.h"
#include "sequencer/sequencer.h"
#include "ui/notes.h"
#include "ui/ui.h"

namespace ot {

// Interfaces handed to the app (mirrors hal::HalSet without depending on src/hal).
struct HalRefs {
    IDisplay* display = nullptr;
    IAudioOut* audio = nullptr;
    IStorage* storage = nullptr;
    IMidiPort* midi = nullptr;
    IInput* input = nullptr;
    ILeds* leds = nullptr;
    IClock* clock = nullptr;
    void (*service)() = nullptr;
    // Firmware update hand-off (injected by the device glue; null on host).
    bool (*isUpdateImageName)(const char* name) = nullptr;
    bool (*requestFirmwareUpdate)(IStorage& fs, const char* name) = nullptr;
    // Line input for the next audio block (128 interleaved frames) or nullptr;
    // called from the audio-block context. Null when the board has no capture.
    const int16_t* (*lineIn)(void* ctx) = nullptr;
    void* lineInCtx = nullptr;
    // Recorder input routing (spec 05 appendix C §2.4/§2.5): source 0 line
    // in, 1 radio, 2 mic low gain, 3 mic high gain; gain 0..100. Returns
    // false when the source is not available. Null: only line in (no
    // hardware gain; OpenTracker applies the gain digitally either way).
    bool (*selectInput)(void* ctx, uint8_t source, uint8_t gain) = nullptr;
    void* inputCtx = nullptr;
    IRadio* radio = nullptr;               // null = no tuner driver
    // Global config image store (spec 05 §5.4 EEPROM). Null: the image is
    // kept in kConfigPath on the card instead.
    INvStore* nvstore = nullptr;
    // 2.x USB Storage mode (Config > USB > USB Storage). Null: not available
    // in this build (OT_USB_MSC=0).
    IUsbStorage* usbStorage = nullptr;
    // SD streaming (docs/PLAN_SD_STREAMING_P1C_P2.md): reads streamed sample
    // data from the card (device: raw sectors; host / sim: IStorage). Null:
    // streaming not available (the option reads as Off).
    audio::IStreamReader* streamReader = nullptr;
};

// Large buffers (SDRAM on the device, spec 02 §8.2 for the sample bank).
struct AppMemory {
    int16_t* sampleBank = nullptr;
    uint32_t sampleBankSamples = 0;
    int16_t* fxMemory = nullptr;          // delay lines + reverb rings (spec 04 §5.1, §5.2)
    uint32_t fxSamples = 0;               // >= audio::AudioEngine::kFxMemorySamples
    PatternStock* patterns = nullptr;     // kPatternCount entries; pattern n at [n-1] (tracks 0..11)
    // 2.x ext tracks 12..15 (model/pattern_store.h): extTrackBlocks track
    // records + ExtTrackPool::metaBytes(extTrackBlocks - 1) bytes of index.
    // The last record is the write target that storedTrack() hands out when
    // the pool is full, so the pool holds extTrackBlocks - 1 tracks.
    // Required (>= 2 records), like seqBuffers.
    Track* extTracks = nullptr;
    int extTrackBlocks = 0;               // ExtTrackPool::kDefaultBlocks + 1 on the device
    uint8_t* extMeta = nullptr;
    Pattern* seqBuffers = nullptr;        // 2 entries: sequencer current + preload (16 tracks)
    // Optional third sequencer buffer: switchPatternNow() assembles the new
    // pattern there in the main loop (no pattern copy in the audio block).
    Pattern* seqStage = nullptr;
    Pattern* undo = nullptr;              // undoSlots entries (spec 05 §5.1: 20)
    uint8_t* undoNums = nullptr;
    int undoSlots = 0;
    Pattern* fileUndo = nullptr;          // spec 05 §5.1 (b): 10
    uint8_t* fileUndoNums = nullptr;
    int fileUndoSlots = 0;
    Pattern* clipboard = nullptr;
    uint8_t* scratch = nullptr;           // file copy / decode buffer
    uint32_t scratchBytes = 0;
    PerfPatternMemory* perfPatterns = nullptr;   // per-track pattern overrides (spec 12 §C.1)
    audio::StemBlock* exportStems = nullptr;     // optional: WAV export stem buffer (else allocated per export)
};

// Main-loop members of App that live in the SDRAM on the device (CPU round 3,
// docs/OPTIMIZATION_2X.md "SRAM map"): a reference to storage in
// `.sdram_bss` (core/sdram_bss.h), constructed in App::init() after
// otAppMain() zeroed that section; host builds keep a plain member. Never
// touched by the audio block, an ISR or the boot code.
#if defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE
#define OT_APP_SDRAM_MEMBER(T, name) T& name
#else
#define OT_APP_SDRAM_MEMBER(T, name) T name
#endif

constexpr const char* kWorkspace = "Workspace";
constexpr const char* kProjectsDir = "Projects";
constexpr const char* kExportDir = "Export";

// SD streaming C14 (sample_import.cpp): how the Sample Loader can import a
// file into a slot.
enum class ImportRoute : uint8_t {
    Fits,        // into the sample memory (App::importSample)
    Streams,     // too big, but converted onto the card and streamed
    Off,         // too big and Stream from card is Off
    TooMany,     // too big: 16 instruments already stream
    NoMemory,    // too big, and not even its streamed head fits
    CardFull,    // the card cannot hold the converted file
    Unreadable,  // not a supported WAV / .pti
};

// What the import route needs of a file (App::importProbe, one header read):
// frames / channels as the import produces them; a .pti's own play mode and
// loop start (a WAV takes the slot's).
struct ImportProbe {
    uint32_t frames = 0;
    bool stereo = false;
    bool pti = false;
    uint8_t playMode = 0;
    uint16_t loopPoint1 = 0;
};

class App final : public ISequencerHost {
public:
    App();
    bool init(const HalRefs& hal, const AppMemory& mem);
    // Mount check, folder layout, config, workspace load or new project.
    bool boot();
    void loop();          // one main-loop pass (spec 05 §1.3 application loop)
    [[noreturn]] void run();
    uint32_t millis() const { return hal_.clock ? hal_.clock->millis() : 0; }
    uint32_t micros() const { return hal_.clock ? hal_.clock->micros() : 0; }

    // Audio: the engine is the IAudioOut callback; at the start of every block
    // it calls preBlock (spec 04 §1.1 step 1), which applies pending mixer
    // changes, hands over line-in and advances the sequencer.
    static void preBlockThunk(void* ctx, float seconds);
    void preBlock(float seconds);
    // Mixer / master / track settings from project + config -> engine.
    void applyEngineSettings();

    // Audition (spec 07 Q7, spec 05 §4.2 pad manager): every held source
    // (voiceId = pad number, or one of the ids below) takes a track voice:
    // starting at the pattern-editor cursor track, scanning 12 tracks with
    // wrap, the first track that is not muted, holds no other audition note
    // and (sample instruments) is < 8. Sample instruments release the track's
    // note and start the audition note on the track voice (up to 8 at once);
    // MIDI instruments (>= 48) send a note on their channel. Release = normal
    // envelope release (note-off mode -4). forceTrack >= 0 plays on that
    // track only (pattern editor note entry in edit mode).
    static constexpr uint16_t kAuditionLoader = 0x60;        // Sample Loader F3 (07 Q7)
    static constexpr uint16_t kAuditionInstrument = 0x61;    // Instrument screen preview
    static constexpr uint16_t kAuditionEncoder = 0x62;       // pattern editor note encoder
    static constexpr uint16_t kAuditionSampleEditor = 0x63;  // sample editor preview
    static constexpr uint16_t kAuditionMidiIn = 0x100;       // + MIDI note number
    static constexpr uint16_t kAuditionNone = 0xFFFF;
    // Tracks the audition search scans (stock 12; 2.x: all 16, so note entry
    // on tracks 13..16 sounds like on 9..12, 2x-01 §2.6).
    static constexpr int kAuditionTracks = kTrackCount;
    // Returns the track used, or -1 when no track was free.
    // startTrack >= 0 starts the search there instead of editorTrack (MIDI
    // input: the pattern-editor target rectangle, spec 17 §1.4 step 4).
    int previewOn(uint16_t voiceId, uint8_t instrument, uint8_t note, uint8_t velocity, int forceTrack = -1,
                  int startTrack = -1);
    void previewOff(uint16_t voiceId, int startTrack = -1);
    // Live velocity (spec 17 §1.4 step 1): 127 unless Recording options is a
    // Velocity mode. Live volume vel * IV / 127 (step 7, FW:0x1191c).
    uint8_t liveVelocity(uint8_t velocity) const;
    uint8_t liveVolume(uint8_t instrument, uint8_t vel) const;
    void previewAllOff();
    // Track holding audition voiceId, or -1.
    int auditionTrack(uint16_t voiceId, int from = -1) const;
    // Pattern-editor cursor track: start of the audition track search.
    uint8_t editorTrack = 0;
    // Sample loader file preview (spec 04 §1.3 WAVPREVIEW); null stops it.
    void previewFile(const int16_t* data, uint32_t frames, bool stereo) { engine.previewBuffer(data, frames, stereo); }
    // Pitched preview (2.x): planar lanes (right nullptr = mono) at note
    // `note` (60 = own pitch) through the same player.
    void previewPitched(const int16_t* left, const int16_t* right, uint32_t frames, int note) {
        engine.previewBufferLR(left, right, frames, previewStep(note));
    }
    // 16.16 player step for `note` relative to 60 (fm::kNoteRatio).
    static uint32_t previewStep(int note);
    // Stop every voice (also before any sample-bank move: voices point into it).
    void allVoicesOff();

    // ---- model -----------------------------------------------------------
    Project project;
    OT_APP_SDRAM_MEMBER(PatternNames, names);   // main loop only (UI, project files)
    Instrument instruments[kSampleInstrumentCount];
    bool instrPresent[kSampleInstrumentCount] = {};
    audio::AudioEngine engine;
    audio::SampleBank& bank;          // engine.bank(): spec 02 §8.2 packing
    Config cfg;
    Sequencer seq;
    PadNotes padNotes;
    UndoRing undo;
    FileUndo fileUndo;
    Ui ui;

    IStorage& fs() { return *hal_.storage; }
    const HalRefs& hal() const { return hal_; }
    const AppMemory& mem() const { return mem_; }

    // ---- patterns ----------------------------------------------------------
    // pattern(): the working (current) pattern, all 16 tracks.
    Pattern& pattern() { return *seq.currentPattern(); }
    uint8_t patternNumber() const { return seq.currentPatternNumber(); }

    // ---- resident pattern store (2.x M1-WP1, docs/M1_PLAN.md §1) ----------
    // Pattern num 1..255. Tracks 0..11 are always resident (storedStock, the
    // .mtp image); tracks 12..15 live in the ext-track pool and take a block
    // only while they hold data. An ext track without a block reads as
    // defaultExtTrack() (empty, stock track 0's length).
    PatternStock& storedStock(int num) { return mem_.patterns[num - 1]; }
    const PatternStock& storedStock(int num) const { return mem_.patterns[num - 1]; }
    // 1.9.3 name of storedStock(): tracks 0..11 only. New code that may touch
    // tracks 12..15 uses storedTrack()/storePattern()/assemblePattern().
    PatternStock& stored(int num) { return storedStock(num); }
    // Write access to track t (0..15) of pattern num; t >= 12 allocates a
    // pool block (initialised to the default ext track). Pool full: reported
    // (notice, extPoolFullEvents()) and a scratch track is returned whose
    // contents are discarded -- never a crash. Check storedTrackWritable()
    // first when the caller can refuse the edit instead.
    Track& storedTrack(int num, int t);
    bool storedTrackWritable(int num, int t) const;
    // Read access without allocation: nullptr for an ext track with no block.
    const Track* findStoredTrack(int num, int t) const;
    // Copy of track t (default ext track when it has no block).
    void readStoredTrack(int num, int t, Track& out) const;
    // Whole working pattern -> store: tracks 0..11 into storedStock, 12..15
    // into existing blocks, or new blocks when not default. Blocks of tracks
    // that became empty are kept until compactExtTracks(). False = pool full
    // (the ext tracks that did not fit are not stored; reported).
    bool storePattern(int num, const Pattern& p);
    // storePattern(patternNumber(), pattern()).
    bool commitCurrentPattern() { return storePattern(patternNumber(), pattern()); }
    // Store -> 16-track pattern (no present check; see loadPattern).
    void assemblePattern(int num, Pattern& dst) const;
    // Frees the blocks of ext tracks that are default again (pattern num, or
    // every pattern when num == 0). Main loop; the autosave calls it for the
    // patterns it writes. Returns the number of blocks freed.
    int compactExtTracks(int num = 0);
    void clearExtTracks(int num);                 // drop all ext tracks of num
    // Pattern num back to the project default (stock tracks + ext tracks).
    void defaultStored(int num);
    // Every pattern default, pool emptied (new project / project load).
    void resetPatternStore();
    const ExtTrackPool& extPool() const { return ext_; }
    uint32_t extPoolFullEvents() const { return ext_.fullEvents(); }
    // File undo (spec 06 §2.3 F3) of a stored pattern, all 16 tracks,
    // without a stack copy: push the store's pattern num / pop into the store.
    void pushFileUndo(int num);
    bool popFileUndo(uint8_t* num);
    bool patternPresent(int num) const { return num >= 1 && num <= kPatternCount && (present_[num >> 3] >> (num & 7)) & 1; }
    bool patternHasData(int num);
    // After editing the current pattern: copy into the store and mark it dirty.
    void patternEdited();
    void markPatternDirty(int num);
    // Change the current (edited) pattern; queues while playing (spec 06 §1.7 focus 0).
    void selectPattern(int num);
    // Pattern + Up/Down (spec 16 §A.2, FW:0x3d548): change the current pattern
    // at once, also while playing (step position kept).
    void switchPatternNow(int num);
    // The pattern the editor shows next: a pending immediate switch, else current.
    int effectivePatternNumber() const { return seq.pendingSwitch() ? seq.pendingSwitch() : patternNumber(); }
    int firstEmptyPattern();
    void reloadCurrentFromStore();

    // Undo (spec 05 §5.1).
    void pushUndo();
    bool undoEdit();
    bool redoEdit();

    // Clipboard (spec 06 §1.4 Copy / §1.11).
    void copyToClipboard(const SelRect& r);
    bool clipboardValid() const { return clipValid_; }
    const SelRect& clipRect() const { return clipRect_; }
    const Pattern& clipboard() const { return *mem_.clipboard; }

    // ---- project settings shortcuts ---------------------------------------
    uint8_t currentInstrument() const { return project.u8(0x111); }
    void setCurrentInstrument(int i);
    uint8_t scale() const { return project.u8(0x115); }
    uint8_t rootNote() const { return project.u8(0x117); }
    uint8_t stepJump() const;
    void setStepJump(int v);
    bool trackMuted(int t) const;
    void setTrackMute(int t, bool m);
    uint8_t padNote(uint8_t pad);                 // current scale map, slice-aware
    void refreshPadNotes();
    void applySequencerSettings();

    // ---- dirty state / autosave (spec 02 §1.1) -----------------------------
    void markProjectDirty() { projectDirty_ = true; unsaved_ = true; engineDirty_ = true; }
    void markNamesDirty() { namesDirty_ = true; unsaved_ = true; }
    void markInstrumentDirty(int i, bool sample = false);
    void markConfigDirty() { configDirty_ = true; engineDirty_ = true; }
    // Config persistence (spec 05 §5.4): the dirty config is written at the
    // next 5 s boundary; force = on the next loop pass (spec 12 §A.5 leave).
    void requestConfigSave(bool force) { markConfigDirty(); if (force) configForce_ = true; }
    bool saveConfig();                            // INvStore, else kConfigPath
    void loadConfig();                            // boot: EEPROM, else import the file once
    void setNvStore(INvStore* nv) { hal_.nvstore = nv; }

    // ---- 2.x theme files from the card (ui/theme_file.h) -------------------
    // The stock themes stay in flash; a Themes/<name>.txt chosen in Config is
    // parsed into one SDRAM table and drawn instead while it is active. Its
    // name is kept in cfg.themeFile (cfg.theme = its base) and the file is
    // read again at every boot(); without it the base theme is used.
    // loadCustomTheme: false (nothing changes) when the file does not open.
    bool loadCustomTheme(const char* name);
    void clearCustomTheme();                      // back to the stock cfg.theme
    bool customThemeActive() const { return themeActive_; }
    const char* customThemeName() const { return themeName_; }
    const Theme& activeTheme() const { return themeActive_ ? themeView_ : theme(cfg.theme); }
    const char* activeThemeName() const { return themeActive_ ? themeName_ : theme(cfg.theme).name; }
    // Config > Export theme: Themes/<activeThemeName()>.txt, never overwritten;
    // refused in USB Storage mode.
    enum class ThemeExport : uint8_t { kOk = 0, kExists, kUnavailable, kFailed };
    ThemeExport exportTheme();
    void setUsbStorage(IUsbStorage* u) { hal_.usbStorage = u; }

    // ---- 2.x USB Storage mode (Tracker Mini manual §13.10) ----------------
    // Enter: refused while a project job other than the autosave runs, while
    // the Games borrow the sample memory, or without a card / USB support;
    // otherwise playback and recording stop, the autosave and the config are
    // written, and the card goes to the computer. The app then leaves the
    // card alone (loop() runs only the UI and the USB block I/O) until
    // exitUsbStorage() takes it back and reloads the Workspace like after a
    // card change. Never persisted: every boot starts with the mode off.
    enum class UsbEnter : uint8_t { kOk = 0, kUnavailable, kBusy, kNoCard };
    UsbEnter enterUsbStorage();
    void exitUsbStorage();
    bool usbStorageActive() const { return usbActive_; }

    // ---- SD streaming (docs/PLAN_SD_STREAMING_P1C_P2.md) ----
    // Rings carved from the top of the sample bank when streaming is on
    // (bank cleared; called at New and the project-load stage).
    void applyStreamOption();
    bool streamingOn() const { return stream_.on; }
    bool streamed(int slot) const {
        return slot >= 0 && slot < kSampleInstrumentCount && stream_.streamOf[slot] >= 0;
    }
    int streamedCount() const {
        int n = 0;
        for (int i = 0; i < kSampleInstrumentCount; ++i) n += stream_.streamOf[i] >= 0;
        return n;
    }
    bool instrumentSampleDirty(int slot) const {
        return slot >= 0 && slot < kSampleInstrumentCount && instrSampleDirty_[slot];
    }
    audio::StreamRing* streamRings() { return stream_.rings; }
    const audio::StreamServiceStats& streamStats() const { return stream_.stats; }
    // One telemetry window of the stream service (C11): the stats so far,
    // then a fresh window; the underrun count stays the rings' running total.
    audio::StreamServiceStats takeStreamStats() {
        const audio::StreamServiceStats w = stream_.stats;
        stream_.stats = audio::StreamServiceStats{};
        stream_.stats.underruns = w.underruns;
        return w;
    }
    // Rings playing a stream right now.
    int streamActive() const {
        int n = 0;
        for (const audio::StreamRing& r : stream_.rings) n += r.src != nullptr;
        return n;
    }
    // Every stream unbound (card lost, USB Storage mode, project change).
    void streamUnbindAll();
    // One slot's stream unbound (delete, import, resample into it).
    void streamUnbind(int slot);
    // After a bank move: every streamed slot's head / loop-head pointers again.
    void refreshStreamSources();
    // A streamed instrument loaded whole from its Workspace file (Sample
    // Editor, play modes that need memory). False (nothing changed, notice)
    // when it does not fit.
    bool loadStreamedIntoMemory(int slot);
    // SD streaming C14: the route of importing `path` into `slot` (the
    // slot's current sample replaced), and the import of a file that does
    // not fit: a background job (progress popup, Back cancels) converts it
    // into the slot's Workspace .pti through instruments/import.tmp, then
    // loads its head and streams the rest. The old instrument stays until
    // the new one is in place. False (importNotice() says why) when the
    // route is not Streams or the job could not start.
    // bankBytes (optional): what the slot then holds in the sample memory
    // (the whole sample when it fits, the streamed head when it streams).
    // The route follows Config > Stream from card like a project open
    // (importDecision); why (optional): the reason it streams. The probe
    // form makes no I/O; cardFree UINT64_MAX skips the card-space check;
    // pendingBytes / pendingStreams: what the files placed before this one
    // add to the memory and the streams (the Sample Loader's selection).
    ImportRoute importRoute(const char* path, int slot, uint32_t* bankBytes = nullptr);
    bool importProbe(const char* path, ImportProbe& p);
    ImportRoute importRoute(const ImportProbe& p, int slot, uint32_t* bankBytes, StreamWhy* why,
                            uint64_t cardFree, int64_t pendingBytes = 0, int pendingStreams = 0);
    bool startStreamedImport(const char* path, int slot, void (*done)(void* ctx, bool ok) = nullptr,
                             void* ctx = nullptr);
    const char* importNotice() const { return importNotice_; }
    static constexpr const char* kImportTmp = "Workspace/instruments/import.tmp";
    bool streamTestEnable = false;   // tests: carve the rings without the Config option
    IUsbStorage::Host usbHost() const {
        return hal_.usbStorage ? hal_.usbStorage->host() : IUsbStorage::Host::kNoUsb;
    }
    // Recorder input routing + tuner after init (host tests; the device glue
    // passes them in HalRefs).
    void setInputRouting(bool (*fn)(void*, uint8_t, uint8_t), void* ctx, IRadio* r) {
        hal_.selectInput = fn;
        hal_.inputCtx = ctx;
        hal_.radio = r;
    }
    // Recorder settings live in the project (0x134..0x13C, spec 02 §3.3);
    // cfg.rec* are the working copies. Called every loop pass.
    void syncRecorderSettings();
    bool unsavedChanges() const { return unsaved_; }
    bool neverSaved() const { return neverSaved_; }
    void autosave();                              // writes dirty items to Workspace now
    uint32_t lastAutosave() const { return lastAutosave_; }

    // ---- synth slots (Tracker Mini, spec 21 §3.3, §7; docs/MINI_PLAN.md §1)
    // Slot s = 0..2 is the project.mt record (project.synthSlot(s)) plus
    // its patch. Open copies <project>/N.patch to Workspace/N.patch and
    // loads it; failing that the library Patches/<ENGINE>/<record name>.patch,
    // then the engine's Init patch. Save writes all three patches; the
    // autosave rewrites Workspace/N.patch when the slot's patch is dirty.
    enum class PatchSource : uint8_t { None = 0, File, Library, Init };
    SynthSlotRecord synthSlot(int s) const { return project.synthSlot(s); }
    SynthPatch& synthPatch(int s) { return patches_[s < 0 || s >= kSynthSlotCount ? 0 : s]; }
    const SynthPatch& synthPatch(int s) const { return patches_[s < 0 || s >= kSynthSlotCount ? 0 : s]; }
    PatchSource synthPatchSource(int s) const { return s >= 0 && s < kSynthSlotCount ? patchSrc_[s] : PatchSource::None; }
    void markSynthPatchDirty(int s) {
        if (s < 0 || s >= kSynthSlotCount) return;
        patchDirty_[s] = true;
        unsaved_ = true;
        engine.synth().patchChanged(s);   // edits apply live (audio picks them up next block)
    }
    bool synthPatchDirty(int s) const { return s >= 0 && s < kSynthSlotCount && patchDirty_[s]; }
    // Synth screens (WP-G, app/synth_slots.cpp). The record setter stores
    // the project bytes and pushes them to the framework (engine sync).
    void setSynthSlotRecord(int s, const SynthSlotRecord& r);
    // New model for slot s: record model + "Init", the patch = library
    // Patches/<MODEL>/Init.patch, else the built-in Init; macro page values
    // reset to the patch defaults.
    void setSynthModel(int s, int model);
    // Built-in Init patch of the slot's model, record name "Init".
    void initSynthPatch(int s);
    // Patches/<MODEL>/<name>.patch -> slot s (record name = name). False when
    // missing, damaged or of another engine (the slot is unchanged).
    bool loadSynthLibraryPatch(int s, const char* name);
    // Slot s's patch -> Patches/<MODEL>/<name>.patch (atomic; folders made),
    // record name = name.
    bool saveSynthLibraryPatch(int s, const char* name);
    // Load Patch popup preview (Tracker+ 1.0; docs/SYNTH_UX_MINI_VS_OT.md
    // row 24): Patches/<MODEL>/<name>.patch plays in slot s (its patch and
    // the audio-side record with the patch's macro defaults) while the
    // project record, the patch source and the dirty flag stay as they were
    // (no autosave of a previewed patch). The first preview keeps a copy of
    // the slot's patch; endSynthPreview() puts it back byte for byte. False
    // (slot unchanged) when the file is missing, damaged or another engine's.
    bool previewSynthLibraryPatch(int s, const char* name);
    void endSynthPreview();
    int synthPreviewSlot() const { return previewSlot_; }
    // The load notice of a project whose slot has no engine in this build
    // ("Synth k (WTFM) not available", the first such slot; MINI_PLAN §3).
    // False (out untouched) when every slot is available.
    bool synthUnavailableNotice(char* out, size_t cap) const;

    // ---- project operations (project_io.cpp) ------------------------------
    // The blocking calls below run the same background job as the start*()
    // variants to completion (power-off, boot, tests).
    bool newProject();
    bool loadWorkspace();
    bool saveProject();                           // to <folder>/<name>
    bool saveProjectAs(const char* name);
    bool openProject(const char* folder, const char* name);
    // Background jobs (spec 05 appendix E3): false = not started. `done`
    // runs from loop() once the job has finished.
    using JobDoneFn = void (*)(void* ctx, bool ok);
    bool startOpenProject(const char* folder, const char* name, JobDoneFn done = nullptr, void* ctx = nullptr);
    bool startSaveProjectAs(const char* nameOrPath, JobDoneFn done = nullptr, void* ctx = nullptr);
    bool startSaveProject(JobDoneFn done = nullptr, void* ctx = nullptr);
    bool startLoadWorkspace(JobDoneFn done = nullptr, void* ctx = nullptr);
    bool startAutosave();                         // false when nothing is dirty
    // Games (spec 14 §1.6, OpenTracker): true while the sample memory is lent
    // to the NES emulator; the periodic autosave is held so garbage sample
    // data is never written. The lender reloads the Workspace afterwards.
    bool sampleBankLent = false;
    // External background job with the progress popup `title` (spec 11
    // §0.1 import / export state machines); false when another job runs.
    bool startExternalJob(const char* title, JobUnitFn unit, JobPercentFn percent, void* unitCtx,
                          JobDoneFn done = nullptr, void* ctx = nullptr);
    JobOp jobOp() const { return pj_.op; }
    bool jobBusy() const { return pj_.op != JobOp::None; }
    int jobPercent() const;
    bool lastJobOk() const { return lastJobOk_; }
    // Files that existed but could not be read/written by the last job.
    int lastJobErrors() const { return lastJobErrors_; }
    // One bounded step (called by loop()); true while a job is still running.
    bool serviceProjectJob();
    // Run the current job to completion (spec 05 appendix E3: every project
    // API call first flushes an in-progress autosave).
    void finishJob();
    // Card removed: drop the job (open handles closed, popup removed).
    void abortJob();
    bool projectExists(const char* folder, const char* name);
    bool removeTree(const char* path);
    bool copyFile(const char* from, const char* to);

    // ---- instruments (project_io.cpp) -------------------------------------
    // Import a .wav (resampled) or .pti into a sample slot. Markers 0..65535.
    bool importSample(const char* path, int slot, uint16_t startMarker = 0, uint16_t endMarker = 65535,
                      bool lowQuality = false);
    bool deleteInstrument(int slot);
    bool copyInstrument(int src, int dst);
    void renameInstrument(int slot, const char* name);
    uint32_t sampleBytesNeeded(const char* path);  // projected memory of a file (bytes)

    // ---- MIDI CC input (midi_cc.h; spec 12 §B, spec 05 appendix C §8) ----
    // One accepted Control Change (after the Config "CC in" port/channel
    // filter): Performance (screen active), master levels, instrument map.
    void handleControlChange(uint8_t cc, uint8_t value);
    // 2.x: an accepted MIDI input pitch bend (after the "Notes in" filter).
    // Live notes only, +-kLiveBendSemis: sample instruments bend their track
    // voice, synth slots their live voices, MIDI instruments get it forwarded.
    void handlePitchBend(uint8_t lsb, uint8_t msb);
    static constexpr float kLiveBendSemis = 2.0f;   // [I] the common default range
    // Performance screen hook for CC 41-48 / 51-62 (task B4); null = ignore.
    PerformanceCcSink* perfCc = nullptr;
    // Instrument chosen in the MIDI Mapping list (appendix C §1 +0x335,
    // 0..47): the CC target while the current instrument is a MIDI one.
    uint8_t ccMapInstrument = 0;
    // Sample instrument the instrument CC map edits (appendix C §8).
    uint8_t ccTargetInstrument() const;

    // ---- export / render (render.cpp) -------------------------------------
    // Offline render through the engine (spec 04 §6.1). stems: `path` is a
    // folder receiving track1..8/reverb/delay/master.wav.
    enum class ExportKind : uint8_t { Song, Pattern, Selection };
    bool exportAudio(ExportKind kind, const char* path, const SelRect* sel = nullptr, bool stems = false);
    // First free Export/<proj>/... name (.wav file, or a folder for stems).
    bool exportName(ExportKind kind, char* out, int cap, bool stems = false);
    bool rendering() const { return rendering_; }

    // ---- transport ----------------------------------------------------------
    void togglePlay(bool shift);
    // Pattern `num` is in the song playlist.
    bool patternInSong(int num) const;
    uint32_t playStartMs() const { return playStart_; }

    // ---- ISequencerHost ---------------------------------------------------
    void loadPattern(uint8_t number, Pattern& dst, bool sync) override;
    // Spread preload (seq_sink.h): the same copy as loadPattern, by track.
    bool loadsPatternTracks() const override { return true; }
    void loadPatternTracks(uint8_t number, Pattern& dst, int first, int count) override;
    void onStepChanged() override { stepChanged_ = true; }
    void onPatternEnd() override {
        ++patternEnds_;
        engine.noteSeqWork(audio::kSeqPatternEnd);
    }
    uint32_t patternEndCount() const { return patternEnds_; }
    bool noteInScale(uint8_t note) override;
    uint8_t instrumentVolume(uint8_t instrument) override;
    uint8_t recordVolume(uint8_t instrument) const;   // live-record IV, 0..127
    void markPatternDirty() override {
        patternEditedFromSeq_ = true;
        engine.noteSeqWork(audio::kSeqLiveRecord);
    }
    bool songScreenActive() override { return ui.active() == kModSong; }

    // Paths
    void workspacePath(char* out, int cap, const char* rel) const;
    void projectPath(char* out, int cap) const;   // <folder>/<name>

    // Live recording state used by the pattern editor.
    bool editMode = false;

    // ---- sample recorder (recorder.cpp; spec 05 §2.11 + appendix C §2) ----
    Capture capture;
    volatile bool inputMonitorMuted = false;

    // [I] Live resampling (app/resample.h). Settings are session values
    // (Sample Recorder, source "Resample"); Shift + Sample Recorder arms /
    // stops from any screen.
    Resampler resampler;
    ResampleSettings resampleCfg;
    ResampleReport lastResample;   // [I] last take, for the "resample:" diag log line (src/app_main.cpp)
    enum class ResampleAction : uint8_t { kArmed, kCancelled, kStopping, kBusy, kNoSlot, kNoMemory };
    ResampleAction resampleToggle();
    void resampleCancel();
    // First slot above every loaded instrument (committing there moves no
    // sample), -1 when the last slot is taken.
    int nextResampleSlot() const;
    // [I] Into: Instrument N. Takes are whole audio blocks, so two takes of
    // the same bars differ by up to a block: within that they fit the slot
    // (trimmed or padded with silence) and keep its length.
    static constexpr uint32_t kResampleFitFrames = 128;
    // The slot a take would land in now: N (empty above every loaded slot,
    // or a stereo sample to overwrite in place) or the next free slot; -1
    // none. `inPlace` (may be null): N overwritten; `fits` (may be null):
    // false when Into N was given but the take goes elsewhere. n: the take's
    // frames (0: not known yet, N is taken when it holds a stereo sample).
    int resampleTarget(uint32_t n, bool* inPlace, bool* fits) const;
    // Into N was confirmed for this slot (asked once when arming, Ui::resampleKey);
    // -1 none. Reset when Into changes.
    int8_t resampleIntoOk = -1;
    // Steps per bar from the Config time signature.
    int resampleBarSteps() const { return (cfg.tsNum + 1) * (cfg.tsDen + 1); }
    // [I] Sliced takes, equal slices: steps per slice for a take of `bars`
    // whole bars and the slice count in `count` (0 / 0: Slices Off or no
    // whole bars). The grid (a beat or a step) doubles until at most
    // kSliceCount slices fit.
    int resampleSliceGrid(uint32_t bars, int& count) const;
    // [I] The slices of a take (planar l / r, n frames, `bars` whole bars) as
    // step offsets into the take, in `steps` (kSliceCount entries); returns
    // the count (0: not sliced). Equal slices, or at hits (findHitCells) on
    // the beat / step grid, which doubles until at most kSliceCount fit.
    int resampleSliceSteps(const int16_t* l, const int16_t* r, uint32_t n, uint32_t bars, uint16_t* steps) const;
    // Main loop: a finished recording becomes an instrument.
    void serviceResample();
    bool selectInput(uint8_t source, uint8_t gain);   // false = source not available
    IRadio* radio() const { return hal_.radio; }
    // Output folder "Recordings" / "Recordings N" (created) and the first
    // free "recordingNNN" name in it (spec 02 §8.3, appendix C §2.12).
    bool recordingsFolder(char* out, int cap);
    bool recordingName(const char* folder, char* out, int cap);
    int firstFreeSampleSlot() const;
    // Write mono frames as a 16-bit 44.1 kHz WAV (spec 02 §8.3).
    bool writeWav(const char* path, const int16_t* mono, uint32_t frames);
    // 2.x stereo recording: planar lanes l / r written interleaved as a
    // 2-channel 16-bit 44.1 kHz WAV (r nullptr = the mono file).
    bool writeWav(const char* path, const int16_t* l, const int16_t* r, uint32_t frames);

    // ---- screenshot (screenshot.cpp; spec 05 §3.4) -------------------------
    // Writes the first free Snapshots/Snapshot_NNN.bmp; false if the display
    // cannot read back or the card write fails.
    bool screenshot(char* pathOut = nullptr, int cap = 0);

    // ---- File > Export > Samples (render.cpp; spec 05 appendix E1.4) --------
    // Every used sample instrument to <folder>/<name>/export/samples/; returns
    // the number of files written, < 0 on error.
    int exportSamples();

private:
    void serviceMidi();
    void loopUsbStorage(uint32_t now);   // loop() while the computer owns the card
    void midiByte(MidiPortId port, uint8_t b);
    void handleMidiMessage(uint8_t st, uint8_t d1, uint8_t d2);
    // Project job internals (project_io.cpp).
    bool jobUnit(uint32_t& cost);
    void jobEnd();
    void jobFail();
    void jobCloseFiles();
    bool jobCopyUnit(const char* from, const char* to, uint32_t& cost, bool* failed);
    bool jobPtiWriteUnit(int slot, const char* path, uint32_t& cost, bool* failed);
    bool jobSamplesUnit(int slot, uint32_t& cost);
    int jobWipeUnit(uint32_t& cost);
    void jobBeginLoad();
    void jobProgress();
    void estimateJobTotal();
    void setPresent(int num, bool on);
    void attachSequencer();
    void defaultPattern(Pattern& p) const;
    void defaultPattern(PatternStock& p) const;
    void reportExtPoolFull();
    static bool exportRunning(void* ctx);
    // Mini pattern / patch file internals (project_io.cpp).
    bool loadPatternBytes(int num, const uint8_t* file, uint32_t len, uint8_t* out, bool* upgrade);
    bool savePatternUnit(int num);
    void loadPatchUnit(int s, uint32_t& cost);
    bool resolvePatch(int s, const uint8_t* buf, uint32_t len, bool haveFile);
    uint32_t patchCrc(int s);
    bool patchKeptOriginal(int s);
    bool savePatch(int s, const char* dir);
    void loadNotice();

    HalRefs hal_;
    AppMemory mem_;
    // SD streaming runtime (SRAM; the audio block reads the rings).
    struct StreamRuntime {
        audio::StreamRing rings[audio::kVoices];
        audio::StreamSource src[audio::kMaxStreams];
        int8_t streamOf[kSampleInstrumentCount];   // slot -> src index, -1 = RAM
        uint8_t warnedMode[kSampleInstrumentCount];   // play mode last notified as not streamable
        bool on = false;
        audio::StreamServiceStats stats;
    };
    StreamRuntime stream_;
    void serviceStreams(uint32_t nowMs);
    // Head-only loading (project_io.cpp): the bank samples a slot needs (and
    // whether it streams), its bank ranges, and the bind at the end.
    uint32_t streamWouldResident(int slot, uint32_t frames, bool stereo);
    void planStreamRanges(int slot, uint32_t frames, bool stereo, uint32_t off);
    // False with *reason (StreamBindInfo::reason, or "no reader" / "no slot")
    // when the slot cannot stream; a load without `reason` notes it in
    // loadNotice_ instead.
    bool bindStreamSlot(int slot, const char* path, const char** reason = nullptr);
    ExtTrackPool ext_;              // tracks 12..15 of the store (SDRAM blocks)
    Track* extDiscard_ = nullptr;   // storedTrack() target when the pool is full (SDRAM)
    uint32_t extFullReported_ = 0;  // ext_.fullEvents() at the last notice
    uint8_t present_[32] = {};
    // Tracks 13..16 that did not fit the pool at load (bit e of entry num):
    // the autosave keeps them from the Workspace file until the store holds
    // the track (never lost).
    // Main loop only (project files, File screen): SDRAM on the device.
    using ExtKeep = uint8_t[kPatternCount + 1];
    OT_APP_SDRAM_MEMBER(ExtKeep, extKeep_);   // cleared in init()
    // Synth slots: patch model, where it came from, dirty bits.
    // Synth slot patches (7.9 KB): edited in the main loop; the audio block
    // reads them through SynthBank::bindPatch on a patch change and for
    // macros at note starts (like the sample bank, SDRAM reads are fine
    // there). SDRAM on the device (stack headroom); constructed in init().
    using SynthPatches = SynthPatch[kSynthSlotCount];
    OT_APP_SDRAM_MEMBER(SynthPatches, patches_);
    PatchSource patchSrc_[kSynthSlotCount] = {};
    bool patchDirty_[kSynthSlotCount] = {};
    // Load Patch preview (synth_slots.cpp): slot (-1 none), its saved source
    // and dirty flag; the saved patch lives in the SDRAM (OT_SDRAM_BSS).
    int8_t previewSlot_ = -1;
    PatchSource previewSrc_ = PatchSource::None;
    bool previewDirty_ = false;
    const SynthPatch* readSynthLibraryPatch(int s, const char* name);
    // previewOn voice holding the synth audition track (kNoSynthAudition: none).
    static constexpr uint16_t kNoSynthAudition = 0xFFFF;
    uint16_t synthAudVoice_ = kNoSynthAudition;
    // MIDI input notes held on the synth live tracks (audio::kSynthLiveTracks;
    // 0 = free) and the next one to take when all are held.
    uint16_t synthLive_[4] = {};
    uint8_t synthLiveNext_ = 0;
    float liveBend_ = 0.0f;         // last MIDI input pitch bend (semitones)
    char loadNotice_[64] = {};      // first problem of the last load (one notice)
    uint8_t patDirty_[32] = {};
    bool projectDirty_ = false, namesDirty_ = false, configDirty_ = false, configForce_ = false;
    uint8_t recShadow_[12] = {};                  // project bytes 0x134..0x13F last synced
    bool recShadowValid_ = false;
    bool instrHeaderDirty_[kSampleInstrumentCount] = {};
    bool instrSampleDirty_[kSampleInstrumentCount] = {};
    bool unsaved_ = false, neverSaved_ = true;
    bool clipValid_ = false;
    SelRect clipRect_;
    uint32_t lastAutosave_ = 0;
    uint32_t lastConfigSave_ = 0;
    volatile bool rendering_ = false;
    volatile bool engineDirty_ = true;
    // Export state machine (render.cpp).
    struct ExportJob {
        ExportKind kind = ExportKind::Pattern;
        const SelRect* sel = nullptr;
        bool started = false;
        uint32_t blocks = 0, tail = 0;
        int songLen = 0;
    } job_;
    // SD streaming C14 import (sample_import.cpp).
    char importNotice_[64] = {};
    int importUnit(uint32_t& cost);
    void importCleanup();
    bool importFinalize();
    static int importUnitThunk(void* ctx, uint32_t& cost);
    static int importPercentThunk(void* ctx);
    static void importDoneThunk(void* ctx, bool ok);
    OT_APP_SDRAM_MEMBER(ProjectJob, pj_);   // background project operation (project_io.cpp)
    bool lastJobOk_ = true;
    int lastJobErrors_ = 0;
    uint32_t jobProgressMs_ = 0;
    volatile bool stepChanged_ = false;
    volatile uint32_t patternEnds_ = 0;
    volatile bool patternEditedFromSeq_ = false;
    bool outputOn_ = false;    // audio mute line state (setOutputEnabled)
    uint32_t unmuteFrom_ = 0;  // start-up load finished at (ms), 0 = not yet
    static constexpr uint32_t kUnmuteSettleMs = 500;
    bool booted_ = false;
    // 2.x theme file (custom_theme.cpp): the view over the SDRAM slot table.
    void restoreCustomTheme();
    Theme themeView_{};
    char themeName_[21] = {};
    bool themeActive_ = false;
    bool usbActive_ = false;   // USB Storage mode: the computer owns the card
    bool wasPlaying_ = false;
    // [I] Resample bar tracking (audio context, App::preBlock).
    bool resampleWasPlaying_ = false;
    int32_t resampleLastStep_ = 0;
    void finishResample();
    void reportResample(uint8_t outcome, int slot);
    void placeResample(int slot, uint32_t bars, const uint16_t* sliceSteps, int slices);
    uint32_t playStart_ = 0;
    uint32_t lastNoCardTry_ = 0;
    // MIDI input parser per port.
    struct MidiIn {
        uint8_t status = 0, data[2] = {}, count = 0;
    } midiIn_[2];
    // Per-track audition record (spec 07 Q7: track record +0x60/+0x61/+0x78/+0x7C).
    struct Audition {
        bool busy = false;
        bool midi = false;
        uint16_t id = kAuditionNone;
        uint8_t instrument = 0, note = 0, ch = 0;
    } aud_[kTrackCount];
    void releaseAudition(int track);
};

}  // namespace ot
