#include "../examples/g10-routed-consumer/asset_morph_oracle.hpp"
#include "harness/rt_allocation_probe.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <type_traits>

TEST_CASE("Public spectral lowering exposes no callable constructor",
          "[host][spectral-ports][spectral-closed-catalog]") {
    using namespace pulp::host;
    using Callback = std::function<pulp::format::GraphRuntimeNodeBinding(NodeId)>;
    using Resolver = decltype(ExecutorSnapshotBinders{}.spectral_for);
    REQUIRE((std::is_constructible_v<Callback, Callback>));
    REQUIRE(std::is_default_constructible_v<Resolver>);
    REQUIRE_FALSE((std::is_constructible_v<Resolver, Callback>));
    REQUIRE_FALSE((std::is_assignable_v<Resolver&, Callback>));
    REQUIRE_FALSE(bool(Resolver{}));
}

namespace {
std::vector<float> render(int partition, float gain) {
    using namespace pulp::host;
    auto identity = SpectralIdentity::make(256, 63, 1, 48000);
    REQUIRE(identity);
    auto a = QualifiedSpectralStage::identity_v1(*identity.value);
    std::vector<float> mask(129, gain);
    auto b = QualifiedSpectralStage::real_mask_v1(*identity.value, mask);
    SignalGraph graph;
    graph.set_parallel_routing_enabled(true);
    graph.set_anticipation_enabled(true);
    const auto in = graph.add_input_node(1);
    const auto out = graph.add_output_node(1);
    auto analysis = graph.add_spectral_analysis(*identity.value, "analysis");
    auto first = graph.add_spectral_stage(*identity.value, *a.value, "identity");
    auto second = graph.add_spectral_stage(*identity.value, *b.value, "mask");
    auto synthesis = graph.add_spectral_synthesis(*identity.value, "synthesis");
    REQUIRE(analysis);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(synthesis);
    REQUIRE(graph.connect(graph.node(in)->out("audio"), analysis.value->audio[0]));
    REQUIRE(graph.connect(analysis.value->frames, first.value->frames_in));
    REQUIRE(graph.connect(first.value->frames_out, second.value->frames_in));
    REQUIRE(graph.connect(second.value->frames_out, synthesis.value->frames));
    REQUIRE(graph.connect(synthesis.value->audio[0], graph.node(out)->in("audio")));
    REQUIRE(graph.prepare(48000, 4096));
    REQUIRE(graph.latency_samples() == 319);
    graph.set_canonical_executor_routing_enabled(false);
    constexpr int total = 8192;
    std::vector<float> input(total), output(total);
    for (int t = 0; t < 6000; ++t)
        input[t] = static_cast<float>(0.2 * std::sin(t * 0.117));
    input[0] = 1;
    input[5011] = 1;
    for (int pos = 0; pos < total;) {
        const int count = std::min(total - pos, partition ? partition : 1 + pos * 17 % 4096);
        const float* ip[] = {input.data() + pos};
        float* op[] = {output.data() + pos};
        pulp::audio::BufferView<const float> iv(ip, 1, count);
        pulp::audio::BufferView<float> ov(op, 1, count);
        graph.process(ov, iv, count);
        pos += count;
    }
    double error = 0, energy = 0;
    for (int t = 0; t < total - 319; ++t) {
        const double wanted = input[t] * gain;
        const double delta = output[t + 319] - wanted;
        error += delta * delta;
        energy += wanted * wanted;
    }
    REQUIRE(error / energy < 1e-10);
    for (int t = 0; t < 319; ++t)
        REQUIRE(output[t] == 0);
    REQUIRE(graph.routed_walk_fallbacks() == 0);
    return output;
}
} // namespace

TEST_CASE("Named spectral stages execute independently in the serial graph plan",
          "[host][spectral-ports]") {
    const auto reference = render(64, 1);
    REQUIRE(render(4096, 1) == reference);
    REQUIRE(render(0, 1) == reference);
    const auto masked = render(127, 0.5f);
    for (size_t i = 0; i < masked.size(); ++i)
        REQUIRE(std::abs(masked[i] - reference[i] * 0.5f) < 1e-6f);
}

