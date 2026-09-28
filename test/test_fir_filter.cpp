// FirFilterT on a linear history: block and per-sample output null against a
// sequential ring-buffer reference, across history compactions, block sizes
// and both element types; processing allocates nothing.

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/fir_filter.hpp>
#include <pulp/signal/oversampling_fir.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <vector>

namespace {

using pulp::signal::FirFilterT;

template <typename T> std::vector<T> noise(std::size_t n, unsigned seed, double amplitude) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(-amplitude, amplitude);
    std::vector<T> v(n);
    for (auto& x : v)
        x = T(dist(rng));
    return v;
}

/// Sequential ring-buffer FIR in long double: y[n] = sum_k h[k] * x[n - k].
template <typename T>
std::vector<long double> reference(const std::vector<T>& h, const std::vector<T>& x) {
    std::vector<long double> y(x.size(), 0.0L);
    for (std::size_t n = 0; n < x.size(); ++n) {
        long double acc = 0.0L;
        for (std::size_t k = 0; k < h.size() && k <= n; ++k)
            acc += static_cast<long double>(h[k]) * x[n - k];
        y[n] = acc;
    }
    return y;
}

/// |error| of any summation order is at most taps * eps * sum|h[k] x[n-k]|.
template <typename T>
long double bound(const std::vector<T>& h, const std::vector<T>& x, std::size_t n) {
    long double s = 0.0L;
    for (std::size_t k = 0; k < h.size() && k <= n; ++k)
        s += std::fabs(static_cast<long double>(h[k]) * x[n - k]);
    return 2.0L * (h.size() + 1) * std::numeric_limits<T>::epsilon() * s +
           std::numeric_limits<T>::min();
}

template <typename T>
void check_against_reference(const std::vector<T>& h, const std::vector<T>& x,
                             const std::vector<T>& y) {
    const auto want = reference(h, x);
    for (std::size_t n = 0; n < x.size(); ++n) {
        INFO("n = " << n);
        CHECK(std::fabs(static_cast<long double>(y[n]) - want[n]) <= bound(h, x, n));
    }
}

} // namespace

TEMPLATE_TEST_CASE("FirFilter block and per-sample output match a sequential reference",
                   "[signal][fir][simd]", float, double) {
    using T = TestType;
    // 3000 samples crosses several history compactions (every 512 appends).
    const auto x = noise<T>(3000, 11, 0.9);
    for (std::size_t taps :
         {std::size_t(1), std::size_t(3), std::size_t(8), std::size_t(9), std::size_t(64),
          std::size_t(65), std::size_t(256), std::size_t(600)}) {
        INFO("taps = " << taps);
        const auto h = noise<T>(taps, unsigned(taps), 0.3);

        FirFilterT<T> per_sample;
        per_sample.set_coefficients(h);
        std::vector<T> y1(x.size());
        for (std::size_t n = 0; n < x.size(); ++n)
            y1[n] = per_sample.process(x[n]);
        check_against_reference(h, x, y1);

        for (int block : {1, 7, 32, 128, 511, 512, 513, 1000}) {
            INFO("block = " << block);
            FirFilterT<T> blocked;
            blocked.set_coefficients(h);
            std::vector<T> y2 = x;
            for (std::size_t n = 0; n < y2.size(); n += std::size_t(block)) {
                const int count = int(std::min<std::size_t>(std::size_t(block), y2.size() - n));
                blocked.process(y2.data() + n, count);
            }
            check_against_reference(h, x, y2);
        }
    }
}

TEST_CASE("FirFilter mixed per-sample and block calls share one history", "[signal][fir][simd]") {
    const auto x = noise<float>(2048, 21, 0.9);
    const auto h = noise<float>(64, 22, 0.3);
    FirFilterT<float> fir;
    fir.set_coefficients(h);
    std::vector<float> y = x;
    std::size_t n = 0;
    int turn = 0;
    while (n < y.size()) {
        if (turn++ % 3 == 0) {
            y[n] = fir.process(y[n]);
            ++n;
        } else {
            const int count = int(std::min<std::size_t>(97, y.size() - n));
            fir.process(y.data() + n, count);
            n += std::size_t(count);
        }
    }
    check_against_reference(h, x, y);
}

