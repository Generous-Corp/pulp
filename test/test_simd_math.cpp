// Accuracy, parity and edge behaviour of the pulp::simd vector math kernels
// (exp, log, sin, cos, sincos, tanh, sqrt, wrap_phase) and their two accuracy
// tiers.
//
// Errors are measured against the C library evaluated in double (for float
// kernels that is far beyond float precision, so the figure is the kernel's own
// error) and, for double kernels, against the C library in double (itself
// accurate to well under one unit in the last place on every supported
// platform, so a double kernel's figure is its distance from libm).
//
// Metrics:
//   ulp   |got - exact| divided by the spacing of the floating-point numbers at
//         exact. Used where the result is not near zero relative to its scale.
//   abs   |got - exact|. Used for sin, cos and tanh near their zeros, and for
//         every function of the `fast` tier whose error is stated as absolute.
//   rel   |got - exact| / |exact|.
//
// The bounds asserted below are the ones documented in
// docs/reference/modules.md; they sit above the maxima measured on arm64
// (NEON) with Accelerate, and the "[accuracy-report]" case prints the measured
// maxima so a documentation update can be checked against them.

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/scoped_flush_denormals.hpp>
#include <pulp/simd/simd.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

namespace backend = pulp::simd::backend;
using pulp::simd::Accuracy;

template <typename T> struct MathKernels {
    const char* name;
    void (*exp)(const T*, T*, std::size_t, Accuracy) noexcept;
    void (*log)(const T*, T, T*, std::size_t, Accuracy) noexcept;
    void (*sin)(const T*, T*, std::size_t, Accuracy) noexcept;
    void (*cos)(const T*, T*, std::size_t, Accuracy) noexcept;
    void (*sincos)(const T*, T*, T*, std::size_t, Accuracy) noexcept;
    void (*tanh)(const T*, T*, std::size_t, Accuracy) noexcept;
    void (*sqrt)(const T*, T*, std::size_t) noexcept;
    void (*wrap_phase)(const T*, T*, std::size_t) noexcept;
};

#define PULP_TEST_MATH(NAME, NS)                                                                   \
    MathKernels<T> {                                                                               \
        NAME, &NS::exp, &NS::log, &NS::sin, &NS::cos, &NS::sincos, &NS::tanh, &NS::sqrt,           \
            &NS::wrap_phase                                                                        \
    }

template <typename T> MathKernels<T> reference_kernels() {
    return PULP_TEST_MATH("scalar", backend::scalar);
}

template <typename T> std::vector<MathKernels<T>> candidate_kernels() {
    std::vector<MathKernels<T>> out;
#if defined(PULP_SIMD_HAS_HIGHWAY)
    out.push_back(PULP_TEST_MATH("highway", backend::highway));
#endif
#if defined(PULP_SIMD_HAS_ACCELERATE)
    out.push_back(PULP_TEST_MATH("accelerate", backend::accelerate));
#endif
    out.push_back(PULP_TEST_MATH("default", pulp::simd));
    return out;
}

template <typename T> std::vector<MathKernels<T>> all_kernels() {
    auto v = candidate_kernels<T>();
    v.push_back(reference_kernels<T>());
    return v;
}

constexpr std::size_t kCount = 20003; // ragged: not a multiple of any lane count

template <typename T>
std::vector<T> uniform(std::size_t n, double lo, double hi, unsigned seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> dist(lo, hi);
    std::vector<T> v(n);
    for (auto& x : v)
        x = static_cast<T>(dist(rng));
    return v;
}

/// Log-uniform in [lo, hi].
template <typename T>
std::vector<T> log_uniform(std::size_t n, double lo, double hi, unsigned seed) {
    auto v = uniform<T>(n, std::log(lo), std::log(hi), seed);
    for (auto& x : v)
        x = static_cast<T>(std::exp(static_cast<double>(x)));
    return v;
}

/// Spacing of the representable T values around `exact` (an unit in the last
/// place of the result).
template <typename T> double spacing(double exact) {
    const double a = std::fabs(exact);
    if constexpr (std::is_same_v<T, float>) {
        const float f = static_cast<float>(a);
        return static_cast<double>(std::nextafter(f, std::numeric_limits<float>::infinity())) -
               static_cast<double>(f);
    } else {
        return std::nextafter(a, std::numeric_limits<double>::infinity()) - a;
    }
}

