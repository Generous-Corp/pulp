// PulpDesignPanel standalone.
//
// `--screenshot <png>` renders the editor and exits WITHOUT opening an audio
// device or a window. That distinction matters: a screenshot mode that starts
// the audio engine makes every headless verification run audible on whatever
// machine happens to execute it, and a CI box or an SSH session is exactly
// where nobody is expecting sound.

#include "design_panel_plugin.hpp"

#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/view/screenshot.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::atomic<bool> should_quit{false};
void signal_handler(int) { should_quit.store(true); }

int render_editor(const std::string& out, float width, float height,
                  float scale) {
    auto processor = pulp::examples::create_design_panel();
    auto editor = processor->create_view();
    if (editor == nullptr) {
        std::cerr << "create_view() returned no editor — the plugin carries no "
                     "usable design\n";
        return 1;
    }
    editor->set_bounds({0.0f, 0.0f, width, height});
    if (!pulp::view::render_to_file(
            *editor, static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height), out, scale,
            pulp::view::ScreenshotBackend::skia)) {
        std::cerr << "render failed\n";
        return 1;
    }
    std::cout << "editor rendered to " << out << "\n";
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    std::string screenshot;
    float width = 900.0f, height = 602.0f, scale = 2.0f;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--screenshot" && has_value) screenshot = argv[++i];
        else if (arg == "--width" && has_value) width = std::strtof(argv[++i], nullptr);
        else if (arg == "--height" && has_value) height = std::strtof(argv[++i], nullptr);
        else if (arg == "--scale" && has_value) scale = std::strtof(argv[++i], nullptr);
        else {
            std::cerr << "usage: PulpDesignPanel [--screenshot <png> "
                         "[--width W] [--height H] [--scale S]]\n";
            return 2;
        }
    }

    // Before any audio device is opened.
    if (!screenshot.empty())
        return render_editor(screenshot, width, height, scale);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    pulp::format::StandaloneApp app(pulp::examples::create_design_panel);
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 2;
    app.set_config(config);

    if (!app.start()) {
        pulp::runtime::log_error("Failed to start standalone app");
        return 1;
    }
    std::cout << "PulpDesignPanel is running. Press Ctrl+C to quit.\n";
    while (!should_quit.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    app.stop();
    return 0;
}
