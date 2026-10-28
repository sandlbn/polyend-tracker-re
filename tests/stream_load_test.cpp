// SD streaming C8 (docs/PLAN_SD_STREAMING_P1C_P2.md §2.2): head-only loading.
// With the option on, a long streamable instrument loads only its head (and
// the head of its loop) into the bank; the slot is partial, the instrument
// keeps its full length, its stream is bound; played through the pattern it
// renders bit for bit like the same instrument loaded into RAM, without an
// underrun. Short or non-streamable instruments still load whole.
#include "test.h"
#include "ui_fakes.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "formats/wav.h"
#include "app/sample_fx.h"
#include "ui/screens.h"

using namespace ot;
using namespace ot::audio;

namespace {
// A mono WAV of `frames` frames (full-band, so any misplaced frame shows).
void addWav(Harness& h, const char* path, uint32_t frames, uint32_t seed, int channels = 1) {
    std::vector<int16_t> s(size_t(frames) * channels);
    uint32_t x = seed;
    for (size_t i = 0; i < s.size(); ++i) {
        x = x * 1664525u + 1013904223u;
        s[i] = int16_t(int32_t(x >> 17) - 16384);
    }
    std::vector<uint8_t> w(s.size() * 2 + 64);
    w.resize(wavEncode(s.data(), frames, uint16_t(channels), w.data(), w.size()));
    h.fs.files[path] = w;
}
// Workspace with instrument 1 = a long sample (and instrument 2 a short one),
// saved; then reloaded with streaming `mode`.
void setupAndReload(Harness& h, uint8_t mode, uint8_t playMode = uint8_t(PlayMode::OneShot), int longSlot = 0,
                    int shortSlot = 1, int loop1 = -1, int channels = 1) {
    App& a = *h.app;
    addWav(h, "long.wav", 300000, 3, channels);   // 6.8 s
    addWav(h, "short.wav", 20000, 9);
    CHECK(a.importSample("long.wav", longSlot));
    CHECK(a.importSample("short.wav", shortSlot));
    a.instruments[longSlot].playMode = playMode;
    if (loop1 >= 0) a.instruments[longSlot].loopPoint1 = uint16_t(loop1);
    a.markInstrumentDirty(longSlot, false);
    Step& st = a.pattern().tracks[0].steps[0];
    st.note = 60;
    st.instrument = uint8_t(longSlot);
    a.patternEdited();
    a.autosave();
    a.cfg.streamMode = mode;
    CHECK(a.loadWorkspace());
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
}  // namespace

TEST(load_all_streams_long_instruments_head_only) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2);
    CHECK(a.streamingOn());
    CHECK(a.bank.partial(0));
    CHECK_EQ(a.bank.frames(0), kStreamHeadFrames);
    CHECK_EQ(a.instruments[0].lengthFrames, 300000u);
    CHECK(a.instrPresent[0]);
    CHECK(h.streamReader.bound(0));
    CHECK(a.engine.streamSource(0) != nullptr);
    CHECK(a.streamed(0));
    // The short one stays whole in RAM.
    CHECK(!a.bank.partial(1));
    CHECK(!a.streamed(1));
    CHECK_EQ(a.bank.frames(1), 20000u);
}

TEST(load_keeps_nonstreamable_play_modes_in_ram) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2, uint8_t(PlayMode::Slice));
    CHECK(!a.bank.partial(0));
    CHECK(!a.streamed(0));
    CHECK_EQ(a.bank.frames(0), 300000u);
}

TEST(streamed_instrument_renders_like_ram) {
    Harness ram, st;
    setupAndReload(ram, 0);
    setupAndReload(st, 2);
    CHECK(!ram.app->streamed(0));
    CHECK(st.app->streamed(0));
    const uint64_t a = renderHash(ram, 600), b = renderHash(st, 600);   // ~3.5 s, past the head
    CHECK(a == b);
    uint32_t under = 0;
    for (int v = 0; v < kVoices; ++v) under += st.app->streamRings()[v].underruns;
    CHECK_EQ(under, 0u);
    CHECK(st.streamReader.reads > 0);
}

// ---- C9: edits on streamed instruments (docs/PLAN_SD_STREAMING_P1C_P2.md §2.4-2.5) ----
TEST(delete_unbinds_a_streamed_slot) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2);
    CHECK(a.streamed(0));
    CHECK(a.deleteInstrument(0));
    CHECK(!a.streamed(0));
    CHECK(!h.streamReader.bound(0));
    CHECK(a.engine.streamSource(0) == nullptr);
    CHECK(!a.bank.loaded(0));
}

