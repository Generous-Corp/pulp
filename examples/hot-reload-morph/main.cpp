// Hot-Reload Morph — standalone app. Runs the morph shell with its editor + audio.
// The DSP hot-reloads live (the watcher swaps it on recompile); the editor
// reflects the active version. Flip versions with morph.sh. (Audio plays out the
// default device.)
#include "morph_shell.hpp"
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>
int main() {
    pulp::format::StandaloneApp app(pulp::examples::create_morph_shell);
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0; config.buffer_size = 256;
    config.input_channels = 2; config.output_channels = 2;
    app.set_config(config);
    pulp::runtime::log_info("Hot-Reload Morph — logic: {}", pulp::examples::morph_logic_path());
    return app.run_with_editor(/*use_gpu=*/true) ? 0 : 1;
}
