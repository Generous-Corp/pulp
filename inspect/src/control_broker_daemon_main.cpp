#include "control_broker_daemon.hpp"

#include <pulp/inspect/control_consent_authority.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include <mach-o/dyld.h>

#ifndef PULP_CONTROL_SDK_VERSION
#define PULP_CONTROL_SDK_VERSION "unknown"
#endif

extern "C" __attribute__((used, visibility("default"))) const volatile char
    pulp_control_broker_version_query_v1[] = "PULP_CONTROL_BROKER_VERSION_QUERY_V1";

namespace {

std::atomic<bool> stopping{false};

void request_stop(int) {
    stopping.store(true, std::memory_order_relaxed);
}

std::filesystem::path executable_path() {
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    std::error_code error;
    const auto path = std::filesystem::weakly_canonical(buffer.data(), error);
    return error ? std::filesystem::path{} : path;
}

std::filesystem::path environment_path(const char* name) {
    const auto* value = std::getenv(name);
    return value != nullptr ? std::filesystem::path{value} : std::filesystem::path{};
}

std::vector<pulp::inspect::ControlInstalledHostSelection>
installed_host_selections(const std::filesystem::path& broker) {
    if (broker.empty())
        return {};
    const auto host = broker.parent_path() / "pulp-control-standalone-host";
    const auto manifest = std::filesystem::path{
        host.string() + ".inspector-capabilities.json"};
    std::error_code error;
    if (!std::filesystem::is_regular_file(host, error) || error ||
        !std::filesystem::is_regular_file(manifest, error) || error)
        return {};
    return {{.host_id = "ordinary-standalone",
             .intent = {.executable = host,
                        .arguments = {},
                        .working_directory = host.parent_path(),
                        .host_tier = pulp::inspect::ControlHostTier::Standalone}}};
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << PULP_CONTROL_SDK_VERSION << '\n';
        return 0;
    }
    if (argc != 1)
        return 2;

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    auto consent_authority = std::make_shared<pulp::inspect::ControlBrokerConsentAuthority>();
    const auto broker = executable_path();
    pulp::inspect::ControlBrokerDaemon daemon({
        .runtime_root = environment_path("PULP_CONTROL_BROKER_RUNTIME_ROOT"),
        .state_root = environment_path("PULP_CONTROL_BROKER_STATE_ROOT"),
        .sdk_version = PULP_CONTROL_SDK_VERSION,
        .executable_path = broker,
        .installed_host_selections = installed_host_selections(broker),
        .decide_consent = [consent_authority](const auto& request) {
            return consent_authority->decide(request);
        },
    });
    if (!daemon.start())
        return 1;
    while (!stopping.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    consent_authority->reset();
    daemon.stop();
    return 0;
}
