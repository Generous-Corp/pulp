#pragma once
// Hot-reload MORPH demo — the SHELL the host loads. A ReloadableShell pointed at
// the morph logic; reloading swaps both the DSP and (via create_view forwarding)
// the editor. Logic path: PULP_RELOAD_LOGIC_PATH, else the install convention.
#include <pulp/format/reload/reloadable_shell.hpp>
#include <cstdlib>
#include <memory>
#include <string>
namespace pulp::examples {
inline std::string morph_logic_path() {
    if (const char* env = std::getenv("PULP_RELOAD_LOGIC_PATH"); env && *env) return std::string(env);
    const char* home = std::getenv("HOME");
    return std::string(home && *home ? home : ".") + "/.pulp/hot-reload-morph/logic.dylib";
}
inline std::unique_ptr<format::Processor> create_morph_shell() {
    return std::make_unique<format::reload::ReloadableShell>(morph_logic_path());
}
}  // namespace pulp::examples
