#include <pulp/format/detail/standalone_updater.hpp>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>

namespace pulp::format::detail {

namespace {

std::string lowered(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

} // namespace

StandaloneUpdaterPlan plan_standalone_updater(const StandaloneUpdaterEnvironment& env) {
    StandaloneUpdaterPlan plan;
    if (!env.bundle_declares_feed || !env.updater_framework_loaded || env.headless)
        return plan;
    const auto override_value = lowered(env.override_value);
    if (override_value == "0" || override_value == "off" || override_value == "false")
        return plan;
    plan.offer_menu_command = true;
    const bool forced_on =
        override_value == "1" || override_value == "on" || override_value == "true";
    plan.start_at_launch = forced_on || env.developer_id_signed;
    return plan;
}

void add_standalone_updater_menu_command(view::WindowOptions& options,
                                         const StandaloneUpdaterPlan& plan) {
    if (!plan.offer_menu_command) return;
    // App-menu commands render in vector order above Quit. macOS convention
    // puts "Check for Updates…" before "Settings…", so it goes first.
    view::WindowOptions::MenuCommand command{
        .menu = {},
        .title = "Check for Updates\xE2\x80\xA6",
        .action = [] { check_for_standalone_updates(); },
    };
    options.menu_commands.insert(options.menu_commands.begin(), std::move(command));
}

} // namespace pulp::format::detail
