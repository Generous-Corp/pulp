// Standalone entry point for the Moonbase Activation example: opens a window with
// the interactive activation editor. Audio is gated on the license (silence until
// activated). Prefers the GPU/Skia editor for faithful text; falls back to CPU.
#include "moonbase_activation_plugin.hpp"

#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

int main()
{
    pulp::format::StandaloneApp app(moonbase_pulp::create_moonbase_activation_plugin);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 2;
    app.set_config(config);

    if (!app.start()) {
        pulp::runtime::log_error("[moonbase] failed to start standalone app");
        return 1;
    }
    if (!app.run_with_editor(/*use_gpu=*/true))
        app.run_with_editor(/*use_gpu=*/false);
    app.stop();
    return 0;
}
