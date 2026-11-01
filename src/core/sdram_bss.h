// OT_SDRAM_BSS: large non-audio scratch buffers that live in the SDRAM on the
// device instead of the internal SRAM (docs/OPTIMIZATION_2X.md OP-5: stack
// headroom). The device linker script (src/hal/opentracker.ld) collects them
// in the NOLOAD section `.sdram_bss` near the top of the SDRAM; otAppMain()
// zeroes it after the SDRAM passed its boot test, before any App code runs.
// Host builds (tests, simulator, tools) leave them in ordinary .bss.
//
// Rules for a tagged object:
//  - zero-initialised POD only (no constructor, no initializer): nothing may
//    touch it before otAppMain(), and static initialisation runs earlier;
//  - main-loop data only, never the audio block or an ISR (an uncached-miss
//    SDRAM access costs tens of cycles). [3.x] One exception (docs/PLAN_TECHNO.md
//    E1): the modules' block (ModuleHost::sdram(), modules/module_host.cpp) may
//    be used from the audio block, as contiguous spans moved once per block
//    with audio/ring_io.h (ringIn / ringOut), never one access per sample;
//  - never used by the selftest or the boot code (the SDRAM may be down).
#pragma once

#include <new>

#if defined(OT_TARGET_DEVICE) && OT_TARGET_DEVICE
#define OT_SDRAM_BSS __attribute__((section(".sdram_bss")))
#else
#define OT_SDRAM_BSS
#endif

namespace ot {

// A main-loop object with a constructor (or non-zero initialisers, which a
// NOLOAD section cannot hold) in `.sdram_bss`: raw storage, constructed on
// the first get() -- in the main loop, after otAppMain() zeroed the section.
// Declare it at namespace scope as `OT_SDRAM_BSS SdramLazy<T> x;` (trivial,
// zero-initialised: no static-initialisation code). Same rules as above.
template <class T>
class SdramLazy {
public:
    T& get() {
        if (!made_) {
            new (static_cast<void*>(mem_)) T();
            made_ = true;
        }
        return *reinterpret_cast<T*>(mem_);
    }

private:
    alignas(T) unsigned char mem_[sizeof(T)];
    bool made_;
};

}  // namespace ot
