// Backend parity and correctness for the pulp::simd complex kernels.
//
// Each compiled backend (Highway always; Accelerate on Apple) and the
// configure-time default is run against the scalar reference over ragged
// lengths, both element types, both layouts (interleaved and split), and
// flush-to-zero on and off. The scalar reference is itself held to a
// long-double computation of the same formulas, so a shared mistake in all
// backends cannot pass.
//
// Tolerances (per component, eps = machine epsilon of the element type):
//   - products and accumulations: 4 * eps * (sum of the magnitudes of the
//     terms, including the accumulator); a fused multiply-add on one side and
//     two roundings on the other differ by at most that.
//   - magnitude squared / power spectrum: 4 * eps * the value.
//   - magnitude: 4 * eps * the value (vDSP computes it without the square-root
//     of a rounded sum, so allow a few ulps).
//   - phase: 16 * eps absolute (atan2 is within 3 ulp of a value no larger
//     than pi; Accelerate and Highway use different polynomials).

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/scoped_flush_denormals.hpp>
#include <pulp/simd/simd.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

namespace backend = pulp::simd::backend;

template <typename T> struct ComplexKernels {
    const char* name;
    void (*multiply)(const T*, const T*, T*, std::size_t) noexcept;
    void (*multiply_conj)(const T*, const T*, T*, std::size_t) noexcept;
    void (*multiply_accumulate)(const T*, const T*, T*, std::size_t) noexcept;
    void (*magnitude)(const T*, T*, std::size_t) noexcept;
    void (*magnitude_squared)(const T*, T*, std::size_t) noexcept;
    void (*phase)(const T*, T*, std::size_t) noexcept;
    void (*power_spectrum)(const T*, T, T*, std::size_t) noexcept;
    void (*multiply_split)(const T*, const T*, const T*, const T*, T*, T*, std::size_t) noexcept;
    void (*multiply_conj_split)(const T*, const T*, const T*, const T*, T*, T*,
                                std::size_t) noexcept;
    void (*multiply_accumulate_split)(const T*, const T*, const T*, const T*, T*, T*,
                                      std::size_t) noexcept;
    void (*magnitude_split)(const T*, const T*, T*, std::size_t) noexcept;
    void (*magnitude_squared_split)(const T*, const T*, T*, std::size_t) noexcept;
    void (*phase_split)(const T*, const T*, T*, std::size_t) noexcept;
    void (*power_spectrum_split)(const T*, const T*, T, T*, std::size_t) noexcept;
};

#define PULP_TEST_COMPLEX(NAME, NS)                                                                \
    ComplexKernels<T> {                                                                            \
        NAME, &NS::complex_multiply, &NS::complex_multiply_conj,                                   \
            &NS::complex_multiply_accumulate, &NS::complex_magnitude,                              \
            &NS::complex_magnitude_squared, &NS::complex_phase, &NS::complex_power_spectrum,       \
            &NS::complex_multiply_split, &NS::complex_multiply_conj_split,                         \
            &NS::complex_multiply_accumulate_split, &NS::complex_magnitude_split,                  \
            &NS::complex_magnitude_squared_split, &NS::complex_phase_split,                        \
            &NS::complex_power_spectrum_split                                                      \
    }

template <typename T> ComplexKernels<T> reference_kernels() {
    return PULP_TEST_COMPLEX("scalar", backend::scalar);
}

template <typename T> std::vector<ComplexKernels<T>> candidate_kernels() {
    std::vector<ComplexKernels<T>> out;
#if defined(PULP_SIMD_HAS_HIGHWAY)
    out.push_back(PULP_TEST_COMPLEX("highway", backend::highway));
#endif
#if defined(PULP_SIMD_HAS_ACCELERATE)
    out.push_back(PULP_TEST_COMPLEX("accelerate", backend::accelerate));
#endif
    out.push_back(PULP_TEST_COMPLEX("default", pulp::simd));
    return out;
}

constexpr std::size_t kLengths[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 64, 65, 100, 255, 1023};

template <typename T> constexpr T eps() {
    return std::numeric_limits<T>::epsilon();
}
template <typename T> constexpr T slack() {
    return std::numeric_limits<T>::min() * T(8);
}

template <typename T> std::vector<T> noise(std::size_t n, unsigned seed, T amplitude = T(0.9)) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::vector<T> v(n);
    for (auto& x : v)
        x = T(dist(rng)) * amplitude;
    return v;
}

