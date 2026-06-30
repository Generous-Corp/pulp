// Knob Playground standalone host. `--screenshot PATH` runs headless and
// captures the first painted frame (verification path, from triaz-demo).
#include "src/processor.hpp"
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>
#include <string>

int main(int argc, char** argv) {
    pulp::format::StandaloneApp app(knobpg::create_knob_processor);

    // Match triaz-demo's known-stable config: no settings-tab chrome, no audio
    // input (a render-loop crash in nextDrawable showed up with the tab chrome +
    // audio-input path on this minimal app; triaz omits both and is stable).
    pulp::format::StandaloneConfig config;
    config.output_channels = 2;
    config.input_channels = 0;
    config.show_settings_tab = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) {
            config.headless = true;
            config.screenshot_path = argv[++i];
        } else if (a.rfind("--screenshot=", 0) == 0) {
            config.headless = true;
            config.screenshot_path = a.substr(std::string("--screenshot=").size());
        }
    }
    app.set_config(config);

    if (!app.run_with_editor(/*use_gpu=*/true)) {
        pulp::runtime::log_error("KnobPlayground: failed to start standalone app");
        return 1;
    }
    return 0;
}
