#pragma once

// Test-only controls for StandaloneApp's editor-host contract window, kept out
// of the public StandaloneConfig. A test sets them with
// StandaloneApp::set_test_hooks(); an app never does, and with none set the
// window behaves as shipped.

#include <pulp/format/detail/standalone_contract_editor.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace pulp::format {
class AppUpdateService;
}

namespace pulp::format::detail {

struct StandaloneTestHooks {
    /// Open the plug-in's settings editor (EditorRole::settings) at this loop
    /// frame, as its menu item would. -1: never.
    int open_plugin_settings_frame = -1;
    /// Install this service in place of the one the updater plan picks, and
    /// offer "Check for Updates…" for it, headless runs included. The window
    /// removes it again when it closes. Null: the plan decides, as shipped.
    std::shared_ptr<AppUpdateService> update_service;
    /// Called once with the window's app-menu items, before the loop, headless
    /// runs included (a headless window installs no menu).
    std::function<void(const std::vector<StandaloneContractMenuItem>&)> on_app_menu;
};

} // namespace pulp::format::detail
