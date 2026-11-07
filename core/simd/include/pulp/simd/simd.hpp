#pragma once

// pulp::simd — portable block kernels for DSP.
//
// One API for vectorized buffer arithmetic, reductions and FIR correlation.
// Every kernel exists in up to three backends, compiled into the pulp-simd
// library when the target supports them:
//
//   backend::scalar      plain sequential loops; the numerical reference and
//                        the fallback when no other backend is compiled.
//   backend::highway     Google Highway with run-time dispatch to the best
//                        instruction set the CPU offers (SSE2 through AVX-512
//                        on x86, NEON/SVE on arm), independent of the
//                        caller's baseline ISA.
//   backend::accelerate  Apple Accelerate (vDSP); Apple platforms only.
//
// The unqualified pulp::simd:: names are the backend chosen at configure time
// by the PULP_SIMD_BACKEND CMake option (default `auto`: Accelerate on Apple,
// Highway elsewhere, scalar when Highway is unavailable such as MSVC
// ARM64EC). The choice is a compile-time alias, so a call costs no run-time
// branch. The explicitly qualified backends stay callable so tests and
// benchmarks can compare them in one binary when they are compiled.
//
// Numerics: the backends agree to rounding, not to the bit. Reductions (sum,
// dot, sum_squares, correlate, decimate2) accumulate in each backend's own
// order, and Highway/Accelerate may fuse multiply-adds, so compare against the
// scalar reference with a tolerance. Each backend is deterministic for a given
// binary, dispatch target and OS. Accelerate is updated with the OS, so never
// pin its output bits in a fixture. NaN ordering in maximum/minimum/max_abs/
// clamp is unspecified.
//
// Realtime: no kernel allocates, locks, or throws.
//
// Aliasing: dst may equal a source pointer exactly where noted; partial
// overlap is never supported.

#include <cstddef>
#include <cstdint>

// Kernel signatures, declared once per backend below.
//
//   add(a, b, dst, n)          dst[i] = a[i] + b[i]            (dst == a or b ok)
//   mul(a, b, dst, n)          dst[i] = a[i] * b[i]            (dst == a or b ok)
//   fma(a, b, c, dst, n)       dst[i] = a[i] * b[i] + c[i]
//   set(value, dst, n)         dst[i] = value
//   scale(a, s, dst, n)        dst[i] = a[i] * s               (dst == a ok)
//   add_scaled(a, s, dst, n)   dst[i] += a[i] * s              (dst == a ok)
//   sum(x, n)                  sum of x[i]                     (0 when n == 0)
//   maximum(x, n)              largest x[i]                    (0 when n == 0)
//   minimum(x, n)              smallest x[i]                   (0 when n == 0)
//   abs(a, dst, n)             dst[i] = |a[i]|                 (dst == a ok)
//   clamp(a, lo, hi, dst, n)   dst[i] = clamp(a[i], lo, hi)    (dst == a ok)
//   dot(a, b, n)               sum of a[i] * b[i]              (0 when n == 0)
//   sum_squares(x, n)          sum of x[i] * x[i]              (0 when n == 0)
//   max_abs(x, n)              largest |x[i]|                  (0 when n == 0)
//   ramp_mul(x, start, step, dst, n)
//                              dst[i] = x[i] * (start + i * step) (dst == x ok)
//   sub(a, b, dst, n)          dst[i] = a[i] - b[i]            (dst == a or b ok)
//   div(a, b, dst, n)          dst[i] = a[i] / b[i]            (dst == a or b ok)
//   negate(a, dst, n)          dst[i] = -a[i]                  (dst == a ok)
//   mean(x, n)                 arithmetic mean of x[i]         (0 when n == 0)
//   rms(x, n)                  sqrt(mean of x[i] * x[i])       (0 when n == 0)
//   find_maximum(x, n, &i)     largest x[i]; *i is the lowest index holding it
//   find_minimum(x, n, &i)     smallest x[i]; *i is the lowest index holding it
//                              Both return 0 and set *i = 0 when n == 0.
//   replace_below(a, threshold, value, dst, n)
//                              dst[i] = a[i] < threshold ? value : a[i]   (dst == a ok)
//   replace_above(a, threshold, value, dst, n)
//                              dst[i] = a[i] > threshold ? value : a[i]   (dst == a ok)
//                              A NaN input is never replaced.
//   from_int16(src, s, dst, n) dst[i] = T(src[i]) * s     (fixed-point to float)
//   from_int32(src, s, dst, n) dst[i] = T(src[i]) * s
//   to_int16(src, s, dst, n)   dst[i] = int16(round_half_even(src[i] * s)),
//                              saturated to [-32768, 32767]; NaN becomes 0.
//   to_int32(src, s, dst, n)   same for int32. A float source saturates at
//                              2147483520, the largest float below 2^31.
//                              Use s = 32768 (int16) or 2147483648 (int32) to
//                              convert full-scale float audio, and the
//                              reciprocal for the way back.
//   interleave2(l, r, dst, frames)
//                              dst[2i] = l[i], dst[2i + 1] = r[i]
//   deinterleave2(src, l, r, frames)
//                              l[i] = src[2i], r[i] = src[2i + 1]
//                              Interleave/deinterleave buffers must not overlap.
//
//   A copy with gain is scale(): dst[i] = a[i] * s.
//
//   correlate(x, h, y, n_out, taps)
//                              y[i] = sum_{k < taps} x[i + k] * h[k]
//                              x holds n_out + taps - 1 samples. A FIR filter
//                              is correlate() with h the time-reversed impulse
//                              response over a linear (unwrapped) history.
//   decimate2(x, h, y, n_out, taps)
//                              y[i] = sum_{k < taps} x[2 * i + k] * h[k]
//                              x holds 2 * (n_out - 1) + taps samples.
//
// y must not overlap x or h in correlate/decimate2.