// Null against the previous ring-buffer implementation's arithmetic (a
// newest-first sequential float sum). Measured residual on this corpus
// (Accelerate backend, arm64 macOS): -124.5 dBFS at 64 taps, -110.9 dBFS at
// 256 taps, most of it the float reference's own rounding. The floor sits
// about 11 dB above the worst reading. The negative control drops one tap and
// must fail it.
TEST_CASE("FirFilter output nulls against the sequential float sum", "[signal][fir][simd]") {
    constexpr double kFloorDbfs = -100.0;
    const auto x = noise<float>(4096, 31, 0.9);
    for (std::size_t taps : {std::size_t(64), std::size_t(256)}) {
        INFO("taps = " << taps);
        const auto h = noise<float>(taps, unsigned(taps) + 3, 0.25);
        std::vector<float> ring(x.size()), broken(x.size());
        for (std::size_t n = 0; n < x.size(); ++n) {
            float acc = 0.0f, acc_broken = 0.0f;
            for (std::size_t k = 0; k < taps && k <= n; ++k) {
                acc += h[k] * x[n - k];
                if (k != taps / 2)
                    acc_broken += h[k] * x[n - k];
            }
            ring[n] = acc;
            broken[n] = acc_broken;
        }
        FirFilterT<float> fir;
        fir.set_coefficients(h);
        std::vector<float> y = x;
        fir.process(y.data(), int(y.size()));

        float* ring_ch[] = {ring.data()};
        float* y_ch[] = {y.data()};
        float* broken_ch[] = {broken.data()};
        const pulp::audio::BufferView<const float> ring_view(ring_ch, 1, x.size());
        const pulp::audio::BufferView<const float> y_view(y_ch, 1, x.size());
        const pulp::audio::BufferView<const float> broken_view(broken_ch, 1, x.size());
        const auto result = pulp::test::audio::assert_null_near(ring_view, y_view, kFloorDbfs);
        INFO(result.message);
        CHECK(result.passed);
        CHECK_FALSE(pulp::test::audio::assert_null_near(ring_view, broken_view, kFloorDbfs).passed);
    }
}

TEST_CASE("FirFilter reset and coefficient replacement clear the linear history",
          "[signal][fir][simd]") {
    FirFilterT<float> fir;
    fir.set_coefficients(noise<float>(64, 41, 0.3));
    auto x = noise<float>(700, 42, 0.9);
    fir.process(x.data(), int(x.size()));
    fir.reset();
    std::vector<float> impulse(64, 0.0f);
    impulse[0] = 1.0f;
    fir.process(impulse.data(), int(impulse.size()));
    for (std::size_t k = 0; k < 64; ++k)
        CHECK(impulse[k] == fir.coefficients()[k]);

    fir.set_coefficients({0.5f, 0.25f});
    CHECK(fir.process(1.0f) == 0.5f);
    CHECK(fir.process(0.0f) == 0.25f);
    CHECK(fir.process(0.0f) == 0.0f);
}

TEMPLATE_TEST_CASE("FirFilter processing allocates nothing", "[signal][fir][rt-safety]", float,
                   double) {
    using T = TestType;
    for (std::size_t taps : {std::size_t(3), std::size_t(64), std::size_t(256)}) {
        FirFilterT<T> fir;
        fir.set_coefficients(noise<T>(taps, 51, 0.3));
        auto x = noise<T>(4096, 52, 0.9);
        pulp::test::RtAllocationProbe probe;
        for (std::size_t n = 0; n < 1500; ++n)
            x[n] = fir.process(x[n]);
        fir.process(x.data() + 1500, 2000);
        fir.process(x.data() + 3500, 596);
        fir.reset();
        CHECK(probe.allocation_count() == 0);
    }
}

TEST_CASE("FixedHalfBandFir65 matches a sequential reference across compactions",
          "[signal][fir][simd]") {
    using pulp::signal::detail::FixedHalfBandFir65;
    const auto& taps = FixedHalfBandFir65::coefficients();
    // The oldest-first window uses the table unreversed, which is only right
    // because the prototype is symmetric.
    for (std::size_t k = 0; k < taps.size(); ++k)
        CHECK(taps[k] == taps[taps.size() - 1 - k]);

    const std::vector<double> h(taps.begin(), taps.end());
    const auto x = noise<double>(1000, 61, 0.9); // crosses 15 compactions
    FixedHalfBandFir65 stage;
    std::vector<double> y(x.size());
    for (std::size_t n = 0; n < x.size(); ++n)
        y[n] = stage.process(x[n]);
    check_against_reference(h, x, y);

    stage.reset();
    CHECK(stage.process(1.0) == taps[0]);
    for (std::size_t k = 1; k < taps.size(); ++k)
        CHECK(stage.process(0.0) == taps[k]);
}

TEST_CASE("FixedHalfBandFir65 processing allocates nothing", "[signal][fir][rt-safety]") {
    pulp::signal::detail::FixedHalfBandFir65 stage;
    const auto x = noise<double>(4096, 62, 0.9);
    double sink = 0.0;
    pulp::test::RtAllocationProbe probe;
    for (double v : x)
        sink += stage.process(v);
    stage.reset();
    CHECK(probe.allocation_count() == 0);
    CHECK(std::isfinite(sink));
}
