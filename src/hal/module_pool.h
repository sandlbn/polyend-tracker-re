// [I] Modules from the card: the block at the stack's
// bottom they run from, this core's export table and versions (modules/module_files.h).
#pragma once

#include "modules/module_files.h"

namespace ot {
namespace hal {

// The pool for HalRefs::modulePool (filled on the first call).
const mod::OtmPool* modulePool();

}  // namespace hal
}  // namespace ot
