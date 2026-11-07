// pulp::simd kernels, Highway backend.
//
// Highway's foreach_target compiles every kernel below once per instruction
// set (SSE2/SSE4/AVX2/AVX-512 on x86, NEON/SVE on arm) and HWY_DYNAMIC_DISPATCH
// picks the best one the running CPU supports, so the kernels are not limited
// by the baseline ISA the rest of the build targets.

#include <pulp/simd/scalar_kernels.hpp>
#include <pulp/simd/simd.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "src/highway.cpp"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>
// Header-only vector math (Apache-2.0), used for atan2 here and for the
// transcendental kernels. Compiled once per target like the code below.
#include <hwy/contrib/math/math-inl.h>

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

template <typename T> void SubT(const T* a, const T* b, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(hn::Sub(hn::LoadU(d, a + i), hn::LoadU(d, b + i)), d, dst + i);
    for (; i < count; ++i)
        dst[i] = a[i] - b[i];
}

template <typename T> void DivT(const T* a, const T* b, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(hn::Div(hn::LoadU(d, a + i), hn::LoadU(d, b + i)), d, dst + i);
    for (; i < count; ++i)
        dst[i] = a[i] / b[i];
}

template <typename T> void NegateT(const T* a, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(hn::Neg(hn::LoadU(d, a + i)), d, dst + i);
    for (; i < count; ++i)
        dst[i] = -a[i];
}

// The extremum is found with vector Max/Min; the index is then the first
// element equal to it, so ties resolve to the lowest index like the scalar
// reference.
template <typename T> T FindMaximumT(const T* HWY_RESTRICT x, size_t count, size_t* index) {
    const T m = MaximumT(x, count);
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) {
        if (x[i] == m) {
            at = i;
            break;
        }
    }
    *index = at;
    return m;
}

template <typename T> T FindMinimumT(const T* HWY_RESTRICT x, size_t count, size_t* index) {
    const T m = MinimumT(x, count);
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) {
        if (x[i] == m) {
            at = i;
            break;
        }
    }
    *index = at;
    return m;
}

