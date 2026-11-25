#include <pulp/vellum_audio/audio_inspector_panel.hpp>

#include <pulp/audio/audio_probe.hpp>
#include <pulp/audio/buffer.hpp>
#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/screenshot.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using pulp::audio::AudioProbe;
using pulp::audio::AudioProbeStage;
using pulp::audio::BufferView;
using pulp::vellum_audio::AudioInspectorMode;
using pulp::vellum_audio::AudioInspectorPanel;
using pulp::vellum_audio::AudioWaveformView;
using pulp::vellum_audio::dbfs_meter_fill;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;

namespace {

// PULP_VELLUM_AUDIO_PNG_DIR names a directory to receive the rendered frame
// for review; unset (the default) writes nothing.
void maybe_write_png(::vellum::view::View& view, std::uint32_t w, std::uint32_t h,
                     const std::string& name) {
    if (const char* dir = std::getenv("PULP_VELLUM_AUDIO_PNG_DIR"))
        CHECK(::vellum::view::render_to_file(view, w, h, std::string(dir) + "/" + name, 2.0f));
}

} // namespace

TEST_CASE("Audio Inspector meter fill is on the dBFS scale, not linear",
          "[vellum-audio][audio-inspector]") {
    // The meter bar and its "… dBFS" label must agree: the fill maps linear
    // amplitude through the same dBFS transform the label uses (−60 dBFS floor
    // → empty bar, 0 dBFS → full bar), not the raw linear amplitude.
    REQUIRE(dbfs_meter_fill(1.0f) == Catch::Approx(1.0f));   // 0 dBFS → full.
    REQUIRE(dbfs_meter_fill(0.0f) == 0.0f);                  // silence → empty.
    REQUIRE(dbfs_meter_fill(0.001f) == 0.0f);                // −60 dBFS → empty.

    // Half amplitude is ≈ −6 dBFS, which on a −60..0 scale is 0.9, NOT the 0.5
    // a linear meter would show — the assertion that distinguishes the two.
    REQUIRE(dbfs_meter_fill(0.5f) == Catch::Approx(0.9f).margin(0.005f));
    REQUIRE(dbfs_meter_fill(0.5f) > 0.6f); // decisively above the linear 0.5.

    // Monotonic and clamped above full scale.
    REQUIRE(dbfs_meter_fill(0.25f) < dbfs_meter_fill(0.5f));
    REQUIRE(dbfs_meter_fill(2.0f) == Catch::Approx(1.0f)); // > 0 dBFS clamps to 1.
}

TEST_CASE("AudioWaveformView trigger mode only repositions over real samples",
          "[vellum-audio][audio-inspector]") {
    AudioWaveformView waveform;
    const float samples[] = {0.4f, 0.2f, -0.3f, -0.1f, 0.05f, 0.3f};
    waveform.set_samples(samples, 6);

    REQUIRE(waveform.trigger_mode() == AudioWaveformView::TriggerMode::kRaw);
    REQUIRE(waveform.display_start_index() == 0);
    REQUIRE(waveform.display_sample_count() == 6);

    waveform.set_trigger_mode(AudioWaveformView::TriggerMode::kRisingZero);
    REQUIRE(waveform.display_start_index() == 4);
    REQUIRE(waveform.sample_at(waveform.display_start_index()) ==
            Catch::Approx(samples[4]));
    REQUIRE(waveform.display_sample_count() == 2);

    const float no_crossing[] = {-0.6f, -0.4f, -0.2f, -0.1f};
    waveform.set_samples(no_crossing, 4);
    REQUIRE(waveform.display_start_index() == 0);
    REQUIRE(waveform.display_sample_count() == 4);
}

TEST_CASE("AudioWaveformView horizontal scale shortens the real-sample window",
          "[vellum-audio][audio-inspector]") {
    AudioWaveformView waveform;
    const float samples[] = {-0.4f, -0.2f, 0.0f, 0.2f, 0.4f, 0.2f, 0.0f, -0.2f};
    waveform.set_samples(samples, 8);

    REQUIRE(waveform.horizontal_scale() == Catch::Approx(1.0f));
    REQUIRE(waveform.display_sample_count() == 8);

    waveform.set_horizontal_scale(2.0f);
    REQUIRE(waveform.horizontal_scale() == Catch::Approx(2.0f));
    REQUIRE(waveform.display_sample_count() == 4);

    waveform.set_horizontal_scale(0.25f);
    REQUIRE(waveform.horizontal_scale() == Catch::Approx(1.0f));
    REQUIRE(waveform.display_sample_count() == 8);
}

