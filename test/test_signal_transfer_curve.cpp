#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/transfer_curve.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <span>
#include <thread>

using Catch::Matchers::WithinAbs;
using pulp::signal::ModulationCurve;
using pulp::signal::ModulationCurveShape;
using pulp::signal::TransferCurve64;
using pulp::signal::TransferCurvePoint64;
using pulp::signal::evaluate_transfer_curve;
using pulp::signal::prepare_transfer_curve;

namespace {

constexpr std::array<TransferCurvePoint64, 5> kCurve{{
    {-2.0, -1.0}, {-0.5, -0.25}, {0.0, 0.1}, {0.75, 0.8}, {2.0, 1.0}}};

double hand_oracle(double input) {
    if (!std::isfinite(input)) return 0.0;
    if (input <= -2.0) return -1.0;
    if (input < -0.5) return -1.0 + (input + 2.0) * (0.75 / 1.5);
    if (input < 0.0) return -0.25 + (input + 0.5) * (0.35 / 0.5);
    if (input < 0.75) return 0.1 + input * (0.7 / 0.75);
    if (input < 2.0) return 0.8 + (input - 0.75) * (0.2 / 1.25);
    return 1.0;
}

bool publish_main_curve(TransferCurve64& curve) {
    return curve.publish_curve(kCurve, -2.0, 2.0, -1.0, 1.0);
}

double hand_shaped_progress(double progress, ModulationCurve curve, bool rising) {
    switch (curve.shape) {
    case ModulationCurveShape::linear:
        return progress;
    case ModulationCurveShape::smoothstep:
        return progress * progress * (3.0 - 2.0 * progress);
    case ModulationCurveShape::hold:
        return 0.0;
    case ModulationCurveShape::exponential:
    case ModulationCurveShape::logarithmic: {
        double sign = curve.shape == ModulationCurveShape::exponential ? -1.0 : 1.0;
        if (!rising) sign = -sign;
        const double k = 8.0 * sign * static_cast<double>(curve.strength);
        return -std::expm1(-k * progress) / -std::expm1(-k);
    }
    }
    return progress;
}

} // namespace

TEST_CASE("Transfer curve matches an independent hand interpolation oracle",
          "[signal][transfer-curve][oracle]") {
    TransferCurve64 curve;
    REQUIRE(publish_main_curve(curve));
    const auto prepared = prepare_transfer_curve<double, 32>(kCurve, -2.0, 2.0, -1.0, 1.0);
    REQUIRE(prepared.has_value());

    for (int i = -240; i <= 240; ++i) {
        const double input = static_cast<double>(i) / 100.0;
        REQUIRE_THAT(curve.process(input), WithinAbs(hand_oracle(input), 2.0e-15));
        REQUIRE_THAT(evaluate_transfer_curve(*prepared, input),
                     WithinAbs(hand_oracle(input), 2.0e-15));
    }
}

TEST_CASE("Transfer curve uses the shared segment-shape vocabulary",
          "[signal][transfer-curve][oracle][shape]") {
    TransferCurve64 curve;
    const std::array<TransferCurvePoint64, 6> shaped{{
        {-1.0, -1.0, {ModulationCurveShape::linear, 1.0f}},
        {-0.6, -0.2, {ModulationCurveShape::smoothstep, 1.0f}},
        {-0.2, 0.4, {ModulationCurveShape::hold, 1.0f}},
        {0.2, -0.4, {ModulationCurveShape::exponential, 0.7f}},
        {0.6, 0.2, {ModulationCurveShape::logarithmic, 0.7f}},
        {1.0, 1.0, {}},
    }};
    REQUIRE(curve.publish_curve(shaped, -1.0, 1.0, -1.0, 1.0));

    for (std::size_t segment = 0; segment + 1 < shaped.size(); ++segment) {
        const auto& left = shaped[segment];
        const auto& right = shaped[segment + 1];
        for (int step = 1; step < 10; ++step) {
            const double progress = static_cast<double>(step) / 10.0;
            const double input = std::lerp(left.input, right.input, progress);
            const double shaped_progress = hand_shaped_progress(
                progress, left.curve_to_next, right.output >= left.output);
            const double expected = std::lerp(left.output, right.output, shaped_progress);
            REQUIRE_THAT(curve.process(input), WithinAbs(expected, 2.0e-15));
        }
    }
}

