#include <pulp/signal/diffusion_network.hpp>
#include <pulp/signal/character_delay/diffusion.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

using Catch::Approx;
using pulp::signal::DiffusionNetwork;
using pulp::signal::DiffusionNetwork64;

namespace {

using Network = DiffusionNetwork64;

Network::Config compact_config() {
    Network::Config config;
    config.stage_count = 3;
    config.stages[0] = {.delay_ms = 2.0, .gain = 0.55};
    config.stages[1] = {.delay_ms = 5.0, .gain = -0.45};
    config.stages[2] = {.delay_ms = 7.0, .gain = 0.35};
    config.stereo_width = 0.7;
    return config;
}

struct IndependentStage {
    std::vector<double> left;
    std::vector<double> right;
    std::size_t left_position = 0;
    std::size_t right_position = 0;
};

/// Direct scalar transcription of the published Schroeder recurrence. This
/// deliberately owns its storage/indexing and consumes only the public derived
/// delay metadata, so it is independent of the production ring implementation.
class IndependentOracle {
  public:
    IndependentOracle(const Network& network, const Network::Config& config, std::size_t capacity)
        : config_(config), angle_(config.stereo_width * std::numbers::pi / 4.0) {
        for (std::size_t i = 0; i < config.stage_count; ++i) {
            left_delay_[i] = network.stage_delay_samples(i, 0);
            right_delay_[i] = network.stage_delay_samples(i, 1);
            stages_[i].left.assign(capacity, 0.0);
            stages_[i].right.assign(capacity, 0.0);
        }
    }

    std::array<double, 2> process(double left, double right) {
        const double c = std::cos(angle_);
        const double s = std::sin(angle_);
        for (std::size_t i = 0; i < config_.stage_count; ++i) {
            auto& stage = stages_[i];
            const double delayed_left = stage.left[(stage.left_position + stage.left.size() -
                                                    static_cast<std::size_t>(left_delay_[i])) %
                                                   stage.left.size()];
            const double delayed_right = stage.right[(stage.right_position + stage.right.size() -
                                                      static_cast<std::size_t>(right_delay_[i])) %
                                                     stage.right.size()];
            const double gain = config_.stages[i].gain;
            const double write_left = left + gain * delayed_left;
            const double write_right = right + gain * delayed_right;
            const double allpass_left = delayed_left - gain * write_left;
            const double allpass_right = delayed_right - gain * write_right;
            stage.left[stage.left_position] = write_left;
            stage.right[stage.right_position] = write_right;
            stage.left_position = (stage.left_position + 1) % stage.left.size();
            stage.right_position = (stage.right_position + 1) % stage.right.size();
            const double sign = (i & 1u) == 0u ? 1.0 : -1.0;
            left = c * allpass_left - sign * s * allpass_right;
            right = sign * s * allpass_left + c * allpass_right;
        }
        return {left, right};
    }

  private:
    Network::Config config_;
    std::array<IndependentStage, Network::kMaxStages> stages_{};
    std::array<int, Network::kMaxStages> left_delay_{};
    std::array<int, Network::kMaxStages> right_delay_{};
    double angle_ = 0.0;
};

struct SpatialMetrics {
    double left_energy = 0.0;
    double right_energy = 0.0;
    double correlation = 1.0;
};

SpatialMetrics metrics(const std::vector<double>& left, const std::vector<double>& right) {
    SpatialMetrics result;
    double dot = 0.0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        result.left_energy += left[i] * left[i];
        result.right_energy += right[i] * right[i];
        dot += left[i] * right[i];
    }
    const double denominator = std::sqrt(result.left_energy * result.right_energy);
    result.correlation = denominator > 0.0 ? dot / denominator : 1.0;
    return result;
}

bool passes_true_stereo_gate(const SpatialMetrics& measured) {
    return measured.right_energy > 0.01 && std::abs(measured.correlation) < 0.98;
}

} // namespace

TEST_CASE("modulated allpass preserves live output-state history across gain changes",
          "[signal][diffusion][allpass][automation]") {
    pulp::signal::chardelay::ModulatedAllpass allpass;
    allpass.prepare(64u);
    REQUIRE(allpass.process(1.0, 4.0, 0.5) == Approx(-0.5));
    for (int sample = 0; sample < 4; ++sample)
        REQUIRE(allpass.process(0.0, 4.0, 0.5) == 0.0);
    REQUIRE(allpass.process(0.0, 4.0, 0.8) == Approx(0.75));
}

