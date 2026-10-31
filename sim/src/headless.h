// OpenTracker simulator -- headless script runner: virtual time (one app
// pass per simulated ms, a 128-frame audio block every 128/44100 s), the
// script language of sim/README.md "Headless scripts", PNG screenshots.
// Used by the simulator's front end (main.cpp) and by programs that embed it.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "machine.h"

namespace sim {

// Saves the framebuffer (or, with panel, the whole front panel) as PNG.
void saveShot(Machine& m, const std::string& path, bool panel);

// Every Ui::notify() text since the last check (expect-open-ok).
extern std::vector<std::string> gNotices;
void recordNotice(const char* text);

struct Headless {
    Headless(Machine& machine, bool panelShots, const std::string& frames = std::string())
        : m(machine), panel(panelShots), framesDir(frames) {}
    Machine& m;
    bool panel;
    std::string framesDir;   // --frames: each "budget" also saves its frame here (tools/eve_emu)
    uint64_t frameAcc = 0;   // frames * 1000
    // Display budget (2.x performance workstream): the largest per-frame
    // command-FIFO upload since the last "budget" / "budget-reset".
    uint32_t seenFrames = 0, maxUpload = 0, maxDl = 0, budgetFrames = 0;
    // An embedder's own script commands: a command the runner does not know
    // is passed here (the line without its comment, the line number) and so
    // is an expect-notice with no match in gNotices. Returns 1 = done,
    // 0 = failed (message printed), -1 = unknown too. Unset in the simulator.
    std::function<int(const std::string& line, int lineNo)> extension;

    void sampleFrame();
    // budget NAME MAXWORDS: fail when a frame since the last reset uploaded
    // more than MAXWORDS display-list words (or the RAM_DL total passed 2000).
    bool budget(const std::string& name, long maxWords, int line);
    // --frames: the current screen recorded as display commands (DIR/NAME.otf).
    bool captureFrame(const std::string& path, int line);
    bool saveFrame(const std::string& name, int line);
    // fragcheck NAME: the current screen drawn inline and through cached
    // fragments must give identical pixels.
    bool fragCheck(const std::string& name, int line);
    // romcheck NAME: ROM-font runs as CMD_TEXT and as VERTEX2II glyphs (OP-6)
    // must give identical pixels.
    bool romCheck(const std::string& name, int line);
    void advance(long ms);
    bool button(const std::string& name, bool down, int line);
    bool run(const std::string& path);
};

}  // namespace sim
