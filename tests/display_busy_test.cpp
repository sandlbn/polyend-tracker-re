// 2.0.0 field bug (first hardware test): progress popups stayed on screen
// after their job ("Loading projects list" on the File screen, "Opening last
// project" at boot). The driver left each frame's last SPI2 DMA segment and
// its REG_CMD_WRITE pending until the next display access, which only comes
// when the UI is dirty again, so the frame that closed the popup never ran.
//
// These tests drive the whole App against RecordingDisplay's realistic busy
// panel (ui_fakes.h: upload time, then shown at the next 54 Hz vsync,
// frameReady() false meanwhile) in 1 ms loop passes and check what the panel
// actually shows: every dirty state must reach it within ~one frame period.
// lazyCommit reproduces the 2.0.0 driver behaviour.
#include <cstring>
#include <string>

#include "test.h"
#include "ui_fakes.h"

#include "formats/file_io.h"
#include "formats/project_mt.h"
#include "ui/screens.h"

using namespace ot;

namespace {

void addProject(FakeStorage& fs, const std::string& d) {
    for (size_t p = d.find('/'); p != std::string::npos; p = d.find('/', p + 1)) fs.dirs.insert(d.substr(0, p));
    fs.dirs.insert(d);
    Project p;
    p.setDefault();
    CHECK(saveProjectFile(fs, (d + "/project.mt").c_str(), p) == FmtStatus::Ok);
}

void stockCard(FakeStorage& fs) {
    fs.dirs.insert("Projects");
    addProject(fs, "Projects/Ask The Stars");
    // Patterns, so opening it takes many job units.
    fs.dirs.insert("Projects/Ask The Stars/patterns");
    Pattern pat;
    for (int n = 1; n <= 40; ++n) {
        char path[96];
        CHECK(patternFilePath(path, sizeof(path), "Projects/Ask The Stars", n));
        CHECK(savePatternFile(fs, path, pat) == FmtStatus::Ok);
    }
    addProject(fs, "Projects/B1t Crunch3r - Dimension X");
    addProject(fs, "Projects/Defrag - We are Foam (Qebo Rmx)");
    for (int i = 0; i < 20; ++i) addProject(fs, "Projects/Song " + std::to_string(i));
}

void busyPanel(Harness& h, bool lazy) {
    h.disp.busy.on = true;
    h.disp.busy.lazyCommit = lazy;
    h.disp.busy.clockMs = &h.clock.ms;
}

// One main-loop pass per millisecond.
void step(Harness& h, uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
        h.clock.ms += 1;
        h.app->loop();
    }
}

// Milliseconds of 1 ms loop passes until the panel shows the latest frame
// and `text` is not on it (limit + 1 if it stays).
uint32_t msUntilGone(Harness& h, const char* text, uint32_t limit) {
    for (uint32_t t = 0; t <= limit; ++t) {
        const bool any = !h.disp.visible().empty();   // ticks the panel model
        if (any && !h.disp.busy.pending && !h.disp.visibleHasText(text)) return t;
        step(h, 1);
    }
    return limit + 1;
}

void press(Harness& h, uint8_t id) {
    InputEvent e;
    e.type = InputEvent::kButtonDown;
    e.id = id;
    e.timeMs = h.clock.ms;
    h.input.q.push_back(e);
    e.type = InputEvent::kButtonUp;
    h.input.q.push_back(e);
}

// File -> Open (F2): the browser lists the card behind "Loading projects list".
uint32_t fileOpenPopupMs(bool lazy) {
    Harness h(false);
    stockCard(h.fs);
    busyPanel(h, lazy);
    h.app->boot();
    step(h, 200);
    h.tap(btn::File);
    step(h, 200);
    press(h, btn::F2);
    step(h, 1);   // the handler lists the card (popup frames drawn synchronously)
    CHECK(!h.app->ui.progressOpen());
    CHECK_EQ(static_cast<FileScreen*>(h.app->ui.screen(kModFile))->state, 1);
    const uint32_t ms = msUntilGone(h, "Loading projects list", 500);
    if (ms <= 500) CHECK(h.disp.visibleHasText("Ask The Stars"));
    return ms;
}

}  // namespace

