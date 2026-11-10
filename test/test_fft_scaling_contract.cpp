// FftT conventions, pinned: the sign of the exponent, the scaling of forward
// and inverse, the data layout, and what is exact. Compiled once per backend
// (the build's own, the built-in radix-2, and muFFT where it builds), so every
// backend a host can run answers to the same contract.
//
//   forward:  X[k] = sum_n x[n] e^(-2 pi i k n / N)        no scaling
//   inverse:  x[n] = (1/N) sum_k X[k] e^(+2 pi i k n / N)  scaled by 1/N
//
// So inverse(forward(x)) == x. A caller that wants the textbook unscaled
// inverse multiplies inverse()'s output by N; for a power-of-two N that is
// exact, because 1/N and N only move the exponent.
//
// What is NOT promised: bit-identical results between backends or platforms
// for general data (vDSP, muFFT and the built-in transform round differently).
// What is promised: the same binary gives the same bits on every run and for
// every instance, results for the vectors below are exact, and the error of a
// round trip stays within the bound asserted here.
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <vector>

using namespace pulp::signal;

namespace {

template <typename T> using cx = std::complex<T>;

constexpr int kSizes[] = {4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

template <typename T> std::vector<T> test_signal(int count, std::uint32_t seed) {
    std::vector<T> x(static_cast<std::size_t>(count));
    for (auto& v : x) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<T>(static_cast<double>(static_cast<std::int32_t>(seed) >> 8) / 8388608.0);
    }
    return x;
}

template <typename T> std::vector<cx<T>> test_complex(int n, std::uint32_t seed) {
    const auto flat = test_signal<T>(2 * n, seed);
    std::vector<cx<T>> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        out[static_cast<std::size_t>(i)] = {flat[static_cast<std::size_t>(2 * i)],
                                            flat[static_cast<std::size_t>(2 * i + 1)]};
    return out;
}

template <typename T> double max_abs(const std::vector<cx<T>>& v) {
    double m = 0.0;
    for (const auto& c : v)
        m = std::max(m, static_cast<double>(std::abs(c)));
    return m;
}

// max |a - b| / max |b|
template <typename T>
double relative_error(const std::vector<cx<T>>& a, const std::vector<cx<T>>& b) {
    double err = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        err = std::max(err, static_cast<double>(std::abs(a[i] - b[i])));
    return err / max_abs(b);
}

template <typename T> bool same_bits(const std::vector<cx<T>>& a, const std::vector<cx<T>>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(cx<T>)) == 0;
}

// Round-trip error bound, relative to the largest sample. The requirement is
// 1e-6 for float; double is held to 1e-13.
template <typename T> constexpr double round_trip_bound() {
    return std::is_same_v<T, float> ? 1.0e-6 : 1.0e-13;
}

} // namespace

TEMPLATE_TEST_CASE("forward is unscaled and uses e^(-i)", "[signal][fft][contract]", float,
                   double) {
    for (int n : kSizes) {
        INFO("N = " << n);
        FftT<TestType> fft(n);
        REQUIRE(fft.ready());

        // An impulse at n = 0 has an all-ones spectrum, exactly.
        std::vector<cx<TestType>> x(static_cast<std::size_t>(n));
        x[0] = {1, 0};
        fft.forward(x.data());
        for (const auto& bin : x)
            REQUIRE(bin == cx<TestType>(1, 0));

        // A constant has all its energy in bin 0, and the bin is N (no 1/N, no
        // 1/sqrt(N)).
        std::fill(x.begin(), x.end(), cx<TestType>(1, 0));
        fft.forward(x.data());
        REQUIRE(x[0] == cx<TestType>(static_cast<TestType>(n), 0));
        for (int k = 1; k < n; ++k)
            REQUIRE(x[static_cast<std::size_t>(k)] == cx<TestType>(0, 0));

        // An alternating signal lands in the Nyquist bin.
        for (int i = 0; i < n; ++i)
            x[static_cast<std::size_t>(i)] = {(i & 1) ? TestType(-1) : TestType(1), 0};
        fft.forward(x.data());
        REQUIRE(x[static_cast<std::size_t>(n / 2)] == cx<TestType>(static_cast<TestType>(n), 0));
        REQUIRE(x[0] == cx<TestType>(0, 0));

        // A unit delay by one sample multiplies bin k by e^(-2 pi i k / N): the
        // sign of the exponent is negative, so bin 1 has a NEGATIVE imaginary part.
        std::fill(x.begin(), x.end(), cx<TestType>(0, 0));
        x[1] = {1, 0};
        fft.forward(x.data());
        const double w = 2.0 * std::numbers::pi / n;
        const double tol = std::is_same_v<TestType, float> ? 1e-6 : 1e-14;
        for (int k = 0; k < n; ++k) {
            const auto got = x[static_cast<std::size_t>(k)];
            CHECK(std::abs(static_cast<double>(got.real()) - std::cos(w * k)) < tol);
            CHECK(std::abs(static_cast<double>(got.imag()) + std::sin(w * k)) < tol);
        }
        CHECK(x[1].imag() < TestType(0));
    }
}