struct Errors {
    double ulp = 0, abs = 0, rel = 0;
    void add(double got, double exact, double unit) {
        const double e = std::fabs(got - exact);
        abs = std::max(abs, e);
        ulp = std::max(ulp, e / unit);
        if (exact != 0)
            rel = std::max(rel, e / std::fabs(exact));
    }
};

/// Runs `kernel` over x and measures it against `truth`.
template <typename T, typename Kernel, typename Truth>
Errors measure(const std::vector<T>& x, Kernel kernel, Truth truth) {
    std::vector<T> y(x.size());
    kernel(x.data(), y.data(), x.size());
    Errors e;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double exact = truth(static_cast<double>(x[i]));
        e.add(static_cast<double>(y[i]), exact, spacing<T>(exact));
    }
    return e;
}


constexpr double kPi = 3.14159265358979323846264338327950288;

/// Distance on the circle: a phase of pi and one of -pi are the same angle.
double circular_distance(double a, double b) {
    double d = std::fabs(a - b);
    d = std::fmod(d, 2 * kPi);
    return std::min(d, 2 * kPi - d);
}

// ── Documented bounds (keep in step with docs/reference/modules.md) ─────────
// precise tier: units in the last place of the result, float and double.
constexpr double kPreciseUlp = 4.0;
// fast tier (float): relative error of exp and log, absolute of the rest.
constexpr double kFastExpRel = 4.0e-6;   // x in [-87, 88]
constexpr double kFastLogRel = 4.5e-6;   // x in [floor, FLT_MAX]
constexpr double kFastSinCosAbs = 2.5e-7; // |x| <= 1e4
constexpr double kFastTanhAbs = 2.2e-6;
// wrap_phase: absolute distance (on the circle) from the exact reduction.
constexpr double kWrapAbsFloat = 3.0e-7;  // |x| <= 1e4
constexpr double kWrapAbsDouble = 1.0e-15; // |x| <= 1e6

template <typename T> constexpr double wrap_domain() {
    return std::is_same_v<T, float> ? 1e4 : 1e6;
}

struct Case {
    const char* what;
    double lo, hi;
    bool log_scale;
};

template <typename T> std::vector<T> samples(const Case& c, unsigned seed) {
    return c.log_scale ? log_uniform<T>(kCount, c.lo, c.hi, seed)
                       : uniform<T>(kCount, c.lo, c.hi, seed);
}

/// Points at which sin or cos is nearly zero: the float (or double) nearest to
/// a multiple of pi/2. Relative accuracy there needs a careful reduction.
template <typename T> std::vector<T> near_zero_points(bool cosine) {
    std::vector<T> x;
    for (int k = 1; k <= 1500; ++k)
        x.push_back(static_cast<T>((k + (cosine ? 0.5 : 0.0)) * kPi * (k % 2 ? 1 : -1)));
    return x;
}

template <typename T, typename Truth, typename Run>
void check_ulp(const char* backend_name, const char* what, const std::vector<T>& x, Run run,
               Truth truth, double bound) {
    const auto e = measure<T>(x, run, truth);
    INFO(backend_name << " " << what << ": max " << e.ulp << " ulp, bound " << bound);
    CHECK(e.ulp <= bound);
}

} // namespace

