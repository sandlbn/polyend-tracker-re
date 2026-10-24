// Sample Editor effects (app/sample_fx.h): executed M4 instructions per
// effect at its default parameters, on a 0.45 s kick and a 10 s mix, under
// QEMU (make -C tests sfx-bench-m4), plus an output checksum (FNV-1a over the
// int16 output) so a faster version can be checked against the old one.
// Built for the host too (make -C tests sfx-bench): microseconds instead.
#include <cmath>
#include <cstdint>
#include <cstdio>
#ifndef OT_BENCH_M4
#include <chrono>
#endif

#include "app/sample_fx.h"

using namespace ot;

namespace {

#ifdef OT_BENCH_M4
// CMSDK APB timer 0 at 25 MHz; with -icount shift=0 one tick = 40 instructions.
#define OT_BIG __attribute__((section(".psram")))
volatile uint32_t* const kTimer = reinterpret_cast<volatile uint32_t*>(0x40000000u);
void clockStart() {
    kTimer[0] = 0;
    kTimer[2] = 0xFFFFFFFFu;
    kTimer[1] = 0xFFFFFFFFu;
    kTimer[0] = 1;
}
double nowUnits() { return double(0xFFFFFFFFu - kTimer[1]) * 40.0; }
constexpr const char* kUnit = "M4 insn";
#else
#define OT_BIG
void clockStart() {}
double nowUnits() {
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count()) /
           1000.0;
}
constexpr const char* kUnit = "us";
#endif

constexpr uint32_t kKick = 19845;     // 0.45 s
constexpr uint32_t kLong = 441000;    // 10 s
constexpr uint32_t kOutCap = 1u << 21; // > every effect's output at these lengths

OT_BIG int16_t gKick[kKick];
OT_BIG int16_t gLong[kLong];
OT_BIG int16_t gOut[kOutCap];

uint32_t rng = 1;
int16_t noise() {
    rng = rng * 1664525u + 1013904223u;
    return int16_t(int32_t(rng >> 16) - 32768);
}

void makeSignals() {
    // Kick: a falling sine sweep with an exponential decay.
    double ph = 0.0;
    for (uint32_t i = 0; i < kKick; ++i) {
        const double t = double(i) / 44100.0;
        ph += 2.0 * 3.14159265358979 * (45.0 + 120.0 * std::exp(-t * 30.0)) / 44100.0;
        gKick[i] = int16_t(28000.0 * std::exp(-t * 7.0) * std::sin(ph));
    }
    // Long: two tones, a chord change every second, and some noise.
    for (uint32_t i = 0; i < kLong; ++i) {
        const double t = double(i) / 44100.0;
        const double f = (int(t) % 2) ? 220.0 : 164.81;
        const double v = 9000.0 * std::sin(2.0 * 3.14159265358979 * f * t) +
                         6000.0 * std::sin(2.0 * 3.14159265358979 * f * 1.5 * t) + 0.08 * double(noise());
        gLong[i] = int16_t(v);
    }
}

uint32_t fnv(const int16_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) {
        h = (h ^ (uint16_t(p[i]) & 0xFFu)) * 16777619u;
        h = (h ^ (uint16_t(p[i]) >> 8)) * 16777619u;
    }
    return h;
}

void run(const char* label, const int16_t* in, uint32_t n) {
    for (int fx = 0; fx < sfx::kEffectCount; ++fx) {
        float p[sfx::kMaxParams];
        sfx::defaults(fx, p);
        // Defaults that leave the sample unchanged get settings that do work.
        if (fx == sfx::kAmplifier) p[0] = 1.5f;
        if (fx == sfx::kChorus) p[1] = 4;
        if (fx == sfx::kEqualizer) p[0] = 6, p[3] = -3, p[6] = 4;
        if (fx == sfx::kLimiter) p[0] = -20;
        const double t0 = nowUnits();
        const uint32_t got = sfx::process(fx, p, in, n, gOut, kOutCap);
        const double t1 = nowUnits();
        std::printf("sfx %-5s %-17s %12.0f %s  out %7lu  hash %08lx\n", label, sfx::effect(fx).name, t1 - t0, kUnit,
                    static_cast<unsigned long>(got), static_cast<unsigned long>(fnv(gOut, got)));
    }
}

}  // namespace

int main() {
    clockStart();
    makeSignals();
    run("kick", gKick, kKick);
    run("10s", gLong, kLong);
    return 0;
}
