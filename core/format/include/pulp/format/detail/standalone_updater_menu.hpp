#pragma once

// The view standalone's "Check for Updates…" menu command. The updater itself
// (standalone_updater.hpp) is view-free and runs in every standalone; only
// this WindowHost menu wiring needs Pulp's view layer. The contract window
// adds its own menu item (standalone_contract_editor.hpp).

#include <pulp/format/detail/standalone_updater.hpp>
#include <pulp/view/window_host.hpp>

namespace pulp::format::detail {

/// Add "Check for Updates…" to the application menu, directly under "About
/// <App>", when the plan offers it. The command calls check_for_app_updates().
void add_standalone_updater_menu_command(view::WindowOptions& options,
                                         const StandaloneUpdaterPlan& plan);

} // namespace pulp::format::detail