TEMPLATE_TEST_CASE("pulp::simd precise vector math stays within its documented ulp bound",
                   "[simd][math][accuracy]", float, double) {
    using T = TestType;
    for (const auto& k : all_kernels<T>()) {
        const auto precise = [&](auto f) {
            return [=](const T* x, T* y, std::size_t n) { f(x, y, n); };
        };
        // exp: the whole range in which the result is a normal number.
        check_ulp<T>(k.name, "exp", samples<T>({"", -87, 88, false}, 1),
                     precise([&](const T* x, T* y, std::size_t n) { k.exp(x, y, n, Accuracy::precise); }),
                     [](double v) { return std::exp(v); }, kPreciseUlp);
        check_ulp<T>(k.name, "exp small", samples<T>({"", -1, 1, false}, 2),
                     precise([&](const T* x, T* y, std::size_t n) { k.exp(x, y, n, Accuracy::precise); }),
                     [](double v) { return std::exp(v); }, kPreciseUlp);
        // Toward overflow and into the subnormal range, where the result is
        // still the nearest representable number.
        check_ulp<T>(k.name, "exp to overflow", samples<T>({"", 88, 88.72, false}, 10),
                     precise([&](const T* x, T* y, std::size_t n) { k.exp(x, y, n, Accuracy::precise); }),
                     [](double v) { return std::exp(v); }, kPreciseUlp);
        // log: from tiny to huge, and around 1 where the result crosses zero.
        const auto log_run = precise(
            [&](const T* x, T* y, std::size_t n) { k.log(x, T(0), y, n, Accuracy::precise); });
        check_ulp<T>(k.name, "log wide", samples<T>({"", 1e-30, 1e30, true}, 3), log_run,
                     [](double v) { return std::log(v); }, kPreciseUlp);
        check_ulp<T>(k.name, "log near one", samples<T>({"", 0.5, 2, false}, 4), log_run,
                     [](double v) { return std::log(v); }, kPreciseUlp);
        // sin and cos over |x| <= 1e4 and at the points where they nearly vanish.
        for (const double range : {3.2, 1e3, 1e4}) {
            const auto sin_run = precise(
                [&](const T* x, T* y, std::size_t n) { k.sin(x, y, n, Accuracy::precise); });
            const auto cos_run = precise(
                [&](const T* x, T* y, std::size_t n) { k.cos(x, y, n, Accuracy::precise); });
            check_ulp<T>(k.name, "sin", samples<T>({"", -range, range, false}, 5), sin_run,
                         [](double v) { return std::sin(v); }, kPreciseUlp);
            check_ulp<T>(k.name, "cos", samples<T>({"", -range, range, false}, 6), cos_run,
                         [](double v) { return std::cos(v); }, kPreciseUlp);
        }
        check_ulp<T>(k.name, "sin near zeros", near_zero_points<T>(false),
                     precise([&](const T* x, T* y, std::size_t n) { k.sin(x, y, n, Accuracy::precise); }),
                     [](double v) { return std::sin(v); }, kPreciseUlp);
        check_ulp<T>(k.name, "cos near zeros", near_zero_points<T>(true),
                     precise([&](const T* x, T* y, std::size_t n) { k.cos(x, y, n, Accuracy::precise); }),
                     [](double v) { return std::cos(v); }, kPreciseUlp);
        const auto tanh_run =
            precise([&](const T* x, T* y, std::size_t n) { k.tanh(x, y, n, Accuracy::precise); });
        check_ulp<T>(k.name, "tanh", samples<T>({"", -10, 10, false}, 7), tanh_run,
                     [](double v) { return std::tanh(v); }, kPreciseUlp);
        check_ulp<T>(k.name, "tanh small", samples<T>({"", -0.1, 0.1, false}, 8), tanh_run,
                     [](double v) { return std::tanh(v); }, kPreciseUlp);
        // sqrt is correctly rounded: zero error, and the same on every backend.
        check_ulp<T>(k.name, "sqrt", samples<T>({"", 1e-10, 1e10, true}, 9),
                     precise([&](const T* x, T* y, std::size_t n) { k.sqrt(x, y, n); }),
                     [](double v) { return std::sqrt(v); }, 0.5);
    }
}

