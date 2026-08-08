#include "control_broker_daemon.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
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

    pulp::inspect::ControlBrokerDaemon daemon({
        .sdk_version = PULP_CONTROL_SDK_VERSION,
        .executable_path = executable_path(),
    });
    if (!daemon.start())
        return 1;
    while (!stopping.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    daemon.stop();
    return 0;
}
