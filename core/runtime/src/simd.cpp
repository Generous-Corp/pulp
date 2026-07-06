// SIMD operations — Highway dynamic dispatch implementation
// This file uses Highway's foreach_target mechanism to generate code
// for multiple SIMD instruction sets, dispatching at runtime to the best one.

#include <pulp/runtime/simd.hpp>
#include <cmath>
#include <algorithm>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "simd.cpp"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();

namespace pulp::runtime {
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

void AddF32(const float* a, const float* b, float* dst, size_t count) {
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        auto vb = hn::LoadU(d, b + i);
        hn::StoreU(hn::Add(va, vb), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = a[i] + b[i];
}

void MulF32(const float* HWY_RESTRICT a, const float* HWY_RESTRICT b,
            float* HWY_RESTRICT dst, size_t count) {
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        auto vb = hn::LoadU(d, b + i);
        hn::StoreU(hn::Mul(va, vb), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = a[i] * b[i];
}

void FmaF32(const float* HWY_RESTRICT a, const float* HWY_RESTRICT b,
            const float* HWY_RESTRICT c, float* HWY_RESTRICT dst,
            size_t count) {
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        auto vb = hn::LoadU(d, b + i);
        auto vc = hn::LoadU(d, c + i);
        hn::StoreU(hn::MulAdd(va, vb, vc), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = a[i] * b[i] + c[i];
}

void SetF32(float value, float* HWY_RESTRICT dst, size_t count) {
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    auto vval = hn::Set(d, value);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(vval, d, dst + i);
    for (; i < count; ++i)
        dst[i] = value;
}

void ScaleF32(const float* HWY_RESTRICT a, float scalar,
              float* HWY_RESTRICT dst, size_t count) {
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    auto vscalar = hn::Set(d, scalar);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        hn::StoreU(hn::Mul(va, vscalar), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = a[i] * scalar;
}

void AddScaledF32(const float* a, float scalar, float* dst, size_t count) {
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    auto vscalar = hn::Set(d, scalar);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        auto vd = hn::LoadU(d, dst + i);
        hn::StoreU(hn::MulAdd(va, vscalar, vd), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] += a[i] * scalar;
}

float ReduceAddF32(const float* HWY_RESTRICT data, size_t count) {
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    auto vsum = hn::Zero(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto v = hn::LoadU(d, data + i);
        vsum = hn::Add(vsum, v);
    }
    float sum = hn::ReduceSum(d, vsum);
    for (; i < count; ++i)
        sum += data[i];
    return sum;
}

float ReduceMaxF32(const float* HWY_RESTRICT data, size_t count) {
    if (count == 0) return 0.0f;
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    auto vmax = hn::Set(d, data[0]);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto v = hn::LoadU(d, data + i);
        vmax = hn::Max(vmax, v);
    }
    float result = hn::ReduceMax(d, vmax);
    for (; i < count; ++i)
        result = std::max(result, data[i]);
    return result;
}

float ReduceMinF32(const float* HWY_RESTRICT data, size_t count) {
    if (count == 0) return 0.0f;
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    auto vmin = hn::Set(d, data[0]);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto v = hn::LoadU(d, data + i);
        vmin = hn::Min(vmin, v);
    }
    float result = hn::ReduceMin(d, vmin);
    for (; i < count; ++i)
        result = std::min(result, data[i]);
    return result;
}

void AbsF32(const float* HWY_RESTRICT a, float* HWY_RESTRICT dst,
            size_t count) {
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        hn::StoreU(hn::Abs(va), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = std::abs(a[i]);
}

void ClampF32(const float* HWY_RESTRICT a, float lo, float hi,
              float* HWY_RESTRICT dst, size_t count) {
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    auto vlo = hn::Set(d, lo);
    auto vhi = hn::Set(d, hi);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        hn::StoreU(hn::Clamp(va, vlo, vhi), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = std::clamp(a[i], lo, hi);
}

void AddF64(const double* a, const double* b, double* dst, size_t count) {
    const hn::ScalableTag<double> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        auto vb = hn::LoadU(d, b + i);
        hn::StoreU(hn::Add(va, vb), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = a[i] + b[i];
}

void MulF64(const double* HWY_RESTRICT a, const double* HWY_RESTRICT b,
            double* HWY_RESTRICT dst, size_t count) {
    const hn::ScalableTag<double> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        auto vb = hn::LoadU(d, b + i);
        hn::StoreU(hn::Mul(va, vb), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = a[i] * b[i];
}

void FmaF64(const double* HWY_RESTRICT a, const double* HWY_RESTRICT b,
            const double* HWY_RESTRICT c, double* HWY_RESTRICT dst,
            size_t count) {
    const hn::ScalableTag<double> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        auto vb = hn::LoadU(d, b + i);
        auto vc = hn::LoadU(d, c + i);
        hn::StoreU(hn::MulAdd(va, vb, vc), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = a[i] * b[i] + c[i];
}

void SetF64(double value, double* HWY_RESTRICT dst, size_t count) {
    const hn::ScalableTag<double> d;
    const size_t N = hn::Lanes(d);
    auto vval = hn::Set(d, value);
    size_t i = 0;
    for (; i + N <= count; i += N)
        hn::StoreU(vval, d, dst + i);
    for (; i < count; ++i)
        dst[i] = value;
}

void ScaleF64(const double* HWY_RESTRICT a, double scalar,
              double* HWY_RESTRICT dst, size_t count) {
    const hn::ScalableTag<double> d;
    const size_t N = hn::Lanes(d);
    auto vscalar = hn::Set(d, scalar);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        hn::StoreU(hn::Mul(va, vscalar), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = a[i] * scalar;
}

void AddScaledF64(const double* a, double scalar, double* dst, size_t count) {
    const hn::ScalableTag<double> d;
    const size_t N = hn::Lanes(d);
    auto vscalar = hn::Set(d, scalar);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        auto vd = hn::LoadU(d, dst + i);
        hn::StoreU(hn::MulAdd(va, vscalar, vd), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] += a[i] * scalar;
}

double ReduceAddF64(const double* HWY_RESTRICT data, size_t count) {
    const hn::ScalableTag<double> d;
    const size_t N = hn::Lanes(d);
    auto vsum = hn::Zero(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto v = hn::LoadU(d, data + i);
        vsum = hn::Add(vsum, v);
    }
    double sum = hn::ReduceSum(d, vsum);
    for (; i < count; ++i)
        sum += data[i];
    return sum;
}

double ReduceMaxF64(const double* HWY_RESTRICT data, size_t count) {
    if (count == 0) return 0.0;
    const hn::ScalableTag<double> d;
    const size_t N = hn::Lanes(d);
    auto vmax = hn::Set(d, data[0]);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto v = hn::LoadU(d, data + i);
        vmax = hn::Max(vmax, v);
    }
    double result = hn::ReduceMax(d, vmax);
    for (; i < count; ++i)
        result = std::max(result, data[i]);
    return result;
}

double ReduceMinF64(const double* HWY_RESTRICT data, size_t count) {
    if (count == 0) return 0.0;
    const hn::ScalableTag<double> d;
    const size_t N = hn::Lanes(d);
    auto vmin = hn::Set(d, data[0]);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto v = hn::LoadU(d, data + i);
        vmin = hn::Min(vmin, v);
    }
    double result = hn::ReduceMin(d, vmin);
    for (; i < count; ++i)
        result = std::min(result, data[i]);
    return result;
}

void AbsF64(const double* HWY_RESTRICT a, double* HWY_RESTRICT dst,
            size_t count) {
    const hn::ScalableTag<double> d;
    const size_t N = hn::Lanes(d);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        hn::StoreU(hn::Abs(va), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = std::abs(a[i]);
}

void ClampF64(const double* HWY_RESTRICT a, double lo, double hi,
              double* HWY_RESTRICT dst, size_t count) {
    const hn::ScalableTag<double> d;
    const size_t N = hn::Lanes(d);
    auto vlo = hn::Set(d, lo);
    auto vhi = hn::Set(d, hi);
    size_t i = 0;
    for (; i + N <= count; i += N) {
        auto va = hn::LoadU(d, a + i);
        hn::StoreU(hn::Clamp(va, vlo, vhi), d, dst + i);
    }
    for (; i < count; ++i)
        dst[i] = std::clamp(a[i], lo, hi);
}

}  // namespace HWY_NAMESPACE
}  // namespace pulp::runtime

HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace pulp::runtime {

HWY_EXPORT(FloatLanes);
HWY_EXPORT(DoubleLanes);
HWY_EXPORT(AddF32);
HWY_EXPORT(MulF32);
HWY_EXPORT(FmaF32);
HWY_EXPORT(SetF32);
HWY_EXPORT(ScaleF32);
HWY_EXPORT(AddScaledF32);
HWY_EXPORT(ReduceAddF32);
HWY_EXPORT(ReduceMaxF32);
HWY_EXPORT(ReduceMinF32);
HWY_EXPORT(AbsF32);
HWY_EXPORT(ClampF32);
HWY_EXPORT(AddF64);
HWY_EXPORT(MulF64);
HWY_EXPORT(FmaF64);
HWY_EXPORT(SetF64);
HWY_EXPORT(ScaleF64);
HWY_EXPORT(AddScaledF64);
HWY_EXPORT(ReduceAddF64);
HWY_EXPORT(ReduceMaxF64);
HWY_EXPORT(ReduceMinF64);
HWY_EXPORT(AbsF64);
HWY_EXPORT(ClampF64);

size_t simd_float_lanes() {
    return HWY_DYNAMIC_DISPATCH(FloatLanes)();
}

size_t simd_double_lanes() {
    return HWY_DYNAMIC_DISPATCH(DoubleLanes)();
}

void simd_add(const float* a, const float* b, float* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(AddF32)(a, b, dst, count);
}

void simd_add(const double* a, const double* b, double* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(AddF64)(a, b, dst, count);
}

void simd_mul(const float* a, const float* b, float* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(MulF32)(a, b, dst, count);
}

void simd_mul(const double* a, const double* b, double* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(MulF64)(a, b, dst, count);
}

void simd_fma(const float* a, const float* b, const float* c, float* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(FmaF32)(a, b, c, dst, count);
}

void simd_fma(const double* a,
              const double* b,
              const double* c,
              double* dst,
              size_t count) {
    HWY_DYNAMIC_DISPATCH(FmaF64)(a, b, c, dst, count);
}

void simd_set(float value, float* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(SetF32)(value, dst, count);
}

void simd_set(double value, double* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(SetF64)(value, dst, count);
}

void simd_scale(const float* a, float scalar, float* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(ScaleF32)(a, scalar, dst, count);
}

void simd_scale(const double* a, double scalar, double* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(ScaleF64)(a, scalar, dst, count);
}

void simd_add_scaled(const float* a, float scalar, float* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(AddScaledF32)(a, scalar, dst, count);
}

void simd_add_scaled(const double* a, double scalar, double* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(AddScaledF64)(a, scalar, dst, count);
}

float simd_reduce_add(const float* data, size_t count) {
    return HWY_DYNAMIC_DISPATCH(ReduceAddF32)(data, count);
}

double simd_reduce_add(const double* data, size_t count) {
    return HWY_DYNAMIC_DISPATCH(ReduceAddF64)(data, count);
}

float simd_reduce_max(const float* data, size_t count) {
    return HWY_DYNAMIC_DISPATCH(ReduceMaxF32)(data, count);
}

double simd_reduce_max(const double* data, size_t count) {
    return HWY_DYNAMIC_DISPATCH(ReduceMaxF64)(data, count);
}

float simd_reduce_min(const float* data, size_t count) {
    return HWY_DYNAMIC_DISPATCH(ReduceMinF32)(data, count);
}

double simd_reduce_min(const double* data, size_t count) {
    return HWY_DYNAMIC_DISPATCH(ReduceMinF64)(data, count);
}

void simd_abs(const float* a, float* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(AbsF32)(a, dst, count);
}

void simd_abs(const double* a, double* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(AbsF64)(a, dst, count);
}

void simd_clamp(const float* a, float lo, float hi, float* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(ClampF32)(a, lo, hi, dst, count);
}

void simd_clamp(const double* a, double lo, double hi, double* dst, size_t count) {
    HWY_DYNAMIC_DISPATCH(ClampF64)(a, lo, hi, dst, count);
}

}  // namespace pulp::runtime

#endif  // HWY_ONCE
