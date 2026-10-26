#pragma once

// Four-lane float and two-lane double vectors for block kernels that run the
// same operation across several independent channels at once — the FDN tank's
// sixteen lines, a meter's channels — where the compiler cannot be relied on
// to find the vectors itself (recursive state, reductions it may not
// reassociate, spans it cannot prove disjoint).
//
// GCC and Clang get native vectors from the vector extension, so recursive
// state stays in registers across a block instead of round-tripping through an
// array per operation. Other compilers get a plain struct with the same
// operations, element by element; it computes the same values, only more
// slowly.
//
// Only what those kernels need is here. Comparisons return a lane mask for
// select(); min/max/clamp keep the first operand when the comparison is false,
// so a NaN lane passes through unchanged rather than being replaced.
//
// A kernel that runs four samples through these vectors and a scalar tail
// through the same expression gets identical results from both only when the
// compiler contracts multiply-adds alike in the two spellings. That holds for
// the vector extension under Clang (it contracts both), for GCC under Pulp's
// -ffp-contract=off, and for the plain struct on compilers that do not
// contract by default; the block kernels' partition-exactness rests on it.

#include <cstdint>
#include <cstring>

namespace pulp::signal::detail::lanes {

// The plain-struct lanes, compiled on every compiler: the implementation a
// compiler without the vector extension uses, and the reference the vector
// path is tested against (both must compute the same values).
namespace portable {

struct F4 {
    float v[4];
};
struct M4 {
    bool v[4];
};
struct D2 {
    double v[2];
};

inline F4 load(const float* p) {
    return {{p[0], p[1], p[2], p[3]}};
}
inline void store(float* p, F4 a) {
    std::memcpy(p, a.v, sizeof a.v);
}
inline D2 load(const double* p) {
    return {{p[0], p[1]}};
}
inline void store(double* p, D2 a) {
    std::memcpy(p, a.v, sizeof a.v);
}
inline F4 splat(float x) {
    return {{x, x, x, x}};
}
inline D2 splat(double x) {
    return {{x, x}};
}
inline F4 make(float a, float b, float c, float d) {
    return {{a, b, c, d}};
}
inline float lane(F4 v, int i) {
    return v.v[i];
}
inline double lane(D2 v, int i) {
    return v.v[i];
}
inline D2 make(double a, double b) {
    return {{a, b}};
}
inline M4 both(M4 a, M4 b) {
    return {{a.v[0] && b.v[0], a.v[1] && b.v[1], a.v[2] && b.v[2], a.v[3] && b.v[3]}};
}
inline F4 operator-(F4 a) {
    return {{-a.v[0], -a.v[1], -a.v[2], -a.v[3]}};
}
inline bool all(M4 m) {
    return m.v[0] && m.v[1] && m.v[2] && m.v[3];
}

#define PULP_FDN_LANES_OP(T, N, op)                                                                \
    inline T operator op(T a, T b) {                                                               \
        T r;                                                                                       \
        for (int i = 0; i < N; ++i)                                                                \
            r.v[i] = a.v[i] op b.v[i];                                                             \
        return r;                                                                                  \
    }                                                                                              \
    inline T& operator op## = (T & a, T b) {                                                       \
        return a = a op b;                                                                         \
    }
PULP_FDN_LANES_OP(F4, 4, +)
PULP_FDN_LANES_OP(F4, 4, -)
PULP_FDN_LANES_OP(F4, 4, *)
PULP_FDN_LANES_OP(F4, 4, /)
PULP_FDN_LANES_OP(D2, 2, +)
PULP_FDN_LANES_OP(D2, 2, -)
PULP_FDN_LANES_OP(D2, 2, *)
#undef PULP_FDN_LANES_OP

inline F4 select(M4 m, F4 a, F4 b) {
    F4 r;
    for (int i = 0; i < 4; ++i)
        r.v[i] = m.v[i] ? a.v[i] : b.v[i];
    return r;
}
inline M4 less(F4 a, F4 b) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] < b.v[i];
    return r;
}
inline M4 less_equal(F4 a, F4 b) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] <= b.v[i];
    return r;
}
inline M4 greater(F4 a, F4 b) {
    return less(b, a);
}
// Clears the sign bit, as the vector form does, so -0 and a negative NaN
// come out with the same bits on both paths.
inline F4 abs(F4 a) {
    F4 r;
    for (int i = 0; i < 4; ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, &a.v[i], sizeof bits);
        bits &= 0x7fffffffu;
        std::memcpy(&r.v[i], &bits, sizeof bits);
    }
    return r;
}
inline D2 load_widen2(const float* p) {
    return {{static_cast<double>(p[0]), static_cast<double>(p[1])}};
}
inline void store_narrow2(float* p, D2 v) {
    p[0] = static_cast<float>(v.v[0]);
    p[1] = static_cast<float>(v.v[1]);
}
inline D2 widen_low(F4 a) {
    return {{a.v[0], a.v[1]}};
}
inline D2 widen_high(F4 a) {
    return {{a.v[2], a.v[3]}};
}
inline F4 narrow(D2 lo, D2 hi) {
    return {{static_cast<float>(lo.v[0]), static_cast<float>(lo.v[1]), static_cast<float>(hi.v[0]),
             static_cast<float>(hi.v[1])}};
}
struct I2 {
    std::int64_t v[2];
};
struct M2 {
    bool v[2];
};
inline double lane_d(D2 v, int i) {
    return v.v[i];
}
inline std::int64_t lane(I2 v, int i) {
    return v.v[i];
}
inline I2 splat_i(std::int64_t x) {
    return {{x, x}};
}
inline I2 to_int(D2 v) {
    return {{static_cast<std::int64_t>(v.v[0]), static_cast<std::int64_t>(v.v[1])}};
}
inline D2 to_double(I2 v) {
    return {{static_cast<double>(v.v[0]), static_cast<double>(v.v[1])}};
}
inline D2 select(M2 m, D2 a, D2 b) {
    return {{m.v[0] ? a.v[0] : b.v[0], m.v[1] ? a.v[1] : b.v[1]}};
}
inline M2 less(D2 a, D2 b) {
    return {{a.v[0] < b.v[0], a.v[1] < b.v[1]}};
}
inline M2 greater(D2 a, D2 b) {
    return {{a.v[0] > b.v[0], a.v[1] > b.v[1]}};
}
inline I2 operator-(I2 a, I2 b) {
    return {{a.v[0] - b.v[0], a.v[1] - b.v[1]}};
}
inline I2 operator&(I2 a, I2 b) {
    return {{a.v[0] & b.v[0], a.v[1] & b.v[1]}};
}
inline void transpose(F4& a, F4& b, F4& c, F4& d) {
    const F4 r[4] = {a, b, c, d};
    a = {{r[0].v[0], r[1].v[0], r[2].v[0], r[3].v[0]}};
    b = {{r[0].v[1], r[1].v[1], r[2].v[1], r[3].v[1]}};
    c = {{r[0].v[2], r[1].v[2], r[2].v[2], r[3].v[2]}};
    d = {{r[0].v[3], r[1].v[3], r[2].v[3], r[3].v[3]}};
}
inline F4 swap_pairs(F4 a) {
    return {{a.v[1], a.v[0], a.v[3], a.v[2]}};
}
inline F4 swap_halves(F4 a) {
    return {{a.v[2], a.v[3], a.v[0], a.v[1]}};
}
inline F4 even_odd(F4 s, F4 d) {
    return {{s.v[0], d.v[0], s.v[2], d.v[2]}};
}
inline F4 low_low(F4 s, F4 d) {
    return {{s.v[0], s.v[1], d.v[0], d.v[1]}};
}

