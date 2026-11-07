#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/spectrum_analyzer.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<float> sine(double hz, double sr, std::size_t n, double amp = 1.0) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / sr));
    return v;
}

// The analyzer owns a lock-free ring and cannot be moved, so hand it out boxed.
std::unique_ptr<SpectrumAnalyzer> make_analyzer(SpectrumLevelMode mode,
                                                WindowFunction::Type window, int fft = 2048) {
    auto a = std::make_unique<SpectrumAnalyzer>();
    SpectrumAnalyzerConfig c;
    c.fft_size = fft;
    c.window = window;
    c.level_mode = mode;
    c.release_seconds = 0.0f;
    a->configure(c);
    return a;
}

std::size_t peak_bin(std::span<const float> bins) {
    return static_cast<std::size_t>(std::max_element(bins.begin(), bins.end()) - bins.begin());
}

} // namespace

TEST_CASE("SpectrumAnalyzer peak level is calibrated per window", "[signal][spectrum]") {
    const auto x = sine(1000.0, 48000.0, 8192);
    // A flat-top window reads an off-bin sine within 0.1 dB of its amplitude.
    SpectrumAnalyzer flat;
    {
        SpectrumAnalyzerConfig c;
        c.window = WindowFunction::Type::flat_top;
        c.release_seconds = 0.0f;
        flat.configure(c);
    }
    flat.push_samples(x.data(), x.size());
    REQUIRE(flat.update(0.016) > 0);
    CHECK_THAT(flat.bins_db()[peak_bin(flat.bins_db())], WithinAbs(0.0, 0.1));

    // Rectangular and Hann: bin-centred sine reads 0 dB; off-bin loses scalloping only.
    for (auto w : {WindowFunction::Type::hann, WindowFunction::Type::blackman_harris,
                   WindowFunction::Type::rectangular}) {
        auto a = make_analyzer(SpectrumLevelMode::peak, w);
        const auto centred = sine(48000.0 / 2048.0 * 64.0, 48000.0, 8192);
        a->push_samples(centred.data(), centred.size());
        REQUIRE(a->update(0.016) > 0);
        CHECK_THAT(a->bins_db()[64], WithinAbs(0.0, 0.05));
    }
}

TEST_CASE("SpectrumAnalyzer level modes", "[signal][spectrum]") {
    const double f = 48000.0 / 2048.0 * 100.0; // bin centred
    const auto x = sine(f, 48000.0, 8192);

    auto rms = make_analyzer(SpectrumLevelMode::rms, WindowFunction::Type::hann);
    rms->push_samples(x.data(), x.size());
    rms->update(0.016);
    CHECK_THAT(rms->bins_db()[100], WithinAbs(-3.0103, 0.05));
    CHECK(rms->aggregation() == SpectrumAggregation::power_mean);

    // Power: bins sum to the signal's mean square (-3.01 dB for a unit sine).
    auto pw = make_analyzer(SpectrumLevelMode::power_db, WindowFunction::Type::hann);
    pw->push_samples(x.data(), x.size());
    pw->update(0.016);
    double total = 0.0;
    for (float db : pw->bins_db())
        total += std::pow(10.0, db / 10.0);
    CHECK_THAT(10.0 * std::log10(total), WithinAbs(-3.0103, 0.1));
    CHECK(pw->aggregation() == SpectrumAggregation::power_sum);

    // PSD is power per Hz.
    auto psd = make_analyzer(SpectrumLevelMode::psd, WindowFunction::Type::hann);
    psd->push_samples(x.data(), x.size());
    psd->update(0.016);
    double psd_total = 0.0;
    for (float db : psd->bins_db())
        psd_total += std::pow(10.0, db / 10.0) * (48000.0 / 2048.0);
    CHECK_THAT(10.0 * std::log10(psd_total), WithinAbs(-3.0103, 0.1));
}

TEST_CASE("SpectrumAnalyzer: 1 kHz sine lands in the 1 kHz column", "[signal][spectrum]") {
    const double sr = 48000.0;
    // Windows whose main lobe is close to a parabola in dB locate the peak
    // to within half a column. Flat-top and rectangular (a sinc lobe) are
    // chosen for amplitude accuracy, not localisation; they are held to 0.3 bin.
    for (auto window : {WindowFunction::Type::hann, WindowFunction::Type::blackman_harris,
                        WindowFunction::Type::flat_top, WindowFunction::Type::rectangular}) {
        const bool localising = window == WindowFunction::Type::hann ||
                                window == WindowFunction::Type::blackman_harris;
        auto box = make_analyzer(SpectrumLevelMode::peak, window, 2048);
        auto& a = *box;
        const auto x = sine(1000.0, sr, 8192);
        a.push_samples(x.data(), x.size());
        REQUIRE(a.update(0.016) > 0);

        for (std::size_t columns : {400u, 800u, 1200u}) {
            SpectrumBinMapping m;
            REQUIRE(m.configure({sr, 2048, columns, 20.0, 20000.0, -120.0f, 200.0f}));
            std::vector<float> out(columns);
            m.map(a.bins_db(), out, a.aggregation());
            const auto best = static_cast<std::size_t>(
                std::max_element(out.begin(), out.end()) - out.begin());
            const double expected = m.frequency_to_column(1000.0) - 0.5; // column index
            if (localising) {
                CHECK(std::abs(static_cast<double>(best) - expected) <= 0.5);
            } else {
                const double bin_hz = sr / 2048.0;
                CHECK(std::abs(m.column_frequency(best) - 1000.0) <= 0.3 * bin_hz);
            }

            // No staircase over the first 100 columns: below ~100 Hz the
            // columns are far narrower than a bin, and no two neighbours match.
            int identical = 0;
            for (std::size_t i = 1; i < 100; ++i)
                if (out[i] == out[i - 1])
                    ++identical;
            CHECK(identical == 0);
        }
    }
}