template <typename T> void sprinkle_subnormals(std::vector<T>& v) {
    const T tiny = std::numeric_limits<T>::denorm_min() * T(3);
    for (std::size_t i = 0; i < v.size(); i += 5)
        v[i] = (i & 1) ? -tiny : tiny;
}

/// A complex test signal in both layouts.
template <typename T> struct Signal {
    std::vector<T> z;  // interleaved, 2n
    std::vector<T> re; // split
    std::vector<T> im;
    std::size_t n;
};

template <typename T> Signal<T> make_signal(std::size_t n, unsigned seed, bool subnormals) {
    Signal<T> s;
    s.n = n;
    s.z = noise<T>(2 * n, seed);
    if (subnormals)
        sprinkle_subnormals(s.z);
    s.re.resize(n);
    s.im.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        s.re[i] = s.z[2 * i];
        s.im[i] = s.z[2 * i + 1];
    }
    return s;
}

template <typename T> std::vector<T> interleave(const std::vector<T>& re, const std::vector<T>& im) {
    std::vector<T> z(2 * re.size());
    for (std::size_t i = 0; i < re.size(); ++i) {
        z[2 * i] = re[i];
        z[2 * i + 1] = im[i];
    }
    return z;
}

/// The kernel's own formulas, evaluated in long double.
template <typename T> struct Exact {
    long double re, im, mag_sum; // result and sum of term magnitudes (re part, im part)
    long double re_mag, im_mag;
};

template <typename T>
void check_close(T want, T got, T bound, const char* what, std::size_t i) {
    INFO(what << " at " << i << ": want " << want << " got " << got << " bound " << bound);
    CHECK(std::fabs(want - got) <= bound);
}

