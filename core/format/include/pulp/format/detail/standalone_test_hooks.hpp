#pragma once

// Test-only controls for StandaloneApp's editor-host contract window, kept out
// of the public StandaloneConfig. A test sets them with
// StandaloneApp::set_test_hooks(); an app never does, and with none set the
// window behaves as shipped.

namespace pulp::format::detail {

struct StandaloneTestHooks {
    /// Open the plug-in's settings editor (EditorRole::settings) at this loop
    /// frame, as its menu item would. -1: never.
    int open_plugin_settings_frame = -1;
};

} // namespace pulp::format::detail
