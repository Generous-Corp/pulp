#pragma once

// Hot-Reload Demo — the SHELL half of the plugin.
//
// This is what the host (REAPER, Logic, the standalone app) actually loads. It
// is a thin pulp::format::reload::ReloadableShell: it owns the audio entry, the
// parameter store, and the RT-safe hot-swap slot, and it watches a logic .dylib
// (the DSP you edit — see logic_tremolo.cpp) and swaps it in live on recompile.
//
// The logic library path: PULP_RELOAD_LOGIC_PATH if set, else the install
// convention "$HOME/.pulp/hot-reload-demo/logic.dylib" that the build writes and
// rebuild_logic.sh refreshes. Resolving it here (not leaving it empty) means the
// plugin works the moment the host loads it, with no environment setup.

#include <pulp/format/reload/reloadable_shell.hpp>

#include <cstdlib>
#include <memory>
#include <string>

namespace pulp::examples {

inline std::string hot_reload_demo_logic_path() {
    if (const char* env = std::getenv("PULP_RELOAD_LOGIC_PATH"); env && *env)
        return std::string(env);
    const char* home = std::getenv("HOME");
    const std::string base = home && *home ? std::string(home) : std::string(".");
    return base + "/.pulp/hot-reload-demo/logic.dylib";
}

inline std::unique_ptr<format::Processor> create_hot_reload_shell() {
    return std::make_unique<format::reload::ReloadableShell>(hot_reload_demo_logic_path());
}

}  // namespace pulp::examples
