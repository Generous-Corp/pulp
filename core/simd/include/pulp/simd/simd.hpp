#pragma once

// pulp::simd — portable block kernels for DSP.
//
// One API for vectorized buffer arithmetic and reductions. The kernels are
// compiled once in the pulp-simd library and dispatch at run time to the best
// instruction set the CPU offers (Google Highway: SSE2/SSE4/AVX2/AVX-512 on
// x86, NEON/SVE on arm), so the baseline ISA of the calling code does not
// limit them. pulp::signal links this target, so DSP headers may call it.
//
// Each call pays one dynamic-dispatch hop, so reserve these for work large
// enough to amortize it (reductions, filters, long buffers). A short
// elementwise loop written inline usually auto-vectorizes and wins.
//
// Aliasing: dst may equal a source exactly where noted; partial overlap is
// never supported. None of the kernels allocate, lock, or throw, so they are
// safe on the audio thread.

#include <cstddef>

namespace pulp::simd {

/// Number of float lanes on the dispatched SIMD target.
std::size_t float_lanes();

/// Number of double lanes on the dispatched SIMD target.
std::size_t double_lanes();

/// Element-wise add: dst[i] = a[i] + b[i].
/// Exact aliasing where dst == a or dst == b is supported.
void add(const float* a, const float* b, float* dst, std::size_t count);
void add(const double* a, const double* b, double* dst, std::size_t count);

/// Element-wise multiply: dst[i] = a[i] * b[i].
void mul(const float* a, const float* b, float* dst, std::size_t count);
void mul(const double* a, const double* b, double* dst, std::size_t count);

/// Fused multiply-add: dst[i] = a[i] * b[i] + c[i].
void fma(const float* a, const float* b, const float* c, float* dst, std::size_t count);
void fma(const double* a, const double* b, const double* c, double* dst, std::size_t count);

/// Fill dst with a constant value.
void set(float value, float* dst, std::size_t count);
void set(double value, double* dst, std::size_t count);

/// Scalar multiply: dst[i] = a[i] * scalar. Exact aliasing a == dst is supported.
void scale(const float* a, float scalar, float* dst, std::size_t count);
void scale(const double* a, double scalar, double* dst, std::size_t count);

/// Accumulate scaled source into destination: dst[i] += a[i] * scalar.
/// Exact aliasing where a == dst is supported.
void add_scaled(const float* a, float scalar, float* dst, std::size_t count);
void add_scaled(const double* a, double scalar, double* dst, std::size_t count);

/// Sum of all elements. The summation order is the backend's, not
/// left-to-right, so the result may differ from a sequential sum by rounding.
float sum(const float* data, std::size_t count);
double sum(const double* data, std::size_t count);

/// Largest element (0 for an empty range).
float maximum(const float* data, std::size_t count);
double maximum(const double* data, std::size_t count);

/// Smallest element (0 for an empty range).
float minimum(const float* data, std::size_t count);
double minimum(const double* data, std::size_t count);

/// Absolute value: dst[i] = |a[i]|.
void abs(const float* a, float* dst, std::size_t count);
void abs(const double* a, double* dst, std::size_t count);

/// Clamp: dst[i] = clamp(a[i], lo, hi).
void clamp(const float* a, float lo, float hi, float* dst, std::size_t count);
void clamp(const double* a, double lo, double hi, double* dst, std::size_t count);

}  // namespace pulp::simd
