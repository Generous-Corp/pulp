#pragma once

// Scalar reference kernels for pulp::simd, shared by the compiled scalar
// backend (src/scalar.cpp) and the inline fallback that <pulp/simd/simd.hpp>
// uses when a translation unit is built without pulp-simd's usage
// requirements (a hand-listed source build such as a wasm or Rack plugin).
//
// Plain sequential loops in the element type. These define the numerical
// reference the other backends are compared against: every reduction
// accumulates left to right, and nothing is reordered on purpose. The
// optimiser may still vectorize the elementwise loops (that does not change
// their results) and, where the target contracts a*b+c into a fused
// multiply-add by default, fuse them; the reference is therefore exact to
// rounding of its own formula, not bit-identical across architectures.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::simd::detail {

template <typename T> inline void add_impl(const T* a, const T* b, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = a[i] + b[i];
}

template <typename T> inline void mul_impl(const T* a, const T* b, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = a[i] * b[i];
}

template <typename T>
inline void fma_impl(const T* a, const T* b, const T* c, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = a[i] * b[i] + c[i];
}

template <typename T> inline void set_impl(T value, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = value;
}

template <typename T> inline void scale_impl(const T* a, T s, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = a[i] * s;
}

template <typename T> inline void add_scaled_impl(const T* a, T s, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] += a[i] * s;
}

template <typename T> inline T sum_impl(const T* x, std::size_t n) noexcept {
    T acc = T(0);
    for (std::size_t i = 0; i < n; ++i)
        acc += x[i];
    return acc;
}

template <typename T> inline T maximum_impl(const T* x, std::size_t n) noexcept {
    if (n == 0)
        return T(0);
    T m = x[0];
    for (std::size_t i = 1; i < n; ++i)
        m = std::max(m, x[i]);
    return m;
}

template <typename T> inline T minimum_impl(const T* x, std::size_t n) noexcept {
    if (n == 0)
        return T(0);
    T m = x[0];
    for (std::size_t i = 1; i < n; ++i)
        m = std::min(m, x[i]);
    return m;
}

template <typename T> inline void abs_impl(const T* a, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = std::abs(a[i]);
}

template <typename T>
inline void clamp_impl(const T* a, T lo, T hi, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = std::clamp(a[i], lo, hi);
}

template <typename T> inline T dot_impl(const T* a, const T* b, std::size_t n) noexcept {
    T acc = T(0);
    for (std::size_t i = 0; i < n; ++i)
        acc += a[i] * b[i];
    return acc;
}

template <typename T> inline T sum_squares_impl(const T* x, std::size_t n) noexcept {
    T acc = T(0);
    for (std::size_t i = 0; i < n; ++i)
        acc += x[i] * x[i];
    return acc;
}

template <typename T> inline T max_abs_impl(const T* x, std::size_t n) noexcept {
    T m = T(0);
    for (std::size_t i = 0; i < n; ++i)
        m = std::max(m, std::abs(x[i]));
    return m;
}

template <typename T>
inline void ramp_mul_impl(const T* x, T start, T step, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = x[i] * (start + T(i) * step);
}

template <typename T>
inline void correlate_impl(const T* x, const T* h, T* y, std::size_t n_out,
                           std::size_t taps) noexcept {
    for (std::size_t i = 0; i < n_out; ++i)
        y[i] = dot_impl(x + i, h, taps);
}

template <typename T>
inline void decimate2_impl(const T* x, const T* h, T* y, std::size_t n_out,
                           std::size_t taps) noexcept {
    for (std::size_t i = 0; i < n_out; ++i)
        y[i] = dot_impl(x + 2 * i, h, taps);
}

template <typename T> inline void sub_impl(const T* a, const T* b, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = a[i] - b[i];
}

template <typename T> inline void div_impl(const T* a, const T* b, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = a[i] / b[i];
}

template <typename T> inline void negate_impl(const T* a, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = -a[i];
}

template <typename T> inline T mean_impl(const T* x, std::size_t n) noexcept {
    return n == 0 ? T(0) : sum_impl(x, n) / T(n);
}

template <typename T> inline T rms_impl(const T* x, std::size_t n) noexcept {
    return n == 0 ? T(0) : std::sqrt(sum_squares_impl(x, n) / T(n));
}

/// Lowest index whose element equals `value`; 0 when none does (only possible
/// with NaN input, where the extremum is unspecified).
template <typename T>
inline std::size_t first_index_of(const T* x, std::size_t n, T value) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        if (x[i] == value)
            return i;
    return 0;
}

