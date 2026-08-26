// Advisory Release benchmark for bounded-cycle sine candidates.
//
// Timing is non-gating. The committed benchmark contains only platform math,
// Pulp's current FastMath implementation, and an intentionally weak Taylor
// negative control. Maintainers can supply non-redistributed research
// candidates at configure time through
// PULP_FAST_TRIG_LOCAL_CANDIDATES_HEADER; the header must provide
// pulp_fast_trig_local::report_candidates(report).

#include <pulp/signal/fast_math.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

#if defined(PULP_FAST_TRIG_LOCAL_CANDIDATES_HEADER)
#include PULP_FAST_TRIG_LOCAL_CANDIDATES_HEADER
#endif

namespace {

constexpr std::size_t kInputCount = 1u << 17;
constexpr std::size_t kFrameCount = 1u << 12;
constexpr int kWarmupPasses = 2;
constexpr int kMeasuredPasses = 6;
constexpr int kTrials = 21;
constexpr float kTwoPi = 6.28318530717958647692f;

volatile double g_observable_sink = 0.0;
volatile std::size_t g_invocation_seed = 0;

std::size_t next_invocation_seed() noexcept {
    const std::size_t next = g_invocation_seed + 1;
    g_invocation_seed = next;
    return next;
}

float wrap_cycles(float phase) noexcept {
    return phase - std::floor(phase);
}

float platform_sine(float phase) noexcept {
    return std::sin(kTwoPi * phase);
}

float legacy_fastmath_sine(float phase) noexcept {
    return pulp::signal::FastMath::sin(kTwoPi * phase);
}

float efficient_fastmath_sine(float phase) noexcept {
    return pulp::signal::FastMath::sin_cycles<
        pulp::signal::FastTrigProfile::realtime_efficient>(phase);
}

float precise_fastmath_sine(float phase) noexcept {
    return pulp::signal::FastMath::sin_cycles<
        pulp::signal::FastTrigProfile::realtime_precise>(phase);
}

// Deliberately incomplete odd Taylor series after quarter-wave folding. This
// is a negative control for both numerical rejection and timing interpretation.
float weak_taylor_sine(float phase) noexcept {
    float x = phase;
    if (x > 0.5f)
        x -= 1.0f;
    if (x > 0.25f)
        x = 0.5f - x;
    else if (x < -0.25f)
        x = -0.5f - x;
    const float radians = kTwoPi * x;
    return radians * (1.0f - radians * radians / 6.0f);
}

std::vector<float> make_inputs() {
    std::vector<float> values(kInputCount);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<float>(i) / static_cast<float>(values.size());
    }
    return values;
}

struct Timing {
    double median_ns;
    double p95_ns;
    double checksum;
};

template <typename Work> Timing measure(std::size_t operations, Work&& work) {
    g_invocation_seed = 0;
    for (int pass = 0; pass < kWarmupPasses; ++pass) {
        g_observable_sink = work(next_invocation_seed());
    }

    std::array<double, kTrials> trials{};
    double checksum = 0.0;
    for (int trial = 0; trial < kTrials; ++trial) {
        const auto begin = std::chrono::steady_clock::now();
        for (int pass = 0; pass < kMeasuredPasses; ++pass) {
            checksum = work(next_invocation_seed());
        }
        const auto end = std::chrono::steady_clock::now();
        g_observable_sink = checksum;
        const double elapsed_ns = std::chrono::duration<double, std::nano>(end - begin).count();
        trials[static_cast<std::size_t>(trial)] =
            elapsed_ns / static_cast<double>(kMeasuredPasses * operations);
    }
    std::sort(trials.begin(), trials.end());
    constexpr std::size_t p95_index = (95 * kTrials + 99) / 100 - 1;
    return {trials[trials.size() / 2], trials[p95_index], checksum};
}

struct Error {
    double max_absolute = 0.0;
    double rms = 0.0;
    double max_pair_radial = 0.0;
};

template <typename Sine> Error characterize(const std::vector<float>& inputs, Sine sine) {
    Error result;
    double squared_error = 0.0;
    for (const float phase : inputs) {
        const double reference = std::sin(static_cast<double>(phase) * 2.0 * std::acos(-1.0));
        const double actual = sine(phase);
        const double error = actual - reference;
        result.max_absolute = std::max(result.max_absolute, std::abs(error));
        squared_error += error * error;

        const double cosine = sine(wrap_cycles(phase + 0.25f));
        result.max_pair_radial =
            std::max(result.max_pair_radial, std::abs(std::hypot(actual, cosine) - 1.0));
    }
    result.rms = std::sqrt(squared_error / static_cast<double>(inputs.size()));
    return result;
}