TEST_CASE("Transfer curve validates domains and rejects discontinuous point sets",
          "[signal][transfer-curve][validation]") {
    TransferCurve64 curve;
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const std::array<TransferCurvePoint64, 1> too_short{{{-1.0, -1.0}}};
    const std::array<TransferCurvePoint64, 3> duplicate{{
        {-1.0, -1.0}, {0.0, 0.0}, {0.0, 1.0}}};
    const std::array<TransferCurvePoint64, 3> unordered{{
        {-1.0, -1.0}, {0.5, 0.0}, {0.25, 1.0}}};
    const std::array<TransferCurvePoint64, 3> wrong_endpoints{{
        {-0.5, -1.0}, {0.0, 0.0}, {1.0, 1.0}}};
    const std::array<TransferCurvePoint64, 3> output_escape{{
        {-1.0, -1.0}, {0.0, 1.5}, {1.0, 1.0}}};
    const std::array<TransferCurvePoint64, 3> nonfinite{{
        {-1.0, -1.0}, {0.0, nan}, {1.0, 1.0}}};
    const std::array<TransferCurvePoint64, 4> over_capacity{{
        {-1.0, -1.0}, {-0.5, -0.5}, {0.5, 0.5}, {1.0, 1.0}}};

    REQUIRE_FALSE(curve.publish_curve(too_short, -1.0, 1.0, -1.0, 1.0));
    REQUIRE_FALSE(curve.publish_curve(duplicate, -1.0, 1.0, -1.0, 1.0));
    REQUIRE_FALSE(curve.publish_curve(unordered, -1.0, 1.0, -1.0, 1.0));
    REQUIRE_FALSE(curve.publish_curve(wrong_endpoints, -1.0, 1.0, -1.0, 1.0));
    REQUIRE_FALSE(curve.publish_curve(output_escape, -1.0, 1.0, -1.0, 1.0));
    REQUIRE_FALSE(curve.publish_curve(nonfinite, -1.0, 1.0, -1.0, 1.0));
    REQUIRE_FALSE(curve.publish_curve(kCurve, 2.0, -2.0, -1.0, 1.0));
    REQUIRE_FALSE(curve.publish_curve(kCurve, -2.0, 2.0, 1.0, -1.0));
    REQUIRE_FALSE((pulp::signal::prepare_transfer_curve<double, 3>(
        over_capacity, -1.0, 1.0, -1.0, 1.0).has_value()));

    REQUIRE_THAT(curve.process(0.25), WithinAbs(0.25, 0.0));
}

TEST_CASE("Transfer curve clamps endpoints and recovers non-finite samples",
          "[signal][transfer-curve][fault]") {
    TransferCurve64 curve;
    REQUIRE(publish_main_curve(curve));

    REQUIRE_THAT(curve.process(-20.0), WithinAbs(-1.0, 0.0));
    REQUIRE_THAT(curve.process(-2.0), WithinAbs(-1.0, 0.0));
    REQUIRE_THAT(curve.process(2.0), WithinAbs(1.0, 0.0));
    REQUIRE_THAT(curve.process(20.0), WithinAbs(1.0, 0.0));
    REQUIRE_THAT(curve.process(std::numeric_limits<double>::quiet_NaN()),
                 WithinAbs(0.0, 0.0));
    REQUIRE_THAT(curve.process(std::numeric_limits<double>::infinity()),
                 WithinAbs(0.0, 0.0));
    REQUIRE_THAT(curve.process(-std::numeric_limits<double>::infinity()),
                 WithinAbs(0.0, 0.0));
}

TEST_CASE("Monotone transfer curves remain monotone and segment bounded",
          "[signal][transfer-curve][monotonicity]") {
    TransferCurve64 curve;
    REQUIRE(publish_main_curve(curve));

    double previous = curve.process(-2.0);
    for (int i = 1; i <= 4000; ++i) {
        const double input = -2.0 + 4.0 * static_cast<double>(i) / 4000.0;
        const double output = curve.process(input);
        REQUIRE(output >= previous);
        REQUIRE(output >= -1.0);
        REQUIRE(output <= 1.0);
        previous = output;
    }
}

