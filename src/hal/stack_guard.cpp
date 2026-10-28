// OpenTracker -- stack guard (stack_guard.h).
#include "stack_guard.h"

#include <errno.h>
#include <kinetis.h>

#include "diag.h"
#if defined(OT_STACK_GUARD_TEST) && OT_STACK_GUARD_TEST
#include <Arduino.h>
#include "hal_api.h"
#endif

extern "C" uint32_t _ebss, _estack;   // opentracker.ld
extern "C" char* __brkval;            // heap top (third_party/teensy3/mk20dx128.c)

namespace ot {
namespace hal {

namespace {

constexpr uintptr_t kGranule = 32;   // SYSMPU region granularity

// RGDn_WORD2 / RGDAACn: crossbar master 0 (core code bus) and master 1 (core
// system bus; SRAM_U, where the guard lies, is reached through it). Each has
// a user-mode r/w/x field (3 bits) and a supervisor-mode field (2 bits:
// 00 = r/w/x, 11 = same as user); master 0 at bits 0-5, master 1 at 6-11.
constexpr uint32_t kCoreAllAccess = 0x07u | (0x07u << 6);   // user rwx, supervisor rwx
constexpr uint32_t kCoreNoAccess = (0x3u << 3) | (0x3u << 9);  // user none, supervisor = user
constexpr uint32_t kCoreFields = 0xFFFu;

bool gArmed = false;
bool gLockup = false;

}  // namespace

uintptr_t stackGuardStart() {
    const uintptr_t lo = reinterpret_cast<uintptr_t>(&_ebss) + kHeapReserveBytes;
    return (lo + kGranule - 1) & ~(kGranule - 1);
}

uintptr_t stackGuardEnd() { return stackGuardStart() + kStackGuardBytes; }

void stackGuardInit(bool arm) {
    gLockup = (RCM_SRS1 & RCM_SRS1_LOCKUP) != 0;
    if (gLockup)
        diagLog("reset reason: core lockup (a stack overflow into the guard, or a fault while handling a fault)");
    if (!arm) {
        diagLog("stack guard: off (/Firmware/ot_noguard.txt)");
        return;
    }
    const uintptr_t lo = stackGuardStart(), hi = stackGuardEnd();
    if (hi + 16384 > reinterpret_cast<uintptr_t>(&_estack)) {   // the linker asserts 16 KB of headroom
        diagLog("stack guard: no room, not armed");
        return;
    }
    // Every step only adds or moves access, never leaves the core without
    // flash / RAM / peripherals: first the two core regions around the guard
    // (descriptors 1 and 2, unused so far), then the core's access through
    // descriptor 0 (the whole map, all masters) is taken away. The other bus
    // masters (DMA, USB, SDHC) keep descriptor 0.
    MPU_RGD1_WORD0 = 0;                           // 0 .. guard - 1
    MPU_RGD1_WORD1 = uint32_t(lo - 1) | 0x1Fu;
    MPU_RGD1_WORD2 = kCoreAllAccess;
    MPU_RGD1_WORD3 = 1;                           // valid
    MPU_RGD2_WORD0 = uint32_t(hi);                // guard end .. 4 GB
    MPU_RGD2_WORD1 = 0xFFFFFFFFu;
    MPU_RGD2_WORD2 = kCoreAllAccess;
    MPU_RGD2_WORD3 = 1;
    MPU_RGDAAC0 = (MPU_RGDAAC0 & ~kCoreFields) | kCoreNoAccess;
    MPU_CESR = 0xF8000001u;   // enabled; clears the sticky SPERR flags (write 1), so a fault names a fresh port
    __asm__ volatile("dsb\n\tisb" ::: "memory");
    gArmed = true;
    diagLogf("stack guard: %lu B at 0x%08lX..0x%08lX, heap reserve %lu B, stack room %lu B",
             static_cast<unsigned long>(kStackGuardBytes), static_cast<unsigned long>(lo),
             static_cast<unsigned long>(hi - 1), static_cast<unsigned long>(kHeapReserveBytes),
             static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&_estack) - hi));
}

bool stackGuardArmed() { return gArmed; }
bool lastResetWasLockup() { return gLockup; }

#if defined(OT_STACK_GUARD_TEST) && OT_STACK_GUARD_TEST
namespace {
constexpr const char* kTestTrigger = "/Firmware/ot_guardtest.txt";
constexpr const char* kTestDone = "/Firmware/ot_guardtest.done";

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"   // on purpose
__attribute__((noinline)) uint32_t overflow(uint32_t depth) {
    volatile uint8_t frame[512];
    frame[0] = uint8_t(depth);
    frame[511] = uint8_t(depth >> 8);
    return overflow(depth + 1) + frame[0] + frame[511];   // not a tail call
}
#pragma GCC diagnostic pop
}  // namespace

void stackGuardSelfTest(IStorage& fs) {
    if (fs.exists(kTestDone)) {
        fs.remove(kTestDone);
        diagLog(gLockup ? "stack guard test: PASSED (the overflow ended in a core lockup reset)"
                        : "stack guard test: FAILED (the overflow did not end in a lockup reset)");
        return;
    }
    if (!fs.exists(kTestTrigger)) return;
    if (!fs.remove(kTestTrigger)) {   // a read-only card would loop: overflow, reset, overflow ...
        diagLog("stack guard test: skipped (could not delete /Firmware/ot_guardtest.txt)");
        return;
    }
    if (!gArmed) {
        diagLog("stack guard test: skipped (guard off)");
        return;
    }
    // The marker survives the reset (on the card); the log line has 3 s to
    // reach a connected terminal.
    const IStorage::Handle h = fs.open(kTestDone, IStorage::kWrite);
    if (h >= 0) {
        fs.write(h, "1", 1);
        fs.close(h);
    }
    diagLog("stack guard test: overflowing the stack in 3 s");
    delay(3000);
    diagLogf("stack guard test: never here (%lu)", static_cast<unsigned long>(overflow(0)));
}
#endif

}  // namespace hal
}  // namespace ot

// newlib's heap stops below the guard (replaces Teensy's weak _sbrk, which
// stopped STACK_MARGIN below the live stack pointer).
extern "C" void* _sbrk(int incr) {
    char* prev = __brkval;
    if (incr != 0) {
        char* const limit = reinterpret_cast<char*>(ot::hal::stackGuardStart());
        if (incr < 0 ? prev + incr < reinterpret_cast<char*>(&_ebss) : prev + incr > limit) {
            errno = ENOMEM;
            return reinterpret_cast<void*>(-1);
        }
        __brkval = prev + incr;
    }
    return prev;
}
