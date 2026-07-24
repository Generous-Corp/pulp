// Multirate FDN reverb — bake-layer catalog node tests.
//
// The engine's own DSP claims are proven in test_fdn_reverb.cpp. What is proven
// here is everything the CATALOG adds: that all five modes bake and register,
// that a control-thread injection of each of the twelve knobs reaches the baked
// node's audio over the real production path (bake -> BakedGraphProcessor ->
// claim_param_injection -> ParamInjector -> routed executor -> ParamCursor)
// without re-baking, that the node is genuinely true-stereo rather than two
// mono halves, and that none of it allocates on the audio thread — including
// the one operation most likely to: a live tank-rate change, which re-derives
// every delay length, filter coefficient and resampler ratio in the engine.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_fdn_reverb_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp::host;
namespace catalog = pulp::host::forge_fdn;
namespace fdn = pulp::signal::fdn;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 256;

// A baked stereo graph: in(2) -> reverb -> out(2). The node's two ports are one
// logical stereo wire, so both are wired from the same input node.
struct BakedReverb {
    SignalGraph graph;
    LowerResult result;
    NodeId node = 0;

    explicit BakedReverb(fdn::Mode mode) {
        const auto type = catalog::make_fdn_reverb_node(mode);
        REQUIRE(graph.register_custom_node_type(type));
        const auto in = graph.add_input_node(2, "In");
        node = graph.add_custom_node(type.type_id, 1, "Reverb");
        const auto out = graph.add_output_node(2, "Out");
        for (PortIndex port = 0; port < 2; ++port) {
            REQUIRE(graph.connect(in, port, node, port));
            REQUIRE(graph.connect(node, port, out, port));
        }
        graph.set_canonical_executor_routing_enabled(true);
        REQUIRE(graph.prepare(kSr, kFrames));

        result = bake(graph);
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

struct Block {
    std::vector<float> left = std::vector<float>(static_cast<std::size_t>(kFrames), 0.0f);
    std::vector<float> right = std::vector<float>(static_cast<std::size_t>(kFrames), 0.0f);
};

Block run_block(pulp::format::Processor& proc, const Block& in) {
    const float* in_ptrs[2] = {in.left.data(), in.right.data()};
    Block out;
    float* out_ptrs[2] = {out.left.data(), out.right.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2,
                                                 static_cast<std::uint32_t>(kFrames));
    pulp::audio::BufferView<float> out_view(out_ptrs, 2, static_cast<std::uint32_t>(kFrames));
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = kSr;
    ctx.num_samples = kFrames;
    proc.process(out_view, in_view, midi_in, midi_out, ctx);
    return out;
}

Block impulse_block(float amplitude = 1.0f, bool left_only = false) {
    Block b;
    b.left[0] = amplitude;
    if (!left_only) b.right[0] = amplitude;
    return b;
}

pulp::state::ParameterEvent immediate(pulp::state::ParamID id, float value) {
    pulp::state::ParameterEvent e;
    e.param_id = id;
    e.value = value;
    e.sample_offset = 0;
    return e;
}

// Render `blocks` blocks, feeding the impulse in the first, and return the
// accumulated wet output.
std::vector<float> tail(pulp::format::Processor& proc, int blocks,
                        float amplitude = 1.0f) {
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(blocks * kFrames));
    Block in = impulse_block(amplitude);
    for (int b = 0; b < blocks; ++b) {
        const Block rendered = run_block(proc, in);
        out.insert(out.end(), rendered.left.begin(), rendered.left.end());
        in = Block{};
    }
    return out;
}

// Render with a loud sustained broadband input. An impulse leaves the tank at a
// level where the loop saturator is indistinguishable from a wire — the whole
// point of a 1-Lipschitz curve is that it does nothing to small signals — so a
// sweep that has to see EVERY knob move the audio needs a stimulus that
// actually reaches the interesting part of each curve.
std::vector<float> excited(pulp::format::Processor& proc, int blocks) {
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(blocks * kFrames));
    std::uint32_t state = 0xC0FFEEu;
    Block in;
    for (int b = 0; b < blocks; ++b) {
        for (int k = 0; k < kFrames; ++k) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const auto v = static_cast<float>(
                (static_cast<double>(state >> 8) * (1.0 / 16777216.0) * 2.0 - 1.0) * 0.8);
            in.left[static_cast<std::size_t>(k)] = v;
            in.right[static_cast<std::size_t>(k)] = -v;
        }
        const Block rendered = run_block(proc, in);
        out.insert(out.end(), rendered.left.begin(), rendered.left.end());
    }
    return out;
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    to = std::min(to, x.size());
    if (to <= from) return 0.0;
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i)
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    return std::sqrt(sum / static_cast<double>(to - from));
}

double peak(const std::vector<float>& x) {
    double p = 0.0;
    for (float v : x) p = std::max(p, std::abs(static_cast<double>(v)));
    return p;
}

constexpr std::array<fdn::Mode, 5> kAllModes = {fdn::Mode::room, fdn::Mode::hall,
                                                fdn::Mode::galaxy, fdn::Mode::shimmer,
                                                fdn::Mode::lofi};

}  // namespace

