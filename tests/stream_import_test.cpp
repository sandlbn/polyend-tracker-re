// SD streaming C14c: importing a sample that does not fit the free sample
// memory. With Stream from card on, App::startStreamedImport converts the WAV
// (or copies the .pti) onto the card in chunks as the slot's Workspace
// instrument file, through Workspace/instruments/import.tmp, then loads its
// head and streams the rest. The old instrument stays until the new one is
// in place; a cancel or a full card leaves it untouched.
#include "test.h"
#include "ui_fakes.h"

#include <cstring>
#include <vector>

#include "formats/file_io.h"
#include "formats/instrument_pti.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {

// Test sample memory: 2.2 M samples; with streaming on, 2 MB of it are the
// stream rings, leaving ~1.15 M samples (2.3 MB) for instruments.
constexpr uint32_t kBank = 2200000;

std::vector<uint8_t> makeWav(uint32_t frames, uint16_t channels, uint32_t rate, uint32_t seed) {
    std::vector<int16_t> s(size_t(frames) * channels);
    uint32_t x = seed;
    for (auto& v : s) {
        x = x * 1664525u + 1013904223u;
        v = int16_t(int32_t(x >> 17) - 16384);
    }
    std::vector<uint8_t> w(s.size() * 2 + 64);
    w.resize(wavEncode(s.data(), frames, channels, w.data(), w.size()));
    // wavEncode writes 44.1 kHz: patch the rate fields for other rates.
    if (rate != 44100) {
        auto w32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) w[o + i] = uint8_t(v >> (8 * i)); };
        w32(24, rate);
        w32(28, rate * 2u * channels);
    }
    return w;
}

// What the in-memory import would have written for this file.
std::vector<int16_t> ramImport(const std::vector<uint8_t>& file, uint32_t* frames, bool* stereo) {
    std::vector<int16_t> out(file.size() * 4);
    WavImportOptions opt;
    opt.stereo = true;
    WavImportResult res;
    CHECK(wavImportMemory(file.data(), file.size(), opt, out.data(), out.size(), res) == FmtStatus::Ok);
    out.resize(size_t(res.frames) * (res.stereo ? 2 : 1));
    *frames = res.frames;
    *stereo = res.stereo;
    return out;
}

std::string instrPath(int slot) {
    char p[96];
    workspaceInstrumentPath(p, sizeof(p), kWorkspace, slot + 1);
    return p;
}

bool payloadEquals(Harness& h, int slot, const std::vector<int16_t>& want) {
    const auto it = h.fs.files.find(instrPath(slot));
    if (it == h.fs.files.end()) return false;
    const std::vector<uint8_t>& f = it->second;
    if (f.size() != pti::kDataOff + want.size() * 2) return false;
    return std::memcmp(f.data() + pti::kDataOff, want.data(), want.size() * 2) == 0;
}

void run(App& a) { a.finishJob(); }

// Config > Stream from card, applied as the Config screen does (save, reload).
void streaming(Harness& h, uint8_t mode) {
    App& a = *h.app;
    a.cfg.streamMode = mode;
    a.autosave();
    CHECK(a.loadWorkspace());
    CHECK(a.streamingOn() == (mode != 0));
}

bool drew(Harness& h, const char* s) {
    for (const std::string& l : h.disp.last)
        if (l.find(s) != std::string::npos) return true;
    return false;
}

// Vertices drawn inside the Sample Playback wave area (x < 600, y 116..334):
// the wave is hundreds of column rects, the markers and strip a few.
int waveAreaVertices(Harness& h) {
    int n = 0;
    for (const std::string& l : h.disp.last) {
        int x = 0, y = 0;
        if (std::sscanf(l.c_str(), "VERTEX %d %d", &x, &y) == 2 && x >= 0 && x < 600 && y >= 116 && y <= 334) ++n;
    }
    return n;
}

// A 34.014 s mono WAV imported into slot 0: streams, 32768 frames resident.
void importLong(Harness& h) {
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/long.wav"] = makeWav(1500000, 1, 44100, 3);
    CHECK(a.startStreamedImport("Samples/long.wav", 0));
    run(a);
    CHECK(a.streamed(0));
    CHECK_EQ(a.bank.frames(0), 32768u);
    CHECK_EQ(a.bank.fullFrames(0), 1500000u);
}

}  // namespace

