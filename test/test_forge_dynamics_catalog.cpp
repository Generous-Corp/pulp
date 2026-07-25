// Dynamics catalog nodes — bake-layer tests.
//
// The sibling of test_forge_lofi_catalog.cpp for the compressor family: it
// proves each baked param moves the BAKED node's audio over the real production
// path (bake() → BakedGraphProcessor → claim_param_injection → ParamInjector →
// routed executor → ParamCursor), not only over the DSP class the unit suite
// already covers.
//
// The stereo-link case is the one that most needs to run here rather than only
// at the DSP level: the node is registered TRUE STEREO (two ports as one
// logical wire), and a wiring mistake that instanced it dual-mono would still
// compress correctly per channel and would only be audible as an image shift on
// hard-panned material. This asserts the coupling survives the graph.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_dynamics_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp::host;
namespace dyn = pulp::host::dynamics;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;
constexpr double kToneHz = 750.0;  // 64 samples/period: whole cycles per block

struct BakedFixture {
    SignalGraph g;
    LowerResult result;
    NodeId custom_node = 0;

    explicit BakedFixture(const CustomNodeType& type) {
        REQUIRE(g.register_custom_node_type(type));
        const auto in = g.add_input_node(2, "In");
        custom_node = g.add_custom_node(type.type_id, 1, "Node");
        const auto out = g.add_output_node(2, "Out");
        for (int port = 0; port < 2; ++port) {
            REQUIRE(g.connect(in, static_cast<PortIndex>(port), custom_node,
                              static_cast<PortIndex>(port)));
            REQUIRE(g.connect(custom_node, static_cast<PortIndex>(port), out,
                              static_cast<PortIndex>(port)));
        }
        g.set_canonical_executor_routing_enabled(true);
        REQUIRE(g.prepare(kSr, kFrames));

        result = bake(g);
        REQUIRE(result.accepted);
        REQUIRE(result.processor);
        REQUIRE(result.reason == LowerRejectReason::None);

        pulp::format::PrepareContext pc;
        pc.sample_rate = kSr;
        pc.max_buffer_size = kFrames;
        pc.input_channels = 2;
        pc.output_channels = 2;
        result.processor->prepare(pc);
    }

    BakedGraphProcessor& baked() {
        return *static_cast<BakedGraphProcessor*>(result.processor.get());
    }
};

std::vector<std::vector<float>> run_block(pulp::format::Processor& proc,
                                          const std::vector<std::vector<float>>& in_channels) {
    std::vector<const float*> in_ptrs(in_channels.size());
    for (std::size_t c = 0; c < in_channels.size(); ++c) in_ptrs[c] = in_channels[c].data();

    std::vector<std::vector<float>> output(2,
                                           std::vector<float>(static_cast<std::size_t>(kFrames), 0.0f));
    std::vector<float*> out_ptrs{output[0].data(), output[1].data()};

    pulp::audio::BufferView<const float> in_view(in_ptrs.data(), 2,
                                                 static_cast<std::uint32_t>(kFrames));
    pulp::audio::BufferView<float> out_view(out_ptrs.data(), 2,
                                            static_cast<std::uint32_t>(kFrames));
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = kSr;
    ctx.num_samples = kFrames;
    proc.process(out_view, in_view, midi_in, midi_out, ctx);
    return output;
}

pulp::state::ParameterEvent immediate(pulp::state::ParamID id, float value,
                                      std::int32_t offset = 0) {
    return {id, offset, value, /*ramp_duration_sample_frames=*/0};
}

std::vector<float> sine(float amp) {
    std::vector<float> v(static_cast<std::size_t>(kFrames), 0.0f);
    for (int k = 0; k < kFrames; ++k)
        v[static_cast<std::size_t>(k)] =
            amp * static_cast<float>(std::sin(2.0 * M_PI * kToneHz * k / kSr));
    return v;
}

std::vector<float> silence() {
    return std::vector<float>(static_cast<std::size_t>(kFrames), 0.0f);
}

std::vector<std::vector<float>> settle(pulp::format::Processor& proc,
                                       const std::vector<std::vector<float>>& feed,
                                       int blocks = 64) {
    std::vector<std::vector<float>> out;
    for (int b = 0; b < blocks; ++b) out = run_block(proc, feed);
    return out;
}

float peak(const std::vector<float>& b) {
    float m = 0.0f;
    for (float v : b) m = std::max(m, std::fabs(v));
    return m;
}