TEST_CASE("fdn reverb catalog registers five modes of one engine",
          "[fdn][catalog][reverb]") {
    std::vector<std::string> ids;
    for (fdn::Mode mode : kAllModes) {
        const auto type = catalog::make_fdn_reverb_node(mode);
        INFO("mode " << fdn::mode_config(mode).name);
        // The type id is what a baked artifact stores, so the factory and the
        // id-lookup must agree exactly — a mistyped pairing would silently bake
        // the wrong reverb.
        REQUIRE(std::string(type.type_id) == catalog::type_id_for(mode));
        REQUIRE(type.lowerable);
        REQUIRE(type.num_input_ports == 2);
        REQUIRE(type.num_output_ports == 2);
        REQUIRE(type.baked_params.size() == static_cast<std::size_t>(fdn::kNumParams));

        // Every declared param must carry the engine's own range and the mode's
        // stamped default, walked exhaustively rather than spot-checked.
        for (int p = 0; p < fdn::kNumParams; ++p) {
            const auto param = static_cast<fdn::Param>(p);
            const fdn::ParamSpec& spec = fdn::kParamSpecs[static_cast<std::size_t>(p)];
            const auto& declared = type.baked_params[static_cast<std::size_t>(p)];
            INFO("parameter " << spec.id);
            REQUIRE(declared.id == catalog::param_id_for(param));
            REQUIRE(catalog::engine_param_for(declared.id) == param);
            REQUIRE(declared.min_value == static_cast<float>(spec.min));
            REQUIRE(declared.max_value == static_cast<float>(spec.max));
            REQUIRE(declared.default_value ==
                    static_cast<float>(fdn::mode_default(mode, param)));
        }
        ids.push_back(type.type_id);
    }
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::unique(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("fdn reverb catalog bakes and produces a wet tail in every mode",
          "[fdn][catalog][reverb]") {
    for (fdn::Mode mode : kAllModes) {
        BakedReverb fixture(mode);
        const auto rendered = tail(*fixture.result.processor, 40);
        INFO("mode " << fdn::mode_config(mode).name);
        for (float v : rendered) REQUIRE(std::isfinite(v));
        REQUIRE(peak(rendered) > 0.0);
        REQUIRE(peak(rendered) < static_cast<double>(catalog::kWorstCaseGain));
        // Wet only: the dry impulse must NOT appear at the output, or the node
        // would be double-counting dry against the graph's dry/wet mixer.
        REQUIRE(std::abs(static_cast<double>(rendered[0])) < 0.1);
    }
}

TEST_CASE("fdn reverb catalog injects decay on the baked graph without re-baking",
          "[fdn][catalog][reverb][injection]") {
    auto tail_energy = [](float decay_seconds) {
        BakedReverb fixture(fdn::Mode::hall);
        auto injector = fixture.baked().claim_param_injection(fixture.node);
        REQUIRE(injector.inject(immediate(catalog::kDecay, decay_seconds)) ==
                InjectStatus::Ok);
        REQUIRE(injector.inject(immediate(catalog::kDampHi, 0.0f)) == InjectStatus::Ok);
        const auto rendered = tail(*fixture.result.processor, 400);
        const auto late = static_cast<std::size_t>(rendered.size() * 3 / 4);
        return rms(rendered, late, rendered.size());
    };

    const double shortest = tail_energy(0.3f);
    const double longest = tail_energy(20.0f);
    INFO("late-tail RMS: decay 0.3 s = " << shortest << ", decay 20 s = " << longest);
    // A long decay must leave measurably more energy late in the render than a
    // short one. This is the whole point of the injection path: the knob turn
    // reached the DSP.
    REQUIRE(longest > shortest * 10.0);
}

TEST_CASE("fdn reverb catalog injects every declared parameter",
          "[fdn][catalog][reverb][injection]") {
    // Exhaustive rather than representative: a param that is declared but not
    // wired reads as a working knob to a host and does nothing, which is the
    // exact failure the catalog registry exists to make impossible.
    for (int p = 0; p < fdn::kNumParams; ++p) {
        const auto param = static_cast<fdn::Param>(p);
        const fdn::ParamSpec& spec = fdn::kParamSpecs[static_cast<std::size_t>(p)];

        auto render_at = [&](double value) {
            BakedReverb fixture(fdn::Mode::hall);
            auto injector = fixture.baked().claim_param_injection(fixture.node);
            REQUIRE(injector.inject(immediate(catalog::param_id_for(param),
                                              static_cast<float>(value))) ==
                    InjectStatus::Ok);
            return excited(*fixture.result.processor, 120);
        };

        const auto low = render_at(spec.min);
        const auto high = render_at(spec.max);
        double difference = 0.0;
        for (std::size_t i = 0; i < low.size(); ++i)
            difference = std::max(difference,
                                  std::abs(static_cast<double>(low[i] - high[i])));
        INFO("parameter " << spec.id << " over [" << spec.min << ", " << spec.max << "]");
        REQUIRE(difference > 1e-6);
        for (std::size_t i = 0; i < low.size(); ++i) {
            REQUIRE(std::isfinite(low[i]));
            REQUIRE(std::isfinite(high[i]));
        }
    }
}

TEST_CASE("fdn reverb catalog node is true stereo", "[fdn][catalog][reverb]") {
    BakedReverb fixture(fdn::Mode::hall);
    // A left-only impulse must produce energy on BOTH outputs, because the two
    // ports are one logical wire through a shared tank. Two mono instances
    // would leave the right rail silent.
    Block in = impulse_block(1.0f, /*left_only=*/true);
    double left_energy = 0.0;
    double right_energy = 0.0;
    for (int b = 0; b < 60; ++b) {
        const Block out = run_block(*fixture.result.processor, in);
        for (int k = 0; k < kFrames; ++k) {
            left_energy += std::abs(static_cast<double>(out.left[static_cast<std::size_t>(k)]));
            right_energy +=
                std::abs(static_cast<double>(out.right[static_cast<std::size_t>(k)]));
        }
        in = Block{};
    }
    INFO("left energy " << left_energy << ", right energy " << right_energy);
    REQUIRE(left_energy > 0.0);
    REQUIRE(right_energy > left_energy * 0.05);
}

TEST_CASE("fdn reverb catalog does not allocate on the audio thread",
          "[fdn][catalog][reverb][rt-safety]") {
    for (fdn::Mode mode : kAllModes) {
        BakedReverb fixture(mode);
        auto injector = fixture.baked().claim_param_injection(fixture.node);

        // Every buffer the measured window touches is allocated BEFORE the
        // probe opens, and the render is driven through pointers rather than by
        // returning blocks. The probe cannot tell the harness's allocations
        // from the engine's, so a convenience helper that builds a vector per
        // call would report the test's own 1 KiB blocks as an RT-safety
        // failure — which is exactly what it did before this was hoisted.
        Block silence;
        Block scratch;
        const float* in_ptrs[2] = {silence.left.data(), silence.right.data()};
        float* out_ptrs[2] = {scratch.left.data(), scratch.right.data()};
        pulp::audio::BufferView<const float> in_view(in_ptrs, 2,
                                                     static_cast<std::uint32_t>(kFrames));
        pulp::audio::BufferView<float> out_view(out_ptrs, 2,
                                                static_cast<std::uint32_t>(kFrames));
        pulp::midi::MidiBuffer midi_in;
        pulp::midi::MidiBuffer midi_out;
        pulp::format::ProcessContext ctx;
        ctx.sample_rate = kSr;
        ctx.num_samples = kFrames;
        auto render_once = [&] {
            fixture.result.processor->process(out_view, in_view, midi_in, midi_out, ctx);
        };

        // Warm up so first-call lazy paths are primed before the probe opens.
        run_block(*fixture.result.processor, impulse_block());
        render_once();

        {
            pulp::test::RtAllocationProbe probe;
            injector.inject(immediate(catalog::kDecay, 12.0f));
            render_once();
            injector.inject(immediate(catalog::kSize, 0.9f));
            render_once();
            // The dangerous one: a live tank-rate change re-derives every delay
            // length, damping coefficient, EQ/flux design and resampler ratio.
            // If anything in that path allocated, this is where it would show.
            injector.inject(immediate(catalog::kTankRate, 0.0f));
            render_once();
            render_once();
            injector.inject(immediate(catalog::kTankRate, 7.0f));
            render_once();
            render_once();
            const auto count = probe.allocation_count();
            const auto bytes = probe.allocated_bytes();
            INFO("mode " << fdn::mode_config(mode).name);
            CHECK(count == 0);
            CHECK(bytes == 0);
        }
    }
}

TEST_CASE("fdn reverb catalog flushes cleanly on a live tank-rate change",
          "[fdn][catalog][reverb][rt-safety]") {
    BakedReverb fixture(fdn::Mode::hall);
    auto injector = fixture.baked().claim_param_injection(fixture.node);

    // Build a tail at the mode's own rate, then switch.
    Block in = impulse_block();
    double before = 0.0;
    for (int b = 0; b < 60; ++b) {
        const Block out = run_block(*fixture.result.processor, in);
        before = std::max(before, peak(out.left));
        in = Block{};
    }
    REQUIRE(before > 0.0);

    REQUIRE(injector.inject(immediate(catalog::kTankRate, 0.0f)) == InjectStatus::Ok);
    double after = 0.0;
    std::vector<float> post;
    for (int b = 0; b < 60; ++b) {
        const Block out = run_block(*fixture.result.processor, Block{});
        after = std::max(after, peak(out.left));
        post.insert(post.end(), out.left.begin(), out.left.end());
    }
    for (float v : post) REQUIRE(std::isfinite(v));
    INFO("peak before the switch " << before << ", after " << after);
    // A hard flush drops the running tail rather than reinterpreting it at the
    // new rate. The honest failure mode for "no crossfade" is a click, so what
    // must be ruled out is a SPIKE: the output may fall to silence, it must
    // never jump above what was already playing.
    REQUIRE(after <= before);
}