TEST_CASE("diffusion preparation and configuration are bounded and transactional",
          "[signal][diffusion][contract]") {
    Network network;
    REQUIRE_FALSE(network.prepared());
    REQUIRE(network.configure(compact_config()));
    REQUIRE_FALSE(network.prepare(999.0, 20.0));
    REQUIRE_FALSE(network.prepare(48000.0, 0.001));
    REQUIRE_FALSE(network.prepare(48000.0, 101.0));
    REQUIRE(network.prepare(1000.0, 40.0));
    REQUIRE(network.prepared());
    REQUIRE(network.retained_bytes() == 2u * Network::kMaxStages * 41u * sizeof(double));
    REQUIRE(network.configure(compact_config()));
    REQUIRE(network.config().stage_count == 3u);
    REQUIRE(network.stage_delay_samples(0, 0) == 2);
    REQUIRE(network.stage_delay_samples(0, 1) == 5);
    REQUIRE(network.stage_delay_samples(2, 0) == 7);
    REQUIRE(network.stage_delay_samples(2, 1) == 14);
    REQUIRE(network.latency_samples() == 0);
    REQUIRE(network.tail_samples() == -1);

    Network::Config rejected = network.config();
    rejected.stages[1].gain = 0.951;
    REQUIRE_FALSE(network.configure(rejected));
    REQUIRE(network.config().stages[1].gain == -0.45);
    rejected = network.config();
    rejected.stages[0].gain = 0.019;
    REQUIRE_FALSE(network.configure(rejected));
    rejected = network.config();
    rejected.stages[0].delay_ms = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(network.configure(rejected));
    rejected = network.config();
    rejected.stereo_width = 0.019;
    REQUIRE_FALSE(network.configure(rejected));
    rejected = network.config();
    rejected.stereo_width = 1.01;
    REQUIRE_FALSE(network.configure(rejected));
    rejected = network.config();
    rejected.stage_count = Network::kMaxStages + 1u;
    REQUIRE_FALSE(network.configure(rejected));

    Network finite;
    REQUIRE(finite.prepare(1000.0, 40.0));
    auto finite_config = compact_config();
    for (std::size_t i = 0; i < finite_config.stage_count; ++i)
        finite_config.stages[i].gain = 0.0;
    REQUIRE(finite.configure(finite_config));
    REQUIRE(finite.latency_samples() == 14);
    REQUIRE(finite.tail_samples() == 29);

    Network path_latency;
    REQUIRE(path_latency.prepare(1000.0, 20.0));
    auto uncoupled = finite_config;
    uncoupled.stage_count = 2;
    uncoupled.stages[0].delay_ms = 19.0;
    uncoupled.stages[1].delay_ms = 1.0;
    uncoupled.stereo_width = 0.0;
    REQUIRE(path_latency.configure(uncoupled));
    REQUIRE(path_latency.latency_samples() == 20);
    uncoupled.stereo_width = 1.0;
    REQUIRE(path_latency.configure(uncoupled));
    REQUIRE(path_latency.latency_samples() == 17);
    uncoupled.stereo_width = Network::kMinimumNonzeroStereoWidth;
    REQUIRE(path_latency.configure(uncoupled));
    REQUIRE(path_latency.latency_samples() == 17);
    double routed_left = 0.0;
    double routed_right = 0.0;
    for (int sample = 0; sample < 17; ++sample) {
        path_latency.process_sample(0.0, sample == 0 ? 1.0 : 0.0, routed_left, routed_right);
        REQUIRE(routed_left == 0.0);
        REQUIRE(routed_right == 0.0);
    }
    path_latency.process_sample(0.0, 0.0, routed_left, routed_right);
    REQUIRE(std::abs(routed_left) >= pulp::signal::snap_threshold<double>());

    auto cancellation_route = uncoupled;
    cancellation_route.stage_count = 3;
    cancellation_route.stages[0] = {.delay_ms = 1.0, .gain = 0.0};
    cancellation_route.stages[1] = {.delay_ms = 20.0, .gain = 0.5};
    cancellation_route.stages[2] = {.delay_ms = 18.0, .gain = 0.0};
    cancellation_route.stereo_width = 1.0;
    REQUIRE(path_latency.configure(cancellation_route));
    // The first two opposite rotations cancel on the direct path. The shorter
    // delay from each zero-gain stage therefore cannot be chained: the first
    // reachable route is right -> 4 samples -> right -> 11 samples -> output.
    REQUIRE(path_latency.latency_samples() == 15);
    for (int sample = 0; sample < 15; ++sample) {
        path_latency.process_sample(0.0, sample == 0 ? 1.0 : 0.0, routed_left, routed_right);
        REQUIRE(routed_left == 0.0);
        REQUIRE(routed_right == 0.0);
    }
    path_latency.process_sample(0.0, 0.0, routed_left, routed_right);
    REQUIRE(std::abs(routed_right) >= pulp::signal::snap_threshold<double>());

    auto recursive_reroute = cancellation_route;
    recursive_reroute.stages[1].delay_ms = 1.0;
    REQUIRE(path_latency.configure(recursive_reroute));
    // The recursive branch of the middle allpass reaches the final stage's
    // shorter right delay before either direct cancellation route completes.
    REQUIRE(path_latency.latency_samples() == 13);
    for (int sample = 0; sample < 13; ++sample) {
        path_latency.process_sample(sample == 0 ? 1.0 : 0.0, 0.0, routed_left, routed_right);
        REQUIRE(routed_left == 0.0);
        REQUIRE(routed_right == 0.0);
    }
    path_latency.process_sample(0.0, 0.0, routed_left, routed_right);
    REQUIRE(std::abs(routed_right) >= pulp::signal::snap_threshold<double>());

    Network single_sample_capacity;
    auto single_sample = compact_config();
    single_sample.stage_count = 1;
    single_sample.stages[0] = {.delay_ms = 1000.0 / 48000.0, .gain = 0.5};
    REQUIRE(single_sample_capacity.configure(single_sample));
    REQUIRE(single_sample_capacity.prepare(48000.0, 1000.0 / 48000.0));
    REQUIRE(single_sample_capacity.stage_delay_samples(0, 0) == 1);
}

