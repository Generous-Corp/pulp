// Saturation toolkit — bake-layer catalog node tests.
//
// The sibling of test_forge_lofi_catalog.cpp for the saturator node: it proves
// that a CONTROL-THREAD injection of each baked param changes the BAKED node's
// audio over the real production path (bake() → BakedGraphProcessor →
// claim_param_injection → ParamInjector → routed executor → ParamCursor),
// rather than only over the DSP class the unit suite already covers.
//
// What each case pins down:
//
//   * drive_db — more drive means more harmonic energy, and at the drive floor
//                the node is transparent (the registry's unity-gain claim,
//                measured through the graph rather than through the class).
//   * bias     — asymmetry appears as second-harmonic energy that grows with
//                |bias|, and zero input still produces exactly zero output.
//   * mix      — a dry/wet crossfade between the shaped and unshaped signal.
//   * trim     — an exact dB scaling.
//   * tone     — the pre/de pair cancels, so the node stays flat at floor drive.
//
// Plus every registered realization instantiating and running, an RT-allocation
// probe over the node's process path, and the worst-case-gain helper agreeing
// with the DSP's own bound (series law 8: the registry entry must cite a number
// the test suite asserts).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_saturator_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp::host;
namespace sat = pulp::host::saturator;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;
constexpr double kToneHz = 750.0;  // 64 samples per period: a whole number of
                                   // cycles fits a 128-frame block exactly.

struct BakedFixture {
    SignalGraph g;
    LowerResult result;
    NodeId custom_node = 0;

    explicit BakedFixture(const CustomNodeType& type) {
        REQUIRE(g.register_custom_node_type(type));
        const auto in = g.add_input_node(1, "In");
        custom_node = g.add_custom_node(type.type_id, 1, "Node");
        const auto out = g.add_output_node(1, "Out");
        REQUIRE(g.connect(in, 0, custom_node, 0));
        REQUIRE(g.connect(custom_node, 0, out, 0));
        g.set_canonical_executor_routing_enabled(true);
        REQUIRE(g.prepare(kSr, kFrames));

        result = bake(g);
        REQUIRE(result.accepted);
        REQUIRE(result.processor);
        REQUIRE(result.reason == LowerRejectReason::None);

        pulp::format::PrepareContext pc;
        pc.sample_rate = kSr;
        pc.max_buffer_size = kFrames;
        pc.input_channels = 1;
        pc.output_channels = 1;
        result.processor->prepare(pc);
    }

    BakedGraphProcessor& baked() {
        return *static_cast<BakedGraphProcessor*>(result.processor.get());
    }
};

std::vector<float> run_block(pulp::format::Processor& proc, const std::vector<float>& mono) {
    const float* in_ptr = mono.data();
    std::vector<float> output(static_cast<std::size_t>(kFrames), 0.0f);
    float* out_ptr = output.data();

    pulp::audio::BufferView<const float> in_view(&in_ptr, 1, static_cast<std::uint32_t>(kFrames));
    pulp::audio::BufferView<float> out_view(&out_ptr, 1, static_cast<std::uint32_t>(kFrames));
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

std::vector<float> settle(pulp::format::Processor& proc, const std::vector<float>& feed,
                          int blocks = 8) {
    std::vector<float> out;
    for (int b = 0; b < blocks; ++b) out = run_block(proc, feed);
    return out;
}

/// Magnitude of harmonic `k` of the block's fundamental, by coherent DFT. The
/// tone is chosen so a whole number of cycles fills one block, which makes this
/// exact rather than approximate.
double harmonic(const std::vector<float>& x, int k) {
    const double w = 2.0 * M_PI * k * kToneHz / kSr;
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        re += x[n] * std::cos(w * static_cast<double>(n));
        im += x[n] * std::sin(w * static_cast<double>(n));
    }
    const double scale = 2.0 / static_cast<double>(x.size());
    return std::hypot(re * scale, im * scale);
}

double third_harmonic_ratio(const std::vector<float>& x) {
    return harmonic(x, 3) / harmonic(x, 1);
}

