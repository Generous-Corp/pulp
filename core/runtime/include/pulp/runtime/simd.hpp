#pragma once

// Portable SIMD operations for Pulp
// Wraps Google Highway for SSE/NEON/AVX with scalar fallback.
// Use for inner-loop DSP, buffer operations, and vectorized math.

#include <cstddef>

namespace pulp::runtime {

/// Number of float lanes on best available SIMD target
size_t simd_float_lanes();

/// Element-wise add: dst[i] = a[i] + b[i]
void simd_add(const float* a, const float* b, float* dst, size_t count);

/// Element-wise multiply: dst[i] = a[i] * b[i]
void simd_mul(const float* a, const float* b, float* dst, size_t count);

/// Fused multiply-add: dst[i] = a[i] * b[i] + c[i]
void simd_fma(const float* a, const float* b, const float* c,
              float* dst, size_t count);

/// Fill dst with a constant value
void simd_set(float value, float* dst, size_t count);

/// Scalar multiply: dst[i] = a[i] * scalar
void simd_scale(const float* a, float scalar, float* dst, size_t count);

/// Sum all elements (horizontal reduce)
float simd_reduce_add(const float* data, size_t count);

/// Maximum element
float simd_reduce_max(const float* data, size_t count);

/// Minimum element
float simd_reduce_min(const float* data, size_t count);

/// Absolute value: dst[i] = |a[i]|
void simd_abs(const float* a, float* dst, size_t count);

/// Clamp: dst[i] = clamp(a[i], lo, hi)
void simd_clamp(const float* a, float lo, float hi, float* dst, size_t count);

}  // namespace pulp::runtime