template <typename T>
void check_products(const ComplexKernels<T>& ref, const ComplexKernels<T>& k, const Signal<T>& a,
                    const Signal<T>& b, const Signal<T>& c) {
    const std::size_t n = a.n;
    std::vector<T> want(2 * n), got(2 * n), wre(n), wim(n), gre(n), gim(n);

    // ── multiply ──
    ref.multiply(a.z.data(), b.z.data(), want.data(), n);
    k.multiply(a.z.data(), b.z.data(), got.data(), n);
    ref.multiply_split(a.re.data(), a.im.data(), b.re.data(), b.im.data(), wre.data(), wim.data(),
                       n);
    k.multiply_split(a.re.data(), a.im.data(), b.re.data(), b.im.data(), gre.data(), gim.data(),
                     n);
    for (std::size_t i = 0; i < n; ++i) {
        const T mr = std::fabs(a.re[i] * b.re[i]) + std::fabs(a.im[i] * b.im[i]);
        const T mi = std::fabs(a.re[i] * b.im[i]) + std::fabs(a.im[i] * b.re[i]);
        const T br = T(4) * eps<T>() * mr + slack<T>(), bi = T(4) * eps<T>() * mi + slack<T>();
        check_close(want[2 * i], got[2 * i], br, "multiply re", i);
        check_close(want[2 * i + 1], got[2 * i + 1], bi, "multiply im", i);
        check_close(wre[i], gre[i], br, "multiply_split re", i);
        check_close(wim[i], gim[i], bi, "multiply_split im", i);
        // The two layouts of one backend agree to the same bound.
        check_close(got[2 * i], gre[i], T(2) * br, "multiply layouts re", i);
        check_close(got[2 * i + 1], gim[i], T(2) * bi, "multiply layouts im", i);
    }

    // ── conjugate multiply ──
    ref.multiply_conj(a.z.data(), b.z.data(), want.data(), n);
    k.multiply_conj(a.z.data(), b.z.data(), got.data(), n);
    ref.multiply_conj_split(a.re.data(), a.im.data(), b.re.data(), b.im.data(), wre.data(),
                            wim.data(), n);
    k.multiply_conj_split(a.re.data(), a.im.data(), b.re.data(), b.im.data(), gre.data(),
                          gim.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
        const T mr = std::fabs(a.re[i] * b.re[i]) + std::fabs(a.im[i] * b.im[i]);
        const T mi = std::fabs(a.im[i] * b.re[i]) + std::fabs(a.re[i] * b.im[i]);
        const T br = T(4) * eps<T>() * mr + slack<T>(), bi = T(4) * eps<T>() * mi + slack<T>();
        check_close(want[2 * i], got[2 * i], br, "multiply_conj re", i);
        check_close(want[2 * i + 1], got[2 * i + 1], bi, "multiply_conj im", i);
        check_close(wre[i], gre[i], br, "multiply_conj_split re", i);
        check_close(wim[i], gim[i], bi, "multiply_conj_split im", i);
    }

    // ── multiply-accumulate (into a copy of c) ──
    want = c.z;
    got = c.z;
    ref.multiply_accumulate(a.z.data(), b.z.data(), want.data(), n);
    k.multiply_accumulate(a.z.data(), b.z.data(), got.data(), n);
    wre = c.re;
    wim = c.im;
    gre = c.re;
    gim = c.im;
    ref.multiply_accumulate_split(a.re.data(), a.im.data(), b.re.data(), b.im.data(), wre.data(),
                                  wim.data(), n);
    k.multiply_accumulate_split(a.re.data(), a.im.data(), b.re.data(), b.im.data(), gre.data(),
                                gim.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
        const T mr = std::fabs(a.re[i] * b.re[i]) + std::fabs(a.im[i] * b.im[i]) + std::fabs(c.re[i]);
        const T mi = std::fabs(a.re[i] * b.im[i]) + std::fabs(a.im[i] * b.re[i]) + std::fabs(c.im[i]);
        const T br = T(4) * eps<T>() * mr + slack<T>(), bi = T(4) * eps<T>() * mi + slack<T>();
        check_close(want[2 * i], got[2 * i], br, "mac re", i);
        check_close(want[2 * i + 1], got[2 * i + 1], bi, "mac im", i);
        check_close(wre[i], gre[i], br, "mac_split re", i);
        check_close(wim[i], gim[i], bi, "mac_split im", i);
    }

    // ── exact in-place aliasing (dst == a, then dst == b) ──
    ref.multiply(a.z.data(), b.z.data(), want.data(), n);
    got = a.z;
    k.multiply(got.data(), b.z.data(), got.data(), n);
    for (std::size_t i = 0; i < 2 * n; ++i) {
        const T m = std::fabs(a.z[i]) * T(2) + T(1);
        check_close(want[i], got[i], T(8) * eps<T>() * m + slack<T>(), "multiply dst==a", i);
    }
    got = b.z;
    k.multiply(a.z.data(), got.data(), got.data(), n);
    for (std::size_t i = 0; i < 2 * n; ++i) {
        const T m = std::fabs(b.z[i]) * T(2) + T(1);
        check_close(want[i], got[i], T(8) * eps<T>() * m + slack<T>(), "multiply dst==b", i);
    }
    gre = a.re;
    gim = a.im;
    ref.multiply_split(a.re.data(), a.im.data(), b.re.data(), b.im.data(), wre.data(), wim.data(), n);
    k.multiply_split(gre.data(), gim.data(), b.re.data(), b.im.data(), gre.data(), gim.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
        check_close(wre[i], gre[i], T(8) * eps<T>() * 2 + slack<T>(), "split dst==a re", i);
        check_close(wim[i], gim[i], T(8) * eps<T>() * 2 + slack<T>(), "split dst==a im", i);
    }
}

template <typename T>
void check_spectral(const ComplexKernels<T>& ref, const ComplexKernels<T>& k, const Signal<T>& a) {
    const std::size_t n = a.n;
    std::vector<T> want(n), got(n), wsplit(n), gsplit(n);
    const T scale = T(0.25);

    ref.magnitude_squared(a.z.data(), want.data(), n);
    k.magnitude_squared(a.z.data(), got.data(), n);
    k.magnitude_squared_split(a.re.data(), a.im.data(), gsplit.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
        const T m = a.re[i] * a.re[i] + a.im[i] * a.im[i];
        check_close(want[i], got[i], T(4) * eps<T>() * m + slack<T>(), "magnitude_squared", i);
        check_close(want[i], gsplit[i], T(4) * eps<T>() * m + slack<T>(), "magnitude_squared_split", i);
    }

    ref.power_spectrum(a.z.data(), scale, want.data(), n);
    k.power_spectrum(a.z.data(), scale, got.data(), n);
    k.power_spectrum_split(a.re.data(), a.im.data(), scale, gsplit.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
        const T m = (a.re[i] * a.re[i] + a.im[i] * a.im[i]) * scale;
        check_close(want[i], got[i], T(4) * eps<T>() * m + slack<T>(), "power_spectrum", i);
        check_close(want[i], gsplit[i], T(4) * eps<T>() * m + slack<T>(), "power_spectrum_split", i);
    }

    ref.magnitude(a.z.data(), want.data(), n);
    k.magnitude(a.z.data(), got.data(), n);
    k.magnitude_split(a.re.data(), a.im.data(), gsplit.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
        // A subnormal component squares to zero in a flushing mode; that moves
        // the magnitude by at most the subnormal, covered by the slack.
        const T m = want[i];
        check_close(want[i], got[i], T(4) * eps<T>() * m + slack<T>(), "magnitude", i);
        check_close(want[i], gsplit[i], T(4) * eps<T>() * m + slack<T>(), "magnitude_split", i);
    }

    ref.phase(a.z.data(), want.data(), n);
    k.phase(a.z.data(), got.data(), n);
    k.phase_split(a.re.data(), a.im.data(), gsplit.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
        // Within the principal range, away from the +/- pi seam where a one-ulp
        // difference in the input flips the sign of the result.
        if (std::fabs(a.im[i]) < T(1e-3))
            continue;
        check_close(want[i], got[i], T(16) * eps<T>() + slack<T>(), "phase", i);
        check_close(want[i], gsplit[i], T(16) * eps<T>() + slack<T>(), "phase_split", i);
    }
}