TEST_CASE("Spectral replacement stages activation before authoring finalization",
          "[host][spectral-transition]") {
    using namespace pulp::host;
    SignalGraph graph;
    const auto identity = *SpectralIdentity::make(256, 64, 1, 48000).value;
    const auto in = graph.add_input_node(1);
    const auto out = graph.add_output_node(1);
    const auto analysis = *graph.add_spectral_analysis(identity, "analysis").value;
    const auto synthesis = *graph.add_spectral_synthesis(identity, "synthesis").value;
    REQUIRE(graph.connect(graph.node(in)->out("audio"), analysis.audio[0]));
    REQUIRE(graph.connect(analysis.frames, synthesis.frames));
    REQUIRE(graph.connect(synthesis.audio[0], graph.node(out)->in("audio")));
    REQUIRE(graph.prepare(48000, 64));
    std::array<float, 64> input{}, output{};
    input.fill(0.25f);
    const float* ip[] = {input.data()};
    float* op[] = {output.data()};
    pulp::audio::BufferView<const float> iv(ip, 1, 64);
    pulp::audio::BufferView<float> ov(op, 1, 64);
    for (int i = 0; i < 12; ++i)
        graph.process(ov, iv, 64);
    REQUIRE(std::abs(output[0] - 0.25f) < 1e-6f);
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->disconnect(analysis.node, 0, synthesis.node, 0));
    const auto token =
        *QualifiedSpectralStage::real_mask_v1(identity, std::vector<float>(129, 0.5f)).value;
    const auto stage = edit->add_spectral_stage(identity, token, "mask");
    REQUIRE(stage);
    REQUIRE(edit->connect(analysis.frames, stage.value->frames_in));
    REQUIRE(edit->connect(stage.value->frames_out, synthesis.frames));
    REQUIRE(edit->prepare(48000, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Staged);
    REQUIRE_FALSE(edit->committed_execution_snapshot());
    REQUIRE(edit->nodes().empty());
    REQUIRE(edit->connections().empty());
    REQUIRE(edit->node(in) == nullptr);
    REQUIRE(edit->custom_node_type_count() == 0);
    REQUIRE_FALSE(edit->set_node_gain(in, 0));
    edit.reset();
    REQUIRE(graph.nodes().size() == 4);
    REQUIRE(graph.spectral_transition_state() == SignalGraph::SpectralTransitionState::Staged);
    graph.process(ov, iv, 64);
    REQUIRE(graph.spectral_transition_state() == SignalGraph::SpectralTransitionState::Activated);
    for (float value : output)
        REQUIRE(value == 0);
    pulp::midi::MidiBuffer events;
    REQUIRE_FALSE(graph.inject_midi(in, events));
    REQUIRE(graph.spectral_error() == SpectralError::TransitionPending);
    REQUIRE_FALSE(graph.begin_prepared_topology_edit());
    REQUIRE(graph.finalize_spectral_transition() == SpectralError::None);
    REQUIRE(graph.nodes().size() == 5);
    for (int i = 0; i < 12; ++i)
        graph.process(ov, iv, 64);
    REQUIRE(std::abs(output[0] - 0.125f) < 1e-6f);
    REQUIRE(graph.stop_spectral_controller() == SpectralError::None);
    graph.release();
}

TEST_CASE("Spectral fanout owns disjoint masks and aligns a dry nested path",
          "[host][spectral-ports][spectral-pdc]") {
    using namespace pulp::host;
    SignalGraph graph;
    const auto identity = *SpectralIdentity::make(256, 64, 1, 48000).value;
    const auto in = graph.add_input_node(1);
    const auto out = graph.add_output_node(2);
    const auto analysis = *graph.add_spectral_analysis(identity, "analysis").value;
    const auto identity_token = *QualifiedSpectralStage::identity_v1(identity).value;
    const auto first = *graph.add_spectral_stage(identity, identity_token, "identity").value;
    const auto mute =
        *graph
             .add_spectral_stage(
                 identity,
                 *QualifiedSpectralStage::real_mask_v1(identity, std::vector<float>(129, 0)).value,
                 "mute")
             .value;
    const auto silence = *graph.add_spectral_synthesis(identity, "silent branch").value;
    const auto wet = *graph.add_spectral_synthesis(identity, "wet").value;
    const auto nested = *graph.add_spectral_analysis(identity, "nested analysis").value;
    const auto nested_out = *graph.add_spectral_synthesis(identity, "nested synthesis").value;
    REQUIRE(graph.connect(graph.node(in)->out("audio"), analysis.audio[0]));
    REQUIRE(graph.connect(analysis.frames, first.frames_in));
    REQUIRE(graph.connect(first.frames_out, mute.frames_in));
    REQUIRE(graph.connect(mute.frames_out, silence.frames));
    REQUIRE(graph.connect(first.frames_out, wet.frames));
    REQUIRE(graph.connect(wet.audio[0], nested.audio[0]));
    REQUIRE(graph.connect(nested.frames, nested_out.frames));
    REQUIRE(graph.connect(silence.node, 0, out, 0));
    REQUIRE(graph.connect(nested_out.node, 0, out, 1));
    REQUIRE(graph.connect(in, 0, out, 1));
    REQUIRE(graph.prepare(48000, 4096));
    REQUIRE(graph.latency_samples() == 640);
    std::vector<float> input(8192), left(8192), right(8192);
    input[0] = 0.25f;
    input[2173] = -0.5f;
    for (int pos = 0; pos < 8192;) {
        const int count = std::min(8192 - pos, 1 + pos * 17 % 4096);
        const float* ip[] = {input.data() + pos};
        float* op[] = {left.data() + pos, right.data() + pos};
        pulp::audio::BufferView<const float> iv(ip, 1, count);
        pulp::audio::BufferView<float> ov(op, 2, count);
        graph.process(ov, iv, count);
        pos += count;
    }
    for (int t = 0; t < 8192; ++t) {
        REQUIRE(left[t] == 0);
        const float expected = t >= 640 ? input[t - 640] * 2 : 0;
        REQUIRE(std::abs(right[t] - expected) < 1e-6f);
    }
    const auto telemetry = graph.poll_live_dsp_telemetry();
    REQUIRE_FALSE(telemetry.enabled);
    REQUIRE(telemetry.available);
    REQUIRE_FALSE(telemetry.spectral.empty());
    for (const auto& diagnostic : telemetry.spectral) {
        if (diagnostic.node == 0)
            continue;
        REQUIRE(diagnostic.epoch != 0);
        REQUIRE(diagnostic.first_error == 0);
    }
}

