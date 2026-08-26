#include <pulp/signal/drum/fm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>

namespace {

using pulp::signal::drum::Fm8DrumVoice;
using pulp::signal::drum::FmWaveTable;
using pulp::signal::drum::VelocityResponse;

constexpr std::size_t kFrames = 1u << 15;
constexpr int kTrials = 21;
constexpr int kPasses = 5;
volatile double g_sink = 0.0;

struct Scenario {
    int algorithm;
    int wave_base;
    double tune_hz;
    double depth;
    double feedback;
};

constexpr std::array<Scenario, 3> kScenarios{{
    {0, 0, 90.0, 0.0, 0.0},
    {12, 20, 180.0, 6.0, 0.45},
    {15, 22, 960.0, 12.0, 1.0},
}};

struct RenderResult {
    double checksum;
    std::uint64_t hash;
};

template <bool HashSamples> RenderResult render_all() {
    double checksum = 0.0;
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto& scenario : kScenarios) {
        Fm8DrumVoice voice;
        voice.prepare(48000.0);
        voice.set_algorithm(scenario.algorithm);
        voice.set_tune_hz(scenario.tune_hz);
        voice.set_depth(scenario.depth);
        voice.set_formant_hz(12000.0);
        voice.set_formant_q(0.7);
        voice.set_noise_level(0.0);
        voice.set_click_level(0.0);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        for (int op = 0; op < Fm8DrumVoice::operator_count; ++op) {
            voice.set_operator_level(op, 1.0 - 0.07 * static_cast<double>(op));
            voice.set_operator_ratio(op, 1.0 + 0.63 * static_cast<double>(op));
            voice.set_operator_decay_ms(op, 40.0 + 37.0 * static_cast<double>(op));
            voice.set_operator_feedback(op, scenario.feedback);
            voice.set_operator_wave(op, (scenario.wave_base + op) % FmWaveTable::wave_count);
        }
        voice.note_on(0.82f);
        std::array<float, 64> block{};
        for (std::size_t offset = 0; offset < kFrames; offset += block.size()) {
            block.fill(0.0f);
            voice.process(block.data(), static_cast<int>(block.size()));
            for (std::size_t index = 0; index < block.size(); ++index) {
                checksum +=
                    static_cast<double>(block[index]) * static_cast<double>(offset + index + 1);
                if constexpr (HashSamples) {
                    const auto bits = std::bit_cast<std::uint32_t>(block[index]);
                    for (unsigned shift = 0; shift < 32; shift += 8) {
                        hash ^= (bits >> shift) & 0xffu;
                        hash *= 1099511628211ULL;
                    }
                }
            }
        }
    }
    return {checksum, hash};
}

} // namespace

int main() {
#if !defined(NDEBUG)
    return 2;
#endif
    std::array<double, kTrials> trials{};
    double checksum = 0.0;
    for (int trial = 0; trial < kTrials; ++trial) {
        const auto begin = std::chrono::steady_clock::now();
        for (int pass = 0; pass < kPasses; ++pass) {
            const auto rendered = render_all<false>();
            checksum += rendered.checksum;
        }
        const auto end = std::chrono::steady_clock::now();
        trials[static_cast<std::size_t>(trial)] =
            std::chrono::duration<double, std::nano>(end - begin).count() /
            static_cast<double>(kPasses * kFrames * kScenarios.size());
        g_sink = checksum;
    }
    std::sort(trials.begin(), trials.end());
    const auto sample_hash = render_all<true>().hash;
    std::cout << std::setprecision(17) << "{\"median_ns_per_frame\":" << trials[trials.size() / 2]
              << ",\"checksum\":" << checksum << ",\"sample_hash\":\"" << std::hex << sample_hash
              << "\"}\n";
    return std::isfinite(g_sink) ? 0 : 1;
}
