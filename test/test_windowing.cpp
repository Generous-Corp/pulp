#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/stft.hpp>
#include <pulp/signal/windowing.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <numeric>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::Stft;
using pulp::signal::StftConfig;
using pulp::signal::WindowFunction;

namespace {

struct WindowMetrics {
    double coherent_gain = 0.0;
    double enbw_bins = 0.0;
};

WindowMetrics metrics(const std::vector<double>& window) {
    const double sum = std::accumulate(window.begin(), window.end(), 0.0);
    const double sum_squares =
        std::inner_product(window.begin(), window.end(), window.begin(), 0.0);
    const double count = static_cast<double>(window.size());
    return {sum / count, count * sum_squares / (sum * sum)};
}

double maximum_sidelobe_db(const std::vector<double>& window) {
    const double dc = std::accumulate(window.begin(), window.end(), 0.0);
    const double count = static_cast<double>(window.size());
    double maximum = 0.0;

    // Both four-term windows have a four-bin-wide main lobe. Sampling from
    // 4.1 bins outward avoids mistaking its skirt for a side lobe while the
    // 0.01-bin spacing resolves the equiripple peaks comfortably.
    for (double bin = 4.1; bin <= 64.0; bin += 0.01) {
        std::complex<double> response{0.0, 0.0};
        for (std::size_t n = 0; n < window.size(); ++n) {
            const double phase = -2.0 * std::numbers::pi * bin * static_cast<double>(n) / count;
            response += window[n] * std::polar(1.0, phase);
        }
        maximum = std::max(maximum, std::abs(response) / dc);
    }
    return 20.0 * std::log10(maximum);
}

double normalized_far_sidelobe(WindowFunction::Type type) {
    constexpr int kSize = 1024;
    constexpr int kToneBin = 37;
    constexpr double kFractionalBin = 0.5;

    StftConfig config;
    config.fft_size = kSize;
    config.hop_size = kSize;
    config.window = type;
    Stft stft(config);

    std::vector<float> signal(kSize);
    for (int i = 0; i < kSize; ++i) {
        const double phase = 2.0 * std::numbers::pi * (kToneBin + kFractionalBin) * i / kSize;
        signal[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(phase));
    }
    REQUIRE(stft.push_samples(signal.data(), kSize));

    const auto& magnitude = stft.latest_frame().magnitude;
    const float peak = *std::max_element(magnitude.begin(), magnitude.end());
    double far_peak = 0.0;
    for (int bin = 1; bin < static_cast<int>(magnitude.size()); ++bin) {
        if (std::abs(bin - kToneBin) <= 6)
            continue;
        far_peak =
            std::max(far_peak, static_cast<double>(magnitude[static_cast<std::size_t>(bin)]));
    }
    return far_peak / peak;
}

} // namespace

static_assert(static_cast<int>(WindowFunction::Type::rectangular) == 0);
static_assert(static_cast<int>(WindowFunction::Type::hann) == 1);
static_assert(static_cast<int>(WindowFunction::Type::hamming) == 2);
static_assert(static_cast<int>(WindowFunction::Type::blackman) == 3);
static_assert(static_cast<int>(WindowFunction::Type::flat_top) == 4);
static_assert(static_cast<int>(WindowFunction::Type::kaiser) == 5);

TEST_CASE("Four-term window coefficients match their published cosine sums", "[signal][window]") {
    const auto blackman_harris =
        WindowFunction::generate<double>(5, WindowFunction::Type::blackman_harris);
    REQUIRE_THAT(blackman_harris[0], WithinAbs(0.00006, 1e-7));
    REQUIRE_THAT(blackman_harris[1], WithinAbs(0.21747, 1e-7));
    REQUIRE_THAT(blackman_harris[2], WithinAbs(1.0, 1e-7));
    REQUIRE_THAT(blackman_harris[3], WithinAbs(0.21747, 1e-7));
    REQUIRE_THAT(blackman_harris[4], WithinAbs(0.00006, 1e-7));

    const auto blackman_nuttall =
        WindowFunction::generate<double>(5, WindowFunction::Type::blackman_nuttall);
    REQUIRE_THAT(blackman_nuttall[0], WithinAbs(0.0003628, 1e-7));
    REQUIRE_THAT(blackman_nuttall[1], WithinAbs(0.2269824, 1e-7));
    REQUIRE_THAT(blackman_nuttall[2], WithinAbs(1.0, 1e-7));
    REQUIRE_THAT(blackman_nuttall[3], WithinAbs(0.2269824, 1e-7));
    REQUIRE_THAT(blackman_nuttall[4], WithinAbs(0.0003628, 1e-7));
}

