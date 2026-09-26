// Backend parity for every public pulp::simd kernel.
//
// Each compiled backend (Highway always; Accelerate on Apple) and the
// configure-time default are run against the scalar reference backend over
// ragged lengths, both element types, and flush-to-zero on and off.
//
// Tolerances, per kernel class:
//   - Elementwise ops with one rounding (add, mul, set, scale, abs, clamp) and
//     order-free reductions (maximum, minimum, max_abs) must match bit for
//     bit: every backend rounds each element once, the same way.
//   - Multiply-adds (fma, add_scaled) may be fused on one side and not the
//     other: at most one extra rounding of the product, bounded by
//     2 * eps * (|a*b| + |c|).
//   - Reductions (sum, dot, sum_squares, correlate, decimate2) accumulate in a
//     backend-specific order. Any order's error is bounded by
//     n * eps * sum|terms| (standard forward bound for recursive summation),
//     so two orders differ by at most 2 * n * eps * sum|terms|.
//   - ramp_mul: the gain start + i * step is formed in the backend's own way
//     (Accelerate advances it by repeated addition), bounded by
//     (i + 2) * eps * (|start| + i * |step|) * |x|.
// Block outputs are additionally held to an assert_null_near floor set from
// the residual measured on this corpus, and each floor has a negative control
// proving it rejects a one-tap defect.

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>
#include <pulp/simd/simd.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

namespace backend = pulp::simd::backend;

template <typename T> struct Kernels {
    const char* name;
    void (*add)(const T*, const T*, T*, std::size_t) noexcept;
    void (*mul)(const T*, const T*, T*, std::size_t) noexcept;
    void (*fma)(const T*, const T*, const T*, T*, std::size_t) noexcept;
    void (*set)(T, T*, std::size_t) noexcept;
    void (*scale)(const T*, T, T*, std::size_t) noexcept;
    void (*add_scaled)(const T*, T, T*, std::size_t) noexcept;
    T (*sum)(const T*, std::size_t) noexcept;
    T (*maximum)(const T*, std::size_t) noexcept;
    T (*minimum)(const T*, std::size_t) noexcept;
    void (*abs)(const T*, T*, std::size_t) noexcept;
    void (*clamp)(const T*, T, T, T*, std::size_t) noexcept;
    T (*dot)(const T*, const T*, std::size_t) noexcept;
    T (*sum_squares)(const T*, std::size_t) noexcept;
    T (*max_abs)(const T*, std::size_t) noexcept;
    void (*ramp_mul)(const T*, T, T, T*, std::size_t) noexcept;
    void (*correlate)(const T*, const T*, T*, std::size_t, std::size_t) noexcept;
    void (*decimate2)(const T*, const T*, T*, std::size_t, std::size_t) noexcept;
};

#define PULP_TEST_KERNELS(NAME, NS)                                                                \
    Kernels<T> {                                                                                   \
        NAME, &NS::add, &NS::mul, &NS::fma, &NS::set, &NS::scale, &NS::add_scaled, &NS::sum,       \
            &NS::maximum, &NS::minimum, &NS::abs, &NS::clamp, &NS::dot, &NS::sum_squares,          \
            &NS::max_abs, &NS::ramp_mul, &NS::correlate, &NS::decimate2                            \
    }

template <typename T> Kernels<T> reference_kernels() {
    return PULP_TEST_KERNELS("scalar", backend::scalar);
}

/// Every non-reference backend compiled into this binary, plus the
/// unqualified pulp::simd names (the configure-time default).
template <typename T> std::vector<Kernels<T>> candidate_kernels() {
    std::vector<Kernels<T>> out;
#if defined(PULP_SIMD_HAS_HIGHWAY)
    out.push_back(PULP_TEST_KERNELS("highway", backend::highway));
#endif
#if defined(PULP_SIMD_HAS_ACCELERATE)
    out.push_back(PULP_TEST_KERNELS("accelerate", backend::accelerate));
#endif
    out.push_back(PULP_TEST_KERNELS("default", pulp::simd));
    return out;
}

constexpr std::size_t kLengths[] = {0, 1, 2, 3, 7, 8, 15, 16, 17, 31, 64, 65, 100, 255, 512, 1023};
constexpr std::size_t kTaps[] = {1, 2, 3, 7, 16, 31, 63, 64, 65, 128, 255, 256};

template <typename T> std::vector<T> noise(std::size_t n, unsigned seed, T amplitude = T(0.9)) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::vector<T> v(n);
    for (auto& x : v)
        x = T(dist(rng)) * amplitude;
    return v;
}