TEST(copy_of_a_streamed_slot_is_a_full_ram_copy) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2);
    CHECK(a.copyInstrument(0, 5));
    CHECK(a.streamed(0));                  // the source keeps streaming
    CHECK(!a.streamed(5));
    CHECK(!a.bank.partial(5));
    CHECK_EQ(a.bank.frames(5), 300000u);   // the whole sample, not the head
    CHECK_EQ(a.instruments[5].lengthFrames, 300000u);
}

TEST(load_into_memory_replaces_the_head) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2);
    CHECK(a.loadStreamedIntoMemory(0));
    CHECK(!a.streamed(0));
    CHECK(!a.bank.partial(0));
    CHECK_EQ(a.bank.frames(0), 300000u);
    CHECK_EQ(a.instruments[0].lengthFrames, 300000u);
    CHECK(!h.streamReader.bound(0));
}

TEST(autosave_never_rewrites_a_streamed_payload) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2);
    const std::string p = "Workspace/instruments/instrument_01.pti";
    CHECK(h.fs.files.count(p));
    const std::vector<uint8_t> before = h.fs.files[p];
    a.instruments[0].volume = 40;
    a.markInstrumentDirty(0, true);   // even a sample-dirty mark writes the header only
    a.autosave();
    const std::vector<uint8_t>& after = h.fs.files[p];
    CHECK_EQ(after.size(), before.size());
    CHECK(std::equal(before.begin() + 0x188, before.end(), after.begin() + 0x188));   // payload untouched
    CHECK(a.streamed(0));
}

TEST(streamed_slot_survives_a_bank_move) {
    // The streamed instrument is slot 2; deleting slot 1 below it moves its
    // head down in the bank: the stream source is re-pointed and it still
    // renders like the same instrument in RAM.
    Harness ram, st;
    setupAndReload(ram, 0, uint8_t(PlayMode::OneShot), 2, 1);
    setupAndReload(st, 2, uint8_t(PlayMode::OneShot), 2, 1);
    CHECK(st.app->streamed(2));
    CHECK(st.app->deleteInstrument(1));
    CHECK(ram.app->deleteInstrument(1));
    CHECK(st.app->streamed(2));
    CHECK(st.app->engine.streamSource(2)->headL == st.app->bank.left(2));
    CHECK(renderHash(ram, 600) == renderHash(st, 600));
}

TEST(loop_point_edit_drops_the_stale_loop_head) {
    // ForwardLoop with the loop start past the head: the loop head is resident.
    // Moving the loop point leaves that copy pointing at the old position, so
    // it is dropped and wraps read the new loop start from the card.
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2, uint8_t(PlayMode::ForwardLoop), 0, 1, 40000);
    CHECK(a.streamed(0));
    const StreamSource* s = a.engine.streamSource(0);
    CHECK(s != nullptr);
    CHECK(s->loopFrames > 0);
    a.instruments[0].loopPoint1 = 30000;
    a.markInstrumentDirty(0);
    CHECK_EQ(s->loopFrames, 0u);
    CHECK(a.streamed(0));
}

TEST(loop_point_unchanged_keeps_the_loop_head) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2, uint8_t(PlayMode::ForwardLoop), 0, 1, 40000);
    const StreamSource* s = a.engine.streamSource(0);
    const uint32_t lf = s->loopFrames;
    a.instruments[0].volume = 30;
    a.markInstrumentDirty(0);
    CHECK_EQ(s->loopFrames, lf);
}

TEST(play_mode_change_on_a_streamed_slot_notifies) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2);
    a.instruments[0].playMode = uint8_t(PlayMode::Slice);
    a.markInstrumentDirty(0);
    CHECK(std::strstr(a.ui.notice(), "load it into memory") != nullptr);
    CHECK(a.streamed(0));   // still streamed; the voice refuses the mode
}

TEST(sample_dirty_mark_on_a_streamed_slot_stays_header_only) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2);
    a.markInstrumentDirty(0, true);
    CHECK(!a.instrumentSampleDirty(0));
}

