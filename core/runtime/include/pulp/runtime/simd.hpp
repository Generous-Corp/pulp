#pragma once

// Compatibility spelling of the pulp::simd kernels.
//
// The kernels live in the pulp-simd library (<pulp/simd/simd.hpp>), which the
// DSP layer can link without the rest of pulp-runtime. These inline wrappers
// keep existing pulp::runtime::simd_* callers compiling unchanged; new code
// should call pulp::simd directly.

#include <pulp/simd/simd.hpp>

#include <cstddef>

namespace pulp::runtime {

inline std::size_t simd_float_lanes() { return pulp::simd::float_lanes(); }
inline std::size_t simd_double_lanes() { return pulp::simd::double_lanes(); }

inline void simd_add(const float* a, const float* b, float* dst, std::size_t count) {
    pulp::simd::add(a, b, dst, count);
}
inline void simd_add(const double* a, const double* b, double* dst, std::size_t count) {
    pulp::simd::add(a, b, dst, count);
}

inline void simd_mul(const float* a, const float* b, float* dst, std::size_t count) {
    pulp::simd::mul(a, b, dst, count);
}
inline void simd_mul(const double* a, const double* b, double* dst, std::size_t count) {
    pulp::simd::mul(a, b, dst, count);
}

inline void simd_fma(const float* a, const float* b, const float* c, float* dst,
                     std::size_t count) {
    pulp::simd::fma(a, b, c, dst, count);
}
inline void simd_fma(const double* a, const double* b, const double* c, double* dst,
                     std::size_t count) {
    pulp::simd::fma(a, b, c, dst, count);
}

inline void simd_set(float value, float* dst, std::size_t count) {
    pulp::simd::set(value, dst, count);
}
inline void simd_set(double value, double* dst, std::size_t count) {
    pulp::simd::set(value, dst, count);
}

inline void simd_scale(const float* a, float scalar, float* dst, std::size_t count) {
    pulp::simd::scale(a, scalar, dst, count);
}
inline void simd_scale(const double* a, double scalar, double* dst, std::size_t count) {
    pulp::simd::scale(a, scalar, dst, count);
}

inline void simd_add_scaled(const float* a, float scalar, float* dst, std::size_t count) {
    pulp::simd::add_scaled(a, scalar, dst, count);
}
inline void simd_add_scaled(const double* a, double scalar, double* dst, std::size_t count) {
    pulp::simd::add_scaled(a, scalar, dst, count);
}

inline float simd_reduce_add(const float* data, std::size_t count) {
    return pulp::simd::sum(data, count);
}
inline double simd_reduce_add(const double* data, std::size_t count) {
    return pulp::simd::sum(data, count);
}

inline float simd_reduce_max(const float* data, std::size_t count) {
    return pulp::simd::maximum(data, count);
}
inline double simd_reduce_max(const double* data, std::size_t count) {
    return pulp::simd::maximum(data, count);
}

inline float simd_reduce_min(const float* data, std::size_t count) {
    return pulp::simd::minimum(data, count);
}
inline double simd_reduce_min(const double* data, std::size_t count) {
    return pulp::simd::minimum(data, count);
}

inline void simd_abs(const float* a, float* dst, std::size_t count) {
    pulp::simd::abs(a, dst, count);
}
inline void simd_abs(const double* a, double* dst, std::size_t count) {
    pulp::simd::abs(a, dst, count);
}

inline void simd_clamp(const float* a, float lo, float hi, float* dst, std::size_t count) {
    pulp::simd::clamp(a, lo, hi, dst, count);
}
inline void simd_clamp(const double* a, double lo, double hi, double* dst, std::size_t count) {
    pulp::simd::clamp(a, lo, hi, dst, count);
}

}  // namespace pulp::runtime