TEST_CASE("Spectral public admission rejects malformed geometry and graph edges",
          "[host][spectral-ports][spectral-refusal]") {
    using namespace pulp::host;
    struct InvalidShape {
        int size, hop, channels;
        double rate;
    };
    const std::array invalid_shapes{
        InvalidShape{128, 64, 1, 48000},
        InvalidShape{32768, 64, 1, 48000},
        InvalidShape{257, 64, 1, 48000},
        InvalidShape{256, 0, 1, 48000},
        InvalidShape{256, 129, 1, 48000},
        InvalidShape{256, 64, 0, 48000},
        InvalidShape{256, 64, 17, 48000},
        InvalidShape{256, 64, 1, 0},
        InvalidShape{256, 64, 1, -1},
        InvalidShape{256, 64, 1, std::numeric_limits<double>::infinity()},
        InvalidShape{256, 64, 1, std::numeric_limits<double>::quiet_NaN()}};
    for (const auto& invalid : invalid_shapes) {
        CAPTURE(invalid.size, invalid.hop, invalid.channels, invalid.rate);
        const auto result =
            SpectralIdentity::make(invalid.size, invalid.hop, invalid.channels, invalid.rate);
        REQUIRE_FALSE(result);
        REQUIRE(result.error == SpectralError::InvalidIdentity);
    }
    REQUIRE(SpectralIdentity::make(256, 1, 1, 1));
    REQUIRE(SpectralIdentity::make(16384, 8192, 16, 96000));
    const auto identity = *SpectralIdentity::make(256, 64, 1, 48000).value;
    std::vector<float> mask(129, 1);
    const auto short_mask =
        QualifiedSpectralStage::real_mask_v1(identity, std::span(mask).first(128));
    REQUIRE_FALSE(short_mask);
    REQUIRE(short_mask.error == SpectralError::InvalidMask);
    for (const float invalid : {-0.1f, 1.1f, std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::quiet_NaN()}) {
        mask[5] = invalid;
        const auto rejected = QualifiedSpectralStage::real_mask_v1(identity, mask);
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error == SpectralError::InvalidMask);
    }
    SignalGraph graph, foreign;
    auto a = *graph.add_spectral_analysis(identity, "analysis").value;
    auto b = *graph.add_spectral_analysis(identity, "other analysis").value;
    auto c = *graph.add_spectral_synthesis(identity, "synthesis").value;
    auto other = *foreign.add_spectral_analysis(identity, "foreign").value;
    const auto foreign_port = graph.connect(other.frames, c.frames);
    REQUIRE(foreign_port.code == GraphResult::Code::UnknownPort);
    REQUIRE_FALSE(foreign_port.rule.empty());
    REQUIRE(graph.connect(a.frames, c.frames));
    const auto join = graph.connect(b.frames, c.frames);
    REQUIRE(join.code == GraphResult::Code::Duplicate);
    REQUIRE_FALSE(join.rule.empty());
    const auto cycle = graph.connect(c.audio[0], a.audio[0]);
    REQUIRE(cycle.code == GraphResult::Code::IllegalCycle);
    REQUIRE_FALSE(cycle.rule.empty());
    REQUIRE_FALSE(graph.connect(a.frames, c.frames));
    REQUIRE_FALSE(graph.prepare(48000, 64)); // orphan analysis
    REQUIRE(graph.remove_node(b.node));
    REQUIRE_FALSE(graph.prepare(44100, 64));
    REQUIRE_FALSE(graph.prepare(48000, 4097));
    REQUIRE(graph.prepare(48000, 64));
    REQUIRE_FALSE(graph.add_gain_node());
    REQUIRE(graph.spectral_error() == SpectralError::UnsupportedOperation);
    REQUIRE_FALSE(graph.prepare(48000, 64));
}

