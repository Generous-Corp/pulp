#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/mirrored_history_buffer.hpp>
#include <pulp/signal/windowing.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <numeric>
#include <string_view>
#include <utility>
#include <vector>

using pulp::signal::MirroredHistoryBuffer;
using pulp::signal::WindowFunction;

namespace {

using Clock = std::chrono::steady_clock;

struct TimingSummary {
    double median_us = 0.0;
    double p99_us = 0.0;
    double maximum_us = 0.0;
};

TimingSummary summarize(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return {
        samples[samples.size() / 2u],
        samples[(samples.size() * 99u) / 100u],
        samples.back(),
    };
}

template <typename Operation> TimingSummary measure(Operation&& operation, int trials) {
    std::vector<double> elapsed;
    elapsed.reserve(static_cast<std::size_t>(trials));
    for (int trial = 0; trial < trials; ++trial) {
        const auto start = Clock::now();
        operation();
        elapsed.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    return summarize(std::move(elapsed));
}

void report(std::string_view name, const TimingSummary& timing) {
    std::cout << name << " median_us=" << timing.median_us << " p99_us=" << timing.p99_us
              << " max_us=" << timing.maximum_us << '\n';
}

} // namespace

TEST_CASE("Analysis utility Release benchmark reports wrap-position costs",
          "[bench][signal][history]") {
    constexpr std::size_t kCapacity = 1024u;
    constexpr int kTrials = 1000;

    MirroredHistoryBuffer<float> history;
    history.prepare(kCapacity);
    float sample = 0.25f;
    float sink = 0.0f;
    const auto push_timing = measure(
        [&] {
            for (int i = 0; i < 256; ++i) {
                history.push(sample);
                sample += 0.0001f;
            }
        },
        kTrials);
    sink += history.window().front();

    std::vector<float> contiguous_copy(kCapacity);
    const auto contiguous_timing = measure(
        [&] {
            const auto window = history.window();
            std::copy(window.begin(), window.end(), contiguous_copy.begin());
            history.push(contiguous_copy.back());
            sink += contiguous_copy.front();
        },
        kTrials);

    std::vector<float> circular(kCapacity);
    std::size_t write_pos = 17u;
    std::vector<float> modulo_copy(kCapacity);
    const auto modulo_timing = measure(
        [&] {
            for (std::size_t i = 0; i < kCapacity; ++i)
                modulo_copy[i] = circular[(write_pos + i) % kCapacity];
            circular[write_pos] = modulo_copy.back();
            sink += modulo_copy.front();
            if (++write_pos == kCapacity)
                write_pos = 0u;
        },
        kTrials);

    report("history_push_256", push_timing);
    report("stft_contiguous_copy_1024", contiguous_timing);
    report("stft_modulo_copy_1024", modulo_timing);
    CHECK(std::isfinite(push_timing.maximum_us));
    CHECK(std::isfinite(contiguous_timing.maximum_us));
    CHECK(std::isfinite(modulo_timing.maximum_us));
    CHECK(std::isfinite(sink));
}

TEST_CASE("Analysis utility Release benchmark reports window generation cost",
          "[bench][signal][window]") {
    constexpr int kTrials = 500;
    constexpr int kSize = 1024;
    std::size_t generated = 0u;
    double coefficient_sum = 0.0;

    const auto blackman_harris = measure(
        [&] {
            const auto window =
                WindowFunction::generate(kSize, WindowFunction::Type::blackman_harris);
            generated += window.size();
            coefficient_sum += std::accumulate(window.begin(), window.end(), 0.0);
        },
        kTrials);
    const auto blackman_nuttall = measure(
        [&] {
            const auto window =
                WindowFunction::generate(kSize, WindowFunction::Type::blackman_nuttall);
            generated += window.size();
            coefficient_sum += std::accumulate(window.begin(), window.end(), 0.0);
        },
        kTrials);

    report("blackman_harris_generate_1024", blackman_harris);
    report("blackman_nuttall_generate_1024", blackman_nuttall);
    CHECK(generated == static_cast<std::size_t>(kTrials * kSize * 2));
    CHECK(coefficient_sum > 0.0);
}
