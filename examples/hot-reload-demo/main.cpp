// Hot-Reload Demo — standalone application.
//
// Runs the reloadable shell against the default audio device, so you can test
// DSP hot-reload without a DAW: start this, then edit logic_tremolo.cpp and run
// rebuild_logic.sh — the tremolo morphs live. (Audio plays out the default
// device; mind your output.)
#include "hot_reload_shell.hpp"
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

static std::atomic<bool> should_quit{false};
static void on_signal(int) { should_quit.store(true); }

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    pulp::format::StandaloneApp app(pulp::examples::create_hot_reload_shell);
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.input_channels = 2;
    config.output_channels = 2;
    app.set_config(config);

    if (!app.start()) {
        pulp::runtime::log_error("Failed to start standalone app");
        return 1;
    }
    std::cout << "\nPulp Hot-Reload Demo running.\n"
              << "Logic library: " << pulp::examples::hot_reload_demo_logic_path() << "\n"
              << "Edit examples/hot-reload-demo/logic_tremolo.cpp, run rebuild_logic.sh,\n"
              << "and the tremolo morphs live. Ctrl+C to quit.\n" << std::endl;

    while (!should_quit.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    app.stop();
    return 0;
}
