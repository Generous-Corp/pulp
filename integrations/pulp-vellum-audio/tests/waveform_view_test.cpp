#include <pulp/vellum_audio/waveform_view.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/waveform_overview.hpp>
#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/screenshot.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;
using pulp::audio::AudioFileData;
using pulp::audio::WaveformOverview;
using pulp::vellum_audio::WaveformOverviewView;
using pulp::vellum_audio::WaveformView;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;

namespace {

constexpr int kTotal = 4096;
constexpr int kHalf = kTotal / 2;

struct Rgb {
    int r, g, b;
};

struct Frame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Rgb at(std::uint32_t x, std::uint32_t y) const {
        const auto o = (static_cast<std::size_t>(y) * width + x) * 4;
        return {pixels[o], pixels[o + 1], pixels[o + 2]};
    }
};

Frame render(::vellum::view::View& view, std::uint32_t w, std::uint32_t h) {
    Frame frame;
    frame.pixels = ::vellum::view::render_to_rgba(view, w, h, 1.0f, &frame.width, &frame.height);
    return frame;
}

// PULP_VELLUM_AUDIO_PNG_DIR names a directory to receive the rendered frames
// for review; unset (the default) writes nothing.
void maybe_write_png(::vellum::view::View& view, std::uint32_t w, std::uint32_t h,
                     const std::string& name) {
    if (const char* dir = std::getenv("PULP_VELLUM_AUDIO_PNG_DIR"))
        CHECK(::vellum::view::render_to_file(view, w, h, std::string(dir) + "/" + name, 4.0f));
}

// Silence in the first half, full-scale bipolar content in the second. A
// constant-DC region has min == max and would collapse to a one-pixel column.
AudioFileData make_half_loud_buffer() {
    AudioFileData data;
    data.sample_rate = 44100;
    std::vector<float> ch(static_cast<std::size_t>(kTotal), 0.0f);
    for (int i = kHalf; i < kTotal; ++i)
        ch[static_cast<std::size_t>(i)] = (i & 1) ? 0.9f : -0.9f;
    data.channels.push_back(std::move(ch));
    return data;
}

AudioFileData make_sine(int frames, double hz) {
    AudioFileData data;
    data.sample_rate = 44100;
    std::vector<float> ch(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
        ch[static_cast<std::size_t>(i)] =
            0.8f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * i / 44100.0));
    data.channels.push_back(std::move(ch));
    return data;
}

std::map<int, float> column_heights(const RecordingCanvas& c) {
    std::map<int, float> h;
    for (const auto& cmd : c.commands()) {
        if (cmd.type != DrawCommand::Type::stroke_line)
            continue;
        h[static_cast<int>(std::floor(cmd.f[0]))] = std::abs(cmd.f[3] - cmd.f[1]);
    }
    return h;
}

float max_height_in_range(const std::map<int, float>& h, int lo, int hi) {
    float m = 0.0f;
    for (const auto& [px, height] : h)
        if (px >= lo && px < hi)
            m = std::max(m, height);
    return m;
}

bool near(Rgb a, Rgb b, int tol = 3) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

} // namespace

// ── WaveformView ─────────────────────────────────────────────────────────────

