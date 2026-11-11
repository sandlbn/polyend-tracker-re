// Issue #184: Open streams a long instrument straight from the project's own
// .pti (only its head and loop head are read); a marker
// Workspace/instruments/instrument_NN.src names that file until the
// autosave has copied it into the Workspace, then the stream moves to the
// copy. The Workspace stays loadable at every point.
#include "test.h"
#include "ui_fakes.h"

#include <cstring>
#include <string>
#include <vector>

#include "formats/instrument_pti.h"
#include "formats/wav.h"
#include "app/project_io.h"

using namespace ot;
using namespace ot::audio;

namespace {
const std::string kSrc = "Projects/P/instruments/1 long.pti";
const std::string kSrcShort = "Projects/P/instruments/2 short.pti";
const std::string kWs = "Workspace/instruments/instrument_01.pti";
const std::string kWsShort = "Workspace/instruments/instrument_02.pti";
const std::string kMarker = "Workspace/instruments/instrument_01.src";
const std::string kHdr = "Workspace/instruments/instrument_01.hdr";
constexpr uint32_t kLongFrames = 300000;
constexpr uint32_t kPayload = kLongFrames * 2;

void addWav(Harness& h, const char* path, uint32_t frames, uint32_t seed) {
    std::vector<int16_t> s(frames);
    uint32_t x = seed;
    for (size_t i = 0; i < s.size(); ++i) {
        x = x * 1664525u + 1013904223u;
        s[i] = int16_t(int32_t(x >> 17) - 16384);
    }
    std::vector<uint8_t> w(s.size() * 2 + 64);
    w.resize(wavEncode(s.data(), frames, 1, w.data(), w.size()));
    h.fs.files[path] = w;
}
// Projects/P: instrument 1 long (6.8 s), instrument 2 short; step 1 plays 1.
// Then a new project, so the Open below starts from an empty Workspace.
void makeProject(Harness& h) {
    App& a = *h.app;
    addWav(h, "long.wav", kLongFrames, 3);
    addWav(h, "short.wav", 20000, 9);
    CHECK(a.importSample("long.wav", 0));
    CHECK(a.importSample("short.wav", 1));
    Step& st = a.pattern().tracks[0].steps[0];
    st.note = 60;
    st.instrument = 0;
    a.patternEdited();
    CHECK(a.saveProjectAs("P"));
    CHECK(h.fs.files.count(kSrc));
    CHECK(a.newProject());
}
void openP(Harness& h, uint8_t mode) {
    makeProject(h);
    h.app->cfg.streamMode = mode;
    CHECK(h.app->openProject("Projects", "P"));
}
uint64_t renderHash(Harness& h, int blocks) {
    h.app->seq.play(0);
    h.audio.hash = 1469598103934665603ull;
    for (int k = 0; k < blocks; ++k) {
        h.audio.pump(2);
        h.run(10);
    }
    h.app->seq.stop();
    return h.audio.hash;
}
// Playing (the audio pumped) for `ms`.
void playFor(Harness& h, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
        h.audio.pump(2);
        h.run(10);
    }
}
uint32_t underruns(Harness& h) {
    uint32_t n = 0;
    for (int v = 0; v < kVoices; ++v) n += h.app->streamRings()[v].underruns;
    return n;
}
bool has(Harness& h, const std::string& p) { return h.fs.files.count(p) != 0; }
bool sameFile(Harness& h, const std::string& a, const std::string& b) {
    return has(h, a) && has(h, b) && h.fs.files[a] == h.fs.files[b];
}
}  // namespace