#define PULP_SIMD_DECLARE_KERNELS(T)                                                               \
    void add(const T* a, const T* b, T* dst, std::size_t count) noexcept;                          \
    void mul(const T* a, const T* b, T* dst, std::size_t count) noexcept;                          \
    void fma(const T* a, const T* b, const T* c, T* dst, std::size_t count) noexcept;              \
    void set(T value, T* dst, std::size_t count) noexcept;                                         \
    void scale(const T* a, T scalar, T* dst, std::size_t count) noexcept;                          \
    void add_scaled(const T* a, T scalar, T* dst, std::size_t count) noexcept;                     \
    T sum(const T* data, std::size_t count) noexcept;                                              \
    T maximum(const T* data, std::size_t count) noexcept;                                          \
    T minimum(const T* data, std::size_t count) noexcept;                                          \
    void abs(const T* a, T* dst, std::size_t count) noexcept;                                      \
    void clamp(const T* a, T lo, T hi, T* dst, std::size_t count) noexcept;                        \
    T dot(const T* a, const T* b, std::size_t count) noexcept;                                     \
    T sum_squares(const T* data, std::size_t count) noexcept;                                      \
    T max_abs(const T* data, std::size_t count) noexcept;                                          \
    void ramp_mul(const T* x, T start, T step, T* dst, std::size_t count) noexcept;                \
    void correlate(const T* x, const T* h, T* y, std::size_t n_out, std::size_t taps) noexcept;    \
    void decimate2(const T* x, const T* h, T* y, std::size_t n_out, std::size_t taps) noexcept;      \
    void sub(const T* a, const T* b, T* dst, std::size_t count) noexcept;                          \
    void div(const T* a, const T* b, T* dst, std::size_t count) noexcept;                          \
    void negate(const T* a, T* dst, std::size_t count) noexcept;                                   \
    T mean(const T* data, std::size_t count) noexcept;                                             \
    T rms(const T* data, std::size_t count) noexcept;                                              \
    T find_maximum(const T* data, std::size_t count, std::size_t* index) noexcept;                 \
    T find_minimum(const T* data, std::size_t count, std::size_t* index) noexcept;                 \
    void replace_below(const T* a, T threshold, T value, T* dst, std::size_t count) noexcept;      \
    void replace_above(const T* a, T threshold, T value, T* dst, std::size_t count) noexcept;      \
    void from_int16(const std::int16_t* src, T scale, T* dst, std::size_t count) noexcept;         \
    void from_int32(const std::int32_t* src, T scale, T* dst, std::size_t count) noexcept;         \
    void to_int16(const T* src, T scale, std::int16_t* dst, std::size_t count) noexcept;           \
    void to_int32(const T* src, T scale, std::int32_t* dst, std::size_t count) noexcept;           \
    void interleave2(const T* left, const T* right, T* dst, std::size_t frames) noexcept;          \
    void deinterleave2(const T* src, T* left, T* right, std::size_t frames) noexcept;

