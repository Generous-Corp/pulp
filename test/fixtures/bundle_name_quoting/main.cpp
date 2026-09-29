#include "tail_edge_probe.hpp"

#include <pulp/format/standalone.hpp>

int main() {
    pulp::format::StandaloneApp app(&pulp::test_fixtures::create_tail_edge_probe);
    pulp::format::StandaloneConfig config;
    config.input_channels = 2;
    config.output_channels = 2;
    config.persist_settings = false;
    app.set_config(config);
    return app.run_with_editor(false) ? 0 : 1;
}