TEST_CASE("AudioWaveformView paints grid and rounded thicker trace",
          "[vellum-audio][audio-inspector]") {
    AudioWaveformView waveform;
    waveform.set_bounds({0, 0, 160, 80});
    const float samples[] = {-0.6f, -0.2f, 0.2f, 0.6f};
    waveform.set_samples(samples, 4);

    RecordingCanvas grid_canvas;
    waveform.paint(grid_canvas);
    REQUIRE(grid_canvas.count(DrawCommand::Type::stroke_line) >= 9);
    REQUIRE(grid_canvas.count(DrawCommand::Type::set_line_cap) >= 2);
    REQUIRE(grid_canvas.count(DrawCommand::Type::set_line_join) >= 2);

    bool saw_trace_width = false;
    for (const auto& cmd : grid_canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_line_width &&
            cmd.f[0] == Catch::Approx(1.75f)) {
            saw_trace_width = true;
        }
    }
    REQUIRE(saw_trace_width);

    waveform.set_show_grid(false);
    RecordingCanvas no_grid_canvas;
    waveform.paint(no_grid_canvas);
    REQUIRE(no_grid_canvas.count(DrawCommand::Type::stroke_line) <
            grid_canvas.count(DrawCommand::Type::stroke_line));
}

TEST_CASE("AudioInspectorPanel distinguishes Scope measurements from Signal readout",
          "[vellum-audio][audio-inspector]") {
    AudioInspectorPanel panel;
    panel.set_mode(AudioInspectorMode::kScope);

    pulp::audio::AudioProbeSnapshot snap{};
    snap.stage_id = AudioProbeStage::kStandaloneOutputBoundary;
    snap.channel_count = 1;
    snap.sample_rate = 48000.0;
    snap.block_size = 64;
    snap.sequence_number = 1;
    snap.peak_max = 0.5f;
    snap.rms_max = 0.25f;

    pulp::audio::AudioScopeResult scope;
    scope.stage = AudioProbeStage::kStandaloneOutputBoundary;
    scope.trigger_mode = pulp::audio::AudioScopeTriggerMode::kRisingZero;
    scope.acquisition.ok = true;
    scope.acquisition.window_samples = 64;
    scope.acquisition.selected_channel = 0;
    scope.acquisition.trigger_found = true;
    scope.measurements.peak_to_peak_available = true;
    scope.measurements.peak_to_peak = 1.0;
    scope.measurements.rms_available = true;
    scope.measurements.rms = 0.353553;
    scope.measurements.frequency_available = true;
    scope.measurements.frequency_hz = 440.0;
    scope.measurements.dc_offset_available = true;
    scope.measurements.dc_offset = 0.0;
    scope.measurements.crest_factor_available = true;
    scope.measurements.crest_factor = 1.414;

    panel.update(AudioInspectorPanel::Status::kLive, snap, {}, nullptr, 0);
    panel.set_scope_result(scope);

    REQUIRE(panel.status_text().find("Scope") != std::string::npos);
    REQUIRE(panel.level_text().find("Freq: 440.0 Hz") != std::string::npos);
    REQUIRE(panel.content_text().find("Trigger: rising-zero (locked)") !=
            std::string::npos);
}

TEST_CASE("AudioInspectorPanel keeps a peak hold while live level falls",
          "[vellum-audio][audio-inspector]") {
    AudioInspectorPanel panel;
    pulp::audio::AudioProbeSnapshot snap{};
    snap.peak_max = 1.0f;
    snap.rms_max = 0.5f;
    panel.update(AudioInspectorPanel::Status::kLive, snap, {}, nullptr, 0);

    const float held = panel.peak_meter_held_peak();
    REQUIRE(held > 0.0f);

    snap.peak_max = 0.05f;
    snap.rms_max = 0.05f;
    panel.update(AudioInspectorPanel::Status::kLive, snap, {}, nullptr, 0);
    REQUIRE(panel.peak_meter_held_peak() >= held);

    for (int i = 0; i < 180; ++i)
        panel.update(AudioInspectorPanel::Status::kLive, snap, {}, nullptr, 0);
    REQUIRE(panel.peak_meter_held_peak() < held);
}

TEST_CASE("AudioInspectorPanel drives the peak meter from the peak and the RMS meter from the RMS",
          "[vellum-audio][audio-inspector]") {
    // The shared level widget takes (rms, peak); the readout feeds each meter
    // the matching channel of the snapshot. Swapping the two would leave both
    // bars legal but wrong, so the held peaks pin which value reached which bar.
    AudioInspectorPanel panel;
    pulp::audio::AudioProbeSnapshot snap{};
    snap.peak_max = 0.5f;
    snap.rms_max = 0.05f;
    panel.update(AudioInspectorPanel::Status::kLive, snap, {}, nullptr, 0);

    REQUIRE(panel.peak_meter_held_peak() == Catch::Approx(dbfs_meter_fill(0.5f)).margin(0.01f));
    REQUIRE(panel.rms_meter_held_peak() == Catch::Approx(dbfs_meter_fill(0.05f)).margin(0.01f));
    REQUIRE(panel.peak_meter_held_peak() > panel.rms_meter_held_peak() + 0.2f);
}

