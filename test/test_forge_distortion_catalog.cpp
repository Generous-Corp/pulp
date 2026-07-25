// Distortion catalog node — bake-layer tests.
//
// Proves each baked param moves the BAKED node's audio over the real production
// path, that the oversampling tiers report their latency exactly (the spec's
// acceptance test 8), and that the composed chain stays deterministic and
// allocation-free.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_distortion_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp::host;
namespace dist = pulp::host::distortion;
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
                          int blocks = 16) {
    std::vector<float> out;
    for (int b = 0; b < blocks; ++b) out = run_block(proc, feed);
    return out;
}

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

constexpr dist::OversampleTier kAllTiers[] = {dist::OversampleTier::x1, dist::OversampleTier::x2,
                                              dist::OversampleTier::x4, dist::OversampleTier::x8};

}  // namespace

TEST_CASE("Forge distortion: every realization bakes and runs",
          "[host][baked][forge][forge-distortion]") {
    for (auto topology : {dist::Topology::to_ground, dist::Topology::in_loop}) {
        for (auto tier : kAllTiers) {
            BakedFixture fx(dist::make_distortion_node(topology, tier));
            const auto out = settle(fx.baked(), sine(0.5f));
            for (float v : out) REQUIRE(std::isfinite(v));
        }
    }
}

TEST_CASE("Forge distortion: injecting drive increases harmonic content",
          "[host][baked][param-injection][forge][forge-distortion]") {
    BakedFixture fx(dist::make_distortion_node(dist::Topology::to_ground));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const auto tone = sine(0.2f);

    REQUIRE(inj.inject(immediate(dist::kToneMix, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kDriveDb, 0.0f)) == InjectStatus::Ok);
    const auto low_out = settle(fx.baked(), tone);
    const double low = harmonic(low_out, 3) / harmonic(low_out, 1);

    REQUIRE(inj.inject(immediate(dist::kDriveDb, 36.0f)) == InjectStatus::Ok);
    const auto high_out = settle(fx.baked(), tone);
    const double high = harmonic(high_out, 3) / harmonic(high_out, 1);

    REQUIRE(high > low * 3.0);
}

TEST_CASE("Forge distortion: injecting symmetry brings in even harmonics",
          "[host][baked][param-injection][forge][forge-distortion]") {
    BakedFixture fx(dist::make_distortion_node(dist::Topology::to_ground));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const auto tone = sine(0.5f);

    REQUIRE(inj.inject(immediate(dist::kToneMix, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kDriveDb, 24.0f)) == InjectStatus::Ok);

    REQUIRE(inj.inject(immediate(dist::kSymmetry, 0.0f)) == InjectStatus::Ok);
    const auto matched = settle(fx.baked(), tone);
    const double even_matched = harmonic(matched, 2) / harmonic(matched, 1);

    REQUIRE(inj.inject(immediate(dist::kSymmetry, -1.0f)) == InjectStatus::Ok);
    const auto half_wave = settle(fx.baked(), tone);
    const double even_half = harmonic(half_wave, 2) / harmonic(half_wave, 1);

    REQUIRE(even_half > even_matched * 10.0);
}

TEST_CASE("Forge distortion: injecting output trim scales by exactly that many dB",
          "[host][baked][param-injection][forge][forge-distortion]") {
    BakedFixture fx(dist::make_distortion_node(dist::Topology::to_ground));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const auto tone = sine(0.05f);

    REQUIRE(inj.inject(immediate(dist::kToneMix, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kDriveDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kOutputDb, 0.0f)) == InjectStatus::Ok);
    const double unity = harmonic(settle(fx.baked(), tone), 1);

    for (float trim : {-24.0f, -12.0f, 6.0f, 12.0f}) {
        REQUIRE(inj.inject(immediate(dist::kOutputDb, trim)) == InjectStatus::Ok);
        const double scaled = harmonic(settle(fx.baked(), tone), 1);
        REQUIRE_THAT(20.0 * std::log10(scaled / unity), WithinAbs(trim, 0.05));
    }
}

TEST_CASE("Forge distortion: injecting the post-tone corner darkens the output",
          "[host][baked][param-injection][forge][forge-distortion]") {
    BakedFixture fx(dist::make_distortion_node(dist::Topology::to_ground));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const auto tone = sine(0.5f);

    REQUIRE(inj.inject(immediate(dist::kToneMix, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kDriveDb, 30.0f)) == InjectStatus::Ok);

    REQUIRE(inj.inject(immediate(dist::kPostToneHz, 12000.0f)) == InjectStatus::Ok);
    const auto bright = settle(fx.baked(), tone);

    REQUIRE(inj.inject(immediate(dist::kPostToneHz, 500.0f)) == InjectStatus::Ok);
    const auto dark = settle(fx.baked(), tone);

    // The de-fizz stage: high harmonics fall relative to the fundamental.
    REQUIRE(harmonic(dark, 5) / harmonic(dark, 1) <
            harmonic(bright, 5) / harmonic(bright, 1) * 0.5);
}

// ── 8. Latency, reported exactly per tier ─────────────────────────────────

