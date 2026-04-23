// PulpWebViewPlugin Standalone
// Runs the WebView editor without the standalone Editor/Settings chrome.

#include "webview_plugin.hpp"
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

int main() {
    pulp::runtime::log_info("PulpWebViewPlugin Standalone v1.0.0");

    pulp::format::StandaloneApp app(pulp::examples::create_pulp_webview_plugin);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 2;
    config.show_settings_tab = false;
    app.set_config(config);

    if (!app.run_with_editor(false)) {
        pulp::runtime::log_error("Failed to run standalone editor");
        return 1;
    }

    return 0;
}
