#include <pulp/format/detail/standalone_updater.hpp>

// Sparkle is macOS-only; other platforms never offer an in-app updater.
namespace pulp::format::detail {

StandaloneUpdaterEnvironment probe_standalone_updater_environment(bool headless) {
    StandaloneUpdaterEnvironment env;
    env.headless = headless;
    return env;
}

void start_standalone_updater() {}

void check_for_standalone_updates() {}

std::shared_ptr<AppUpdateService>
make_standalone_update_service(const StandaloneUpdaterPlan& plan) {
    // Only the development stub exists off macOS; it never contacts a feed.
    if (plan.backend == StandaloneUpdaterBackend::stub)
        return make_stub_update_service("Pulp", {});
    return nullptr;
}

} // namespace pulp::format::detail
