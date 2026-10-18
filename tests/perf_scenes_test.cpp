// [I] Performance scenes A / B and the crossfader (Octatrack-style, not in
// stock): storing with Shift+Left / Shift+Right, blending per FX type with
// the encoder or CC 63, stepped FX switching at the middle, filters of
// different types crossing over, perf_scenes.bin next to project.mt.
#include <cstring>

#include "test.h"
#include "ui_fakes.h"
#include "nvstore_fake.h"

#include "app/perf_presets.h"
#include "formats/file_io.h"
#include "formats/perf_scenes.h"
#include "ui/screens.h"

using namespace ot;

namespace {

struct Rig {
    FakeNvStore nv;
    std::unique_ptr<PerfPatternMemory> mem{new PerfPatternMemory};
    Harness h{false};
    Rig() {
        h.app->setNvStore(&nv);
        h.app->seq.setPerfPatternMemory(mem.get());
        h.app->boot();
        h.app->ui.fragments.enabled = false;   // title text drawn inline every frame
        h.tap(btn::Performance);
    }
    App& a() { return *h.app; }
    PerformanceScreen& p() { return *static_cast<PerformanceScreen*>(h.app->ui.screen(kModPerformance)); }
    void store(int scene) {
        h.down(btn::Shift);
        h.tap(scene ? btn::Right : btn::Left);
        h.up(btn::Shift);
    }
    // Fader to x (0..100) with the encoder (2 per detent).
    void fade(int x) { h.enc(int8_t((x - p().fader) / PerformanceScreen::kFaderStep)); }
    const audio::PerfOffsets& voice(int t) { return h.app->engine.voice(t).perf(); }
};

}  // namespace

TEST(perf_scene_volume_blend_with_encoder) {
    Rig r;
    PerformanceScreen& p = r.p();
    // Slot 0 = Volume (default slot types are FX 1..12).
    p.src.setValue(0, 1, -50);
    p.src.setValue(0, 2, 50);
    r.h.pad(12 * 1 + 0);   // row 1
    r.store(0);
    r.h.pad(12 * 2 + 0);   // row 2
    r.store(1);
    CHECK_EQ(r.a().project.perfScene[0][0], 1);
    CHECK_EQ(r.a().project.perfScene[1][0], 2);
    CHECK_EQ(r.a().project.perfScene[0][1], 0);
    CHECK_EQ(p.fader, 0);
    CHECK_EQ(p.cur[0], 50);   // storing changes nothing until the fader moves
    r.fade(50);
    CHECK_EQ(p.fader, 50);
    CHECK_EQ(p.cur[0], 0);
    CHECK_EQ(r.voice(0).volume, 0);
    r.fade(76);
    CHECK_EQ(p.cur[0], 26);
    r.fade(100);
    CHECK_EQ(p.cur[0], 50);
    CHECK_EQ(r.voice(3).volume, 50);
    r.h.enc(10);   // clamped at B
    CHECK_EQ(p.fader, 100);
    r.fade(0);
    CHECK_EQ(p.cur[0], -50);
    CHECK_EQ(r.voice(0).volume, -50);
    // The title shows the fader once a scene exists.
    r.fade(50);
    r.h.run(40);
    CHECK(r.h.disp.lastHasText("A -----|----- B"));
}

TEST(perf_scene_same_type_in_different_slots) {
    Rig r;
    PerformanceScreen& p = r.p();
    p.src.setValue(0, 1, 40);         // A: Volume in slot 0
    p.src.setType(5, perf::kVolume);  // B: Volume in slot 5
    p.src.setValue(5, 1, -40);
    r.h.pad(12 + 0);
    r.store(0);
    r.h.pad(12 + 5);   // resets slot 0 (same type)
    r.store(1);
    CHECK_EQ(r.a().project.perfScene[1][0], 0);
    CHECK_EQ(r.a().project.perfScene[1][5], 1);
    r.fade(24);
    CHECK_EQ(p.cur[0], 21);   // 40 -> -40 at 24 %, in A's slot
    CHECK_EQ(p.cur[5], 0);
    CHECK_EQ(r.voice(0).volume, 21);
    r.fade(76);
    CHECK_EQ(p.cur[0], 0);
    CHECK_EQ(p.cur[5], -21);  // in B's slot past the middle
    CHECK_EQ(r.voice(0).volume, -21);
}

TEST(perf_scene_filters_cross_over) {
    Rig r;
    PerformanceScreen& p = r.p();
    p.src.setValue(3, 1, 60);   // slot 3 Low-pass
    p.src.setValue(4, 1, 80);   // slot 4 High-pass
    r.h.pad(12 + 3);
    r.store(0);
    r.h.pad(12 + 4);            // excludes the low-pass
    r.store(1);
    r.fade(24);
    CHECK_EQ(p.cur[3], 31);     // 60 fading out towards the middle
    CHECK_EQ(p.cur[4], 0);
    CHECK_EQ(r.voice(0).filterType, 0);
    r.fade(50);
    CHECK_EQ(p.cur[3], 0);
    CHECK_EQ(p.cur[4], 0);      // no filter at the middle
    r.fade(74);
    CHECK_EQ(p.cur[3], 0);
    CHECK_EQ(p.cur[4], 38);     // 0 -> 80 over the second half
    CHECK_EQ(r.voice(0).filterType, 1);
    // Same filter type in both scenes: a plain blend.
    p.src.setValue(3, 2, 20);
    r.h.pad(12 * 2 + 3);
    r.store(1);
    r.fade(50);
    CHECK_EQ(p.cur[3], 40);
    CHECK_EQ(p.cur[4], 0);
}

