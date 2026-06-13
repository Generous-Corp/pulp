// PulpTempoSampler standalone — runs the tempo-matching sampler as a desktop
// audio app with the GPU-hosted Ink & Signal editor. Optionally loads a loop
// from the command line; the loop is BPM-detected, sliced, and rendered to the
// host/built-in tempo on a background thread, then played back via MIDI input.
//
//   pulp-tempo-sampler [loop.wav]                        # open the live GPU editor
//   pulp-tempo-sampler [loop.wav] --screenshot=out.png   # headless GPU capture
//
// The --screenshot path drives the SDK's built-in capture: run_with_editor()
// opens the real GPU window, waits screenshot_frame_delay frames for the first
// layout/paint to settle, reads the live back buffer via
// WindowHost::capture_png(), writes the PNG, and exits. This is the LIVE GPU
// host proof; it briefly shows a real window because the macOS GPU host's
// frame pump (CVDisplayLink) only ticks for an on-screen window. For a fully
// headless GPU capture with no window, use the offscreen tool
// `pulp-tempo-sampler-shot --gpu` (render_to_png_gpu / HeadlessSurface).
// `--headless` here forces the hidden path (best-effort; an accessory app may
// not receive vsync, so prefer the offscreen tool when you need a guarantee).

#include "pulp_tempo_sampler.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ProcessorFactory is a plain function pointer (not std::function), so the
// factory must be captureless. Stash the loop path here for it to read.
std::string g_loop_path;

// Load a loop file into a freshly-created processor. Called from the factory so
// the editor's create_view() sees a populated waveform on the first painted
// frame — which is the frame the --screenshot path captures.
void load_loop_into(pulp::examples::PulpTempoSamplerProcessor& ts,
                    const std::string& path) {
    auto data = pulp::audio::FormatRegistry::instance().read(path);
    if (!data || data->empty()) {
        pulp::runtime::log_error("PulpTempoSampler: could not read loop file '{}'", path);
        return;
    }
    std::vector<const float*> ch(data->num_channels());
    for (std::uint32_t c = 0; c < data->num_channels(); ++c)
        ch[c] = data->channels[c].data();
    ts.load_loop(ch.data(), static_cast<int>(data->num_channels()),
                 static_cast<long>(data->num_frames()), data->sample_rate);
    pulp::runtime::log_info("PulpTempoSampler: loaded loop, detecting tempo + slices");
}

} // namespace

int main(int argc, char** argv) {
    using namespace pulp;
    runtime::log_info("PulpTempoSampler Standalone v1.0.0");

    std::string loop_path;
    std::string screenshot_path;
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        constexpr std::string_view shot = "--screenshot=";
        if (arg.rfind(shot, 0) == 0) {
            screenshot_path = arg.substr(shot.size());
        } else if (arg == "--headless") {
            headless = true;  // best-effort hidden capture (see file header)
        } else if (!arg.empty() && arg[0] != '-') {
            loop_path = arg;
        }
    }
    g_loop_path = loop_path;

    // Load the loop inside the factory so the editor renders the waveform on
    // its first painted frame (run_with_editor builds the editor via the
    // processor the factory returns). Captureless lambda → ProcessorFactory.
    format::StandaloneApp app([]() -> std::unique_ptr<format::Processor> {
        auto p = examples::create_pulp_tempo_sampler();
        if (!g_loop_path.empty()) {
            if (auto* ts = dynamic_cast<examples::PulpTempoSamplerProcessor*>(p.get()))
                load_loop_into(*ts, g_loop_path);
        }
        return p;
    });

    format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 0;
    config.persist_settings = false;
    config.headless = headless;
    config.screenshot_path = screenshot_path;
    app.set_config(config);

    if (!app.run_with_editor(/*use_gpu=*/true)) {
        runtime::log_error("PulpTempoSampler: failed to run GPU editor");
        return 1;
    }
    return 0;
}
