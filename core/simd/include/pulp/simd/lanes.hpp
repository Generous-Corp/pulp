#pragma once

/// @file lanes.hpp
/// Fixed-width register lanes for per-sample recursive kernels.
///
/// A recursive filter cannot vectorize along time, but independent channels
/// or sections can share one register: `Lanes<float, 2>` holds a left and a
/// right recursion side by side, and ordinary `+ - *` act on each lane. Every
/// lane runs exactly the scalar operation sequence, so each lane's result is
/// bit-identical to the scalar code (subject to the same FMA contraction).
///
/// These are GNU vector extensions (GCC and Clang), not Highway. Highway is a
/// private dependency of the compiled pulp-simd kernels, so header-only code
/// such as the DSP layer cannot include it; and its API is shaped for loops
/// over arrays with run-time dispatch, whereas a recursion needs a fixed-width
/// value that stays in a register across a whole per-sample loop and inlines
/// into the caller. Compilers without the extension (MSVC) do not define
/// `Lanes`; `kHasVectorLanes` (and the `PULP_SIMD_HAS_VECTOR_LANES` macro, for
/// preprocessor guards) is false there and callers run each lane as a scalar
/// instead. Defining `PULP_SIMD_DISABLE_VECTOR_LANES` selects that fallback on
/// GCC and Clang too, so its parity can be tested where the extension exists;
/// a program must define it in every translation unit or in none.
///
/// Whether multiply-adds are fused follows the translation unit's floating-
/// point contraction policy; see tools/cmake/PulpFloatingPointContraction.cmake.

#include <cstddef>
#include <type_traits>

namespace pulp::simd {

#if (defined(__GNUC__) || defined(__clang__)) && !defined(PULP_SIMD_DISABLE_VECTOR_LANES)
#define PULP_SIMD_HAS_VECTOR_LANES 1
inline constexpr bool kHasVectorLanes = true;

namespace detail {
template <typename T, std::size_t N> struct vector_lanes {
    static_assert(std::is_arithmetic_v<T> && N > 1 && (N & (N - 1)) == 0,
                  "Lanes needs an arithmetic element type and a power-of-two width above 1");
    typedef T type __attribute__((vector_size(N * sizeof(T))));
};
} // namespace detail

/// `N` lanes of `T` in one register-sized value (GCC and Clang only).
template <typename T, std::size_t N> using Lanes = typename detail::vector_lanes<T, N>::type;
#else
#define PULP_SIMD_HAS_VECTOR_LANES 0
inline constexpr bool kHasVectorLanes = false;
#endif

/// Number of lanes in `V`: 1 for a scalar, N for `Lanes<T, N>`.
template <typename V>
inline constexpr std::size_t lane_count = [] {
    if constexpr (std::is_arithmetic_v<V>)
        return std::size_t{1};
    else
        return sizeof(V) / sizeof(V{}[0]);
}();

/// Lane `i` of `v`; a scalar is its own lane 0.
template <typename V> inline auto get_lane(const V& v, std::size_t i) noexcept {
    if constexpr (std::is_arithmetic_v<V>) {
        (void)i;
        return v;
    } else {
        return v[i];
    }
}

/// Set lane `i` of `v` to `x`; a scalar is its own lane 0.
template <typename V, typename T> inline void set_lane(V& v, std::size_t i, T x) noexcept {
    if constexpr (std::is_arithmetic_v<V>) {
        (void)i;
        v = static_cast<V>(x);
    } else {
        v[i] = x;
    }
}

} // namespace pulp::simd