template <typename T> void ReplaceBelowT(const T* a, T threshold, T value, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    const auto vt = hn::Set(d, threshold);
    const auto vv = hn::Set(d, value);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        const auto x = hn::LoadU(d, a + i);
        hn::StoreU(hn::IfThenElse(hn::Lt(x, vt), vv, x), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = a[i] < threshold ? value : a[i];
}

template <typename T> void ReplaceAboveT(const T* a, T threshold, T value, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    const auto vt = hn::Set(d, threshold);
    const auto vv = hn::Set(d, value);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        const auto x = hn::LoadU(d, a + i);
        hn::StoreU(hn::IfThenElse(hn::Gt(x, vt), vv, x), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = a[i] > threshold ? value : a[i];
}

template <typename T, typename I>
void FromIntT(const I* src, T scale, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const hn::Rebind<int32_t, decltype(d)> di32;
    const hn::Rebind<I, decltype(d)> dsrc;
    const size_t N = hn::Lanes(d);
    const auto s = hn::Set(d, scale);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        hn::Vec<decltype(di32)> wide;
        if constexpr (sizeof(I) == 4)
            wide = hn::LoadU(di32, src + i);
        else
            wide = hn::PromoteTo(di32, hn::LoadU(dsrc, src + i));
        hn::Vec<decltype(d)> f;
        if constexpr (sizeof(T) == 8)
            f = hn::PromoteTo(d, wide);
        else
            f = hn::ConvertTo(d, wide);
        hn::StoreU(hn::Mul(f, s), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = static_cast<T>(src[i]) * scale;
}

// float -> integer: scale, send NaN to 0, clamp to the representable range,
// round half to even, then narrow. The scalar tail is the same sequence.
template <typename T, typename I> void ToIntT(const T* src, T scale, I* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const hn::Rebind<int32_t, decltype(d)> di32;
    const hn::Rebind<I, decltype(d)> ddst;
    const size_t N = hn::Lanes(d);
    constexpr T lo = static_cast<T>(std::numeric_limits<I>::min());
    constexpr T hi = ::pulp::simd::detail::int_upper_bound<T, I>();
    const auto s = hn::Set(d, scale);
    const auto vlo = hn::Set(d, lo);
    const auto vhi = hn::Set(d, hi);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto v = hn::Mul(hn::LoadU(d, src + i), s);
        v = hn::IfThenElseZero(hn::Eq(v, v), v);
        v = hn::Min(hn::Max(v, vlo), vhi);
        hn::Vec<decltype(di32)> r;
        if constexpr (sizeof(T) == 4)
            r = hn::NearestInt(v);
        else
            r = hn::DemoteToNearestInt(di32, v);
        if constexpr (sizeof(I) == 4)
            hn::StoreU(r, di32, dst + i);
        else
            hn::StoreU(hn::DemoteTo(ddst, r), ddst, dst + i);
    }
    for (; i < count; ++i) {
        T v = src[i] * scale;
        if (v != v) {
            dst[i] = 0;
            continue;
        }
        v = v < lo ? lo : (v > hi ? hi : v);
        dst[i] = static_cast<I>(std::nearbyint(v));
    }
}

template <typename T> void Interleave2T(const T* l, const T* r, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreInterleaved2(hn::LoadU(d, l + i), hn::LoadU(d, r + i), d, dst + 2 * i);
    for (; i < count; ++i) {
        dst[2 * i] = l[i];
        dst[2 * i + 1] = r[i];
    }
}

template <typename T> void Deinterleave2T(const T* src, T* l, T* r, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        hn::Vec<decltype(d)> va, vb;
        hn::LoadInterleaved2(d, src + 2 * i, va, vb);
        hn::StoreU(va, d, l + i);
        hn::StoreU(vb, d, r + i);
    }
    for (; i < count; ++i) {
        l[i] = src[2 * i];
        r[i] = src[2 * i + 1];
    }
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
    void Sub##SUFFIX(const T* a, const T* b, T* dst, size_t n) {                                   \
        SubT(a, b, dst, n);                                                                        \
    }                                                                                              \
    void Div##SUFFIX(const T* a, const T* b, T* dst, size_t n) {                                   \
        DivT(a, b, dst, n);                                                                        \
    }                                                                                              \
    void Negate##SUFFIX(const T* a, T* dst, size_t n) {                                            \
        NegateT(a, dst, n);                                                                        \
    }                                                                                              \
    T FindMaximum##SUFFIX(const T* x, size_t n, size_t* index) {                                   \
        return FindMaximumT(x, n, index);                                                          \
    }                                                                                              \
    T FindMinimum##SUFFIX(const T* x, size_t n, size_t* index) {                                   \
        return FindMinimumT(x, n, index);                                                          \
    }                                                                                              \
    void ReplaceBelow##SUFFIX(const T* a, T threshold, T value, T* dst, size_t n) {                \
        ReplaceBelowT(a, threshold, value, dst, n);                                                \
    }                                                                                              \
    void ReplaceAbove##SUFFIX(const T* a, T threshold, T value, T* dst, size_t n) {                \
        ReplaceAboveT(a, threshold, value, dst, n);                                                \
    }                                                                                              \
    void FromInt16##SUFFIX(const int16_t* src, T scale, T* dst, size_t n) {                        \
        FromIntT(src, scale, dst, n);                                                              \
    }                                                                                              \
    void FromInt32##SUFFIX(const int32_t* src, T scale, T* dst, size_t n) {                        \
        FromIntT(src, scale, dst, n);                                                              \
    }                                                                                              \
    void ToInt16##SUFFIX(const T* src, T scale, int16_t* dst, size_t n) {                          \
        ToIntT(src, scale, dst, n);                                                                \
    }                                                                                              \
    void ToInt32##SUFFIX(const T* src, T scale, int32_t* dst, size_t n) {                          \
        ToIntT(src, scale, dst, n);                                                                \
    }                                                                                              \
    void Interleave2##SUFFIX(const T* l, const T* r, T* dst, size_t n) {                           \
        Interleave2T(l, r, dst, n);                                                                \
    }                                                                                              \
    void Deinterleave2##SUFFIX(const T* src, T* l, T* r, size_t n) {                               \
        Deinterleave2T(src, l, r, n);                                                              \
    }                                                                                              \
    void Decimate2##SUFFIX(const T* x, const T* h, T* y, size_t n_out, size_t taps) {              \
        Decimate2T(x, h, y, n_out, taps);                                                          \
    }

PULP_SIMD_HWY_INSTANTIATE(F32, float)
PULP_SIMD_HWY_INSTANTIATE(F64, double)

