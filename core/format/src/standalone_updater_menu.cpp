#include <pulp/format/detail/standalone_updater_menu.hpp>

#include <utility>

namespace pulp::format::detail {

void add_standalone_updater_menu_command(view::WindowOptions& options,
                                         const StandaloneUpdaterPlan& plan) {
    if (!plan.offer_menu_command)
        return;
    // Directly under "About <App>", as macOS apps place it. The command goes
    // through the AppUpdateService so the menu, the Settings controls and a
    // JS editor all drive the same backend.
    view::WindowOptions::MenuCommand command{
        .menu = {},
        .title = "Check for Updates\xE2\x80\xA6",
        .action = [] { check_for_app_updates(); },
        .app_menu_section = view::WindowOptions::MenuCommand::AppMenuSection::after_about,
    };
    options.menu_commands.push_back(std::move(command));
}

} // namespace pulp::format::detail
