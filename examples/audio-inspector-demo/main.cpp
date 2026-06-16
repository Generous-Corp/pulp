// Audio Inspector Demo
//
// A minimal run_with_editor app with continuous processor-generated audio.
// It gives the live Audio Inspector a deterministic signal to observe and lets
// `pulp run --audio-probe-json` / `--audio-inspector --screenshot` exercise the
// real standalone GUI path instead of a throwaway driver.

#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include "audio_inspector_demo_processor.hpp"

int main() {
    pulp::runtime::log_info("AudioInspectorDemo Standalone v1.0.0");

    pulp::format::StandaloneApp app(pulp::examples::create_audio_inspector_demo);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 0;
    config.persist_settings = false;
    app.set_config(config);

    if (!app.run_with_editor(false)) {
        pulp::runtime::log_error("AudioInspectorDemo failed to run");
        return 1;
    }

    return 0;
}
