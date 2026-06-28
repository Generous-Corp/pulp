// test_standalone_audio_inspector.cpp — the standalone-host wiring proof for
// the developer Audio Inspector.
//
// The window tests in test_audio_inspector_window.cpp prove the window in
// isolation. This file proves the SEAM the standalone host owns: the exact
// `AudioInspectorWindow::set_probe(&StandaloneApp::output_probe())` +
// `poll()` contract that `run_with_editor()` wires. Driving the full GPU host
// headlessly (device callback → analyze_output) is impractical in a unit test,
// so we observe the same `output_probe()` reference the host hands the window,
// feed it the way the audio callback would, and assert the window reflects it.
//
// Without a GPU WindowHost the window never creates a native surface, but the
// data path (probe → snapshot → panel state) is fully exercised window-side.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/standalone.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/view/audio_inspector_window.hpp>
#include <pulp/view/audio_inspector_panel.hpp>

#include <cmath>
#include <memory>
#include <vector>

#if PULP_ENABLE_AUDIO_PROBES

using namespace pulp::format;
using pulp::audio::AudioProbe;
using pulp::audio::AudioProbeStage;
using pulp::audio::BufferView;
using pulp::view::AudioInspectorPanel;
using pulp::view::AudioInspectorWindow;
using pulp::view::AudioWaveformView;

namespace {

// Minimal pass-through processor; the test feeds the probe directly, so the
// processor body is never exercised — it exists only so StandaloneApp::start()
// has something to prepare.
class SilentProc : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "StandaloneAudioInspectorProc";
        d.manufacturer = "Pulp";
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ProcessContext&) override {}
};

std::unique_ptr<Processor> make_silent_proc() {
    return std::make_unique<SilentProc>();
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
};

}  // namespace

TEST_CASE("StandaloneApp wires its output probe into the Audio Inspector window",
          "[format][standalone][audio-inspector][audio-harness]") {
    StandaloneApp app(&make_silent_proc);

    StandaloneConfig cfg;
    cfg.headless = true;
    cfg.output_channels = 2;
    cfg.buffer_size = 128;
    cfg.sample_rate = 48000.0;
    app.set_config(cfg);

    if (!app.start()) {
        // No usable audio device (headless CI without an output device). The
        // probe is only prepare()d inside start(); skip honestly rather than
        // assert on an un-prepared probe.
        SKIP("no audio device available; standalone wiring check cannot run");
        return;
    }

    // start() opens AND starts a real output device, whose callback runs
    // output_probe_.analyze_output() on the audio thread. Stop it BEFORE we feed
    // the probe manually below — otherwise the live (silent) callback is a
    // second producer racing our tone snapshot, and the window could poll() a
    // silence frame (peak == 0). stop() halts the device but leaves the probe
    // prepared (it is a StandaloneApp member, not torn down here), so the
    // wiring reference stays valid.
    auto& probe = app.output_probe();
    app.stop();
    REQUIRE(probe.capture_enabled());

    // The window consumes the SAME reference the host hands it in
    // run_with_editor(): app.output_probe(). No host factory → no native
    // surface, but the probe → panel data path is fully exercised.
    AudioInspectorWindow window;
    window.set_probe(&probe);
    REQUIRE(window.probe() == &probe);

    // Feed the probe the way the audio callback would (analyze_output on the
    // output boundary) with a 440 Hz tone, then poll: the window must read Live.
    // (No "idle before feed" assertion here: the now-stopped device may have
    // already published silent snapshots, so the first poll would legitimately
    // read Live-but-silent. The idle / no-probe states are covered by the
    // window-only tests in test_audio_inspector_window.cpp.)
    probe.reset();  // clear any snapshot the live device published before stop()
    StereoBlock block(static_cast<std::size_t>(cfg.buffer_size));
    for (std::size_t i = 0; i < block.left.size(); ++i) {
        const float s = std::sin(2.0f * 3.14159265f * 440.0f *
                                 static_cast<float>(i) / 48000.0f);
        block.left[i] = 0.6f * s;
        block.right[i] = 0.45f * s;
    }
    probe.analyze_output(block.view());

    window.poll();
    REQUIRE(window.panel().status() == AudioInspectorPanel::Status::kLive);
    REQUIRE(window.panel().observed_stage() ==
            AudioProbeStage::kStandaloneOutputBoundary);
    REQUIRE(window.panel().peak() > 0.0f);
    REQUIRE(window.panel().rms() > 0.0f);
    REQUIRE(window.panel().waveform().sample_count() > 0);

    // The panel surfaces the live readout as visible text (not a blank panel):
    // the status reads "live", the level carries a "dBFS" readout, and the
    // heading color is a valid [0,1] Color (the white-on-white bug fed 0-255
    // into the [0,1] ctor, leaving every channel >1 and clamped to white).
    REQUIRE(window.panel().status_text().find("live") != std::string::npos);
    REQUIRE(window.panel().level_text().find("dBFS") != std::string::npos);
    REQUIRE(window.panel().status_color().r <= 1.0f);
    REQUIRE(window.panel().status_color().g <= 1.0f);
    REQUIRE(window.panel().status_color().b <= 1.0f);
}

TEST_CASE("Standalone-style capture ring lets the inspector show a live waveform",
          "[format][standalone][audio-inspector][audio-harness]") {
    // run_with_editor() enables a capture ring sized to the panel's display
    // capacity when PULP_AUDIO_INSPECTOR is set, so the inspector paints a
    // waveform and not just meters. Reproduce that prepare() shape directly
    // (start() leaves capture off by default) and assert the window copies a
    // non-empty trace off the probe.
    AudioProbe probe;
    AudioProbe::CaptureConfig cap;
    cap.capture_frames = AudioWaveformView::kCapacity;
    probe.prepare(2, 128, 48000.0,
                  AudioProbeStage::kStandaloneOutputBoundary, cap);

    StereoBlock block(128);
    for (std::size_t i = 0; i < block.left.size(); ++i) {
        const float s = std::sin(2.0f * 3.14159265f * 440.0f *
                                 static_cast<float>(i) / 48000.0f);
        block.left[i] = 0.6f * s;
        block.right[i] = 0.45f * s;
    }
    // Several blocks so the ring fills past one buffer of frames.
    for (int b = 0; b < 8; ++b) probe.analyze_output(block.view());

    AudioInspectorWindow window;
    window.set_probe(&probe);
    window.poll();

    REQUIRE(window.panel().status() == AudioInspectorPanel::Status::kLive);
    // The waveform the panel renders is genuinely non-empty — the standalone
    // capture ring is what makes the inspector's trace visible.
    REQUIRE(window.panel().waveform().sample_count() > 0);
}

#else  // PULP_ENABLE_AUDIO_PROBES

TEST_CASE("Standalone audio inspector wiring requires PULP_ENABLE_AUDIO_PROBES",
          "[format][standalone][audio-inspector][audio-harness]") {
    SUCCEED("audio probes disabled in this build — standalone wiring not present");
}

#endif  // PULP_ENABLE_AUDIO_PROBES
