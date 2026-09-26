// pulp::simd kernels, Highway backend.
//
// Highway's foreach_target compiles every kernel below once per instruction
// set (SSE2/SSE4/AVX2/AVX-512 on x86, NEON/SVE on arm) and HWY_DYNAMIC_DISPATCH
// picks the best one the running CPU supports, so the kernels are not limited
// by the baseline ISA the rest of the build targets.

#include <pulp/simd/simd.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "src/highway.cpp"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();

namespace pulp::simd::backend::highway {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

size_t FloatLanes() {
    const hn::ScalableTag<float> d;
    return hn::Lanes(d);
}

size_t DoubleLanes() {
    const hn::ScalableTag<double> d;
    return hn::Lanes(d);
}

template <typename T> void AddT(const T* a, const T* b, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(hn::Add(hn::LoadU(d, a + i), hn::LoadU(d, b + i)), d, dst + i);
    for (; i < count; ++i)
        dst[i] = a[i] + b[i];
}

template <typename T> void MulT(const T* a, const T* b, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(hn::Mul(hn::LoadU(d, a + i), hn::LoadU(d, b + i)), d, dst + i);
    for (; i < count; ++i)
        dst[i] = a[i] * b[i];
}

template <typename T>
void FmaT(const T* HWY_RESTRICT a, const T* HWY_RESTRICT b, const T* HWY_RESTRICT c,
          T* HWY_RESTRICT dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(hn::MulAdd(hn::LoadU(d, a + i), hn::LoadU(d, b + i), hn::LoadU(d, c + i)), d,
                   dst + i);
    for (; i < count; ++i)
        dst[i] = a[i] * b[i] + c[i];
}

template <typename T> void SetT(T value, T* HWY_RESTRICT dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    const auto v = hn::Set(d, value);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(v, d, dst + i);
    for (; i < count; ++i)
        dst[i] = value;
}

template <typename T> void ScaleT(const T* a, T scalar, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    const auto s = hn::Set(d, scalar);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(hn::Mul(hn::LoadU(d, a + i), s), d, dst + i);
    for (; i < count; ++i)
        dst[i] = a[i] * scalar;
}

template <typename T> void AddScaledT(const T* a, T scalar, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    const auto s = hn::Set(d, scalar);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(hn::MulAdd(hn::LoadU(d, a + i), s, hn::LoadU(d, dst + i)), d, dst + i);
    for (; i < count; ++i)
        dst[i] += a[i] * scalar;
}

// Four independent accumulators hide the add latency that makes a single
// running sum latency-bound.
template <typename T> T SumT(const T* HWY_RESTRICT x, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    auto s0 = hn::Zero(d), s1 = hn::Zero(d), s2 = hn::Zero(d), s3 = hn::Zero(d);
    size_t i = 0;
    for (; i + 4 * N <= count; i += 4 * N) {
        s0 = hn::Add(s0, hn::LoadU(d, x + i));
        s1 = hn::Add(s1, hn::LoadU(d, x + i + N));
        s2 = hn::Add(s2, hn::LoadU(d, x + i + 2 * N));
        s3 = hn::Add(s3, hn::LoadU(d, x + i + 3 * N));
    }
    for (; i + N <= count; i += N)
        s0 = hn::Add(s0, hn::LoadU(d, x + i));
    T acc = hn::ReduceSum(d, hn::Add(hn::Add(s0, s1), hn::Add(s2, s3)));
    for (; i < count; ++i)
        acc += x[i];
    return acc;
}

template <typename T> T MaximumT(const T* HWY_RESTRICT x, size_t count) {
    if (count == 0)
        return T(0);
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    auto m = hn::Set(d, x[0]);
    size_t i = 0;
    for (; i + N <= count; i += N)
        m = hn::Max(m, hn::LoadU(d, x + i));
    T r = hn::ReduceMax(d, m);
    for (; i < count; ++i)
        r = std::max(r, x[i]);
    return r;
}

template <typename T> T MinimumT(const T* HWY_RESTRICT x, size_t count) {
    if (count == 0)
        return T(0);
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    auto m = hn::Set(d, x[0]);
    size_t i = 0;
    for (; i + N <= count; i += N)
        m = hn::Min(m, hn::LoadU(d, x + i));
    T r = hn::ReduceMin(d, m);
    for (; i < count; ++i)
        r = std::min(r, x[i]);
    return r;
}

template <typename T> void AbsT(const T* a, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(hn::Abs(hn::LoadU(d, a + i)), d, dst + i);
    for (; i < count; ++i)
        dst[i] = std::abs(a[i]);
}

template <typename T> void ClampT(const T* a, T lo, T hi, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    const auto vlo = hn::Set(d, lo);
    const auto vhi = hn::Set(d, hi);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(hn::Clamp(hn::LoadU(d, a + i), vlo, vhi), d, dst + i);
    for (; i < count; ++i)
        dst[i] = std::clamp(a[i], lo, hi);
}

template <typename T> T DotT(const T* HWY_RESTRICT a, const T* HWY_RESTRICT b, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    auto s0 = hn::Zero(d), s1 = hn::Zero(d), s2 = hn::Zero(d), s3 = hn::Zero(d);
    size_t i = 0;
    for (; i + 4 * N <= count; i += 4 * N) {
        s0 = hn::MulAdd(hn::LoadU(d, a + i), hn::LoadU(d, b + i), s0);
        s1 = hn::MulAdd(hn::LoadU(d, a + i + N), hn::LoadU(d, b + i + N), s1);
        s2 = hn::MulAdd(hn::LoadU(d, a + i + 2 * N), hn::LoadU(d, b + i + 2 * N), s2);
        s3 = hn::MulAdd(hn::LoadU(d, a + i + 3 * N), hn::LoadU(d, b + i + 3 * N), s3);
    }
    for (; i + N <= count; i += N)
        s0 = hn::MulAdd(hn::LoadU(d, a + i), hn::LoadU(d, b + i), s0);
    T acc = hn::ReduceSum(d, hn::Add(hn::Add(s0, s1), hn::Add(s2, s3)));
    for (; i < count; ++i)
        acc += a[i] * b[i];
    return acc;
}

template <typename T> T SumSquaresT(const T* HWY_RESTRICT x, size_t count) {
    return DotT(x, x, count);
}

template <typename T> T MaxAbsT(const T* HWY_RESTRICT x, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    auto m0 = hn::Zero(d), m1 = hn::Zero(d);
    size_t i = 0;
    for (; i + 2 * N <= count; i += 2 * N) {
        m0 = hn::Max(m0, hn::Abs(hn::LoadU(d, x + i)));
        m1 = hn::Max(m1, hn::Abs(hn::LoadU(d, x + i + N)));
    }
    for (; i + N <= count; i += N)
        m0 = hn::Max(m0, hn::Abs(hn::LoadU(d, x + i)));
    T r = hn::ReduceMax(d, hn::Max(m0, m1));
    for (; i < count; ++i)
        r = std::max(r, std::abs(x[i]));
    return r;
}

// Gain for lane j of the vector starting at i is start + (i + j) * step,
// computed directly (not by repeated addition) so long ramps do not drift.
template <typename T> void RampMulT(const T* x, T start, T step, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    const auto vstart = hn::Set(d, start);
    const auto vstep = hn::Set(d, step);
    const auto iota = hn::Iota(d, T(0));
    size_t i = 0;
    for (; i + N <= count; i += N) {
        const auto index = hn::Add(iota, hn::Set(d, T(i)));
        const auto gain = hn::MulAdd(index, vstep, vstart);
        hn::StoreU(hn::Mul(hn::LoadU(d, x + i), gain), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = x[i] * (start + T(i) * step);
}

// Vectorized across outputs: four vectors of consecutive outputs share each
// broadcast tap, so every output accumulates its taps in order (k = 0, 1, ...)
// with fused multiply-adds and no horizontal reduction. The scalar tail uses
// the same order and std::fma, so an output's value does not depend on where
// it falls in the block: splitting a block differently gives identical bits.
template <typename T>
void CorrelateT(const T* HWY_RESTRICT x, const T* HWY_RESTRICT h, T* HWY_RESTRICT y, size_t n_out,
                size_t taps) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + 4 * N <= n_out; i += 4 * N) {
        auto a0 = hn::Zero(d), a1 = hn::Zero(d), a2 = hn::Zero(d), a3 = hn::Zero(d);
        const T* base = x + i;
        for (size_t k = 0; k < taps; ++k) {
            const auto hk = hn::Set(d, h[k]);
            a0 = hn::MulAdd(hn::LoadU(d, base + k), hk, a0);
            a1 = hn::MulAdd(hn::LoadU(d, base + k + N), hk, a1);
            a2 = hn::MulAdd(hn::LoadU(d, base + k + 2 * N), hk, a2);
            a3 = hn::MulAdd(hn::LoadU(d, base + k + 3 * N), hk, a3);
        }
        hn::StoreU(a0, d, y + i);
        hn::StoreU(a1, d, y + i + N);
        hn::StoreU(a2, d, y + i + 2 * N);
        hn::StoreU(a3, d, y + i + 3 * N);
    }
    for (; i + N <= n_out; i += N) {
        auto a0 = hn::Zero(d);
        for (size_t k = 0; k < taps; ++k)
            a0 = hn::MulAdd(hn::LoadU(d, x + i + k), hn::Set(d, h[k]), a0);
        hn::StoreU(a0, d, y + i);
    }
    for (; i < n_out; ++i) {
        T acc = T(0);
        for (size_t k = 0; k < taps; ++k)
            acc = std::fma(x[i + k], h[k], acc);
        y[i] = acc;
    }
}

template <typename T>
void Decimate2T(const T* HWY_RESTRICT x, const T* HWY_RESTRICT h, T* HWY_RESTRICT y, size_t n_out,
                size_t taps) {
    for (size_t i = 0; i < n_out; ++i)
        y[i] = DotT(x + 2 * i, h, taps);
}

#define PULP_SIMD_HWY_INSTANTIATE(SUFFIX, T)                                                       \
    void Add##SUFFIX(const T* a, const T* b, T* dst, size_t n) {                                   \
        AddT(a, b, dst, n);                                                                        \
    }                                                                                              \
    void Mul##SUFFIX(const T* a, const T* b, T* dst, size_t n) {                                   \
        MulT(a, b, dst, n);                                                                        \
    }                                                                                              \
    void Fma##SUFFIX(const T* a, const T* b, const T* c, T* dst, size_t n) {                       \
        FmaT(a, b, c, dst, n);                                                                     \
    }                                                                                              \
    void Set##SUFFIX(T v, T* dst, size_t n) {                                                      \
        SetT(v, dst, n);                                                                           \
    }                                                                                              \
    void Scale##SUFFIX(const T* a, T s, T* dst, size_t n) {                                        \
        ScaleT(a, s, dst, n);                                                                      \
    }                                                                                              \
    void AddScaled##SUFFIX(const T* a, T s, T* dst, size_t n) {                                    \
        AddScaledT(a, s, dst, n);                                                                  \
    }                                                                                              \
    T Sum##SUFFIX(const T* x, size_t n) {                                                          \
        return SumT(x, n);                                                                         \
    }                                                                                              \
    T Maximum##SUFFIX(const T* x, size_t n) {                                                      \
        return MaximumT(x, n);                                                                     \
    }                                                                                              \
    T Minimum##SUFFIX(const T* x, size_t n) {                                                      \
        return MinimumT(x, n);                                                                     \
    }                                                                                              \
    void Abs##SUFFIX(const T* a, T* dst, size_t n) {                                               \
        AbsT(a, dst, n);                                                                           \
    }                                                                                              \
    void Clamp##SUFFIX(const T* a, T lo, T hi, T* dst, size_t n) {                                 \
        ClampT(a, lo, hi, dst, n);                                                                 \
    }                                                                                              \
    T Dot##SUFFIX(const T* a, const T* b, size_t n) {                                              \
        return DotT(a, b, n);                                                                      \
    }                                                                                              \
    T SumSquares##SUFFIX(const T* x, size_t n) {                                                   \
        return SumSquaresT(x, n);                                                                  \
    }                                                                                              \
    T MaxAbs##SUFFIX(const T* x, size_t n) {                                                       \
        return MaxAbsT(x, n);                                                                      \
    }                                                                                              \
    void RampMul##SUFFIX(const T* x, T start, T step, T* dst, size_t n) {                          \
        RampMulT(x, start, step, dst, n);                                                          \
    }                                                                                              \
    void Correlate##SUFFIX(const T* x, const T* h, T* y, size_t n_out, size_t taps) {              \
        CorrelateT(x, h, y, n_out, taps);                                                          \
    }                                                                                              \
    void Decimate2##SUFFIX(const T* x, const T* h, T* y, size_t n_out, size_t taps) {              \
        Decimate2T(x, h, y, n_out, taps);                                                          \
    }