TEMPLATE_TEST_CASE("pulp::simd fast vector math stays within its documented error bound",
                   "[simd][math][accuracy]", float, double) {
    using T = TestType;
    for (const auto& k : all_kernels<T>()) {
        INFO("backend " << k.name);
        const auto fast = [&](auto f) {
            return [=](const T* x, T* y, std::size_t n) { f(x, y, n); };
        };
        // The documented fast bounds are for float. A double request runs the
        // precise kernel, so it must meet the precise bound instead.
        const double exp_rel = std::is_same_v<T, float> ? kFastExpRel : 1e-15;
        const double log_rel = std::is_same_v<T, float> ? kFastLogRel : 1e-15;
        const double sc_abs = std::is_same_v<T, float> ? kFastSinCosAbs : 1e-15;
        const double tanh_abs = std::is_same_v<T, float> ? kFastTanhAbs : 1e-15;

        auto e = measure<T>(samples<T>({"", -87, 88, false}, 21),
                            fast([&](const T* x, T* y, std::size_t n) { k.exp(x, y, n, Accuracy::fast); }),
                            [](double v) { return std::exp(v); });
        INFO("exp max rel " << e.rel);
        CHECK(e.rel <= exp_rel);

        const auto log_run =
            fast([&](const T* x, T* y, std::size_t n) { k.log(x, T(0), y, n, Accuracy::fast); });
        e = measure<T>(samples<T>({"", 1e-30, 1e30, true}, 22), log_run,
                       [](double v) { return std::log(v); });
        INFO("log wide max rel " << e.rel);
        CHECK(e.rel <= log_rel);
        e = measure<T>(samples<T>({"", 0.5, 2, false}, 23), log_run,
                       [](double v) { return std::log(v); });
        INFO("log near one max rel " << e.rel);
        CHECK(e.rel <= log_rel);
        // Points one to fifty ulps from 1, where ln(x) is tiny: the relative
        // error must not blow up there.
        std::vector<T> near_one;
        for (int j = -50; j <= 50; ++j)
            if (j != 0)
                near_one.push_back(T(1) + T(j) * std::numeric_limits<T>::epsilon() * T(0.5));
        e = measure<T>(near_one, log_run, [](double v) { return std::log(v); });
        INFO("log one-ulp neighbours max rel " << e.rel);
        CHECK(e.rel <= log_rel);

        for (const double range : {3.2, 1e3, 1e4}) {
            e = measure<T>(samples<T>({"", -range, range, false}, 24),
                           fast([&](const T* x, T* y, std::size_t n) { k.sin(x, y, n, Accuracy::fast); }),
                           [](double v) { return std::sin(v); });
            INFO("sin max abs " << e.abs << " over +-" << range);
            CHECK(e.abs <= sc_abs);
            e = measure<T>(samples<T>({"", -range, range, false}, 25),
                           fast([&](const T* x, T* y, std::size_t n) { k.cos(x, y, n, Accuracy::fast); }),
                           [](double v) { return std::cos(v); });
            INFO("cos max abs " << e.abs << " over +-" << range);
            CHECK(e.abs <= sc_abs);
        }
        e = measure<T>(samples<T>({"", -10, 10, false}, 26),
                       fast([&](const T* x, T* y, std::size_t n) { k.tanh(x, y, n, Accuracy::fast); }),
                       [](double v) { return std::tanh(v); });
        INFO("tanh max abs " << e.abs);
        CHECK(e.abs <= tanh_abs);
        e = measure<T>(samples<T>({"", -1e-3, 1e-3, false}, 27),
                       fast([&](const T* x, T* y, std::size_t n) { k.tanh(x, y, n, Accuracy::fast); }),
                       [](double v) { return std::tanh(v); });
        CHECK(e.abs <= tanh_abs);
    }
}

TEMPLATE_TEST_CASE("pulp::simd wrap_phase lands in [-pi, pi] on the right angle",
                   "[simd][math][accuracy]", float, double) {
    using T = TestType;
    const double limit = wrap_domain<T>();
    const double bound = std::is_same_v<T, float> ? kWrapAbsFloat : kWrapAbsDouble;
    const double kTwoPiHi = 6.283185307179586, kTwoPiLo = 2.4492935982947064e-16;
    for (const auto& k : all_kernels<T>()) {
        INFO("backend " << k.name);
        for (const double range : {20.0, limit}) {
            const auto x = uniform<T>(kCount, -range, range, 31);
            std::vector<T> y(x.size());
            k.wrap_phase(x.data(), y.data(), x.size());
            double worst = 0, widest = 0;
            for (std::size_t i = 0; i < x.size(); ++i) {
                const double v = x[i];
                const double m = std::nearbyint(v / (2 * kPi));
                const double exact = std::fma(-m, kTwoPiLo, std::fma(-m, kTwoPiHi, v));
                worst = std::max(worst, circular_distance(static_cast<double>(y[i]), exact));
                widest = std::max(widest, std::fabs(static_cast<double>(y[i])));
            }
            INFO("range " << range << ": worst " << worst << ", widest " << widest);
            CHECK(worst <= bound);
            CHECK(widest <= kPi * (1 + 4 * std::numeric_limits<T>::epsilon()));
        }
    }
}

