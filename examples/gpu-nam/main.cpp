// GPU NAM standalone host.
//
// Standard Pulp standalone entry: name the processor factory, set a
// StandaloneConfig, call run_with_editor(). The window, the native GPU editor
// (Processor::create_view()), and the Audio + MIDI device Settings tabs are all
// provided by the SDK's Pulp::standalone. The AU / VST3 / CLAP builds reach the
// same editor through create_view() and never show the settings chrome, because
// in a DAW the host owns audio routing.
#include "gpu_nam_processor.hpp"
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

int main() {
    pulp::format::StandaloneApp app(pulp::examples::create_gpu_nam);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.input_channels = 2;     // GPU NAM is an effect — process live input
    config.output_channels = 2;
    config.supports_audio_input = true;
    config.show_settings_tab = true;
    config.persist_settings = true;
    app.set_config(config);

    if (!app.run_with_editor(/*use_gpu=*/true)) {
        pulp::runtime::log_error("GPU NAM: failed to start standalone app");
        return 1;
    }
    return 0;
}