// Complex kernels. `count` is the number of complex values. An interleaved
// buffer holds re, im, re, im, ... (2 * count scalars; the layout of
// std::complex<T> and of an FFT's packed output); a split buffer is two arrays
// of `count` scalars. Real outputs (magnitude, phase, power) are `count`
// scalars.
//
//   complex_multiply(a, b, dst, n)              dst = a * b
//   complex_multiply_conj(a, b, dst, n)         dst = a * conj(b)
//   complex_multiply_accumulate(a, b, acc, n)   acc += a * b
//   complex_magnitude(z, dst, n)                dst[i] = sqrt(re^2 + im^2)
//   complex_magnitude_squared(z, dst, n)        dst[i] = re^2 + im^2
//   complex_phase(z, dst, n)                    dst[i] = atan2(im, re), in [-pi, pi]
//   complex_power_spectrum(z, scale, dst, n)    dst[i] = scale * (re^2 + im^2)
//
// Each has a `_split` form taking separate real and imaginary arrays for every
// complex operand, e.g. complex_multiply_split(a_re, a_im, b_re, b_im,
// dst_re, dst_im, n). For the multiply family dst (or acc) may equal an input
// exactly, never partially. Magnitude, phase and power outputs must not overlap
// their input.
//
// The products are the plain formulas (ar*br - ai*bi, ar*bi + ai*br): unlike
// std::complex::operator*, they do not branch to recover a NaN from an
// infinity times zero, so they are for finite spectra. Magnitude does not
// guard against overflow of re^2 + im^2. Phase of (0, 0) is 0; phase at a
// signed-zero boundary (such as (-0, +0)) is not pinned across backends.

#define PULP_SIMD_DECLARE_COMPLEX_KERNELS(T)                                                       \
    void complex_multiply(const T* a, const T* b, T* dst, std::size_t count) noexcept;             \
    void complex_multiply_conj(const T* a, const T* b, T* dst, std::size_t count) noexcept;        \
    void complex_multiply_accumulate(const T* a, const T* b, T* acc, std::size_t count) noexcept;  \
    void complex_magnitude(const T* z, T* dst, std::size_t count) noexcept;                        \
    void complex_magnitude_squared(const T* z, T* dst, std::size_t count) noexcept;                \
    void complex_phase(const T* z, T* dst, std::size_t count) noexcept;                            \
    void complex_power_spectrum(const T* z, T scale, T* dst, std::size_t count) noexcept;          \
    void complex_multiply_split(const T* a_re, const T* a_im, const T* b_re, const T* b_im,        \
                                T* dst_re, T* dst_im, std::size_t count) noexcept;                 \
    void complex_multiply_conj_split(const T* a_re, const T* a_im, const T* b_re, const T* b_im,   \
                                     T* dst_re, T* dst_im, std::size_t count) noexcept;            \
    void complex_multiply_accumulate_split(const T* a_re, const T* a_im, const T* b_re,            \
                                           const T* b_im, T* acc_re, T* acc_im,                    \
                                           std::size_t count) noexcept;                            \
    void complex_magnitude_split(const T* re, const T* im, T* dst, std::size_t count) noexcept;    \
    void complex_magnitude_squared_split(const T* re, const T* im, T* dst,                         \
                                         std::size_t count) noexcept;                              \
    void complex_phase_split(const T* re, const T* im, T* dst, std::size_t count) noexcept;        \
    void complex_power_spectrum_split(const T* re, const T* im, T scale, T* dst,                   \
                                      std::size_t count) noexcept;

namespace pulp::simd {

namespace backend {

namespace scalar {
PULP_SIMD_DECLARE_KERNELS(float)
PULP_SIMD_DECLARE_KERNELS(double)
PULP_SIMD_DECLARE_COMPLEX_KERNELS(float)
PULP_SIMD_DECLARE_COMPLEX_KERNELS(double)
} // namespace scalar

#if defined(PULP_SIMD_HAS_HIGHWAY)
namespace highway {
PULP_SIMD_DECLARE_KERNELS(float)
PULP_SIMD_DECLARE_KERNELS(double)
PULP_SIMD_DECLARE_COMPLEX_KERNELS(float)
PULP_SIMD_DECLARE_COMPLEX_KERNELS(double)
} // namespace highway
#endif

#if defined(PULP_SIMD_HAS_ACCELERATE)
namespace accelerate {
PULP_SIMD_DECLARE_KERNELS(float)
PULP_SIMD_DECLARE_KERNELS(double)
PULP_SIMD_DECLARE_COMPLEX_KERNELS(float)
PULP_SIMD_DECLARE_COMPLEX_KERNELS(double)
} // namespace accelerate
#endif

} // namespace backend

} // namespace pulp::simd