TEST(open_streams_from_project_without_copying_payload) {
    Harness ram, st;
    openP(ram, 0);
    const JobProfile rp = jobProfile();
    openP(st, 2);
    const JobProfile sp = jobProfile();
    App& a = *st.app;
    CHECK(a.streamed(0));
    CHECK(a.bank.partial(0));
    CHECK_EQ(a.instruments[0].lengthFrames, kLongFrames);
    CHECK(!has(st, kWs));
    CHECK(has(st, kMarker));
    // The payload is neither read past the head nor written.
    CHECK(sp.bytesRead + kPayload - kStreamHeadFrames * 2u <= rp.bytesRead + 0x1000);
    CHECK(sp.bytesWritten + kPayload <= rp.bytesWritten + 0x1000);
    CHECK(renderHash(ram, 600) == renderHash(st, 600));
    CHECK_EQ(underruns(st), 0u);
    CHECK(st.streamReader.reads > 0);
}

TEST(open_copies_non_streamed_slots_as_before) {
    Harness h;
    openP(h, 2);
    CHECK(!h.app->streamed(1));
    CHECK(sameFile(h, kWsShort, kSrcShort));
    CHECK(!has(h, "Workspace/instruments/instrument_02.src"));
}

TEST(open_bind_failure_copies_like_before) {
    Harness h;
    makeProject(h);
    h.streamReader.failBindPath = kSrc.c_str();
    h.app->cfg.streamMode = 2;
    CHECK(h.app->openProject("Projects", "P"));
    App& a = *h.app;
    CHECK(!a.streamed(0));
    CHECK_EQ(a.bank.frames(0), kLongFrames);
    CHECK(sameFile(h, kWs, kSrc));
    CHECK(!has(h, kMarker));
}

TEST(idle_autosave_copies_and_rebinds) {
    Harness ram, h;
    openP(ram, 0);
    openP(h, 2);
    h.run(11000);   // stopped: the autosave copies
    CHECK(sameFile(h, kWs, kSrc));
    CHECK(!has(h, kMarker));
    CHECK(h.app->streamed(0));
    // The stream reads the Workspace copy now.
    h.fs.files.erase(kSrc);
    CHECK(renderHash(ram, 600) == renderHash(h, 600));
    CHECK_EQ(underruns(h), 0u);
}

TEST(copy_waits_while_playing) {
    Harness h;
    openP(h, 2);
    h.app->seq.play(0);
    playFor(h, 30000);
    CHECK(!has(h, kWs));
    CHECK(has(h, kMarker));
    CHECK_EQ(underruns(h), 0u);
    h.app->seq.stop();
    playFor(h, 11000);   // the last note rings out, then the autosave copies
    CHECK(sameFile(h, kWs, kSrc));
    CHECK(!has(h, kMarker));
}

TEST(header_edit_while_playing_waits_for_the_idle_copy) {
    // The edit is kept in instrument_01.hdr; nothing is copied while playing.
    Harness h;
    openP(h, 2);
    App& a = *h.app;
    a.seq.play(0);
    playFor(h, 500);
    a.instruments[0].volume = 40;
    a.markInstrumentDirty(0);
    playFor(h, 12000);
    CHECK(a.seq.isPlaying());
    CHECK(!has(h, kWs));
    CHECK(has(h, kMarker));
    CHECK(has(h, kHdr));
    CHECK(a.streamed(0));
    CHECK_EQ(underruns(h), 0u);
    a.seq.stop();
    playFor(h, 11000);
    CHECK(has(h, kWs));
    CHECK(!has(h, kMarker));
    CHECK(!has(h, kHdr));
    const std::vector<uint8_t>& ws = h.fs.files[kWs];
    const std::vector<uint8_t>& src = h.fs.files[kSrc];
    CHECK_EQ(ws.size(), src.size());
    CHECK(std::equal(src.begin() + pti::kDataOff, src.end(), ws.begin() + pti::kDataOff));
    Instrument ins;
    CHECK(decodePtiHeader(ws.data(), ws.size(), ins) == FmtStatus::Ok);
    CHECK_EQ(ins.volume, 40);
    CHECK(a.streamed(0));
}

