// Standalone host for the latency probe (CPU host — this branch configures GPU
// off). Not the focus of P0.5 (REAPER is), but keeps format parity with the
// other examples and gives a quick non-DAW smoke.
#include "src/latency_probe.hpp"
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

int main() {
    pulp::format::StandaloneApp app(probe::create_latency_probe);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.input_channels = 2;
    config.output_channels = 2;
    config.supports_audio_input = true;
    app.set_config(config);

    if (!app.run_with_editor(/*use_gpu=*/false)) {
        pulp::runtime::log_error("LatencyProbe: failed to start standalone app");
        return 1;
    }
    return 0;
}
