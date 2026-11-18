#include <pulp/format/detail/standalone_settings_window.hpp>

#include <pulp/runtime/log.hpp>

namespace pulp::format::detail {

bool standalone_settings_window_supported() {
    return false;
}

bool open_standalone_settings_window(StandaloneApp&) {
    runtime::log_warn("Standalone: this platform has no native Audio/MIDI settings window yet");
    return false;
}

} // namespace pulp::format::detail
