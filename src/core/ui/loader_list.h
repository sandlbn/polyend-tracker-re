// Sample Loader SD list storage (spec 06 §5.2, issue #54).
#pragma once

#include <cstddef>
#include <cstdint>

namespace ot {

// The listed names of one Sample Loader directory view. It lives in the
// browsers' shared list buffer (ui/screens.h browserListMem(), .sdram_bss on
// the device), so the FAT long names (up to 255 characters) cost no SRAM /
// stack headroom.
//
// Names are packed into `pool` (each NUL-terminated); entry i is the name at
// pool + off[i]. The pool holds 101 names of 255 characters, the most an
// unsplit folder lists (spec 06 §5.2). A part view of a large folder lists
// up to 100 files plus its subfolders (at most 100 [I]) plus "..", so up to
// kMaxEntries entries; those only run short of pool space when most names
// are very long, and then the names that no longer fit are left out [I].
struct LoaderList {
    static constexpr int kMaxEntries = 201;      // ".." + 100 subfolders + 100 files
    static constexpr int kNameMax = 255;         // FAT long name (FF_MAX_LFN)
    static constexpr size_t kPoolBytes = 101 * (kNameMax + 1);
    char pool[kPoolBytes];
    uint16_t off[kMaxEntries];
    // 0: an ordinary entry; k >= 1: part k of a folder with more than 100
    // files, label "<name> ((k-1)*100+1-k*100)".
    uint16_t part[kMaxEntries];
    bool isDir[kMaxEntries];
    // The import probe of each listed file (App::importProbe), read by the
    // listing so the memory projection and the stream marks open no file.
    struct Probe {
        uint32_t frames;
        uint16_t loop1;     // a .pti's loop start
        uint8_t playMode;   // a .pti's play mode
        uint8_t flags;      // kProbe*; 0: not probed (a folder, an unreadable file)
    };
    static constexpr uint8_t kProbeOk = 1, kProbeStereo = 2, kProbePti = 4;
    Probe probe[kMaxEntries];
};

}  // namespace ot
