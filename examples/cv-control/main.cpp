#include "cv_control_processor.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>
#include <thread>

namespace {
std::atomic<bool> quit{false};
void stop(int) {
    quit.store(true, std::memory_order_release);
}
} // namespace

int main() {
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    pulp::format::StandaloneApp app(pulp::examples::cv_control::create_face_tone);
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    app.set_config(config);
    if (!app.start()) {
        pulp::runtime::log_error("CV face tone failed to start");
        return 1;
    }
    std::cout << "CV Face Tone running. Set PULP_CV_OSC_PORT (for example 9001) "
                 "to receive /pulp/cv/face mouth confidence tracked [sequence].\n";
    while (!quit.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    app.stop();
    return 0;
}