TEST(header_edit_on_a_pending_slot_survives_power_off) {
    Harness h;
    openP(h, 2);
    App& a = *h.app;
    a.instruments[0].volume = 40;
    a.markInstrumentDirty(0);
    a.autosave(StreamCopies::kNone);   // power-off
    CHECK(!has(h, kWs));
    CHECK(has(h, kHdr));
    CHECK(a.loadWorkspace());
    CHECK_EQ(a.instruments[0].volume, 40);
    CHECK_EQ(a.instruments[0].lengthFrames, kLongFrames);
    CHECK(a.streamed(0));
    CHECK(has(h, kMarker));
    CHECK(!has(h, kWs));
}

TEST(header_edit_kept_when_the_reload_copies) {
    Harness h;
    openP(h, 2);
    App& a = *h.app;
    a.instruments[0].volume = 40;
    a.markInstrumentDirty(0);
    a.autosave(StreamCopies::kNone);
    a.cfg.streamMode = 0;
    CHECK(a.loadWorkspace());
    CHECK(!a.streamed(0));
    CHECK_EQ(a.instruments[0].volume, 40);
    const std::vector<uint8_t>& ws = h.fs.files[kWs];
    Instrument ins;
    CHECK(decodePtiHeader(ws.data(), ws.size(), ins) == FmtStatus::Ok);
    CHECK_EQ(ins.volume, 40);
    CHECK_EQ(ws.size(), h.fs.files[kSrc].size());
    CHECK(!has(h, kMarker));
    CHECK(!has(h, kHdr));
}

TEST(save_as_over_the_source_keeps_the_sample) {
    Harness ram, h;
    openP(ram, 0);
    openP(h, 2);
    const std::vector<uint8_t> orig = h.fs.files[kSrc];
    CHECK(h.app->saveProjectAs("P"));
    CHECK(has(h, kSrc));
    CHECK(h.fs.files[kSrc] == orig);
    CHECK(sameFile(h, kWs, kSrc));
    CHECK(!has(h, kMarker));
    CHECK(h.app->streamed(0));
    CHECK(renderHash(ram, 600) == renderHash(h, 600));
}

TEST(save_as_elsewhere_copies_first) {
    Harness h;
    openP(h, 2);
    CHECK(h.app->saveProjectAs("Q"));
    CHECK(sameFile(h, "Projects/Q/instruments/1 long.pti", kSrc));
    CHECK(sameFile(h, kWs, kSrc));
    CHECK(!has(h, kMarker));
}

TEST(reload_with_marker_streams_from_source) {
    // A power cut before the copy: the next start streams from the project.
    Harness ram, h;
    openP(ram, 0);
    openP(h, 2);
    CHECK(h.app->loadWorkspace());
    CHECK(jobProfile().bytesRead < kPayload / 2);
    App& a = *h.app;
    CHECK(a.streamed(0));
    CHECK_EQ(a.instruments[0].lengthFrames, kLongFrames);
    CHECK(has(h, kMarker));
    CHECK(!has(h, kWs));
    CHECK(renderHash(ram, 600) == renderHash(h, 600));
}

TEST(reload_with_marker_and_streaming_off_copies) {
    Harness h;
    openP(h, 2);
    h.app->cfg.streamMode = 0;
    CHECK(h.app->loadWorkspace());
    CHECK(!h.app->streamed(0));
    CHECK_EQ(h.app->bank.frames(0), kLongFrames);
    CHECK(sameFile(h, kWs, kSrc));
    CHECK(!has(h, kMarker));
}

TEST(reload_with_missing_source_clears_slot_and_workspace_still_loads) {
    Harness h;
    openP(h, 2);
    h.fs.files.erase(kSrc);
    App& a = *h.app;
    CHECK(a.loadWorkspace());
    CHECK(!a.instrPresent[0]);
    CHECK(!a.streamed(0));
    CHECK(a.instrPresent[1]);
    CHECK_EQ(a.pattern().tracks[0].steps[0].note, 60);
    CHECK(std::strstr(a.ui.notice(), "project file missing") != nullptr);
    CHECK(!has(h, kMarker));
    CHECK(!has(h, kWs));
}