float peak(const std::vector<float>& b) {
    float m = 0.0f;
    for (float v : b) m = std::max(m, std::fabs(v));
    return m;
}

}  // namespace

TEST_CASE("Forge saturator: every realization bakes and runs",
          "[host][baked][forge][forge-saturator]") {
    for (auto shape : {sat::Shape::tanh_soft, sat::Shape::atan_soft, sat::Shape::cubic_soft,
                       sat::Shape::sinh_arc}) {
        BakedFixture fx(sat::make_saturator_node(shape));
        const auto out = settle(fx.baked(), sine(0.5f));
        REQUIRE(peak(out) > 0.0f);
        for (float v : out) REQUIRE(std::isfinite(v));
    }
}

TEST_CASE("Forge saturator: injecting drive changes harmonic content",
          "[host][baked][param-injection][forge][forge-saturator]") {
    BakedFixture fx(sat::make_saturator_node(sat::Shape::tanh_soft));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const auto tone = sine(0.5f);

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(sat::kDriveDb, 0.0f)) == InjectStatus::Ok);
    const double low = third_harmonic_ratio(settle(fx.baked(), tone));

    REQUIRE(inj.inject(immediate(sat::kDriveDb, 24.0f)) == InjectStatus::Ok);
    const double high = third_harmonic_ratio(settle(fx.baked(), tone));

    REQUIRE(high > low * 3.0);
}

TEST_CASE("Forge saturator: the drive floor is transparent through the graph",
          "[host][baked][param-injection][forge][forge-saturator]") {
    // The registry's unity-gain claim, measured over the production path rather
    // than over the DSP class.
    BakedFixture fx(sat::make_saturator_node(sat::Shape::tanh_soft));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(sat::kDriveDb, -12.0f)) == InjectStatus::Ok);

    const float amp = 0.01f;  // −40 dBFS
    const auto out = settle(fx.baked(), sine(amp));
    REQUIRE_THAT(20.0 * std::log10(harmonic(out, 1) / amp), WithinAbs(0.0, 0.05));
}

TEST_CASE("Forge saturator: injecting bias adds even harmonics without DC",
          "[host][baked][param-injection][forge][forge-saturator]") {
    BakedFixture fx(sat::make_saturator_node(sat::Shape::tanh_soft));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const auto tone = sine(0.5f);

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(sat::kDriveDb, 12.0f)) == InjectStatus::Ok);

    double previous = -1.0;
    for (float bias : {0.0f, 0.2f, 0.5f}) {
        REQUIRE(inj.inject(immediate(sat::kBias, bias)) == InjectStatus::Ok);
        const auto out = settle(fx.baked(), tone);
        const double h2 = harmonic(out, 2) / harmonic(out, 1);
        REQUIRE(h2 > previous);
        previous = h2;
    }

    // ...and silence still produces silence, at the largest bias.
    REQUIRE(inj.inject(immediate(sat::kBias, 1.0f)) == InjectStatus::Ok);
    const std::vector<float> silence(static_cast<std::size_t>(kFrames), 0.0f);
    const auto quiet = settle(fx.baked(), silence);
    for (float v : quiet) REQUIRE(std::abs(v) <= 1e-6f);
}

TEST_CASE("Forge saturator: injecting mix crossfades dry against wet",
          "[host][baked][param-injection][forge][forge-saturator]") {
    BakedFixture fx(sat::make_saturator_node(sat::Shape::cubic_soft));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const auto tone = sine(0.5f);

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(sat::kDriveDb, 24.0f)) == InjectStatus::Ok);

    REQUIRE(inj.inject(immediate(sat::kMix, 1.0f)) == InjectStatus::Ok);
    const double wet = third_harmonic_ratio(settle(fx.baked(), tone));

    REQUIRE(inj.inject(immediate(sat::kMix, 0.0f)) == InjectStatus::Ok);
    const auto dry = settle(fx.baked(), tone);
    // Fully dry is the input back, unshaped.
    REQUIRE(third_harmonic_ratio(dry) < wet * 0.05);
    for (int k = 0; k < kFrames; ++k)
        REQUIRE_THAT(static_cast<double>(dry[static_cast<std::size_t>(k)]),
                     WithinAbs(static_cast<double>(tone[static_cast<std::size_t>(k)]), 1e-5));
}

