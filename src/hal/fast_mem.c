// Word-wise memcpy / memmove / memset for the device (OPTIMIZATION_2X item 2,
// OP-1). newlib-nano links byte-loop versions (libc_a-memcpy-stub.o etc.):
// one byte per iteration, and to SDRAM one bus access per byte. These
// definitions are linked from this object, so the library members are not
// pulled in. Same contracts as the C standard functions.
//
//   - mutually word-aligned pointers: 32-byte LDM/STM blocks, then words;
//   - otherwise (memcpy / forward memmove): aligned word stores fed by
//     unaligned word loads (the Cortex-M4 supports them for normal memory);
//   - memmove copies backwards when the destination overlaps the source
//     from above.
//
// The loops carry an empty asm statement so GCC cannot turn them back into
// calls to the functions being defined. Device only (src/hal); the M4
// benchmark (tests/bench_m4) links it too.
#include <stddef.h>
#include <stdint.h>

// memset runs from SRAM_L (OT_FASTRUN, core/fastrun.h); memcpy cannot: the
// startup code copies .data (and so .fastrun) with it.
#include "../core/fastrun.h"

#ifdef __cplusplus
extern "C" {
#endif

void* memcpy(void* __restrict dst, const void* __restrict src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);

#ifdef __cplusplus
}
#endif

#define OT_MEM_BARRIER() __asm__ volatile("")
// clang (host build in tests/fast_mem_test.cpp) has no optimize attribute; the
// asm barrier above keeps the loops as written there too.
#if defined(__clang__)
#define OT_MEM_FN __attribute__((used))
#else
#define OT_MEM_FN __attribute__((used, optimize("no-tree-loop-distribute-patterns")))
#endif

typedef uint32_t __attribute__((may_alias)) ot_w32;
typedef struct __attribute__((packed, may_alias)) {
    uint32_t v;
} ot_uw32;

// Forward copy; safe for overlapping buffers with dst <= src (every block
// is read before the bytes it overwrites).
static inline __attribute__((always_inline)) void copyForward(uint8_t* d, const uint8_t* s, size_t n) {
    if (n >= 8) {
        while ((uintptr_t)d & 3u) {   // align the destination
            OT_MEM_BARRIER();
            *d++ = *s++;
            --n;
        }
        if (!((uintptr_t)s & 3u)) {
            ot_w32* dw = (ot_w32*)d;
            const ot_w32* sw = (const ot_w32*)s;
#if defined(__arm__) && defined(__thumb2__)
            while (n >= 32) {
                register uint32_t a __asm__("r4");
                register uint32_t b __asm__("r5");
                register uint32_t c __asm__("r6");
                register uint32_t e __asm__("r7");
                register uint32_t f __asm__("r8");
                register uint32_t g __asm__("r9");
                register uint32_t h __asm__("r10");
                register uint32_t k __asm__("r11");
                __asm__ volatile(
                    "ldmia %[s]!, {%[a], %[b], %[c], %[e], %[f], %[g], %[h], %[k]}\n\t"
                    "stmia %[d]!, {%[a], %[b], %[c], %[e], %[f], %[g], %[h], %[k]}"
                    : [s] "+r"(sw), [d] "+r"(dw), [a] "=r"(a), [b] "=r"(b), [c] "=r"(c), [e] "=r"(e),
                      [f] "=r"(f), [g] "=r"(g), [h] "=r"(h), [k] "=r"(k)
                    :
                    : "memory");
                n -= 32;
            }
#endif
            while (n >= 4) {
                OT_MEM_BARRIER();
                *dw++ = *sw++;
                n -= 4;
            }
            d = (uint8_t*)dw;
            s = (const uint8_t*)sw;
        } else {
            ot_w32* dw = (ot_w32*)d;
            while (n >= 16) {
                OT_MEM_BARRIER();
                const uint32_t w0 = ((const ot_uw32*)s)[0].v, w1 = ((const ot_uw32*)s)[1].v;
                const uint32_t w2 = ((const ot_uw32*)s)[2].v, w3 = ((const ot_uw32*)s)[3].v;
                dw[0] = w0;
                dw[1] = w1;
                dw[2] = w2;
                dw[3] = w3;
                dw += 4;
                s += 16;
                n -= 16;
            }
            while (n >= 4) {
                OT_MEM_BARRIER();
                *dw++ = ((const ot_uw32*)s)->v;
                s += 4;
                n -= 4;
            }
            d = (uint8_t*)dw;
        }
    }
    while (n) {
        OT_MEM_BARRIER();
        *d++ = *s++;
        --n;
    }
}