TEST(import_long_wav_streams_from_workspace_file) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);   // Auto
    h.fs.files["Samples/long.wav"] = makeWav(1500000, 1, 44100, 3);   // 3 MB > 2.3 MB
    CHECK(a.importRoute("Samples/long.wav", 0) == ImportRoute::Streams);
    CHECK(a.startStreamedImport("Samples/long.wav", 0));
    run(a);
    CHECK(a.lastJobOk());
    uint32_t frames = 0;
    bool stereo = false;
    const std::vector<int16_t> want = ramImport(h.fs.files["Samples/long.wav"], &frames, &stereo);
    CHECK(payloadEquals(h, 0, want));
    CHECK(!h.fs.files.count("Workspace/instruments/import.tmp"));
    CHECK(a.instrPresent[0]);
    CHECK(a.streamed(0));
    CHECK(a.bank.partial(0));
    CHECK(h.streamReader.bound(0));
    CHECK_EQ(a.instruments[0].lengthFrames, frames);
    CHECK_STR(a.instruments[0].name, "long");
    // The header on the card matches the instrument.
    Instrument ins;
    const std::vector<uint8_t>& f = h.fs.files[instrPath(0)];
    CHECK(decodePtiHeader(f.data(), f.size(), ins) == FmtStatus::Ok);
    CHECK_EQ(ins.lengthFrames, frames);
    // The autosave rewrites only the header: the payload stays.
    a.autosave();
    CHECK(payloadEquals(h, 0, want));
}

TEST(import_stereo_resampled_matches_ram) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 2);   // All
    h.fs.files["Samples/st.wav"] = makeWav(700000, 2, 48000, 7);
    CHECK(a.importRoute("Samples/st.wav", 3) == ImportRoute::Streams);
    CHECK(a.startStreamedImport("Samples/st.wav", 3));
    run(a);
    CHECK(a.lastJobOk());
    uint32_t frames = 0;
    bool stereo = false;
    const std::vector<int16_t> want = ramImport(h.fs.files["Samples/st.wav"], &frames, &stereo);
    CHECK(stereo);
    CHECK(payloadEquals(h, 3, want));
    CHECK(a.streamed(3));
    CHECK(a.instruments[3].stereo);
}

TEST(import_long_pti_copies_and_streams) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    std::vector<int16_t> s(1300000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(i * 37);
    Instrument ins;
    ins.setDefault();
    std::snprintf(ins.name, sizeof(ins.name), "Big One");
    ins.lengthFrames = uint32_t(s.size());
    ins.volume = 33;
    std::vector<uint8_t> file(ptiFileSize(ins.lengthFrames, false));
    CHECK_EQ(encodePti(ins, s.data(), nullptr, file.data(), file.size()), file.size());
    h.fs.files["Samples/big.pti"] = file;
    CHECK(a.importRoute("Samples/big.pti", 5) == ImportRoute::Streams);
    CHECK(a.startStreamedImport("Samples/big.pti", 5));
    run(a);
    CHECK(a.lastJobOk());
    CHECK(payloadEquals(h, 5, s));
    CHECK(a.streamed(5));
    CHECK_STR(a.instruments[5].name, "Big One");
    CHECK_EQ(int(a.instruments[5].volume), 33);
}

TEST(import_cancel_removes_tmp_keeps_old_instrument) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/old.wav"] = makeWav(20000, 1, 44100, 1);
    CHECK(a.importSample("Samples/old.wav", 0));
    h.fs.files["Samples/long.wav"] = makeWav(1800000, 1, 44100, 3);
    CHECK(a.startStreamedImport("Samples/long.wav", 0));
    CHECK(a.serviceProjectJob());   // a few chunks
    CHECK(a.jobBusy());
    a.abortJob();                   // Back on the progress popup / card removed
    CHECK(!a.jobBusy());
    CHECK(!a.lastJobOk());
    CHECK(!h.fs.files.count("Workspace/instruments/import.tmp"));
    CHECK(a.instrPresent[0]);
    CHECK_STR(a.instruments[0].name, "old");
    CHECK(!a.bank.partial(0));
    CHECK_EQ(a.bank.frames(0), 20000u);
}

TEST(import_bind_failure_names_the_reason) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/long.wav"] = makeWav(1500000, 1, 44100, 3);
    // Only a fragmented card is fixed by formatting it.
    h.streamReader.failBind = "fragmented";
    CHECK(a.startStreamedImport("Samples/long.wav", 0));
    run(a);
    CHECK(!a.lastJobOk());
    CHECK_STR(a.importNotice(), "Card too fragmented to stream: format the SD card");
    CHECK(!a.instrPresent[0]);
    CHECK(!a.streamed(0));
    CHECK(!h.fs.files.count(instrPath(0)));
    // Any other reason is named as it is.
    h.streamReader.failBind = "map check";
    CHECK(a.startStreamedImport("Samples/long.wav", 0));
    run(a);
    CHECK(!a.lastJobOk());
    CHECK_STR(a.importNotice(), "Cannot stream the file (map check)");
    CHECK(!a.instrPresent[0]);
    h.streamReader.failBind = nullptr;
    CHECK(a.startStreamedImport("Samples/long.wav", 0));
    run(a);
    CHECK(a.lastJobOk());
    CHECK_STR(a.importNotice(), "Streams from the card (longer than 10 s)");
}