#undef PULP_SIMD_HWY_INSTANTIATE

// ── complex kernels ──────────────────────────────────────────────────────────
// One body serves both layouts. For an interleaved buffer the load splits a
// vector's worth of complex values into real and imaginary registers and the
// store merges them again; for split buffers they are plain loads and stores.
// The scalar tail runs the strided reference loop on the remaining elements.

template <typename T, bool kInterleaved, class D, class V>
HWY_INLINE void LoadComplex(D d, const T* re, const T* im, size_t i, V& vr, V& vi) {
    if constexpr (kInterleaved) {
        (void)im;
        hn::LoadInterleaved2(d, re + 2 * i, vr, vi);
    } else {
        vr = hn::LoadU(d, re + i);
        vi = hn::LoadU(d, im + i);
    }
}

template <typename T, bool kInterleaved, class D, class V>
HWY_INLINE void StoreComplex(D d, V vr, V vi, T* re, T* im, size_t i) {
    if constexpr (kInterleaved) {
        (void)im;
        hn::StoreInterleaved2(vr, vi, d, re + 2 * i);
    } else {
        hn::StoreU(vr, d, re + i);
        hn::StoreU(vi, d, im + i);
    }
}

template <typename T, bool I> constexpr size_t Stride() {
    return I ? 2 : 1;
}

/// Imaginary pointer for the strided scalar reference: z + 1 when interleaved.
template <bool I, typename P> HWY_INLINE P ImagOf(P re, P im) {
    return I ? re + 1 : im;
}

template <typename T, bool I>
void ComplexMultiplyT(const T* ar, const T* ai, const T* br, const T* bi, T* dr, T* di,
                      size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        hn::Vec<decltype(d)> xr, xi, yr, yi;
        LoadComplex<T, I>(d, ar, ai, i, xr, xi);
        LoadComplex<T, I>(d, br, bi, i, yr, yi);
        const auto rr = hn::NegMulAdd(xi, yi, hn::Mul(xr, yr));
        const auto ri = hn::MulAdd(xi, yr, hn::Mul(xr, yi));
        StoreComplex<T, I>(d, rr, ri, dr, di, i);
    }
    constexpr size_t S = Stride<T, I>();
    ::pulp::simd::detail::complex_multiply_impl(ar + i * S, ImagOf<I>(ar, ai) + i * S, S,
                                                br + i * S, ImagOf<I>(br, bi) + i * S, S,
                                                dr + i * S, ImagOf<I>(dr, di) + i * S, S, count - i);
}

template <typename T, bool I>
void ComplexMultiplyConjT(const T* ar, const T* ai, const T* br, const T* bi, T* dr, T* di,
                          size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        hn::Vec<decltype(d)> xr, xi, yr, yi;
        LoadComplex<T, I>(d, ar, ai, i, xr, xi);
        LoadComplex<T, I>(d, br, bi, i, yr, yi);
        const auto rr = hn::MulAdd(xi, yi, hn::Mul(xr, yr));
        const auto ri = hn::NegMulAdd(xr, yi, hn::Mul(xi, yr));
        StoreComplex<T, I>(d, rr, ri, dr, di, i);
    }
    constexpr size_t S = Stride<T, I>();
    ::pulp::simd::detail::complex_multiply_conj_impl(
        ar + i * S, ImagOf<I>(ar, ai) + i * S, S, br + i * S, ImagOf<I>(br, bi) + i * S, S,
        dr + i * S, ImagOf<I>(dr, di) + i * S, S, count - i);
}

template <typename T, bool I>
void ComplexMultiplyAccumulateT(const T* ar, const T* ai, const T* br, const T* bi, T* cr, T* ci,
                                size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        hn::Vec<decltype(d)> xr, xi, yr, yi, cur, cui;
        LoadComplex<T, I>(d, ar, ai, i, xr, xi);
        LoadComplex<T, I>(d, br, bi, i, yr, yi);
        LoadComplex<T, I>(d, cr, ci, i, cur, cui);
        // acc.re += xr*yr - xi*yi ; acc.im += xr*yi + xi*yr
        cur = hn::NegMulAdd(xi, yi, hn::MulAdd(xr, yr, cur));
        cui = hn::MulAdd(xi, yr, hn::MulAdd(xr, yi, cui));
        StoreComplex<T, I>(d, cur, cui, cr, ci, i);
    }
    constexpr size_t S = Stride<T, I>();
    ::pulp::simd::detail::complex_multiply_accumulate_impl(
        ar + i * S, ImagOf<I>(ar, ai) + i * S, S, br + i * S, ImagOf<I>(br, bi) + i * S, S,
        cr + i * S, ImagOf<I>(cr, ci) + i * S, S, count - i);
}

