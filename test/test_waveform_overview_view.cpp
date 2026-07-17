// WaveformOverviewView: verify the Canvas (non-GPU) min/max-column rendering
// tracks the peaks, and that sweeping the viewport zooms into the region it
// selects. All assertions run headlessly through RecordingCanvas — the point of
// the widget is that it draws through plain Canvas verbs, no GPU required.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/waveform_overview_view.hpp>
#include <pulp/audio/waveform_overview.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/canvas/canvas.hpp>

#include <cmath>
#include <map>
#include <vector>

using namespace pulp::view;
using pulp::audio::AudioFileData;
using pulp::audio::WaveformOverview;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;

namespace {

constexpr int kTotal = 4096;
constexpr int kHalf = kTotal / 2;

// Silence in the first half, full-scale BIPOLAR content in the second half. A
// bipolar (alternating-sign) loud region matters: a constant-DC region has
// min == max and would collapse to a one-pixel column, hiding the peak.
AudioFileData make_half_loud_buffer() {
    AudioFileData data;
    data.sample_rate = 44100;
    std::vector<float> ch(static_cast<size_t>(kTotal), 0.0f);
    for (int i = kHalf; i < kTotal; ++i)
        ch[static_cast<size_t>(i)] = (i & 1) ? 0.9f : -0.9f;
    data.channels.push_back(std::move(ch));
    return data;
}

// Map recorded stroke_line columns to their painted height (|y1 - y0|), keyed
// by the integer pixel column (floor of the x coordinate).
std::map<int, float> column_heights(const RecordingCanvas& c) {
    std::map<int, float> h;
    for (const auto& cmd : c.commands()) {
        if (cmd.type != DrawCommand::Type::stroke_line) continue;
        const int px = static_cast<int>(std::floor(cmd.f[0]));
        h[px] = std::abs(cmd.f[3] - cmd.f[1]);
    }
    return h;
}

float max_height_in_range(const std::map<int, float>& h, int lo, int hi) {
    float m = 0.0f;
    for (const auto& [px, height] : h)
        if (px >= lo && px < hi) m = std::max(m, height);
    return m;
}

}  // namespace

TEST_CASE("WaveformOverviewView paints taller columns where the signal is loud",
          "[view][waveform][overview]") {
    const AudioFileData data = make_half_loud_buffer();
    const WaveformOverview ov = WaveformOverview::build_from_buffer(data, /*spp=*/64);
    REQUIRE_FALSE(ov.empty());

    WaveformOverviewView view;
    view.set_overview(&ov);
    view.set_bounds({0, 0, 64, 100});
    REQUIRE(view.viewport_start() == 0);
    REQUIRE(view.viewport_end() == static_cast<uint64_t>(kTotal));

    RecordingCanvas c;
    view.paint(c);

    // Background fill + at least one waveform column.
    CHECK(c.count(DrawCommand::Type::fill_rect) >= 1);
    CHECK(c.count(DrawCommand::Type::stroke_line) > 0);

    const auto heights = column_heights(c);
    const float left_max = max_height_in_range(heights, 0, 32);    // silent half
    const float right_max = max_height_in_range(heights, 32, 64);  // loud half

    // The loud half draws tall columns; the silent half collapses to ~1px.
    CHECK(right_max > 50.0f);
    CHECK(left_max < 5.0f);
    CHECK(right_max > left_max);
}

TEST_CASE("WaveformOverviewView viewport sweep zooms into the selected region",
          "[view][waveform][overview]") {
    const AudioFileData data = make_half_loud_buffer();
    const WaveformOverview ov = WaveformOverview::build_from_buffer(data, /*spp=*/64);

    WaveformOverviewView view;
    view.set_overview(&ov);
    view.set_bounds({0, 0, 64, 100});

    // Zoom onto the loud half only — now EVERY column should be tall.
    view.set_viewport(kHalf, kTotal);
    CHECK(view.viewport_start() == static_cast<uint64_t>(kHalf));
    CHECK(view.viewport_end() == static_cast<uint64_t>(kTotal));
    RecordingCanvas loud;
    view.paint(loud);
    const auto loud_h = column_heights(loud);
    REQUIRE_FALSE(loud_h.empty());
    for (const auto& [px, height] : loud_h) {
        (void)px;
        CHECK(height > 50.0f);
    }

    // Zoom onto the silent half — now every column is flat.
    view.set_viewport(0, kHalf);
    RecordingCanvas quiet;
    view.paint(quiet);
    const auto quiet_h = column_heights(quiet);
    for (const auto& [px, height] : quiet_h) {
        (void)px;
        CHECK(height <= 2.0f);
    }
}

TEST_CASE("WaveformOverviewView::column_peak folds min/max over a frame window",
          "[view][waveform][overview]") {
    const AudioFileData data = make_half_loud_buffer();
    const WaveformOverview ov = WaveformOverview::build_from_buffer(data, /*spp=*/64);

    WaveformOverviewView view;
    view.set_overview(&ov);

    float lo = 9.0f, hi = 9.0f;
    REQUIRE(view.column_peak(0, kHalf, lo, hi));
    CHECK(std::abs(lo) < 0.05f);  // silence
    CHECK(std::abs(hi) < 0.05f);

    lo = hi = 0.0f;
    REQUIRE(view.column_peak(kHalf, kTotal, lo, hi));
    CHECK(hi > 0.8f);   // +0.9 peaks
    CHECK(lo < -0.8f);  // -0.9 troughs
}

TEST_CASE("WaveformOverviewView clamps the viewport to the source length",
          "[view][waveform][overview]") {
    const AudioFileData data = make_half_loud_buffer();
    const WaveformOverview ov = WaveformOverview::build_from_buffer(data, /*spp=*/64);

    WaveformOverviewView view;
    view.set_overview(&ov);

    view.set_viewport(1000, 999'999);  // end past the source
    CHECK(view.viewport_start() == 1000);
    CHECK(view.viewport_end() == static_cast<uint64_t>(kTotal));

    view.set_viewport(3000, 2000);  // inverted -> collapses to empty at start
    CHECK(view.viewport_start() == 3000);
    CHECK(view.viewport_end() == 3000);

    view.view_all();
    CHECK(view.viewport_start() == 0);
    CHECK(view.viewport_end() == static_cast<uint64_t>(kTotal));
}

TEST_CASE("WaveformOverviewView with no overview paints only the background",
          "[view][waveform][overview]") {
    WaveformOverviewView view;
    view.set_bounds({0, 0, 64, 100});
    CHECK_FALSE(view.has_overview());

    RecordingCanvas c;
    view.paint(c);

    CHECK(c.count(DrawCommand::Type::fill_rect) == 1);      // background only
    CHECK(c.count(DrawCommand::Type::stroke_line) == 0);    // no columns

    float lo = 0.0f, hi = 0.0f;
    CHECK_FALSE(view.column_peak(0, 100, lo, hi));
}
