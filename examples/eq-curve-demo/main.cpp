// EqCurveDemo standalone — opens a window with the interactive EqCurveView
// editor and runs the four-band EQ. Drag the curve to shape the sound; reach
// the built-in test signal / output device via the editor's Settings button.
//
//   pulp-eq-curve-demo                 # windowed editor
//   pulp-eq-curve-demo --screenshot P  # headless: capture first frame to P

#include "eq_curve_demo.hpp"

#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <string>

int main(int argc, char** argv) {
    std::string screenshot_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--screenshot" && i + 1 < argc) screenshot_path = argv[++i];
    }

    pulp::runtime::log_info("EQ Curve Demo standalone");

    pulp::format::StandaloneApp app(pulp::examples::create_eq_curve_demo);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 0;   // no mic; hear it via the Settings test signal
    config.persist_settings = false;
    config.show_settings_tab = true;
    config.headless = !screenshot_path.empty();
    config.screenshot_path = screenshot_path;
    app.set_config(config);

    // CPU editor: this build has no GPU host, and the curve is 2D vector
    // drawing that the CoreGraphics canvas renders directly.
    if (!app.run_with_editor(/*use_gpu=*/false)) {
        pulp::runtime::log_error("EQ Curve Demo: failed to run editor");
        return 1;
    }
    return 0;
}