TEST(import_card_full_keeps_old_instrument) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/old.wav"] = makeWav(20000, 1, 44100, 1);
    CHECK(a.importSample("Samples/old.wav", 0));
    h.fs.files["Samples/long.wav"] = makeWav(1500000, 1, 44100, 3);
    h.fs.capacity = h.fs.usedBytes() + 300000;   // less than the 3 MB file
    CHECK(!a.startStreamedImport("Samples/long.wav", 0));   // reserve() refuses, before converting
    CHECK_STR(a.importNotice(), "Not enough space on the SD card");
    CHECK_STR(a.instruments[0].name, "old");
}

TEST(streamed_import_with_room_scans_no_free_space) {
    // Device: the first free-space query after a mount scans the whole FAT
    // (2.3 s on a 16 GB card, the main loop stalled). The reservation fails
    // by itself on a full card, so a streamed Add only asks for the free
    // space when it does.
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/long.wav"] = makeWav(1500000, 1, 44100, 3);
    h.fs.capacity = h.fs.usedBytes() + 8000000;
    h.fs.freeScans = 0;
    CHECK(a.importRoute("Samples/long.wav", 0) == ImportRoute::Streams);   // the Loader's Add asks this first
    CHECK(a.startStreamedImport("Samples/long.wav", 0));
    CHECK_EQ(h.fs.freeScans, 0);
    // A full card still says so, before any conversion.
    h.fs.files["Samples/long2.wav"] = makeWav(1500000, 1, 44100, 4);
    for (int k = 0; k < 4000 && a.jobBusy(); ++k) h.run(20);
    h.fs.capacity = h.fs.usedBytes() + 300000;
    CHECK(!a.startStreamedImport("Samples/long2.wav", 1));
    CHECK_STR(a.importNotice(), "Not enough space on the SD card");
}

TEST(import_refused_when_streaming_is_off) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 0);   // the whole 4.4 MB bank: a 5 MB file does not fit
    h.fs.files["Samples/long.wav"] = makeWav(2500000, 1, 44100, 3);
    CHECK(a.importRoute("Samples/long.wav", 0) == ImportRoute::Off);
    CHECK(!a.startStreamedImport("Samples/long.wav", 0));
    // A file that fits: the normal import.
    h.fs.files["Samples/short.wav"] = makeWav(20000, 1, 44100, 1);
    CHECK(a.importRoute("Samples/short.wav", 0) == ImportRoute::Fits);
}

TEST(import_forces_oneshot_on_a_kept_slice_mode) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/old.wav"] = makeWav(20000, 1, 44100, 1);
    CHECK(a.importSample("Samples/old.wav", 2));
    a.instruments[2].playMode = uint8_t(PlayMode::Slice);
    a.instruments[2].volume = 41;
    h.fs.files["Samples/long.wav"] = makeWav(1500000, 1, 44100, 3);
    CHECK(a.startStreamedImport("Samples/long.wav", 2));
    run(a);
    CHECK(a.lastJobOk());
    CHECK(a.streamed(2));
    CHECK_EQ(int(a.instruments[2].playMode), int(PlayMode::OneShot));
    CHECK_EQ(int(a.instruments[2].volume), 41);   // the rest of the settings kept
}

TEST(stale_import_tmp_removed_at_load) {
    Harness h(true, kBank);
    App& a = *h.app;
    a.autosave();
    h.fs.files["Workspace/instruments/import.tmp"] = std::vector<uint8_t>(1000, 1);
    CHECK(a.loadWorkspace());
    CHECK(!h.fs.files.count("Workspace/instruments/import.tmp"));
}

// ---- The import follows Stream from card (Off / Auto / All + threshold) ----

namespace {
constexpr uint32_t kSec = 44100;

// `frames` mono frames as Samples/<name>.wav, streamed-imported into `slot`
// when the route says so: true when it streams.
bool importAs(Harness& h, const char* name, uint32_t frames, int slot, uint32_t seed = 5) {
    App& a = *h.app;
    const std::string p = std::string("Samples/") + name;
    h.fs.files[p] = makeWav(frames, 1, 44100, seed);
    const ImportRoute r = a.importRoute(p.c_str(), slot);
    if (r == ImportRoute::Fits) return a.importSample(p.c_str(), slot), false;
    if (r != ImportRoute::Streams || !a.startStreamedImport(p.c_str(), slot)) return false;
    run(a);
    return a.lastJobOk() && a.streamed(slot);
}

std::vector<uint8_t> makePti(uint32_t frames, PlayMode pm) {
    std::vector<int16_t> s(frames);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(i * 13);
    Instrument ins;
    ins.setDefault();
    ins.lengthFrames = frames;
    ins.playMode = uint8_t(pm);
    std::vector<uint8_t> file(ptiFileSize(frames, false));
    CHECK_EQ(encodePti(ins, s.data(), nullptr, file.data(), file.size()), file.size());
    return file;
}
}  // namespace

