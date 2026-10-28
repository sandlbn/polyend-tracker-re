// Config help box texts: OpenTracker's own wording (spec 15 §3.2 item 1).
#include "ui/config_help.h"

namespace ot {

namespace {
// spec 15 §2.1 Project Settings: all 10 have help.
const char* const kProjectHelp[] = {
    "Global keeps one set of Performance presets for all projects. Per Project stores the presets inside "
    "each project.",
    "Draws a highlighted line every N steps in the pattern grid, to make the bar structure easier to read.",
    "Limits the notes played from the pads to the chosen scale. Chromatic uses every semitone.",
    "The note the pad grid starts from. Together with the scale it sets the key of the pads.",
    "Sets how the rows of pads continue each other. 12 places the rows end to end; smaller values make each "
    "row repeat notes from the row below.",
    "Filters the samples while they are pitched up, which reduces harsh aliasing tones at a small cost of "
    "brightness.",
    "How hard the master limiter works. Subtle keeps the dynamics natural; Extreme squeezes loud parts much "
    "more.",
    "Pre-fader sends feed delay and reverb before the track volume; Post-fader sends follow the track volume.",
    "Lowers the internal mix level to leave more room before clipping when many loud tracks play together.",
    "Makes the sample editor markers move to the nearest zero crossing, which avoids clicks at loop and slice "
    "points.",
};
// spec 15 §2.2 General: all 11 have help (9 and 10 short in stock); 2.x adds Export theme, CPU meter, Synth voices, Export sample rate and the two SD streaming rows.
const char* const kGeneralHelp[] = {
    "Selects the FM band and channel steps used by the built-in radio in your part of the world.",
    "Sets the backlight level of the screen.",
    "Selects the colour scheme of the user interface. Theme files from the Themes folder of the SD card are "
    "listed after the three built-in themes.",
    "Selects the set of fonts used on screen.",
    // [I] 2.x theme files.
    "Saves the current colour scheme as a text file in the Themes folder of the SD card, to edit on a "
    "computer and choose under Display theme. An existing file is never overwritten.",
    "Sets how bright the pad lights are.",
    "Lights the pads of the notes on the current pattern line: on the cursor track only, on all tracks, or "
    "not at all.",
    "What live recording from the pads stores: notes only, or also the exact timing between steps, the "
    "velocity, or both.",
    "How step numbers are shown: decimal counting from 0 or from 1, or hexadecimal.",
    "Layout of the pattern editor and the direction the cursor moves.",
    "What the line above the pattern shows.",
    "Which side of the line input is used: both channels, or one side copied to both. Applies to the line "
    "input in the mixer and to the sample recorder.",
    // [I] 2.x (spec 2x-02 §2.2): not in stock.
    "Shows the audio processing load next to the play time. Auto: only when it gets high. Always: all the "
    "time. Off: never.",
    "How many synth voices can sound at once over all three synths (1 to 8, default 4). More voices cost more "
    "processing time; when the load gets too high, voices are taken away for a while.",
    "Sample rate of exported songs, patterns, stems and renders. 44.1 kHz converts them, so they play in "
    "time in any DAW. 44117 Hz keeps the Tracker's own rate as the original firmware does; a DAW converts "
    "them on import.",
    "Plays long samples straight from the SD card, so they use only a little memory. Auto streams samples "
    "longer than the threshold (and long ones when the project would not fit); All streams every long "
    "sample. The project reloads when you change it.",
    "How long a sample must be before Auto streams it from the card.",
};
// spec 15 §2.3 MIDI: only CC in (8), CC in channel (9) and Clock sync correction (11).
const char* const kMidiHelp[] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    "Which port accepts MIDI control changes for remote control of mixer and performance parameters.",
    "The MIDI channel listened to for control changes. ALL accepts every channel.",
    nullptr,
    "Shifts the incoming MIDI clock by whole clock pulses (one pulse is a sixth of a step) to line the "
    "sequencer up with other gear.",
};
// [I] 2.x USB (Tracker Mini manual §3 Config options, §13.10).
const char* const kUsbHelp[] = {
    "On shows the SD card on a connected computer as a USB drive, for copying projects and samples. "
    "Playback stops and the project is saved first. Eject the drive on the computer before Exit or Off; "
    "the project then reloads from the card. Always Off after power-on.",
};
// spec 15 §2.5 Firmware: only Firmware update.
const char* const kFirmwareHelp[] = {
    "Installs a firmware file from the Firmware folder of the SD card. The project is saved first.",
    nullptr,
    nullptr,
    nullptr,
};

struct HelpCat {
    const char* const* t;
    int n;
};
#define N(a) int(sizeof(a) / sizeof((a)[0]))
const HelpCat kHelp[] = {
    {kProjectHelp, N(kProjectHelp)}, {kGeneralHelp, N(kGeneralHelp)}, {kMidiHelp, N(kMidiHelp)},
    {kUsbHelp, N(kUsbHelp)},         {nullptr, 0},                      {kFirmwareHelp, N(kFirmwareHelp)},
    {nullptr, 0},
};
constexpr int kHelpCats = N(kHelp);
#undef N
}  // namespace

const char* configHelp(int cat, int opt) {
    if (cat < 0 || cat >= kHelpCats || opt < 0 || opt >= kHelp[cat].n) return nullptr;
    return kHelp[cat].t[opt];
}

int configHelpCount() {
    int n = 0;
    for (int c = 0; c < kHelpCats; ++c)
        for (int i = 0; i < kHelp[c].n; ++i) n += kHelp[c].t[i] != nullptr;
    return n;
}

}  // namespace ot
