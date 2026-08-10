// Forge Modular standalone.
//
// The same shell the plugins load, in its own window. Standalone is the more
// capable of the two contexts, and deliberately so: from here Rack can be
// launched and handed a patch file, which a plugin inside a DAW can never do
// because no plugin may instantiate another or tell its host to open a file.
//
//   forge-modular                    # windowed
//   forge-modular --screenshot P     # headless: first editor frame to P
//
// The screenshot path exists so the shell can be proven to render without a
// human watching a window, which is how it gets checked in CI and how a
// regression in the UI script is caught before anyone opens the app.

#include "forge_modular/shell.hpp"

#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <string>

int main(int argc, char** argv) {
    std::string screenshot_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--screenshot" && i + 1 < argc) screenshot_path = argv[++i];
    }

    pulp::format::StandaloneApp app(forge_modular::create_shell);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    // It passes audio through rather than generating any, so it opens an input
    // for the same reason the plugin is an effect: this belongs on an insert.
    config.input_channels = 2;
    config.output_channels = 2;
    config.supports_audio_input = true;
    config.show_settings_tab = true;
    config.persist_settings = true;
    config.headless = !screenshot_path.empty();
    config.screenshot_path = screenshot_path;
    app.set_config(config);

    if (!app.run_with_editor(/*use_gpu=*/true)) {
        pulp::runtime::log_error("Forge Modular: failed to start");
        return 1;
    }
    return 0;
}
