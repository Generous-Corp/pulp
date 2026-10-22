#pragma once

// Private four-lane SIMD layer for the neural CPU kernels.
//
// Every kernel is written once over an `Isa` policy and instantiated twice:
// `ScalarIsa` (plain arrays of four floats; builds everywhere) and, on arm64,
// `NeonIsa`. Both perform the same operations in the same order per output
// lane, and every kernel multiply-add is spelled explicitly (madd) rather
// than left to compiler contraction, so on FMA targets the two agree to the
// bit outside tanh. The scalar tanh is signal::FastMath::tanh_accurate as
// the compiler contracts it; the NEON lane fuses the same multiply-adds.
// `DefaultIsa` is what production paths use.
//
// Realtime: nothing here allocates, locks, or calls into the platform.
//
// Inputs are finite by contract: the realtime node sanitizes non-finite
// samples before they reach a CPU model. NaN still propagates (tanh_accurate
// keeps it), so a contract breach is visible rather than masked; debug
// builds assert on it at each kernel's entry.

#include <pulp/signal/fast_math.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(PULP_NEURAL_SIMD_FORCE_SCALAR)
#include <arm_neon.h>
#define PULP_NEURAL_SIMD_HAS_NEON 1
#else
#define PULP_NEURAL_SIMD_HAS_NEON 0
#endif

namespace pulp::gpu_audio::detail::neural_simd {

inline constexpr std::uint32_t kLanes = 4;

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__FMA__)
/// c + a * b, fused (one instruction on these targets).
inline float madd(float c, float a, float b) noexcept {
    return std::fma(a, b, c);
}
#else
/// c + a * b. Without hardware FMA, std::fma would be a library call.
inline float madd(float c, float a, float b) noexcept {
    return c + a * b;
}
#endif

constexpr std::uint32_t pad_lanes(std::uint32_t count) noexcept {
    return (count + kLanes - 1) / kLanes * kLanes;
}

/// signal::FastMath::tanh_accurate: within 3.0e-7 of std::tanh, bounded to
/// [-1, 1], odd, NaN-propagating. That error budget is what lets a block
/// kernel null against std::tanh at 1e-6.
inline float tanh_bounded(float x) noexcept {
    return signal::FastMath::tanh_accurate(x);
}

/// Logistic sigmoid through the tanh identity, within 1.5e-7 of the exact
/// value.
inline float sigmoid_bounded(float x) noexcept {
    return madd(0.5f, 0.5f, tanh_bounded(0.5f * x));
}

/// Debug-only check of the finite-input contract.
inline void assert_finite([[maybe_unused]] const float* x,
                          [[maybe_unused]] std::uint32_t n) noexcept {
#ifndef NDEBUG
    for (std::uint32_t i = 0; i < n; ++i)
        assert(std::isfinite(x[i]) && "neural CPU kernel input must be finite");
#endif
}

/// Portable four-lane policy. Lane loops are fixed-trip, so optimizers are
/// free to vectorize them on any target, including x86.
struct ScalarIsa {
    struct V {
        float l[kLanes];
    };
    static constexpr const char* name = "scalar";

    static V load(const float* p) noexcept {
        return {{p[0], p[1], p[2], p[3]}};
    }
    static void store(float* p, V v) noexcept {
        for (std::uint32_t i = 0; i < kLanes; ++i)
            p[i] = v.l[i];
    }
    static V splat(float s) noexcept {
        return {{s, s, s, s}};
    }
    static V add(V a, V b) noexcept {
        for (std::uint32_t i = 0; i < kLanes; ++i)
            a.l[i] += b.l[i];
        return a;
    }
    static V sub(V a, V b) noexcept {
        for (std::uint32_t i = 0; i < kLanes; ++i)
            a.l[i] -= b.l[i];
        return a;
    }
    static V mul(V a, V b) noexcept {
        for (std::uint32_t i = 0; i < kLanes; ++i)
            a.l[i] *= b.l[i];
        return a;
    }
    /// acc + a * b per lane.
    static V fma(V acc, V a, V b) noexcept {
        for (std::uint32_t i = 0; i < kLanes; ++i)
            acc.l[i] = madd(acc.l[i], a.l[i], b.l[i]);
        return acc;
    }
    /// acc + w * x[Lane] per lane of w.
    template <int Lane> static V fma_lane(V acc, V w, V x) noexcept {
        for (std::uint32_t i = 0; i < kLanes; ++i)
            acc.l[i] = madd(acc.l[i], w.l[i], x.l[Lane]);
        return acc;
    }
    static V max(V a, V b) noexcept {
        for (std::uint32_t i = 0; i < kLanes; ++i)
            a.l[i] = std::fmax(a.l[i], b.l[i]);
        return a;
    }
    static V min(V a, V b) noexcept {
        for (std::uint32_t i = 0; i < kLanes; ++i)
            a.l[i] = std::fmin(a.l[i], b.l[i]);
        return a;
    }
    static V tanh(V v) noexcept {
        for (std::uint32_t i = 0; i < kLanes; ++i)
            v.l[i] = tanh_bounded(v.l[i]);
        return v;
    }
    static V sigmoid(V v) noexcept {
        for (std::uint32_t i = 0; i < kLanes; ++i)
            v.l[i] = sigmoid_bounded(v.l[i]);
        return v;
    }
    /// ((l0 + l1) + (l2 + l3)), the pairwise order NEON's vaddvq uses.
    static float hsum(V v) noexcept {
        return (v.l[0] + v.l[1]) + (v.l[2] + v.l[3]);
    }
};