template <typename Sine> Timing measure_primitive(const std::vector<float>& inputs, Sine sine) {
    return measure(inputs.size(), [&](std::size_t seed) {
        double sum = 0.0;
        const std::size_t mask = inputs.size() - 1;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            sum += sine(inputs[(i + seed) & mask]);
        }
        return sum;
    });
}

template <typename Sine> Timing measure_fm(Sine sine) {
    constexpr std::size_t kOperators = 8;
    constexpr std::size_t operations = kFrameCount * kOperators;
    return measure(operations, [&](std::size_t seed) {
        std::array<float, kOperators> phases{};
        std::array<float, kOperators> previous{};
        for (std::size_t op = 0; op < kOperators; ++op) {
            phases[op] = static_cast<float>((seed + op * 17) & 1023u) / 1024.0f;
        }
        double sum = 0.0;
        for (std::size_t frame = 0; frame < kFrameCount; ++frame) {
            float modulation = 0.0f;
            for (std::size_t op = 0; op < kOperators; ++op) {
                phases[op] = wrap_cycles(phases[op] + 0.00091f * static_cast<float>(op + 1));
                const float value =
                    sine(wrap_cycles(phases[op] + 0.075f * (previous[op] + modulation)));
                previous[op] = value;
                modulation = value;
                sum += value * (1.0 / static_cast<double>(op + 1));
            }
        }
        return sum;
    });
}

template <typename Sine> Timing measure_additive(Sine sine) {
    constexpr std::size_t kPartials = 64;
    constexpr std::size_t operations = kFrameCount * kPartials;
    return measure(operations, [&](std::size_t seed) {
        std::array<float, kPartials> phases{};
        for (std::size_t partial = 0; partial < kPartials; ++partial) {
            phases[partial] = static_cast<float>((seed + partial * 17) & 1023u) / 1024.0f;
        }
        double sum = 0.0;
        for (std::size_t frame = 0; frame < kFrameCount; ++frame) {
            double sample = 0.0;
            for (std::size_t partial = 0; partial < kPartials; ++partial) {
                phases[partial] =
                    wrap_cycles(phases[partial] + 0.000113f * static_cast<float>(partial + 1));
                sample += sine(phases[partial]) / static_cast<double>(partial + 1);
            }
            sum += sample;
        }
        return sum;
    });
}

template <typename Sine>
void report(std::string_view name, const std::vector<float>& inputs, Sine sine) {
    const Error error = characterize(inputs, sine);
    const Timing primitive = measure_primitive(inputs, sine);
    const Timing fm = measure_fm(sine);
    const Timing additive = measure_additive(sine);
    std::cout << "{\"kernel\":\"" << name << "\",\"max_abs_error\":" << error.max_absolute
              << ",\"rms_error\":" << error.rms
              << ",\"max_pair_radial_error\":" << error.max_pair_radial
              << ",\"primitive_median_ns\":" << primitive.median_ns
              << ",\"primitive_p95_ns\":" << primitive.p95_ns
              << ",\"fm_median_ns\":" << fm.median_ns << ",\"fm_p95_ns\":" << fm.p95_ns
              << ",\"additive_median_ns\":" << additive.median_ns
              << ",\"additive_p95_ns\":" << additive.p95_ns
              << ",\"checksum\":" << (primitive.checksum + fm.checksum + additive.checksum)
              << "}\n";
}

} // namespace

int main() {
#if !defined(NDEBUG)
    std::cerr << "error: configure with -DCMAKE_BUILD_TYPE=Release\n";
    return 2;
#endif

    const auto inputs = make_inputs();
    std::cout << std::setprecision(10)
              << "{\"schema\":\"pulp.fast-trig-benchmark.v1\",\"source\":\""
#if defined(PULP_FAST_TRIG_LOCAL_CANDIDATES_HEADER)
              << "committed-plus-local-overlay"
#else
              << "committed"
#endif
              << "\","
                 "\"inputs\":"
              << inputs.size() << ",\"trials\":" << kTrials << "}\n";
    report("platform-sinf", inputs, [](float phase) { return platform_sine(phase); });
    report("legacy-fastmath-bhaskara", inputs,
           [](float phase) { return legacy_fastmath_sine(phase); });
    report("fastmath-realtime-efficient", inputs,
           [](float phase) { return efficient_fastmath_sine(phase); });
    report("fastmath-realtime-precise", inputs,
           [](float phase) { return precise_fastmath_sine(phase); });
    report("weak-taylor-negative-control", inputs,
           [](float phase) { return weak_taylor_sine(phase); });

#if defined(PULP_FAST_TRIG_LOCAL_CANDIDATES_HEADER)
    pulp_fast_trig_local::report_candidates(
        [&](std::string_view name, auto sine) { report(name, inputs, sine); });
#else
    std::cout << "{\"local_candidates\":\"not_configured\"}\n";
#endif

    return std::isfinite(g_observable_sink) ? 0 : 1;
}
