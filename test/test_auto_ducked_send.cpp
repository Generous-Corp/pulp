#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/auto_ducked_send.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

using Catch::Matchers::WithinAbs;
using pulp::signal::AutoDuckedSend;
using pulp::signal::AutoDuckedSend64;
using pulp::signal::AutoDuckedSendStatus;
using pulp::signal::DynamicsStereoLink;

namespace {

double reference_gain_db(double detector_db, const AutoDuckedSend64::Config& config) {
    const double attenuation = std::clamp(detector_db - config.threshold_db, 0.0, config.range_db);
    return config.send_gain_db - attenuation;
}

bool curve_matches(const AutoDuckedSend64::Config& config, bool wrong_polarity) {
    for (double level = -120.0; level <= 12.0; level += 0.25) {
        const double expected = wrong_polarity
                                    ? config.send_gain_db + std::clamp(level - config.threshold_db,
                                                                       0.0, config.range_db)
                                    : reference_gain_db(level, config);
        if (std::abs(AutoDuckedSend64::gain_computer_db(level, config) - expected) > 1.0e-12)
            return false;
    }
    return true;
}

double oracle_step(double state, double input, double milliseconds, double sample_rate,
                   bool wrong_time_constant = false) {
    const double exponent = wrong_time_constant ? 1.0 : std::log(9.0);
    const double coefficient = 1.0 - std::exp(-exponent / (milliseconds * 0.001 * sample_rate));
    return state + coefficient * (std::abs(input) - state);
}

} // namespace

static_assert(noexcept(std::declval<AutoDuckedSend&>().process(0.0f, 0.0f, 0.0f, 0.0f)));
static_assert(noexcept(std::declval<AutoDuckedSend&>().configure({})));
static_assert(noexcept(std::declval<AutoDuckedSend&>().set_bypassed(true)));
static_assert(noexcept(std::declval<AutoDuckedSend&>().reset()));
static_assert(AutoDuckedSend::latency_samples() == 0);
static_assert(AutoDuckedSend::tail_samples() == 0);

TEST_CASE("auto-ducked send configuration is failure atomic",
          "[signal][auto-ducked-send][configuration]") {
    AutoDuckedSend64 changed;
    AutoDuckedSend64 control;
    auto config = changed.config();
    config.threshold_db = -30.0;
    config.range_db = 18.0;
    config.attack_ms = 4.0;
    config.release_ms = 80.0;
    config.send_gain_db = -3.0;
    config.detector = AutoDuckedSend64::DetectorMode::rms;
    REQUIRE(changed.configure(config) == AutoDuckedSendStatus::ready);
    REQUIRE(control.configure(config) == AutoDuckedSendStatus::ready);
    REQUIRE(changed.prepare(48000.0) == AutoDuckedSendStatus::ready);
    REQUIRE(control.prepare(48000.0) == AutoDuckedSendStatus::ready);
    for (int i = 0; i < 128; ++i)
        REQUIRE(changed.process(0.25, -0.5, 0.8, -0.4) == control.process(0.25, -0.5, 0.8, -0.4));

    auto invalid = config;
    invalid.range_db = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(changed.configure(invalid) == AutoDuckedSendStatus::invalid_config);
    REQUIRE(changed.config().range_db == config.range_db);
    REQUIRE(changed.process(0.125, -0.375, 0.5, -0.25) ==
            control.process(0.125, -0.375, 0.5, -0.25));
    REQUIRE(changed.prepare(std::numeric_limits<double>::infinity()) ==
            AutoDuckedSendStatus::invalid_sample_rate);
    REQUIRE(changed.prepared());
    REQUIRE(changed.sample_rate() == 48000.0);
}

TEST_CASE("auto-ducked send curve matches an independent bounded attenuation oracle",
          "[signal][auto-ducked-send][gain-law][oracle]") {
    AutoDuckedSend64::Config config;
    config.threshold_db = -28.0;
    config.range_db = 15.0;
    config.send_gain_db = -4.0;
    REQUIRE(curve_matches(config, false));

    // Planted wrong-polarity control: a detector rising over threshold must
    // reduce send gain, never increase it.
    REQUIRE_FALSE(curve_matches(config, true));
    REQUIRE(AutoDuckedSend64::gain_computer_db(-8.0, config) <
            AutoDuckedSend64::gain_computer_db(-48.0, config));
}

