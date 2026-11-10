// The FFT parity recipe: compare two FFT implementations on the same frames
// with assert_null_near, and prove the comparison can fail.
//
// An FFT output is complex, and assert_null_near compares audio buffers, so a
// frame becomes a two-channel buffer (real part, imaginary part) scaled by its
// peak so the dBFS tolerance reads as a relative error. Compare the implementation
// under test against a reference that is not derived from it (here a double
// precision radix-2 with long-double twiddles).
//
// The floor is proved, not assumed. A deliberately wrong candidate (conjugated
// output, the classic sign-convention slip) must fail the same assertion with
// the same tolerance; a comparison that also passed for it would measure
// nothing.
#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

using namespace pulp;
using namespace pulp::test::audio;
using namespace pulp::signal;

namespace {

using cd = std::complex<double>;

std::vector<cd> reference_dft(std::vector<cd> a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        for (std::size_t k = 0; k < len / 2; ++k) {
            const long double angle = -2.0L * 3.141592653589793238462643383279502884L *
                                      static_cast<long double>(k) / static_cast<long double>(len);
            const cd w(static_cast<double>(std::cos(angle)), static_cast<double>(std::sin(angle)));
            for (std::size_t i = 0; i < n; i += len) {
                const cd u = a[i + k];
                const cd v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
    return a;
}

std::vector<cd> recorded_frame(int n, std::uint32_t seed) {
    std::vector<cd> x(static_cast<std::size_t>(n));
    for (auto& v : x) {
        seed = seed * 1664525u + 1013904223u;
        const double re = static_cast<double>(static_cast<std::int32_t>(seed) >> 8) / 8388608.0;
        seed = seed * 1664525u + 1013904223u;
        const double im = static_cast<double>(static_cast<std::int32_t>(seed) >> 8) / 8388608.0;
        v = {re, im};
    }
    return x;
}

// A frame as a two-channel buffer (real, imaginary) divided by `peak`.
::pulp::audio::Buffer<float> as_buffer(const std::vector<cd>& v, double peak) {
    ::pulp::audio::Buffer<float> buf(2, v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        buf.channel(0)[i] = static_cast<float>(v[i].real() / peak);
        buf.channel(1)[i] = static_cast<float>(v[i].imag() / peak);
    }
    return buf;
}

double peak_of(const std::vector<cd>& v) {
    double p = 0.0;
    for (const auto& c : v)
        p = std::max(p, std::max(std::abs(c.real()), std::abs(c.imag())));
    return p;
}

constexpr double kToleranceDbfs = -110.0;

} // namespace

TEST_CASE("FftT agrees with a double reference under assert_null_near", "[signal][fft][parity]") {
    for (int n : {64, 128, 1024}) {
        for (std::uint32_t seed = 1; seed <= 5; ++seed) {
            INFO("N = " << n << " seed " << seed);
            const auto frame = recorded_frame(n, seed);
            const auto want = reference_dft(frame);

            FftT<float> fft(n);
            std::vector<std::complex<float>> got(static_cast<std::size_t>(n));
            for (std::size_t i = 0; i < got.size(); ++i)
                got[i] = {static_cast<float>(frame[i].real()), static_cast<float>(frame[i].imag())};
            fft.forward(got.data());
            std::vector<cd> got_d(got.begin(), got.end());

            const double peak = peak_of(want);
            const auto result =
                assert_null_near(as_buffer(got_d, peak), as_buffer(want, peak), kToleranceDbfs);
            INFO(result.message);
            CHECK(result.passed);
        }
    }
}

TEST_CASE("the parity recipe's floor holds: a conjugated output fails the same assertion",
          "[signal][fft][parity][negative-control]") {
    // Negative control. The wrong candidate is the right one with its
    // imaginary part negated; if this passed, the tolerance would be too loose
    // to see a sign-convention error.
    constexpr int n = 128;
    const auto frame = recorded_frame(n, 3u);
    const auto want = reference_dft(frame);

    FftT<float> fft(n);
    std::vector<std::complex<float>> got(n);
    for (int i = 0; i < n; ++i)
        got[static_cast<std::size_t>(i)] = {
            static_cast<float>(frame[static_cast<std::size_t>(i)].real()),
            static_cast<float>(frame[static_cast<std::size_t>(i)].imag())};
    fft.forward(got.data());
    std::vector<cd> conjugated;
    for (const auto& c : got)
        conjugated.push_back(std::conj(cd(c)));

    const double peak = peak_of(want);
    const auto bad =
        assert_null_near(as_buffer(conjugated, peak), as_buffer(want, peak), kToleranceDbfs);
    INFO(bad.message);
    CHECK_FALSE(bad.passed);

    // And a candidate missing the forward transform's own 1/N error class: the
    // same data scaled by 1/N, as an inverse-scaled forward would read.
    std::vector<cd> scaled;
    for (const auto& c : got)
        scaled.push_back(cd(c) / static_cast<double>(n));
    const auto bad_scale =
        assert_null_near(as_buffer(scaled, peak), as_buffer(want, peak), kToleranceDbfs);
    CHECK_FALSE(bad_scale.passed);
}