TEST_CASE("Vellum WaveformView paints per-column amplitude bars",
          "[pulp-vellum-audio][waveform-view]") {
    WaveformView waveform;
    waveform.set_bounds({0, 0, 200, 60});
    std::vector<float> data(200);
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = std::sin(2.0f * 3.14159f * static_cast<float>(i) / 200.0f);
    waveform.set_data(std::move(data));
    REQUIRE(waveform.sample_count() == 200);

    RecordingCanvas canvas;
    waveform.paint(canvas);
    CHECK(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    CHECK(canvas.count(DrawCommand::Type::stroke_line) == 1); // centre line
    CHECK(canvas.count(DrawCommand::Type::fill_rect) > 10);
}

TEST_CASE("Vellum WaveformView with no data paints only the background",
          "[pulp-vellum-audio][waveform-view]") {
    WaveformView waveform;
    waveform.set_bounds({0, 0, 200, 60});
    RecordingCanvas canvas;
    waveform.paint(canvas);
    CHECK(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    CHECK(canvas.count(DrawCommand::Type::stroke_line) == 0);
}

TEST_CASE("Vellum WaveformView rotates the buffer to the trigger crossing",
          "[pulp-vellum-audio][waveform-view][trigger]") {
    constexpr std::size_t N = 256;
    std::vector<float> samples(N);
    for (std::size_t i = 0; i < N; ++i)
        samples[i] = std::sin(3.14159f / 4.0f + 2.0f * 3.14159f * static_cast<float>(i) / N);

    const std::size_t expected =
        WaveformView::find_trigger_index(samples.data(), N, WaveformView::TriggerMode::rising_zero);
    REQUIRE(expected > 0);
    REQUIRE(expected < N);

    WaveformView waveform;
    waveform.set_trigger_mode(WaveformView::TriggerMode::rising_zero);
    waveform.set_data(samples);
    REQUIRE(waveform.sample_count() == N);
    CHECK(waveform.samples()[0] == samples[expected]);
    CHECK(waveform.samples()[N - expected] == samples[0]);
    CHECK(waveform.samples()[0] > 0.0f);
}

TEST_CASE("Vellum WaveformView trigger search covers each mode",
          "[pulp-vellum-audio][waveform-view][trigger]") {
    const std::vector<float> ramp{2, 1, 0.5f, 0, -0.5f, -1, -2, -1};
    CHECK(WaveformView::find_trigger_index(ramp.data(), ramp.size(),
                                           WaveformView::TriggerMode::falling_zero) == 4);
    CHECK(WaveformView::find_trigger_index(ramp.data(), ramp.size(),
                                           WaveformView::TriggerMode::free_run) == 0);

    const std::vector<float> positive{0.5f, 0.6f, 0.7f, 0.8f, 0.9f};
    CHECK(WaveformView::find_trigger_index(positive.data(), positive.size(),
                                           WaveformView::TriggerMode::rising_zero) == 0);
    WaveformView waveform;
    waveform.set_trigger_mode(WaveformView::TriggerMode::rising_zero);
    waveform.set_data(positive);
    CHECK(waveform.samples() == positive); // no crossing: stored as is
}

TEST_CASE("Vellum WaveformView thumbnail and sample sources displace each other",
          "[pulp-vellum-audio][waveform-view]") {
    WaveformView view;
    REQUIRE_FALSE(view.has_thumbnail());

    auto thumb = std::make_shared<WaveformOverview>(
        WaveformOverview::build_from_buffer(make_sine(4410, 200.0), 256));
    REQUIRE_FALSE(thumb->empty());

    view.set_thumbnail(thumb);
    REQUIRE(view.has_thumbnail());
    CHECK(view.thumbnail() == thumb.get());
    CHECK(view.sample_count() == 0);

    view.set_data(std::vector<float>(128, 0.1f));
    CHECK_FALSE(view.has_thumbnail());
    CHECK(view.sample_count() == 128);

    WaveformOverview borrowed = WaveformOverview::build_from_buffer(make_sine(4410, 200.0), 256);
    view.set_thumbnail_borrowed(&borrowed);
    CHECK(view.has_thumbnail());
    CHECK(view.thumbnail() == &borrowed);
    CHECK(view.sample_count() == 0);
    view.clear_thumbnail();
    CHECK_FALSE(view.has_thumbnail());
}

TEST_CASE("Vellum WaveformView thumbnail path draws the waveform primitive",
          "[pulp-vellum-audio][waveform-view]") {
    WaveformView view;
    view.set_bounds({0, 0, 120, 40});
    view.set_thumbnail(std::make_shared<WaveformOverview>(
        WaveformOverview::build_from_buffer(make_sine(8820, 200.0), 64)));
    RecordingCanvas canvas;
    view.paint(canvas);
    CHECK(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    CHECK(canvas.count(DrawCommand::Type::fill_rect) == 0); // no sample bars
}

TEST_CASE("Vellum WaveformView preview shapes parse and paint a path",
          "[pulp-vellum-audio][waveform-view]") {
    WaveformView view;
    view.set_bounds({0, 0, 120, 40});
    view.set_preview_shape("Sawtooth");
    CHECK(view.preview_shape() == WaveformView::PreviewShape::saw);
    view.set_series_preview("SIN");
    CHECK(view.preview_shape() == WaveformView::PreviewShape::sine);
    view.set_preview_shape("squ");
    CHECK(view.preview_shape() == WaveformView::PreviewShape::square);
    view.set_preview_shape("tri");
    CHECK(view.preview_shape() == WaveformView::PreviewShape::triangle);
    view.set_preview_shape("noise");
    CHECK(view.preview_shape() == WaveformView::PreviewShape::none);

    view.set_preview_shape("triangle");
    RecordingCanvas canvas;
    view.paint(canvas);
    CHECK(canvas.count(DrawCommand::Type::stroke_line) == 1); // centre line
    CHECK(canvas.count(DrawCommand::Type::fill_rect) == 0);
}

TEST_CASE("Vellum WaveformView reports facts and accepts series data",
          "[pulp-vellum-audio][waveform-view]") {
    WaveformView view;
    const float data[4] = {0.1f, -0.2f, 0.3f, -0.4f};
    static_cast<::vellum::view::SeriesDisplay&>(view).set_series(data, 4);
    CHECK(view.sample_count() == 4);

    std::vector<::vellum::view::InspectorFacts::Fact> facts;
    view.inspector_facts(facts);
    REQUIRE(facts.size() == 1);
    CHECK(facts[0].key == "samples");
    CHECK(facts[0].value == Approx(4.0));
    CHECK(view.inspector_type_name() == "WaveformView");
}

TEST_CASE("Vellum WaveformView mutators request repaint", "[pulp-vellum-audio][waveform-view]") {
    WaveformView view;
    const std::vector<float> samples(16, 0.5f);
    auto before = ::vellum::view::View::damage_request_count();
    view.set_data(samples);
    CHECK(::vellum::view::View::damage_request_count() > before);

    before = ::vellum::view::View::damage_request_count();
    view.set_data(samples.data(), samples.size());
    CHECK(::vellum::view::View::damage_request_count() > before);

    WaveformOverview ov = WaveformOverview::build_from_buffer(make_sine(2048, 100.0), 64);
    before = ::vellum::view::View::damage_request_count();
    view.set_thumbnail_borrowed(&ov);
    CHECK(::vellum::view::View::damage_request_count() > before);

    before = ::vellum::view::View::damage_request_count();
    view.clear_thumbnail();
    CHECK(::vellum::view::View::damage_request_count() > before);
}

TEST_CASE("Vellum WaveformView renders bars in the waveform colour",
          "[pulp-vellum-audio][waveform-view][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    WaveformView view;
    view.set_bounds({0, 0, 100, 40});
    view.set_data(std::vector<float>(100, 1.0f));
    const auto frame = render(view, 100, 40);
    REQUIRE(frame.width == 100);
    CHECK(near(frame.at(50, 20), {100, 180, 250}));
    CHECK(near(frame.at(50, 3), {100, 180, 250})); // full-scale reaches the edges
    CHECK(near(frame.at(50, 0), {30, 30, 40}, 6)); // background above the bar
    maybe_write_png(view, 100, 40, "waveform_view_bars.png");
}

TEST_CASE("Vellum WaveformView renders a sine thumbnail and a preview for review",
          "[pulp-vellum-audio][waveform-view][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    WaveformView thumb;
    thumb.set_bounds({0, 0, 160, 50});
    thumb.set_thumbnail(std::make_shared<WaveformOverview>(
        WaveformOverview::build_from_buffer(make_sine(8820, 100.0), 64)));
    const auto thumb_frame = render(thumb, 160, 50);
    // Somewhere on the centre line the primitive has drawn the waveform colour.
    bool drew = false;
    for (std::uint32_t x = 0; x < 160 && !drew; ++x)
        drew = !near(thumb_frame.at(x, 25), {30, 30, 40}, 8);
    CHECK(drew);
    maybe_write_png(thumb, 160, 50, "waveform_view_thumbnail.png");

    WaveformView preview;
    preview.set_bounds({0, 0, 160, 50});
    preview.set_preview_shape("saw");
    const auto preview_frame = render(preview, 160, 50);
    bool stroked = false;
    for (std::uint32_t y = 5; y < 15 && !stroked; ++y)
        for (std::uint32_t x = 0; x < 160 && !stroked; ++x)
            stroked = !near(preview_frame.at(x, y), {30, 30, 40}, 8);
    CHECK(stroked);
    maybe_write_png(preview, 160, 50, "waveform_view_preview_saw.png");
}

// ── WaveformOverviewView ─────────────────────────────────────────────────────

TEST_CASE("Vellum WaveformOverviewView paints taller columns where the signal is loud",
          "[pulp-vellum-audio][waveform-overview]") {
    const WaveformOverview ov = WaveformOverview::build_from_buffer(make_half_loud_buffer(), 64);
    REQUIRE_FALSE(ov.empty());

    WaveformOverviewView view;
    view.set_overview(&ov);
    view.set_bounds({0, 0, 64, 100});
    REQUIRE(view.viewport_start() == 0);
    REQUIRE(view.viewport_end() == static_cast<std::uint64_t>(kTotal));

    RecordingCanvas c;
    view.paint(c);
    CHECK(c.count(DrawCommand::Type::fill_rect) >= 1);
    CHECK(c.count(DrawCommand::Type::stroke_line) > 0);

    const auto heights = column_heights(c);
    const float left_max = max_height_in_range(heights, 0, 32);
    const float right_max = max_height_in_range(heights, 32, 64);
    CHECK(right_max > 50.0f);
    CHECK(left_max < 5.0f);
}

TEST_CASE("Vellum WaveformOverviewView viewport sweep zooms into the selected region",
          "[pulp-vellum-audio][waveform-overview]") {
    const WaveformOverview ov = WaveformOverview::build_from_buffer(make_half_loud_buffer(), 64);
    WaveformOverviewView view;
    view.set_overview(&ov);
    view.set_bounds({0, 0, 64, 100});

    view.set_viewport(kHalf, kTotal);
    RecordingCanvas loud;
    view.paint(loud);
    const auto loud_h = column_heights(loud);
    REQUIRE_FALSE(loud_h.empty());
    for (const auto& [px, height] : loud_h) {
        (void)px;
        CHECK(height > 50.0f);
    }

    view.set_viewport(0, kHalf);
    RecordingCanvas quiet;
    view.paint(quiet);
    for (const auto& [px, height] : column_heights(quiet)) {
        (void)px;
        CHECK(height <= 2.0f);
    }
}

TEST_CASE("Vellum WaveformOverviewView::column_peak folds min/max over a frame window",
          "[pulp-vellum-audio][waveform-overview]") {
    const WaveformOverview ov = WaveformOverview::build_from_buffer(make_half_loud_buffer(), 64);
    WaveformOverviewView view;
    view.set_overview(&ov);

    float lo = 9.0f, hi = 9.0f;
    REQUIRE(view.column_peak(0, kHalf, lo, hi));
    CHECK(std::abs(lo) < 0.05f);
    CHECK(std::abs(hi) < 0.05f);

    REQUIRE(view.column_peak(kHalf, kTotal, lo, hi));
    CHECK(hi > 0.8f);
    CHECK(lo < -0.8f);
}

TEST_CASE("Vellum WaveformOverviewView clamps the viewport to the source length",
          "[pulp-vellum-audio][waveform-overview]") {
    const WaveformOverview ov = WaveformOverview::build_from_buffer(make_half_loud_buffer(), 64);
    WaveformOverviewView view;
    view.set_overview(&ov);

    view.set_viewport(1000, 999'999);
    CHECK(view.viewport_start() == 1000);
    CHECK(view.viewport_end() == static_cast<std::uint64_t>(kTotal));

    view.set_viewport(3000, 2000);
    CHECK(view.viewport_start() == 3000);
    CHECK(view.viewport_end() == 3000);

    view.view_all();
    CHECK(view.viewport_start() == 0);
    CHECK(view.viewport_end() == static_cast<std::uint64_t>(kTotal));
}

TEST_CASE("Vellum WaveformOverviewView with no overview paints only the background",
          "[pulp-vellum-audio][waveform-overview]") {
    WaveformOverviewView view;
    view.set_bounds({0, 0, 64, 100});
    CHECK_FALSE(view.has_overview());

    RecordingCanvas c;
    view.paint(c);
    CHECK(c.count(DrawCommand::Type::fill_rect) == 1);
    CHECK(c.count(DrawCommand::Type::stroke_line) == 0);

    float lo = 0.0f, hi = 0.0f;
    CHECK_FALSE(view.column_peak(0, 100, lo, hi));
}

TEST_CASE("Vellum WaveformOverviewView mutators request repaint",
          "[pulp-vellum-audio][waveform-overview]") {
    const WaveformOverview ov = WaveformOverview::build_from_buffer(make_half_loud_buffer(), 64);
    WaveformOverviewView view;

    auto before = ::vellum::view::View::damage_request_count();
    view.set_overview(&ov);
    CHECK(::vellum::view::View::damage_request_count() > before);

    before = ::vellum::view::View::damage_request_count();
    view.set_viewport(0, 100);
    CHECK(::vellum::view::View::damage_request_count() > before);

    before = ::vellum::view::View::damage_request_count();
    view.set_line_color(::vellum::canvas::Color::rgba8(255, 0, 0));
    CHECK(::vellum::view::View::damage_request_count() > before);

    before = ::vellum::view::View::damage_request_count();
    view.set_background_color(::vellum::canvas::Color::rgba8(0, 0, 0));
    CHECK(::vellum::view::View::damage_request_count() > before);

    before = ::vellum::view::View::damage_request_count();
    view.clear_overview();
    CHECK(::vellum::view::View::damage_request_count() > before);
}

TEST_CASE("Vellum WaveformOverviewView renders loud columns over a silent half",
          "[pulp-vellum-audio][waveform-overview][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    const WaveformOverview ov = WaveformOverview::build_from_buffer(make_half_loud_buffer(), 64);
    WaveformOverviewView view;
    view.set_overview(&ov);
    view.set_bounds({0, 0, 64, 100});

    const auto frame = render(view, 64, 100);
    REQUIRE(frame.width == 64);
    // Background everywhere off the strokes; the loud half stands tall.
    CHECK(near(frame.at(1, 10), {20, 20, 30}, 4));
    bool loud_top = false;
    for (std::uint32_t x = 32; x < 64 && !loud_top; ++x)
        loud_top = !near(frame.at(x, 10), {20, 20, 30}, 20);
    CHECK(loud_top);
    bool silent_top = false;
    for (std::uint32_t x = 0; x < 32 && !silent_top; ++x)
        silent_top = !near(frame.at(x, 10), {20, 20, 30}, 20);
    CHECK_FALSE(silent_top);
    maybe_write_png(view, 64, 100, "waveform_overview_view.png");
}
