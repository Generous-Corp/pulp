#include "timeline_audio_player.hpp"

#include <pulp/format/headless.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using pulp::examples::timeline_phase1::TimelineAudioPlayerProcessor;

std::shared_ptr<const std::vector<std::uint8_t>> source_bytes;
std::atomic<bool> quit_requested{false};

std::unique_ptr<pulp::format::Processor> create_loaded_audio_player() {
    if (!source_bytes)
        return nullptr;
    return std::make_unique<TimelineAudioPlayerProcessor>(*source_bytes);
}

void on_signal(int) { quit_requested.store(true, std::memory_order_relaxed); }

bool run_headless() {
    pulp::format::HeadlessHost host(
        pulp::examples::timeline_phase1::create_validation_timeline_audio_player);
    host.prepare(48'000.0, 128, 0, 2);
    if (!host.valid())
        return false;
    std::vector<float> left(128), right(128);
    std::array<float*, 2> output_ptrs{left.data(), right.data()};
    std::array<const float*, 0> input_ptrs{};
    auto output = pulp::audio::BufferView<float>(output_ptrs.data(), 2, 128);
    auto input = pulp::audio::BufferView<const float>(input_ptrs.data(), 0, 128);
    host.process(output, input);
    double energy = 0.0;
    for (float sample : left)
        energy += std::abs(sample);
    return energy > 0.0;
}

std::shared_ptr<const std::vector<std::uint8_t>> read_file(const std::string& path) {
    constexpr std::streamoff kMaximumInputBytes = 256 * 1024 * 1024;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return {};
    const auto size = stream.tellg();
    if (size <= 0 || size > kMaximumInputBytes)
        return {};
    stream.seekg(0);
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(
        static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes->data()), size))
        return {};
    return bytes;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--headless")
        return run_headless() ? 0 : 1;
    if (argc != 2) {
        std::cerr << "usage: pulp-timeline-audio-player <file.wav>\n";
        return 2;
    }
    source_bytes = read_file(argv[1]);
    if (!source_bytes) {
        std::cerr << "unable to read WAV: " << argv[1] << '\n';
        return 2;
    }
    auto validation = create_loaded_audio_player();
    auto* typed = dynamic_cast<TimelineAudioPlayerProcessor*>(validation.get());
    if (!typed || !typed->source_valid()) {
        std::cerr << "WAV rejected by the bounded decoder\n";
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    pulp::format::StandaloneApp app(create_loaded_audio_player);
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48'000.0;
    config.buffer_size = 256;
    config.input_channels = 0;
    config.output_channels = 2;
    app.set_config(config);
    if (!app.start()) {
        pulp::runtime::log_error("Failed to start Timeline Audio Player");
        return 1;
    }
    std::cout << "Playing " << argv[1] << " through the Creative Timeline Engine. "
                 "Press Ctrl+C to stop.\n";
    while (!quit_requested.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    app.stop();
    return 0;
}