TEST(perf_scene_stepped_fx_switch_at_middle) {
    Rig r;
    PerformanceScreen& p = r.p();
    p.src.setValue(0, 1, 30);   // Volume in both scenes
    p.src.setValue(10, 1, 1);   // Direction "<<<" only in B
    r.h.pad(12 + 0);
    r.store(0);
    r.h.pad(12 + 10);
    r.store(1);
    r.fade(48);
    CHECK_EQ(p.cur[10], 0);
    CHECK(!r.voice(0).reverse);
    r.fade(50);
    CHECK_EQ(p.cur[10], 1);
    CHECK(r.voice(0).reverse);
    r.fade(0);
    CHECK_EQ(p.cur[10], 0);
    CHECK_EQ(p.cur[0], 30);
}

TEST(perf_scene_leaves_other_fx_and_held_pads) {
    Rig r;
    PerformanceScreen& p = r.p();
    p.src.setValue(0, 1, -20);
    p.src.setValue(0, 2, 20);
    p.src.setValue(1, 1, 30);   // Panning: in neither scene
    r.h.pad(12 + 0);
    r.store(0);
    r.h.pad(24 + 0);
    r.store(1);
    r.h.pad(12 + 1);
    CHECK_EQ(p.cur[1], 30);
    r.fade(100);
    CHECK_EQ(p.cur[1], 30);
    CHECK_EQ(r.voice(0).pan, 30);
    // With a pad held the encoder edits that slot, not the fader.
    r.h.push(InputEvent::kPadDown, 12 + 1);
    r.h.enc(5);
    CHECK_EQ(p.fader, 100);
    CHECK_EQ(p.cur[1], 35);
    r.h.push(InputEvent::kPadUp, 12 + 1);
    // Shift+Left with a track key held is still the pattern queue, not a store.
    const uint8_t before = r.a().project.perfScene[0][0];
    r.h.down(btn::F1);
    r.h.down(btn::Shift);
    r.h.tap(btn::Left);
    r.h.up(btn::Shift);
    r.h.up(btn::F1);
    CHECK_EQ(r.a().project.perfScene[0][0], before);
    // Storing with nothing selected clears the scene.
    for (int c = 0; c < 12; ++c) p.src.setRow(c, 0);
    r.store(0);
    for (int c = 0; c < 12; ++c) CHECK_EQ(r.a().project.perfScene[0][c], 0);
}

TEST(perf_scene_cc63_moves_fader) {
    Rig r;
    PerformanceScreen& p = r.p();
    p.src.setValue(0, 1, -100);
    p.src.setValue(0, 2, 100);
    r.h.pad(12 + 0);
    r.store(0);
    r.h.pad(24 + 0);
    r.store(1);
    r.a().handleControlChange(63, 127);
    CHECK_EQ(p.fader, 0);   // latched, applied at the next update
    r.h.run(60);
    CHECK_EQ(p.fader, 100);
    CHECK_EQ(p.cur[0], 100);
    r.a().handleControlChange(63, 64);
    r.h.run(60);
    CHECK_EQ(p.fader, 50);
    r.a().handleControlChange(63, 0);
    r.h.run(60);
    CHECK_EQ(p.cur[0], -100);
    // Only on the Performance screen.
    r.h.tap(btn::Pattern);
    r.a().handleControlChange(63, 127);
    r.h.run(60);
    CHECK_EQ(p.fader, 0);
}

TEST(perf_scenes_file_and_codec) {
    Project p;
    CHECK(!perfScenesUsed(p));
    p.perfScene[0][3] = 2;
    p.perfScene[1][11] = 3;
    CHECK(perfScenesUsed(p));
    uint8_t buf[pscn::kFileSize];
    encodePerfScenes(p, buf);
    CHECK(std::memcmp(buf, "OTPS", 4) == 0);
    Project q;
    CHECK(decodePerfScenes(buf, sizeof(buf), q) == FmtStatus::Ok);
    CHECK(std::memcmp(q.perfScene, p.perfScene, sizeof(p.perfScene)) == 0);
    buf[pscn::kHeaderSize + 3] ^= 1;
    CHECK(decodePerfScenes(buf, sizeof(buf), q) == FmtStatus::BadMagic);
    CHECK(!perfScenesUsed(q));
    // Next to project.mt, only while used; project.mt itself is unchanged.
    FakeStorage fs;
    fs.mkdir("P");
    Project e;
    CHECK(saveProjectFile(fs, "P/project.mt", e) == FmtStatus::Ok);
    CHECK(!fs.exists("P/perf_scenes.bin"));
    const std::vector<uint8_t> plain = fs.files["P/project.mt"];
    CHECK(saveProjectFile(fs, "P/project.mt", p) == FmtStatus::Ok);
    CHECK(fs.exists("P/perf_scenes.bin"));
    CHECK(fs.files["P/project.mt"] == plain);
    CHECK(loadProjectFile(fs, "P/project.mt", q) == FmtStatus::Ok);
    CHECK(std::memcmp(q.perfScene, p.perfScene, sizeof(p.perfScene)) == 0);
    CHECK(saveProjectFile(fs, "P/project.mt", e) == FmtStatus::Ok);
    CHECK(!fs.exists("P/perf_scenes.bin"));
}

TEST(perf_scenes_survive_save_and_open) {
    Rig r;
    App& a = r.a();
    a.project.perfScene[1][2] = 3;
    a.markProjectDirty();
    CHECK(a.saveProjectAs("scenes"));
    CHECK(r.h.fs.exists("Projects/scenes/perf_scenes.bin"));
    CHECK(a.newProject());
    CHECK(!perfScenesUsed(a.project));
    CHECK(a.openProject(kProjectsDir, "scenes"));
    CHECK_EQ(a.project.perfScene[1][2], 3);
}
