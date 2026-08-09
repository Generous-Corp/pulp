#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/expander.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::Expander;
using pulp::signal::Expander64;
using pulp::signal::ExpanderStatus;
using pulp::signal::ExpansionMode;

namespace {

double reference_gain_db(double level, ExpansionMode mode, double threshold, double ratio,
                         double range, double knee) {
    const double x = level - threshold;
    double gain = 0.0;
    if (mode == ExpansionMode::downward) {
        if (knee == 0.0)
            gain = x < 0.0 ? (ratio - 1.0) * x : 0.0;
        else if (x <= -knee / 2.0)
            gain = (ratio - 1.0) * x;
        else if (x < knee / 2.0)
            gain = -(ratio - 1.0) * std::pow(knee / 2.0 - x, 2.0) / (2.0 * knee);
        return std::max(-range, gain);
    }

    if (knee == 0.0)
        gain = x > 0.0 ? (ratio - 1.0) * x : 0.0;
    else if (x >= knee / 2.0)
        gain = (ratio - 1.0) * x;
    else if (x > -knee / 2.0)
        gain = (ratio - 1.0) * std::pow(x + knee / 2.0, 2.0) / (2.0 * knee);
    return std::min(range, gain);
}

bool curve_matches(const Expander64::Config& config, double ratio_for_oracle) {
    for (double level = -100.0; level <= 20.0; level += 0.25) {
        const double expected =
            reference_gain_db(level, config.mode, config.threshold_db, ratio_for_oracle,
                              config.range_db, config.knee_db);
        if (std::abs(Expander64::gain_computer_db(level, config) - expected) > 1.0e-12)
            return false;
    }
    return true;
}

int rising_gain_interval(Expander64& expander, double input, double lower, double upper,
                         int limit) {
    int lower_crossing = -1;
    for (int sample = 0; sample < limit; ++sample) {
        (void)expander.process(input, input);
        const double gain = expander.current_gain_db()[0];
        if (lower_crossing < 0 && gain >= lower)
            lower_crossing = sample;
        if (lower_crossing >= 0 && gain >= upper)
            return sample - lower_crossing;
    }
    return -1;
}

int falling_gain_interval(Expander64& expander, double input, double upper, double lower,
                          int limit) {
    int upper_crossing = -1;
    for (int sample = 0; sample < limit; ++sample) {
        (void)expander.process(input, input);
        const double gain = expander.current_gain_db()[0];
        if (upper_crossing < 0 && gain <= upper)
            upper_crossing = sample;
        if (upper_crossing >= 0 && gain <= lower)
            return sample - upper_crossing;
    }
    return -1;
}

} // namespace

static_assert(noexcept(std::declval<Expander&>().process(0.0f, 0.0f)));
static_assert(noexcept(std::declval<Expander&>().configure({})));
static_assert(noexcept(std::declval<Expander&>().set_bypassed(true)));
static_assert(noexcept(std::declval<Expander&>().reset()));

TEST_CASE("expander configuration is failure atomic", "[signal][expander][configuration]") {
    Expander64 changed;
    Expander64 control;
    auto config = changed.config();
    config.ratio = 4.0;
    config.detector = Expander64::DetectorMode::peak;
    REQUIRE(changed.configure(config) == ExpanderStatus::ready);
    REQUIRE(control.configure(config) == ExpanderStatus::ready);
    REQUIRE(changed.prepare(48000.0) == ExpanderStatus::ready);
    REQUIRE(control.prepare(48000.0) == ExpanderStatus::ready);
    for (int i = 0; i < 128; ++i) {
        REQUIRE(changed.process(0.25, -0.5) == control.process(0.25, -0.5));
    }

    auto invalid = config;
    invalid.range_db = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(changed.configure(invalid) == ExpanderStatus::invalid_config);
    REQUIRE(changed.config().ratio == config.ratio);
    REQUIRE(changed.process(0.125, -0.375) == control.process(0.125, -0.375));
    REQUIRE(changed.prepare(std::numeric_limits<double>::infinity()) ==
            ExpanderStatus::invalid_sample_rate);
    REQUIRE(changed.prepared());
    REQUIRE(changed.sample_rate() == 48000.0);
}

TEST_CASE("expander curves match an independent bounded knee oracle",
          "[signal][expander][gain-law][oracle]") {
    for (const auto mode : {ExpansionMode::downward, ExpansionMode::upward}) {
        for (double knee : {0.0, 8.0}) {
            Expander64::Config config;
            config.mode = mode;
            config.threshold_db = -30.0;
            config.ratio = 3.5;
            config.range_db = 18.0;
            config.knee_db = knee;
            REQUIRE(curve_matches(config, config.ratio));

            // Planted gain-law mutation: the same oracle must reject a ratio
            // accidentally interpreted as 1 + half of the requested slope.
            REQUIRE_FALSE(curve_matches(config, 1.0 + (config.ratio - 1.0) * 0.5));
        }
    }
}