TEST(file_list_popup_closes_within_a_frame) {
    const uint32_t ms = fileOpenPopupMs(false);
    std::printf("  file list popup gone after %u ms\n", unsigned(ms));
    CHECK(ms <= 50);
}

TEST(lazy_commit_driver_keeps_the_popup) {
    // The 2.0.0 driver (frame tail committed only by the next access): the
    // popup frame stays on the panel although the UI is clean.
    CHECK(fileOpenPopupMs(true) > 500);
}

TEST(boot_autoload_popup_closes_within_a_frame) {
    FakeStorage card;
    {
        Harness first;   // creates the workspace on its card
        first.app->project.setDefault();
        CHECK(saveProjectFile(first.fs, "Workspace/project.mt", first.app->project) == FmtStatus::Ok);
        card = first.fs;
    }
    stockCard(card);
    Harness h(false);
    h.fs = card;
    busyPanel(h, false);
    CHECK(h.app->boot());
    CHECK(h.app->ui.active() == kModPattern);
    CHECK(!h.app->ui.progressOpen());
    // The last splash frame is still on its way to the panel.
    CHECK(h.disp.busy.pending && h.disp.busy.frame.size() > 0);
    const uint32_t ms = msUntilGone(h, "Opening last project", 500);
    std::printf("  boot splash gone after %u ms\n", unsigned(ms));
    CHECK(ms <= 50);
    // And it stays a live screen: a later change shows within a frame too.
    step(h, 300);
    const uint32_t shown0 = h.disp.busy.shownFrames;
    h.app->ui.dirty();
    step(h, 40);
    h.disp.visible();   // ticks the panel model
    CHECK(h.disp.busy.shownFrames > shown0);
    CHECK(!h.disp.busy.pending);
}

TEST(open_project_popup_closes_within_a_frame) {
    Harness h(false);
    stockCard(h.fs);
    busyPanel(h, false);
    h.app->boot();
    step(h, 200);
    h.tap(btn::File);
    h.tap(btn::F2);
    step(h, 100);
    CHECK(!h.disp.visibleHasText("Loading projects list"));
    h.fs.readLatencyClockMs = &h.clock.ms;   // the open job spans many passes
    h.fs.readLatencyMs = 15;
    press(h, btn::F8);   // Open the first project
    bool sawPopup = false;
    uint32_t t = 0;
    for (; t < 5000 && (t < 2 || h.app->jobBusy() || h.app->ui.progressOpen()); ++t) {
        step(h, 1);
        sawPopup = sawPopup || h.disp.visibleHasText("Opening project");
    }
    CHECK(t < 5000);
    CHECK(sawPopup);
    CHECK(h.app->ui.active() == kModPattern);
    const uint32_t ms = msUntilGone(h, "Opening project", 500);
    std::printf("  open popup gone after %u ms\n", unsigned(ms));
    CHECK(ms <= 50);
}

TEST(deferral_polls_are_throttled) {
    // While the panel is busy the UI polls frameReady() at most once per
    // millisecond (device log: thousands of deferrals per 5 s window).
    Harness h(false);
    stockCard(h.fs);
    busyPanel(h, false);
    h.app->boot();
    step(h, 200);
    Ui& ui = h.app->ui;
    const uint32_t d0 = ui.framesDeferred(), f0 = ui.frames();
    for (int ms = 0; ms < 1000; ++ms) {
        // Continuous changes (e.g. playback), several loop passes per
        // millisecond like the device.
        for (int p = 0; p < 10; ++p) {
            ui.dirty();
            h.app->loop();
        }
        h.clock.ms += 1;
    }
    const uint32_t frames = ui.frames() - f0, deferred = ui.framesDeferred() - d0;
    std::printf("  %u frames, %u deferred in 1000 ms\n", unsigned(frames), unsigned(deferred));
    CHECK(frames >= 45);
    CHECK(deferred <= 1000);
}