TEST(import_policy_auto_streams_a_long_file_that_fits) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);   // Auto, 10 s
    h.fs.files["Samples/twelve.wav"] = makeWav(12 * kSec, 1, 44100, 3);   // fits the free memory
    CHECK(a.importRoute("Samples/twelve.wav", 0) == ImportRoute::Streams);
    CHECK(a.startStreamedImport("Samples/twelve.wav", 0));
    run(a);
    CHECK(a.lastJobOk());
    CHECK(a.streamed(0));
    CHECK_STR(a.importNotice(), "Streams from the card (longer than 10 s)");
}

TEST(import_policy_auto_keeps_a_short_file_in_memory) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/nine.wav"] = makeWav(9 * kSec, 1, 44100, 3);
    CHECK(a.importRoute("Samples/nine.wav", 0) == ImportRoute::Fits);
    CHECK(!importAs(h, "nine.wav", 9 * kSec, 0));
    CHECK(a.instrPresent[0]);
    CHECK(!a.streamed(0));
}

TEST(import_policy_threshold_moves_the_line) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/twelve.wav"] = makeWav(12 * kSec, 1, 44100, 3);
    h.fs.files["Samples/six.wav"] = makeWav(6 * kSec, 1, 44100, 3);
    a.cfg.streamThreshold = 0;   // 5 s
    CHECK(a.importRoute("Samples/six.wav", 0) == ImportRoute::Streams);
    CHECK(a.importRoute("Samples/twelve.wav", 0) == ImportRoute::Streams);
    a.cfg.streamThreshold = 2;   // 20 s
    CHECK(a.importRoute("Samples/twelve.wav", 0) == ImportRoute::Fits);
    a.cfg.streamThreshold = 3;   // 30 s
    CHECK(a.importRoute("Samples/twelve.wav", 0) == ImportRoute::Fits);
    a.cfg.streamThreshold = 0;
    CHECK(a.startStreamedImport("Samples/six.wav", 0));
    run(a);
    CHECK_STR(a.importNotice(), "Streams from the card (longer than 5 s)");
}

TEST(import_policy_all_streams_from_65537_frames) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 2);   // All
    h.fs.files["Samples/a.wav"] = makeWav(65536, 1, 44100, 3);
    h.fs.files["Samples/b.wav"] = makeWav(65537, 1, 44100, 3);
    CHECK(a.importRoute("Samples/a.wav", 0) == ImportRoute::Fits);
    CHECK(a.importRoute("Samples/b.wav", 0) == ImportRoute::Streams);
    CHECK(a.startStreamedImport("Samples/b.wav", 0));
    run(a);
    CHECK(a.streamed(0));
    CHECK_STR(a.importNotice(), "Streams from the card (Stream from card: All)");
}

TEST(import_policy_off_never_streams_a_file_that_fits) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 0);
    h.fs.files["Samples/long.wav"] = makeWav(40 * kSec, 1, 44100, 3);   // fits the whole 4.4 MB bank
    CHECK(a.importRoute("Samples/long.wav", 0) == ImportRoute::Fits);
    CHECK(!a.startStreamedImport("Samples/long.wav", 0));
}

TEST(import_policy_kept_slice_mode_that_fits_stays_in_memory) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/old.wav"] = makeWav(20000, 1, 44100, 1);
    CHECK(a.importSample("Samples/old.wav", 2));
    a.instruments[2].playMode = uint8_t(PlayMode::Slice);
    h.fs.files["Samples/twelve.wav"] = makeWav(12 * kSec, 1, 44100, 3);
    CHECK(a.importRoute("Samples/twelve.wav", 2) == ImportRoute::Fits);
    // An empty slot takes 1-Shot: streams.
    CHECK(a.importRoute("Samples/twelve.wav", 3) == ImportRoute::Streams);
}

TEST(import_policy_pti_play_mode_decides) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/slice.pti"] = makePti(12 * kSec, PlayMode::Slice);
    h.fs.files["Samples/loop.pti"] = makePti(12 * kSec, PlayMode::ForwardLoop);
    CHECK(a.importRoute("Samples/slice.pti", 0) == ImportRoute::Fits);
    CHECK(a.importRoute("Samples/loop.pti", 0) == ImportRoute::Streams);
}

TEST(import_policy_no_fit_streams_below_threshold) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/fill.wav"] = makeWav(900000, 1, 44100, 1);
    CHECK(a.importSample("Samples/fill.wav", 1));    // into memory: little room left
    h.fs.files["Samples/nine.wav"] = makeWav(9 * kSec, 1, 44100, 3);
    CHECK(a.importRoute("Samples/nine.wav", 0) == ImportRoute::Streams);
    CHECK(a.startStreamedImport("Samples/nine.wav", 0));
    run(a);
    CHECK(a.streamed(0));
    CHECK_STR(a.importNotice(), "Streams from the card (does not fit memory)");
}

