// OpenTracker device entry point: board bring-up (spec 01 §0), then the
// application if one is linked, otherwise the hardware selftest.
#include <Arduino.h>
#include "hal/hal.h"
#include "hal/diag.h"
#include "hal/selftest.h"
#include "hal/stack_watch.h"
#include "hal/fw_update.h"
#include "hal/sd_storage.h"
#include "hal/usb_storage.h"

// The application module provides this (e.g. forwarding to App::run()). It is a
// weak reference so the firmware links and runs the selftest without it.
void otAppMain(ot::hal::HalSet& hal) __attribute__((weak));

extern "C" int main(void)
{
    // Stack high-water mark for the telemetry line (hal/stack_watch.h).
    ot::hal::stackPaint();
    // Boot diagnostics over USB CDC (fault capture, stuck watchdog), before
    // anything that could fault (hal/diag.h).
    ot::hal::diagInit();
    // USB mass storage (2.x): answers "medium not present" from enumeration
    // on, so the computer never waits for the app (hal/usb_storage.h).
    ot::hal::usbStorageInit();
    // Escape hatch back to stock firmware that needs only the rail and the SD card:
    // checked before any other driver runs (docs/HARDWARE_TESTING.md path B).
    if (ot::hal::boardEarlyInit().sdMounted) {
        ot::hal::diagStage("restore trigger check");
        ot::hal::checkRestoreTrigger(ot::hal::storage());
        ot::hal::diagLog("restore: no trigger");
    }
    ot::hal::bootPowerGate();
    const ot::hal::BootReport& boot = ot::hal::boardInit();
    (void)boot;   // unused in SELFTEST builds
#if !defined(OT_FORCE_SELFTEST)
    // The application keeps its sample pool, effect memory and patterns in the
    // external SDRAM (src/app_main.cpp), so it only runs when the SDRAM came up;
    // otherwise the selftest reports the failure (screen, status LED, USB serial).
    if (otAppMain && boot.sdramOk) {
        ot::hal::diagStage("application");
        ot::hal::diagFreezeBoot();
        ot::hal::diagWatchdog(false);   // the app main loop has no diagBeat()
        otAppMain(ot::hal::halSet());
    }
#endif
    ot::hal::diagStage("selftest");
    ot::hal::diagWatchdog(true);
    ot::hal::runSelftest();
}