/// Gain, in dB, of a settled block relative to its input amplitude.
double gain_db(const std::vector<float>& out, float in_amp) {
    return 20.0 * std::log10(std::max(static_cast<double>(peak(out)), 1e-12) / in_amp);
}

}  // namespace

TEST_CASE("Forge dynamics: the compressor bakes and runs",
          "[host][baked][forge][forge-dynamics]") {
    BakedFixture fx(dyn::make_feedforward_compressor_node());
    const auto tone = sine(0.5f);
    const auto out = settle(fx.baked(), {tone, tone});
    for (int ch = 0; ch < 2; ++ch)
        for (float v : out[static_cast<std::size_t>(ch)]) REQUIRE(std::isfinite(v));
    REQUIRE(peak(out[0]) > 0.0f);
}

TEST_CASE("Forge dynamics: injecting threshold deepens gain reduction",
          "[host][baked][param-injection][forge][forge-dynamics]") {
    BakedFixture fx(dyn::make_feedforward_compressor_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const float amp = 0.5f;
    const auto tone = sine(amp);

    REQUIRE(inj.inject(immediate(dyn::kAutoMakeup, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kMakeupDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kRatio, 8.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kKneeDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kAttackMs, 1.0f)) == InjectStatus::Ok);

    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, 0.0f)) == InjectStatus::Ok);
    const double high = gain_db(settle(fx.baked(), {tone, tone})[0], amp);

    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, -40.0f)) == InjectStatus::Ok);
    const double low = gain_db(settle(fx.baked(), {tone, tone})[0], amp);

    REQUIRE_THAT(high, WithinAbs(0.0, 0.2));  // above threshold: untouched
    REQUIRE(low < high - 6.0);                // well below: substantial reduction
}

TEST_CASE("Forge dynamics: injecting ratio deepens gain reduction",
          "[host][baked][param-injection][forge][forge-dynamics]") {
    BakedFixture fx(dyn::make_feedforward_compressor_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const float amp = 0.5f;
    const auto tone = sine(amp);

    REQUIRE(inj.inject(immediate(dyn::kAutoMakeup, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, -30.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kKneeDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kAttackMs, 1.0f)) == InjectStatus::Ok);

    double previous = 1.0;
    for (float ratio : {1.0f, 2.0f, 8.0f, 100.0f}) {
        REQUIRE(inj.inject(immediate(dyn::kRatio, ratio)) == InjectStatus::Ok);
        const double g = gain_db(settle(fx.baked(), {tone, tone})[0], amp);
        REQUIRE(g < previous);
        previous = g;
    }
}

TEST_CASE("Forge dynamics: the stereo link survives the graph",
          "[host][baked][param-injection][forge][forge-dynamics]") {
    // The true-stereo wiring assertion. A hard-panned transient must pull BOTH
    // channels down when linked, and only its own channel when unlinked. A node
    // wired dual-mono would pass every other test in this file and fail here.
    BakedFixture fx(dyn::make_feedforward_compressor_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);

    REQUIRE(inj.inject(immediate(dyn::kAutoMakeup, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, -30.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kRatio, 8.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kKneeDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kAttackMs, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kProgramDependent, 0.0f)) == InjectStatus::Ok);
    // A fast release, because the two halves of this test share one fixture:
    // the unlinked measurement follows the linked one, and at the default
    // 120 ms release the right channel's detector is still recovering from the
    // linked pass 170 ms later — which reads as "the link is stuck on".
    REQUIRE(inj.inject(immediate(dyn::kReleaseMs,
                                 static_cast<float>(
                                     pulp::signal::FeedforwardCompressor::kReleaseMsMin))) ==
            InjectStatus::Ok);

    // A quiet tone on the right so there is something to attenuate, a loud one
    // on the left to drive the detector.
    const float quiet = 0.02f;  // −34 dBFS: on its own, below threshold
    const auto loud_left = sine(0.9f);
    const auto quiet_right = sine(quiet);

    REQUIRE(inj.inject(immediate(dyn::kStereoLink, 1.0f)) == InjectStatus::Ok);
    const double linked_right = gain_db(settle(fx.baked(), {loud_left, quiet_right})[1], quiet);

    REQUIRE(inj.inject(immediate(dyn::kStereoLink, 0.0f)) == InjectStatus::Ok);
    const double unlinked_right = gain_db(settle(fx.baked(), {loud_left, quiet_right})[1], quiet);

    REQUIRE_THAT(unlinked_right, WithinAbs(0.0, 0.2));  // its own channel is quiet
    REQUIRE(linked_right < unlinked_right - 6.0);       // the loud channel pulled it down
}

TEST_CASE("Forge dynamics: injecting lookahead changes reported latency",
          "[host][baked][param-injection][forge][forge-dynamics]") {
    // The node's ceiling is what `prepare()` sized the ring for, so a value
    // beyond it clamps rather than allocating.
    BakedFixture fx(dyn::make_feedforward_compressor_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);

    REQUIRE(inj.inject(immediate(dyn::kAutoMakeup, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kRatio, 1.0f)) == InjectStatus::Ok);

    // At ratio 1 with no makeup the node is a pure delay, so an impulse's
    // arrival index IS the latency.
    REQUIRE(inj.inject(immediate(dyn::kLookaheadMs, 2.0f)) == InjectStatus::Ok);
    settle(fx.baked(), {silence(), silence()}, 4);

    auto impulse = silence();
    impulse[0] = 0.5f;
    const auto first = run_block(fx.baked(), {impulse, impulse});
    const auto second = run_block(fx.baked(), {silence(), silence()});

    const int expected = static_cast<int>(std::llround(2.0 * kSr / 1000.0));
    std::vector<float> joined = first[0];
    joined.insert(joined.end(), second[0].begin(), second[0].end());
    int index = -1;
    float best = 0.0f;
    for (int i = 0; i < static_cast<int>(joined.size()); ++i) {
        if (std::fabs(joined[static_cast<std::size_t>(i)]) > best) {
            best = std::fabs(joined[static_cast<std::size_t>(i)]);
            index = i;
        }
    }
    REQUIRE(index == expected);
}

TEST_CASE("Forge dynamics: auto-makeup restores level through the graph",
          "[host][baked][param-injection][forge][forge-dynamics]") {
    BakedFixture fx(dyn::make_feedforward_compressor_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const float amp = 1.0f;
    const auto tone = sine(amp);

    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, -18.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kRatio, 4.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kKneeDb, 6.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kAttackMs, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kReleaseMs, 50.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kProgramDependent, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kAutoMakeup, 1.0f)) == InjectStatus::Ok);

    REQUIRE_THAT(gain_db(settle(fx.baked(), {tone, tone}, 128)[0], amp), WithinAbs(0.0, 0.3));
}