TEST(reload_with_half_copy_prefers_source) {
    Harness h;
    openP(h, 2);
    const std::vector<uint8_t>& src = h.fs.files[kSrc];
    h.fs.files[kWs] = std::vector<uint8_t>(src.begin(), src.begin() + 0x8000);   // a copy cut short
    App& a = *h.app;
    CHECK(a.loadWorkspace());
    CHECK(a.streamed(0));
    CHECK_EQ(a.instruments[0].lengthFrames, kLongFrames);
    CHECK(has(h, kMarker));
    h.run(11000);
    CHECK(sameFile(h, kWs, kSrc));
    CHECK(!has(h, kMarker));
}

TEST(load_into_memory_from_the_source) {
    Harness ram, h;
    openP(ram, 0);
    openP(h, 2);
    App& a = *h.app;
    CHECK(a.loadStreamedIntoMemory(0));
    CHECK(!a.streamed(0));
    CHECK_EQ(a.bank.frames(0), kLongFrames);
    CHECK(std::equal(a.bank.left(0), a.bank.left(0) + kLongFrames, ram.app->bank.left(0)));
    a.autosave();
    const std::vector<uint8_t>& ws = h.fs.files[kWs];
    const std::vector<uint8_t>& src = h.fs.files[kSrc];
    CHECK_EQ(ws.size(), src.size());
    CHECK(std::equal(src.begin() + pti::kDataOff, src.end(), ws.begin() + pti::kDataOff));
    CHECK(!has(h, kMarker));
}

TEST(copy_instrument_from_pending_source) {
    Harness ram, h;
    openP(ram, 0);
    openP(h, 2);
    App& a = *h.app;
    CHECK(a.copyInstrument(0, 5));
    CHECK(a.streamed(0));
    CHECK(!a.streamed(5));
    CHECK_EQ(a.bank.frames(5), kLongFrames);
    CHECK(std::equal(a.bank.left(5), a.bank.left(5) + kLongFrames, ram.app->bank.left(0)));
}

TEST(delete_pending_slot_removes_marker) {
    Harness h;
    openP(h, 2);
    App& a = *h.app;
    CHECK(a.deleteInstrument(0));
    a.autosave();
    CHECK(!has(h, kMarker));
    CHECK(!has(h, kWs));
    CHECK(a.loadWorkspace());
    CHECK(!a.instrPresent[0]);
    CHECK(a.instrPresent[1]);
}

TEST(new_project_drops_pending_copy_without_reading_payload) {
    Harness h;
    openP(h, 2);
    App& a = *h.app;
    h.clock.ms += 11000;
    a.loop();   // the idle autosave starts
    CHECK(a.jobBusy());
    CHECK(a.newProject());
    CHECK(jobProfile().op == uint8_t(JobOp::Autosave));
    CHECK(jobProfile().bytesRead < kPayload / 2);
    CHECK(!has(h, kMarker));
    CHECK(!has(h, kWs));
    CHECK(!a.streamed(0));
}

TEST(usb_mode_finishes_copies_before_handing_the_card) {
    Harness h;
    openP(h, 2);
    App& a = *h.app;
    CHECK(a.enterUsbStorage() == App::UsbEnter::kOk);
    CHECK(h.fs.locked);
    h.run(100);
    CHECK_EQ(h.fs.lockedAccess, 0);
    h.fs.locked = false;
    CHECK(sameFile(h, kWs, kSrc));
    CHECK(!has(h, kMarker));
    h.fs.locked = true;
    a.exitUsbStorage();
    CHECK(a.instrPresent[0]);
}

TEST(rebind_failure_falls_back_to_ram) {
    Harness ram, h;
    openP(ram, 0);
    openP(h, 2);
    h.streamReader.failBindPath = kWs.c_str();
    h.run(11000);
    App& a = *h.app;
    CHECK(sameFile(h, kWs, kSrc));
    CHECK(!has(h, kMarker));
    CHECK(!a.streamed(0));
    CHECK_EQ(a.bank.frames(0), kLongFrames);
    CHECK(renderHash(ram, 600) == renderHash(h, 600));
}


