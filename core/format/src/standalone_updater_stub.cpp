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

} // namespace pulp::format::detail