TEST_CASE("Independent audio feedback survives spectral preparation",
          "[host][spectral-ports][spectral-feedback]") {
    using namespace pulp::host;
    for (bool crosses_spectral : {false, true}) {
        SignalGraph graph;
        const auto identity = *SpectralIdentity::make(256, 64, 1, 48000).value;
        const auto in = graph.add_input_node(1), out = graph.add_output_node(1);
        const auto a = *graph.add_spectral_analysis(identity, "analysis").value;
        const auto s = *graph.add_spectral_synthesis(identity, "synthesis").value;
        const auto before = graph.add_gain_node("before");
        const auto after = graph.add_gain_node("after");
        REQUIRE(graph.connect(in, 0, before, 0));
        REQUIRE(graph.connect(graph.node(before)->out("audio"), a.audio[0]));
        REQUIRE(graph.connect(a.frames, s.frames));
        REQUIRE(graph.connect(s.audio[0], graph.node(out)->in("audio")));
        if (crosses_spectral) {
            REQUIRE(graph.connect(s.audio[0], graph.node(after)->in("audio")));
        } else {
            REQUIRE(graph.connect(before, 0, after, 0));
        }
        REQUIRE(graph.connect_feedback(after, 0, before, 0));
        REQUIRE(graph.prepare(48000, 64) == !crosses_spectral);
        if (!crosses_spectral) {
            std::array<float, 64> input{}, output{};
            const float* ip[] = {input.data()};
            float* op[] = {output.data()};
            pulp::audio::BufferView<float> ov{op, 1, 64};
            pulp::audio::BufferView<const float> iv{ip, 1, 64};
            graph.process(ov, iv, 64);
            for (float value : output)
                REQUIRE(value == 0);
            REQUIRE(graph.routed_walk_fallbacks() == 0);
        }
    }
}

TEST_CASE("Named spectral ports reject forged kinds before recording an edge",
          "[host][spectral-ports][spectral-handle-validation]") {
    using namespace pulp::host;
    const auto identity = *SpectralIdentity::make(256, 64, 1, 48000).value;
    for (bool prepared_edit : {false, true}) {
        SignalGraph graph;
        const auto analysis = *graph.add_spectral_analysis(identity, "analysis").value;
        const auto synthesis = *graph.add_spectral_synthesis(identity, "synthesis").value;
        auto edit = prepared_edit ? graph.begin_prepared_topology_edit() : nullptr;
        auto source = graph.node(analysis.node)->out("frames");
        auto destination = graph.node(synthesis.node)->in("frames");
        source.kind = PortKind::Audio;
        const auto mismatched =
            edit ? edit->connect(source, destination) : graph.connect(source, destination);
        REQUIRE(mismatched.code == GraphResult::Code::KindMismatch);
        REQUIRE((edit ? edit->connections() : graph.connections()).empty());
        for (const auto kind : {PortKind::Audio, PortKind::Sidechain, PortKind::Midi,
                                PortKind::Event, PortKind::Control, PortKind::Param}) {
            source.kind = destination.kind = kind;
            const auto result =
                edit ? edit->connect(source, destination) : graph.connect(source, destination);
            REQUIRE(result.code == GraphResult::Code::KindMismatch);
            REQUIRE_FALSE(result.rule.empty());
            REQUIRE((edit ? edit->connections() : graph.connections()).empty());
        }
        source.kind = destination.kind = PortKind::Spectral;
        REQUIRE((edit ? edit->connect(source, destination) : graph.connect(source, destination)));
    }
}