TEST(resample_never_lands_in_a_streamed_slot) {
    // Into a streamed slot: the bank holds only its head, so it is never an
    // in-place overwrite (of the head only); the take goes to the next free slot.
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2, uint8_t(PlayMode::OneShot), 0, 1, -1, 2);
    CHECK(a.streamed(0));
    CHECK(a.bank.stereo(0));
    a.resampleCfg.into = 1;   // instrument 1 = slot 0
    bool inPlace = true, fits = true;
    const int slot = a.resampleTarget(kStreamHeadFrames, &inPlace, &fits);
    CHECK(!inPlace);
    CHECK(slot != 0);
    CHECK(!fits);
}

TEST(editor_entry_asks_to_load_a_streamed_sample) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2);
    a.setCurrentInstrument(0);
    h.tap(btn::SampleEditor);
    CHECK(a.ui.askOpen());
    CHECK(std::strstr(a.ui.askText(), "streams from the card") != nullptr);
    h.tap(btn::F8);   // Load
    CHECK(!a.ui.askOpen());
    CHECK(!a.streamed(0));
    CHECK_EQ(a.bank.frames(0), 300000u);
    CHECK_EQ(a.ui.active(), int(kModEditor));
    CHECK(!static_cast<EditorScreen*>(a.ui.screen(kModEditor))->inert);
}

TEST(editor_entry_cancel_keeps_streaming) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2);
    a.setCurrentInstrument(0);
    h.tap(btn::SampleEditor);
    CHECK(a.ui.askOpen());
    h.tap(btn::F7);   // Cancel
    CHECK(!a.ui.askOpen());
    CHECK(a.streamed(0));
    CHECK(a.ui.active() != int(kModEditor));
}

TEST(slicer_and_zero_marks_skip_a_streamed_sample) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2);
    a.setCurrentInstrument(0);
    PlaybackScreen& p = *static_cast<PlaybackScreen*>(a.ui.screen(kModPlayback));
    a.instruments[0].sliceCount = 3;
    p.slicesAuto();
    CHECK_EQ(a.instruments[0].sliceCount, 3);   // untouched: the data is not resident
    CHECK(std::strstr(a.ui.notice(), "load it into memory") != nullptr);
    CHECK_EQ(p.zeroMask(), 0);
}

// ---- C10: Config rows, apply -> reload, marks, notices (§2.7) ----
namespace {
constexpr int kGeneralCat = 1;
constexpr int kStreamRow = 15, kThresholdRow = 16;   // after Export theme, Export sample rate
void runJob(Harness& h) {
    for (int i = 0; i < 4000 && h.app->jobBusy(); ++i) h.run(10);
}
bool drew(Harness& h, const char* s) {
    for (const std::string& l : h.disp.last)
        if (l.find(s) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST(config_stream_apply_reloads_workspace) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 0);
    CHECK(!a.streamed(0));
    h.tap(btn::Config);
    ConfigScreen& c = *static_cast<ConfigScreen*>(a.ui.screen(kModConfig));
    char t[48];
    c.optionText(kGeneralCat, kStreamRow, t, sizeof t);
    CHECK_STR(t, "Off");
    c.optionText(kGeneralCat, kThresholdRow, t, sizeof t);
    CHECK_STR(t, "10 s");
    c.applyOption(kGeneralCat, kStreamRow, 2);   // All
    CHECK_EQ(a.cfg.streamMode, 2);
    runJob(h);
    CHECK(a.streamingOn());
    CHECK(a.streamed(0));
    CHECK_EQ(a.instruments[0].lengthFrames, 300000u);
    CHECK_EQ(a.ui.active(), int(kModConfig));   // the Config screen stays
    c.applyOption(kGeneralCat, kStreamRow, 0);   // Off: everything back in RAM
    runJob(h);
    CHECK(!a.streamingOn());
    CHECK(!a.streamed(0));
    CHECK_EQ(a.bank.frames(0), 300000u);
    // The threshold applies the same way (Auto: 6.8 s streams at 5 s only).
    c.applyOption(kGeneralCat, kStreamRow, 1);   // Auto, 10 s
    runJob(h);
    CHECK(!a.streamed(0));
    c.applyOption(kGeneralCat, kThresholdRow, 0);   // 5 s
    runJob(h);
    CHECK_EQ(a.cfg.streamThreshold, 0);
    CHECK(a.streamed(0));
}

TEST(load_notice_counts_streamed_instruments) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 0);
    a.cfg.streamMode = 2;
    CHECK(a.startLoadWorkspace());
    runJob(h);
    CHECK(a.streamed(0));
    CHECK_STR(a.ui.notice(), "1 instrument streams from the card");
}

