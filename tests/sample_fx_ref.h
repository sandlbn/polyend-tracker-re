// The Sample Editor effects before the speed work (sample_fx_ref.cpp): the
// same interface as app/sample_fx.h, in namespace sfx_ref. Test-only.
#pragma once

#include <cstdint>

#include "app/sample_fx.h"

namespace ot {
namespace sfx_ref {

using sfx::EffectDesc;
using sfx::ParamDesc;
const EffectDesc& effect(int i);
void defaults(int fx, float* params);
float clampParam(int fx, int i, float v);
void formatParam(int fx, int i, float v, char* out, int cap);
uint32_t outputFrames(int fx, const float* params, uint32_t n);
uint32_t process(int fx, const float* params, const int16_t* in, uint32_t n, int16_t* out, uint32_t cap);

}  // namespace sfx_ref
}  // namespace ot
