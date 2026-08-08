#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/nlms_adaptive_filter.hpp>
#include <pulp/signal/noise_source.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

namespace {

constexpr std::array<double, 5> kPlant{1.0, 0.6, -0.35, 0.15, -0.05};

template <typename T>
T plant_output(const std::array<double, 5>& plant, const std::vector<T>& history, T x) {
    T y = static_cast<T>(plant[0]) * x;
    for (std::size_t i = 1; i < plant.size() && i <= history.size(); ++i)
        y += static_cast<T>(plant[i]) * history[history.size() - i];
    return y;
}

double misalignment(const std::array<float, 5>& actual,
                    const std::array<double, 5>& expected = kPlant) {
    double sum = 0.0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const double d = static_cast<double>(actual[i]) - expected[i];
        sum += d * d;
    }
    return 10.0 * std::log10(std::max(sum, 1.0e-30));
}

} // namespace

TEST_CASE("NLMS identifies a plant with an independent coefficient oracle",
          "[signal][nlms]") {
    pulp::signal::NlmsAdaptiveFilter filter;
    filter.prepare(48000.0, 5);
    filter.set_step_size(0.8f);
    filter.set_denominator_floor(1.0e-8f);
    pulp::signal::NoiseSource noise;
    noise.set_seed(0x12345678u);
    std::vector<float> history;
    history.reserve(4);
    for (int n = 0; n < 30000; ++n) {
        const float x = noise.white();
        const float desired = plant_output(kPlant, history, x);
        filter.process_sample(desired, x);
        if (history.size() == 4) history.erase(history.begin());
        history.push_back(x);
    }
    std::array<float, 5> coefficients{};
    int count = 0;
    REQUIRE(filter.try_snapshot_coefficients(coefficients.data(), 5, count));
    REQUIRE(count == 5);
    CHECK(misalignment(coefficients) <= -80.0);
    CHECK(filter.latency_samples() == 0);
    CHECK(filter.retained_bytes() == 5u * 5u * sizeof(double));
}

TEST_CASE("NLMS freeze, reset, partitioning, validation, and snapshot bounds are exact",
          "[signal][nlms]") {
    pulp::signal::NlmsAdaptiveFilter a;
    pulp::signal::NlmsAdaptiveFilter b;
    a.prepare(48000.0, 8);
    b.prepare(48000.0, 8);
    a.set_active_taps(5);
    b.set_active_taps(5);
    std::array<float, 4168> primary{}, reference{};
    for (std::size_t i = 0; i < reference.size(); ++i) {
        reference[i] = std::sin(static_cast<float>(i) * 0.17f);
        primary[i] = reference[i] * 0.25f;
    }
    std::array<pulp::signal::NlmsAdaptiveFilter::Result, 4168> whole{}, pieces{};
    a.process_block(primary.data(), reference.data(), whole.data(), 4168);
    b.process_block(primary.data(), reference.data(), pieces.data(), 1);
    b.process_block(primary.data() + 1, reference.data() + 1, pieces.data() + 1, 7);
    b.process_block(primary.data() + 8, reference.data() + 8, pieces.data() + 8, 64);
    b.process_block(primary.data() + 72, reference.data() + 72, pieces.data() + 72, 4096);
    for (std::size_t i = 0; i < whole.size(); ++i) {
        CHECK(whole[i].estimate == pieces[i].estimate);
        CHECK(whole[i].error == pieces[i].error);
    }

    std::array<float, 5> before{}, after{};
    int count = 0;
    REQUIRE(a.try_snapshot_coefficients(before.data(), 5, count));
    a.set_adapt_enabled(false);
    a.process_block(primary.data(), reference.data(), whole.data(), 4168);
    REQUIRE(a.try_snapshot_coefficients(after.data(), 5, count));
    CHECK(before == after);
    CHECK_FALSE(a.try_snapshot_coefficients(after.data(), 4, count));
    CHECK_FALSE(a.set_active_taps(0));
    CHECK_FALSE(a.set_active_taps(9));
    a.set_step_size(std::numeric_limits<float>::quiet_NaN());
    a.set_denominator_floor(-1.0f);
    a.set_leakage(std::numeric_limits<float>::infinity());
    const auto bad = a.process_sample(std::numeric_limits<float>::quiet_NaN(), 0.0f);
    CHECK(bad.estimate == 0.0f);
    CHECK(bad.error == 0.0f);
    a.reset();
    b.reset();
    const auto denormal = a.process_sample(std::numeric_limits<float>::denorm_min(),
                                           std::numeric_limits<float>::denorm_min());
    CHECK(denormal.estimate == 0.0f);
    CHECK(denormal.error == 0.0f);
    std::array<float, 5> reset_a{}, reset_b{};
    REQUIRE(a.try_snapshot_coefficients(reset_a.data(), 5, count));
    REQUIRE(b.try_snapshot_coefficients(reset_b.data(), 5, count));
    CHECK(reset_a == reset_b);

    a.prepare(96000.0, 4);
    CHECK(a.sample_rate() == 96000.0);
    CHECK(a.max_taps() == 4);
    CHECK(a.active_taps() == 4);
    std::array<float, 4> reprepare{};
    REQUIRE(a.try_snapshot_coefficients(reprepare.data(), 4, count));
    CHECK(reprepare == std::array<float, 4>{});

    pulp::signal::NlmsAdaptiveFilter invalid;
    invalid.prepare(-1.0, 0);
    const auto fail_closed = invalid.process_sample(1.0f, 1.0f);
    CHECK(fail_closed.estimate == 0.0f);
    CHECK(fail_closed.error == 0.0f);
}