/// Replaces every fifth sample with a subnormal value so flush-to-zero on and
/// off take different paths through every kernel.
template <typename T> void sprinkle_subnormals(std::vector<T>& v) {
    const T tiny = std::numeric_limits<T>::denorm_min() * T(3);
    for (std::size_t i = 0; i < v.size(); i += 5)
        v[i] = (i & 1) ? -tiny : tiny;
}

template <typename T> constexpr T eps() {
    return std::numeric_limits<T>::epsilon();
}

/// Under flush-to-zero a subnormal operand or result may become zero in one
/// backend and survive in another; allow that much absolute slack per term.
template <typename T> constexpr T subnormal_slack() {
    return std::numeric_limits<T>::min() * T(4);
}

template <typename T> bool same_bits(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
}

template <typename T> T sum_abs_products(const T* a, const T* b, std::size_t n) {
    long double acc = 0;
    for (std::size_t i = 0; i < n; ++i)
        acc += std::fabs(static_cast<long double>(a[i]) * b[i]);
    return static_cast<T>(acc);
}

template <typename T> T reduction_bound(std::size_t n, T sum_abs_terms) {
    return T(2) * T(n + 1) * eps<T>() * sum_abs_terms + T(n + 1) * subnormal_slack<T>();
}

template <typename T>
void check_elementwise(const Kernels<T>& ref, const Kernels<T>& k, std::size_t n, unsigned seed,
                       bool subnormals) {
    auto a = noise<T>(n, seed), b = noise<T>(n, seed + 1), c = noise<T>(n, seed + 2);
    if (subnormals) {
        sprinkle_subnormals(a);
        sprinkle_subnormals(c);
    }
    std::vector<T> want(n), got(n);

    ref.add(a.data(), b.data(), want.data(), n);
    k.add(a.data(), b.data(), got.data(), n);
    CHECK(same_bits(want, got));

    ref.mul(a.data(), b.data(), want.data(), n);
    k.mul(a.data(), b.data(), got.data(), n);
    CHECK(same_bits(want, got));

    ref.set(T(0.375), want.data(), n);
    k.set(T(0.375), got.data(), n);
    CHECK(same_bits(want, got));

    ref.scale(a.data(), T(-1.25), want.data(), n);
    k.scale(a.data(), T(-1.25), got.data(), n);
    CHECK(same_bits(want, got));

    ref.abs(a.data(), want.data(), n);
    k.abs(a.data(), got.data(), n);
    CHECK(same_bits(want, got));

    // The scalar clamp copies a subnormal through untouched; a vector min/max
    // under flush-to-zero returns it as zero. Bit-exact otherwise.
    ref.clamp(a.data(), T(-0.5), T(0.25), want.data(), n);
    k.clamp(a.data(), T(-0.5), T(0.25), got.data(), n);
    if (subnormals) {
        for (std::size_t i = 0; i < n; ++i)
            CHECK(std::fabs(want[i] - got[i]) <= subnormal_slack<T>());
    } else {
        CHECK(same_bits(want, got));
    }

    ref.fma(a.data(), b.data(), c.data(), want.data(), n);
    k.fma(a.data(), b.data(), c.data(), got.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
        const T bound =
            T(2) * eps<T>() * (std::fabs(a[i] * b[i]) + std::fabs(c[i])) + subnormal_slack<T>();
        CHECK(std::fabs(want[i] - got[i]) <= bound);
    }

    want = c;
    got = c;
    ref.add_scaled(a.data(), T(0.7), want.data(), n);
    k.add_scaled(a.data(), T(0.7), got.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
        const T bound =
            T(2) * eps<T>() * (std::fabs(a[i] * T(0.7)) + std::fabs(c[i])) + subnormal_slack<T>();
        CHECK(std::fabs(want[i] - got[i]) <= bound);
    }

    // In-place aliasing where the contract allows it.
    want = a;
    got = a;
    ref.scale(want.data(), T(3), want.data(), n);
    k.scale(got.data(), T(3), got.data(), n);
    CHECK(same_bits(want, got));
    want = a;
    got = a;
    ref.add(want.data(), b.data(), want.data(), n);
    k.add(got.data(), b.data(), got.data(), n);
    CHECK(same_bits(want, got));
}

