// standalone_view_layer_none.cpp: StandaloneApp without Pulp's view layer, for
// pulp::standalone-contract, the standalone of a UI package plug-in. Its editor
// is the package's, opened through the editor-host contract, so the app links
// no Pulp view, canvas or render code and carries no Dawn of its own; a UI
// package that draws with Dawn brings exactly one, its own. A processor
// without an editor factory has no editor here; the musical typing keyboard
// and the Audio Inspector, which are Pulp views, are not available.

#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

namespace pulp::format {

void StandaloneApp::attach_view_layer() {}

void StandaloneApp::prepare_view_input(const StandaloneConfig& config) {
    if (config.enable_musical_typing_keyboard)
        runtime::log_warn("Standalone: the musical typing keyboard is a Pulp view; this "
                          "standalone links no view layer, so it is not available");
}

void StandaloneApp::drain_view_input(midi::MidiBuffer&, int) noexcept {}

void StandaloneApp::shutdown_view_input() noexcept {}

int StandaloneApp::view_waveform_capacity() const noexcept {
    return 0;
}

bool StandaloneApp::run_view_editor(bool, const StandaloneConfig&) {
    runtime::log_error("Standalone: this standalone links no Pulp view layer (a UI package "
                       "plug-in); its processor offers no editor factory, so it has no editor");
    stop();
    return false;
}

} // namespace pulp::format