inline F4 min(F4 a, F4 b) {
    return select(less(b, a), b, a);
}
inline F4 max(F4 a, F4 b) {
    return select(greater(b, a), b, a);
}
inline D2 clamp(D2 x, D2 lo, D2 hi) {
    return select(greater(x, hi), hi, select(less(x, lo), lo, x));
}
inline F4 clamp(F4 x, F4 lo, F4 hi) {
    return min(max(x, lo), hi);
}

// A mask from a per-lane flag.
inline M4 mask_of(bool a, bool b, bool c, bool d) {
    return {{a, b, c, d}};
}

// In-register 4-point Hadamard (unnormalized) on each vector.
inline F4 hadamard4(F4 v) {
    const F4 p = swap_pairs(v);
    v = even_odd(v + p, v - p);
    const F4 h = swap_halves(v);
    return low_low(v + h, v - h);
}

} // namespace portable

// PULP_SIMD_LANES_PORTABLE selects the plain-struct lanes on GCC and Clang too,
// so the kernels' fallback can be built and tested where the extension exists.
// A program must define it in every translation unit or in none.
#if (defined(__GNUC__) || defined(__clang__)) && !defined(PULP_SIMD_LANES_PORTABLE)

using F4 = float __attribute__((vector_size(16)));
using M4 = std::int32_t __attribute__((vector_size(16)));
using D2 = double __attribute__((vector_size(16)));

