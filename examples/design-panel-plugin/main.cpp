// PulpDesignPanel standalone.
//
// `--screenshot <png>` uses StandaloneApp's own headless capture: it drives the
// real WindowHost and paints through the same GPU/Skia path a visible run does,
// then writes the PNG and exits. That launch creates NO audio system and NO
// audio device, which is what makes it safe to run anywhere — a screenshot mode
// that opened one would make every headless check audible on whatever machine
// happened to execute it.
//
// Rendering the view tree offscreen instead would prove the design materializes
// and say nothing about whether the app can put a window on screen; this goes
// through the window.

#include "design_panel_plugin.hpp"

#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 2;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--screenshot" && has_value) {
            config.screenshot_path = argv[++i];
            config.headless = true;
        } else if (arg == "--screenshot-frame-delay" && has_value) {
            config.screenshot_frame_delay = std::atoi(argv[++i]);
        } else {
            std::cerr << "usage: PulpDesignPanel [--screenshot <png>] "
                         "[--screenshot-frame-delay N]\n";
            return 2;
        }
    }

    pulp::format::StandaloneApp app(pulp::examples::create_design_panel);
    app.set_config(config);

    // run_with_editor() owns the window and the event loop. start() alone would
    // bring up audio with no editor at all, which is not a standalone app.
    if (!app.run_with_editor(/*use_gpu=*/true)) {
        pulp::runtime::log_error("Standalone: run_with_editor() failed");
        return 1;
    }
    if (!config.screenshot_path.empty())
        std::cout << "editor captured to " << config.screenshot_path << "\n";
    return 0;
}