TEST_CASE("Transfer curve publication is transactional and reset adopts a complete curve",
          "[signal][transfer-curve][publication]") {
    TransferCurve64 curve;
    const std::array<TransferCurvePoint64, 3> square{{
        {-1.0, 1.0}, {0.0, 0.0}, {1.0, 1.0}}};
    const std::array<TransferCurvePoint64, 3> invalid{{
        {-1.0, -1.0}, {0.0, 2.0}, {1.0, 1.0}}};

    REQUIRE(curve.publish_curve(square, -1.0, 1.0, 0.0, 1.0));
    curve.reset();
    REQUIRE_THAT(curve.process(-0.5), WithinAbs(0.5, 0.0));
    REQUIRE_FALSE(curve.publish_curve(invalid, -1.0, 1.0, -1.0, 1.0));
    REQUIRE_THAT(curve.process(-0.5), WithinAbs(0.5, 0.0));
    REQUIRE(TransferCurve64::latency_samples() == 0);
    REQUIRE(TransferCurve64::tail_samples() == 0);
}

TEST_CASE("Transfer curve block, in-place, scalar, and partitioned processing agree",
          "[signal][transfer-curve][partition]") {
    std::array<double, 257> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = -2.4 + 4.8 * static_cast<double>(i) /
                              static_cast<double>(input.size() - 1);

    TransferCurve64 scalar_curve;
    TransferCurve64 block_curve;
    TransferCurve64 partitioned_curve;
    TransferCurve64 in_place_curve;
    REQUIRE(publish_main_curve(scalar_curve));
    REQUIRE(publish_main_curve(block_curve));
    REQUIRE(publish_main_curve(partitioned_curve));
    REQUIRE(publish_main_curve(in_place_curve));

    std::array<double, 257> scalar{};
    std::array<double, 257> block{};
    std::array<double, 257> partitioned{};
    auto in_place = input;
    for (std::size_t i = 0; i < input.size(); ++i) scalar[i] = scalar_curve.process(input[i]);
    block_curve.process(input.data(), block.data(), static_cast<int>(input.size()));
    partitioned_curve.process(input.data(), partitioned.data(), 31);
    partitioned_curve.process(input.data() + 31, partitioned.data() + 31, 97);
    partitioned_curve.process(input.data() + 128, partitioned.data() + 128, 129);
    in_place_curve.process(in_place.data(), static_cast<int>(in_place.size()));

    REQUIRE(block == scalar);
    REQUIRE(partitioned == scalar);
    REQUIRE(in_place == scalar);
}

TEST_CASE("Transfer curve invalid block boundaries are no-ops",
          "[signal][transfer-curve][bounds]") {
    TransferCurve64 curve;
    REQUIRE(publish_main_curve(curve));
    const std::array<double, 4> input{-2.0, -0.5, 0.75, 2.0};
    std::array<double, 4> output{7.0, 7.0, 7.0, 7.0};
    const auto unchanged = output;

    curve.process(nullptr, output.data(), static_cast<int>(output.size()));
    REQUIRE(output == unchanged);
    curve.process(input.data(), nullptr, static_cast<int>(input.size()));
    curve.process(input.data(), output.data(), 0);
    REQUIRE(output == unchanged);
    curve.process(input.data(), output.data(), -1);
    REQUIRE(output == unchanged);
}

TEST_CASE("Transfer curve audio processing allocates no memory",
          "[signal][transfer-curve][rt-safety]") {
    TransferCurve64 curve;
    REQUIRE(publish_main_curve(curve));
    std::array<double, 64> samples{};
    bool published = false;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        published = publish_main_curve(curve);
        curve.process(samples.data(), static_cast<int>(samples.size()));
        allocations = probe.allocation_count();
    }
    REQUIRE(published);
    REQUIRE(allocations == 0);
}

TEST_CASE("Transfer curve publication never exposes a torn snapshot",
          "[signal][transfer-curve][thread]") {
    TransferCurve64 curve;
    const std::array<TransferCurvePoint64, 3> ascending{{
        {-1.0, 8.0}, {0.0, 10.0}, {1.0, 12.0}}};
    const std::array<TransferCurvePoint64, 3> descending{{
        {-1.0, -7.0}, {0.0, -10.0}, {1.0, -13.0}}};
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> torn{false};

    REQUIRE(curve.publish_curve(ascending, -1.0, 1.0, -20.0, 20.0));
    curve.reset();

    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 20000; ++i) {
            const auto& points = (i & 1) == 0 ? ascending : descending;
            if (!curve.publish_curve(points, -1.0, 1.0, -20.0, 20.0))
                torn.store(true, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire)) {
        const double output = curve.process(0.25);
        if (output != 10.5 && output != -10.75)
            torn.store(true, std::memory_order_relaxed);
    }
    writer.join();
    REQUIRE_FALSE(torn.load(std::memory_order_relaxed));
}
