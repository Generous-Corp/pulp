#include "control_broker_daemon.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#ifndef PULP_CONTROL_SDK_VERSION
#define PULP_CONTROL_SDK_VERSION "unknown"
#endif

namespace {

std::atomic<bool> stopping{false};

void request_stop(int) {
    stopping.store(true, std::memory_order_relaxed);
}

} // namespace

int main() {
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    pulp::inspect::ControlBrokerDaemon daemon({
        .sdk_version = PULP_CONTROL_SDK_VERSION,
    });
    if (!daemon.start())
        return 1;
    while (!stopping.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    daemon.stop();
    return 0;
}