template <typename T, bool I>
void ComplexMagnitudeSquaredT(const T* re, const T* im, T scale, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    const auto vs = hn::Set(d, scale);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        hn::Vec<decltype(d)> xr, xi;
        LoadComplex<T, I>(d, re, im, i, xr, xi);
        hn::StoreU(hn::Mul(hn::MulAdd(xr, xr, hn::Mul(xi, xi)), vs), d, dst + i);
    }
    constexpr size_t S = Stride<T, I>();
    ::pulp::simd::detail::complex_magnitude_squared_impl(re + i * S, ImagOf<I>(re, im) + i * S, S,
                                                         scale, dst + i, count - i);
}

template <typename T, bool I>
void ComplexMagnitudeT(const T* re, const T* im, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        hn::Vec<decltype(d)> xr, xi;
        LoadComplex<T, I>(d, re, im, i, xr, xi);
        hn::StoreU(hn::Sqrt(hn::MulAdd(xr, xr, hn::Mul(xi, xi))), d, dst + i);
    }
    constexpr size_t S = Stride<T, I>();
    ::pulp::simd::detail::complex_magnitude_impl(re + i * S, ImagOf<I>(re, im) + i * S, S, dst + i,
                                                 count - i);
}

template <typename T, bool I> void ComplexPhaseT(const T* re, const T* im, T* dst, size_t count) {
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        hn::Vec<decltype(d)> xr, xi;
        LoadComplex<T, I>(d, re, im, i, xr, xi);
        hn::StoreU(hn::Atan2(d, xi, xr), d, dst + i);
    }
    constexpr size_t S = Stride<T, I>();
    ::pulp::simd::detail::complex_phase_impl(re + i * S, ImagOf<I>(re, im) + i * S, S, dst + i,
                                             count - i);
}

#define PULP_SIMD_HWY_INSTANTIATE_COMPLEX(SUFFIX, T)                                               \
    void ComplexMultiply##SUFFIX(const T* a, const T* b, T* dst, size_t n) {                       \
        ComplexMultiplyT<T, true>(a, nullptr, b, nullptr, dst, nullptr, n);                        \
    }                                                                                              \
    void ComplexMultiplyConj##SUFFIX(const T* a, const T* b, T* dst, size_t n) {                   \
        ComplexMultiplyConjT<T, true>(a, nullptr, b, nullptr, dst, nullptr, n);                    \
    }                                                                                              \
    void ComplexMultiplyAccumulate##SUFFIX(const T* a, const T* b, T* acc, size_t n) {             \
        ComplexMultiplyAccumulateT<T, true>(a, nullptr, b, nullptr, acc, nullptr, n);              \
    }                                                                                              \
    void ComplexMagnitude##SUFFIX(const T* z, T* dst, size_t n) {                                  \
        ComplexMagnitudeT<T, true>(z, nullptr, dst, n);                                            \
    }                                                                                              \
    void ComplexMagnitudeSquared##SUFFIX(const T* z, T scale, T* dst, size_t n) {                  \
        ComplexMagnitudeSquaredT<T, true>(z, nullptr, scale, dst, n);                              \
    }                                                                                              \
    void ComplexPhase##SUFFIX(const T* z, T* dst, size_t n) {                                      \
        ComplexPhaseT<T, true>(z, nullptr, dst, n);                                                \
    }                                                                                              \
    void ComplexMultiplySplit##SUFFIX(const T* ar, const T* ai, const T* br, const T* bi, T* dr,   \
                                      T* di, size_t n) {                                           \
        ComplexMultiplyT<T, false>(ar, ai, br, bi, dr, di, n);                                     \
    }                                                                                              \
    void ComplexMultiplyConjSplit##SUFFIX(const T* ar, const T* ai, const T* br, const T* bi,      \
                                          T* dr, T* di, size_t n) {                                \
        ComplexMultiplyConjT<T, false>(ar, ai, br, bi, dr, di, n);                                 \
    }                                                                                              \
    void ComplexMultiplyAccumulateSplit##SUFFIX(const T* ar, const T* ai, const T* br,             \
                                                const T* bi, T* cr, T* ci, size_t n) {             \
        ComplexMultiplyAccumulateT<T, false>(ar, ai, br, bi, cr, ci, n);                           \
    }                                                                                              \
    void ComplexMagnitudeSplit##SUFFIX(const T* re, const T* im, T* dst, size_t n) {               \
        ComplexMagnitudeT<T, false>(re, im, dst, n);                                               \
    }                                                                                              \
    void ComplexMagnitudeSquaredSplit##SUFFIX(const T* re, const T* im, T scale, T* dst,           \
                                              size_t n) {                                          \
        ComplexMagnitudeSquaredT<T, false>(re, im, scale, dst, n);                                 \
    }                                                                                              \
    void ComplexPhaseSplit##SUFFIX(const T* re, const T* im, T* dst, size_t n) {                   \
        ComplexPhaseT<T, false>(re, im, dst, n);                                                   \
    }