TEST_CASE("Forge dynamics: the registry's worst-case gain matches the makeup ceiling",
          "[host][baked][forge][forge-dynamics]") {
    // Series law 8: a tested invariant, not an estimate. Feedforward topology,
    // so the gain computer can only reduce and the bound is exactly the makeup
    // ceiling.
    const double expected =
        std::pow(10.0, pulp::signal::FeedforwardCompressor::kMakeupDbMax / 20.0);
    REQUIRE_THAT(static_cast<double>(dyn::feedforward_compressor_worst_case_gain()),
                 WithinAbs(expected, 1e-4));
}

TEST_CASE("Forge dynamics: the node's process path allocates nothing",
          "[host][baked][forge][forge-dynamics][rt-safety]") {
    BakedFixture fx(dyn::make_feedforward_compressor_node());
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const auto tone = sine(0.5f);
    settle(fx.baked(), {tone, tone}, 8);

    // Buffers and views built outside the probe; the shared `run_block` helper
    // allocates its own output vectors, which would be reported as the node's.
    const float* in_ptrs[2] = {tone.data(), tone.data()};
    std::vector<float> left(static_cast<std::size_t>(kFrames), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(kFrames), 0.0f);
    float* out_ptrs[2] = {left.data(), right.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, static_cast<std::uint32_t>(kFrames));
    pulp::audio::BufferView<float> out_view(out_ptrs, 2, static_cast<std::uint32_t>(kFrames));
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = kSr;
    ctx.num_samples = kFrames;

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(dyn::kThresholdDb, static_cast<float>(-40 + b)));
        inj.inject(immediate(dyn::kLookaheadMs, 0.25f * static_cast<float>(b % 40)));
        inj.inject(immediate(dyn::kDetectorMode, (b % 2) ? 1.0f : 0.0f));
        fx.baked().process(out_view, in_view, midi_in, midi_out, ctx);
    }
    REQUIRE(probe.allocation_count() == 0);
}
