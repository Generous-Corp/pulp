// Bendr standalone host.
//
// Standard Pulp standalone entry — identical in shape to the SDK examples
// (examples/audio-inspector-demo/main.cpp etc.): name the processor factory,
// set a StandaloneConfig, call run_with_editor(). Everything else — the window,
// the editor, and the Audio + MIDI device Settings tabs (output device, sample
// rate, buffer size, MIDI input source; persisted across launches) — is
// provided by the SDK's Pulp::standalone. There is NO Bendr-specific settings
// UI here.
//
// The settings tabs are STANDALONE-ONLY chrome: the AU / VST3 / CLAP plugin
// builds reach the same Bendr editor through Processor::create_view() and never
// show them, because in a DAW the host owns audio + MIDI routing.
#include "src/reference_processor.hpp"
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

int main() {
    pulp::format::StandaloneApp app(bendr::create_reference);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.input_channels = 2;     // Bendr is an effect — process live input
    config.output_channels = 2;
    config.supports_audio_input = true;
    config.show_settings_tab = true;   // SDK Audio + MIDI device tabs (default on)
    config.persist_settings = true;    // remember device / rate / buffer / MIDI in
    app.set_config(config);

    // Opens the Bendr editor + gear → Audio/MIDI settings, runs the event loop
    // until the window closes. GPU host for the Dawn/Skia editor.
    if (!app.run_with_editor(/*use_gpu=*/true)) {
        pulp::runtime::log_error("Bendr: failed to start standalone app");
        return 1;
    }
    return 0;
}