TEST_CASE("AudioInspectorPanel shows the honest no-probe state and drops stale numbers",
          "[vellum-audio][audio-inspector]") {
    AudioInspectorPanel panel;
    pulp::audio::AudioProbeSnapshot snap{};
    snap.peak_max = 0.5f;
    snap.rms_max = 0.25f;
    snap.channel_count = 2;
    snap.rms[0] = 0.2f;
    snap.rms[1] = 0.1f;
    const float samples[] = {0.1f, -0.1f, 0.2f};
    panel.update(AudioInspectorPanel::Status::kLive, snap, {}, samples, 3);
    REQUIRE(panel.waveform().sample_count() == 3);
    REQUIRE(panel.lr_match() == Catch::Approx(0.5f));
    REQUIRE(panel.balance() == Catch::Approx(-1.0f / 3.0f));

    panel.update(AudioInspectorPanel::Status::kNoProbe, snap, {}, samples, 3);
    REQUIRE(panel.status_text().find("no probe") != std::string::npos);
    REQUIRE(panel.level_text().find("Peak: \xE2\x80\x94") != std::string::npos);
    REQUIRE(panel.waveform().sample_count() == 0);
    REQUIRE(panel.peak() == 0.0f);
    REQUIRE(panel.lr_match() == 0.0f);

    panel.update(AudioInspectorPanel::Status::kStale, snap, {}, samples, 3);
    REQUIRE(panel.status_text().find("stale") != std::string::npos);
    REQUIRE_FALSE(panel.waveform().is_live());
}

TEST_CASE("AudioInspectorPanel mode buttons and set_mode stay in step",
          "[vellum-audio][audio-inspector]") {
    AudioInspectorPanel panel;
    std::vector<AudioInspectorMode> seen;
    panel.on_mode_changed = [&](AudioInspectorMode m) { seen.push_back(m); };
    panel.set_mode(AudioInspectorMode::kScope);
    panel.set_mode(AudioInspectorMode::kScope);
    panel.set_mode(AudioInspectorMode::kSignal);
    REQUIRE(seen.size() == 2);
    REQUIRE(seen[0] == AudioInspectorMode::kScope);
    REQUIRE(seen[1] == AudioInspectorMode::kSignal);
    REQUIRE(panel.mode() == AudioInspectorMode::kSignal);
}

TEST_CASE("AudioInspectorPanel renders a non-empty headless snapshot",
          "[vellum-audio][audio-inspector]") {
    AudioProbe probe;
    AudioProbe::CaptureConfig cap;
    cap.capture_frames = AudioWaveformView::kCapacity;
    probe.prepare(2, 128, 48000.0, AudioProbeStage::kStandaloneOutputBoundary, cap);
    std::vector<float> left(128), right(128);
    for (std::size_t i = 0; i < left.size(); ++i) {
        const float s = std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
        left[i] = 0.6f * s;
        right[i] = 0.45f * s;
    }
    const float* ptrs[2] = {left.data(), right.data()};
    for (int b = 0; b < 8; ++b)
        probe.analyze_output(BufferView<const float>(ptrs, 2, left.size()));

    const auto snap = probe.latest();
    std::vector<float> wf(AudioWaveformView::kCapacity);
    const int n = probe.read_capture(wf.data(), static_cast<int>(wf.size()));
    REQUIRE(n > 0);

    AudioInspectorPanel panel;
    panel.set_bounds({0, 0, 300, 440});
    panel.update(AudioInspectorPanel::Status::kLive, snap, {}, wf.data(), n);
    REQUIRE(panel.waveform().sample_count() == n);
    REQUIRE(panel.status_text().find("live") != std::string::npos);
    REQUIRE(panel.level_text().find("dBFS") != std::string::npos);

    // Colours are in [0,1]; a 0-255 value fed to the float constructor would
    // clamp to white on the dark panel and hide every label.
    const auto sc = panel.status_color();
    REQUIRE(sc.r <= 1.0f);
    REQUIRE(sc.g <= 1.0f);
    REQUIRE(sc.b <= 1.0f);
    REQUIRE(sc.a <= 1.0f);

    std::uint32_t w = 0, h = 0;
    const auto pixels = ::vellum::view::render_to_rgba(panel, 300, 440, 1.0f, &w, &h);
    if (pixels.empty())
        SKIP("offscreen render unavailable at runtime");
    REQUIRE(w == 300);
    REQUIRE(h == 440);
    double sum = 0.0, sum2 = 0.0;
    const std::size_t count = static_cast<std::size_t>(w) * h;
    for (std::size_t i = 0; i < count; ++i) {
        const double lum = 0.2126 * pixels[i * 4] + 0.7152 * pixels[i * 4 + 1] +
                           0.0722 * pixels[i * 4 + 2];
        sum += lum;
        sum2 += lum * lum;
    }
    const double mean = sum / static_cast<double>(count);
    const double var = sum2 / static_cast<double>(count) - mean * mean;
    REQUIRE(mean < 128.0);          // dark panel, not a white-clamped frame
    REQUIRE(std::sqrt(std::max(var, 0.0)) > 1.0); // text and trace, not flat
    maybe_write_png(panel, 300, 440, "audio-inspector-live.png");
}