template <typename T>
void check_reductions(const Kernels<T>& ref, const Kernels<T>& k, std::size_t n, unsigned seed,
                      bool subnormals) {
    auto a = noise<T>(n, seed), b = noise<T>(n, seed + 7);
    if (subnormals)
        sprinkle_subnormals(a);
    std::vector<T> ones(n, T(1));

    CHECK(std::fabs(ref.sum(a.data(), n) - k.sum(a.data(), n)) <=
          reduction_bound(n, sum_abs_products(a.data(), ones.data(), n)));
    CHECK(std::fabs(ref.dot(a.data(), b.data(), n) - k.dot(a.data(), b.data(), n)) <=
          reduction_bound(n, sum_abs_products(a.data(), b.data(), n)));
    CHECK(std::fabs(ref.sum_squares(a.data(), n) - k.sum_squares(a.data(), n)) <=
          reduction_bound(n, sum_abs_products(a.data(), a.data(), n)));

    CHECK(ref.maximum(a.data(), n) == k.maximum(a.data(), n));
    CHECK(ref.minimum(a.data(), n) == k.minimum(a.data(), n));
    CHECK(ref.max_abs(a.data(), n) == k.max_abs(a.data(), n));
}

template <typename T>
void check_ramp(const Kernels<T>& ref, const Kernels<T>& k, std::size_t n, unsigned seed) {
    const auto x = noise<T>(n, seed);
    std::vector<T> want(n), got(n);
    const T start = T(0.2);
    const T step = n ? T(0.9) / T(n) : T(0);
    ref.ramp_mul(x.data(), start, step, want.data(), n);
    k.ramp_mul(x.data(), start, step, got.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
        const T gain_mag = std::fabs(start) + T(i) * std::fabs(step);
        const T bound = T(i + 2) * eps<T>() * gain_mag * std::fabs(x[i]) + subnormal_slack<T>();
        CHECK(std::fabs(want[i] - got[i]) <= bound);
    }
    // In place.
    got = x;
    k.ramp_mul(got.data(), start, step, got.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
        const T gain_mag = std::fabs(start) + T(i) * std::fabs(step);
        CHECK(std::fabs(want[i] - got[i]) <=
              T(i + 2) * eps<T>() * gain_mag * std::fabs(x[i]) + subnormal_slack<T>());
    }
}

template <typename T>
void check_fir(const Kernels<T>& ref, const Kernels<T>& k, std::size_t n_out, std::size_t taps,
               unsigned seed, bool subnormals) {
    auto x = noise<T>(2 * n_out + taps, seed);
    const auto h = noise<T>(taps, seed + 3, T(0.25));
    if (subnormals)
        sprinkle_subnormals(x);
    std::vector<T> want(n_out), got(n_out, T(123));

    ref.correlate(x.data(), h.data(), want.data(), n_out, taps);
    k.correlate(x.data(), h.data(), got.data(), n_out, taps);
    for (std::size_t i = 0; i < n_out; ++i)
        CHECK(std::fabs(want[i] - got[i]) <=
              reduction_bound(taps, sum_abs_products(x.data() + i, h.data(), taps)));

    std::fill(got.begin(), got.end(), T(123));
    ref.decimate2(x.data(), h.data(), want.data(), n_out, taps);
    k.decimate2(x.data(), h.data(), got.data(), n_out, taps);
    for (std::size_t i = 0; i < n_out; ++i)
        CHECK(std::fabs(want[i] - got[i]) <=
              reduction_bound(taps, sum_abs_products(x.data() + 2 * i, h.data(), taps)));
}

template <typename T> void run_parity(bool subnormals) {
    const auto ref = reference_kernels<T>();
    for (const auto& k : candidate_kernels<T>()) {
        INFO("backend " << k.name << (subnormals ? " with subnormals" : ""));
        unsigned seed = 1;
        for (std::size_t n : kLengths) {
            INFO("n = " << n);
            check_elementwise(ref, k, n, seed, subnormals);
            check_reductions(ref, k, n, seed, subnormals);
            check_ramp(ref, k, n, seed);
            seed += 11;
        }
        for (std::size_t taps : kTaps) {
            for (std::size_t n_out : {std::size_t(1), std::size_t(5), std::size_t(33),
                                      std::size_t(128), std::size_t(517)}) {
                INFO("taps = " << taps << ", n_out = " << n_out);
                check_fir(ref, k, n_out, taps, seed++, subnormals);
            }
        }
    }
}

} // namespace

TEMPLATE_TEST_CASE("pulp::simd backends match the scalar reference", "[simd][parity]", float,
                   double) {
    SECTION("flush-to-zero off") {
        run_parity<TestType>(false);
    }
    SECTION("flush-to-zero on") {
        pulp::signal::ScopedFlushDenormals ftz;
        run_parity<TestType>(true);
    }
}