TEST_CASE("Frequency selective masks preserve finite tails through the complete drain",
          "[host][spectral-ports][spectral-mask-tail]") {
    using namespace pulp::host;
    const auto render_tail = [](bool irregular) {
        SignalGraph graph;
        const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
        const auto input_node = graph.add_input_node(1), output_node = graph.add_output_node(1);
        const auto analysis = *graph.add_spectral_analysis(shape, "analysis").value;
        std::vector<float> gains(129);
        gains[0] = 1;
        gains[9] = 0.5f;
        const auto stage =
            *graph
                 .add_spectral_stage(shape,
                                     *QualifiedSpectralStage::real_mask_v1(shape, gains).value,
                                     "DC and interior bin")
                 .value;
        const auto synthesis = *graph.add_spectral_synthesis(shape, "synthesis").value;
        REQUIRE(graph.connect(graph.node(input_node)->out("audio"), analysis.audio[0]));
        REQUIRE(graph.connect(analysis.frames, stage.frames_in));
        REQUIRE(graph.connect(stage.frames_out, synthesis.frames));
        REQUIRE(graph.connect(synthesis.audio[0], graph.node(output_node)->in("audio")));
        REQUIRE(graph.prepare(48000, 4096));
        REQUIRE(graph.latency_samples() == 320);
        std::vector<float> input(1800), output(1800);
        for (int i = 0; i <= 701; ++i)
            input[i] = 0.25f + 0.1f * std::sin(i * 0.117f);
        input[701] = 1;
        for (int position = 0; position < 1800;) {
            const int count = std::min(1800 - position, irregular ? 1 + position * 17 % 511 : 64);
            const float* ip[] = {input.data() + position};
            float* op[] = {output.data() + position};
            pulp::audio::BufferView<const float> iv(ip, 1, count);
            pulp::audio::BufferView<float> ov(op, 1, count);
            graph.process(ov, iv, count);
            position += count;
        }
        for (float value : output)
            REQUIRE(std::isfinite(value));
        // The last input contributes to the frame at origin 640. Its late
        // support must survive beyond a drain shortened by one hop.
        REQUIRE(std::abs(output[1213]) > 1e-9f);
        for (int i = 702 + 320 + 255; i < 1800; ++i)
            REQUIRE(output[i] == 0);
        return output;
    };
    REQUIRE(render_tail(false) == render_tail(true));
}

TEST_CASE("Spectral batches carry canonical versioned frame fields",
          "[host][spectral-ports][spectral-contract]") {
    using namespace pulp::host;
    using pulp::graph::SpectralFrameContract;
    const auto identity = *SpectralIdentity::make(256, 64, 1, 48000).value;
    const auto contract = identity.frame_contract();
    REQUIRE(contract.version == 1);
    REQUIRE(contract.profile == 1);
    REQUIRE(contract.fft_size == 256);
    REQUIRE(contract.hop == 64);
    REQUIRE(contract.channels == 1);
    REQUIRE(contract.bins == 129);
    REQUIRE(contract.sample_rate_bits == identity.sample_rate_bits());
    REQUIRE(contract.window == 1);
    REQUIRE(contract.phase == 1);
    REQUIRE(contract.normalization == 1);
    REQUIRE(contract.precision == 1);

    const auto encoded = contract.encode();
    REQUIRE(encoded.size() == SpectralFrameContract::encoded_size);
    const auto decoded = SpectralFrameContract::decode(encoded);
    REQUIRE(decoded);
    REQUIRE(*decoded.value == contract);
    auto unsupported = encoded;
    unsupported[0] = 2;
    REQUIRE_FALSE(SpectralFrameContract::decode(unsupported));
    REQUIRE(SpectralFrameContract::decode(unsupported).error == SpectralError::InvalidContract);
    auto noncanonical = encoded;
    noncanonical[31] = 1;
    REQUIRE_FALSE(SpectralFrameContract::decode(noncanonical));

    // Telemetry consumers persist these values; adding InvalidContract must
    // not renumber any existing diagnostic.
    REQUIRE(static_cast<std::uint8_t>(SpectralError::None) == 0);
    REQUIRE(static_cast<std::uint8_t>(SpectralError::IdentityMismatch) == 3);
    REQUIRE(static_cast<std::uint8_t>(SpectralError::OutputUnderrun) == 19);
    REQUIRE(static_cast<std::uint8_t>(SpectralError::InvalidContract) == 20);
}