TEST(streamed_instrument_is_marked_on_screens) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2);
    a.setCurrentInstrument(0);
    h.tap(btn::SamplePlayback);
    h.run(50);
    CHECK(drew(h, "1. long (SD)"));
    h.tap(btn::InstrParams);
    h.run(50);
    CHECK(drew(h, "1. long (SD)"));
    a.setCurrentInstrument(1);   // RAM instrument: no mark
    h.tap(btn::SamplePlayback);
    h.run(50);
    CHECK(drew(h, "2. short"));
    CHECK(!drew(h, "short (SD)"));
}

TEST(loader_marks_streamed_and_counts_them_in_memory) {
    Harness h;
    App& a = *h.app;
    setupAndReload(h, 2);
    a.setCurrentInstrument(0);
    h.tap(btn::SampleLoader);
    h.run(50);
    CHECK(drew(h, "1.~long"));
    CHECK(drew(h, "2.short"));
    CHECK(std::strstr(a.ui.bar.label[7].bottom, "+1") != nullptr);
}

TEST(editor_apply_and_undo_repoint_a_later_streamed_slot) {
    // The Sample Editor's apply / undo (SampleBank::replaceFromTail) moves
    // every slot after the edited one when the length changes: a streamed
    // instrument above it is re-pointed and still renders like RAM.
    Harness ram, st;
    setupAndReload(ram, 0, uint8_t(PlayMode::OneShot), 2, 1);
    setupAndReload(st, 2, uint8_t(PlayMode::OneShot), 2, 1);
    CHECK(st.app->streamed(2));
    for (Harness* h : {&ram, &st}) {
        App& a = *h->app;
        a.setCurrentInstrument(1);   // the short RAM sample below the streamed one
        h->tap(btn::Pattern);
        static_cast<PatternScreen*>(a.ui.screen(kModPattern))->step = 1;   // an empty step (#52)
        h->tap(btn::SampleEditor);
        EditorScreen& e = *static_cast<EditorScreen*>(a.ui.screen(kModEditor));
        CHECK(!e.inert);
        e.effect = sfx::kCrop;
        e.startM = 0;
        e.endM = 32767;
        CHECK(e.apply());   // slot 1 shrinks: slot 2 moves down
        CHECK(a.bank.frames(1) < 20000u);
        if (a.streamed(2)) CHECK(a.engine.streamSource(2)->headL == a.bank.left(2));
        CHECK(e.undo());    // slot 1 grows back: slot 2 moves up
        CHECK_EQ(a.bank.frames(1), 20000u);
        if (a.streamed(2)) CHECK(a.engine.streamSource(2)->headL == a.bank.left(2));
        e.effect = sfx::kCrop;
        e.startM = 0;
        e.endM = 32767;
        CHECK(e.apply());
        h->tap(btn::Pattern);
    }
    CHECK(st.app->streamed(2));
    CHECK(st.app->engine.streamSource(2)->headL == st.app->bank.left(2));
    CHECK(renderHash(ram, 600) == renderHash(st, 600));
}

TEST(config_refuses_streaming_off_when_the_project_needs_it) {
    // Two stems that fit only while streaming: turning Stream from card Off
    // would drop one at the reload (and a later save could lose its file).
    // Config keeps the option on and says why; nothing is reloaded.
    Harness h;
    App& a = *h.app;
    addWav(h, "big.wav", 2200000, 5);   // 4.4 MB: two do not fit the 8 MB bank
    CHECK(a.importSample("big.wav", 0));
    a.autosave();
    const std::string p1 = "Workspace/instruments/instrument_01.pti", p2 = "Workspace/instruments/instrument_02.pti";
    CHECK(h.fs.files.count(p1));
    h.fs.files[p2] = h.fs.files[p1];
    a.cfg.streamMode = 2;
    CHECK(a.loadWorkspace());
    CHECK(a.streamed(0));
    CHECK(a.streamed(1));
    h.tap(btn::Config);
    ConfigScreen& c = *static_cast<ConfigScreen*>(a.ui.screen(kModConfig));
    c.applyOption(kGeneralCat, kStreamRow, 0);   // Off
    runJob(h);
    CHECK_EQ(a.cfg.streamMode, 2);
    CHECK(std::strstr(a.ui.notice(), "Stream from card") != nullptr);
    CHECK(a.streamed(0));
    CHECK(a.streamed(1));
    CHECK(h.fs.files.count(p1) && h.fs.files.count(p2));
}
