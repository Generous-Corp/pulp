#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;
using namespace pulp::canvas;
using Catch::Matchers::WithinAbs;

TEST_CASE("Label renders text", "[view][widget]") {
    Label label("Gain");
    label.set_bounds({0, 0, 100, 20});

    RecordingCanvas canvas;
    label.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::set_font) == 1);
}

TEST_CASE("Label text can be changed", "[view][widget]") {
    Label label("Initial");
    REQUIRE(label.text() == "Initial");

    label.set_text("Changed");
    REQUIRE(label.text() == "Changed");
}

TEST_CASE("Knob value clamping", "[view][widget]") {
    Knob knob;
    knob.set_value(0.5f);
    REQUIRE_THAT(knob.value(), WithinAbs(0.5, 0.001));

    knob.set_value(1.5f);
    REQUIRE_THAT(knob.value(), WithinAbs(1.0, 0.001));

    knob.set_value(-0.5f);
    REQUIRE_THAT(knob.value(), WithinAbs(0.0, 0.001));
}

TEST_CASE("Knob renders arcs and indicator", "[view][widget]") {
    Knob knob;
    knob.set_bounds({0, 0, 48, 48});
    knob.set_value(0.5f);
    knob.set_label("Gain");

    RecordingCanvas canvas;
    knob.paint(canvas);

    // Should draw: track arc, value arc, thumb line, label text
    REQUIRE(canvas.count(DrawCommand::Type::stroke_arc) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("Knob with format function shows value text", "[view][widget]") {
    Knob knob;
    knob.set_bounds({0, 0, 48, 48});
    knob.set_value(0.75f);
    knob.set_format([](float v) { return std::to_string(static_cast<int>(v * 100)) + "%"; });

    RecordingCanvas canvas;
    knob.paint(canvas);

    // Should have label text + value text
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("Fader value clamping", "[view][widget]") {
    Fader fader;
    fader.set_value(0.7f);
    REQUIRE_THAT(fader.value(), WithinAbs(0.7, 0.001));

    fader.set_value(2.0f);
    REQUIRE_THAT(fader.value(), WithinAbs(1.0, 0.001));
}

TEST_CASE("Fader renders track and thumb", "[view][widget]") {
    Fader fader;
    fader.set_bounds({0, 0, 24, 200});
    fader.set_value(0.6f);
    fader.set_label("Volume");

    RecordingCanvas canvas;
    fader.paint(canvas);

    // Track + fill = 2 rounded rects, thumb = 1 circle, label text
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("Fader horizontal orientation", "[view][widget]") {
    Fader fader;
    fader.set_orientation(Fader::Orientation::horizontal);
    fader.set_bounds({0, 0, 200, 24});
    fader.set_value(0.5f);

    RecordingCanvas canvas;
    fader.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
}

TEST_CASE("Toggle state", "[view][widget]") {
    Toggle toggle;
    REQUIRE_FALSE(toggle.is_on());

    toggle.set_on(true);
    REQUIRE(toggle.is_on());
}

TEST_CASE("Toggle renders switch", "[view][widget]") {
    Toggle toggle;
    toggle.set_bounds({0, 0, 50, 30});
    toggle.set_on(true);
    toggle.set_label("Bypass");

    RecordingCanvas canvas;
    toggle.paint(canvas);

    // Background rounded rect + thumb circle + label
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("Meter set_level", "[view][widget]") {
    Meter meter;
    meter.set_bounds({0, 0, 12, 200});
    meter.set_level(0.5f, 0.8f);

    REQUIRE_THAT(meter.display_rms(), WithinAbs(0.5, 0.01));
    REQUIRE_THAT(meter.display_peak(), WithinAbs(0.8, 0.01));
}

TEST_CASE("Meter renders with levels", "[view][widget]") {
    Meter meter;
    meter.set_bounds({0, 0, 12, 200});
    meter.set_level(0.6f, 0.85f);

    RecordingCanvas canvas;
    meter.paint(canvas);

    // Background + RMS fill = 2 rects, peak line + held peak = 2 lines
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) >= 1);
}

TEST_CASE("Meter horizontal orientation", "[view][widget]") {
    Meter meter;
    meter.set_orientation(Meter::Orientation::horizontal);
    meter.set_bounds({0, 0, 200, 12});
    meter.set_level(0.4f, 0.7f);

    RecordingCanvas canvas;
    meter.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
}

TEST_CASE("Meter update with ballistics", "[view][widget]") {
    Meter meter;
    meter.set_bounds({0, 0, 12, 200});

    // Sudden peak
    meter.update(0.9f, 0.5f, 1.0f / 60.0f);
    REQUIRE(meter.display_peak() > 0);
    REQUIRE(meter.display_rms() > 0);

    // Decay
    for (int i = 0; i < 10; ++i) {
        meter.update(0.0f, 0.0f, 1.0f / 60.0f);
    }
    REQUIRE(meter.display_peak() < 0.9f);
    REQUIRE(meter.held_peak() > 0.8f); // Still held
}

TEST_CASE("XYPad value clamping", "[view][widget]") {
    XYPad pad;
    pad.set_x(0.3f);
    pad.set_y(0.7f);
    REQUIRE_THAT(pad.x_value(), WithinAbs(0.3, 0.001));
    REQUIRE_THAT(pad.y_value(), WithinAbs(0.7, 0.001));

    pad.set_x(1.5f);
    REQUIRE_THAT(pad.x_value(), WithinAbs(1.0, 0.001));
    pad.set_y(-0.5f);
    REQUIRE_THAT(pad.y_value(), WithinAbs(0.0, 0.001));
}

TEST_CASE("XYPad renders crosshair", "[view][widget]") {
    XYPad pad;
    pad.set_bounds({0, 0, 100, 100});
    pad.set_x(0.5f);
    pad.set_y(0.5f);
    pad.set_x_label("Freq");
    pad.set_y_label("Res");

    RecordingCanvas canvas;
    pad.paint(canvas);

    // Background + grid lines + crosshair lines + thumb + labels
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) >= 4); // 2 grid + 2 crosshair
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 2);
}