namespace {
struct AssetGraph {
    pulp::host::SignalGraph graph;
    pulp::host::SpectralAssetMorphPorts source;
    AssetGraph(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b,
               std::uint64_t length, bool stage = false) {
        using namespace pulp::host;
        const auto input = graph.add_input_node(1), output = graph.add_output_node(1);
        const auto result = graph.add_spectral_asset_morph_v1(a, b, length, "owned assets");
        REQUIRE(result);
        source = *result.value;
        const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
        const auto synthesis = *graph.add_spectral_synthesis(shape, "synthesis").value;
        REQUIRE(graph.connect(graph.node(input)->out("audio"), source.morph));
        if (stage) {
            const auto identity =
                *graph
                     .add_spectral_stage(shape, *QualifiedSpectralStage::identity_v1(shape).value,
                                         "identity")
                     .value;
            REQUIRE(graph.connect(source.frames, identity.frames_in));
            REQUIRE(graph.connect(identity.frames_out, synthesis.frames));
        } else
            REQUIRE(graph.connect(source.frames, synthesis.frames));
        REQUIRE(graph.connect(synthesis.audio[0], graph.node(output)->in("audio")));
        REQUIRE(graph.prepare(48000, 4096));
        REQUIRE(graph.latency_samples() == 320);
    }
    std::vector<float> render(const std::vector<float>& control, int partition,
                              bool reset = false) {
        std::vector<float> audio(control.size());
        for (int pos = 0; pos < int(control.size());) {
            const auto count =
                std::min(int(control.size()) - pos, partition ? partition : 1 + pos * 17 % 511);
            const float* ip[] = {control.data() + pos};
            float* op[] = {audio.data() + pos};
            pulp::audio::BufferView<const float> iv(ip, 1, count);
            pulp::audio::BufferView<float> ov(op, 1, count);
            pulp::format::ProcessContext transport;
            transport.reset_requested = reset && pos == 0;
            transport.position_samples = 99999; // reset is a restart, not an asset seek
            std::size_t allocations = 0;
            {
                pulp::test::RtAllocationProbe probe;
                graph.process(ov, iv, count, transport);
                allocations = probe.allocation_count();
            }
            REQUIRE(allocations == 0);
            pos += count;
        }
        return audio;
    }
    void clean_diagnostics() {
        for (const auto& diagnostic : graph.poll_live_dsp_telemetry().spectral)
            REQUIRE(diagnostic.first_error == 0);
        REQUIRE(graph.routed_walk_fallbacks() == 0);
    }
    std::uint64_t epoch() {
        for (const auto& diagnostic : graph.poll_live_dsp_telemetry().spectral)
            if (diagnostic.node == source.node)
                return diagnostic.epoch;
        FAIL("source diagnostic absent");
        return 0;
    }
};
} // namespace

TEST_CASE("Owned asset morph agrees with independent direct DFT live audio oracle",
          "[host][spectral-asset-morph][asset-audio-oracle]") {
    using namespace asset_oracle;
    const auto input_a = stimulus(0), input_b = stimulus(1);
    const auto a = analyze(input_a), b = analyze(input_b);
    const auto bytes_a = encode(a), bytes_b = encode(b);
    for (float amount : {0.0f, 1.0f, 0.37f, -1.0f}) {
        std::vector<float> control(a.size() * 64 + 1024, amount);
        if (amount < 0)
            for (std::size_t i = 0; i < control.size(); ++i)
                control[i] = float((i * 13 % 257) / 256.0);
        const auto wanted = expected(a, b, control);
        // Calibrate the independent oracle against known analytic delay before
        // comparing candidate audio. Expected output never calls production DSP.
        if (amount == 0 || amount == 1) {
            const auto& input = amount == 0 ? input_a : input_b;
            for (std::size_t t = 0; t < input.size(); ++t)
                REQUIRE(std::abs(wanted[t + 320] - input[t]) < 1e-11);
        }
        std::vector<float> reference;
        for (int partition : {32, 64, 128, 0, 4096}) {
            AssetGraph fixture(bytes_a.view(), bytes_b.view(), input_a.size(), partition == 0);
            const auto audio = fixture.render(control, partition);
            compare(audio, wanted);
            REQUIRE((reference.empty() || audio == reference));
            reference = audio;
            for (std::size_t t = 0; t < audio.size(); ++t)
                if (t < 320 || t >= fixture.source.render_frames)
                    REQUIRE(audio[t] == 0);
            fixture.clean_diagnostics();
            const auto old_epoch = fixture.epoch();
            REQUIRE(old_epoch != 0);
            fixture.render(std::vector<float>(113, 0.25f), 32, true);
            REQUIRE(fixture.render(control, partition, true) == audio);
            const auto fresh_epoch = fixture.epoch();
            REQUIRE(fresh_epoch != old_epoch);
            fixture.clean_diagnostics();
        }
    }
}

TEST_CASE("Asset morph phase wrap and self conjugate bins follow normalized control",
          "[host][spectral-asset-morph][asset-audio-oracle]") {
    using namespace asset_oracle;
    std::vector<Frame> a(8), b(8);
    for (std::size_t f = 0; f < a.size(); ++f) {
        a[f][9] = std::polar(2.0, 3.10);
        b[f][9] = std::polar(4.0, -3.10);
        a[f][0] = 3;
        b[f][0] = -1;
        a[f][128] = -2;
        b[f][128] = 4;
    }
    auto bytes_a = encode(a), bytes_b = encode(b);
    for (float amount : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        std::vector<float> control(a.size() * 64 + 1024, amount);
        AssetGraph fixture(bytes_a.view(), bytes_b.view(), 320);
        compare(fixture.render(control, 0), expected(a, b, control));
        fixture.clean_diagnostics();
    }
}

