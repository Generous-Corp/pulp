// Dev-environment gate fixture.
//
// Compiled twice as an object by test/cmake/format_reload_tests.cmake, once
// with PULP_DEV_BUILD=1 and once with =0, overriding the value pulp::platform
// exports. The dev_env_gate_symbol_check.cmake test reads both objects and
// asserts the PULP_DEV_HOT_RELOAD and PULP_RELOAD_LOGIC_PATH literals land only
// in the dev object: a release build has no environment-variable path to a
// script-watching editor or an environment-chosen logic dlopen.
#include <pulp/format/reload/reloadable_shell.hpp>
#include <pulp/format/view_bridge.hpp>

// Non-inline so the inline readers are emitted (or inlined) into this object.
bool pulp_dev_env_gate_fixture_hot_reload() {
    return pulp::format::dev_editor_hot_reload_enabled();
}

std::string pulp_dev_env_gate_fixture_logic_path() {
    pulp::format::reload::ReloadableShell shell; // default path resolution
    return shell.logic_path();
}
