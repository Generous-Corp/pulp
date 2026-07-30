// Forge Modular standalone host.
//
// The same entry every Forge product uses: name the processor factory, set a
// StandaloneConfig, call run_with_editor(). The window and the editor come from
// the SDK; the editor itself is Forge's chrome, which ForgeModularShell varies
// through chrome_copy(), composer_row() and home_accessory() rather than by
// reimplementing anything.
//
// An effect, not an instrument: it passes signal through untouched so it can sit
// on the same track as a Rack Pro instance rather than taking a track of its own.
//
//   pulp-forge-modular                 # windowed editor
//   pulp-forge-modular --screenshot P  # headless: capture the first editor frame
#include "forge/brand.hpp"
#include "forge/modular_shell.hpp"

#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <memory>
#include <string>

namespace {

std::unique_ptr<pulp::format::Processor> create_forge_modular() {
    return std::make_unique<forge_modular::ForgeModularShell>();
}

}  // namespace

int main(int argc, char** argv) {
    std::string screenshot_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--screenshot" && i + 1 < argc) screenshot_path = argv[++i];
    }

    pulp::format::StandaloneApp app(create_forge_modular);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.input_channels = 2;    // effect — signal passes through untouched
    config.output_channels = 2;
    config.supports_audio_input = true;
    config.show_settings_tab = true;
    config.persist_settings = false;
    config.headless = !screenshot_path.empty();
    config.screenshot_path = screenshot_path;
    app.set_config(config);

    if (!app.run_with_editor(/*use_gpu=*/true)) {
        pulp::runtime::log_error("Forge Modular: failed to start standalone app");
        return 1;
    }
    return 0;
}