TEST_CASE("Four-term windows preserve symmetric edge and singleton contracts", "[signal][window]") {
    for (const auto type :
         {WindowFunction::Type::blackman_harris, WindowFunction::Type::blackman_nuttall}) {
        CHECK(WindowFunction::generate(0, type).empty());
        CHECK(WindowFunction::generate(-1, type).empty());
        CHECK(WindowFunction::generate(1, type) == std::vector<float>{1.0f});

        const auto even = WindowFunction::generate<double>(128, type);
        for (std::size_t i = 0; i < even.size(); ++i)
            REQUIRE_THAT(even[i], WithinAbs(even[even.size() - 1u - i], 1e-13));

        const auto odd = WindowFunction::generate<double>(129, type);
        REQUIRE_THAT(odd[64], WithinAbs(1.0, 1e-7));
        CHECK(odd.front() > 0.0);
        REQUIRE_THAT(odd.front(), WithinAbs(odd.back(), 1e-13));
    }
}

TEST_CASE("Four-term windows meet coherent-gain ENBW and sidelobe behavior",
          "[signal][window][spectral]") {
    constexpr int kSize = 1025;

    const auto blackman_harris =
        WindowFunction::generate<double>(kSize, WindowFunction::Type::blackman_harris);
    const auto bh_metrics = metrics(blackman_harris);
    CHECK(std::abs(bh_metrics.coherent_gain - 0.35875) < 4e-4);
    CHECK(std::abs(bh_metrics.enbw_bins - 2.0063) < 0.005);
    const double bh_sidelobe_db = maximum_sidelobe_db(blackman_harris);
    INFO("Blackman-Harris maximum sidelobe " << bh_sidelobe_db << " dB");
    CHECK(bh_sidelobe_db < -90.0);
    CHECK(bh_sidelobe_db > -94.0);

    const auto blackman_nuttall =
        WindowFunction::generate<double>(kSize, WindowFunction::Type::blackman_nuttall);
    const auto bn_metrics = metrics(blackman_nuttall);
    CHECK(std::abs(bn_metrics.coherent_gain - 0.3635819) < 4e-4);
    CHECK(std::abs(bn_metrics.enbw_bins - 1.9761) < 0.005);
    const double bn_sidelobe_db = maximum_sidelobe_db(blackman_nuttall);
    INFO("Blackman-Nuttall maximum sidelobe " << bn_sidelobe_db << " dB");
    CHECK(bn_sidelobe_db < -95.0);
    CHECK(bn_sidelobe_db > -101.0);
}

TEST_CASE("STFT accepts four-term windows and lowers far-bin leakage", "[signal][window][stft]") {
    const double hann = normalized_far_sidelobe(WindowFunction::Type::hann);
    const double blackman_harris = normalized_far_sidelobe(WindowFunction::Type::blackman_harris);
    const double blackman_nuttall = normalized_far_sidelobe(WindowFunction::Type::blackman_nuttall);

    INFO("hann=" << hann << " blackman_harris=" << blackman_harris
                 << " blackman_nuttall=" << blackman_nuttall);
    CHECK(blackman_harris < hann * 0.1);
    CHECK(blackman_nuttall < hann * 0.1);
}

TEST_CASE("Applying precomputed four-term windows is allocation-free",
          "[signal][window][rt-safety]") {
    std::vector<float> samples(1024, 1.0f);
    const auto blackman_harris =
        WindowFunction::generate(1024, WindowFunction::Type::blackman_harris);
    const auto blackman_nuttall =
        WindowFunction::generate(1024, WindowFunction::Type::blackman_nuttall);

    pulp::test::RtAllocationProbe probe;
    WindowFunction::apply(samples.data(), blackman_harris);
    WindowFunction::apply(samples.data(), blackman_nuttall);
    CHECK(probe.allocation_count() == 0);
}