TEMPLATE_TEST_CASE("pulp::simd empty ranges and zero-tap filters", "[simd][parity]", float,
                   double) {
    using T = TestType;
    const auto ref = reference_kernels<T>();
    auto all = candidate_kernels<T>();
    all.push_back(ref);
    for (const auto& k : all) {
        INFO("backend " << k.name);
        const T canary = T(42);
        T x[4] = {T(1), T(-2), T(3), T(-4)};
        T y[4] = {canary, canary, canary, canary};
        CHECK(k.sum(x, 0) == T(0));
        CHECK(k.dot(x, x, 0) == T(0));
        CHECK(k.sum_squares(x, 0) == T(0));
        CHECK(k.maximum(x, 0) == T(0));
        CHECK(k.minimum(x, 0) == T(0));
        CHECK(k.max_abs(x, 0) == T(0));
        k.add(x, x, y, 0);
        k.correlate(x, x, y, 0, 3);
        k.decimate2(x, x, y, 0, 3);
        k.ramp_mul(x, T(1), T(1), y, 0);
        for (T v : y)
            CHECK(v == canary);
        k.correlate(x, x, y, 4, 0);
        for (T v : y)
            CHECK(v == T(0));
        std::fill(y, y + 4, canary);
        k.decimate2(x, x, y, 2, 0);
        CHECK(y[0] == T(0));
        CHECK(y[1] == T(0));
        CHECK(y[2] == canary);
    }
}

TEST_CASE("pulp::simd unqualified names are the configured backend", "[simd][parity]") {
    using DotF = float (*)(const float*, const float*, std::size_t) noexcept;
    const DotF active = &pulp::simd::dot;
#if defined(PULP_SIMD_DEFAULT_BACKEND_ACCELERATE)
    CHECK(std::string(pulp::simd::active_backend_name) == "accelerate");
    CHECK(active == static_cast<DotF>(&backend::accelerate::dot));
#elif defined(PULP_SIMD_DEFAULT_BACKEND_HIGHWAY)
    CHECK(std::string(pulp::simd::active_backend_name) == "highway");
    CHECK(active == static_cast<DotF>(&backend::highway::dot));
#else
    CHECK(std::string(pulp::simd::active_backend_name) == "scalar");
    CHECK(active == static_cast<DotF>(&backend::scalar::dot));
#endif
}

// assert_null_near floor for the float FIR kernel. Measured on this corpus
// (amplitude 0.9 input, 0.25 taps, arm64 macOS): worst residual -122.9 dBFS
// for Highway and -115.6 dBFS for Accelerate at 256 taps, -126 to -133 dBFS at
// 64/65 taps. The floor keeps 10 dB above the worst reading for other ISAs and
// OS releases. The negative control perturbs one tap by 1e-3 (a residual near
// -60 dBFS) and must fail the same floor, so the floor is proven able to see a
// one-tap defect rather than assumed.
TEST_CASE("pulp::simd float FIR kernels null against the scalar reference", "[simd][parity]") {
    constexpr double kFloorDbfs = -105.0;
    const auto ref = reference_kernels<float>();
    const std::size_t n_out = 512;
    for (std::size_t taps : {std::size_t(64), std::size_t(65), std::size_t(256)}) {
        const auto x = noise<float>(2 * n_out + taps, unsigned(taps));
        auto h = noise<float>(taps, unsigned(taps) + 1, 0.25f);
        std::vector<float> want(n_out), got(n_out), broken(n_out);
        ref.correlate(x.data(), h.data(), want.data(), n_out, taps);
        auto h_broken = h;
        h_broken[taps / 2] += 1e-3f;
        ref.correlate(x.data(), h_broken.data(), broken.data(), n_out, taps);

        float* want_ch[] = {want.data()};
        float* got_ch[] = {got.data()};
        float* broken_ch[] = {broken.data()};
        const pulp::audio::BufferView<const float> want_view(want_ch, 1, n_out);
        const pulp::audio::BufferView<const float> got_view(got_ch, 1, n_out);
        const pulp::audio::BufferView<const float> broken_view(broken_ch, 1, n_out);

        INFO("taps = " << taps);
        CHECK_FALSE(pulp::test::audio::assert_null_near(want_view, broken_view, kFloorDbfs).passed);
        for (const auto& k : candidate_kernels<float>()) {
            INFO("backend " << k.name);
            k.correlate(x.data(), h.data(), got.data(), n_out, taps);
            const auto result =
                pulp::test::audio::assert_null_near(want_view, got_view, kFloorDbfs);
            INFO(result.message);
            CHECK(result.passed);
        }
    }
}

