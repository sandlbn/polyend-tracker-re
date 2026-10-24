// OT fork Sample Editor effects: Transcode (G.711 / G.726 generation loss,
// bit errors, packet loss) and DCT Crush (spectral crush). See the .cpp.
#pragma once

#include <cstdint>

namespace ot {
namespace sfx {

extern const char* const kTcCodecs[6];
extern const char* const kTcRates[4];
extern const char* const kTcConceal[3];
extern const char* const kDcBlocks[4];

// p: Codec, Rate, Generations, Bit errors (per 10k bits), Loss %, Conceal, Mix %.
uint32_t transcode(const float* p, const int16_t* in, uint32_t n, int16_t* out);
// p: Block, Keep %, Bits, Freeze %, Mix %.
uint32_t dctCrush(const float* p, const int16_t* in, uint32_t n, int16_t* out);

// Radix-2 complex FFT in place (n a power of two; inverse unscaled).
void fftInPlace(float* re, float* im, int n, bool inverse);

// The codec chain alone, at the input rate, no errors (tests).
void codecDirect(int codec, int gens, const int16_t* in, uint32_t n, int16_t* out);
// G.711 round trips (tests).
int16_t ulawRoundTrip(int16_t x);
int16_t alawRoundTrip(int16_t x);

}  // namespace sfx
}  // namespace ot