/// The scalar reference against long double: guards against a mistake shared
/// by every backend (a swapped sign, conjugating the wrong operand).
template <typename T> void check_reference_against_long_double(std::size_t n, unsigned seed) {
    const auto ref = reference_kernels<T>();
    const auto a = make_signal<T>(n, seed, false), b = make_signal<T>(n, seed + 1, false);
    const auto c = make_signal<T>(n, seed + 2, false);
    std::vector<T> got(2 * n);
    using C = std::complex<long double>;
    for (int mode = 0; mode < 3; ++mode) {
        got = c.z;
        if (mode == 0)
            ref.multiply(a.z.data(), b.z.data(), got.data(), n);
        else if (mode == 1)
            ref.multiply_conj(a.z.data(), b.z.data(), got.data(), n);
        else
            ref.multiply_accumulate(a.z.data(), b.z.data(), got.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            const C x(a.re[i], a.im[i]), y(b.re[i], b.im[i]), acc(c.re[i], c.im[i]);
            const C want = mode == 0 ? x * y : mode == 1 ? x * std::conj(y) : acc + x * y;
            const T bound = T(8) * eps<T>() * T(3) + slack<T>();
            CHECK(std::fabs(static_cast<long double>(got[2 * i]) - want.real()) <= bound);
            CHECK(std::fabs(static_cast<long double>(got[2 * i + 1]) - want.imag()) <= bound);
        }
    }
    std::vector<T> out(n);
    ref.phase(a.z.data(), out.data(), n);
    for (std::size_t i = 0; i < n; ++i)
        CHECK(std::fabs(static_cast<long double>(out[i]) - std::atan2(static_cast<long double>(a.im[i]),
                                                                       static_cast<long double>(a.re[i]))) <=
              T(4) * eps<T>());
}

template <typename T> void run_complex(bool subnormals) {
    const auto ref = reference_kernels<T>();
    for (const auto& k : candidate_kernels<T>()) {
        INFO("backend " << k.name << (subnormals ? " with subnormals" : ""));
        unsigned seed = 3;
        for (std::size_t n : kLengths) {
            INFO("n = " << n);
            const auto a = make_signal<T>(n, seed, subnormals);
            const auto b = make_signal<T>(n, seed + 1, false);
            const auto c = make_signal<T>(n, seed + 2, subnormals);
            check_products(ref, k, a, b, c);
            check_spectral(ref, k, a);
            seed += 13;
        }
    }
}

} // namespace

TEMPLATE_TEST_CASE("pulp::simd complex kernels match the scalar reference", "[simd][complex]",
                   float, double) {
    SECTION("flush-to-zero off") {
        run_complex<TestType>(false);
    }
    SECTION("flush-to-zero on") {
        pulp::signal::ScopedFlushDenormals ftz;
        run_complex<TestType>(true);
    }
}

TEMPLATE_TEST_CASE("pulp::simd scalar complex reference matches long double", "[simd][complex]",
                   float, double) {
    for (std::size_t n : {std::size_t(1), std::size_t(7), std::size_t(64), std::size_t(257)})
        check_reference_against_long_double<TestType>(n, unsigned(n) + 5);
}

