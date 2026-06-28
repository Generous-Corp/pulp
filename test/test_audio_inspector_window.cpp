// test_audio_inspector_window.cpp — headless coverage for the developer
// Audio Inspector tool window.
//
// The window is a SIBLING of the layout InspectorWindow, not a tab in it. It
// owns its own state model and consumes the RT probe schema. These
// tests drive a REAL AudioProbe as the synthetic source (the production data
// path — analyze_output() publishes snapshots, the window reads latest()), and
// assert on state, not pixels:
//
//   * meters / status reflect the snapshot the probe published;
//   * the observed stage follows the probe's configured stage;
//   * the honest "no probe" / "stale" states show when no probe is wired or
//     the sequence number stops advancing;
//   * the toggle command opens/focuses via a CommandRegistry, and the RAII
//     handler does not dangle after the window is destroyed;
//   * Cmd+I (layout inspector) still works alongside Cmd+Shift+A (audio
//     inspector) — no chord clobber.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_probe.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/view/audio_inspector_window.hpp>
#include <pulp/view/command_registry.hpp>
#include <pulp/view/inspector_window.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/view.hpp>
#include <pulp/state/properties_file.hpp>

#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace pulp::view;
using pulp::audio::AudioProbe;
using pulp::audio::AudioProbeStage;
using pulp::audio::BufferView;

namespace {

#ifdef __APPLE__
constexpr std::uint16_t kMainMod = kModCmd;
#else
constexpr std::uint16_t kMainMod = kModCtrl;
#endif

// Toggle chord for the audio inspector: Cmd+Shift+A / Ctrl+Shift+A.
KeyEvent audio_toggle_chord() {
    KeyEvent e;
    e.key = KeyCode::a;
    e.modifiers = kMainMod | kModShift;
    e.is_down = true;
    return e;
}

// Toggle chord for the layout inspector: Cmd+I / Ctrl+I.
KeyEvent layout_toggle_chord() {
    KeyEvent e;
    e.key = KeyCode::i;
    e.modifiers = kMainMod;
    e.is_down = true;
    return e;
}

// Fixed-capacity stereo block over stable storage; channel pointers built once.
struct StereoBlock {
    std::vector<float> left;
    std::vector<float> right;
    std::vector<const float*> ptrs;

    explicit StereoBlock(std::size_t frames)
        : left(frames, 0.0f), right(frames, 0.0f) {
        ptrs = {left.data(), right.data()};
    }

    BufferView<const float> view() const {
        return BufferView<const float>(ptrs.data(), 2, left.size());
    }

    void fill(float l, float r) {
        std::fill(left.begin(), left.end(), l);
        std::fill(right.begin(), right.end(), r);
    }
};

// Headless host factory: returns nullptr so show()/toggle() stay window-free
// while still proving dispatch reached the handler.
AudioInspectorWindow::HostFactory null_host_factory(int* calls) {
    return [calls](View&, const WindowOptions&) -> std::unique_ptr<WindowHost> {
        if (calls) ++*calls;
        return nullptr;
    };
}

void set_test_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

void unset_test_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

struct ScopedEnv {
    explicit ScopedEnv(const char* name, std::string value)
        : name_(name) {
        if (const char* prev = std::getenv(name_.c_str())) {
            previous_ = std::string(prev);
        }
        set_test_env(name_.c_str(), value);
    }

    ~ScopedEnv() {
        if (previous_) set_test_env(name_.c_str(), *previous_);
        else unset_test_env(name_.c_str());
    }

    std::string name_;
    std::optional<std::string> previous_;
};

}  // namespace

TEST_CASE("AudioInspectorWindow shows the honest no-probe state until wired",
          "[view][audio-inspector][audio-harness]") {
    AudioInspectorWindow window;
    REQUIRE(window.probe() == nullptr);

    window.poll();
    REQUIRE(window.panel().status() == AudioInspectorPanel::Status::kNoProbe);
    // No faked zeros — the unavailable state carries no stage / sequence.
    REQUIRE(window.panel().observed_stage() == AudioProbeStage::kUnknown);
    REQUIRE(window.panel().sequence_number() == 0);
}