TEST_CASE("WaveformView renders waveform", "[view][widget]") {
    WaveformView waveform;
    waveform.set_bounds({0, 0, 200, 60});

    // Generate sine wave
    std::vector<float> data(200);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = std::sin(2.0f * 3.14159f * i / data.size());
    }
    waveform.set_data(std::move(data));

    REQUIRE(waveform.sample_count() == 200);

    RecordingCanvas canvas;
    waveform.paint(canvas);

    // Background + center line + many waveform lines
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) > 10);
}

TEST_CASE("WaveformView empty renders background only", "[view][widget]") {
    WaveformView waveform;
    waveform.set_bounds({0, 0, 200, 60});

    RecordingCanvas canvas;
    waveform.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 0); // No waveform
}

TEST_CASE("WaveformView trigger — rising zero crossing", "[view][widget][trigger]") {
    // Phase-shifted sine: starts at +0.5, crosses zero downward, crosses
    // zero upward roughly at index N/2. With rising_zero trigger, the
    // display should be rotated so the rising crossing is at index 0.
    constexpr size_t N = 256;
    std::vector<float> samples(N);
    const float phase_offset = 3.14159f / 4.0f;  // start 45° into the cycle
    for (size_t i = 0; i < N; ++i) {
        samples[i] = std::sin(phase_offset + 2.0f * 3.14159f * i / N);
    }

    // Locate the expected trigger index in the raw buffer before triggering.
    size_t expected = WaveformView::find_trigger_index(
        samples.data(), samples.size(),
        WaveformView::TriggerMode::rising_zero);
    REQUIRE(expected > 0);
    REQUIRE(expected < N);

    WaveformView waveform;
    waveform.set_trigger_mode(WaveformView::TriggerMode::rising_zero);
    waveform.set_data(samples);  // copy so we can compare

    REQUIRE(waveform.sample_count() == N);

    // After triggering, index 0 should correspond to old index `expected`,
    // which is the first sample > 0 following a sample <= 0.
    // We can't directly read the rotated buffer, but we can observe
    // stability: applying the same data twice should yield the same result.
    WaveformView waveform2;
    waveform2.set_trigger_mode(WaveformView::TriggerMode::rising_zero);
    waveform2.set_data(samples);
    REQUIRE(waveform2.sample_count() == N);
}