// How much a per-output kernel's value may depend on how the outputs are
// split into calls. The scalar and Highway backends accumulate each output in
// a fixed order wherever it falls, so a FIR filter or convolver head fed
// ragged host blocks renders the same bits as one fed full blocks. Accelerate's
// vDSP_conv / vDSP_desamp block their outputs internally and are not
// partition-invariant; they are held to the reduction bound instead.
TEMPLATE_TEST_CASE("pulp::simd correlate and decimate2 are block-partition invariant",
                   "[simd][parity]", float, double) {
    using T = TestType;
    auto all = candidate_kernels<T>();
    all.push_back(reference_kernels<T>());
    const std::size_t n_out = 700;
    for (std::size_t taps : {std::size_t(5), std::size_t(64), std::size_t(257)}) {
        const auto x = noise<T>(2 * n_out + taps, unsigned(taps) + 90);
        const auto h = noise<T>(taps, unsigned(taps) + 91, T(0.25));
        for (const auto& k : all) {
            INFO("backend " << k.name << ", taps = " << taps);
            std::vector<T> whole(n_out), split(n_out);
            k.correlate(x.data(), h.data(), whole.data(), n_out, taps);
            std::size_t pos = 0, turn = 0;
            const std::size_t sizes[] = {1, 7, 64, 33, 128, 5, 3};
            while (pos < n_out) {
                const std::size_t m = std::min(sizes[turn++ % 7], n_out - pos);
                k.correlate(x.data() + pos, h.data(), split.data() + pos, m, taps);
                pos += m;
            }
            const bool bit_exact =
                std::string(k.name) != "accelerate" && std::string(k.name) != "default";
            const bool default_is_accelerate =
                std::string(pulp::simd::active_backend_name) == "accelerate";
            const bool expect_bits =
                bit_exact || (std::string(k.name) == "default" && !default_is_accelerate);
            if (expect_bits) {
                CHECK(same_bits(whole, split));
            } else {
                for (std::size_t i = 0; i < n_out; ++i)
                    CHECK(std::fabs(whole[i] - split[i]) <=
                          reduction_bound(taps, sum_abs_products(x.data() + i, h.data(), taps)));
            }

            const std::size_t n_dec = n_out / 2;
            std::vector<T> dwhole(n_dec), dsplit(n_dec);
            k.decimate2(x.data(), h.data(), dwhole.data(), n_dec, taps);
            pos = 0;
            turn = 0;
            while (pos < n_dec) {
                const std::size_t m = std::min(sizes[turn++ % 7], n_dec - pos);
                k.decimate2(x.data() + 2 * pos, h.data(), dsplit.data() + pos, m, taps);
                pos += m;
            }
            if (expect_bits) {
                CHECK(same_bits(dwhole, dsplit));
            } else {
                for (std::size_t i = 0; i < n_dec; ++i)
                    CHECK(
                        std::fabs(dwhole[i] - dsplit[i]) <=
                        reduction_bound(taps, sum_abs_products(x.data() + 2 * i, h.data(), taps)));
            }
        }
    }
}

TEMPLATE_TEST_CASE("pulp::simd kernels do not allocate", "[simd][rt-safety]", float, double) {
    using T = TestType;
    auto all = candidate_kernels<T>();
    all.push_back(reference_kernels<T>());
    const std::size_t n = 1024;
    auto a = noise<T>(n + 256, 5), b = noise<T>(n + 256, 6);
    std::vector<T> dst(n);
    for (const auto& k : all) {
        INFO("backend " << k.name);
        pulp::test::RtAllocationProbe probe;
        T sink = T(0);
        k.add(a.data(), b.data(), dst.data(), n);
        k.mul(a.data(), b.data(), dst.data(), n);
        k.fma(a.data(), b.data(), a.data(), dst.data(), n);
        k.set(T(1), dst.data(), n);
        k.scale(a.data(), T(2), dst.data(), n);
        k.add_scaled(a.data(), T(2), dst.data(), n);
        sink += k.sum(a.data(), n) + k.maximum(a.data(), n) + k.minimum(a.data(), n);
        k.abs(a.data(), dst.data(), n);
        k.clamp(a.data(), T(-0.1), T(0.1), dst.data(), n);
        sink += k.dot(a.data(), b.data(), n) + k.sum_squares(a.data(), n) + k.max_abs(a.data(), n);
        k.ramp_mul(a.data(), T(0), T(0.001), dst.data(), n);
        k.correlate(a.data(), b.data(), dst.data(), n, 256);
        k.decimate2(a.data(), b.data(), dst.data(), n / 2, 255);
        CHECK(probe.allocation_count() == 0);
        CHECK(std::isfinite(sink));
    }
}
