// PulpHostBench Standalone — for smoke-loading the plugin outside any DAW.
//
// Useful as a sanity check: launch this and you should see a log file
// appear under `~/Library/Logs/PulpHostBench/` with a single
// `session_start` event and the host name "Standalone".

#include "host_bench.hpp"

#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> should_quit{false};
void signal_handler(int) { should_quit.store(true); }
}  // namespace

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    pulp::runtime::log_info("PulpHostBench Standalone v1.0.0");

    pulp::format::StandaloneApp app(pulp::examples::create_host_bench);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 2;
    app.set_config(config);

    if (!app.start()) {
        pulp::runtime::log_error("Failed to start standalone bench app");
        return 1;
    }

    std::cout << "\nPulpHostBench is running. The log file appears under\n"
              << "  ~/Library/Logs/PulpHostBench/  (macOS)\n"
              << "  %LOCALAPPDATA%/PulpHostBench/  (Windows)\n"
              << "  ${XDG_STATE_HOME}/pulp-host-bench/  (Linux)\n"
              << "Ctrl+C to stop.\n" << std::endl;

    while (!should_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    app.stop();
    pulp::runtime::log_info("PulpHostBench stopped");
    return 0;
}