TEST(import_policy_no_fit_forces_oneshot) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/fill.wav"] = makeWav(900000, 1, 44100, 1);
    CHECK(a.importSample("Samples/fill.wav", 1));
    a.instruments[1].playMode = uint8_t(PlayMode::Granular);
    // A WAV into the granular slot: its own sample is replaced, the rest does
    // not fit with it either: 1-Shot.
    h.fs.files["Samples/huge.wav"] = makeWav(1300000, 1, 44100, 3);
    CHECK(a.importRoute("Samples/huge.wav", 1) == ImportRoute::Streams);
    CHECK(a.startStreamedImport("Samples/huge.wav", 1));
    run(a);
    CHECK(a.streamed(1));
    CHECK_EQ(int(a.instruments[1].playMode), int(PlayMode::OneShot));
    CHECK_STR(a.importNotice(), "Streams from the card (does not fit memory, 1-Shot)");
}

TEST(import_policy_seventeenth_stream_loads_into_memory_when_it_fits) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 2);   // All: 70,000 frames stream
    for (int i = 0; i < audio::kMaxStreams; ++i) {
        const std::string n = "s" + std::to_string(i) + ".wav";
        CHECK(importAs(h, n.c_str(), 70000, i, uint32_t(i + 1)));
    }
    h.fs.files["Samples/x.wav"] = makeWav(70000, 1, 44100, 99);
    CHECK(a.importRoute("Samples/x.wav", 20) == ImportRoute::Fits);   // into memory, as at an open
    h.fs.files["Samples/big.wav"] = makeWav(1000000, 1, 44100, 98);
    CHECK(a.importRoute("Samples/big.wav", 20) == ImportRoute::TooMany);
    // Into one of the 16: its own stream is freed first.
    CHECK(a.importRoute("Samples/x.wav", 3) == ImportRoute::Streams);
}

TEST(import_and_reopen_agree) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    a.cfg.streamThreshold = 0;   // 5 s; the whole project fits (else Auto streams from 2 s up at an open)
    CHECK(importAs(h, "six.wav", 6 * kSec, 0));
    CHECK(!importAs(h, "four.wav", 4 * kSec, 1));
    h.fs.files["Samples/old.wav"] = makeWav(20000, 1, 44100, 1);
    CHECK(a.importSample("Samples/old.wav", 2));
    a.instruments[2].playMode = uint8_t(PlayMode::Slice);
    CHECK(!importAs(h, "slice.wav", 6 * kSec, 2));
    const bool before[3] = {a.streamed(0), a.streamed(1), a.streamed(2)};
    a.autosave();
    CHECK(a.loadWorkspace());
    for (int i = 0; i < 3; ++i) {
        CHECK(a.instrPresent[i]);
        CHECK_EQ(a.streamed(i), before[i]);
    }
    CHECK(a.streamed(0));
}

// ---- Sample Loader (C14d) ---------------------------------------------------

namespace {
LoaderScreen& ld(Harness& h) { return *static_cast<LoaderScreen*>(h.app->ui.screen(kModLoader)); }
// Loader open on /Samples with the cursor on `file`.
bool onFile(Harness& h, const char* file) {
    h.tap(btn::SampleLoader);
    LoaderScreen& l = ld(h);
    auto find = [&](const char* want) {
        char b[wg::kListItemCap];
        for (int i = 0; i < l.count; ++i) {
            l.entryLabel(i, b, sizeof(b));
            if (std::string(b) == want) return i;
        }
        return -1;
    };
    int i = find("/Samples");
    if (i < 0) return false;
    l.sdCursor = i;
    h.tap(btn::F1);   // enter the folder
    i = find(file);
    if (i < 0) return false;
    l.sdCursor = i;
    return true;
}
}  // namespace

TEST(loader_add_long_wav_streams) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.mkdir("Samples");
    h.fs.files["Samples/long.wav"] = makeWav(1500000, 1, 44100, 3);
    CHECK(onFile(h, "long.wav"));
    h.tap(btn::F1);         // Add
    CHECK(!ld(h).over());   // the bar counts only the streamed head
    CHECK(a.jobBusy());
    CHECK(std::string(a.ui.progressTitle()).find("Delete cancels") != std::string::npos);
    for (int k = 0; k < 400 && a.jobBusy(); ++k) h.run(20);
    CHECK(!a.jobBusy());
    CHECK(a.streamed(0));
    CHECK(std::string(a.ui.notice()).find("Streams from the card") != std::string::npos);
}

