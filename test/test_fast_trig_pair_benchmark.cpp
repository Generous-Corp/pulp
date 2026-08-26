// Advisory Release-only actual-consumer sine/cosine-pair benchmark.
// Timing never gates CI.

#include <pulp/signal/fast_math.hpp>
#include <pulp/signal/frequency_shifter_ssb.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>

namespace {

constexpr std::size_t kFrames = 1u << 15;
constexpr int kTrials = 15;
constexpr int kPasses = 3;
volatile double g_sink = 0.0;

struct Scenario {
    double shift_hz;
    double feedback;
    bool stereo;
};

struct Timing {
    double median_ns_per_frame;
    double p95_ns_per_frame;
    double checksum;
};

template <pulp::signal::FastTrigProfile Profile>
auto make_shifter(const Scenario& scenario) {
    pulp::signal::SsbFrequencyShifterT<double, Profile> shifter;
    shifter.prepare(48000.0);
    shifter.set_mode(scenario.stereo ? pulp::signal::FrequencyShiftMode::stereo_split
                                     : pulp::signal::FrequencyShiftMode::up);
    shifter.set_shift_hz(scenario.shift_hz);
    shifter.set_feedback(scenario.feedback);
    shifter.set_feedback_delay_ms(13.0);
    shifter.set_mix(1.0);
    shifter.set_stereo_spread(0.73);
    shifter.reset();
    return shifter;
}

const std::array<double, kFrames>& input_left() {
    static const auto input = [] {
        std::array<double, kFrames> values{};
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] = 0.37 * std::sin(2.0 * std::acos(-1.0) * 997.0 *
                                       static_cast<double>(i) / 48000.0);
        return values;
    }();
    return input;
}

const std::array<double, kFrames>& input_right() {
    static const auto input = [] {
        std::array<double, kFrames> values{};
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] = 0.29 * std::sin(2.0 * std::acos(-1.0) * 1481.0 *
                                       static_cast<double>(i) / 48000.0);
        return values;
    }();
    return input;
}

template <pulp::signal::FastTrigProfile Profile>
double render(const Scenario& scenario, std::array<double, kFrames>& left,
              std::array<double, kFrames>& right) {
    auto shifter = make_shifter<Profile>(scenario);
    // Force lazy input construction before the clock so the first p95 sample
    // cannot include table initialization.
    const auto& left_input = input_left();
    const auto& right_input = input_right();
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        left[frame] = left_input[frame];
        if (scenario.stereo) {
            right[frame] = right_input[frame];
            shifter.process_stereo(left[frame], right[frame]);
        } else {
            left[frame] = shifter.process(left[frame]);
        }
    }
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(end - begin).count();
}

struct TimingPair {
    Timing reference;
    Timing candidate;
};

Timing summarise(std::array<double, kTrials> trials, double checksum) {
    std::sort(trials.begin(), trials.end());
    return {trials[trials.size() / 2], trials[(trials.size() * 95) / 100], checksum};
}

