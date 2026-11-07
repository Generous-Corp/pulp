#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/audio_widgets.hpp>
#include <pulp/view/continuous_frames.hpp>
#include <pulp/view/eq_curve_view.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<float> sine(double hz, double sr, std::size_t n) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * i / sr));
    return v;
}

} // namespace

TEST_CASE("SpectrumView analysis puts a 1 kHz sine on the 1 kHz column", "[view][spectrum]") {
    SpectrumView view;
    view.set_bounds({0, 0, 600, 120});
    view.set_analysis_enabled(true);
    view.set_sample_rate(48000.0);
    view.set_fft_size(2048);
    view.set_window(SpectrumView::WindowType::hann);
    view.set_level_mode(SpectrumView::LevelMode::peak);
    view.set_frequency_range(20.0f, 20000.0f);
    view.set_release_time(0.0f);

    const auto x = sine(1000.0, 48000.0, 8192);
    view.push_samples(x.data(), x.size());
    REQUIRE(view.update_analysis(0.016) > 0);

    const std::size_t columns = 600;
    const auto cols = view.analysis_columns(columns);
    REQUIRE(cols.size() == columns);
    const auto best = static_cast<std::size_t>(std::max_element(cols.begin(), cols.end()) -
                                               cols.begin());
    const LogFrequencyScale axis{20.0f, 20000.0f, 0.0f, static_cast<float>(columns)};
    const double expected_column = axis.to_x(1000.0f) - 0.5; // index of the column holding 1 kHz
    CHECK(std::abs(static_cast<double>(best) - expected_column) <= 0.5);

    // No staircase at low frequencies.
    int identical = 0;
    for (std::size_t i = 1; i < 100; ++i)
        if (cols[i] == cols[i - 1])
            ++identical;
    CHECK(identical == 0);
}

TEST_CASE("SpectrumView analysis paints in every display type", "[view][spectrum]") {
    for (auto style : {SpectrumView::Style::bars, SpectrumView::Style::line,
                       SpectrumView::Style::filled}) {
        SpectrumView view;
        view.set_bounds({0, 0, 300, 100});
        view.set_style(style);
        view.set_analysis_enabled(true);
        const auto x = sine(1000.0, 48000.0, 8192);
        view.push_samples(x.data(), x.size());

        RecordingCanvas canvas;
        view.paint(canvas);
        CHECK(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
        if (style == SpectrumView::Style::bars)
            CHECK(canvas.count(DrawCommand::Type::fill_rect) >= 50);
        else // the polyline is one segment per pixel column (300 px wide)
            CHECK(canvas.count(DrawCommand::Type::stroke_line) >= 290);
    }
}

TEST_CASE("SpectrumView zero-width and disabled states are safe", "[view][spectrum]") {
    SpectrumView view;
    view.set_analysis_enabled(true);
    const auto x = sine(1000.0, 48000.0, 4096);
    for (float w : {0.0f, 0.4f}) {
        view.set_bounds({0, 0, w, 100});
        view.push_samples(x.data(), x.size());
        RecordingCanvas canvas;
        view.paint(canvas); // must not crash or divide by zero
        CHECK(canvas.count(DrawCommand::Type::fill_rect) == 0);
    }

    // Disabled: push is a no-op and nothing queues.
    SpectrumView off;
    off.push_samples(x.data(), x.size());
    CHECK(off.update_analysis(0.016) == 0);
    CHECK(off.analysis_columns(10).empty());
    off.set_analysis_enabled(true);
    CHECK(off.update_analysis(0.016) == 0); // earlier push was not queued
    off.set_analysis_enabled(false);
    off.push_samples(x.data(), x.size());
}

TEST_CASE("SpectrumView analysis keeps the view repainting only while enabled",
          "[view][spectrum]") {
    SpectrumView view;
    CHECK_FALSE(needs_continuous_frames(&view));
    view.set_analysis_enabled(true);
    CHECK(needs_continuous_frames(&view));
    view.set_analysis_enabled(false);
    CHECK_FALSE(needs_continuous_frames(&view));
}

TEST_CASE("SpectrumView survives setting changes while audio keeps flowing", "[view][spectrum]") {
    SpectrumView view;
    view.set_bounds({0, 0, 400, 100});
    view.set_analysis_enabled(true);
    const auto x = sine(1000.0, 48000.0, 4096);
    for (int fft : {256, 4096, 1000, 99999, 512}) {
        view.set_fft_size(fft);
        view.set_window(SpectrumView::WindowType::blackman_harris);
        view.set_level_mode(SpectrumView::LevelMode::psd);
        view.set_overlap(0.75f);
        view.push_samples(x.data(), x.size());
        view.update_analysis(0.016);
        RecordingCanvas canvas;
        view.paint(canvas);
        for (float v : view.analysis_columns(200))
            CHECK(std::isfinite(v));
    }
    // Bad values are rejected, not stored.
    view.set_frequency_range(-1.0f, 10.0f);
    view.set_frequency_range(500.0f, 100.0f);
    CHECK(view.min_frequency() == 20.0f);
    CHECK(view.max_frequency() == 20000.0f);
    view.set_sample_rate(-5.0);
    CHECK(view.sample_rate() == 48000.0);
}

TEST_CASE("SpectrumView supplied bins keep their original drawing", "[view][spectrum]") {
    SpectrumView view;
    view.set_bounds({0, 0, 300, 100});
    view.set_style(SpectrumView::Style::bars);
    view.set_spectrum(std::vector<float>(32, -40.0f));
    RecordingCanvas canvas;
    view.paint(canvas);
    CHECK(canvas.count(DrawCommand::Type::fill_rect) >= 30);
    CHECK_FALSE(view.analysis_enabled());
}

TEST_CASE("EqCurveView analyzer overlay maps bins through SpectrumBinMapping", "[view][spectrum]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 640, 240});
    eq.set_sample_rate(48000.0f);

    // A narrow peak between two bins of a 2048-point spectrum.
    std::vector<float> bins(1025, -90.0f);
    const double vertex = 1000.0 / (48000.0 / 2048.0);
    for (int k = 0; k < 1025; ++k)
        bins[static_cast<std::size_t>(k)] =
            std::max(-90.0f, -3.0f - 6.0f * static_cast<float>((k - vertex) * (k - vertex)));
    eq.set_spectrum(bins.data(), bins.size());

    RecordingCanvas with_overlay;
    eq.paint(with_overlay);
    CHECK(eq.analyzer_animating());
    eq.set_analyzer_enabled(false);
    RecordingCanvas without_overlay;
    eq.paint(without_overlay);
    eq.set_analyzer_enabled(true);
    // The overlay's top stroke is a polyline of many segments.
    CHECK(with_overlay.count(DrawCommand::Type::stroke_line) >
          without_overlay.count(DrawCommand::Type::stroke_line) + 100);

    // A bin count that changes between frames (FFT-size change) repaints cleanly.
    std::vector<float> bigger(4097, -60.0f);
    eq.set_spectrum(bigger.data(), bigger.size());
    RecordingCanvas again;
    eq.paint(again);
    CHECK(again.count(DrawCommand::Type::stroke_line) >
          without_overlay.count(DrawCommand::Type::stroke_line) + 100);

    // A degenerate one-bin spectrum draws a floor line, not garbage.
    float single = -20.0f;
    eq.set_spectrum(&single, 1);
    RecordingCanvas degenerate;
    eq.paint(degenerate);
}
