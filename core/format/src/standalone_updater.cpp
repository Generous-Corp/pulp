#include <pulp/format/detail/standalone_updater.hpp>

#include <pulp/runtime/log.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <iterator>
#include <utility>

namespace pulp::format::detail {

namespace {

std::string lowered(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

class StubUpdateService final : public AppUpdateService {
  public:
    StubUpdateService(std::string app_name, std::string version)
        : app_name_(std::move(app_name)), version_(std::move(version)) {}

    AppUpdateStatus status() const override {
        AppUpdateStatus status;
        status.available = true;
        status.can_check_now = true;
        status.stub = true;
        status.automatic_checks = automatic_.load();
        status.app_name = app_name_;
        status.version = version_;
        status.last_check_unix_seconds = last_check_.load();
        return status;
    }
    bool check_for_updates() override {
        last_check_.store(static_cast<std::int64_t>(std::time(nullptr)));
        runtime::log_info("Standalone updater (stub): update check requested; "
                          "this build has no update feed");
        return true;
    }
    bool set_automatic_checks(bool on) override {
        automatic_.store(on);
        return true;
    }

  private:
    std::string app_name_;
    std::string version_;
    std::atomic<bool> automatic_{false};
    std::atomic<std::int64_t> last_check_{0};
};

} // namespace

StandaloneUpdaterPlan plan_standalone_updater(const StandaloneUpdaterEnvironment& env) {
    StandaloneUpdaterPlan plan;
    if (env.headless)
        return plan;
    const auto override_value = lowered(env.override_value);
    if (override_value == "0" || override_value == "off" || override_value == "false")
        return plan;
    if (override_value == "stub") {
        plan.offer_menu_command = true;
        plan.backend = StandaloneUpdaterBackend::stub;
        return plan;
    }
    if (!env.bundle_declares_feed || !env.updater_framework_loaded)
        return plan;
    plan.offer_menu_command = true;
    plan.backend = StandaloneUpdaterBackend::sparkle;
    const bool forced_on =
        override_value == "1" || override_value == "on" || override_value == "true";
    plan.start_at_launch = forced_on || env.developer_id_signed;
    return plan;
}

std::shared_ptr<AppUpdateService> make_stub_update_service(std::string app_name,
                                                           std::string version) {
    return std::make_shared<StubUpdateService>(std::move(app_name), std::move(version));
}

void add_standalone_updater_menu_command(view::WindowOptions& options,
                                         const StandaloneUpdaterPlan& plan) {
    if (!plan.offer_menu_command) return;
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