OT_MEM_FN void* memcpy(void* __restrict dst, const void* __restrict src, size_t n) {
    copyForward((uint8_t*)dst, (const uint8_t*)src, n);
    return dst;
}

OT_MEM_FN void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) {
        copyForward(d, s, n);
        return dst;
    }
    // Overlap with dst above src: copy backwards. As forward: aligned word
    // stores, fed by aligned or (mutually misaligned pointers) unaligned word
    // loads; each block is read before the bytes it overwrites. A byte loop
    // here made a misaligned move one SDRAM access per byte (shifting the
    // sample bank up by an odd number of int16 samples took seconds).
    d += n;
    s += n;
    if (n >= 8) {
        while ((uintptr_t)d & 3u) {
            OT_MEM_BARRIER();
            *--d = *--s;
            --n;
        }
        ot_w32* dw = (ot_w32*)d;
        if (!((uintptr_t)s & 3u)) {
            const ot_w32* sw = (const ot_w32*)s;
            while (n >= 16) {
                OT_MEM_BARRIER();
                const uint32_t w3 = sw[-1], w2 = sw[-2], w1 = sw[-3], w0 = sw[-4];
                dw[-1] = w3;
                dw[-2] = w2;
                dw[-3] = w1;
                dw[-4] = w0;
                dw -= 4;
                sw -= 4;
                n -= 16;
            }
            while (n >= 4) {
                OT_MEM_BARRIER();
                *--dw = *--sw;
                n -= 4;
            }
            s = (const uint8_t*)sw;
        } else {
            while (n >= 16) {
                OT_MEM_BARRIER();
                const uint32_t w3 = ((const ot_uw32*)(s - 4))->v, w2 = ((const ot_uw32*)(s - 8))->v;
                const uint32_t w1 = ((const ot_uw32*)(s - 12))->v, w0 = ((const ot_uw32*)(s - 16))->v;
                dw[-1] = w3;
                dw[-2] = w2;
                dw[-3] = w1;
                dw[-4] = w0;
                dw -= 4;
                s -= 16;
                n -= 16;
            }
            while (n >= 4) {
                OT_MEM_BARRIER();
                *--dw = ((const ot_uw32*)(s - 4))->v;
                s -= 4;
                n -= 4;
            }
        }
        d = (uint8_t*)dw;
    }
    while (n) {
        OT_MEM_BARRIER();
        *--d = *--s;
        --n;
    }
    return dst;
}

OT_MEM_FN OT_FASTRUN_FN void* memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t b = (uint8_t)c;
    if (n >= 8) {
        while ((uintptr_t)d & 3u) {
            OT_MEM_BARRIER();
            *d++ = b;
            --n;
        }
        const uint32_t w = 0x01010101u * b;
        ot_w32* dw = (ot_w32*)d;
#if defined(__arm__) && defined(__thumb2__)
        {
            register uint32_t r4 __asm__("r4") = w;
            register uint32_t r5 __asm__("r5") = w;
            register uint32_t r6 __asm__("r6") = w;
            register uint32_t r7 __asm__("r7") = w;
            while (n >= 32) {
                __asm__ volatile(
                    "stmia %[d]!, {%[a], %[b], %[c], %[e]}\n\t"
                    "stmia %[d]!, {%[a], %[b], %[c], %[e]}"
                    : [d] "+r"(dw)
                    : [a] "r"(r4), [b] "r"(r5), [c] "r"(r6), [e] "r"(r7)
                    : "memory");
                n -= 32;
            }
        }
#endif
        while (n >= 4) {
            OT_MEM_BARRIER();
            *dw++ = w;
            n -= 4;
        }
        d = (uint8_t*)dw;
    }
    while (n) {
        OT_MEM_BARRIER();
        *d++ = b;
        --n;
    }
    return dst;
}