TEST_CASE("Asset impulses preserve startup and final support with absolute hop means",
          "[host][spectral-asset-morph][asset-audio-oracle]") {
    using namespace asset_oracle;
    for (int position : {0, 13, 320}) {
        std::vector<float> input_a(321), input_b(321);
        input_a[position] = 0.5f;
        input_b[position] = 0.25f;
        const auto a = analyze(input_a), b = analyze(input_b);
        auto bytes_a = encode(a), bytes_b = encode(b);
        for (float amount : {0.0f, 0.37f, 1.0f}) {
            CAPTURE(position, amount);
            std::vector<float> control(8192, amount);
            AssetGraph fixture(bytes_a.view(), bytes_b.view(), 321);
            REQUIRE_NOTHROW(compare(fixture.render(control, 4096), expected(a, b, control)));
            fixture.clean_diagnostics();
        }
        std::vector<float> constant(8192, 0.5f), stepped(8192);
        for (std::size_t t = 0; t < stepped.size(); ++t)
            stepped[t] = t % 64 < 32 ? 0 : 1;
        AssetGraph first(bytes_a.view(), bytes_b.view(), 321),
            second(bytes_a.view(), bytes_b.view(), 321);
        REQUIRE(first.render(constant, 128) == second.render(stepped, 32));
        first.clean_diagnostics();
        second.clean_diagnostics();
        // Terminal controls are not consumed and cannot turn ordinary EOF into
        // a fatal control error or restart frame synthesis.
        std::fill(stepped.begin(), stepped.end(), std::numeric_limits<float>::quiet_NaN());
        const auto silence = second.render(stepped, 4096);
        REQUIRE(std::all_of(silence.begin(), silence.end(), [](float x) { return x == 0; }));
        second.clean_diagnostics();
    }
}

TEST_CASE("Canonical antipodal bins preserve analytic branches endpoints and partitions",
          "[host][spectral-asset-morph][asset-audio-oracle]") {
    using namespace asset_oracle;
    // Original double DFT endpoints at exactly pi separation can choose the
    // other equally short arc after float32 serialization. For chronology use
    // non-antipodal impulses; here exact real serialized bins and canonical +0
    // imaginary components isolate both analytic half-turn branches.
    std::vector<Frame> a(8), b(8);
    for (bool reverse : {false, true}) {
        for (std::size_t f = 0; f < a.size(); ++f) {
            a[f][9] = reverse ? -4 : 2;
            b[f][9] = reverse ? 2 : -4;
        }
        auto bytes_a = encode(a), bytes_b = encode(b);
        for (float amount : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            std::vector<float> control(2048, amount), reference;
            const auto wanted = expected(a, b, control);
            const auto bin = interpolate(a[0][9], b[0][9], amount, 9);
            // Forward +pi branch has nonnegative imaginary component; the
            // reverse branch traverses the same upper half-plane toward zero.
            REQUIRE(bin.imag() >= 0);
            for (int partition : {32, 64, 128, 0}) {
                AssetGraph fixture(bytes_a.view(), bytes_b.view(), 320);
                const auto audio = fixture.render(control, partition);
                compare(audio, wanted);
                REQUIRE((reference.empty() || audio == reference));
                reference = audio;
                fixture.clean_diagnostics();
            }
        }
    }
}

TEST_CASE("Asset factory refusals preserve authoring and copied asset ownership",
          "[host][spectral-asset-morph][spectral-refusal]") {
    using namespace pulp::host;
    using namespace asset_oracle;
    const auto a = analyze(stimulus(0)), b = analyze(stimulus(1));
    auto bytes_a = encode(a), bytes_b = encode(b);
    SignalGraph graph;
    const auto input = graph.add_input_node(1);
    auto refuse = [&](std::span<const std::uint8_t> first, std::span<const std::uint8_t> second,
                      std::uint64_t length) {
        REQUIRE_FALSE(graph.add_spectral_asset_morph_v1(first, second, length, "rejected"));
        REQUIRE(graph.nodes().size() == 1);
        REQUIRE(graph.connections().empty());
    };
    refuse(bytes_a.view(), bytes_b.view(), 0);
    refuse(bytes_a.view(), bytes_b.view(), 1);
    refuse(bytes_a.view(), bytes_b.view(), std::numeric_limits<std::uint64_t>::max());
    refuse(bytes_a.view().first(12), bytes_b.view(), 769);
    const auto other_rate = encode(b, 44100);
    refuse(bytes_a.view(), other_rate.view(), 769);
    for (std::size_t position :
         {std::size_t(80), std::size_t(64 + 1048), std::size_t(64 + 1048 + 8)}) {
        std::vector<std::uint8_t> corrupt(bytes_a.view().begin(), bytes_a.view().end());
        if (position == 80) {
            corrupt[82] = 0x80;
            corrupt[83] = 0x7f;
        } else
            corrupt[position] ^= 1;
        refuse(corrupt, bytes_b.view(), 769);
    }
    const auto accepted =
        graph.add_spectral_asset_morph_v1(bytes_a.view(), bytes_b.view(), 769, "valid");
    REQUIRE(accepted);
    REQUIRE(accepted.value->node == input + 1);
    REQUIRE_FALSE(graph.connect(input, 0, accepted.value->node, 0));
    REQUIRE(graph.connect(graph.node(input)->out("audio"), accepted.value->morph));
    REQUIRE_FALSE(graph.connect(graph.node(input)->out("audio"), accepted.value->morph));
    AssetGraph owned(bytes_a.view(), bytes_b.view(), 769);
    std::fill_n(bytes_a.bytes.get(), bytes_a.size, 0);
    std::fill_n(bytes_b.bytes.get(), bytes_b.size, 0);
    std::vector<float> control(a.size() * 64 + 1024, 0.37f);
    compare(owned.render(control, 64), expected(a, b, control));
    owned.clean_diagnostics();
}