// A translation unit compiled without pulp-simd's usage requirements (a
// hand-listed source build, e.g. a wasm or VCV Rack plugin that compiles the
// signal headers directly) gets the scalar reference inline, so it needs only
// this include directory and no library.
#if !defined(PULP_SIMD_DEFAULT_BACKEND_ACCELERATE) &&                                              \
    !defined(PULP_SIMD_DEFAULT_BACKEND_HIGHWAY) && !defined(PULP_SIMD_DEFAULT_BACKEND_SCALAR)
#include <pulp/simd/scalar_kernels.hpp>
namespace pulp::simd::backend::inline_scalar {
PULP_SIMD_SCALAR_DEFINE(inline, float)
PULP_SIMD_SCALAR_DEFINE(inline, double)
PULP_SIMD_SCALAR_DEFINE_COMPLEX(inline, float)
PULP_SIMD_SCALAR_DEFINE_COMPLEX(inline, double)
} // namespace pulp::simd::backend::inline_scalar
#endif

namespace pulp::simd {

// The configure-time backend (see PULP_SIMD_BACKEND).
#if defined(PULP_SIMD_DEFAULT_BACKEND_ACCELERATE)
namespace active_backend = backend::accelerate;
inline constexpr const char* active_backend_name = "accelerate";
#elif defined(PULP_SIMD_DEFAULT_BACKEND_HIGHWAY)
namespace active_backend = backend::highway;
inline constexpr const char* active_backend_name = "highway";
#elif defined(PULP_SIMD_DEFAULT_BACKEND_SCALAR)
namespace active_backend = backend::scalar;
inline constexpr const char* active_backend_name = "scalar";
#else
namespace active_backend = backend::inline_scalar;
inline constexpr const char* active_backend_name = "inline-scalar";
#endif

using active_backend::abs;
using active_backend::add;
using active_backend::add_scaled;
using active_backend::clamp;
using active_backend::correlate;
using active_backend::complex_magnitude;
using active_backend::complex_magnitude_split;
using active_backend::complex_magnitude_squared;
using active_backend::complex_magnitude_squared_split;
using active_backend::complex_multiply;
using active_backend::complex_multiply_accumulate;
using active_backend::complex_multiply_accumulate_split;
using active_backend::complex_multiply_conj;
using active_backend::complex_multiply_conj_split;
using active_backend::complex_multiply_split;
using active_backend::complex_phase;
using active_backend::complex_phase_split;
using active_backend::complex_power_spectrum;
using active_backend::complex_power_spectrum_split;
using active_backend::decimate2;
using active_backend::deinterleave2;
using active_backend::div;
using active_backend::dot;
using active_backend::find_maximum;
using active_backend::find_minimum;
using active_backend::fma;
using active_backend::from_int16;
using active_backend::from_int32;
using active_backend::interleave2;
using active_backend::max_abs;
using active_backend::maximum;
using active_backend::mean;
using active_backend::minimum;
using active_backend::mul;
using active_backend::negate;
using active_backend::ramp_mul;
using active_backend::replace_above;
using active_backend::replace_below;
using active_backend::rms;
using active_backend::scale;
using active_backend::set;
using active_backend::sub;
using active_backend::sum;
using active_backend::sum_squares;
using active_backend::to_int16;
using active_backend::to_int32;

#if defined(PULP_SIMD_HAS_HIGHWAY)
/// Number of float lanes on the dispatched Highway target.
std::size_t float_lanes() noexcept;

/// Number of double lanes on the dispatched Highway target.
std::size_t double_lanes() noexcept;
#else
// The scalar and Accelerate backends do not have a Highway dispatch target
// whose lane width can be queried. Keep the backend-independent API callable
// for those configurations; one lane is the scalar/reference width.
inline constexpr std::size_t float_lanes() noexcept {
    return 1;
}

inline constexpr std::size_t double_lanes() noexcept {
    return 1;
}
#endif

} // namespace pulp::simd

#undef PULP_SIMD_DECLARE_COMPLEX_KERNELS
#undef PULP_SIMD_DECLARE_KERNELS