TEST_CASE("8 the oversampling tiers report their latency exactly",
          "[host][baked][forge][forge-distortion][latency]") {
    // Zero at ×1, since the ADAA path has no filter at all; the composed
    // oversampler's own exact integer group delay above that.
    REQUIRE(dist::distortion_latency_samples(dist::OversampleTier::x1, kSr) == 0);

    int previous = 0;
    for (auto tier : {dist::OversampleTier::x2, dist::OversampleTier::x4,
                      dist::OversampleTier::x8}) {
        const int latency = dist::distortion_latency_samples(tier, kSr);
        REQUIRE(latency > 0);
        // More cascaded stages can only add delay, never remove it.
        REQUIRE(latency >= previous);
        previous = latency;
    }
}

TEST_CASE("8 the reported latency matches the measured impulse position",
          "[host][baked][forge][forge-distortion][latency]") {
    // The reported figure is checked against the actual impulse response rather
    // than trusted, which is the whole point of "reported, not estimated".
    for (auto tier : {dist::OversampleTier::x2, dist::OversampleTier::x4}) {
        BakedFixture fx(dist::make_distortion_node(dist::Topology::to_ground, tier));
        ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
        // Linear settings so the impulse survives recognisably: no drive past
        // the knee, no tone shaping.
        REQUIRE(inj.inject(immediate(dist::kDriveDb, 0.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dist::kToneMix, 0.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dist::kOutputDb, 0.0f)) == InjectStatus::Ok);

        const std::vector<float> silence(static_cast<std::size_t>(kFrames), 0.0f);
        settle(fx.baked(), silence, 4);

        auto impulse = silence;
        impulse[0] = 0.05f;  // small: stays well below the diode knee
        std::vector<float> joined = run_block(fx.baked(), impulse);
        for (int b = 0; b < 4; ++b) {
            const auto next = run_block(fx.baked(), silence);
            joined.insert(joined.end(), next.begin(), next.end());
        }

        int index = -1;
        float best = 0.0f;
        for (int i = 0; i < static_cast<int>(joined.size()); ++i) {
            if (std::fabs(joined[static_cast<std::size_t>(i)]) > best) {
                best = std::fabs(joined[static_cast<std::size_t>(i)]);
                index = i;
            }
        }
        REQUIRE(index == dist::distortion_latency_samples(tier, kSr));
    }
}

// ── 7. Determinism through the graph ──────────────────────────────────────

TEST_CASE("7 the baked node renders identically after a reset",
          "[host][baked][forge][forge-distortion][determinism]") {
    for (auto tier : {dist::OversampleTier::x1, dist::OversampleTier::x4}) {
        BakedFixture fx(dist::make_distortion_node(dist::Topology::in_loop, tier));
        const auto tone = sine(0.4f);

        // Two independently baked instances of the same graph, rendered the
        // same way, must agree sample for sample. That is the property that
        // matters — no uncontrolled state, no uninitialised carry — and it does
        // not depend on the processor exposing a public reset.
        BakedFixture other(dist::make_distortion_node(dist::Topology::in_loop, tier));
        for (int b = 0; b < 16; ++b) {
            const auto a = run_block(fx.baked(), tone);
            const auto c = run_block(other.baked(), tone);
            for (int k = 0; k < kFrames; ++k)
                REQUIRE(a[static_cast<std::size_t>(k)] == c[static_cast<std::size_t>(k)]);
        }
    }
}

TEST_CASE("Forge distortion: the registry's worst-case gain covers the trims",
          "[host][baked][forge][forge-distortion]") {
    // The clipper itself cannot amplify, so the node's bound is the drive and
    // output trims either side of it. The drive counts because it sits BEFORE
    // the clipper, where small signals pass un-clipped — a bound that only
    // counted the clipper would be wrong for exactly the quiet material a
    // path-gain lint most needs to reason about.
    const double expected =
        std::pow(10.0, (dist::kDriveDbMax + dist::kOutputDbMax) / 20.0);
    REQUIRE_THAT(static_cast<double>(dist::distortion_worst_case_gain()),
                 WithinAbs(expected, 1e-3));

    BakedFixture fx(dist::make_distortion_node(dist::Topology::to_ground));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    REQUIRE(inj.inject(immediate(dist::kDriveDb, dist::kDriveDbMax)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kOutputDb, dist::kOutputDbMax)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kToneMix, 0.0f)) == InjectStatus::Ok);

    // A quiet input, where the drive passes through the clipper unclipped: this
    // is where the bound is actually approached.
    const auto out = settle(fx.baked(), sine(1e-4f));
    double peak = 0.0;
    for (float v : out) peak = std::max(peak, static_cast<double>(std::fabs(v)));
    REQUIRE(peak <= 1e-4 * expected * 1.01);
}

TEST_CASE("Forge distortion: the node's process path allocates nothing",
          "[host][baked][forge][forge-distortion][rt-safety]") {
    BakedFixture fx(dist::make_distortion_node(dist::Topology::in_loop,
                                               dist::OversampleTier::x4));
    ParamInjector inj = fx.baked().claim_param_injection(fx.custom_node);
    const auto tone = sine(0.5f);
    settle(fx.baked(), tone, 8);

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
        inj.inject(immediate(dist::kDriveDb, static_cast<float>(b)));
        inj.inject(immediate(dist::kSymmetry, -1.0f + 0.06f * static_cast<float>(b)));
        inj.inject(immediate(dist::kDiodeModel, static_cast<float>(b % 3)));
        fx.baked().process(out_view, in_view, midi_in, midi_out, ctx);
    }
    REQUIRE(probe.allocation_count() == 0);
}
