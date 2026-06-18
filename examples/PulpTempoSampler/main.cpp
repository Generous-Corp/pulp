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

#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace {

// ProcessorFactory is a plain function pointer (not std::function), so the
// factory must be captureless. Stash the loop path here for it to read.
std::string g_loop_path;

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

    // Queue the command-line loop through the SAME deferred worker path a UI
    // drop uses (request_load_path): the background worker decodes + analyzes it
    // off the audio/UI threads. We must NOT decode synchronously here — the
    // factory runs inside StandaloneApp::start() BEFORE it binds the processor's
    // StateStore, so load_loop()'s analyze step (state().get_value(kOnsetSens))
    // would dereference a null store and crash before the window ever paints.
    // The worker only fires once prepare() → start_worker() runs, by which time
    // start() has bound the store; the waveform then populates within a few
    // frames (the --screenshot path waits screenshot_frame_delay frames for it).
    // Captureless lambda → ProcessorFactory.
    format::StandaloneApp app([]() -> std::unique_ptr<format::Processor> {
        auto p = examples::create_pulp_tempo_sampler();
        if (!g_loop_path.empty()) {
            if (auto* ts = dynamic_cast<examples::PulpTempoSamplerProcessor*>(p.get()))
                ts->request_load_path(g_loop_path);
        }
        return p;
    });

    format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 0;
    config.persist_settings = false;
    // Host the editor directly (no Settings-tab chrome) so it is the window root
    // — the host's global-key hook fires on it, which is what makes musical
    // typing work without the editor holding keyboard focus.
    config.show_settings_tab = false;
    config.headless = headless;
    config.screenshot_path = screenshot_path;
    app.set_config(config);

    if (!app.run_with_editor(/*use_gpu=*/true)) {
        runtime::log_error("PulpTempoSampler: failed to run GPU editor");
        return 1;
    }
    return 0;
}