TEST_CASE("failed prepare and configure preserve live diffusion state",
          "[signal][diffusion][transaction]") {
    Network subject;
    Network control;
    REQUIRE(subject.prepare(1000.0, 40.0));
    REQUIRE(control.prepare(1000.0, 40.0));
    REQUIRE(subject.configure(compact_config()));
    REQUIRE(control.configure(compact_config()));

    for (int i = 0; i < 23; ++i) {
        double subject_left = 0.0;
        double subject_right = 0.0;
        double control_left = 0.0;
        double control_right = 0.0;
        const double input_left = i == 0 ? 1.0 : 0.01 * i;
        const double input_right = i == 3 ? -0.5 : -0.005 * i;
        subject.process_sample(input_left, input_right, subject_left, subject_right);
        control.process_sample(input_left, input_right, control_left, control_right);
        REQUIRE(subject_left == control_left);
        REQUIRE(subject_right == control_right);
    }

    REQUIRE_FALSE(subject.prepare(1000.0, 4.0));
    auto invalid = subject.config();
    invalid.stages[0].gain = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(subject.configure(invalid));
    REQUIRE(subject.sample_rate() == 1000.0);
    REQUIRE(subject.maximum_delay_ms() == 40.0);

    for (int i = 0; i < 100; ++i) {
        double subject_left = 0.0;
        double subject_right = 0.0;
        double control_left = 0.0;
        double control_right = 0.0;
        subject.process_sample(0.0, 0.0, subject_left, subject_right);
        control.process_sample(0.0, 0.0, control_left, control_right);
        REQUIRE(subject_left == control_left);
        REQUIRE(subject_right == control_right);
    }
}

TEST_CASE("diffusion impulse matches an independent scalar recurrence",
          "[signal][diffusion][impulse][oracle]") {
    Network network;
    REQUIRE(network.prepare(1000.0, 20.0));
    auto config = compact_config();
    config.stage_count = 2;
    config.stereo_width = 0.6;
    REQUIRE(network.configure(config));
    IndependentOracle oracle(network, config, 21u);

    for (int sample = 0; sample < 256; ++sample) {
        const double input_left = sample == 0 ? 1.0 : 0.0;
        const double input_right = sample == 11 ? -0.25 : 0.0;
        double left = 0.0;
        double right = 0.0;
        network.process_sample(input_left, input_right, left, right);
        const auto expected = oracle.process(input_left, input_right);
        REQUIRE(left == Approx(expected[0]).margin(2.0e-14));
        REQUIRE(right == Approx(expected[1]).margin(2.0e-14));
    }
}

TEST_CASE("unitary diffusion preserves energy and rejects a dual-mono negative control",
          "[signal][diffusion][energy][stereo][negative-control]") {
    Network network;
    REQUIRE(network.prepare(1000.0, 40.0));
    REQUIRE(network.configure(compact_config()));
    constexpr std::size_t render_samples = 65536;
    std::vector<double> left(render_samples, 0.0);
    std::vector<double> right(render_samples, 0.0);
    for (std::size_t i = 0; i < render_samples; ++i)
        network.process_sample(i == 0u ? 1.0 : 0.0, 0.0, left[i], right[i]);

    const auto measured = metrics(left, right);
    REQUIRE(measured.left_energy + measured.right_energy == Approx(1.0).epsilon(2.0e-11));
    REQUIRE(passes_true_stereo_gate(measured));

    // Planted defect: two unrelated mono chains cannot route a left impulse to
    // the right output. The same acceptance metric must reject that mutation.
    const std::vector<double> unrelated_mono_right(render_samples, 0.0);
    REQUIRE_FALSE(passes_true_stereo_gate(metrics(left, unrelated_mono_right)));
}

