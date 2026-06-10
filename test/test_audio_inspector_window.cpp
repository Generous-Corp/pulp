// test_audio_inspector_window.cpp — headless coverage for the developer
// Audio Inspector tool window (Phase 6 of the audio observability harness,
// planning/2026-06-09-audio-inspector-separate-tool-window-proposal.md).
//
// The window is a SIBLING of the layout InspectorWindow, not a tab in it. It
// owns its own state model and consumes the Phase 5 RT probe schema. These
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
#include <pulp/view/view.hpp>

#include <cmath>
#include <memory>
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

TEST_CASE("AudioInspectorPanel renders a non-empty headless snapshot",
          "[view][audio-inspector][audio-harness]") {
    // Visual proof the panel paints end-to-end (meters + waveform + labels)
    // without a window. Asserts a non-trivial PNG; skips honestly when the
    // build has no screenshot provider (CPU-only / headless CI without Skia).
    if (!has_screenshot_provider()) {
        SUCCEED("no screenshot provider in this build — render proof skipped");
        return;
    }

    AudioProbe probe;
    AudioProbe::CaptureConfig cap;
    cap.capture_frames = 256;
    probe.prepare(2, 128, 48000.0, AudioProbeStage::kStandaloneOutputBoundary, cap);
    StereoBlock block(128);
    for (std::size_t i = 0; i < block.left.size(); ++i) {
        block.left[i] = 0.5f * std::sin(static_cast<float>(i) * 0.2f);
        block.right[i] = 0.25f * std::sin(static_cast<float>(i) * 0.2f);
    }
    probe.analyze_output(block.view());

    AudioInspectorPanel panel;
    panel.set_bounds({0, 0, 300, 440});
    {
        AudioInspectorWindow window;
        window.set_probe(&probe);
        window.poll();
        REQUIRE(window.panel().status() == AudioInspectorPanel::Status::kLive);
    }
    // Drive the standalone panel directly so it owns the render tree.
    const auto snap = probe.latest();
    std::vector<float> wf(AudioWaveformView::kCapacity);
    const int n = probe.read_capture(wf.data(), static_cast<int>(wf.size()));
    panel.update(AudioInspectorPanel::Status::kLive, snap, {}, wf.data(), n);

    // Skia backend composites the meters + waveform correctly (CoreGraphics
    // is fine here too — no file images — but Skia is the faithful default).
    auto png = render_to_png(panel, 300, 440, 2.0f, ScreenshotBackend::skia);
    REQUIRE(png.size() > 100);  // a real PNG, not an empty/blank buffer
}

TEST_CASE("AudioInspectorWindow toggle command opens via the registry and does "
          "not dangle after destruction",
          "[view][audio-inspector][audio-harness]") {
    CommandRegistry reg;
    int factory_calls = 0;

    {
        AudioInspectorWindow window(nullptr, nullptr,
                                    null_host_factory(&factory_calls));
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