#if PULP_NEURAL_SIMD_HAS_NEON
struct NeonIsa {
    using V = float32x4_t;
    static constexpr const char* name = "neon";

    static V load(const float* p) noexcept {
        return vld1q_f32(p);
    }
    static void store(float* p, V v) noexcept {
        vst1q_f32(p, v);
    }
    static V splat(float s) noexcept {
        return vdupq_n_f32(s);
    }
    static V add(V a, V b) noexcept {
        return vaddq_f32(a, b);
    }
    static V sub(V a, V b) noexcept {
        return vsubq_f32(a, b);
    }
    static V mul(V a, V b) noexcept {
        return vmulq_f32(a, b);
    }
    static V fma(V acc, V a, V b) noexcept {
        return vfmaq_f32(acc, a, b);
    }
    template <int Lane> static V fma_lane(V acc, V w, V x) noexcept {
        return vfmaq_laneq_f32(acc, w, x, Lane);
    }
    // minNum/maxNum semantics match std::fmin/std::fmax: a NaN operand
    // yields the other operand.
    static V max(V a, V b) noexcept {
        return vmaxnmq_f32(a, b);
    }
    static V min(V a, V b) noexcept {
        return vminnmq_f32(a, b);
    }
    static V tanh(V x) noexcept {
        const V u = vminnmq_f32(vmulq_f32(x, x), vdupq_n_f32(81.0f));
        const V u2 = vmulq_f32(u, u);
        // The rational of FastMath::tanh_accurate_rational, with each
        // multiply-add fused as -ffp-contract=on fuses the scalar form.
        const V p =
            vfmaq_f32(vfmaq_n_f32(vdupq_n_f32(1.0f), u, 0.133803148f), u2,
                      vfmaq_n_f32(vfmaq_n_f32(vdupq_n_f32(0.00349475497f), u, 2.05960245e-05f), u2,
                                  1.33379685e-08f));
        const V q =
            vfmaq_f32(vfmaq_n_f32(vdupq_n_f32(1.0f), u, 0.467136322f), u2,
                      vfmaq_n_f32(vfmaq_n_f32(vdupq_n_f32(0.0258737793f), u, 0.000328429886f), u2,
                                  7.7694232e-07f));
        const V r = vdivq_f32(vmulq_f32(x, p), q);
        // Ordered compares, as tanh_accurate: a NaN fails both and survives.
        const V one = vdupq_n_f32(1.0f), minus_one = vdupq_n_f32(-1.0f);
        const V upper = vbslq_f32(vcgtq_f32(r, one), one, r);
        return vbslq_f32(vcltq_f32(upper, minus_one), minus_one, upper);
    }
    static V sigmoid(V x) noexcept {
        const V half = vdupq_n_f32(0.5f);
        return vfmaq_f32(half, half, tanh(vmulq_f32(half, x)));
    }
    static float hsum(V v) noexcept {
        return vaddvq_f32(v);
    }
};
using DefaultIsa = NeonIsa;
#else
using DefaultIsa = ScalarIsa;
#endif