TEST_CASE("AudioInspectorWindow meters reflect the probe snapshot",
          "[view][audio-inspector][audio-harness]") {
    AudioProbe probe;
    probe.prepare(2, 256, 48000.0, AudioProbeStage::kStandaloneOutputBoundary);

    StereoBlock block(256);
    block.fill(0.5f, 0.25f);          // distinct L/R for balance
    block.left[3] = 2.0f;             // one clipped sample
    probe.analyze_output(block.view());

    AudioInspectorWindow window;
    window.set_probe(&probe);
    window.poll();

    const auto& panel = window.panel();
    REQUIRE(panel.status() == AudioInspectorPanel::Status::kLive);
    REQUIRE(panel.observed_stage() == AudioProbeStage::kStandaloneOutputBoundary);
    REQUIRE(panel.sequence_number() == 1);

    // Peak picked up the clip; RMS is positive but below peak.
    REQUIRE(panel.peak() >= 2.0f);
    REQUIRE(panel.rms() > 0.0f);
    REQUIRE(panel.rms() < panel.peak());
    REQUIRE(panel.clip_count() == 1);
    REQUIRE(panel.nan_inf_count() == 0);

    // Balance leans right-light: L (0.5) louder than R (0.25) → negative.
    REQUIRE(panel.balance() < 0.0f);
    // Both channels carry energy → positive L/R level-match.
    REQUIRE(panel.lr_match() > 0.0f);
}

TEST_CASE("Audio Inspector meter fill is on the dBFS scale, not linear",
          "[view][audio-inspector][audio-harness]") {
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

TEST_CASE("AudioInspectorWindow goes stale when the sequence stops advancing",
          "[view][audio-inspector][audio-harness]") {
    AudioProbe probe;
    probe.prepare(1, 64, 44100.0, AudioProbeStage::kProcessorOutput);

    StereoBlock block(64);
    block.fill(0.3f, 0.0f);
    probe.analyze_output(block.view());

    AudioInspectorWindow window;
    window.set_probe(&probe);

    // First poll observes a freshly advanced sequence → live.
    window.poll();
    REQUIRE(window.panel().status() == AudioInspectorPanel::Status::kLive);
    const auto seq = window.panel().sequence_number();
    REQUIRE(seq >= 1);

    // No further analyze_output() — the sequence number is frozen. The next
    // poll must report stale, not a fake-live readout.
    window.poll();
    REQUIRE(window.panel().status() == AudioInspectorPanel::Status::kStale);
    REQUIRE(window.panel().sequence_number() == seq);

    // A new block advances the sequence → live again.
    probe.analyze_output(block.view());
    window.poll();
    REQUIRE(window.panel().status() == AudioInspectorPanel::Status::kLive);
    REQUIRE(window.panel().sequence_number() > seq);
}

TEST_CASE("AudioInspectorWindow stage selector follows the observed probe",
          "[view][audio-inspector][audio-harness]") {
    AudioProbe boundary;
    boundary.prepare(2, 128, 48000.0, AudioProbeStage::kStandaloneOutputBoundary);
    AudioProbe device;
    device.prepare(2, 128, 48000.0, AudioProbeStage::kDeviceCallback);

    StereoBlock block(128);
    block.fill(0.2f, 0.2f);
    boundary.analyze_output(block.view());
    device.analyze_output(block.view());

    AudioInspectorWindow window;

    window.set_probe(&boundary);
    window.poll();
    REQUIRE(window.panel().observed_stage() ==
            AudioProbeStage::kStandaloneOutputBoundary);

    // Switching the observed stage = pointing the window at a different probe.
    window.set_probe(&device);
    window.poll();
    REQUIRE(window.panel().observed_stage() == AudioProbeStage::kDeviceCallback);

    // Clearing the probe drops back to the honest unavailable state.
    window.set_probe(nullptr);
    REQUIRE(window.panel().status() == AudioInspectorPanel::Status::kNoProbe);
}

TEST_CASE("AudioInspectorWindow copies a fixed-capacity waveform off the probe",
          "[view][audio-inspector][audio-harness]") {
    AudioProbe probe;
    AudioProbe::CaptureConfig cap;
    cap.capture_frames = 256;
    probe.prepare(1, 128, 48000.0, AudioProbeStage::kProcessorOutput, cap);

    // A ramped block so the copied trace is non-constant.
    StereoBlock block(128);
    for (std::size_t i = 0; i < block.left.size(); ++i)
        block.left[i] = 0.5f * std::sin(static_cast<float>(i) * 0.2f);
    probe.analyze_output(block.view());

    AudioInspectorWindow window;
    window.set_probe(&probe);
    window.poll();

    const auto& wf = window.panel().waveform();
    REQUIRE(wf.is_live());
    REQUIRE(wf.sample_count() > 0);
    REQUIRE(wf.sample_count() <= AudioWaveformView::kCapacity);
}

TEST_CASE("AudioWaveformView trigger mode only repositions over real samples",
          "[view][audio-inspector][audio-harness]") {
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
          "[view][audio-inspector][audio-harness]") {
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
          "[view][audio-inspector][audio-harness]") {
    AudioWaveformView waveform;
    waveform.set_bounds({0, 0, 160, 80});
    const float samples[] = {-0.6f, -0.2f, 0.2f, 0.6f};
    waveform.set_samples(samples, 4);

    pulp::canvas::RecordingCanvas grid_canvas;
    waveform.paint(grid_canvas);
    REQUIRE(grid_canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) >= 9);
    REQUIRE(grid_canvas.count(pulp::canvas::DrawCommand::Type::set_line_cap) >= 2);
    REQUIRE(grid_canvas.count(pulp::canvas::DrawCommand::Type::set_line_join) >= 2);

    bool saw_trace_width = false;
    for (const auto& cmd : grid_canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_line_width &&
            cmd.f[0] == Catch::Approx(1.75f)) {
            saw_trace_width = true;
        }
    }
    REQUIRE(saw_trace_width);

    waveform.set_show_grid(false);
    pulp::canvas::RecordingCanvas no_grid_canvas;
    waveform.paint(no_grid_canvas);
    REQUIRE(no_grid_canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) <
            grid_canvas.count(pulp::canvas::DrawCommand::Type::stroke_line));
}