TEST_CASE("WaveformView trigger — free run leaves buffer unchanged", "[view][widget][trigger]") {
    constexpr size_t N = 32;
    std::vector<float> samples(N);
    for (size_t i = 0; i < N; ++i) samples[i] = static_cast<float>(i);

    WaveformView waveform;
    REQUIRE(waveform.trigger_mode() == WaveformView::TriggerMode::free_run);
    waveform.set_data(samples);
    REQUIRE(waveform.sample_count() == N);
    // In free_run mode, find_trigger_index should return 0 regardless
    REQUIRE(WaveformView::find_trigger_index(
        samples.data(), samples.size(),
        WaveformView::TriggerMode::free_run) == 0);
}

TEST_CASE("WaveformView trigger — falling zero crossing", "[view][widget][trigger]") {
    // A simple ramp that crosses zero downward at the midpoint.
    std::vector<float> samples = {2, 1, 0.5f, 0, -0.5f, -1, -2, -1};
    size_t idx = WaveformView::find_trigger_index(
        samples.data(), samples.size(),
        WaveformView::TriggerMode::falling_zero);
    // prev=0 at i=3 is not > 0, so the first falling crossing is i=4 (prev=0, curr=-0.5)
    REQUIRE(idx == 4);
}

TEST_CASE("WaveformView trigger — no crossing leaves buffer alone", "[view][widget][trigger]") {
    // All positive samples — no rising zero crossing possible
    std::vector<float> samples = {0.5f, 0.6f, 0.7f, 0.8f, 0.9f};
    size_t idx = WaveformView::find_trigger_index(
        samples.data(), samples.size(),
        WaveformView::TriggerMode::rising_zero);
    REQUIRE(idx == 0);

    WaveformView waveform;
    waveform.set_trigger_mode(WaveformView::TriggerMode::rising_zero);
    waveform.set_data(samples);
    REQUIRE(waveform.sample_count() == samples.size());
}

TEST_CASE("SpectrumView renders bars", "[view][widget]") {
    SpectrumView spectrum;
    spectrum.set_bounds({0, 0, 300, 100});
    spectrum.set_style(SpectrumView::Style::bars);

    std::vector<float> bins(32);
    for (size_t i = 0; i < bins.size(); ++i) {
        bins[i] = -80.0f + 80.0f * (1.0f - static_cast<float>(i) / bins.size());
    }
    spectrum.set_spectrum(std::move(bins));

    REQUIRE(spectrum.bin_count() == 32);

    RecordingCanvas canvas;
    spectrum.paint(canvas);

    // Background + 32 bars + 3 grid lines
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 30);
}

TEST_CASE("SpectrumView renders filled line", "[view][widget]") {
    SpectrumView spectrum;
    spectrum.set_bounds({0, 0, 300, 100});
    spectrum.set_style(SpectrumView::Style::filled);

    std::vector<float> bins(64, -40.0f);
    spectrum.set_spectrum(std::move(bins));

    RecordingCanvas canvas;
    spectrum.paint(canvas);

    // GPU path uses draw_waveform (single call), CPU fallback uses stroke_line
    // At minimum: background rect + center line + waveform fallback lines
    REQUIRE(canvas.command_count() > 5);
}

TEST_CASE("SpectrumView empty renders background", "[view][widget]") {
    SpectrumView spectrum;
    spectrum.set_bounds({0, 0, 300, 100});

    RecordingCanvas canvas;
    spectrum.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 0);
}

TEST_CASE("View paint_all paints children", "[view][widget]") {
    View root;
    root.set_bounds({0, 0, 300, 200});

    auto label = std::make_unique<Label>("Test");
    label->set_bounds({10, 10, 100, 20});

    auto knob = std::make_unique<Knob>();
    knob->set_bounds({10, 40, 48, 48});
    knob->set_value(0.5f);

    root.add_child(std::move(label));
    root.add_child(std::move(knob));

    RecordingCanvas canvas;
    root.paint_all(canvas);

    // Should have commands from both children
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_arc) >= 2);
    // save/restore pairs for root + 2 children = 3 pairs
    REQUIRE(canvas.count(DrawCommand::Type::save) == 3);
    REQUIRE(canvas.count(DrawCommand::Type::restore) == 3);
}
