// Hot-Reload Demo — AU v2 entry point (aufx effect).
// The shell is a plain Processor factory, so the AU entry mirrors the CLAP/VST3
// ones. Class name HotReloadDemoAU → AUSDK factory HotReloadDemoAUFactory, which
// matches the ${target}AUFactory name pulp_add_plugin wires into Info.plist.au.
#include "hot_reload_shell.hpp"
#include <pulp/format/au_v2_entry.hpp>

PULP_AU_PLUGIN(HotReloadDemoAU, pulp::examples::create_hot_reload_shell)
