// [I] Modules from the card (hal/module_pool.h, fork issue #21).
#include "module_pool.h"

#include <kinetis.h>

#include "modules/module.h"
#include "otm_core.h"   // generated: kOtmLayoutHash, kOtmAbiFlags, kOtmExportCount

extern "C" const uint32_t otmExports[];   // generated (tools/make_otm.py exports-cpp)

namespace ot {
namespace hal {

namespace {

// SRAM_L (code bus, the fast path for code), right after .data; the linker
// script (`.otm_pool`) keeps it below 0x20000000. Not zeroed at boot: the
// loader writes or zeroes every byte a module uses.
constexpr uint32_t kPoolBytes = 12 * 1024;
alignas(8) __attribute__((section(".otm_pool"))) uint8_t gPool[kPoolBytes];

mod::OtmPool gOtmPool;   // constant-initialised; filled by modulePool()

// LMEM PCCCR (K66 RM "Local memory controller"): GO, PUSHW1, INVW1, PUSHW0,
// INVW0. The code cache covers SRAM_L: push and invalidate it, so the core
// fetches the new code, not lines cached from what was there before.
constexpr uint32_t kLmemGo = 1u << 31, kLmemClearAll = (1u << 27) | (1u << 26) | (1u << 25) | (1u << 24);

void syncCode(const void*, uint32_t) {
    LMEM_PCCCR |= kLmemGo | kLmemClearAll;
    while (LMEM_PCCCR & kLmemGo) {}
    __asm__ volatile("dsb\n\tisb" ::: "memory");
}

mod::IModule& enter(uint32_t entry) {
    return reinterpret_cast<mod::ModuleFactory>(static_cast<uintptr_t>(entry))();
}

}  // namespace

const mod::OtmPool* modulePool() {
    if (!gOtmPool.mem) {
        gOtmPool.mem = gPool;
        gOtmPool.base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(gPool));
        gOtmPool.cap = kPoolBytes;
        gOtmPool.core = mod::OtmCore{mod::kModuleApi, kOtmLayoutHash, kOtmAbiFlags, otmExports, kOtmExportCount};
        gOtmPool.sync = &syncCode;
        gOtmPool.enter = &enter;
    }
    return &gOtmPool;
}

}  // namespace hal
}  // namespace ot
