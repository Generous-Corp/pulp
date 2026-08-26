// Advisory Release-only actual-consumer FM8 fast-trig benchmark.
// Timing never gates CI.

#include <pulp/signal/drum/fm.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

using pulp::signal::FastTrigProfile;
using pulp::signal::drum::Fm8DrumVoice;

constexpr std::size_t kTimingFrames = 1u << 14;
constexpr double kQualitySeconds = 0.75;
constexpr int kTrials = 15;
constexpr int kPasses = 3;
volatile double g_sink = 0.0;

struct Scenario {
    const char* name;
    int algorithm;
    int wave_base;
    double tune_hz;
    double depth;
    double feedback;
};

constexpr std::array<Scenario, 3> kScenarios{{
    {"parallel_sine", 0, 0, 90.0, 0.0, 0.0},
    {"branching_harmonic", 12, 20, 180.0, 6.0, 0.45},
    {"serial_stress", 15, 22, 960.0, 12.0, 1.0},
}};

struct Timing {
    double median_ns_per_frame;
    double p95_ns_per_frame;
    double checksum;
};

void configure_voice(Fm8DrumVoice& voice, const Scenario& scenario, double sample_rate,
                     FastTrigProfile profile) {
    voice.prepare(sample_rate);
    voice.set_algorithm(scenario.algorithm);
    voice.set_tune_hz(scenario.tune_hz);
    voice.set_depth(scenario.depth);
    voice.set_formant_hz(std::min(12000.0, sample_rate * 0.24));
    voice.set_formant_q(0.7);
    voice.set_noise_level(0.0);
    voice.set_click_level(0.0);
    voice.set_velocity_response({0.0f, 0.0f, 0.0f, 0.0f});
    for (int op = 0; op < Fm8DrumVoice::operator_count; ++op) {
        voice.set_operator_level(op, 1.0 - 0.07 * static_cast<double>(op));
        voice.set_operator_ratio(op, 1.0 + 0.63 * static_cast<double>(op));
        voice.set_operator_decay_ms(op, 40.0 + 37.0 * static_cast<double>(op));
        voice.set_operator_feedback(op, scenario.feedback);
        voice.set_operator_wave(op, (scenario.wave_base + op) %
                                        pulp::signal::drum::FmWaveTable::wave_count);
    }
    voice.set_trig_profile(profile);
    voice.note_on(0.82f);
}

double render(const Scenario& scenario, double sample_rate, int block_size, FastTrigProfile profile,
              std::vector<float>& output) {
    Fm8DrumVoice voice;
    configure_voice(voice, scenario, sample_rate, profile);
    std::fill(output.begin(), output.end(), 0.0f);
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t offset = 0; offset < output.size();) {
        const int count = std::min<int>(block_size, static_cast<int>(output.size() - offset));
        voice.process(output.data() + offset, count);
        offset += static_cast<std::size_t>(count);
    }
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(end - begin).count();
}

Timing summarise(std::array<double, kTrials> trials, double checksum) {
    std::sort(trials.begin(), trials.end());
    return {trials[trials.size() / 2], trials[(trials.size() * 95) / 100], checksum};
}

struct Pair {
    Timing reference;
    Timing candidate;
    double maximum_error;
    double rms_error;
    double reference_peak;
    double candidate_peak;
    double reference_rms;
    double candidate_rms;
    double reference_dc;
    double candidate_dc;
    double reference_high_fraction;
    double candidate_high_fraction;
};

struct Metrics {
    double peak;
    double rms;
    double dc;
    double high_fraction;
};

Metrics metrics(const std::vector<float>& output, double sample_rate) {
    double peak = 0.0;
    double squared = 0.0;
    double sum = 0.0;
    double low = 0.0;
    double high = 0.0;
    double state = 0.0;
    const double coefficient = 1.0 - std::exp(-2.0 * std::acos(-1.0) * 2000.0 / sample_rate);
    for (float sample : output) {
        const double value = static_cast<double>(sample);
        peak = std::max(peak, std::abs(value));
        squared += value * value;
        sum += value;
        state += coefficient * (value - state);
        low += state * state;
        const double high_pass = value - state;
        high += high_pass * high_pass;
    }
    return {peak, std::sqrt(squared / static_cast<double>(output.size())),
            sum / static_cast<double>(output.size()), high / (high + low + 1.0e-30)};
}