PULP_SIMD_HWY_INSTANTIATE_COMPLEX(F32, float)
PULP_SIMD_HWY_INSTANTIATE_COMPLEX(F64, double)

#undef PULP_SIMD_HWY_INSTANTIATE_COMPLEX

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
    HWY_EXPORT(Sub##SUFFIX);                                                            \
    HWY_EXPORT(Div##SUFFIX);                                                            \
    HWY_EXPORT(Negate##SUFFIX);                                                         \
    HWY_EXPORT(FindMaximum##SUFFIX);                                                    \
    HWY_EXPORT(FindMinimum##SUFFIX);                                                    \
    HWY_EXPORT(ReplaceBelow##SUFFIX);                                                   \
    HWY_EXPORT(ReplaceAbove##SUFFIX);                                                   \
    HWY_EXPORT(FromInt16##SUFFIX);                                                      \
    HWY_EXPORT(FromInt32##SUFFIX);                                                      \
    HWY_EXPORT(ToInt16##SUFFIX);                                                        \
    HWY_EXPORT(ToInt32##SUFFIX);                                                        \
    HWY_EXPORT(Interleave2##SUFFIX);                                                    \
    HWY_EXPORT(Deinterleave2##SUFFIX);                                                  \
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
    void sub(const T* a, const T* b, T* dst, std::size_t n) noexcept {                             \
        HWY_DYNAMIC_DISPATCH(Sub##SUFFIX)(a, b, dst, n);                                           \
    }                                                                                              \
    void div(const T* a, const T* b, T* dst, std::size_t n) noexcept {                             \
        HWY_DYNAMIC_DISPATCH(Div##SUFFIX)(a, b, dst, n);                                           \
    }                                                                                              \
    void negate(const T* a, T* dst, std::size_t n) noexcept {                                      \
        HWY_DYNAMIC_DISPATCH(Negate##SUFFIX)(a, dst, n);                                           \
    }                                                                                              \
    T mean(const T* x, std::size_t n) noexcept {                                                   \
        return n == 0 ? T(0) : sum(x, n) / T(n);                                                   \
    }                                                                                              \
    T rms(const T* x, std::size_t n) noexcept {                                                    \
        return n == 0 ? T(0) : std::sqrt(sum_squares(x, n) / T(n));                                \
    }                                                                                              \
    T find_maximum(const T* x, std::size_t n, std::size_t* index) noexcept {                       \
        return HWY_DYNAMIC_DISPATCH(FindMaximum##SUFFIX)(x, n, index);                             \
    }                                                                                              \
    T find_minimum(const T* x, std::size_t n, std::size_t* index) noexcept {                       \
        return HWY_DYNAMIC_DISPATCH(FindMinimum##SUFFIX)(x, n, index);                             \
    }                                                                                              \
    void replace_below(const T* a, T threshold, T value, T* dst, std::size_t n) noexcept {         \
        HWY_DYNAMIC_DISPATCH(ReplaceBelow##SUFFIX)(a, threshold, value, dst, n);                   \
    }                                                                                              \
    void replace_above(const T* a, T threshold, T value, T* dst, std::size_t n) noexcept {         \
        HWY_DYNAMIC_DISPATCH(ReplaceAbove##SUFFIX)(a, threshold, value, dst, n);                   \
    }                                                                                              \
    void from_int16(const std::int16_t* src, T scale, T* dst, std::size_t n) noexcept {            \
        HWY_DYNAMIC_DISPATCH(FromInt16##SUFFIX)(src, scale, dst, n);                               \
    }                                                                                              \
    void from_int32(const std::int32_t* src, T scale, T* dst, std::size_t n) noexcept {            \
        HWY_DYNAMIC_DISPATCH(FromInt32##SUFFIX)(src, scale, dst, n);                               \
    }                                                                                              \
    void to_int16(const T* src, T scale, std::int16_t* dst, std::size_t n) noexcept {              \
        HWY_DYNAMIC_DISPATCH(ToInt16##SUFFIX)(src, scale, dst, n);                                 \
    }                                                                                              \
    void to_int32(const T* src, T scale, std::int32_t* dst, std::size_t n) noexcept {              \
        HWY_DYNAMIC_DISPATCH(ToInt32##SUFFIX)(src, scale, dst, n);                                 \
    }                                                                                              \
    void interleave2(const T* l, const T* r, T* dst, std::size_t n) noexcept {                     \
        HWY_DYNAMIC_DISPATCH(Interleave2##SUFFIX)(l, r, dst, n);                                   \
    }                                                                                              \
    void deinterleave2(const T* src, T* l, T* r, std::size_t n) noexcept {                         \
        HWY_DYNAMIC_DISPATCH(Deinterleave2##SUFFIX)(src, l, r, n);                                 \
    }                                                                                              \
    void decimate2(const T* x, const T* h, T* y, std::size_t n_out, std::size_t taps) noexcept {   \
        HWY_DYNAMIC_DISPATCH(Decimate2##SUFFIX)(x, h, y, n_out, taps);                             \
    }

PULP_SIMD_HWY_DEFINE(F32, float)
PULP_SIMD_HWY_DEFINE(F64, double)

#undef PULP_SIMD_HWY_DEFINE

#define PULP_SIMD_HWY_EXPORT_COMPLEX(SUFFIX) \
    HWY_EXPORT(ComplexMultiply##SUFFIX); \
    HWY_EXPORT(ComplexMultiplyConj##SUFFIX); \
    HWY_EXPORT(ComplexMultiplyAccumulate##SUFFIX); \
    HWY_EXPORT(ComplexMagnitude##SUFFIX); \
    HWY_EXPORT(ComplexMagnitudeSquared##SUFFIX); \
    HWY_EXPORT(ComplexPhase##SUFFIX); \
    HWY_EXPORT(ComplexMultiplySplit##SUFFIX); \
    HWY_EXPORT(ComplexMultiplyConjSplit##SUFFIX); \
    HWY_EXPORT(ComplexMultiplyAccumulateSplit##SUFFIX); \
    HWY_EXPORT(ComplexMagnitudeSplit##SUFFIX); \
    HWY_EXPORT(ComplexMagnitudeSquaredSplit##SUFFIX); \
    HWY_EXPORT(ComplexPhaseSplit##SUFFIX);

PULP_SIMD_HWY_EXPORT_COMPLEX(F32)
PULP_SIMD_HWY_EXPORT_COMPLEX(F64)
#undef PULP_SIMD_HWY_EXPORT_COMPLEX

#define PULP_SIMD_HWY_DEFINE_COMPLEX(SUFFIX, T)                                                    \
    void complex_multiply(const T* a, const T* b, T* dst, std::size_t n) noexcept {                \
        HWY_DYNAMIC_DISPATCH(ComplexMultiply##SUFFIX)(a, b, dst, n);                               \
    }                                                                                              \
    void complex_multiply_conj(const T* a, const T* b, T* dst, std::size_t n) noexcept {           \
        HWY_DYNAMIC_DISPATCH(ComplexMultiplyConj##SUFFIX)(a, b, dst, n);                           \
    }                                                                                              \
    void complex_multiply_accumulate(const T* a, const T* b, T* acc, std::size_t n) noexcept {     \
        HWY_DYNAMIC_DISPATCH(ComplexMultiplyAccumulate##SUFFIX)(a, b, acc, n);                     \
    }                                                                                              \
    void complex_magnitude(const T* z, T* dst, std::size_t n) noexcept {                           \
        HWY_DYNAMIC_DISPATCH(ComplexMagnitude##SUFFIX)(z, dst, n);                                 \
    }                                                                                              \
    void complex_magnitude_squared(const T* z, T* dst, std::size_t n) noexcept {                   \
        HWY_DYNAMIC_DISPATCH(ComplexMagnitudeSquared##SUFFIX)(z, T(1), dst, n);                    \
    }                                                                                              \
    void complex_phase(const T* z, T* dst, std::size_t n) noexcept {                               \
        HWY_DYNAMIC_DISPATCH(ComplexPhase##SUFFIX)(z, dst, n);                                     \
    }                                                                                              \
    void complex_power_spectrum(const T* z, T scale, T* dst, std::size_t n) noexcept {             \
        HWY_DYNAMIC_DISPATCH(ComplexMagnitudeSquared##SUFFIX)(z, scale, dst, n);                   \
    }                                                                                              \
    void complex_multiply_split(const T* a_re, const T* a_im, const T* b_re, const T* b_im,        \
                                T* dst_re, T* dst_im, std::size_t n) noexcept {                    \
        HWY_DYNAMIC_DISPATCH(ComplexMultiplySplit##SUFFIX)(a_re, a_im, b_re, b_im, dst_re, dst_im, \
                                                           n);                                     \
    }                                                                                              \
    void complex_multiply_conj_split(const T* a_re, const T* a_im, const T* b_re, const T* b_im,   \
                                     T* dst_re, T* dst_im, std::size_t n) noexcept {               \
        HWY_DYNAMIC_DISPATCH(ComplexMultiplyConjSplit##SUFFIX)(a_re, a_im, b_re, b_im, dst_re,     \
                                                               dst_im, n);                         \
    }                                                                                              \
    void complex_multiply_accumulate_split(const T* a_re, const T* a_im, const T* b_re,            \
                                           const T* b_im, T* acc_re, T* acc_im,                    \
                                           std::size_t n) noexcept {                               \
        HWY_DYNAMIC_DISPATCH(ComplexMultiplyAccumulateSplit##SUFFIX)(a_re, a_im, b_re, b_im,       \
                                                                     acc_re, acc_im, n);           \
    }                                                                                              \
    void complex_magnitude_split(const T* re, const T* im, T* dst, std::size_t n) noexcept {       \
        HWY_DYNAMIC_DISPATCH(ComplexMagnitudeSplit##SUFFIX)(re, im, dst, n);                       \
    }                                                                                              \
    void complex_magnitude_squared_split(const T* re, const T* im, T* dst,                         \
                                         std::size_t n) noexcept {                                 \
        HWY_DYNAMIC_DISPATCH(ComplexMagnitudeSquaredSplit##SUFFIX)(re, im, T(1), dst, n);          \
    }                                                                                              \
    void complex_phase_split(const T* re, const T* im, T* dst, std::size_t n) noexcept {           \
        HWY_DYNAMIC_DISPATCH(ComplexPhaseSplit##SUFFIX)(re, im, dst, n);                           \
    }                                                                                              \
    void complex_power_spectrum_split(const T* re, const T* im, T scale, T* dst,                   \
                                      std::size_t n) noexcept {                                    \
        HWY_DYNAMIC_DISPATCH(ComplexMagnitudeSquaredSplit##SUFFIX)(re, im, scale, dst, n);         \
    }

PULP_SIMD_HWY_DEFINE_COMPLEX(F32, float)
PULP_SIMD_HWY_DEFINE_COMPLEX(F64, double)

#undef PULP_SIMD_HWY_DEFINE_COMPLEX

// HWY_DYNAMIC_DISPATCH expands its argument relative to the generated target
// namespace. Keep these calls inside the backend namespace so the single-target
// Highway configuration used by Windows ARM64 resolves the target correctly.
std::size_t dispatch_float_lanes() noexcept {
    return HWY_DYNAMIC_DISPATCH(FloatLanes)();
}

std::size_t dispatch_double_lanes() noexcept {
    return HWY_DYNAMIC_DISPATCH(DoubleLanes)();
}

} // namespace pulp::simd::backend::highway

namespace pulp::simd {

std::size_t float_lanes() noexcept {
    return backend::highway::dispatch_float_lanes();
}

std::size_t double_lanes() noexcept {
    return backend::highway::dispatch_double_lanes();
}

} // namespace pulp::simd

#endif // HWY_ONCE