TEST(loader_add_too_big_with_streaming_off_says_why) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 0);
    h.fs.mkdir("Samples");
    h.fs.files["Samples/huge.wav"] = makeWav(2500000, 1, 44100, 3);
    CHECK(onFile(h, "huge.wav"));
    h.tap(btn::F1);
    CHECK(ld(h).over());
    CHECK(!a.jobBusy());
    CHECK(!a.instrPresent[0]);
    CHECK(std::string(a.ui.notice()).find("Stream from card") != std::string::npos);
}

TEST(loader_delete_cancels_the_import) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.mkdir("Samples");
    h.fs.files["Samples/long.wav"] = makeWav(1800000, 1, 44100, 3);
    CHECK(onFile(h, "long.wav"));
    h.tap(btn::F1);
    CHECK(a.jobBusy());
    h.tap(btn::Delete);
    for (int k = 0; k < 20 && a.jobBusy(); ++k) h.run(20);
    CHECK(!a.jobBusy());
    CHECK(!a.instrPresent[0]);
    CHECK(!h.fs.files.count("Workspace/instruments/import.tmp"));
    CHECK(std::string(a.ui.notice()).find("cancelled") != std::string::npos);
}

TEST(playback_bar_times_use_the_full_length_when_streamed) {
    Harness h(true, kBank);
    App& a = *h.app;
    importLong(h);
    a.setCurrentInstrument(0);
    h.tap(btn::SamplePlayback);
    h.run(50);
    CHECK_STR(a.ui.bar.label[4].bottom, "34.014s");   // the whole sample, not the 0.743 s head
    CHECK_STR(a.ui.bar.label[1].bottom, "0.000s");
    // The markers move over the whole length: a step of End is a step of 34 s.
    CHECK_EQ(a.instruments[0].endPoint, 65535);
    h.tap(btn::InstrParams);
    h.run(50);
    a.instruments[0].endPoint = 32768;
    h.tap(btn::SamplePlayback);
    h.run(50);
    CHECK_STR(a.ui.bar.label[4].bottom, "17.007s");
}

TEST(playback_streamed_hides_wave) {
    Harness h(true, kBank);
    App& a = *h.app;
    importLong(h);
    h.fs.files["Samples/short.wav"] = makeWav(20000, 1, 44100, 9);
    CHECK(a.importSample("Samples/short.wav", 1));
    // A RAM instrument: its wave is drawn.
    a.setCurrentInstrument(1);
    h.tap(btn::SamplePlayback);
    h.run(50);
    CHECK(waveAreaVertices(h) > 300);
    CHECK(!drew(h, "Streams from the card"));
    // Streamed: no wave (only the head is in memory), a caption instead; the
    // Start / End markers stay.
    a.setCurrentInstrument(0);
    h.tap(btn::SamplePlayback);
    h.run(50);
    CHECK(drew(h, "Streams from the card"));
    CHECK(waveAreaVertices(h) < 60);
    CHECK(waveAreaVertices(h) > 0);
}

TEST(loader_marker_needs_no_probe_after_listing) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.mkdir("Samples");
    h.fs.files["Samples/a.wav"] = makeWav(20000, 1, 44100, 1);
    h.fs.files["Samples/b.pti"] = makePti(12 * kSec, PlayMode::OneShot);
    h.fs.files["Samples/c.wav"] = makeWav(12 * kSec, 1, 44100, 3);
    CHECK(onFile(h, "a.wav"));
    const int opens = h.fs.opens;
    // Cursor moves: the projection (300 ms later) and the draws read the
    // listing's probe cache, never the card.
    for (int k = 0; k < 2; ++k) {
        h.enc(1);
        h.run(400);
    }
    CHECK_EQ(h.fs.opens, opens);
    // c.wav streams: the bar counts its head only.
    CHECK(!ld(h).over());
    CHECK(ld(h).memoryShown() < a.bank.usedBytes() + 12 * kSec);
}

// ---- Sample Loader: the '~' mark and the notices ----------------------------

namespace {
// SD list rows (x < 300) drawn with the '~' tag: their row numbers (row y =
// 61 + 17 + 27 r; r 0 is "..").
std::vector<int> sdMarks(Harness& h) {
    h.run(400);   // past the 300 ms projection debounce, one more frame
    std::vector<int> rows;
    for (const std::string& l : h.disp.last) {
        int x = 0, y = 0;
        if (l.find("\"~\"") != std::string::npos && std::sscanf(l.c_str(), "TEXT %d %d", &x, &y) == 2 && x < 300)
            rows.push_back((y - 78) / 27);
    }
    return rows;
}
void addAndWait(Harness& h, int lastSlot) {
    App& a = *h.app;
    h.tap(btn::F1);   // Add
    for (int k = 0; k < 2000 && (a.jobBusy() || !a.instrPresent[lastSlot]); ++k) h.run(20);
}
}  // namespace