template <typename T>
inline T find_maximum_impl(const T* x, std::size_t n, std::size_t* index) noexcept {
    T best = n == 0 ? T(0) : x[0];
    std::size_t at = 0;
    for (std::size_t i = 1; i < n; ++i) {
        if (x[i] > best) {
            best = x[i];
            at = i;
        }
    }
    *index = at;
    return best;
}

template <typename T>
inline T find_minimum_impl(const T* x, std::size_t n, std::size_t* index) noexcept {
    T best = n == 0 ? T(0) : x[0];
    std::size_t at = 0;
    for (std::size_t i = 1; i < n; ++i) {
        if (x[i] < best) {
            best = x[i];
            at = i;
        }
    }
    *index = at;
    return best;
}

template <typename T>
inline void replace_below_impl(const T* a, T threshold, T value, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = a[i] < threshold ? value : a[i];
}

template <typename T>
inline void replace_above_impl(const T* a, T threshold, T value, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = a[i] > threshold ? value : a[i];
}

template <typename T, typename I>
inline void from_int_impl(const I* src, T scale, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = static_cast<T>(src[i]) * scale;
}

/// Largest value of T that converts to I without overflow: the float just
/// below 2^31 for float -> int32, the integer limit otherwise.
template <typename T, typename I> inline constexpr T int_upper_bound() noexcept {
    if constexpr (sizeof(I) == 4 && sizeof(T) == 4)
        return T(2147483520.0);
    else
        return static_cast<T>(std::numeric_limits<I>::max());
}

template <typename T, typename I>
inline void to_int_impl(const T* src, T scale, I* dst, std::size_t n) noexcept {
    constexpr T lo = static_cast<T>(std::numeric_limits<I>::min());
    constexpr T hi = int_upper_bound<T, I>();
    for (std::size_t i = 0; i < n; ++i) {
        T v = src[i] * scale;
        if (v != v) {
            dst[i] = 0;
            continue;
        }
        v = v < lo ? lo : (v > hi ? hi : v);
        dst[i] = static_cast<I>(std::nearbyint(v));
    }
}

template <typename T>
inline void interleave2_impl(const T* l, const T* r, T* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        dst[2 * i] = l[i];
        dst[2 * i + 1] = r[i];
    }
}

template <typename T>
inline void deinterleave2_impl(const T* src, T* l, T* r, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        l[i] = src[2 * i];
        r[i] = src[2 * i + 1];
    }
}

} // namespace pulp::simd::detail

