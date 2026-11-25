#include <pulp/vellum_audio/multi_meter.hpp>

#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/screenshot.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

using Catch::Approx;
using pulp::vellum_audio::CorrelationMeter;
using pulp::vellum_audio::MultiMeter;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;

namespace {

constexpr float kFrame = 1.0f / 60.0f;

pulp::signal::MultiChannelMeterData three_channels() {
    pulp::signal::MultiChannelMeterData data;
    data.num_channels = 3;
    data.channels[0].rms = 0.95f;
    data.channels[0].peak = 0.98f;
    data.channels[0].clipped = true;
    data.channels[1].rms = 0.75f;
    data.channels[1].peak = 0.82f;
    data.channels[2].rms = 0.35f;
    data.channels[2].peak = 0.42f;
    return data;
}

struct Rgba {
    int r, g, b, a;
};

struct Frame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Rgba at(std::uint32_t x, std::uint32_t y) const {
        const auto o = (static_cast<std::size_t>(y) * width + x) * 4;
        return {pixels[o], pixels[o + 1], pixels[o + 2], pixels[o + 3]};
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

} // namespace

TEST_CASE("Vellum MultiMeter update clamps channel count and moves ballistics",
          "[pulp-vellum-audio][multi-meter]") {
    MultiMeter meter;
    meter.set_channel_count(4);
    REQUIRE(meter.channel_count() == 4);
    meter.set_channel_count(20);
    REQUIRE(meter.channel_count() == pulp::signal::kMaxMeterChannels);

    pulp::signal::MultiChannelMeterData data;
    data.num_channels = 4;
    for (int ch = 0; ch < 4; ++ch) {
        data.channels[ch].peak = 0.2f * static_cast<float>(ch + 1);
        data.channels[ch].rms = 0.1f * static_cast<float>(ch + 1);
    }
    REQUIRE(meter.update(data, kFrame));
    REQUIRE(meter.channel_count() == 4);
    CHECK(meter.ballistics().channels[0].display_peak > 0.0f);
    CHECK(meter.ballistics().channels[3].display_peak >
          meter.ballistics().channels[0].display_peak);
}

TEST_CASE("Vellum MultiMeter layout and style setters request repaint",
          "[pulp-vellum-audio][multi-meter]") {
    MultiMeter meter;
    CHECK(meter.layout() == MultiMeter::Layout::vertical);
    CHECK(meter.display_style() == MultiMeter::DisplayStyle::continuous);

    auto before = ::vellum::view::View::damage_request_count();
    meter.set_layout(MultiMeter::Layout::horizontal);
    CHECK(meter.layout() == MultiMeter::Layout::horizontal);
    CHECK(::vellum::view::View::damage_request_count() > before);

    before = ::vellum::view::View::damage_request_count();
    meter.set_display_style(MultiMeter::DisplayStyle::segmented);
    CHECK(meter.display_style() == MultiMeter::DisplayStyle::segmented);
    CHECK(::vellum::view::View::damage_request_count() > before);

    before = ::vellum::view::View::damage_request_count();
    meter.set_channel_count(2);
    CHECK(::vellum::view::View::damage_request_count() > before);
}

TEST_CASE("Vellum MultiMeter goes quiet once its levels settle",
          "[pulp-vellum-audio][multi-meter][idle-gate]") {
    MultiMeter meter;
    auto data = three_channels();
    data.channels[0].clipped = false;

    // Control: the report can say yes.
    REQUIRE(meter.update(data, kFrame));

    // Silence decays to rest, then reports no movement.
    pulp::signal::MultiChannelMeterData silence;
    silence.num_channels = 3;
    for (int i = 0; i < 2000; ++i)
        (void)meter.update(silence, kFrame);
    for (int i = 0; i < 60; ++i)
        CHECK_FALSE(meter.update(silence, kFrame));
}

TEST_CASE("Vellum MultiMeter paints each layout and style", "[pulp-vellum-audio][multi-meter]") {
    const auto data = three_channels();

    MultiMeter vertical;
    vertical.set_bounds({0, 0, 90, 120});
    (void)vertical.update(data, 0.1f);
    RecordingCanvas vertical_canvas;
    vertical.paint(vertical_canvas);
    CHECK(vertical_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    CHECK(vertical_canvas.count(DrawCommand::Type::fill_rect) >= 4);
    CHECK(vertical_canvas.count(DrawCommand::Type::stroke_line) >= 6);

    MultiMeter horizontal;
    horizontal.set_bounds({0, 0, 120, 60});
    horizontal.set_layout(MultiMeter::Layout::horizontal);
    (void)horizontal.update(data, 0.1f);
    RecordingCanvas horizontal_canvas;
    horizontal.paint(horizontal_canvas);
    CHECK(horizontal_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    CHECK(horizontal_canvas.count(DrawCommand::Type::fill_rect) >= 4);
    CHECK(horizontal_canvas.count(DrawCommand::Type::stroke_line) >= 6);

    MultiMeter segmented;
    segmented.set_bounds({0, 0, 260, 48});
    segmented.set_layout(MultiMeter::Layout::horizontal);
    segmented.set_display_style(MultiMeter::DisplayStyle::segmented);
    (void)segmented.update(data, 0.1f);
    RecordingCanvas segmented_canvas;
    segmented.paint(segmented_canvas);
    CHECK(segmented_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    CHECK(segmented_canvas.count(DrawCommand::Type::fill_rect) >= 24);
    CHECK(segmented_canvas.count(DrawCommand::Type::fill_text) >= 4);
    CHECK(segmented_canvas.count(DrawCommand::Type::stroke_line) >= 8);
}

TEST_CASE("Vellum MultiMeter with no channels paints nothing", "[pulp-vellum-audio][multi-meter]") {
    MultiMeter meter;
    meter.set_bounds({0, 0, 80, 40});
    RecordingCanvas canvas;
    meter.paint(canvas);
    CHECK(canvas.command_count() == 0);
}

// The channel dims to 175/255 when only its peak, not its RMS, reaches a
// segment. Alpha is a [0,1] float in Vellum; an 8-bit literal would be an
// opaque HDR value and the dimming would silently never happen.
TEST_CASE("Vellum MultiMeter dims peak-only segments with a unit-range alpha",
          "[pulp-vellum-audio][multi-meter]") {
    pulp::signal::MultiChannelMeterData data;
    data.num_channels = 1;
    data.channels[0].rms = 0.1f;
    data.channels[0].peak = 0.9f;

    MultiMeter meter;
    meter.set_bounds({0, 0, 260, 48});
    meter.set_layout(MultiMeter::Layout::horizontal);
    meter.set_display_style(MultiMeter::DisplayStyle::segmented);
    for (int i = 0; i < 20; ++i)
        (void)meter.update(data, 0.1f);

    RecordingCanvas canvas;
    meter.paint(canvas);
    bool dimmed = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != DrawCommand::Type::set_fill_color)
            continue;
        CHECK(cmd.color.a <= 1.0f);
        dimmed = dimmed || cmd.color.a == Approx(175.0f / 255.0f).margin(1e-4);
    }
    CHECK(dimmed);
}

TEST_CASE("Vellum CorrelationMeter smooths and clamps its input",
          "[pulp-vellum-audio][correlation-meter]") {
    CorrelationMeter meter;
    CHECK(meter.display_correlation() == Approx(0.0).margin(0.001));

    for (int i = 0; i < 60; ++i)
        (void)meter.update(1.0f, kFrame);
    CHECK(meter.display_correlation() > 0.9f);

    for (int i = 0; i < 120; ++i)
        (void)meter.update(-1.0f, kFrame);
    CHECK(meter.display_correlation() < -0.9f);

    for (int i = 0; i < 120; ++i)
        (void)meter.update(5.0f, kFrame);
    CHECK(meter.display_correlation() == 1.0f);
}

TEST_CASE("Vellum CorrelationMeter ignores non-finite input",
          "[pulp-vellum-audio][correlation-meter]") {
    CorrelationMeter meter;
    for (int i = 0; i < 30; ++i)
        (void)meter.update(0.5f, kFrame);
    for (int i = 0; i < 2000; ++i)
        (void)meter.update(std::numeric_limits<float>::quiet_NaN(), kFrame);
    CHECK(std::isfinite(meter.display_correlation()));
    CHECK(meter.display_correlation() == 0.0f);
    CHECK_FALSE(meter.update(std::numeric_limits<float>::infinity(), kFrame));
}

// The needle rests wherever the signal is, not at zero, so the snap has to
// target the input value: a floor-to-zero test would never let it go quiet.
TEST_CASE("Vellum CorrelationMeter settles at a non-zero target and then goes quiet",
          "[pulp-vellum-audio][correlation-meter][idle-gate]") {
    CorrelationMeter meter;
    REQUIRE(meter.update(1.0f, kFrame)); // control: the report can say yes
    for (int i = 0; i < 120; ++i)
        (void)meter.update(1.0f, kFrame);
    CHECK(meter.display_correlation() == 1.0f);
    for (int i = 0; i < 120; ++i)
        CHECK_FALSE(meter.update(1.0f, kFrame));

    CHECK(meter.update(-1.0f, kFrame));
    for (int i = 0; i < 240; ++i)
        (void)meter.update(-1.0f, kFrame);
    CHECK(meter.display_correlation() == -1.0f);
    for (int i = 0; i < 120; ++i)
        CHECK_FALSE(meter.update(-1.0f, kFrame));
}

TEST_CASE("Vellum CorrelationMeter paints both polarities",
          "[pulp-vellum-audio][correlation-meter]") {
    CorrelationMeter meter;
    meter.set_bounds({0, 0, 100, 20});
    for (const float target : {2.0f, -2.0f}) {
        (void)meter.update(target, 1.0f);
        RecordingCanvas canvas;
        meter.paint(canvas);
        CHECK(canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
        CHECK(canvas.count(DrawCommand::Type::stroke_line) == 3);
        CHECK(canvas.count(DrawCommand::Type::fill_rect) == 1);
    }
}

TEST_CASE("Vellum meters expose the meter accessibility role", "[pulp-vellum-audio][multi-meter]") {
    MultiMeter multi;
    CorrelationMeter correlation;
    CHECK(multi.access_role() == ::vellum::view::View::AccessRole::meter);
    CHECK(correlation.access_role() == ::vellum::view::View::AccessRole::meter);
}

TEST_CASE("Vellum MultiMeter renders level colours into its channel columns",
          "[pulp-vellum-audio][multi-meter][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    // Three columns at distinct levels: below, between and above the warning
    // and error thresholds. Peak sits just above RMS so the peak line stays
    // clear of the bottom rows sampled below.
    pulp::signal::MultiChannelMeterData data;
    data.num_channels = 3;
    const float rms[3] = {0.5f, 0.8f, 0.95f};
    for (int ch = 0; ch < 3; ++ch) {
        data.channels[ch].rms = rms[ch];
        data.channels[ch].peak = rms[ch] + 0.03f;
    }

    MultiMeter meter;
    meter.set_bounds({0, 0, 90, 120});
    for (int i = 0; i < 8; ++i)
        (void)meter.update(data, 0.1f);
    const auto frame = render(meter, 90, 120);
    REQUIRE(frame.width == 90);
    REQUIRE(frame.height == 120);

    // Column centres: width 90, 3 columns, 2 px gaps.
    const auto low = frame.at(14, 112);
    const auto mid = frame.at(45, 112);
    const auto high = frame.at(75, 112);
    CHECK(low.g > low.r + 60); // green
    CHECK(low.g > low.b + 60);
    CHECK(mid.r > 200); // amber
    CHECK(mid.g > 140);
    CHECK(mid.b < 120);
    CHECK(high.r > 200); // red
    CHECK(high.g < 110);

    // Above each bar the track shows through, and the bars differ in height.
    const auto track = frame.at(14, 4);
    CHECK(track.g < 60);
    CHECK(track.r < 60);
    const auto above_mid = frame.at(45, 120 - static_cast<int>(0.8f * 120) - 6);
    CHECK(above_mid.g < 60);
    const auto above_low = frame.at(14, 120 - static_cast<int>(0.5f * 120) - 6);
    CHECK(above_low.g < 60);
    // The same row is inside the taller bars.
    CHECK(frame.at(75, 120 - static_cast<int>(0.5f * 120) - 6).r > 200);

    maybe_write_png(meter, 90, 120, "multi-meter-vertical.png");
}

TEST_CASE("Vellum MultiMeter renders the segmented horizontal style",
          "[pulp-vellum-audio][multi-meter][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    MultiMeter meter;
    meter.set_bounds({0, 0, 260, 60});
    meter.set_layout(MultiMeter::Layout::horizontal);
    meter.set_display_style(MultiMeter::DisplayStyle::segmented);
    auto data = three_channels();
    for (int i = 0; i < 8; ++i)
        (void)meter.update(data, 0.1f);

    const auto frame = render(meter, 260, 60);
    REQUIRE(frame.width == 260);

    // The lit segments differ from the dark background and from the same
    // meter at silence.
    MultiMeter silent;
    silent.set_bounds({0, 0, 260, 60});
    silent.set_layout(MultiMeter::Layout::horizontal);
    silent.set_display_style(MultiMeter::DisplayStyle::segmented);
    pulp::signal::MultiChannelMeterData quiet;
    quiet.num_channels = 3;
    (void)silent.update(quiet, 0.1f);
    const auto quiet_frame = render(silent, 260, 60);
    REQUIRE(quiet_frame.pixels.size() == frame.pixels.size());
    CHECK(quiet_frame.pixels != frame.pixels);

    // The first row, early in the bar (segment 3), is lit green when the signal is loud.
    const auto lit = frame.at(44, 10);
    CHECK(lit.g > lit.r + 40);
    const auto dark = quiet_frame.at(44, 10);
    CHECK(lit.g > dark.g + 80);

    maybe_write_png(meter, 260, 60, "multi-meter-segmented.png");
}

TEST_CASE("Vellum CorrelationMeter renders its needle, polarity colour and soft fill",
          "[pulp-vellum-audio][correlation-meter][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    CorrelationMeter positive;
    positive.set_bounds({0, 0, 100, 20});
    (void)positive.update(1.0f, 1.0f);
    const auto pos = render(positive, 100, 20);
    REQUIRE(pos.width == 100);

    // Needle at the right edge, green.
    const auto needle = pos.at(98, 10);
    CHECK(needle.g > 200);
    CHECK(needle.r < 40);
    // Soft fill between centre and needle: tinted green but translucent, so the
    // track still shows through (an opaque fill would read as the needle colour).
    const auto fill = pos.at(75, 10);
    CHECK(fill.g > fill.r + 15);
    CHECK(fill.g < 150);
    // Nothing is drawn on the other side of centre.
    const auto other = pos.at(20, 10);
    CHECK(other.g < 60);

    CorrelationMeter negative;
    negative.set_bounds({0, 0, 100, 20});
    (void)negative.update(-1.0f, 1.0f);
    const auto neg = render(negative, 100, 20);
    const auto neg_needle = neg.at(2, 10);
    CHECK(neg_needle.r > 200);
    CHECK(neg_needle.g < 40);
    CHECK(pos.pixels != neg.pixels);

    maybe_write_png(positive, 100, 20, "correlation-positive.png");
    maybe_write_png(negative, 100, 20, "correlation-negative.png");
}