TEMPLATE_TEST_CASE("pulp::simd vector math agrees with itself across layouts", "[simd][math]",
                   float, double) {
    using T = TestType;
    for (const auto& k : all_kernels<T>()) {
        for (const Accuracy acc : {Accuracy::precise, Accuracy::fast}) {
            INFO("backend " << k.name << (acc == Accuracy::fast ? " fast" : " precise"));
            const auto x = uniform<T>(kCount, -5, 5, 41);
            std::vector<T> whole(x.size()), single(x.size()), inplace = x;
            // Every function: a whole-buffer call, one-element calls (so every
            // element is a "tail"), and an in-place call give identical bits.
            const auto check = [&](const char* what, auto call) {
                call(x.data(), whole.data(), x.size());
                for (std::size_t i = 0; i < x.size(); ++i)
                    call(x.data() + i, single.data() + i, 1);
                inplace = x;
                call(inplace.data(), inplace.data(), inplace.size());
                INFO(what);
                CHECK(std::memcmp(whole.data(), single.data(), whole.size() * sizeof(T)) == 0);
                CHECK(std::memcmp(whole.data(), inplace.data(), whole.size() * sizeof(T)) == 0);
            };
            check("exp", [&](const T* a, T* b, std::size_t n) { k.exp(a, b, n, acc); });
            check("log", [&](const T* a, T* b, std::size_t n) { k.log(a, T(1e-3), b, n, acc); });
            check("sin", [&](const T* a, T* b, std::size_t n) { k.sin(a, b, n, acc); });
            check("cos", [&](const T* a, T* b, std::size_t n) { k.cos(a, b, n, acc); });
            check("tanh", [&](const T* a, T* b, std::size_t n) { k.tanh(a, b, n, acc); });
            check("sqrt", [&](const T* a, T* b, std::size_t n) { k.sqrt(a, b, n); });
            check("wrap_phase", [&](const T* a, T* b, std::size_t n) { k.wrap_phase(a, b, n); });

            // sincos matches sin and cos to the tier's accuracy.
            std::vector<T> s(x.size()), c(x.size()), s1(x.size()), c1(x.size());
            k.sincos(x.data(), s.data(), c.data(), x.size(), acc);
            k.sin(x.data(), s1.data(), x.size(), acc);
            k.cos(x.data(), c1.data(), x.size(), acc);
            const double tol = acc == Accuracy::fast ? 1e-6 : 1e-6;
            for (std::size_t i = 0; i < x.size(); ++i) {
                CHECK(std::fabs(double(s[i]) - double(s1[i])) <= tol);
                CHECK(std::fabs(double(c[i]) - double(c1[i])) <= tol);
            }
        }
    }
}