TimingPair measure_pair(const Scenario& scenario) {
    std::array<double, kFrames> reference_left{}, reference_right{};
    std::array<double, kFrames> candidate_left{}, candidate_right{};
    std::array<double, kTrials> reference_trials{}, candidate_trials{};
    double reference_checksum = 0.0;
    double candidate_checksum = 0.0;

    auto run = [&](pulp::signal::FastTrigProfile profile,
                   std::array<double, kFrames>& left,
                   std::array<double, kFrames>& right,
                   double& checksum) {
        const double elapsed = profile == pulp::signal::FastTrigProfile::realtime_precise
                                   ? render<pulp::signal::FastTrigProfile::realtime_precise>(
                                         scenario, left, right)
                                   : render<pulp::signal::FastTrigProfile::reference>(scenario,
                                                                                     left, right);
        // Read every output outside the timed region. Besides defeating dead-code
        // elimination, this catches a fast path that accidentally renders only a
        // prefix or one stereo channel.
        for (std::size_t frame = 0; frame < kFrames; ++frame) {
            checksum += left[frame] * static_cast<double>(frame + 1);
            if (scenario.stereo)
                checksum += right[frame] * static_cast<double>(frame + 1);
        }
        return elapsed;
    };

    for (int trial = 0; trial < kTrials; ++trial) {
        double reference_elapsed = 0.0;
        double candidate_elapsed = 0.0;
        for (int pass = 0; pass < kPasses; ++pass) {
            const bool candidate_first = ((trial + pass) & 1) != 0;
            if (candidate_first) {
                candidate_elapsed += run(pulp::signal::FastTrigProfile::realtime_precise,
                                         candidate_left, candidate_right,
                                         candidate_checksum);
                reference_elapsed += run(pulp::signal::FastTrigProfile::reference,
                                         reference_left, reference_right,
                                         reference_checksum);
            } else {
                reference_elapsed += run(pulp::signal::FastTrigProfile::reference,
                                         reference_left, reference_right,
                                         reference_checksum);
                candidate_elapsed += run(pulp::signal::FastTrigProfile::realtime_precise,
                                         candidate_left, candidate_right,
                                         candidate_checksum);
            }
        }
        reference_trials[static_cast<std::size_t>(trial)] =
            reference_elapsed / (kPasses * kFrames);
        candidate_trials[static_cast<std::size_t>(trial)] =
            candidate_elapsed / (kPasses * kFrames);
        g_sink = reference_checksum + candidate_checksum;
    }
    return {summarise(reference_trials, reference_checksum),
            summarise(candidate_trials, candidate_checksum)};
}

struct Error {
    double maximum;
    double rms;
};

Error compare(const Scenario& scenario) {
    std::array<double, kFrames> reference_left{}, reference_right{};
    std::array<double, kFrames> candidate_left{}, candidate_right{};
    render<pulp::signal::FastTrigProfile::reference>(scenario, reference_left, reference_right);
    render<pulp::signal::FastTrigProfile::realtime_precise>(scenario, candidate_left,
                                                            candidate_right);
    double maximum = 0.0;
    double squared = 0.0;
    std::size_t count = 0;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        const double left_error = candidate_left[frame] - reference_left[frame];
        maximum = std::max(maximum, std::abs(left_error));
        squared += left_error * left_error;
        ++count;
        if (scenario.stereo) {
            const double right_error = candidate_right[frame] - reference_right[frame];
            maximum = std::max(maximum, std::abs(right_error));
            squared += right_error * right_error;
            ++count;
        }
    }
    return {maximum, std::sqrt(squared / static_cast<double>(count))};
}

} // namespace

int main() {
#if !defined(NDEBUG)
    std::cerr << "error: configure with -DCMAKE_BUILD_TYPE=Release\n";
    return 2;
#endif
    std::cout << std::setprecision(10)
              << "{\"schema\":\"pulp.fast-trig-pair-benchmark.v1\",\"frames\":"
              << kFrames << ",\"trials\":" << kTrials << ",\"passes\":" << kPasses
              << "}\n";
    for (bool stereo : {false, true}) {
        for (double feedback : {0.0, 0.7}) {
            for (double shift_hz : {5.0, 250.0, 2000.0}) {
                const Scenario scenario{shift_hz, feedback, stereo};
                const auto timings = measure_pair(scenario);
                const auto& reference = timings.reference;
                const auto& candidate = timings.candidate;
                const auto error = compare(scenario);
                std::cout << "{\"stereo\":" << (stereo ? "true" : "false")
                          << ",\"feedback\":" << feedback << ",\"shift_hz\":" << shift_hz
                          << ",\"reference_ns_per_frame\":" << reference.median_ns_per_frame
                          << ",\"candidate_ns_per_frame\":" << candidate.median_ns_per_frame
                          << ",\"speedup_percent\":"
                          << 100.0 * (reference.median_ns_per_frame -
                                      candidate.median_ns_per_frame) /
                                 reference.median_ns_per_frame
                          << ",\"reference_p95_ns_per_frame\":" << reference.p95_ns_per_frame
                          << ",\"candidate_p95_ns_per_frame\":" << candidate.p95_ns_per_frame
                          << ",\"max_abs_error\":" << error.maximum
                          << ",\"rms_error\":" << error.rms
                          << ",\"reference_checksum\":" << reference.checksum
                          << ",\"candidate_checksum\":" << candidate.checksum << "}\n";
            }
        }
    }
    return std::isfinite(g_sink) ? 0 : 1;
}
