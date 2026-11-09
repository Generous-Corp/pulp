// The visible Standalone app. The broker launches the separate control host
// that pulp_add_plugin builds from PROCESSOR_FACTORY.

#include "agent_toolkit_synth.hpp"

#include <pulp/format/standalone.hpp>

int main() {
    pulp::format::StandaloneApp app(&create_agent_toolkit_synth);
    pulp::format::StandaloneConfig config;
    config.input_channels = 0;
    config.output_channels = 2;
    app.set_config(config);
    return app.run_with_editor(false) ? 0 : 1;
}
