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
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

#if defined(PULP_FAST_TRIG_LOCAL_CANDIDATES_HEADER)
#include PULP_FAST_TRIG_LOCAL_CANDIDATES_HEADER
#endif

namespace {

constexpr std::size_t kDefaultInputCount = 1u << 17;
constexpr std::size_t kMaximumInputCount = 1u << 24;
constexpr std::uint32_t kQualificationAdjacentRadius = 1u << 16;
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

std::vector<float> make_inputs(std::size_t input_count) {
    std::vector<float> values(input_count);
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
    float max_absolute_phase = 0.0f;
    double rms = 0.0;
    double max_pair_radial = 0.0;
    float max_pair_radial_phase = 0.0f;
    std::size_t non_finite_evaluations = 0;
};

template <typename Sine>
void inspect_error(float phase, Sine sine, Error& result, double* squared_error) {
    const double reference = std::sin(static_cast<double>(phase) * 2.0 * std::acos(-1.0));
    const double actual = sine(phase);
    if (!std::isfinite(actual)) {
        ++result.non_finite_evaluations;
        return;
    }
    const double error = actual - reference;
    const double absolute_error = std::abs(error);
    if (absolute_error > result.max_absolute) {
        result.max_absolute = absolute_error;
        result.max_absolute_phase = phase;
    }
    if (squared_error != nullptr)
        *squared_error += error * error;

    const double cosine = sine(wrap_cycles(phase + 0.25f));
    if (!std::isfinite(cosine)) {
        ++result.non_finite_evaluations;
        return;
    }
    const double radial_error = std::abs(std::hypot(actual, cosine) - 1.0);
    if (radial_error > result.max_pair_radial) {
        result.max_pair_radial = radial_error;
        result.max_pair_radial_phase = phase;
    }
}

template <typename Sine>
Error characterize(const std::vector<float>& inputs, Sine sine, std::uint32_t adjacent_radius) {
    Error result;
    double squared_error = 0.0;
    for (const float phase : inputs)
        inspect_error(phase, sine, result, &squared_error);
    result.rms = std::sqrt(squared_error / static_cast<double>(inputs.size()));

    if (adjacent_radius == 0)
        return result;

    const std::array<float, 8> anchors{
        result.max_absolute_phase,
        result.max_pair_radial_phase,
        0.0f,
        0.25f,
        0.5f,
        0.75f,
        std::nextafter(0.5f, 0.0f),
        std::nextafter(1.0f, 0.0f),
    };
    constexpr std::uint32_t last_phase_bits = 0x3f7fffffu;
    for (const float anchor : anchors) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(anchor);
        const std::uint32_t begin = bits > adjacent_radius ? bits - adjacent_radius : 0;
        const std::uint32_t end = std::min(last_phase_bits, bits + adjacent_radius);
        for (std::uint32_t candidate_bits = begin; candidate_bits <= end; ++candidate_bits)
            inspect_error(std::bit_cast<float>(candidate_bits), sine, result, nullptr);
    }
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
void report(std::string_view name, const std::vector<float>& inputs, Sine sine,
            bool qualification_only) {
    const Error error = characterize(
        inputs, sine, qualification_only ? kQualificationAdjacentRadius : 0);
    std::cout << "{\"kernel\":\"" << name << "\",\"max_abs_error\":" << error.max_absolute
              << ",\"max_abs_phase\":" << error.max_absolute_phase
              << ",\"dense_rms_error\":" << error.rms
              << ",\"max_pair_radial_error\":" << error.max_pair_radial
              << ",\"max_pair_radial_phase\":" << error.max_pair_radial_phase
              << ",\"non_finite_evaluations\":" << error.non_finite_evaluations;
    if (qualification_only) {
        std::cout << "}\n";
        return;
    }

    const Timing primitive = measure_primitive(inputs, sine);
    const Timing fm = measure_fm(sine);
    const Timing additive = measure_additive(sine);
    std::cout << ",\"primitive_median_ns\":" << primitive.median_ns
              << ",\"primitive_p95_ns\":" << primitive.p95_ns
              << ",\"fm_median_ns\":" << fm.median_ns << ",\"fm_p95_ns\":" << fm.p95_ns
              << ",\"additive_median_ns\":" << additive.median_ns
              << ",\"additive_p95_ns\":" << additive.p95_ns
              << ",\"checksum\":" << (primitive.checksum + fm.checksum + additive.checksum)
              << "}\n";
}

struct Options {
    std::size_t input_count = kDefaultInputCount;
    bool qualification_only = false;
};

bool parse_input_count(std::string_view text, std::size_t& result) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end && result >= 2 &&
           result <= kMaximumInputCount && (result & (result - 1)) == 0;
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--qualification-only") {
            options.qualification_only = true;
        } else if (argument == "--inputs") {
            if (++i >= argc || !parse_input_count(argv[i], options.input_count)) {
                std::cerr << "error: --inputs requires a power of two from 2 through "
                          << kMaximumInputCount << "\n";
                return false;
            }
        } else if (argument == "--help") {
            std::cout << "usage: pulp-fast-trig-benchmark "
                         "[--qualification-only] [--inputs POWER_OF_TWO]\n";
            return false;
        } else {
            std::cerr << "error: unknown argument: " << argument << "\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
#if !defined(NDEBUG)
    std::cerr << "error: configure with -DCMAKE_BUILD_TYPE=Release\n";
    return 2;
#endif

    Options options;
    if (!parse_options(argc, argv, options))
        return argc == 2 && std::string_view{argv[1]} == "--help" ? 0 : 2;

    const auto inputs = make_inputs(options.input_count);
    std::cout << std::setprecision(10)
              << "{\"schema\":\""
              << (options.qualification_only ? "pulp.fast-trig-qualification.v1"
                                             : "pulp.fast-trig-benchmark.v1")
              << "\",\"source\":\""
#if defined(PULP_FAST_TRIG_LOCAL_CANDIDATES_HEADER)
              << "committed-plus-local-overlay"
#else
              << "committed"
#endif
              << "\",\"inputs\":" << inputs.size();
    if (!options.qualification_only)
        std::cout << ",\"trials\":" << kTrials;
    else
        std::cout << ",\"adjacent_radius\":" << kQualificationAdjacentRadius;
    std::cout << "}\n";
    report("platform-sinf", inputs, [](float phase) { return platform_sine(phase); },
           options.qualification_only);
    report("legacy-fastmath-bhaskara", inputs,
           [](float phase) { return legacy_fastmath_sine(phase); }, options.qualification_only);
    report("fastmath-realtime-efficient", inputs,
           [](float phase) { return efficient_fastmath_sine(phase); },
           options.qualification_only);
    report("fastmath-realtime-precise", inputs,
           [](float phase) { return precise_fastmath_sine(phase); },
           options.qualification_only);
    report("weak-taylor-negative-control", inputs,
           [](float phase) { return weak_taylor_sine(phase); }, options.qualification_only);

#if defined(PULP_FAST_TRIG_LOCAL_CANDIDATES_HEADER)
    pulp_fast_trig_local::report_candidates(
        [&](std::string_view name, auto sine) {
            report(name, inputs, sine, options.qualification_only);
        });
#else
    std::cout << "{\"local_candidates\":\"not_configured\"}\n";
#endif

    return std::isfinite(g_observable_sink) ? 0 : 1;
}