TEMPLATE_TEST_CASE("pulp::simd vector math special values", "[simd][math]", float, double) {
    using T = TestType;
    const T inf = std::numeric_limits<T>::infinity();
    const T nan = std::numeric_limits<T>::quiet_NaN();
    for (const auto& k : all_kernels<T>()) {
        INFO("backend " << k.name);
        {
            const T x[] = {T(0), -inf, inf, nan, T(1000), T(-1000), T(-1)};
            T y[7];
            k.exp(x, y, 7, Accuracy::precise);
            CHECK(y[0] == T(1));
            CHECK(y[1] == T(0));
            CHECK(y[2] == inf);
            CHECK(std::isnan(y[3]));
            CHECK(y[4] == inf);  // overflow
            CHECK(y[5] == T(0)); // underflow
            CHECK(std::fabs(y[6] - T(0.36787944117144233)) <= T(1e-6));
        }
        {
            // The floor keeps the argument positive and normal: no -inf, no NaN.
            const T x[] = {T(0), T(-5), T(1), inf, T(1e-3), nan};
            T y[6];
            k.log(x, T(1e-10), y, 6, Accuracy::precise);
            CHECK(std::isnan(y[5]));
            CHECK(std::fabs(y[0] - T(std::log(1e-10))) <= T(1e-5));
            CHECK(y[1] == y[0]);
            CHECK(y[2] == T(0));
            CHECK(y[3] == inf);
            CHECK(std::fabs(y[4] - T(std::log(1e-3))) <= T(1e-6));
            for (const T floor : {T(0), T(-1), nan}) {
                k.log(x, floor, y, 5, Accuracy::precise);
                CHECK(std::isfinite(y[0]));
                CHECK(y[0] == y[1]);
                CHECK(y[0] < T(-80));
            }
        }
        {
            const T x[] = {T(0), -inf, inf, nan, T(-4)};
            T y[5];
            k.sqrt(x, y, 5);
            CHECK(y[0] == T(0));
            CHECK(std::isnan(y[1]));
            CHECK(y[2] == inf);
            CHECK(std::isnan(y[3]));
            CHECK(std::isnan(y[4]));
        }
        {
            const T x[] = {T(0), T(30), T(-30), inf, -inf, nan};
            T y[6];
            k.tanh(x, y, 6, Accuracy::precise);
            CHECK(y[0] == T(0));
            CHECK(y[1] == T(1));
            CHECK(y[2] == T(-1));
            CHECK(y[3] == T(1));
            CHECK(y[4] == T(-1));
            CHECK(std::isnan(y[5]));
            T s[6], c[6];
            const T z[] = {T(0), nan, inf, -inf};
            k.sin(z, s, 4, Accuracy::precise);
            k.cos(z, c, 4, Accuracy::precise);
            CHECK(std::isnan(s[2]));
            CHECK(std::isnan(s[3]));
            CHECK(std::isnan(c[2]));
            CHECK(std::isnan(c[3]));
            T s2[4], c2[4];
            k.sincos(z, s2, c2, 4, Accuracy::precise);
            CHECK(std::isnan(s2[1]));
            CHECK(std::isnan(s2[2]));
            CHECK(std::isnan(c2[3]));
            CHECK(s[0] == T(0));
            CHECK(std::fabs(double(c[0]) - 1.0) <= 1e-15); // Accelerate's double cos(0) is 1 - 1.1e-16
            CHECK(std::isnan(s[1]));
            CHECK(std::isnan(c[1]));
        }
        {
            const T x[] = {T(0), T(3.14159265358979), T(-3.14159265358979), T(6.283185307179586),
                           T(7), nan, inf};
            T y[7];
            k.wrap_phase(x, y, 7);
            CHECK(y[0] == T(0));
            CHECK(std::fabs(circular_distance(y[1], kPi)) <= 1e-6);
            CHECK(std::fabs(y[3]) <= 1e-6);
            CHECK(std::fabs(double(y[4]) - (7 - 2 * kPi)) <= 1e-6);
            CHECK(std::isnan(y[5]));
            CHECK(std::isnan(y[6]));
        }
        // The fast tier is exact at the identities audio code relies on.
        {
            const T x[] = {T(0), T(1)};
            T y[2];
            k.exp(x, y, 1, Accuracy::fast);
            CHECK(std::fabs(double(y[0]) - 1.0) <= 4e-6);
            k.log(x + 1, T(0), y, 1, Accuracy::fast);
            CHECK(std::fabs(double(y[0])) <= 1e-7);
            k.sin(x, y, 1, Accuracy::fast);
            CHECK(std::fabs(double(y[0])) <= 1e-7);
            k.cos(x, y, 1, Accuracy::fast);
            CHECK(std::fabs(double(y[0]) - 1.0) <= 1e-6);
            k.tanh(x, y, 1, Accuracy::fast);
            CHECK(std::fabs(double(y[0])) <= 1e-6);
        }
    }
}

TEMPLATE_TEST_CASE("pulp::simd vector math handles subnormal input under flush-to-zero",
                   "[simd][math]", float, double) {
    using T = TestType;
    pulp::signal::ScopedFlushDenormals ftz;
    const T tiny = std::numeric_limits<T>::denorm_min() * T(5);
    for (const auto& k : all_kernels<T>()) {
        INFO("backend " << k.name);
        const T x[] = {tiny, -tiny, tiny, -tiny, tiny};
        T y[5];
        for (const Accuracy acc : {Accuracy::precise, Accuracy::fast}) {
            k.exp(x, y, 5, acc);
            for (T v : y)
                CHECK(std::fabs(double(v) - 1.0) <= 1e-6);
            k.sin(x, y, 5, acc);
            for (T v : y)
                CHECK(std::fabs(double(v)) <= 1e-30);
            k.cos(x, y, 5, acc);
            for (T v : y)
                CHECK(std::fabs(double(v) - 1.0) <= 1e-6);
            k.tanh(x, y, 5, acc);
            for (T v : y)
                CHECK(std::fabs(double(v)) <= 1e-30);
            // Subnormal input to log is lifted by the floor: finite result.
            k.log(x, T(0), y, 5, acc);
            for (T v : y)
                CHECK(std::isfinite(v));
        }
    }
}