TEST_CASE("auto-ducked send detector matches an independent exact timing oracle",
          "[signal][auto-ducked-send][timing][oracle]") {
    constexpr double sample_rate = 1000.0;
    AutoDuckedSend64 send;
    auto config = send.config();
    config.attack_ms = 10.0;
    config.release_ms = 25.0;
    config.detector = AutoDuckedSend64::DetectorMode::peak;
    config.stereo_link = DynamicsStereoLink::independent;
    REQUIRE(send.configure(config) == AutoDuckedSendStatus::ready);
    REQUIRE(send.prepare(sample_rate) == AutoDuckedSendStatus::ready);

    double expected = 0.0;
    double wrong = 0.0;
    bool wrong_diverged = false;
    for (int i = 0; i < 40; ++i) {
        (void)send.process(0.25, 0.25, 1.0, 0.0);
        expected = oracle_step(expected, 1.0, config.attack_ms, sample_rate);
        wrong = oracle_step(wrong, 1.0, config.attack_ms, sample_rate, true);
        REQUIRE_THAT(send.detector_envelope()[0], WithinAbs(expected, 1.0e-12));
        wrong_diverged |= std::abs(send.detector_envelope()[0] - wrong) > 1.0e-3;
    }
    REQUIRE(wrong_diverged);

    for (int i = 0; i < 80; ++i) {
        (void)send.process(0.25, 0.25, 0.0, 0.0);
        expected = oracle_step(expected, 0.0, config.release_ms, sample_rate);
        REQUIRE_THAT(send.detector_envelope()[0], WithinAbs(expected, 1.0e-12));
    }
}

TEST_CASE("auto-ducked send applies gain only to the supplied wet send",
          "[signal][auto-ducked-send][wet-only]") {
    AutoDuckedSend64 send;
    auto config = send.config();
    config.threshold_db = -20.0;
    config.range_db = 12.0;
    config.attack_ms = 0.01;
    config.release_ms = 0.01;
    config.send_gain_db = -6.0;
    config.detector = AutoDuckedSend64::DetectorMode::peak;
    REQUIRE(send.configure(config) == AutoDuckedSendStatus::ready);
    REQUIRE(send.prepare(1000.0) == AutoDuckedSendStatus::ready);

    const auto quiet = send.process(0.8, -0.4, 0.01, -0.01);
    const double base = std::pow(10.0, config.send_gain_db / 20.0);
    REQUIRE_THAT(quiet[0], WithinAbs(0.8 * base, 1.0e-12));
    REQUIRE_THAT(quiet[1], WithinAbs(-0.4 * base, 1.0e-12));

    const auto loud = send.process(0.8, -0.4, 1.0, -1.0);
    const double ducked = std::pow(10.0, (config.send_gain_db - config.range_db) / 20.0);
    REQUIRE_THAT(loud[0], WithinAbs(0.8 * ducked, 1.0e-12));
    REQUIRE_THAT(loud[1], WithinAbs(-0.4 * ducked, 1.0e-12));
    // A dry path is deliberately absent from the API. Mixing happens after
    // this processor, so the external dry term remains unscaled.
    const double dry = 0.375;
    REQUIRE_THAT(dry + loud[0], WithinAbs(dry + 0.8 * ducked, 1.0e-12));

    AutoDuckedSend64 boundary;
    config.threshold_db = -160.0;
    config.range_db = 96.0;
    config.send_gain_db = -160.0;
    REQUIRE(boundary.configure(config) == AutoDuckedSendStatus::ready);
    REQUIRE(boundary.prepare(1000.0) == AutoDuckedSendStatus::ready);
    const auto base_boundary = boundary.process(1.0, -1.0, 0.0, 0.0);
    REQUIRE_THAT(base_boundary[0], WithinAbs(std::pow(10.0, -160.0 / 20.0), 1.0e-20));
    const auto combined_boundary = boundary.process(1.0, -1.0, 1.0, -1.0);
    REQUIRE(boundary.current_gain_db()[0] == -256.0);
    REQUIRE_THAT(combined_boundary[0], WithinAbs(std::pow(10.0, -256.0 / 20.0), 1.0e-24));
}