TEST(loader_marks_files_that_will_stream) {
    Harness h(true, kBank);
    streaming(h, 1);
    h.fs.mkdir("Samples");
    h.fs.files["Samples/long.wav"] = makeWav(12 * kSec, 1, 44100, 3);
    h.fs.files["Samples/short.wav"] = makeWav(2 * kSec, 1, 44100, 4);
    CHECK(onFile(h, "short.wav"));
    const std::vector<int> m = sdMarks(h);
    CHECK_EQ(m.size(), size_t(1));
    CHECK(!m.empty() && m[0] == 1);   // "..", long.wav, short.wav
}

TEST(loader_marker_follows_the_config) {
    auto marks = [](uint8_t mode, uint8_t threshold) {
        Harness h(true, kBank);
        streaming(h, mode);
        h.app->cfg.streamThreshold = threshold;
        h.fs.mkdir("Samples");
        h.fs.files["Samples/long.wav"] = makeWav(12 * kSec, 1, 44100, 3);
        h.fs.files["Samples/short.wav"] = makeWav(2 * kSec, 1, 44100, 4);   // 88,200 frames
        CHECK(onFile(h, "short.wav"));
        return sdMarks(h).size();
    };
    CHECK_EQ(marks(0, 1), size_t(0));   // Off
    CHECK_EQ(marks(1, 1), size_t(1));   // Auto 10 s: the 12 s file
    CHECK_EQ(marks(1, 2), size_t(0));   // Auto 20 s
    CHECK_EQ(marks(2, 1), size_t(2));   // All: both
}

TEST(loader_marker_uses_the_target_slot_play_mode) {
    // The instrument cursor on `slot`; slot 2 holds a Slice instrument.
    auto marks = [](int slot) {
        Harness h(true, kBank);
        App& a = *h.app;
        streaming(h, 1);
        h.fs.files["Samples/old.wav"] = makeWav(20000, 1, 44100, 1);
        CHECK(a.importSample("Samples/old.wav", 2));
        a.instruments[2].playMode = uint8_t(PlayMode::Slice);
        h.fs.mkdir("Samples");
        h.fs.files["Samples/long.wav"] = makeWav(12 * kSec, 1, 44100, 3);
        a.setCurrentInstrument(slot);
        CHECK(onFile(h, "long.wav"));
        return sdMarks(h).size();
    };
    CHECK_EQ(marks(2), size_t(0));   // the Slice slot keeps it in memory
    CHECK_EQ(marks(3), size_t(1));   // an empty slot: 1-Shot, streams
}

TEST(loader_marker_for_a_selection_counts_memory_in_order) {
    Harness h(true, kBank);
    streaming(h, 1);
    h.fs.mkdir("Samples");
    for (const char* n : {"a.wav", "b.wav", "c.wav"}) h.fs.files[std::string("Samples/") + n] = makeWav(9 * kSec, 1, 44100, 3);
    CHECK(onFile(h, "a.wav"));
    CHECK(sdMarks(h).empty());   // alone, each 9 s file fits
    h.down(btn::Shift);
    h.tap(btn::Down);
    h.tap(btn::Down);
    h.up(btn::Shift);
    // a, b, c into slots 1-3: the third no longer fits after the other two.
    const std::vector<int> m = sdMarks(h);
    CHECK_EQ(m.size(), size_t(1));
    CHECK(!m.empty() && m[0] == 3);
}

TEST(loader_notice_names_why_it_streams) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.mkdir("Samples");
    h.fs.files["Samples/long.wav"] = makeWav(12 * kSec, 1, 44100, 3);
    CHECK(onFile(h, "long.wav"));
    addAndWait(h, 0);
    CHECK(a.streamed(0));
    CHECK_STR(a.ui.notice(), "Streams from the card (longer than 10 s)");
}

TEST(loader_multi_add_summarises_streams) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.mkdir("Samples");
    h.fs.files["Samples/a.wav"] = makeWav(12 * kSec, 1, 44100, 3);
    h.fs.files["Samples/b.wav"] = makeWav(2 * kSec, 1, 44100, 4);
    h.fs.files["Samples/c.wav"] = makeWav(12 * kSec, 1, 44100, 5);
    CHECK(onFile(h, "a.wav"));
    h.down(btn::Shift);
    h.tap(btn::Down);
    h.tap(btn::Down);
    h.up(btn::Shift);
    addAndWait(h, 2);
    CHECK(a.streamed(0));
    CHECK(!a.streamed(1));
    CHECK(a.streamed(2));
    CHECK_STR(a.ui.notice(), "2 of 3 stream from the card");
}

// ---- Pre-check before the conversion ------------------------------------------

