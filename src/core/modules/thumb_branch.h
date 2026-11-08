// [I] Thumb-2 long branches, for code that moves at run time
// (modules/module_loader.cpp, modules/hot_rows.cpp). Byte access only, so the
// host tests can patch images built for the device.
#pragma once

#include <cstdint>

namespace ot {
namespace mod {
namespace thumb {

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline void wr16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

// BL (T1) / B.W (T4): both keep S:I1:I2:imm10:imm11 the same way (ARMv7-M
// ARM A7.7.18 / A7.7.12); I1 = !(J1 ^ S), I2 = !(J2 ^ S).
inline bool isLongBranch(const uint8_t* p) {
    const uint16_t a = rd16(p), b = rd16(p + 2);
    return (a & 0xF800u) == 0xF000u && ((b & 0xD000u) == 0xD000u || (b & 0xD000u) == 0x9000u);
}

// The displacement from the branch's address + 4.
inline int32_t branchOffset(const uint8_t* p) {
    const uint32_t a = rd16(p), b = rd16(p + 2);
    const uint32_t s = (a >> 10) & 1u, j1 = (b >> 13) & 1u, j2 = (b >> 11) & 1u;
    const uint32_t i1 = ~(j1 ^ s) & 1u, i2 = ~(j2 ^ s) & 1u;
    uint32_t v = s << 24 | i1 << 23 | i2 << 22 | (a & 0x3FFu) << 12 | (b & 0x7FFu) << 1;
    if (s) v |= 0xFE000000u;   // sign-extend the 25 bits
    return int32_t(v);
}

inline bool inBranchRange(int64_t off) { return off >= -(int64_t(1) << 24) && off < (int64_t(1) << 24) && !(off & 1); }

// False (nothing written) when off is out of range or odd.
inline bool setBranchOffset(uint8_t* p, int64_t off) {
    if (!inBranchRange(off)) return false;
    const uint32_t v = uint32_t(off);
    const uint32_t s = (v >> 24) & 1u, i1 = (v >> 23) & 1u, i2 = (v >> 22) & 1u;
    const uint32_t j1 = (~i1 ^ s) & 1u, j2 = (~i2 ^ s) & 1u;
    wr16(p, uint16_t((rd16(p) & 0xF800u) | s << 10 | ((v >> 12) & 0x3FFu)));
    wr16(p + 2, uint16_t((rd16(p + 2) & 0xD000u) | j1 << 13 | j2 << 11 | ((v >> 1) & 0x7FFu)));
    return true;
}

}  // namespace thumb
}  // namespace mod
}  // namespace ot
