#include <pulp/signal/frequency_shifter_ssb.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>

namespace {
constexpr std::size_t kFrames = 1u << 18;
constexpr int kTrials = 21;
volatile double g_sink = 0.0;

double run() {
    pulp::signal::SsbFrequencyShifter64 shifter;
    shifter.prepare(48000.0);
    shifter.set_shift_hz(250.0);
    shifter.set_feedback(0.7);
    shifter.set_feedback_delay_ms(13.0);
    shifter.set_mix(1.0);
    shifter.reset();
    double checksum = 0.0;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        const double input = 0.37 * std::sin(0.017 * static_cast<double>(frame));
        checksum += shifter.process(input) * static_cast<double>(frame + 1);
    }
    const auto end = std::chrono::steady_clock::now();
    g_sink = checksum;
    return std::chrono::duration<double, std::nano>(end - begin).count() / kFrames;
}
}  // namespace

int main() {
    std::array<double, kTrials> samples{};
    for (double& sample : samples) sample = run();
    std::sort(samples.begin(), samples.end());
    std::cout << std::setprecision(10) << samples[samples.size() / 2] << ' '
              << samples[(samples.size() * 95) / 100] << ' ' << g_sink << '\n';
}