TEST_CASE("Invalid live asset control stops at the absolute failing frame origin",
          "[host][spectral-asset-morph][spectral-fatal-prefix]") {
    using namespace asset_oracle;
    const auto a = analyze(stimulus(0)), b = analyze(stimulus(1));
    auto bytes_a = encode(a), bytes_b = encode(b);
    std::vector<float> control(a.size() * 64 + 1024, 0.37f);
    AssetGraph good(bytes_a.view(), bytes_b.view(), 769);
    const auto reference = good.render(control, 64);
    for (float invalid : {-0.01f, 1.01f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
        for (int partition : {32, 128, 0, 4096}) {
            auto bad_control = control;
            bad_control[701] = invalid;
            AssetGraph bad(bytes_a.view(), bytes_b.view(), 769);
            const auto audio = bad.render(bad_control, partition);
            // Sample 701 is in hop 10, whose frame origin is -192+10*64.
            constexpr int cutoff = 320 - 192 + 10 * 64;
            for (std::size_t t = 0; t < audio.size(); ++t)
                REQUIRE(audio[t] == (t < cutoff ? reference[t] : 0));
            bool saw_error = false;
            for (const auto& diagnostic : bad.graph.poll_live_dsp_telemetry().spectral)
                saw_error |= diagnostic.first_error != 0;
            REQUIRE(saw_error);
            REQUIRE(bad.render(control, partition, true) == reference);
        }
    }
}

TEST_CASE("Asset preparation enforces geometry control topology and retained memory bounds",
          "[host][spectral-asset-morph][spectral-capacity]") {
    using namespace pulp::host;
    using namespace asset_oracle;
    const auto a = analyze(stimulus(0));
    const auto bytes = encode(a);
    for (bool connect_control : {false, true}) {
        SignalGraph graph;
        const auto source =
            *graph.add_spectral_asset_morph_v1(bytes.view(), bytes.view(), 769, "source").value;
        const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
        const auto synthesis = *graph.add_spectral_synthesis(shape, "synthesis").value;
        const auto output = graph.add_output_node(1), gain = graph.add_gain_node("delayed control");
        REQUIRE_FALSE(graph.connect(graph.node(gain)->out("audio"), source.morph));
        if (connect_control) {
            const auto input = graph.add_input_node(1);
            REQUIRE(graph.connect(graph.node(input)->out("audio"), source.morph));
        }
        REQUIRE(graph.connect(source.frames, synthesis.frames));
        REQUIRE(graph.connect(synthesis.audio[0], graph.node(output)->in("audio")));
        REQUIRE_FALSE(graph.prepare(44100, 64));
        REQUIRE_FALSE(graph.prepare(48000, 4097));
        REQUIRE(graph.prepare(48000, 64) == connect_control);
    }
    for (auto geometry : {pulp::signal::SpectralStreamGeometry{512, 64, 1, 48000},
                          pulp::signal::SpectralStreamGeometry{256, 32, 1, 48000},
                          pulp::signal::SpectralStreamGeometry{256, 64, 2, 48000}}) {
        auto stream = pulp::signal::make_spectral_stream(geometry, a.size());
        REQUIRE(stream);
        auto wrong = pulp::signal::encode_spectral_stream(*stream.value);
        REQUIRE(wrong);
        SignalGraph graph;
        REQUIRE_FALSE(
            graph.add_spectral_asset_morph_v1(wrong.value->view(), bytes.view(), 769, "wrong"));
        REQUIRE(graph.nodes().empty());
    }
    auto large = pulp::signal::make_spectral_stream({256, 64, 1, 48000}, 33000);
    REQUIRE(large);
    auto encoded = pulp::signal::encode_spectral_stream(*large.value);
    REQUIRE(encoded);
    SignalGraph graph;
    REQUIRE_FALSE(graph.add_spectral_asset_morph_v1(encoded.value->view(), encoded.value->view(),
                                                    33000 * 64 - 192, "over budget"));
    REQUIRE(graph.nodes().empty());
}