TEST_CASE("expander detector attack and release honor exact 10-to-90 times",
          "[signal][expander][timing][oracle]") {
    constexpr double sample_rate = 1000.0;
    Expander64 expander;
    auto config = expander.config();
    config.mode = ExpansionMode::upward;
    config.threshold_db = -60.0;
    config.ratio = 2.0;
    config.range_db = 80.0;
    config.knee_db = 0.0;
    config.attack_ms = 10.0;
    config.release_ms = 25.0;
    config.detector = Expander64::DetectorMode::peak;
    config.stereo_link = pulp::signal::DynamicsStereoLink::independent;
    REQUIRE(expander.configure(config) == ExpanderStatus::ready);
    REQUIRE(expander.prepare(sample_rate) == ExpanderStatus::ready);

    const double gain_at_ten_percent = 20.0 * std::log10(0.1) - config.threshold_db;
    const double gain_at_ninety_percent = 20.0 * std::log10(0.9) - config.threshold_db;
    const int attack_interval =
        rising_gain_interval(expander, 1.0, gain_at_ten_percent, gain_at_ninety_percent, 100);
    REQUIRE(attack_interval >= 0);
    REQUIRE(std::abs(attack_interval - 10) <= 1);

    for (int i = 0; i < 100; ++i)
        (void)expander.process(1.0, 1.0);
    const int release_interval =
        falling_gain_interval(expander, 0.0, gain_at_ninety_percent, gain_at_ten_percent, 100);
    REQUIRE(release_interval >= 0);
    REQUIRE(std::abs(release_interval - 25) <= 1);

    // Negative control: an e-folding interpretation (63.2 to 36.8 percent)
    // must not accidentally satisfy the declared 10-to-90 attack interval.
    expander.reset();
    const double gain_at_632 = 20.0 * std::log10(0.632) - config.threshold_db;
    const double gain_at_368 = 20.0 * std::log10(0.368) - config.threshold_db;
    const int efold_interval = rising_gain_interval(expander, 1.0, gain_at_368, gain_at_632, 100);
    REQUIRE(efold_interval >= 0);
    REQUIRE(std::abs(efold_interval - 10) > 1);
}

TEST_CASE("expander processing preserves the curve below the threshold domain floor",
          "[signal][expander][gain-law][regression]") {
    Expander64 expander;
    auto config = expander.config();
    config.mode = ExpansionMode::downward;
    config.threshold_db = -160.0;
    config.ratio = 2.0;
    config.range_db = 96.0;
    config.knee_db = 0.0;
    config.attack_ms = 0.01;
    config.release_ms = 0.01;
    config.detector = Expander64::DetectorMode::peak;
    REQUIRE(expander.configure(config) == ExpanderStatus::ready);
    REQUIRE(expander.prepare(48000.0) == ExpanderStatus::ready);

    constexpr double input = 1.0e-9; // -180 dBFS, below the minimum threshold.
    std::array<double, 2> output{};
    for (int i = 0; i < 8; ++i)
        output = expander.process(input, input);
    REQUIRE_THAT(expander.current_gain_db()[0], WithinAbs(-20.0, 1.0e-10));
    REQUIRE_THAT(output[0], WithinAbs(input * 0.1, 1.0e-20));

    // Zero is the limiting downward case and takes the configured range; it is
    // not a sample exactly at the minimum configurable threshold.
    expander.reset();
    (void)expander.process(0.0, 0.0);
    REQUIRE(expander.current_gain_db()[0] == -config.range_db);

    Expander64 rms;
    config.detector = Expander64::DetectorMode::rms;
    config.attack_ms = 2000.0;
    REQUIRE(rms.configure(config) == ExpanderStatus::ready);
    REQUIRE(rms.prepare(48000.0) == ExpanderStatus::ready);
    const double above_threshold = std::pow(10.0, -155.0 / 20.0);
    for (int i = 0; i < 96000; ++i)
        (void)rms.process(above_threshold, above_threshold);
    REQUIRE_THAT(rms.current_gain_db()[0], WithinAbs(0.0, 1.0e-12));
}

TEST_CASE("expander bypass is exact and keeps detector history current",
          "[signal][expander][bypass]") {
    Expander64 bypassed;
    Expander64 control;
    auto config = bypassed.config();
    config.mode = ExpansionMode::upward;
    config.threshold_db = -40.0;
    config.detector = Expander64::DetectorMode::peak;
    REQUIRE(bypassed.configure(config) == ExpanderStatus::ready);
    REQUIRE(control.configure(config) == ExpanderStatus::ready);
    REQUIRE(bypassed.prepare(48000.0) == ExpanderStatus::ready);
    REQUIRE(control.prepare(48000.0) == ExpanderStatus::ready);
    bypassed.set_bypassed(true);

    for (int i = 0; i < 512; ++i) {
        const double left = static_cast<double>(i % 19 - 9) / 10.0;
        const double right = static_cast<double>(i % 13 - 6) / 8.0;
        REQUIRE(bypassed.process(left, right) == std::array<double, 2>{left, right});
        (void)control.process(left, right);
        REQUIRE(bypassed.current_gain_db() == control.current_gain_db());
    }

    bypassed.set_bypassed(false);
    REQUIRE(bypassed.process(0.5, -0.25) == control.process(0.5, -0.25));
}