PULP_SIMD_HWY_INSTANTIATE(F32, float)
PULP_SIMD_HWY_INSTANTIATE(F64, double)

#undef PULP_SIMD_HWY_INSTANTIATE

} // namespace HWY_NAMESPACE
} // namespace pulp::simd::backend::highway

HWY_AFTER_NAMESPACE();

#if HWY_ONCE

// Cross-arch (macOS x86_64 + arm64 universal) portability invariant.
//
// Pulp's SIMD is Google Highway runtime dispatch ONLY — there are no raw
// NEON/SSE intrinsics in core/, so this translation unit recompiles cleanly
// for whichever slice (arm64 or x86_64) a thin or universal build is
// compiling, and the right kernel is chosen at run time by HWY_DYNAMIC_DISPATCH.
// If Highway ever sees an architecture with no compiled target at all, fail at
// compile time here rather than silently degrading to an empty dispatch table.
static_assert(HWY_TARGETS != 0,
              "Highway compiled no SIMD target for this architecture. Pulp requires at "
              "least a baseline target for its runtime-dispatched SIMD; an empty target "
              "set means the build arch is unsupported by the pinned Highway.");

namespace pulp::simd::backend::highway {

HWY_EXPORT(FloatLanes);
HWY_EXPORT(DoubleLanes);

#define PULP_SIMD_HWY_EXPORT(SUFFIX)                                                               \
    HWY_EXPORT(Add##SUFFIX);                                                                       \
    HWY_EXPORT(Mul##SUFFIX);                                                                       \
    HWY_EXPORT(Fma##SUFFIX);                                                                       \
    HWY_EXPORT(Set##SUFFIX);                                                                       \
    HWY_EXPORT(Scale##SUFFIX);                                                                     \
    HWY_EXPORT(AddScaled##SUFFIX);                                                                 \
    HWY_EXPORT(Sum##SUFFIX);                                                                       \
    HWY_EXPORT(Maximum##SUFFIX);                                                                   \
    HWY_EXPORT(Minimum##SUFFIX);                                                                   \
    HWY_EXPORT(Abs##SUFFIX);                                                                       \
    HWY_EXPORT(Clamp##SUFFIX);                                                                     \
    HWY_EXPORT(Dot##SUFFIX);                                                                       \
    HWY_EXPORT(SumSquares##SUFFIX);                                                                \
    HWY_EXPORT(MaxAbs##SUFFIX);                                                                    \
    HWY_EXPORT(RampMul##SUFFIX);                                                                   \
    HWY_EXPORT(Correlate##SUFFIX);                                                                 \
    HWY_EXPORT(Decimate2##SUFFIX);

PULP_SIMD_HWY_EXPORT(F32)
PULP_SIMD_HWY_EXPORT(F64)

#undef PULP_SIMD_HWY_EXPORT

#define PULP_SIMD_HWY_DEFINE(SUFFIX, T)                                                            \
    void add(const T* a, const T* b, T* dst, std::size_t n) noexcept {                             \
        HWY_DYNAMIC_DISPATCH(Add##SUFFIX)(a, b, dst, n);                                           \
    }                                                                                              \
    void mul(const T* a, const T* b, T* dst, std::size_t n) noexcept {                             \
        HWY_DYNAMIC_DISPATCH(Mul##SUFFIX)(a, b, dst, n);                                           \
    }                                                                                              \
    void fma(const T* a, const T* b, const T* c, T* dst, std::size_t n) noexcept {                 \
        HWY_DYNAMIC_DISPATCH(Fma##SUFFIX)(a, b, c, dst, n);                                        \
    }                                                                                              \
    void set(T v, T* dst, std::size_t n) noexcept {                                                \
        HWY_DYNAMIC_DISPATCH(Set##SUFFIX)(v, dst, n);                                              \
    }                                                                                              \
    void scale(const T* a, T s, T* dst, std::size_t n) noexcept {                                  \
        HWY_DYNAMIC_DISPATCH(Scale##SUFFIX)(a, s, dst, n);                                         \
    }                                                                                              \
    void add_scaled(const T* a, T s, T* dst, std::size_t n) noexcept {                             \
        HWY_DYNAMIC_DISPATCH(AddScaled##SUFFIX)(a, s, dst, n);                                     \
    }                                                                                              \
    T sum(const T* x, std::size_t n) noexcept {                                                    \
        return HWY_DYNAMIC_DISPATCH(Sum##SUFFIX)(x, n);                                            \
    }                                                                                              \
    T maximum(const T* x, std::size_t n) noexcept {                                                \
        return HWY_DYNAMIC_DISPATCH(Maximum##SUFFIX)(x, n);                                        \
    }                                                                                              \
    T minimum(const T* x, std::size_t n) noexcept {                                                \
        return HWY_DYNAMIC_DISPATCH(Minimum##SUFFIX)(x, n);                                        \
    }                                                                                              \
    void abs(const T* a, T* dst, std::size_t n) noexcept {                                         \
        HWY_DYNAMIC_DISPATCH(Abs##SUFFIX)(a, dst, n);                                              \
    }                                                                                              \
    void clamp(const T* a, T lo, T hi, T* dst, std::size_t n) noexcept {                           \
        HWY_DYNAMIC_DISPATCH(Clamp##SUFFIX)(a, lo, hi, dst, n);                                    \
    }                                                                                              \
    T dot(const T* a, const T* b, std::size_t n) noexcept {                                        \
        return HWY_DYNAMIC_DISPATCH(Dot##SUFFIX)(a, b, n);                                         \
    }                                                                                              \
    T sum_squares(const T* x, std::size_t n) noexcept {                                            \
        return HWY_DYNAMIC_DISPATCH(SumSquares##SUFFIX)(x, n);                                     \
    }                                                                                              \
    T max_abs(const T* x, std::size_t n) noexcept {                                                \
        return HWY_DYNAMIC_DISPATCH(MaxAbs##SUFFIX)(x, n);                                         \
    }                                                                                              \
    void ramp_mul(const T* x, T start, T step, T* dst, std::size_t n) noexcept {                   \
        HWY_DYNAMIC_DISPATCH(RampMul##SUFFIX)(x, start, step, dst, n);                             \
    }                                                                                              \
    void correlate(const T* x, const T* h, T* y, std::size_t n_out, std::size_t taps) noexcept {   \
        HWY_DYNAMIC_DISPATCH(Correlate##SUFFIX)(x, h, y, n_out, taps);                             \
    }                                                                                              \
    void decimate2(const T* x, const T* h, T* y, std::size_t n_out, std::size_t taps) noexcept {   \
        HWY_DYNAMIC_DISPATCH(Decimate2##SUFFIX)(x, h, y, n_out, taps);                             \
    }

PULP_SIMD_HWY_DEFINE(F32, float)
PULP_SIMD_HWY_DEFINE(F64, double)

#undef PULP_SIMD_HWY_DEFINE

} // namespace pulp::simd::backend::highway

namespace pulp::simd {

std::size_t float_lanes() noexcept {
    return HWY_DYNAMIC_DISPATCH(backend::highway::FloatLanes)();
}

std::size_t double_lanes() noexcept {
    return HWY_DYNAMIC_DISPATCH(backend::highway::DoubleLanes)();
}

} // namespace pulp::simd

#endif // HWY_ONCE