inline F4 load(const float* p) {
    F4 v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline void store(float* p, F4 v) {
    std::memcpy(p, &v, sizeof v);
}
inline D2 load(const double* p) {
    D2 v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline void store(double* p, D2 v) {
    std::memcpy(p, &v, sizeof v);
}

inline F4 splat(float x) {
    return F4{x, x, x, x};
}
inline D2 splat(double x) {
    return D2{x, x};
}
inline F4 make(float a, float b, float c, float d) {
    return F4{a, b, c, d};
}
inline float lane(F4 v, int i) {
    return v[i];
}
inline double lane(D2 v, int i) {
    return v[i];
}
inline D2 make(double a, double b) {
    return D2{a, b};
}
inline M4 both(M4 a, M4 b) {
    return a & b;
}
inline bool all(M4 m) {
    return (m[0] & m[1] & m[2] & m[3]) != 0;
}

// C-style casts: the one spelling of a same-size vector reinterpretation that
// GCC and Clang both accept.
inline F4 select(M4 mask, F4 a, F4 b) {
    return (F4)((mask & (M4)a) | (~mask & (M4)b));
}
inline M4 less(F4 a, F4 b) {
    return a < b;
}
inline M4 less_equal(F4 a, F4 b) {
    return a <= b;
}
inline M4 greater(F4 a, F4 b) {
    return a > b;
}
inline F4 abs(F4 a) {
    return (F4)((M4)a & M4{0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff});
}

// Widen the low / high pair of a float vector to doubles, and narrow two
// double vectors back to one float vector.
using F2 = float __attribute__((vector_size(8)));
// Two adjacent floats, widened to doubles in one step, and back.
inline D2 load_widen2(const float* p) {
    F2 v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_convertvector(v, D2);
}
inline void store_narrow2(float* p, D2 v) {
    const F2 n = __builtin_convertvector(v, F2);
    std::memcpy(p, &n, sizeof n);
}
inline D2 widen_low(F4 v) {
    return D2{static_cast<double>(v[0]), static_cast<double>(v[1])};
}
inline D2 widen_high(F4 v) {
    return D2{static_cast<double>(v[2]), static_cast<double>(v[3])};
}
inline F4 narrow(D2 lo, D2 hi) {
    return F4{static_cast<float>(lo[0]), static_cast<float>(lo[1]), static_cast<float>(hi[0]),
              static_cast<float>(hi[1])};
}

// The two in-register butterfly stages of a 4-point Hadamard transform:
// [a+b, a-b, c+d, c-d] (stride 1) and [a+c, b+d, a-c, b-d] (stride 2).
using I2 = std::int64_t __attribute__((vector_size(16)));
using M2 = std::int64_t __attribute__((vector_size(16)));

inline double lane_d(D2 v, int i) {
    return v[i];
}
inline std::int64_t lane(I2 v, int i) {
    return v[i];
}
inline I2 splat_i(std::int64_t x) {
    return I2{x, x};
}
// Truncation toward zero, and back.
inline I2 to_int(D2 v) {
    return __builtin_convertvector(v, I2);
}
inline D2 to_double(I2 v) {
    return __builtin_convertvector(v, D2);
}
inline D2 select(M2 mask, D2 a, D2 b) {
    return (D2)((mask & (M2)a) | (~mask & (M2)b));
}
inline M2 less(D2 a, D2 b) {
    return a < b;
}
inline M2 greater(D2 a, D2 b) {
    return a > b;
}

#if defined(__clang__)
// Transpose four rows of four: rows a..d become columns.
inline void transpose(F4& a, F4& b, F4& c, F4& d) {
    const F4 ab_lo = __builtin_shufflevector(a, b, 0, 4, 1, 5);
    const F4 ab_hi = __builtin_shufflevector(a, b, 2, 6, 3, 7);
    const F4 cd_lo = __builtin_shufflevector(c, d, 0, 4, 1, 5);
    const F4 cd_hi = __builtin_shufflevector(c, d, 2, 6, 3, 7);
    a = __builtin_shufflevector(ab_lo, cd_lo, 0, 1, 4, 5);
    b = __builtin_shufflevector(ab_lo, cd_lo, 2, 3, 6, 7);
    c = __builtin_shufflevector(ab_hi, cd_hi, 0, 1, 4, 5);
    d = __builtin_shufflevector(ab_hi, cd_hi, 2, 3, 6, 7);
}
#else
inline void transpose(F4& a, F4& b, F4& c, F4& d) {
    const F4 ab_lo = __builtin_shuffle(a, b, M4{0, 4, 1, 5});
    const F4 ab_hi = __builtin_shuffle(a, b, M4{2, 6, 3, 7});
    const F4 cd_lo = __builtin_shuffle(c, d, M4{0, 4, 1, 5});
    const F4 cd_hi = __builtin_shuffle(c, d, M4{2, 6, 3, 7});
    a = __builtin_shuffle(ab_lo, cd_lo, M4{0, 1, 4, 5});
    b = __builtin_shuffle(ab_lo, cd_lo, M4{2, 3, 6, 7});
    c = __builtin_shuffle(ab_hi, cd_hi, M4{0, 1, 4, 5});
    d = __builtin_shuffle(ab_hi, cd_hi, M4{2, 3, 6, 7});
}
#endif

#if defined(__clang__)
inline F4 swap_pairs(F4 v) {
    return __builtin_shufflevector(v, v, 1, 0, 3, 2);
}
inline F4 swap_halves(F4 v) {
    return __builtin_shufflevector(v, v, 2, 3, 0, 1);
}
inline F4 even_odd(F4 sum, F4 diff) {
    return __builtin_shufflevector(sum, diff, 0, 4, 2, 6);
}
inline F4 low_low(F4 sum, F4 diff) {
    return __builtin_shufflevector(sum, diff, 0, 1, 4, 5);
}
#else
inline F4 swap_pairs(F4 v) {
    return __builtin_shuffle(v, M4{1, 0, 3, 2});
}
inline F4 swap_halves(F4 v) {
    return __builtin_shuffle(v, M4{2, 3, 0, 1});
}
inline F4 even_odd(F4 sum, F4 diff) {
    return __builtin_shuffle(sum, diff, M4{0, 4, 2, 6});
}
inline F4 low_low(F4 sum, F4 diff) {
    return __builtin_shuffle(sum, diff, M4{0, 1, 4, 5});
}
#endif

inline F4 min(F4 a, F4 b) {
    return select(less(b, a), b, a);
}
inline F4 max(F4 a, F4 b) {
    return select(greater(b, a), b, a);
}
inline D2 clamp(D2 x, D2 lo, D2 hi) {
    return select(greater(x, hi), hi, select(less(x, lo), lo, x));
}
inline F4 clamp(F4 x, F4 lo, F4 hi) {
    return min(max(x, lo), hi);
}

// A mask from a per-lane flag.
inline M4 mask_of(bool a, bool b, bool c, bool d) {
    return M4{a ? -1 : 0, b ? -1 : 0, c ? -1 : 0, d ? -1 : 0};
}

// In-register 4-point Hadamard (unnormalized) on each vector.
inline F4 hadamard4(F4 v) {
    const F4 p = swap_pairs(v);
    v = even_odd(v + p, v - p);
    const F4 h = swap_halves(v);
    return low_low(v + h, v - h);
}

#else

using namespace portable;

#endif

} // namespace pulp::signal::detail::lanes
