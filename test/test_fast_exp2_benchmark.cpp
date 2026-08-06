// Advisory Release benchmark for pulp::signal::FastMath::exp2.
//
// `prior_pulp_cubic_exp2` is the exact cubic-plus-ldexp implementation that
// Pulp shipped before FastMath::exp2 adopted std::exp2. It exists only in this
// benchmark translation unit; production code must not call it.
//
// Timing is intentionally non-gating. The executable prints repeated-trial
// medians and deterministic checksums for three real call domains so reviewers
// can reproduce the performance evidence without making CI host load a test.

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

namespace {

constexpr std::size_t kInputCount = 1u << 18;
constexpr int kWarmupPasses = 2;
constexpr int kMeasuredPasses = 8;
constexpr int kTrials = 9;

volatile double g_observable_sink = 0.0;

float prior_pulp_cubic_exp2(float x) noexcept {
    const float xi = std::floor(x);
    const float xf = x - xi;
    const float p = 1.0f + xf * (0.6931472f + xf * (0.2402265f + xf * 0.0558015f));
    return p * std::ldexp(1.0f, static_cast<int>(xi));
}

struct Domain {
    std::string_view name;
    float low;
    float high;
};

std::vector<float> make_inputs(const Domain& domain) {
    std::vector<float> values(kInputCount);
    const double span = static_cast<double>(domain.high) - domain.low;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double unit = static_cast<double>(i) / static_cast<double>(values.size() - 1);
        values[i] = static_cast<float>(static_cast<double>(domain.low) + span * unit);
    }
    return values;
}

template <typename Exp2>
double evaluate(const std::vector<float>& inputs, Exp2&& exp2) {
    double checksum = 0.0;
    for (const float input : inputs) checksum += static_cast<double>(exp2(input));
    return checksum;
}

struct Measurement {
    double median_ns_per_call;
    double checksum;
};

template <typename Exp2>
Measurement measure(const std::vector<float>& inputs, Exp2&& exp2) {
    for (int pass = 0; pass < kWarmupPasses; ++pass) {
        g_observable_sink = evaluate(inputs, exp2);
    }

    std::array<double, kTrials> trials{};
    double checksum = 0.0;
    for (int trial = 0; trial < kTrials; ++trial) {
        const auto begin = std::chrono::steady_clock::now();
        for (int pass = 0; pass < kMeasuredPasses; ++pass) {
            checksum = evaluate(inputs, exp2);
            g_observable_sink = checksum;
        }
        const auto end = std::chrono::steady_clock::now();
        const double elapsed_ns =
            std::chrono::duration<double, std::nano>(end - begin).count();
        trials[static_cast<std::size_t>(trial)] =
            elapsed_ns / static_cast<double>(kMeasuredPasses * inputs.size());
    }
    std::sort(trials.begin(), trials.end());
    return {trials[trials.size() / 2], checksum};
}

}  // namespace

int main() {
#if !defined(NDEBUG)
    std::cerr << "error: configure with -DCMAKE_BUILD_TYPE=Release\n";
    return 2;
#endif

    constexpr std::array domains{
        Domain{"pitch[-16,16]", -16.0f, 16.0f},
        Domain{"fm[-8,8]", -8.0f, 8.0f},
        Domain{"normal[-126,127]", -126.0f, 127.0f},
    };

    std::cout << std::fixed << std::setprecision(6)
              << "[fast-exp2-benchmark] Release NDEBUG=1 inputs=" << kInputCount
              << " trials=" << kTrials << " passes=" << kMeasuredPasses << '\n'
              << "domain fastmath_std_delegate_ns_per_call prior_pulp_cubic_ns_per_call "
                 "prior_over_fastmath fastmath_checksum prior_checksum\n";

    for (const auto& domain : domains) {
        const auto inputs = make_inputs(domain);
        const auto standard =
            measure(inputs, [](float x) { return pulp::signal::FastMath::exp2(x); });
        const auto prior = measure(inputs, prior_pulp_cubic_exp2);
        std::cout << domain.name << ' ' << standard.median_ns_per_call << ' '
                  << prior.median_ns_per_call << ' '
                  << prior.median_ns_per_call / standard.median_ns_per_call << ' '
                  << standard.checksum << ' ' << prior.checksum << '\n';
    }

    return std::isfinite(g_observable_sink) ? 0 : 1;
}
