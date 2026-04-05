// PulpGain Standalone Application
// Runs PulpGain as a standalone audio processor
// Opens the default audio device and passes audio through the gain effect

#include "pulp_gain.hpp"
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

static std::atomic<bool> should_quit{false};

void signal_handler(int) {
    should_quit.store(true);
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    pulp::runtime::log_info("PulpGain Standalone v1.0.0");

    pulp::format::StandaloneApp app(pulp::examples::create_pulp_gain);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 2; // Pass-through effect needs input

    app.set_config(config);

    if (!app.start()) {
        pulp::runtime::log_error("Failed to start standalone app");
        return 1;
    }

    std::cout << "\nPulpGain is running. Audio passes through with gain applied.\n"
              << "Input Gain:  " << app.state().get_value(pulp::examples::kInputGain) << " dB\n"
              << "Output Gain: " << app.state().get_value(pulp::examples::kOutputGain) << " dB\n"
              << "Bypass:      " << (app.state().get_value(pulp::examples::kBypass) >= 0.5f ? "ON" : "OFF") << "\n"
              << "\nPress Ctrl+C to quit.\n" << std::endl;

    while (!should_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    app.stop();
    pulp::runtime::log_info("PulpGain stopped");
    return 0;
}