TEMPLATE_TEST_CASE("a single complex exponential lands in one bin with amplitude N",
                   "[signal][fft][contract]", float, double) {
    constexpr int n = 128;
    FftT<TestType> fft(n);
    const double tol = (std::is_same_v<TestType, float> ? 1e-5 : 1e-12) * n;
    for (int k0 : {1, 5, 31, 64, 100}) {
        INFO("bin " << k0);
        std::vector<cx<TestType>> x(n);
        for (int i = 0; i < n; ++i) {
            const double a = 2.0 * std::numbers::pi * k0 * i / n;
            x[static_cast<std::size_t>(i)] = {static_cast<TestType>(std::cos(a)),
                                              static_cast<TestType>(std::sin(a))};
        }
        fft.forward(x.data());
        for (int k = 0; k < n; ++k) {
            const double expect = k == k0 ? n : 0.0;
            CHECK(std::abs(static_cast<double>(x[static_cast<std::size_t>(k)].real()) - expect) <
                  tol);
            CHECK(std::abs(static_cast<double>(x[static_cast<std::size_t>(k)].imag())) < tol);
        }
    }
}

TEMPLATE_TEST_CASE("inverse is scaled by 1/N, and times N it is the textbook unscaled inverse",
                   "[signal][fft][contract]", float, double) {
    for (int n : kSizes) {
        INFO("N = " << n);
        FftT<TestType> fft(n);

        // A spectrum of N at bin 0 is a constant 1, exactly: the 1/N is applied.
        std::vector<cx<TestType>> x(static_cast<std::size_t>(n));
        x[0] = {static_cast<TestType>(n), 0};
        fft.inverse(x.data());
        for (const auto& s : x)
            REQUIRE(s == cx<TestType>(1, 0));

        // A spectrum of 1 at bin 0 is 1/N everywhere; times N it is the
        // unscaled inverse (all ones), exactly, because N is a power of two.
        std::fill(x.begin(), x.end(), cx<TestType>(0, 0));
        x[0] = {1, 0};
        fft.inverse(x.data());
        for (const auto& s : x) {
            REQUIRE(s == cx<TestType>(TestType(1) / static_cast<TestType>(n), 0));
            REQUIRE(s * static_cast<TestType>(n) == cx<TestType>(1, 0));
        }
    }
}

TEMPLATE_TEST_CASE("scaling the input by a power of two only moves the exponent",
                   "[signal][fft][contract]", float, double) {
    // Linearity with an exact scale: forward and inverse of 2^k * x are exactly
    // 2^k times the transform of x. This is what makes N * inverse() exact.
    for (int n : kSizes) {
        INFO("N = " << n);
        FftT<TestType> fft(n);
        const auto base = test_complex<TestType>(n, 11u + static_cast<std::uint32_t>(n));
        for (int shift : {-3, 1, 5}) {
            const TestType s = std::ldexp(TestType(1), shift);
            auto a = base;
            auto b = base;
            for (auto& v : b)
                v *= s;
            fft.forward(a.data());
            fft.forward(b.data());
            for (auto& v : a)
                v *= s;
            REQUIRE(same_bits(a, b));

            auto c = base;
            auto d = base;
            for (auto& v : d)
                v *= s;
            fft.inverse(c.data());
            fft.inverse(d.data());
            for (auto& v : c)
                v *= s;
            REQUIRE(same_bits(c, d));
        }
    }
}