TEST_CASE("auto-ducked send neutral and bypass are exact while detection advances",
          "[signal][auto-ducked-send][bypass][neutral]") {
    AutoDuckedSend64 neutral;
    auto neutral_config = neutral.config();
    neutral_config.range_db = 0.0;
    neutral_config.send_gain_db = 0.0;
    REQUIRE(neutral.configure(neutral_config) == AutoDuckedSendStatus::ready);
    REQUIRE(neutral.prepare(48000.0) == AutoDuckedSendStatus::ready);
    const double tiny = std::numeric_limits<double>::denorm_min();
    REQUIRE(neutral.process(tiny, -tiny, 1.0, -1.0) == std::array<double, 2>{tiny, -tiny});

    AutoDuckedSend64 bypassed;
    AutoDuckedSend64 control;
    REQUIRE(bypassed.prepare(48000.0) == AutoDuckedSendStatus::ready);
    REQUIRE(control.prepare(48000.0) == AutoDuckedSendStatus::ready);
    bypassed.set_bypassed(true);
    for (int i = 0; i < 256; ++i) {
        const double wet_l = static_cast<double>(i % 17 - 8) / 9.0;
        const double wet_r = static_cast<double>(i % 13 - 6) / 7.0;
        const double detector = i < 128 ? 1.0 : 0.0;
        REQUIRE(bypassed.process(wet_l, wet_r, detector, -detector) ==
                std::array<double, 2>{wet_l, wet_r});
        (void)control.process(wet_l, wet_r, detector, -detector);
        REQUIRE(bypassed.detector_envelope() == control.detector_envelope());
    }
    bypassed.set_bypassed(false);
    REQUIRE(bypassed.process(0.5, -0.25, 0.3, -0.3) == control.process(0.5, -0.25, 0.3, -0.3));
}

TEST_CASE("auto-ducked send stereo link is polarity safe and preserves the image",
          "[signal][auto-ducked-send][stereo-link]") {
    AutoDuckedSend64 linked;
    auto config = linked.config();
    config.attack_ms = 0.01;
    config.release_ms = 0.01;
    config.threshold_db = -30.0;
    config.range_db = 18.0;
    config.stereo_link = DynamicsStereoLink::peak_linked;
    REQUIRE(linked.configure(config) == AutoDuckedSendStatus::ready);
    REQUIRE(linked.prepare(1000.0) == AutoDuckedSendStatus::ready);
    const auto linked_output = linked.process(0.8, -0.4, 1.0, -1.0);
    REQUIRE(linked.current_gain_db()[0] == linked.current_gain_db()[1]);
    REQUIRE_THAT(linked_output[0] / linked_output[1], WithinAbs(-2.0, 1.0e-12));

    AutoDuckedSend64 independent;
    config.stereo_link = DynamicsStereoLink::independent;
    REQUIRE(independent.configure(config) == AutoDuckedSendStatus::ready);
    REQUIRE(independent.prepare(1000.0) == AutoDuckedSendStatus::ready);
    const auto independent_output = independent.process(0.8, -0.4, 1.0, 0.0);
    REQUIRE(independent.current_gain_db()[0] < independent.current_gain_db()[1]);
    REQUIRE(std::abs(independent_output[0]) < std::abs(independent_output[1]));
}

TEST_CASE("auto-ducked send block in-place partitions and reset are deterministic",
          "[signal][auto-ducked-send][block][partition][reset]") {
    constexpr std::size_t count = 257;
    std::array<double, count> send_left{};
    std::array<double, count> send_right{};
    std::array<double, count> detector_left{};
    std::array<double, count> detector_right{};
    for (std::size_t i = 0; i < count; ++i) {
        send_left[i] = static_cast<double>(static_cast<int>(i % 17) - 8) / 9.0;
        send_right[i] = static_cast<double>(static_cast<int>(i % 13) - 6) / 7.0;
        detector_left[i] = static_cast<double>(i % 23) / 22.0;
        detector_right[i] = -static_cast<double>(i % 19) / 18.0;
    }

    AutoDuckedSend64 scalar;
    AutoDuckedSend64 blocked;
    AutoDuckedSend64 out_of_place;
    REQUIRE(scalar.prepare(48000.0) == AutoDuckedSendStatus::ready);
    REQUIRE(blocked.prepare(48000.0) == AutoDuckedSendStatus::ready);
    REQUIRE(out_of_place.prepare(48000.0) == AutoDuckedSendStatus::ready);
    auto scalar_left = send_left;
    auto scalar_right = send_right;
    for (std::size_t i = 0; i < count; ++i) {
        const auto output =
            scalar.process(scalar_left[i], scalar_right[i], detector_left[i], detector_right[i]);
        scalar_left[i] = output[0];
        scalar_right[i] = output[1];
    }

    auto block_left = send_left;
    auto block_right = send_right;
    for (std::size_t offset = 0; offset < count;) {
        const int partition =
            static_cast<int>(std::min<std::size_t>((offset % 11) + 1, count - offset));
        blocked.process_block(block_left.data() + offset, block_right.data() + offset,
                              detector_left.data() + offset, detector_right.data() + offset,
                              partition);
        offset += static_cast<std::size_t>(partition);
    }
    REQUIRE(block_left == scalar_left);
    REQUIRE(block_right == scalar_right);

    std::array<double, count> separate_left{};
    std::array<double, count> separate_right{};
    out_of_place.process_block(send_left.data(), send_right.data(), detector_left.data(),
                               detector_right.data(), separate_left.data(), separate_right.data(),
                               static_cast<int>(count));
    REQUIRE(separate_left == scalar_left);
    REQUIRE(separate_right == scalar_right);

    scalar.reset();
    blocked.reset();
    REQUIRE(scalar.detector_envelope() == std::array<double, 2>{0.0, 0.0});
    REQUIRE(scalar.process(0.25, -0.5, 0.75, -0.5) == blocked.process(0.25, -0.5, 0.75, -0.5));
}

