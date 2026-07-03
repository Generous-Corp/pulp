#pragma once

// Hot-Reload SYNTH — the SHELL half (what the host loads as an INSTRUMENT).
//
// A thin pulp::format::reload::ReloadableShell that adopts the loaded synth
// logic's descriptor — since logic_synth.cpp reports PluginCategory::Instrument
// + accepts_midi, the shell presents as an instrument (aumu in Logic). It watches
// the logic .dylib and hot-swaps the synth DSP in live on recompile.
//
// Logic path: PULP_RELOAD_LOGIC_PATH if set, else the install convention
// "$HOME/.pulp/hot-reload-synth/logic.dylib" that the build writes and
// rebuild_logic.sh refreshes.

#include <pulp/format/reload/reloadable_shell.hpp>

#include <cstdlib>
#include <memory>
#include <string>

namespace pulp::examples {

inline std::string hot_reload_synth_logic_path() {
    if (const char* env = std::getenv("PULP_RELOAD_LOGIC_PATH"); env && *env)
        return std::string(env);
    const char* home = std::getenv("HOME");
    const std::string base = home && *home ? std::string(home) : std::string(".");
    return base + "/.pulp/hot-reload-synth/logic.dylib";
}

inline std::unique_ptr<format::Processor> create_hot_reload_synth() {
    return std::make_unique<format::reload::ReloadableShell>(hot_reload_synth_logic_path());
}

}  // namespace pulp::examples