TEST_CASE("SpectrumAnalyzer release is frame-rate independent", "[signal][spectrum]") {
    auto make_loud = [] {
        auto a = std::make_unique<SpectrumAnalyzer>();
        SpectrumAnalyzerConfig c;
        c.release_seconds = 0.5f;
        c.window = WindowFunction::Type::hann;
        a->configure(c);
        const auto x = sine(1000.0, 48000.0, 8192);
        a->push_samples(x.data(), x.size());
        a->update(0.0);
        return a;
    };
    auto a30p = make_loud();
    auto a144p = make_loud();
    auto& a30 = *a30p;
    auto& a144 = *a144p;
    const std::size_t bin = peak_bin(a30.bins_db());
    const float start = a30.bins_db()[bin];

    // One second of silence: 30 steps of 1/30 s versus 144 steps of 1/144 s.
    for (int i = 0; i < 30; ++i)
        a30.update(1.0 / 30.0);
    for (int i = 0; i < 144; ++i)
        a144.update(1.0 / 144.0);
    CHECK_THAT(a30.bins_db()[bin], WithinAbs(a144.bins_db()[bin], 0.01));
    // And it has genuinely released: exp(-2) of the dB distance to the floor.
    const double floor_db = a30.config().floor_db;
    CHECK_THAT(a30.bins_db()[bin], WithinAbs(floor_db + (start - floor_db) * std::exp(-2.0), 0.05));
    // Attack is instant.
    const auto x = sine(1000.0, 48000.0, 4096);
    a30.push_samples(x.data(), x.size());
    a30.update(1.0 / 30.0);
    CHECK_THAT(a30.bins_db()[bin], WithinAbs(start, 0.5));
}

TEST_CASE("SpectrumAnalyzer skips backlog instead of draining it", "[signal][spectrum]") {
    SpectrumAnalyzer a;
    SpectrumAnalyzerConfig c;
    c.fft_size = 1024;
    c.overlap = 0.75f; // hop 256
    c.max_frames_per_update = 4;
    c.release_seconds = 0.0f;
    a.configure(c);

    // 40 hops worth of audio queued while the "UI" was stalled.
    const auto x = sine(2000.0, 48000.0, 1024 + 40 * 256);
    a.push_samples(x.data(), x.size());
    const int frames = a.update(0.5);
    CHECK(frames <= 4);
    CHECK(frames >= 1);
    CHECK(a.skipped_frame_count() >= 30);
    // The newest audio was analysed: the tone is visible.
    CHECK(a.bins_db()[peak_bin(a.bins_db())] > -6.0f);

    // Normal cadence processes every hop and skips nothing more.
    const auto skipped = a.skipped_frame_count();
    const auto y = sine(2000.0, 48000.0, 3 * 256);
    a.push_samples(y.data(), y.size());
    CHECK(a.update(0.016) == 3);
    CHECK(a.skipped_frame_count() == skipped);
}

TEST_CASE("SpectrumAnalyzer FFT-size changes mid-stream", "[signal][spectrum]") {
    SpectrumAnalyzer a;
    SpectrumAnalyzerConfig c;
    c.release_seconds = 0.0f;
    for (int fft : {256, 4096, 512, 16384, 100, 1 << 20, 0, -5}) {
        c.fft_size = fft;
        a.configure(c);
        REQUIRE(a.bin_count() == static_cast<std::size_t>(a.config().fft_size / 2 + 1));
        REQUIRE(a.config().fft_size >= SpectrumAnalyzer::kMinFftSize);
        REQUIRE(a.config().fft_size <= SpectrumAnalyzer::kMaxFftSize);
        const auto x = sine(1000.0, 48000.0, static_cast<std::size_t>(a.config().fft_size) * 2);
        a.push_samples(x.data(), x.size());
        REQUIRE(a.update(0.016) > 0);
        for (float v : a.bins_db())
            REQUIRE(std::isfinite(v));
    }
}

TEST_CASE("SpectrumAnalyzer tolerates NaN and empty input", "[signal][spectrum]") {
    auto box = make_analyzer(SpectrumLevelMode::peak, WindowFunction::Type::hann);
    auto& a = *box;
    a.push_samples(nullptr, 100);
    a.push_samples(nullptr, 0);
    float nan[512];
    std::fill(std::begin(nan), std::end(nan), std::numeric_limits<float>::quiet_NaN());
    for (int i = 0; i < 8; ++i)
        a.push_samples(nan, 512);
    a.update(0.016);
    for (float v : a.bins_db())
        CHECK(std::isfinite(v));
    CHECK(a.update(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(a.update(-1.0) == 0);
}

TEST_CASE("SpectrumAnalyzer push is wait-free under a concurrent consumer", "[signal][spectrum]") {
    SpectrumAnalyzer a;
    SpectrumAnalyzerConfig c;
    c.release_seconds = 0.0f;
    a.configure(c);
    std::atomic<bool> done{false};
    std::thread audio([&] {
        const auto block = sine(1000.0, 48000.0, 256);
        for (int i = 0; i < 4000; ++i)
            a.push_samples(block.data(), block.size());
        done.store(true);
    });
    std::uint64_t frames = 0;
    while (!done.load())
        frames += static_cast<std::uint64_t>(a.update(0.001));
    audio.join();
    frames += static_cast<std::uint64_t>(a.update(0.001));
    CHECK(frames > 0);
    for (float v : a.bins_db())
        CHECK(std::isfinite(v));
}
