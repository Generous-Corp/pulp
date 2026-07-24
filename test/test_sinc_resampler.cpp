// SincResampler — Kaiser-windowed sinc fractional-delay resampling.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <pulp/signal/sinc_resampler.hpp>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double kPi = 3.14159265358979323846;
std::vector<float> sine(double cycles_per_sample, int n, double phase = 0.0) {
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<size_t>(i)] =
            static_cast<float>(std::sin(2.0 * kPi * cycles_per_sample * i + phase));
    return v;
}
} // namespace

TEST_CASE("SincResampler reproduces samples at integer positions", "[signal][sinc]") {
    SincResampler rs;
    rs.build();
    auto x = sine(0.05, 400);
    for (int i = 50; i < 350; ++i)
        REQUIRE_THAT(rs.read(x.data(), 400, static_cast<double>(i)),
                     WithinAbs(x[static_cast<size_t>(i)], 1e-4f));
}

TEST_CASE("SincResampler preserves a constant (kernel ~ partition of unity)", "[signal][sinc]") {
    SincResampler rs;
    rs.build();
    std::vector<float> dc(400, 0.7f);
    for (double frac = 0.0; frac < 1.0; frac += 0.1)
        REQUIRE_THAT(rs.read(dc.data(), 400, 200.0 + frac), WithinAbs(0.7f, 2e-3f));
}

TEST_CASE("SincResampler interpolates a band-limited sine accurately", "[signal][sinc]") {
    SincResampler rs;
    rs.build();
    const double f = 0.07; // well below Nyquist
    auto x = sine(f, 600);
    double sinc_err = 0.0, lin_err = 0.0;
    int count = 0;
    for (int i = 64; i < 536; ++i) {
        const double pos = i + 0.5;
        const double truth = std::sin(2.0 * kPi * f * pos);
        const double got = rs.read(x.data(), 600, pos);
        // Linear interpolation for comparison.
        const double lin = 0.5 * (x[static_cast<size_t>(i)] + x[static_cast<size_t>(i + 1)]);
        sinc_err += (got - truth) * (got - truth);
        lin_err += (lin - truth) * (lin - truth);
        ++count;
    }
    sinc_err = std::sqrt(sinc_err / count);
    lin_err = std::sqrt(lin_err / count);
    INFO("sinc RMS err=" << sinc_err << "  linear RMS err=" << lin_err);
    REQUIRE(sinc_err < 1e-3);          // transparent
    REQUIRE(sinc_err < lin_err * 0.2); // and far better than linear
}

TEST_CASE("SincResampler apply() matches read() for a gathered neighbourhood", "[signal][sinc]") {
    SincResampler rs;
    rs.build();
    const int half = rs.half_width();
    const int taps = rs.taps();
    REQUIRE(taps == 2 * half);
    auto x = sine(0.06, 500);
    for (double frac = 0.0; frac < 1.0; frac += 0.13) {
        const int i0 = 250;
        // Gather the same neighbourhood read() uses: i0-half+1 .. i0+half.
        std::vector<float> nb(static_cast<size_t>(taps));
        for (int t = 0; t < taps; ++t)
            nb[static_cast<size_t>(t)] = x[static_cast<size_t>(i0 + t - half + 1)];
        const float via_apply = rs.apply(nb.data(), frac);
        const float via_read = rs.read(x.data(), 500, i0 + frac);
        REQUIRE_THAT(via_apply, WithinAbs(via_read, 1e-5f));
    }
}

TEST_CASE("SincResampler stays bounded at buffer edges", "[signal][sinc]") {
    SincResampler rs;
    rs.build();
    auto x = sine(0.1, 128);
    for (double pos = 0.0; pos < 127.0; pos += 0.3) {
        const float y = rs.read(x.data(), 128, pos);
        REQUIRE(std::isfinite(y));
        REQUIRE(std::abs(y) < 2.0f);
    }
}

TEST_CASE("SincResampler rejects invalid source domains", "[signal][sinc]") {
    SincResampler rs;
    rs.build();

    REQUIRE(rs.read(nullptr, 0, 0.0) == 0.0f);
    REQUIRE(rs.read(nullptr, -1, 0.0) == 0.0f);

    constexpr float sample = 0.25f;
    REQUIRE(rs.read(&sample, 1, std::numeric_limits<double>::quiet_NaN()) == 0.0f);
    REQUIRE_THAT(rs.read(&sample, 1, -1.0e300), WithinAbs(0.25f, 1.0e-6f));
    REQUIRE_THAT(rs.read(&sample, 1, 1.0e300), WithinAbs(0.25f, 1.0e-6f));
}

TEST_CASE("SincResampler clamps invalid reconstruction cutoffs", "[signal][sinc]") {
    SincResampler rs;
    rs.build(16, 512, 9.0, 2.0);
    REQUIRE(rs.cutoff() == 1.0);
    rs.build(16, 512, 9.0, 0.0);
    REQUIRE(rs.cutoff() == 1.0e-6);
    rs.build(16, 512, 9.0, std::numeric_limits<double>::quiet_NaN());
    REQUIRE(rs.cutoff() == 1.0);
}