TEST_CASE("expander nonfinite recovery is reset equivalent",
          "[signal][expander][nonfinite][reset]") {
    for (double bad :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity()}) {
        Expander64 expander;
        Expander64 reset_control;
        REQUIRE(expander.prepare(48000.0) == ExpanderStatus::ready);
        REQUIRE(reset_control.prepare(48000.0) == ExpanderStatus::ready);
        for (int i = 0; i < 128; ++i)
            (void)expander.process(0.8, 0.4);
        REQUIRE(expander.process(0.5, bad) == std::array<double, 2>{0.0, 0.0});
        for (int i = 0; i < 64; ++i) {
            const double input = static_cast<double>(i % 7 - 3) / 4.0;
            REQUIRE(expander.process(input, -input) == reset_control.process(input, -input));
        }
    }
}

TEST_CASE("expander block partitions are sample deterministic", "[signal][expander][partition]") {
    Expander64 scalar;
    Expander64 blocked;
    REQUIRE(scalar.prepare(48000.0) == ExpanderStatus::ready);
    REQUIRE(blocked.prepare(48000.0) == ExpanderStatus::ready);
    std::array<double, 257> left_scalar{};
    std::array<double, 257> right_scalar{};
    for (std::size_t i = 0; i < left_scalar.size(); ++i) {
        left_scalar[i] = static_cast<double>(static_cast<int>(i % 17) - 8) / 9.0;
        right_scalar[i] = static_cast<double>(static_cast<int>(i % 13) - 6) / 7.0;
    }
    auto left_block = left_scalar;
    auto right_block = right_scalar;
    for (std::size_t i = 0; i < left_scalar.size(); ++i) {
        const auto output = scalar.process(left_scalar[i], right_scalar[i]);
        left_scalar[i] = output[0];
        right_scalar[i] = output[1];
    }
    for (std::size_t offset = 0; offset < left_block.size();) {
        const int count =
            static_cast<int>(std::min<std::size_t>((offset % 11) + 1, left_block.size() - offset));
        blocked.process(left_block.data() + offset, right_block.data() + offset, count);
        offset += static_cast<std::size_t>(count);
    }
    REQUIRE(left_block == left_scalar);
    REQUIRE(right_block == right_scalar);

    scalar.reset();
    blocked.reset();
    REQUIRE(scalar.config().range_db == blocked.config().range_db);
    REQUIRE(scalar.process(0.25, -0.5) == blocked.process(0.25, -0.5));
}

TEST_CASE("expander process and bypass paths are allocation free", "[signal][expander][realtime]") {
    std::size_t planted_allocations = 0;
    void* planted = nullptr;
    {
        pulp::test::RtAllocationProbe probe;
        planted = ::operator new(sizeof(double) * 64);
        static_cast<double*>(planted)[0] = 1.0;
        planted_allocations = probe.allocation_count();
    }
    REQUIRE(static_cast<double*>(planted)[0] == 1.0);
    ::operator delete(planted);
    REQUIRE(planted_allocations > 0);

    Expander64 expander;
    REQUIRE(expander.prepare(48000.0) == ExpanderStatus::ready);
    std::array<double, 128> left{};
    std::array<double, 128> right{};
    pulp::test::RtAllocationProbe probe;
    expander.process(left.data(), right.data(), static_cast<int>(left.size()));
    expander.set_bypassed(true);
    expander.process(left.data(), right.data(), static_cast<int>(left.size()));
    expander.reset();
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("expander fails closed before prepare and bounds extreme upward gain",
          "[signal][expander][lifecycle]") {
    Expander expander;
    REQUIRE(expander.process(1.0f, -1.0f) == std::array<float, 2>{0.0f, 0.0f});
    auto config = expander.config();
    config.mode = ExpansionMode::upward;
    config.threshold_db = -160.0f;
    config.ratio = 20.0f;
    config.range_db = 96.0f;
    config.attack_ms = 0.01f;
    config.release_ms = 0.01f;
    config.detector = Expander::DetectorMode::peak;
    REQUIRE(expander.configure(config) == ExpanderStatus::ready);
    REQUIRE(expander.prepare(48000.0f) == ExpanderStatus::ready);
    const float largest = std::numeric_limits<float>::max();
    REQUIRE(expander.process(largest, largest)[0] == largest);
    REQUIRE(std::isfinite(expander.current_gain_db()[0]));
    REQUIRE(expander.latency_samples() == 0);
    REQUIRE(expander.tail_samples() == 0);
}