namespace {
// An old instrument in slot 0 and a 34 s file that streams.
void precheckSetup(Harness& h) {
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/old.wav"] = makeWav(20000, 1, 44100, 1);
    CHECK(a.importSample("Samples/old.wav", 0));
    h.fs.files["Samples/long.wav"] = makeWav(1500000, 1, 44100, 3);
    h.fs.reserves.clear();
}
void oldKept(Harness& h) {
    App& a = *h.app;
    CHECK(!a.jobBusy());
    CHECK(!h.fs.files.count("Workspace/instruments/import.tmp"));
    CHECK(a.instrPresent[0]);
    CHECK_STR(a.instruments[0].name, "old");
    CHECK(!a.streamed(0));
}
}  // namespace

TEST(import_precheck_refuses_a_fragmented_card_before_converting) {
    Harness h(true, kBank);
    App& a = *h.app;
    precheckSetup(h);
    h.streamReader.failStreamable = "fragmented";
    const uint32_t binds = h.streamReader.binds;
    CHECK(!a.startStreamedImport("Samples/long.wav", 0));
    CHECK_EQ(h.fs.reserves.size(), size_t(1));   // laid out, then checked: no conversion
    CHECK_EQ(h.streamReader.binds, binds);
    CHECK_STR(a.importNotice(), "Card too fragmented to stream: format the SD card");
    oldKept(h);
    // Any other reason is named.
    h.streamReader.failStreamable = "open";
    CHECK(!a.startStreamedImport("Samples/long.wav", 0));
    CHECK_STR(a.importNotice(), "Cannot stream the file (open)");
    oldKept(h);
}

TEST(import_precheck_refuses_without_a_free_binding) {
    Harness h(true, kBank);
    App& a = *h.app;
    precheckSetup(h);
    // The reader's 16 slots taken behind the App's back.
    for (int i = 0; i < audio::kMaxStreams; ++i) {
        audio::StreamSource s;
        s.instrument = uint8_t(30 + i);
        s.frames = 1;
        CHECK(h.streamReader.bind(s, "Samples/old.wav", 0, false, nullptr));
    }
    CHECK(!a.startStreamedImport("Samples/long.wav", 0));
    CHECK_STR(a.importNotice(), "16 instruments already stream from the card");
    oldKept(h);
}

TEST(import_without_a_contiguous_run_writes_the_file_in_pieces) {
    // Device report (2.2.3-beta3): the second long Add said "Card too
    // fragmented" on a card with room but no single free run of the file's
    // size. A stream may span up to kStreamMaxExtents pieces: with room on
    // the card the import goes on unreserved, and only bind() after the
    // conversion refuses a file in too many pieces.
    Harness h(true, kBank);
    App& a = *h.app;
    precheckSetup(h);
    h.fs.capacity = h.fs.usedBytes() + 50000000;   // room, but no contiguous run
    h.fs.reserveFails = true;
    CHECK(a.importRoute("Samples/long.wav", 0) == ImportRoute::Streams);
    CHECK(a.startStreamedImport("Samples/long.wav", 0));
    for (int k = 0; k < 4000 && a.jobBusy(); ++k) h.run(20);
    CHECK(a.streamed(0));
    CHECK(std::strncmp(a.importNotice(), "Streams from the card", 21) == 0);
    // In too many pieces after all: refused after the conversion, and the
    // slot's old instrument is still there (it was removed before the check:
    // "the imported lines show nothing").
    precheckSetup(h);
    h.fs.capacity = h.fs.usedBytes() + 50000000;
    h.fs.reserveFails = true;
    h.streamReader.failStreamable = "fragmented";   // the converted file's map
    CHECK(a.startStreamedImport("Samples/long.wav", 0));
    for (int k = 0; k < 4000 && a.jobBusy(); ++k) h.run(20);
    CHECK_STR(a.importNotice(), "Card too fragmented to stream: format the SD card");
    oldKept(h);
    h.streamReader.failStreamable = nullptr;
}

TEST(import_file_ends_where_the_conversion_ends) {
    // reserve() lays the whole file out at once (stale card data until
    // written): the file ends where the writes did, here with a reserve
    // that took more than the conversion writes.
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.reserveExtra = 4096;
    h.fs.files["Samples/long.wav"] = makeWav(1500000, 1, 44100, 3);
    CHECK(a.startStreamedImport("Samples/long.wav", 0));
    run(a);
    CHECK(a.lastJobOk());
    CHECK_EQ(h.fs.files[instrPath(0)].size(), size_t(pti::kDataOff + 1500000u * 2u));
    uint32_t frames = 0;
    bool stereo = false;
    CHECK(payloadEquals(h, 0, ramImport(h.fs.files["Samples/long.wav"], &frames, &stereo)));
}

TEST(import_cancel_after_reserve_removes_the_laid_out_file) {
    Harness h(true, kBank);
    App& a = *h.app;
    precheckSetup(h);
    CHECK(a.startStreamedImport("Samples/long.wav", 0));
    CHECK(h.fs.files.count("Workspace/instruments/import.tmp"));
    a.abortJob();
    oldKept(h);
}
