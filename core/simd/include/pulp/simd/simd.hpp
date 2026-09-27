#pragma once

// pulp::simd — portable block kernels for DSP.
//
// One API for vectorized buffer arithmetic, reductions and FIR correlation.
// Every kernel exists in up to three backends, compiled into the pulp-simd
// library:
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
// Highway elsewhere). The choice is a compile-time alias, so a call costs no
// run-time branch. The explicitly qualified backends stay callable so tests
// and benchmarks can compare them in one binary.
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
    void decimate2(const T* x, const T* h, T* y, std::size_t n_out, std::size_t taps) noexcept;

namespace pulp::simd {

namespace backend {

namespace scalar {
PULP_SIMD_DECLARE_KERNELS(float)
PULP_SIMD_DECLARE_KERNELS(double)
} // namespace scalar

#if defined(PULP_SIMD_HAS_HIGHWAY)
namespace highway {
PULP_SIMD_DECLARE_KERNELS(float)
PULP_SIMD_DECLARE_KERNELS(double)
} // namespace highway
#endif

#if defined(PULP_SIMD_HAS_ACCELERATE)
namespace accelerate {
PULP_SIMD_DECLARE_KERNELS(float)
PULP_SIMD_DECLARE_KERNELS(double)
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
using active_backend::decimate2;
using active_backend::dot;
using active_backend::fma;
using active_backend::max_abs;
using active_backend::maximum;
using active_backend::minimum;
using active_backend::mul;
using active_backend::ramp_mul;
using active_backend::scale;
using active_backend::set;
using active_backend::sum;
using active_backend::sum_squares;

#if defined(PULP_SIMD_HAS_HIGHWAY)
/// Number of float lanes on the dispatched Highway target.
std::size_t float_lanes() noexcept;

/// Number of double lanes on the dispatched Highway target.
std::size_t double_lanes() noexcept;
#endif

} // namespace pulp::simd

#undef PULP_SIMD_DECLARE_KERNELS