TEST_CASE("Forge saturator: injecting output trim scales by exactly that many dB",
          "[host][baked][param-injection][forge][forge-saturator]") {
    BakedFixture fx(sat::make_saturator_node(sat::Shape::tanh_soft));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const auto tone = sine(0.2f);

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(sat::kDriveDb, -12.0f)) == InjectStatus::Ok);

    REQUIRE(inj.inject(immediate(sat::kOutputTrimDb, 0.0f)) == InjectStatus::Ok);
    const double unity = harmonic(settle(fx.baked(), tone), 1);

    for (float trim : {-12.0f, -6.0f, 6.0f, 12.0f}) {
        REQUIRE(inj.inject(immediate(sat::kOutputTrimDb, trim)) == InjectStatus::Ok);
        const double scaled = harmonic(settle(fx.baked(), tone), 1);
        REQUIRE_THAT(20.0 * std::log10(scaled / unity), WithinAbs(trim, 0.05));
    }
}

TEST_CASE("Forge saturator: the tone pair cancels through the graph",
          "[host][baked][param-injection][forge][forge-saturator]") {
    BakedFixture fx(sat::make_saturator_node(sat::Shape::tanh_soft));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);

    REQUIRE(inj.inject(immediate(sat::kDriveDb, -12.0f)) == InjectStatus::Ok);
    const float amp = 0.01f;

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 0.0f)) == InjectStatus::Ok);
    const double flat = harmonic(settle(fx.baked(), sine(amp)), 1);

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 3000.0f)) == InjectStatus::Ok);
    const double bracketed = harmonic(settle(fx.baked(), sine(amp), 32), 1);

    REQUIRE_THAT(20.0 * std::log10(bracketed / flat), WithinAbs(0.0, 0.2));
}

TEST_CASE("Forge saturator: the registry's worst-case gain matches the DSP bound",
          "[host][baked][forge][forge-saturator]") {
    // Series law 8: the registry constant must cite an invariant the module's
    // own suite asserts, never an estimate. This is the two agreeing.
    for (auto shape : {sat::Shape::tanh_soft, sat::Shape::atan_soft, sat::Shape::cubic_soft,
                       sat::Shape::sinh_arc}) {
        pulp::signal::Saturator probe;
        probe.set_shape(shape);
        probe.set_bias(1.0);
        REQUIRE_THAT(static_cast<double>(sat::saturator_worst_case_gain(shape)),
                     WithinAbs(probe.worst_case_gain(), 1e-5));
        // Unbiased it is exactly 1.0 for every shape.
        probe.set_bias(0.0);
        REQUIRE_THAT(probe.worst_case_gain(), WithinAbs(1.0, 1e-12));
    }
}

TEST_CASE("Forge saturator: the node's process path allocates nothing",
          "[host][baked][forge][forge-saturator][rt-safety]") {
    BakedFixture fx(sat::make_saturator_node(sat::Shape::tanh_soft));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const auto tone = sine(0.5f);
    settle(fx.baked(), tone);  // warm every lazily-touched path first

    // Buffers and views are built OUTSIDE the probe. The `run_block` helper
    // used everywhere else constructs its own output vector, which is fine for
    // a behavioural test and fatal for this one — it would report one
    // allocation per block and blame the node for the harness.
    const float* in_ptr = tone.data();
    std::vector<float> output(static_cast<std::size_t>(kFrames), 0.0f);
    float* out_ptr = output.data();
    pulp::audio::BufferView<const float> in_view(&in_ptr, 1, static_cast<std::uint32_t>(kFrames));
    pulp::audio::BufferView<float> out_view(&out_ptr, 1, static_cast<std::uint32_t>(kFrames));
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = kSr;
    ctx.num_samples = kFrames;

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(sat::kDriveDb, static_cast<float>(-12 + b)));
        inj.inject(immediate(sat::kBias, 0.02f * static_cast<float>(b)));
        fx.baked().process(out_view, in_view, midi_in, midi_out, ctx);
    }
    REQUIRE(probe.allocation_count() == 0);
}
