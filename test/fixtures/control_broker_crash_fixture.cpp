#include "control_broker_daemon.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace {
std::atomic<bool> stopping{false};

void request_stop(int) {
    stopping.store(true, std::memory_order_relaxed);
}

std::filesystem::path environment_path(const char* name) {
    const auto* value = std::getenv(name);
    return value ? std::filesystem::path{value} : std::filesystem::path{};
}

std::string environment_string(const char* name) {
    const auto* value = std::getenv(name);
    return value ? std::string{value} : std::string{};
}
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    std::error_code executable_error;
    const auto executable = argc > 0
                                ? std::filesystem::weakly_canonical(argv[0], executable_error)
                                : std::filesystem::path{};
    if (executable_error || executable.empty())
        return 2;
    const pulp::inspect::ControlTrustedHostLaunchIntent trusted_host{
        .executable = environment_path("PULP_CONTROL_TEST_TRUSTED_HOST_EXECUTABLE"),
        .arguments = {
            environment_string("PULP_CONTROL_TEST_TRUSTED_HOST_REGISTRATION"),
            environment_string("PULP_CONTROL_TEST_TRUSTED_HOST_STOP"),
            environment_string("PULP_CONTROL_TEST_TRUSTED_HOST_DEFERRED"),
        },
        .working_directory =
            environment_path("PULP_CONTROL_TEST_TRUSTED_HOST_WORKING_DIRECTORY"),
        .host_tier = pulp::inspect::ControlHostTier::Standalone,
    };
    pulp::inspect::ControlBrokerDaemon daemon({
        .runtime_root = environment_path("PULP_CONTROL_BROKER_RUNTIME_ROOT"),
        .state_root = environment_path("PULP_CONTROL_BROKER_STATE_ROOT"),
        .sdk_version = "phase15-crash-fixture",
        .executable_path = executable,
        .trusted_host_allowlist = {trusted_host},
        .decide_consent =
            [](const pulp::inspect::VerifiedControlPeerIdentity&,
               const pulp::inspect::ControlGrantRequest&) {
                return pulp::inspect::ControlConsentDecision{
                    true, pulp::inspect::ControlConsentAuthority::TrustedHostUi,
                    "phase15-crash-fixture-consent"};
            },
    });
    if (!daemon.start())
        return 1;
    while (!stopping.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    daemon.stop();
    return 0;
}