TEST_CASE("auto-ducked send nonfinite recovery is reset equivalent",
          "[signal][auto-ducked-send][nonfinite]") {
    for (double bad :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity()}) {
        for (int bad_port = 0; bad_port < 4; ++bad_port) {
            AutoDuckedSend64 send;
            AutoDuckedSend64 reset_control;
            REQUIRE(send.prepare(48000.0) == AutoDuckedSendStatus::ready);
            REQUIRE(reset_control.prepare(48000.0) == AutoDuckedSendStatus::ready);
            for (int i = 0; i < 128; ++i)
                (void)send.process(0.8, -0.4, 1.0, -1.0);
            std::array<double, 4> input{0.5, -0.25, 0.75, -0.5};
            input[static_cast<std::size_t>(bad_port)] = bad;
            REQUIRE(send.process(input[0], input[1], input[2], input[3]) ==
                    std::array<double, 2>{0.0, 0.0});
            REQUIRE(send.process(0.25, -0.5, 0.75, -0.5) ==
                    reset_control.process(0.25, -0.5, 0.75, -0.5));
        }
    }

    const double bad = std::numeric_limits<double>::quiet_NaN();
    AutoDuckedSend64 bypassed;
    REQUIRE(bypassed.prepare(48000.0) == AutoDuckedSendStatus::ready);
    bypassed.set_bypassed(true);
    REQUIRE(bypassed.process(0.25, -0.5, bad, 0.0) == std::array<double, 2>{0.25, -0.5});
    REQUIRE(bypassed.detector_envelope() == std::array<double, 2>{0.0, 0.0});

    AutoDuckedSend64 neutral;
    auto config = neutral.config();
    config.range_db = 0.0;
    config.send_gain_db = 0.0;
    REQUIRE(neutral.configure(config) == AutoDuckedSendStatus::ready);
    REQUIRE(neutral.prepare(48000.0) == AutoDuckedSendStatus::ready);
    REQUIRE(neutral.process(0.25, -0.5, 0.0, bad) == std::array<double, 2>{0.25, -0.5});

    config.send_gain_db = -6.0;
    REQUIRE(neutral.configure(config) == AutoDuckedSendStatus::ready);
    const auto base_gain_only = neutral.process(0.25, -0.5, bad, 0.0);
    const double base_gain = std::pow(10.0, config.send_gain_db / 20.0);
    REQUIRE_THAT(base_gain_only[0], WithinAbs(0.25 * base_gain, 1.0e-12));
    REQUIRE_THAT(base_gain_only[1], WithinAbs(-0.5 * base_gain, 1.0e-12));
}

TEST_CASE("auto-ducked send realtime paths allocate no memory",
          "[signal][auto-ducked-send][realtime]") {
    std::size_t planted_allocations = 0;
    void* planted = nullptr;
    {
        pulp::test::RtAllocationProbe probe;
        planted = ::operator new(sizeof(double) * 64);
        planted_allocations = probe.allocation_count();
    }
    ::operator delete(planted);
    REQUIRE(planted_allocations > 0);

    AutoDuckedSend64 send;
    REQUIRE(send.prepare(48000.0) == AutoDuckedSendStatus::ready);
    std::array<double, 128> left{};
    std::array<double, 128> right{};
    std::array<double, 128> detector{};
    detector.fill(1.0);
    pulp::test::RtAllocationProbe probe;
    send.process_block(left.data(), right.data(), detector.data(), detector.data(),
                       static_cast<int>(left.size()));
    send.set_bypassed(true);
    send.process_block(left.data(), right.data(), detector.data(), detector.data(),
                       static_cast<int>(left.size()));
    send.reset();
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("auto-ducked send lifecycle and metadata fail closed",
          "[signal][auto-ducked-send][lifecycle]") {
    AutoDuckedSend64 send;
    REQUIRE_FALSE(send.prepared());
    REQUIRE(send.process(1.0, -1.0, 1.0, -1.0) == std::array<double, 2>{0.0, 0.0});
    REQUIRE(send.latency_samples() == 0);
    REQUIRE(send.tail_samples() == 0);
    REQUIRE(send.prepare(48000.0) == AutoDuckedSendStatus::ready);
    send.process_block(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 128);
}