TEST_CASE("AudioInspectorWindow applies display-only waveform env knobs",
          "[view][audio-inspector][audio-harness]") {
    ScopedEnv trigger("PULP_AUDIO_INSPECTOR_TRIGGER", "rising-zero");
    ScopedEnv grid("PULP_AUDIO_INSPECTOR_GRID", "0");
    ScopedEnv scale("PULP_AUDIO_INSPECTOR_SCALE", "2.5");

    AudioInspectorWindow window;
    const auto& waveform = window.panel().waveform();
    REQUIRE(waveform.trigger_mode() == AudioWaveformView::TriggerMode::kRisingZero);
    REQUIRE_FALSE(waveform.show_grid());
    REQUIRE(waveform.horizontal_scale() == Catch::Approx(2.5f));
}

TEST_CASE("AudioInspectorWindow ignores non-finite waveform scale",
          "[view][audio-inspector][audio-harness]") {
    ScopedEnv scale("PULP_AUDIO_INSPECTOR_SCALE", "nan");

    AudioInspectorWindow window;
    const auto& waveform = window.panel().waveform();
    REQUIRE(waveform.horizontal_scale() == Catch::Approx(1.0f));
}

TEST_CASE("AudioInspectorWindow remembers Signal and Scope mode preference",
          "[view][audio-inspector][audio-scope]") {
    pulp::state::PropertiesFile prefs;

    AudioInspectorWindow first;
    first.set_preferences(&prefs);
    REQUIRE(first.mode() == AudioInspectorMode::kSignal);
    REQUIRE(first.panel().mode() == AudioInspectorMode::kSignal);

    first.set_mode(AudioInspectorMode::kScope);
    REQUIRE(first.mode() == AudioInspectorMode::kScope);
    REQUIRE(first.panel().mode() == AudioInspectorMode::kScope);
    REQUIRE(prefs.get_string("audio_inspector.mode").value_or("") == "scope");

    AudioInspectorWindow second;
    second.set_preferences(&prefs);
    REQUIRE(second.mode() == AudioInspectorMode::kScope);
    REQUIRE(second.panel().mode() == AudioInspectorMode::kScope);

    second.set_mode(AudioInspectorMode::kSignal);
    REQUIRE(prefs.get_string("audio_inspector.mode").value_or("") == "signal");
}

