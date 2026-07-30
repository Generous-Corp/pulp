#include "pulp_delay.hpp"

#include <pulp/format/standalone.hpp>

int main() {
    pulp::format::StandaloneApp app(pulp::examples::delay::create_pulp_delay);
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.input_channels = 2;
    config.output_channels = 2;
    app.set_config(config);
    return app.run_with_editor(true) ? 0 : 1;
}