TEMPLATE_TEST_CASE("pulp::simd complex phase axes and the origin", "[simd][complex]", float,
                   double) {
    using T = TestType;
    // (re, im) pairs on the axes and in each quadrant.
    const std::vector<T> z = {T(1),  T(0),  T(0),  T(1), T(-1), T(0),  T(0),  T(-1),
                              T(0),  T(0),  T(1),  T(1), T(-1), T(1),  T(-1), T(-1), T(1), T(-1)};
    const std::size_t n = z.size() / 2;
    const T pi = T(3.14159265358979323846L);
    const T want[] = {T(0),         pi / 2,       pi,          -pi / 2,     T(0),
                      pi / 4,       T(3) * pi / 4, -T(3) * pi / 4, -pi / 4};
    auto all = candidate_kernels<T>();
    all.push_back(reference_kernels<T>());
    for (const auto& k : all) {
        INFO("backend " << k.name);
        std::vector<T> out(n);
        k.phase(z.data(), out.data(), n);
        for (std::size_t i = 0; i < n; ++i)
            CHECK(std::fabs(out[i] - want[i]) <= T(16) * eps<T>());
    }
}

TEMPLATE_TEST_CASE("pulp::simd complex kernels leave empty ranges alone", "[simd][complex]", float,
                   double) {
    using T = TestType;
    auto all = candidate_kernels<T>();
    all.push_back(reference_kernels<T>());
    for (const auto& k : all) {
        INFO("backend " << k.name);
        const T canary = T(42);
        T x[4] = {T(1), T(-2), T(3), T(-4)};
        T y[4] = {canary, canary, canary, canary};
        T w[4] = {canary, canary, canary, canary};
        k.multiply(x, x, y, 0);
        k.multiply_conj(x, x, y, 0);
        k.multiply_accumulate(x, x, y, 0);
        k.magnitude(x, y, 0);
        k.magnitude_squared(x, y, 0);
        k.phase(x, y, 0);
        k.power_spectrum(x, T(2), y, 0);
        k.multiply_split(x, x, x, x, y, w, 0);
        k.multiply_conj_split(x, x, x, x, y, w, 0);
        k.multiply_accumulate_split(x, x, x, x, y, w, 0);
        k.magnitude_split(x, x, y, 0);
        k.magnitude_squared_split(x, x, y, 0);
        k.phase_split(x, x, y, 0);
        k.power_spectrum_split(x, x, T(2), y, 0);
        for (int i = 0; i < 4; ++i) {
            CHECK(y[i] == canary);
            CHECK(w[i] == canary);
        }
    }
}

TEMPLATE_TEST_CASE("pulp::simd complex kernels do not allocate", "[simd][rt-safety]", float,
                   double) {
    using T = TestType;
    auto all = candidate_kernels<T>();
    all.push_back(reference_kernels<T>());
    const std::size_t n = 1024;
    const auto a = make_signal<T>(n, 5, false), b = make_signal<T>(n, 6, false);
    std::vector<T> z(2 * n), r1(n), r2(n), r3(n);
    for (const auto& k : all) {
        INFO("backend " << k.name);
        pulp::test::RtAllocationProbe probe;
        k.multiply(a.z.data(), b.z.data(), z.data(), n);
        k.multiply_conj(a.z.data(), b.z.data(), z.data(), n);
        k.multiply_accumulate(a.z.data(), b.z.data(), z.data(), n);
        k.magnitude(a.z.data(), r1.data(), n);
        k.magnitude_squared(a.z.data(), r1.data(), n);
        k.phase(a.z.data(), r1.data(), n);
        k.power_spectrum(a.z.data(), T(0.5), r1.data(), n);
        k.multiply_split(a.re.data(), a.im.data(), b.re.data(), b.im.data(), r1.data(), r2.data(), n);
        k.multiply_conj_split(a.re.data(), a.im.data(), b.re.data(), b.im.data(), r1.data(), r2.data(), n);
        k.multiply_accumulate_split(a.re.data(), a.im.data(), b.re.data(), b.im.data(), r1.data(), r2.data(), n);
        k.magnitude_split(a.re.data(), a.im.data(), r3.data(), n);
        k.magnitude_squared_split(a.re.data(), a.im.data(), r3.data(), n);
        k.phase_split(a.re.data(), a.im.data(), r3.data(), n);
        k.power_spectrum_split(a.re.data(), a.im.data(), T(0.5), r3.data(), n);
        CHECK(probe.allocation_count() == 0);
    }
}