TEMPLATE_TEST_CASE("pulp::simd vector math leaves empty ranges alone and does not allocate",
                   "[simd][math][rt-safety]", float, double) {
    using T = TestType;
    for (const auto& k : all_kernels<T>()) {
        INFO("backend " << k.name);
        const T canary = T(42);
        T x[4] = {T(1), T(2), T(3), T(4)};
        T y[4] = {canary, canary, canary, canary}, z[4] = {canary, canary, canary, canary};
        k.exp(x, y, 0, Accuracy::fast);
        k.log(x, T(0), y, 0, Accuracy::fast);
        k.sin(x, y, 0, Accuracy::precise);
        k.cos(x, y, 0, Accuracy::precise);
        k.sincos(x, y, z, 0, Accuracy::precise);
        k.tanh(x, y, 0, Accuracy::precise);
        k.sqrt(x, y, 0);
        k.wrap_phase(x, y, 0);
        for (int i = 0; i < 4; ++i) {
            CHECK(y[i] == canary);
            CHECK(z[i] == canary);
        }
        const auto in = uniform<T>(1024, -3, 3, 51);
        std::vector<T> out(1024), out2(1024);
        pulp::test::RtAllocationProbe probe;
        for (const Accuracy acc : {Accuracy::precise, Accuracy::fast}) {
            k.exp(in.data(), out.data(), in.size(), acc);
            k.log(in.data(), T(1e-9), out.data(), in.size(), acc);
            k.sin(in.data(), out.data(), in.size(), acc);
            k.cos(in.data(), out.data(), in.size(), acc);
            k.sincos(in.data(), out.data(), out2.data(), in.size(), acc);
            k.tanh(in.data(), out.data(), in.size(), acc);
        }
        k.sqrt(in.data(), out.data(), in.size());
        k.wrap_phase(in.data(), out.data(), in.size());
        CHECK(probe.allocation_count() == 0);
    }
}

// Prints the measured maxima for the documentation tables:
//   test binary "[accuracy-report]"
TEMPLATE_TEST_CASE("pulp::simd vector math accuracy report", "[.][accuracy-report]", float,
                   double) {
    using T = TestType;
    for (const auto& k : candidate_kernels<T>()) {
        for (const Accuracy acc : {Accuracy::precise, Accuracy::fast}) {
            const char* tier = acc == Accuracy::fast ? "fast" : "precise";
            const auto report = [&](const char* fn, const std::vector<T>& x, auto call, auto truth) {
                std::vector<T> y(x.size());
                call(x.data(), y.data(), x.size());
                const auto e = measure<T>(x, call, truth);
                std::printf("%-10s %-7s %-7s %-6s max %8.3g ulp  %9.3g abs  %9.3g rel\n", k.name, tier,
                            std::is_same_v<T, float> ? "float" : "double", fn, e.ulp, e.abs, e.rel);
            };
            report("exp", samples<T>({"", -87, 88, false}, 1),
                   [&](const T* x, T* y, std::size_t n) { k.exp(x, y, n, acc); },
                   [](double v) { return std::exp(v); });
            report("log", samples<T>({"", 1e-30, 1e30, true}, 3),
                   [&](const T* x, T* y, std::size_t n) { k.log(x, T(0), y, n, acc); },
                   [](double v) { return std::log(v); });
            report("sin", samples<T>({"", -1e4, 1e4, false}, 5),
                   [&](const T* x, T* y, std::size_t n) { k.sin(x, y, n, acc); },
                   [](double v) { return std::sin(v); });
            report("cos", samples<T>({"", -1e4, 1e4, false}, 6),
                   [&](const T* x, T* y, std::size_t n) { k.cos(x, y, n, acc); },
                   [](double v) { return std::cos(v); });
            report("tanh", samples<T>({"", -10, 10, false}, 7),
                   [&](const T* x, T* y, std::size_t n) { k.tanh(x, y, n, acc); },
                   [](double v) { return std::tanh(v); });
        }
    }
}