// Defines every kernel overload for element type T in the current namespace.
// SPEC is `inline` for the header fallback and empty for the compiled backend.
#define PULP_SIMD_SCALAR_DEFINE(SPEC, T)                                                           \
    SPEC void add(const T* a, const T* b, T* dst, std::size_t n) noexcept {                        \
        ::pulp::simd::detail::add_impl(a, b, dst, n);                                              \
    }                                                                                              \
    SPEC void mul(const T* a, const T* b, T* dst, std::size_t n) noexcept {                        \
        ::pulp::simd::detail::mul_impl(a, b, dst, n);                                              \
    }                                                                                              \
    SPEC void fma(const T* a, const T* b, const T* c, T* dst, std::size_t n) noexcept {            \
        ::pulp::simd::detail::fma_impl(a, b, c, dst, n);                                           \
    }                                                                                              \
    SPEC void set(T value, T* dst, std::size_t n) noexcept {                                       \
        ::pulp::simd::detail::set_impl(value, dst, n);                                             \
    }                                                                                              \
    SPEC void scale(const T* a, T s, T* dst, std::size_t n) noexcept {                             \
        ::pulp::simd::detail::scale_impl(a, s, dst, n);                                            \
    }                                                                                              \
    SPEC void add_scaled(const T* a, T s, T* dst, std::size_t n) noexcept {                        \
        ::pulp::simd::detail::add_scaled_impl(a, s, dst, n);                                       \
    }                                                                                              \
    SPEC T sum(const T* x, std::size_t n) noexcept {                                               \
        return ::pulp::simd::detail::sum_impl(x, n);                                               \
    }                                                                                              \
    SPEC T maximum(const T* x, std::size_t n) noexcept {                                           \
        return ::pulp::simd::detail::maximum_impl(x, n);                                           \
    }                                                                                              \
    SPEC T minimum(const T* x, std::size_t n) noexcept {                                           \
        return ::pulp::simd::detail::minimum_impl(x, n);                                           \
    }                                                                                              \
    SPEC void abs(const T* a, T* dst, std::size_t n) noexcept {                                    \
        ::pulp::simd::detail::abs_impl(a, dst, n);                                                 \
    }                                                                                              \
    SPEC void clamp(const T* a, T lo, T hi, T* dst, std::size_t n) noexcept {                      \
        ::pulp::simd::detail::clamp_impl(a, lo, hi, dst, n);                                       \
    }                                                                                              \
    SPEC T dot(const T* a, const T* b, std::size_t n) noexcept {                                   \
        return ::pulp::simd::detail::dot_impl(a, b, n);                                            \
    }                                                                                              \
    SPEC T sum_squares(const T* x, std::size_t n) noexcept {                                       \
        return ::pulp::simd::detail::sum_squares_impl(x, n);                                       \
    }                                                                                              \
    SPEC T max_abs(const T* x, std::size_t n) noexcept {                                           \
        return ::pulp::simd::detail::max_abs_impl(x, n);                                           \
    }                                                                                              \
    SPEC void ramp_mul(const T* x, T start, T step, T* dst, std::size_t n) noexcept {              \
        ::pulp::simd::detail::ramp_mul_impl(x, start, step, dst, n);                               \
    }                                                                                              \
    SPEC void correlate(const T* x, const T* h, T* y, std::size_t n_out,                           \
                        std::size_t taps) noexcept {                                               \
        ::pulp::simd::detail::correlate_impl(x, h, y, n_out, taps);                                \
    }                                                                                              \
    SPEC void sub(const T* a, const T* b, T* dst, std::size_t n) noexcept {                        \
        ::pulp::simd::detail::sub_impl(a, b, dst, n);                                              \
    }                                                                                              \
    SPEC void div(const T* a, const T* b, T* dst, std::size_t n) noexcept {                        \
        ::pulp::simd::detail::div_impl(a, b, dst, n);                                              \
    }                                                                                              \
    SPEC void negate(const T* a, T* dst, std::size_t n) noexcept {                                 \
        ::pulp::simd::detail::negate_impl(a, dst, n);                                              \
    }                                                                                              \
    SPEC T mean(const T* x, std::size_t n) noexcept {                                              \
        return ::pulp::simd::detail::mean_impl(x, n);                                              \
    }                                                                                              \
    SPEC T rms(const T* x, std::size_t n) noexcept {                                               \
        return ::pulp::simd::detail::rms_impl(x, n);                                               \
    }                                                                                              \
    SPEC T find_maximum(const T* x, std::size_t n, std::size_t* index) noexcept {                  \
        return ::pulp::simd::detail::find_maximum_impl(x, n, index);                               \
    }                                                                                              \
    SPEC T find_minimum(const T* x, std::size_t n, std::size_t* index) noexcept {                  \
        return ::pulp::simd::detail::find_minimum_impl(x, n, index);                               \
    }                                                                                              \
    SPEC void replace_below(const T* a, T threshold, T value, T* dst, std::size_t n) noexcept {    \
        ::pulp::simd::detail::replace_below_impl(a, threshold, value, dst, n);                     \
    }                                                                                              \
    SPEC void replace_above(const T* a, T threshold, T value, T* dst, std::size_t n) noexcept {    \
        ::pulp::simd::detail::replace_above_impl(a, threshold, value, dst, n);                     \
    }                                                                                              \
    SPEC void from_int16(const std::int16_t* src, T scale, T* dst, std::size_t n) noexcept {       \
        ::pulp::simd::detail::from_int_impl(src, scale, dst, n);                                   \
    }                                                                                              \
    SPEC void from_int32(const std::int32_t* src, T scale, T* dst, std::size_t n) noexcept {       \
        ::pulp::simd::detail::from_int_impl(src, scale, dst, n);                                   \
    }                                                                                              \
    SPEC void to_int16(const T* src, T scale, std::int16_t* dst, std::size_t n) noexcept {         \
        ::pulp::simd::detail::to_int_impl(src, scale, dst, n);                                     \
    }                                                                                              \
    SPEC void to_int32(const T* src, T scale, std::int32_t* dst, std::size_t n) noexcept {         \
        ::pulp::simd::detail::to_int_impl(src, scale, dst, n);                                     \
    }                                                                                              \
    SPEC void interleave2(const T* l, const T* r, T* dst, std::size_t n) noexcept {                \
        ::pulp::simd::detail::interleave2_impl(l, r, dst, n);                                      \
    }                                                                                              \
    SPEC void deinterleave2(const T* src, T* l, T* r, std::size_t n) noexcept {                    \
        ::pulp::simd::detail::deinterleave2_impl(src, l, r, n);                                    \
    }                                                                                              \
    SPEC void decimate2(const T* x, const T* h, T* y, std::size_t n_out,                           \
                        std::size_t taps) noexcept {                                               \
        ::pulp::simd::detail::decimate2_impl(x, h, y, n_out, taps);                                \
    }