TEST_CASE("AudioInspectorPanel distinguishes Scope measurements from Signal readout",
          "[view][audio-inspector][audio-scope]") {
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
          "[view][audio-inspector][audio-harness]") {
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

TEST_CASE("AudioInspectorPanel renders a non-empty headless snapshot",
          "[view][audio-inspector][audio-harness]") {
    // Visual proof the panel paints end-to-end (meters + waveform + labels)
    // through the offscreen GPU surface (Dawn + Skia). Skips honestly when this
    // build has no GPU capture (CPU-only / no Skia / headless CI without GPU).
    if (!has_gpu_capture()) {
        SKIP("no GPU capture in this build — render proof cannot run");
        return;
    }

    AudioProbe probe;
    AudioProbe::CaptureConfig cap;
    cap.capture_frames = AudioWaveformView::kCapacity;
    probe.prepare(2, 128, 48000.0, AudioProbeStage::kStandaloneOutputBoundary, cap);
    // Several blocks of a 440 Hz sine so the meters read a real level and the
    // capture ring fills with a visible waveform (one block of 128 frames is
    // less than the display width — feed enough to populate the trace).
    StereoBlock block(128);
    for (std::size_t i = 0; i < block.left.size(); ++i) {
        const float s = std::sin(2.0f * 3.14159265f * 440.0f *
                                 static_cast<float>(i) / 48000.0f);
        block.left[i] = 0.6f * s;
        block.right[i] = 0.45f * s;
    }
    for (int b = 0; b < 8; ++b) probe.analyze_output(block.view());

    // Read the snapshot + captured waveform DIRECTLY (no intervening window
    // poll() — poll() drains the single-consumer capture FIFO, which would
    // leave the panel's waveform silently empty).
    const auto snap = probe.latest();
    std::vector<float> wf(AudioWaveformView::kCapacity);
    const int n = probe.read_capture(wf.data(), static_cast<int>(wf.size()));
    REQUIRE(n > 0);  // the waveform the panel renders is genuinely non-empty

    AudioInspectorPanel panel;
    panel.set_bounds({0, 0, 300, 440});
    panel.update(AudioInspectorPanel::Status::kLive, snap, {}, wf.data(), n);
    REQUIRE(panel.status() == AudioInspectorPanel::Status::kLive);
    REQUIRE(panel.waveform().sample_count() == n);

    // The labels actually carry the live readout text — the panel is not
    // silently blank. (The text strings were always set; this guards the data
    // path, the pixel assertion below guards that they actually render.)
    REQUIRE(panel.status_text().find("live") != std::string::npos);
    REQUIRE(panel.level_text().find("dBFS") != std::string::npos);
    // The heading color must be a valid [0,1] Color. The white-on-white bug
    // that hid every label fed 0-255 values into the [0,1] ctor, so the
    // channels read >1 and Skia clamped them to white. Assert they're in range.
    const auto sc = panel.status_color();
    REQUIRE(sc.r <= 1.0f);
    REQUIRE(sc.g <= 1.0f);
    REQUIRE(sc.b <= 1.0f);
    REQUIRE(sc.a <= 1.0f);

    // Render through the offscreen GPU surface. `has_gpu_capture()` only proves
    // the GPU backend is COMPILED in — the Dawn/Metal device can still fail to
    // initialize at RUNTIME (e.g. a headless CI session with no GPU access), in
    // which case render_to_png_gpu returns an empty buffer. Skip the pixel
    // assertions honestly there; the white-on-white regression is already caught
    // GPU-independently by the status_color() channel-range check above.
    auto png = render_to_png_gpu(panel, 300, 440, 2.0f);
    if (png.empty()) {
        SKIP("offscreen GPU device unavailable at runtime — pixel proof cannot run");
        return;
    }
    REQUIRE(png.size() > 100);  // a real PNG, not an empty/blank buffer
    const auto out =
        (std::filesystem::temp_directory_path() / "pulp-audio-inspector-live.png")
            .string();
    std::ofstream(out, std::ios::binary)
        .write(reinterpret_cast<const char*>(png.data()),
               static_cast<std::streamsize>(png.size()));

    // Pixel-level guard against the white-on-white regression: the panel paints
    // a dark UI background ({26,26,32}), so a correctly-rendered frame is
    // predominantly dark. The bug clamped every fill/text/trace to white, which
    // makes the frame predominantly bright — a high luminance mean. Assert the
    // frame is dark AND has real content variation (text + waveform pixels).
    // luminance_mean / stddev are on a 0-255 scale. A correct dark UI sits well
    // under mid-gray (~30 here); the white-clamped bug pushed it toward 255.
    const ScreenshotContentStats stats = analyze_screenshot_content(png);
    REQUIRE(stats.valid);
    REQUIRE(stats.luminance_mean < 128.0);   // dark UI, not a white-clamped frame
    REQUIRE(stats.luminance_stddev > 1.0);   // real content (text + trace), not flat
}

TEST_CASE("AudioInspectorWindow toggle command opens via the registry and does "
          "not dangle after destruction",
          "[view][audio-inspector][audio-harness]") {
    CommandRegistry reg;
    int factory_calls = 0;
    bool captured_root_background = false;
    pulp::canvas::Color root_background;

    {
        AudioInspectorWindow window(
            nullptr, nullptr,
            [&](View& root, const WindowOptions&) -> std::unique_ptr<WindowHost> {
                ++factory_calls;
                captured_root_background = root.has_background_color();
                root_background = root.background_color();
                return nullptr;
            });
        window.register_command_handler(reg);

        // The registry path must not clobber any raw global-key hook.
        REQUIRE(reg.handler_count() == 1);
        REQUIRE(reg.shortcuts().find(KeyCode::a, kMainMod | kModShift) ==
                AudioInspectorWindow::kToggleAudioInspector);
        REQUIRE(reg.command_info(AudioInspectorWindow::kToggleAudioInspector)
                    ->category == "Developer");

        // Distinct command id from the layout inspector.
        REQUIRE(AudioInspectorWindow::kToggleAudioInspector !=
                InspectorWindow::kToggleCommand);

        // Toggle chord routes to the window's handler → show() → host factory.
        REQUIRE(reg.dispatch_key_event(audio_toggle_chord()));
        REQUIRE(factory_calls == 1);
        REQUIRE(captured_root_background);
        REQUIRE(root_background.r <= 1.0f);
        REQUIRE(root_background.g <= 1.0f);
        REQUIRE(root_background.b <= 1.0f);
        REQUIRE(root_background.a <= 1.0f);

        // Re-registering with the same registry must not double-add.
        window.register_command_handler(reg);
        REQUIRE(reg.handler_count() == 1);
    }

    // RAII: the destroyed window removed its handler. Dispatch after destruction
    // must not crash and must report unhandled; the command + chord persist.
    REQUIRE(reg.handler_count() == 0);
    REQUIRE_FALSE(reg.dispatch_key_event(audio_toggle_chord()));
    REQUIRE(factory_calls == 1);
    REQUIRE(reg.shortcuts().find(KeyCode::a, kMainMod | kModShift) ==
            AudioInspectorWindow::kToggleAudioInspector);
}

TEST_CASE("Layout and audio inspectors coexist on one registry without clobber",
          "[view][audio-inspector][audio-harness]") {
    CommandRegistry reg;
    View target_root;
    int layout_calls = 0;
    int audio_calls = 0;

    InspectorWindow layout(
        target_root, nullptr, nullptr,
        [&](View&, const WindowOptions&) -> std::unique_ptr<WindowHost> {
            ++layout_calls;
            return nullptr;
        });
    AudioInspectorWindow audio(nullptr, nullptr, null_host_factory(&audio_calls));

    layout.register_command_handler(reg);
    audio.register_command_handler(reg);

    // Both tools registered; both chords distinct and live.
    REQUIRE(reg.handler_count() == 2);
    REQUIRE(reg.shortcuts().find(KeyCode::i, kMainMod) ==
            InspectorWindow::kToggleCommand);
    REQUIRE(reg.shortcuts().find(KeyCode::a, kMainMod | kModShift) ==
            AudioInspectorWindow::kToggleAudioInspector);

    // Cmd/Ctrl+I drives only the layout inspector.
    REQUIRE(reg.dispatch_key_event(layout_toggle_chord()));
    REQUIRE(layout_calls == 1);
    REQUIRE(audio_calls == 0);

    // Cmd/Ctrl+Shift+A drives only the audio inspector.
    REQUIRE(reg.dispatch_key_event(audio_toggle_chord()));
    REQUIRE(layout_calls == 1);
    REQUIRE(audio_calls == 1);
}
