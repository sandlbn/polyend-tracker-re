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
    CHECK_STR(a.importNotice(), "Streams from the card");
}

TEST(import_card_full_keeps_old_instrument) {
    Harness h(true, kBank);
    App& a = *h.app;
    streaming(h, 1);
    h.fs.files["Samples/old.wav"] = makeWav(20000, 1, 44100, 1);
    CHECK(a.importSample("Samples/old.wav", 0));
    h.fs.files["Samples/long.wav"] = makeWav(1500000, 1, 44100, 3);
    h.fs.capacity = h.fs.usedBytes() + 300000;   // less than the 3 MB file
    CHECK(a.importRoute("Samples/long.wav", 0) == ImportRoute::CardFull);
    CHECK(!a.startStreamedImport("Samples/long.wav", 0));
    CHECK_STR(a.instruments[0].name, "old");
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