/// Dense, optionally dilated, multi-tap matrix product over rows of activations:
///
///   out[f][o] = init(f, o) + sum_{k < taps} sum_{i < in_pad} w[k][i][o] * row(f, k)[i]
///
/// `w` is laid out [taps][in_pad][out_pad] so one weight vector serves four
/// output channels; inputs are broadcast from a lane. Frames are tiled so
/// each tile keeps up to 16 independent accumulators in flight, which hides
/// FMA latency. Per output lane the accumulation order is always k, then i,
/// independent of the tiling, so results do not depend on how a block is
/// partitioned.
///
///   row(f, k)          -> const float* (in_pad floats, 16-byte multiple)
///   init(f, col)       -> Isa::V for output lanes [col, col + 4)
///   store(f, col, V)   consumes the finished lanes
template <class Isa, int FB, int VB, class Row, class Init, class Store>
inline void dense_tile(const float* w, std::uint32_t taps, std::uint32_t in_pad,
                       std::uint32_t out_pad, std::uint32_t f0, std::uint32_t col, Row& row,
                       Init& init, Store& store) noexcept {
    using V = typename Isa::V;
    V acc[FB][VB];
    for (int f = 0; f < FB; ++f)
        for (int v = 0; v < VB; ++v)
            acc[f][v] = init(f0 + f, col + 4u * v);
    for (std::uint32_t k = 0; k < taps; ++k) {
        const float* r[FB];
        for (int f = 0; f < FB; ++f)
            r[f] = row(f0 + f, k);
        const float* wk = w + static_cast<std::size_t>(k) * in_pad * out_pad + col;
        for (std::uint32_t i = 0; i < in_pad; i += kLanes) {
            V x[FB];
            for (int f = 0; f < FB; ++f)
                x[f] = Isa::load(r[f] + i);
            const float* wi = wk + static_cast<std::size_t>(i) * out_pad;
            V wv[VB];
            for (int v = 0; v < VB; ++v)
                wv[v] = Isa::load(wi + 4 * v);
            for (int f = 0; f < FB; ++f)
                for (int v = 0; v < VB; ++v)
                    acc[f][v] = Isa::template fma_lane<0>(acc[f][v], wv[v], x[f]);
            for (int v = 0; v < VB; ++v)
                wv[v] = Isa::load(wi + out_pad + 4 * v);
            for (int f = 0; f < FB; ++f)
                for (int v = 0; v < VB; ++v)
                    acc[f][v] = Isa::template fma_lane<1>(acc[f][v], wv[v], x[f]);
            for (int v = 0; v < VB; ++v)
                wv[v] = Isa::load(wi + 2 * out_pad + 4 * v);
            for (int f = 0; f < FB; ++f)
                for (int v = 0; v < VB; ++v)
                    acc[f][v] = Isa::template fma_lane<2>(acc[f][v], wv[v], x[f]);
            for (int v = 0; v < VB; ++v)
                wv[v] = Isa::load(wi + 3 * out_pad + 4 * v);
            for (int f = 0; f < FB; ++f)
                for (int v = 0; v < VB; ++v)
                    acc[f][v] = Isa::template fma_lane<3>(acc[f][v], wv[v], x[f]);
        }
    }
    for (int f = 0; f < FB; ++f)
        for (int v = 0; v < VB; ++v)
            store(f0 + f, col + 4u * v, acc[f][v]);
}

template <class Isa, int VB, class Row, class Init, class Store>
inline void dense_columns(const float* w, std::uint32_t taps, std::uint32_t in_pad,
                          std::uint32_t out_pad, std::uint32_t frames, std::uint32_t col, Row& row,
                          Init& init, Store& store) noexcept {
    // Four frames by up to four vectors, or eight frames by one or two.
    constexpr int FB = VB >= 3 ? 4 : 8;
    std::uint32_t f = 0;
    for (; f + FB <= frames; f += FB)
        dense_tile<Isa, FB, VB>(w, taps, in_pad, out_pad, f, col, row, init, store);
    for (; f < frames; ++f)
        dense_tile<Isa, 1, VB>(w, taps, in_pad, out_pad, f, col, row, init, store);
}

template <class Isa, class Row, class Init, class Store>
inline void dense(const float* w, std::uint32_t taps, std::uint32_t in_pad, std::uint32_t out_pad,
                  std::uint32_t frames, Row&& row, Init&& init, Store&& store) noexcept {
    std::uint32_t col = 0;
    while (col < out_pad) {
        switch (std::min<std::uint32_t>((out_pad - col) / kLanes, 4u)) {
        case 4:
            dense_columns<Isa, 4>(w, taps, in_pad, out_pad, frames, col, row, init, store);
            col += 16;
            break;
        case 3:
            dense_columns<Isa, 3>(w, taps, in_pad, out_pad, frames, col, row, init, store);
            col += 12;
            break;
        case 2:
            dense_columns<Isa, 2>(w, taps, in_pad, out_pad, frames, col, row, init, store);
            col += 8;
            break;
        default:
            dense_columns<Isa, 1>(w, taps, in_pad, out_pad, frames, col, row, init, store);
            col += 4;
            break;
        }
    }
}

} // namespace pulp::gpu_audio::detail::neural_simd
