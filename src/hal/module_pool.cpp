// [I] Modules from the card (hal/module_pool.h).
#include "module_pool.h"

#include <kinetis.h>

#include "modules/module.h"
#include "stack_guard.h"
#include "otm_core.h"   // generated: kOtmLayoutHash, kOtmAbiFlags, kOtmExportCount

extern "C" const uint32_t otmExports[];   // generated (tools/make_otm.py exports-cpp)
extern "C" uint8_t __otm_fast_start[], __otm_fast_end[];   // opentracker.ld (OT_POOL_TEST / OT_HOT_POOL images)
#if defined(OT_HOT_POOL) && OT_HOT_POOL
extern "C" const ot::mod::HotRows otHotRows;   // generated (tools/hot_pool.py table)
#endif

namespace ot {
namespace hal {

namespace {

// The pool borrows the bottom of the stack's room in SRAM_U (stack_guard.h:
// the guard moves up over the part in use). The most it may take ("Card code
// memory"); less when the stack floor needs it. Not zeroed: the loader writes
// or zeroes every byte a module uses.
#if defined(OT_POOL_TEST) && OT_POOL_TEST
constexpr uint32_t kPoolCapBytes = 48 * 1024;   // [I] measurement images (profiles/pooltest.toml)
#else
constexpr uint32_t kPoolCapBytes = 32 * 1024;
#endif

mod::OtmPool gOtmPool = {};   // constant-initialised (= {}: the fast fields have initialisers); filled by modulePool()

// LMEM PCCCR (K66 RM "Local memory controller"): GO, PUSHW1, INVW1, PUSHW0,
// INVW0. The code cache covers the code bus (SRAM_L, the fast block), not
// SRAM_U where the pool is; pushed and invalidated either way, then the
// barriers make the core fetch the new code.
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
        const uintptr_t base = modulePoolBase();
        const uint32_t room = modulePoolMaxBytes();
        gOtmPool.mem = reinterpret_cast<uint8_t*>(base);
        gOtmPool.base = static_cast<uint32_t>(base);
        gOtmPool.cap = room < kPoolCapBytes ? room : kPoolCapBytes;
        gOtmPool.core = mod::OtmCore{mod::kModuleApi, kOtmLayoutHash, kOtmAbiFlags, otmExports, kOtmExportCount};
        gOtmPool.sync = &syncCode;
        gOtmPool.enter = &enter;
        gOtmPool.resize = &setModulePoolBytes;
        const uintptr_t fs = reinterpret_cast<uintptr_t>(__otm_fast_start);
        const uintptr_t fe = reinterpret_cast<uintptr_t>(__otm_fast_end);
        if (fe > fs) {   // modules' fast parts in SRAM_L
            gOtmPool.fastMem = __otm_fast_start;
            gOtmPool.fastBase = static_cast<uint32_t>(fs);
            gOtmPool.fastCap = static_cast<uint32_t>(fe - fs);
        }
#if defined(OT_HOT_POOL) && OT_HOT_POOL
        gOtmPool.hotRows = &otHotRows;   // [I] the built-in synths' rows share the block
#endif
    }
    return &gOtmPool;
}

}  // namespace hal
}  // namespace ot