TEMPLATE_TEST_CASE("forward then inverse returns the input", "[signal][fft][contract]", float,
                   double) {
    for (int n : kSizes) {
        INFO("N = " << n);
        FftT<TestType> fft(n);
        double worst = 0.0;
        for (std::uint32_t seed = 1; seed <= 8; ++seed) {
            const auto x = test_complex<TestType>(n, seed * 7919u + static_cast<std::uint32_t>(n));
            auto y = x;
            fft.forward(y.data());
            fft.inverse(y.data());
            worst = std::max(worst, relative_error(y, x));
        }
        CHECK(worst < round_trip_bound<TestType>());
    }
}

TEMPLATE_TEST_CASE("a short real waveform round trips at N = 128", "[signal][fft][contract]", float,
                   double) {
    constexpr int n = 128;
    FftT<TestType> fft(n);
    const auto x = test_signal<TestType>(n, 99u);

    // Complex path: real input, imaginary part zero, back to the same real.
    std::vector<cx<TestType>> c(n);
    for (int i = 0; i < n; ++i)
        c[static_cast<std::size_t>(i)] = {x[static_cast<std::size_t>(i)], 0};
    const auto original = c;
    fft.forward(c.data());
    fft.inverse(c.data());
    CHECK(relative_error(c, original) < round_trip_bound<TestType>());
    for (const auto& v : c)
        CHECK(std::abs(static_cast<double>(v.imag())) < 10.0 * round_trip_bound<TestType>());

    // Half-spectrum path: N/2 + 1 bins, unscaled forward (scale 1), 1/N inverse.
    std::vector<cx<TestType>> half(n / 2 + 1);
    fft.forward_real_half(x.data(), half.data());
    double sum = 0.0;
    for (TestType v : x)
        sum += static_cast<double>(v);
    CHECK(std::abs(static_cast<double>(half[0].real()) - sum) <
          (std::is_same_v<TestType, float> ? 1e-4 : 1e-12));
    std::vector<TestType> back(n);
    fft.inverse_real_half(half.data(), back.data());
    double err = 0.0;
    double peak = 0.0;
    for (int i = 0; i < n; ++i) {
        err = std::max(err, std::abs(static_cast<double>(back[static_cast<std::size_t>(i)]) -
                                     static_cast<double>(x[static_cast<std::size_t>(i)])));
        peak = std::max(peak, std::abs(static_cast<double>(x[static_cast<std::size_t>(i)])));
    }
    CHECK(err / peak < round_trip_bound<TestType>());
}

TEMPLATE_TEST_CASE("Parseval: forward energy is N times the time-domain energy",
                   "[signal][fft][contract]", float, double) {
    for (int n : {64, 128, 1024}) {
        FftT<TestType> fft(n);
        auto x = test_complex<TestType>(n, 3u);
        double time_energy = 0.0;
        for (const auto& v : x)
            time_energy += std::norm(std::complex<double>(v));
        fft.forward(x.data());
        double freq_energy = 0.0;
        for (const auto& v : x)
            freq_energy += std::norm(std::complex<double>(v));
        CHECK(std::abs(freq_energy / (n * time_energy) - 1.0) <
              (std::is_same_v<TestType, float> ? 1e-5 : 1e-12));
    }
}

TEMPLATE_TEST_CASE("the same binary gives the same bits every time", "[signal][fft][determinism]",
                   float, double) {
    for (int n : {64, 128, 1024, 4096}) {
        INFO("N = " << n);
        const auto x = test_complex<TestType>(n, 12345u);

        FftT<TestType> first(n);
        auto want_fwd = x;
        first.forward(want_fwd.data());
        auto want_inv = want_fwd;
        first.inverse(want_inv.data());

        // Repeated runs on one instance, and fresh instances, agree to the bit.
        for (int run = 0; run < 20; ++run) {
            FftT<TestType> fresh(n);
            FftT<TestType>& fft = (run & 1) ? first : fresh;
            auto fwd = x;
            fft.forward(fwd.data());
            REQUIRE(same_bits(fwd, want_fwd));
            auto inv = fwd;
            fft.inverse(inv.data());
            REQUIRE(same_bits(inv, want_inv));
        }

        // What an earlier call left in the instance does not leak into the next.
        auto noise = test_complex<TestType>(n, 777u);
        first.forward(noise.data());
        auto again = x;
        first.forward(again.data());
        REQUIRE(same_bits(again, want_fwd));
    }
}