TEST_CASE("NLMS remains normalized on colored excitation and follows a plant switch",
          "[signal][nlms]") {
    pulp::signal::NlmsAdaptiveFilter filter;
    filter.prepare(48000.0, 5);
    filter.set_step_size(0.7f);
    pulp::signal::NoiseSource noise;
    noise.set_seed(0xBADC0DEu);
    float colored = 0.0f;
    std::vector<float> history;
    history.reserve(4);
    const std::array<double, 5> second{0.2, -0.1, 0.8, 0.4, -0.2};
    for (int n = 0; n < 50000; ++n) {
        colored = 0.92f * colored + 0.35f * noise.white();
        const auto& plant = n < 25000 ? kPlant : second;
        const float desired = plant_output(plant, history, colored);
        filter.process_sample(desired, colored);
        if (history.size() == 4) history.erase(history.begin());
        history.push_back(colored);
        if (n == 24999) {
            std::array<float, 5> first_coefficients{};
            int first_count = 0;
            REQUIRE(filter.try_snapshot_coefficients(first_coefficients.data(), 5,
                                                      first_count));
            CHECK(first_count == 5);
            CHECK(misalignment(first_coefficients) <= -80.0);
        }
    }
    std::array<float, 5> coefficients{};
    int count = 0;
    REQUIRE(filter.try_snapshot_coefficients(coefficients.data(), 5, count));
    CHECK(misalignment(coefficients, second) <= -65.0);
}

TEST_CASE("NLMS legal boundary parameters remain finite and energy bounded",
          "[signal][nlms]") {
    pulp::signal::NlmsAdaptiveFilter filter;
    filter.prepare(48000.0, 32);
    filter.set_step_size(std::nextafter(2.0f, 0.0f));
    filter.set_denominator_floor(std::numeric_limits<float>::denorm_min());
    filter.set_leakage(std::nextafter(1.0f, 0.0f));
    double energy = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const float reference = (i & 1) == 0 ? 0.75f : -0.5f;
        const auto result = filter.process_sample(0.25f * reference, reference);
        INFO("sample=" << i << " estimate=" << result.estimate
                       << " error=" << result.error);
        REQUIRE(std::isfinite(result.estimate));
        REQUIRE(std::isfinite(result.error));
        energy += static_cast<double>(result.estimate) * result.estimate +
                  static_cast<double>(result.error) * result.error;
    }
    CHECK(std::isfinite(energy));
    CHECK(energy < 1.0e8);
}

TEST_CASE("NLMS snapshot publication keeps coefficients and tap count coherent",
          "[signal][nlms][concurrency]") {
    pulp::signal::NlmsAdaptiveFilter filter;
    filter.prepare(48000.0, 8);
    filter.set_step_size(0.8f);
    constexpr std::array<double, 8> plant{0.8, -0.4, 0.3, -0.2,
                                          0.15, -0.12, 0.09, -0.07};
    pulp::signal::NoiseSource noise;
    noise.set_seed(0x51A7C0DEu);
    std::array<float, 7> history{};
    for (int n = 0; n < 40000; ++n) {
        const float x = noise.white();
        float desired = static_cast<float>(plant[0]) * x;
        for (std::size_t i = 1; i < plant.size(); ++i)
            desired += static_cast<float>(plant[i]) * history[i - 1];
        filter.process_sample(desired, x);
        for (std::size_t i = history.size() - 1; i > 0; --i)
            history[i] = history[i - 1];
        history[0] = x;
    }
    // Prime every publication slot with the now-static coefficients.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(filter.set_active_taps(5));
        REQUIRE(filter.set_active_taps(8));
    }
    std::array<float, 8> expected{};
    int expected_count = 0;
    REQUIRE(filter.try_snapshot_coefficients(expected.data(), 8, expected_count));
    REQUIRE(expected_count == 8);
    REQUIRE(std::abs(expected[7]) > 0.05f);

    std::atomic<bool> done{false};
    std::atomic<bool> mismatch{false};
    std::thread writer([&] {
        for (int i = 0; i < 100000; ++i)
            filter.set_active_taps((i & 1) == 0 ? 5 : 8);
        done.store(true, std::memory_order_release);
    });
    std::thread reader([&] {
        std::array<float, 8> snapshot{};
        while (!done.load(std::memory_order_acquire)) {
            int count = 0;
            if (!filter.try_snapshot_coefficients(snapshot.data(), 8, count)) continue;
            if (count != 5 && count != 8) {
                mismatch.store(true, std::memory_order_relaxed);
                break;
            }
            for (int i = 0; i < count; ++i) {
                if (snapshot[static_cast<std::size_t>(i)] !=
                    expected[static_cast<std::size_t>(i)]) {
                    mismatch.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        }
    });
    writer.join();
    reader.join();
    CHECK_FALSE(mismatch.load(std::memory_order_relaxed));
}

TEST_CASE("NLMS realtime paths allocate nothing after prepare", "[signal][nlms][rt]") {
    pulp::signal::NlmsAdaptiveFilter filter;
    filter.prepare(48000.0, 16);
    std::array<float, 32> input{};
    std::array<pulp::signal::NlmsAdaptiveFilter::Result, 32> output{};
    std::array<float, 16> coefficients{};
    int count = 0;
    pulp::test::RtAllocationProbe probe;
    filter.process_block(input.data(), input.data(), output.data(), 32);
    REQUIRE(filter.try_snapshot_coefficients(coefficients.data(), 16, count));
    CHECK_FALSE(probe.saw_allocation());
}