Pair measure(const Scenario& scenario, double sample_rate, int block_size) {
    std::vector<float> reference_output(kTimingFrames);
    std::vector<float> candidate_output(kTimingFrames);
    std::array<double, kTrials> reference_trials{};
    std::array<double, kTrials> candidate_trials{};
    double reference_checksum = 0.0;
    double candidate_checksum = 0.0;

    auto run = [&](FastTrigProfile profile, std::vector<float>& output, double& checksum) {
        const double elapsed = render(scenario, sample_rate, block_size, profile, output);
        for (std::size_t frame = 0; frame < output.size(); ++frame)
            checksum += static_cast<double>(output[frame]) * static_cast<double>(frame + 1);
        return elapsed;
    };

    for (int trial = 0; trial < kTrials; ++trial) {
        double reference_elapsed = 0.0;
        double candidate_elapsed = 0.0;
        for (int pass = 0; pass < kPasses; ++pass) {
            const bool candidate_first = ((trial + pass) & 1) != 0;
            if (candidate_first) {
                candidate_elapsed +=
                    run(FastTrigProfile::realtime_precise, candidate_output, candidate_checksum);
                reference_elapsed +=
                    run(FastTrigProfile::reference, reference_output, reference_checksum);
            } else {
                reference_elapsed +=
                    run(FastTrigProfile::reference, reference_output, reference_checksum);
                candidate_elapsed +=
                    run(FastTrigProfile::realtime_precise, candidate_output, candidate_checksum);
            }
        }
        reference_trials[static_cast<std::size_t>(trial)] =
            reference_elapsed / (kPasses * kTimingFrames);
        candidate_trials[static_cast<std::size_t>(trial)] =
            candidate_elapsed / (kPasses * kTimingFrames);
        g_sink = reference_checksum + candidate_checksum;
    }

    const auto quality_frames = static_cast<std::size_t>(std::ceil(sample_rate * kQualitySeconds));
    reference_output.resize(quality_frames);
    candidate_output.resize(quality_frames);
    render(scenario, sample_rate, block_size, FastTrigProfile::reference, reference_output);
    render(scenario, sample_rate, block_size, FastTrigProfile::realtime_precise, candidate_output);
    double maximum_error = 0.0;
    double squared_error = 0.0;
    for (std::size_t frame = 0; frame < quality_frames; ++frame) {
        const double error = static_cast<double>(candidate_output[frame]) -
                             static_cast<double>(reference_output[frame]);
        maximum_error = std::max(maximum_error, std::abs(error));
        squared_error += error * error;
    }

    const auto reference_metrics = metrics(reference_output, sample_rate);
    const auto candidate_metrics = metrics(candidate_output, sample_rate);
    return {summarise(reference_trials, reference_checksum),
            summarise(candidate_trials, candidate_checksum),
            maximum_error,
            std::sqrt(squared_error / static_cast<double>(quality_frames)),
            reference_metrics.peak,
            candidate_metrics.peak,
            reference_metrics.rms,
            candidate_metrics.rms,
            reference_metrics.dc,
            candidate_metrics.dc,
            reference_metrics.high_fraction,
            candidate_metrics.high_fraction};
}

} // namespace

int main() {
#if !defined(NDEBUG)
    std::cerr << "error: configure with -DCMAKE_BUILD_TYPE=Release\n";
    return 2;
#endif
    std::cout << std::setprecision(10)
              << "{\"schema\":\"pulp.fast-trig-fm8-benchmark.v2\",\"timing_frames\":"
              << kTimingFrames << ",\"quality_seconds\":" << kQualitySeconds
              << ",\"trials\":" << kTrials << ",\"passes\":" << kPasses << "}\n";
    for (const auto& scenario : kScenarios) {
        for (double sample_rate : {44100.0, 48000.0, 96000.0}) {
            for (int block_size : {32, 64, 128, 512}) {
                const auto pair = measure(scenario, sample_rate, block_size);
                const double speedup =
                    100.0 *
                    (pair.reference.median_ns_per_frame - pair.candidate.median_ns_per_frame) /
                    pair.reference.median_ns_per_frame;
                std::cout << "{\"scenario\":\"" << scenario.name
                          << "\",\"sample_rate\":" << sample_rate
                          << ",\"block_size\":" << block_size
                          << ",\"reference_ns_per_frame\":" << pair.reference.median_ns_per_frame
                          << ",\"candidate_ns_per_frame\":" << pair.candidate.median_ns_per_frame
                          << ",\"speedup_percent\":" << speedup
                          << ",\"reference_p95_ns_per_frame\":" << pair.reference.p95_ns_per_frame
                          << ",\"candidate_p95_ns_per_frame\":" << pair.candidate.p95_ns_per_frame
                          << ",\"max_abs_error\":" << pair.maximum_error
                          << ",\"rms_error\":" << pair.rms_error
                          << ",\"reference_peak\":" << pair.reference_peak
                          << ",\"candidate_peak\":" << pair.candidate_peak
                          << ",\"reference_rms\":" << pair.reference_rms
                          << ",\"candidate_rms\":" << pair.candidate_rms
                          << ",\"reference_dc\":" << pair.reference_dc
                          << ",\"candidate_dc\":" << pair.candidate_dc
                          << ",\"reference_high_fraction\":" << pair.reference_high_fraction
                          << ",\"candidate_high_fraction\":" << pair.candidate_high_fraction
                          << ",\"reference_checksum\":" << pair.reference.checksum
                          << ",\"candidate_checksum\":" << pair.candidate.checksum << "}\n";
            }
        }
    }
    return std::isfinite(g_sink) ? 0 : 1;
}
