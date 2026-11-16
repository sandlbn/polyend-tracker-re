#include "model/instrument.h"

#include "sdram_bss.h"

namespace ot {
const Instrument& defaultInstrument() {
    OT_SDRAM_BSS static const Instrument def;   // built on first use (main loop)
    return def;
}
}  // namespace ot