TEST_CASE("diffusion blocks are in-place and partition invariant",
          "[signal][diffusion][block][partition]") {
    constexpr std::size_t sample_count = 997;
    std::array<double, sample_count> input_left{};
    std::array<double, sample_count> input_right{};
    for (std::size_t i = 0; i < sample_count; ++i) {
        input_left[i] = 0.6 * std::sin(0.037 * static_cast<double>(i));
        input_right[i] = 0.4 * std::cos(0.023 * static_cast<double>(i));
    }
    Network whole;
    Network partitioned;
    REQUIRE(whole.prepare(1000.0, 40.0));
    REQUIRE(partitioned.prepare(1000.0, 40.0));
    REQUIRE(whole.configure(compact_config()));
    REQUIRE(partitioned.configure(compact_config()));
    std::array<double, sample_count> expected_left{};
    std::array<double, sample_count> expected_right{};
    auto actual_left = input_left;
    auto actual_right = input_right;
    whole.process_block(input_left.data(), input_right.data(), expected_left.data(),
                        expected_right.data(), sample_count);
    std::size_t offset = 0;
    for (std::size_t size : {1u, 17u, 3u, 255u, 64u, 19u, 511u, 127u}) {
        const auto count = std::min(size, sample_count - offset);
        partitioned.process_block(actual_left.data() + offset, actual_right.data() + offset,
                                  actual_left.data() + offset, actual_right.data() + offset, count);
        offset += count;
        if (offset == sample_count)
            break;
    }
    if (offset < sample_count)
        partitioned.process_block(actual_left.data() + offset, actual_right.data() + offset,
                                  actual_left.data() + offset, actual_right.data() + offset,
                                  sample_count - offset);
    REQUIRE(actual_left == expected_left);
    REQUIRE(actual_right == expected_right);

    whole.process_block(nullptr, input_right.data(), expected_left.data(), expected_right.data(),
                        1);
    whole.process_block(input_left.data(), nullptr, expected_left.data(), expected_right.data(), 1);
    whole.process_block(input_left.data(), input_right.data(), nullptr, expected_right.data(), 1);
    whole.process_block(input_left.data(), input_right.data(), expected_left.data(), nullptr, 1);
}

TEST_CASE("reset and nonfinite faults recover to fresh diffusion state",
          "[signal][diffusion][reset][fault]") {
    Network network;
    Network fresh;
    REQUIRE(network.prepare(1000.0, 40.0));
    REQUIRE(fresh.prepare(1000.0, 40.0));
    REQUIRE(network.configure(compact_config()));
    REQUIRE(fresh.configure(compact_config()));
    double left = 0.0;
    double right = 0.0;
    for (int i = 0; i < 32; ++i)
        network.process_sample(i == 0 ? 1.0 : 0.0, 0.0, left, right);
    network.process_sample(std::numeric_limits<double>::quiet_NaN(), 0.0, left, right);
    REQUIRE(left == 0.0);
    REQUIRE(right == 0.0);

    for (int i = 0; i < 64; ++i) {
        double fresh_left = 0.0;
        double fresh_right = 0.0;
        const double input_left = i == 0 ? 0.75 : 0.0;
        const double input_right = i == 4 ? -0.25 : 0.0;
        network.process_sample(input_left, input_right, left, right);
        fresh.process_sample(input_left, input_right, fresh_left, fresh_right);
        REQUIRE(left == fresh_left);
        REQUIRE(right == fresh_right);
    }

    auto overflow_config = compact_config();
    overflow_config.stage_count = 1;
    overflow_config.stages[0].gain = Network::kMaximumAllpassGain;
    overflow_config.stereo_width = 1.0;
    REQUIRE(network.configure(overflow_config));
    network.process_sample(std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                           left, right);
    REQUIRE(left == 0.0);
    REQUIRE(right == 0.0);
    network.process_sample(0.0, 0.0, left, right);
    REQUIRE(left == 0.0);
    REQUIRE(right == 0.0);
}

TEST_CASE("diffusion realtime paths allocate no memory", "[signal][diffusion][rt-safety]") {
    DiffusionNetwork network;
    REQUIRE(network.prepare(48000.0, 50.0));
    std::array<float, 257> left{};
    std::array<float, 257> right{};
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = 0.5f * std::sin(0.1f * static_cast<float>(i));
        right[i] = 0.4f * std::cos(0.07f * static_cast<float>(i));
    }
    bool configured = false;
    bool allocated = false;
    {
        pulp::test::RtAllocationProbe probe;
        configured = network.configure(network.config());
        network.process_block(left.data(), right.data(), left.data(), right.data(), left.size());
        network.reset();
        allocated = probe.saw_allocation();
    }
    REQUIRE(configured);
    REQUIRE_FALSE(allocated);
}
