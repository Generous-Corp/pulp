#include "timeline_step_sequencer.hpp"

#include <pulp/format/headless.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> quit_requested{false};
void on_signal(int) { quit_requested.store(true, std::memory_order_relaxed); }

bool run_headless() {
    pulp::format::HeadlessHost host(
        pulp::examples::timeline_phase1::create_timeline_step_sequencer);
    host.prepare(48'000.0, 128, 0, 2);
    if (!host.valid())
        return false;
    std::vector<float> left(128), right(128), input_left(128), input_right(128);
    std::array<float*, 2> output_ptrs{left.data(), right.data()};
    std::array<const float*, 2> input_ptrs{input_left.data(), input_right.data()};
    auto output = pulp::audio::BufferView<float>(output_ptrs.data(), 2, 128);
    auto input = pulp::audio::BufferView<const float>(input_ptrs.data(), 2, 128);
    host.process(output, input);
    double energy = 0.0;
    for (float sample : left)
        energy += std::abs(sample);
    return energy > 0.0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--headless")
        return run_headless() ? 0 : 1;
    if (argc != 1) {
        std::cerr << "usage: pulp-timeline-step-sequencer [--headless]\n";
        return 2;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    pulp::format::StandaloneApp app(
        pulp::examples::timeline_phase1::create_timeline_step_sequencer);
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48'000.0;
    config.buffer_size = 256;
    config.input_channels = 0;
    config.output_channels = 2;
    app.set_config(config);
    if (!app.start()) {
        pulp::runtime::log_error("Failed to start Timeline Step Sequencer");
        return 1;
    }
    std::cout << "Playing the one-bar Creative Timeline Engine pattern. "
                 "Press Ctrl+C to stop.\n";
    while (!quit_requested.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    app.stop();
    return 0;
}