TEST(copy_onto_a_pending_slot_without_memory_keeps_it) {
    // Instruments 1 and 3 stream from Projects/P; the sample memory is then
    // nearly full: copying 1 onto 3 is refused and 3 stays as it was, so a
    // Save over the folder still has its file.
    Harness h;
    App& a = *h.app;
    addWav(h, "long.wav", kLongFrames, 3);
    addWav(h, "long2.wav", kLongFrames, 5);
    addWav(h, "short.wav", 20000, 9);
    CHECK(a.importSample("long.wav", 0));
    CHECK(a.importSample("short.wav", 1));
    CHECK(a.importSample("long2.wav", 2));
    CHECK(a.saveProjectAs("P"));
    CHECK(a.newProject());
    a.cfg.streamMode = 2;
    CHECK(a.openProject("Projects", "P"));
    const std::string src3 = "Projects/P/instruments/3 long2.pti";
    const std::vector<uint8_t> orig = h.fs.files[src3];
    CHECK(a.streamCopyPending(2));
    addWav(h, "fill.wav", uint32_t(a.bank.freeSamples()) - 100000, 7);
    CHECK(a.importSample("fill.wav", 10));
    CHECK(!a.copyInstrument(0, 2));
    CHECK(a.instrPresent[2]);
    CHECK(a.streamed(2));
    CHECK(a.streamCopyPending(2));
    CHECK(a.saveProjectAs("P"));
    CHECK(h.fs.files.count(src3) && h.fs.files[src3] == orig);
    CHECK(sameFile(h, "Workspace/instruments/instrument_03.pti", src3));
}

TEST(save_with_a_copy_that_cannot_finish_keeps_the_project) {
    Harness h;
    openP(h, 2);
    const std::vector<uint8_t> orig = h.fs.files[kSrc];
    h.fs.capacity = h.fs.usedBytes() + 0x10000;   // the card fills during the copy
    CHECK(!h.app->saveProjectAs("P"));
    CHECK(h.fs.files.count(kSrc) && h.fs.files[kSrc] == orig);
    CHECK(has(h, kMarker));
    CHECK(h.app->streamed(0));
}

TEST(missing_project_file_is_reported_once) {
    Harness h;
    openP(h, 2);
    App& a = *h.app;
    h.fs.files.erase(kSrc);
    h.run(11000);
    CHECK(std::strstr(a.ui.notice(), "project file missing") != nullptr);
    a.ui.notify("other", 60000);
    h.run(25000);   // idle autosaves do not try again
    CHECK_STR(a.ui.notice(), "other");
    CHECK(has(h, kMarker));
}

TEST(usb_mode_names_an_instrument_it_could_not_copy) {
    Harness h;
    openP(h, 2);
    App& a = *h.app;
    h.fs.files.erase(kSrc);
    CHECK(a.enterUsbStorage() == App::UsbEnter::kOk);
    CHECK(std::strstr(a.ui.notice(), "Instrument 1 not copied") != nullptr);
}

TEST(damaged_marker_leaves_the_slot_empty) {
    Harness h;
    openP(h, 2);
    App& a = *h.app;
    h.fs.files[kMarker] = std::vector<uint8_t>{'3', '9'};   // cut while written
    CHECK(a.loadWorkspace());
    CHECK(!a.instrPresent[0]);
    CHECK(std::strstr(a.ui.notice(), "could not be restored") != nullptr);
    CHECK(!has(h, kMarker));
    CHECK(a.instrPresent[1]);
}

TEST(damaged_marker_keeps_a_complete_workspace_file) {
    Harness h;
    openP(h, 2);
    App& a = *h.app;
    h.fs.files[kWs] = h.fs.files[kSrc];
    h.fs.files[kMarker] = std::vector<uint8_t>{'3', '9'};
    CHECK(a.loadWorkspace());
    CHECK(a.instrPresent[0]);
    CHECK(a.streamed(0));
    CHECK(!a.streamCopyPending(0));
    CHECK(!has(h, kMarker));
    CHECK(sameFile(h, kWs, kSrc));
}
