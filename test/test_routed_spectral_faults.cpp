#include "../core/host/src/spectral_graph_internal.hpp"
#include "support/thread_progress.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>

namespace {
struct FaultGraph {
    pulp::host::SignalGraph graph;
    pulp::host::SpectralRuntimeBank bank;
    pulp::host::NodeId analysis, stage, wet, sibling;
    FaultGraph(int hop = 64, int block = 512) {
        using namespace pulp::host;
        const auto identity = *SpectralIdentity::make(256, hop, 1, 48000).value;
        const auto a = *graph.add_spectral_analysis(identity, "analysis").value;
        const auto b =
            *graph
                 .add_spectral_stage(identity, *QualifiedSpectralStage::identity_v1(identity).value,
                                     "identity")
                 .value;
        const auto c = *graph.add_spectral_synthesis(identity, "wet").value;
        const auto d = *graph.add_spectral_synthesis(identity, "sibling").value;
        REQUIRE(graph.connect(a.frames, b.frames_in));
        REQUIRE(graph.connect(b.frames_out, c.frames));
        REQUIRE(graph.connect(a.frames, d.frames));
        REQUIRE(bank.prepare(graph.nodes(), graph.connections(), 48000, block) ==
                SpectralError::None);
        analysis = a.node;
        stage = b.node;
        wet = c.node;
        sibling = d.node;
        bank.reset(17);
    }
    void process(pulp::host::NodeId id, std::span<const float> input, std::span<float> output) {
        const float* ip[] = {input.data()};
        float* op[] = {output.data()};
        pulp::format::GraphRuntimeNodeProcessContext ctx;
        ctx.node_inputs = {ip, 1, input.size()};
        ctx.node_outputs = {op, 1, output.size()};
        ctx.spectral_input = bank.find(id)->input;
        pulp::format::ProcessBlock block;
        REQUIRE(pulp::host::SpectralRuntimeBank::Node::process(block, ctx, bank.find(id)));
    }
};
} // namespace

TEST_CASE("Routed late synthesis failure preserves prefix and independent sibling",
          "[host][spectral-fault][spectral-fatal-prefix]") {
    FaultGraph bad, reference;
    std::array<float, 512> input, scratch{}, wet{}, sibling{}, expected{};
    input.fill(0.25f);
    for (int block = 0; block < 5; ++block) {
        for (auto* f : {&bad, &reference}) {
            f->bank.begin_block();
            f->process(f->analysis, input, scratch);
            f->process(f->stage, input, scratch);
        }
        if (block == 2) {
            auto* edge = bad.bank.find(bad.wet)->input;
            REQUIRE(edge->count == 8);
            // A late frame has finite codec-valid bins whose inverse overflows.
            const auto offset = 6 * edge->bins_per_frame;
            std::fill_n(edge->bins.begin() + offset, edge->bins_per_frame,
                        std::complex<float>{3e38f, 0});
        }
        bad.process(bad.wet, input, wet);
        bad.process(bad.sibling, input, sibling);
        reference.process(reference.wet, input, expected);
        reference.process(reference.sibling, input, scratch);
        REQUIRE(sibling == expected);
        for (int i = 0; i < 512; ++i) {
            const auto* node = bad.bank.find(bad.wet);
            const auto cutoff = node->fatal_origin == std::numeric_limits<std::int64_t>::max()
                                    ? node->fatal_origin
                                    : std::max<std::int64_t>(0, node->fatal_origin) + 320;
            REQUIRE(std::isfinite(wet[i]));
            if (block * 512 + i < cutoff)
                REQUIRE(wet[i] == expected[i]);
            else
                REQUIRE(wet[i] == 0.0f);
        }
    }
    REQUIRE(bad.bank.find(bad.wet)->first_error == pulp::host::SpectralError::SynthesisOverflow);
    REQUIRE(bad.bank.find(bad.sibling)->first_error == pulp::host::SpectralError::None);
}

TEST_CASE("Frame capacity failure carries a fatal marker without a payload slot",
          "[host][spectral-fault][spectral-capacity]") {
    FaultGraph f;
    std::array<float, 512> input{}, output{};
    auto* edge = f.bank.find(f.stage)->input;
    edge->stamps = edge->stamps.first(1);
    f.bank.begin_block();
    f.process(f.analysis, input, output);
    REQUIRE(edge->count == 1);
    REQUIRE(edge->fatal);
    REQUIRE(f.bank.find(f.analysis)->first_error == pulp::host::SpectralError::CapacityExceeded);
    f.process(f.stage, input, output);
    REQUIRE(f.bank.find(f.wet)->input->fatal);
    REQUIRE(f.bank.find(f.stage)->sequence == 1);
    f.process(f.wet, input, output);
    for (float value : output)
        REQUIRE(value == 0);
}

TEST_CASE("Missing frame replacement advances once and preserves the sibling payload",
          "[host][spectral-fault][spectral-missing]") {
    FaultGraph f;
    std::array<float, 512> input{}, output{};
    input.fill(0.25f);
    f.bank.begin_block();
    f.process(f.analysis, input, output);
    auto* edge = f.bank.find(f.stage)->input;
    const auto sibling = f.bank.find(f.sibling)->input;
    const std::vector<std::complex<float>> saved(sibling->bins.begin(), sibling->bins.end());
    edge->stamps[0].epoch = 99;
    f.process(f.stage, input, output);
    REQUIRE(f.bank.find(f.stage)->sequence == 8);
    REQUIRE(f.bank.find(f.stage)->first_error == pulp::host::SpectralError::MissingFrame);
    REQUIRE(f.bank.find(f.stage)->errors == 1);
    REQUIRE(std::equal(saved.begin(), saved.end(), sibling->bins.begin()));
    const auto* replaced = f.bank.find(f.wet)->input;
    for (size_t i = 0; i < replaced->bins_per_frame; ++i)
        REQUIRE(replaced->bins[i] == std::complex<float>{});
}

#include <latch>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <thread>

namespace pulp::host {
// Private seam exercises the same capture/transition functions as process().
// It exposes no executable stage registration or callback-time test hook.
struct SpectralControllerTestAccess {
    static auto capture(SignalGraph& g) {
        return g.capture_spectral_boundary_();
    }
    static void render(SignalGraph& g, SignalGraph::SpectralBoundary boundary,
                       audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                       const format::ProcessContext* transport) {
        g.process_spectral_boundary_(boundary, out, in, static_cast<int>(out.num_samples()),
                                     transport);
    }
    static auto reader(SignalGraph& g) {
        return g.read_snapshot_();
    }
    static auto epoch(SignalGraph& g) {
        return g.spectral_transition_serial_;
    }
    static void exhaust(SignalGraph& g) {
        g.spectral_transition_serial_ = UINT32_MAX;
    }
    static bool terminal(SignalGraph& g) {
        return g.spectral_terminal_;
    }
};
} // namespace pulp::host
namespace {
struct ControllerGraph {
    pulp::host::SignalGraph graph;
    std::array<float, 64> input{}, output{};
    const float* ip[1] = {input.data()};
    float* op[1] = {output.data()};
    pulp::audio::BufferView<const float> iv{ip, 1, 64};
    pulp::audio::BufferView<float> ov{op, 1, 64};
    pulp::host::NodeId midi_in = 0, midi_out = 0, custom = 0;
    ControllerGraph(bool with_midi = false, bool with_custom = false) {
        using namespace pulp::host;
        const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
        const auto in = graph.add_input_node(1), out = graph.add_output_node(1);
        const auto a = *graph.add_spectral_analysis(shape, "analysis").value;
        const auto s = *graph.add_spectral_synthesis(shape, "synthesis").value;
        REQUIRE(graph.connect(graph.node(in)->out("audio"), a.audio[0]));
        REQUIRE(graph.connect(a.frames, s.frames));
        REQUIRE(graph.connect(s.audio[0], graph.node(out)->in("audio")));
        REQUIRE(graph.connect(in, 0, out, 0));
        if (with_midi) {
            midi_in = graph.add_midi_input_node();
            midi_out = graph.add_midi_output_node();
            REQUIRE(graph.connect_midi(midi_in, midi_out));
        }
        if (with_custom) {
            CustomNodeType type;
            type.type_id = "spectral.test.custom";
            type.version = 1;
            type.num_input_ports = type.num_output_ports = 1;
            type.create = []() -> void* { return new int{}; };
            type.destroy = [](void* value) { delete static_cast<int*>(value); };
            type.process_instance = [](void*, auto& out, const auto&, int) { out.clear(); };
            REQUIRE(graph.register_custom_node_type(std::move(type)));
            custom = graph.add_custom_node("spectral.test.custom");
            REQUIRE(custom != 0);
        }
        REQUIRE(graph.prepare(48000, 64));
        input.fill(0.25f);
        for (int i = 0; i < 12; ++i)
            graph.process(ov, iv, 64);
    }
    auto edit(int block = 64) {
        auto candidate = graph.begin_prepared_topology_edit();
        REQUIRE(candidate);
        REQUIRE(candidate->prepare(48000, block) ==
                pulp::host::SignalGraph::PreparedTopologyEdit::Result::Prepared);
        return candidate;
    }
    void process(const pulp::format::ProcessContext* transport = nullptr) {
        if (transport)
            graph.process(ov, iv, 64, *transport);
        else
            graph.process(ov, iv, 64);
    }
};
} // namespace

TEST_CASE("Offer after boundary capture cannot steal the active reset",
          "[host][spectral-transition][spectral-barrier]") {
    using namespace pulp::host;
    ControllerGraph target, control;
    auto candidate = target.edit();
    pulp::format::ProcessContext reset;
    reset.reset_requested = true;
    target.input.fill(0);
    control.input.fill(0);
    std::latch captured{1}, offered{1};
    bool offered_in_time = false;
    std::thread render([&] {
        const auto boundary = SpectralControllerTestAccess::capture(target.graph);
        captured.count_down();
        offered_in_time = pulp::test::wait_for_condition([&] { return offered.try_wait(); });
        if (!offered_in_time)
            return;
        SpectralControllerTestAccess::render(target.graph, boundary, target.ov, target.iv, &reset);
    });
    const bool captured_in_time =
        pulp::test::wait_for_condition([&] { return captured.try_wait(); });
    const auto result = candidate->commit();
    offered.count_down();
    render.join();
    REQUIRE(captured_in_time);
    REQUIRE(offered_in_time);
    REQUIRE(result == SignalGraph::PreparedTopologyEdit::Result::Staged);
    control.process(&reset);
    REQUIRE(target.output == control.output);
    for (float value : target.output)
        REQUIRE(value == 0);
    REQUIRE(SpectralControllerTestAccess::epoch(target.graph) == 2);
    REQUIRE(target.graph.spectral_transition_state() ==
            SignalGraph::SpectralTransitionState::Staged);
    target.process();
    REQUIRE(SpectralControllerTestAccess::epoch(target.graph) == 3);
    REQUIRE(target.graph.spectral_transition_state() ==
            SignalGraph::SpectralTransitionState::Activated);
    {
        auto reader = SpectralControllerTestAccess::reader(target.graph);
        REQUIRE(reader);
        REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::Busy);
    }
    REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::None);
}

TEST_CASE("Rejected candidates preserve active reset history and consume no epoch",
          "[host][spectral-transition][spectral-rejected-reset]") {
    using namespace pulp::host;
    for (const bool reset_requested : {false, true}) {
        ControllerGraph target, control;
        auto candidate = target.edit(32);
        REQUIRE(candidate->commit() == SignalGraph::PreparedTopologyEdit::Result::Staged);
        pulp::format::ProcessContext reset;
        reset.reset_requested = reset_requested;
        target.input.fill(0);
        control.input.fill(0);
        target.process(&reset);
        control.process(&reset);
        REQUIRE(target.output == control.output);
        REQUIRE(SpectralControllerTestAccess::epoch(target.graph) ==
                SpectralControllerTestAccess::epoch(control.graph));
        REQUIRE(target.graph.spectral_transition_state() ==
                SignalGraph::SpectralTransitionState::Rejected);
        REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::CapacityExceeded);
        for (int block = 0; block < 10; ++block) {
            target.process();
            control.process();
            REQUIRE(target.output == control.output);
        }
    }
}

TEST_CASE("Successful offer and seek share one epoch while exhaustion is terminal only on reset",
          "[host][spectral-transition][spectral-epoch]") {
    using namespace pulp::host;
    ControllerGraph target;
    auto candidate = target.edit();
    REQUIRE(candidate->commit() == SignalGraph::PreparedTopologyEdit::Result::Staged);
    pulp::format::ProcessContext seek;
    seek.transport_jump = true;
    target.process(&seek);
    REQUIRE(SpectralControllerTestAccess::epoch(target.graph) == 2);
    REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::None);
    for (int i = 0; i < 12; ++i)
        target.process();
    const auto uninterrupted = target.output;
    SpectralControllerTestAccess::exhaust(target.graph);
    candidate = target.edit();
    REQUIRE(candidate->commit() == SignalGraph::PreparedTopologyEdit::Result::Staged);
    target.process();
    REQUIRE(target.output == uninterrupted);
    REQUIRE_FALSE(SpectralControllerTestAccess::terminal(target.graph));
    REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::EpochExhausted);
    target.process(&seek);
    REQUIRE(SpectralControllerTestAccess::terminal(target.graph));
    REQUIRE(target.graph.spectral_error() == SpectralError::EpochExhausted);
    for (float value : target.output)
        REQUIRE(value == 0);
    REQUIRE(SpectralControllerTestAccess::epoch(target.graph) == UINT32_MAX);
}

#include "harness/rt_allocation_probe.hpp"
TEST_CASE("Spectral render reset and activation allocate no callback storage",
          "[host][spectral-transition][spectral-allocation]") {
    using namespace pulp::host;
    ControllerGraph target;
    auto candidate = target.edit();
    REQUIRE(candidate->commit() == SignalGraph::PreparedTopologyEdit::Result::Staged);
    pulp::format::ProcessContext reset;
    reset.reset_requested = true;
    size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        target.process(&reset);
        for (int i = 0; i < 300; ++i)
            target.process();
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
    REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::None);
    const auto telemetry = target.graph.poll_live_dsp_telemetry();
    REQUIRE(telemetry.blocks_dropped > 0);
    REQUIRE(telemetry.age_blocks > 0);
}

TEST_CASE("Synthetic chronology limits refuse before frame arithmetic",
          "[host][spectral-fault][spectral-chronology]") {
    FaultGraph f;
    std::array<float, 512> input{}, output{};
    auto* node = f.bank.find(f.analysis);
    node->origin = std::numeric_limits<std::int64_t>::max() - 1;
    const auto sequence = node->sequence;
    f.process(f.analysis, input, output);
    REQUIRE(node->first_error == pulp::host::SpectralError::ChronologyExhausted);
    REQUIRE(node->sequence == sequence);
    REQUIRE(node->outputs.front()->count == 0);
    REQUIRE(node->outputs.front()->fatal);
}

TEST_CASE("Built-in identity preserves zero without invoking a contract guard",
          "[host][spectral-fault][spectral-zero]") {
    FaultGraph f;
    std::array<float, 512> input{}, output{};
    f.bank.begin_block();
    f.process(f.analysis, input, output);
    f.process(f.stage, input, output);
    REQUIRE(f.bank.find(f.stage)->first_error == pulp::host::SpectralError::None);
    f.process(f.wet, input, output);
    for (float value : output)
        REQUIRE((value == 0 && !std::signbit(value)));
}

TEST_CASE("Activated block size is visible before authoring finalization",
          "[host][spectral-transition][spectral-block-query]") {
    using namespace pulp::host;
    ControllerGraph target;
    REQUIRE(target.graph.prepared_max_block_size() == 64);
    auto candidate = target.edit(128);
    REQUIRE(candidate->commit() == SignalGraph::PreparedTopologyEdit::Result::Staged);
    target.process();
    REQUIRE(target.graph.prepared_max_block_size() == 128);
    REQUIRE(target.graph.prepared_stats().max_block_size == 128);
    REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::None);
}

namespace {
class ProviderProbe final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "spectral-provider-probe";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.spectral-provider";
        d.version = "1.0.0";
        d.input_buses = {{"input", 1, false}};
        d.output_buses = {{"output", 1, false}};
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        out.clear();
    }
};
} // namespace
TEST_CASE("A live spectral graph refuses processor input provider mutation",
          "[host][spectral-transition][spectral-provider]") {
    using namespace pulp::host;
    SignalGraph graph;
    const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
    const auto a = *graph.add_spectral_analysis(shape, "analysis").value;
    const auto s = *graph.add_spectral_synthesis(shape, "synthesis").value;
    REQUIRE(graph.connect(a.frames, s.frames));
    const auto processor = graph.add_processor_node(std::make_unique<ProviderProbe>());
    REQUIRE(processor != 0);
    int context = 0;
    pulp::format::AudioInputBusProvider provider{
        &context, [](void*, size_t, size_t, pulp::audio::BufferView<const float>&) noexcept {
            return false;
        }};
    REQUIRE(graph.set_processor_audio_input_provider(processor, provider));
    REQUIRE(graph.prepare(48000, 64));
    REQUIRE_FALSE(graph.set_processor_audio_input_provider(processor, provider));
    REQUIRE(graph.spectral_error() == SpectralError::UnsupportedOperation);
}

TEST_CASE("A boundary aligned callback needs every ceil B over H frame slot",
          "[host][spectral-ports][spectral-max-batch]") {
    FaultGraph f(63, 4096);
    std::array<float, 4096> input{}, output{};
    f.process(f.analysis, std::span(input).first(3), std::span(output).first(3));
    f.bank.begin_block();
    f.process(f.analysis, input, output);
    const auto* node = f.bank.find(f.analysis);
    REQUIRE(node->outputs.front()->count == 66);
    REQUIRE_FALSE(node->outputs.front()->fatal);
    REQUIRE(node->first_error == pulp::host::SpectralError::None);
}

TEST_CASE("Unsolicited duplicate frames never advance the scheduled grid",
          "[host][spectral-fault][spectral-duplicate]") {
    FaultGraph f;
    std::array<float, 64> input{}, output{};
    f.process(f.analysis, input, output);
    f.process(f.stage, input, output);
    auto* stage = f.bank.find(f.stage);
    REQUIRE(stage->sequence == 1);
    f.bank.begin_block();
    stage->input->count = 1; // Repeat the previous accepted descriptor/payload.
    f.process(f.stage, std::span(input).first(1), std::span(output).first(1));
    REQUIRE(stage->sequence == 1);
    REQUIRE(stage->first_error == pulp::host::SpectralError::InvalidFrame);
    REQUIRE(stage->outputs.front()->count == 0);
}

TEST_CASE("Mailbox refusal preserves ingress and egress exactly once",
          "[host][spectral-transition][spectral-mailbox]") {
    using namespace pulp::host;
    ControllerGraph target(true);
    auto candidate = target.edit();
    pulp::midi::MidiBuffer events, extracted;
    REQUIRE(events.add(pulp::midi::MidiEvent::note_on(0, 60, 100)));
    REQUIRE(target.graph.inject_midi(target.midi_in, events));
    REQUIRE(candidate->commit() == SignalGraph::PreparedTopologyEdit::Result::Staged);
    REQUIRE_FALSE(target.graph.inject_midi(target.midi_in, events));
    target.process();
    REQUIRE(target.graph.spectral_transition_state() ==
            SignalGraph::SpectralTransitionState::Rejected);
    REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::PendingMailboxData);
    candidate = target.edit();
    REQUIRE(candidate->commit() == SignalGraph::PreparedTopologyEdit::Result::Staged);
    target.process(); // Undrained output also refuses replacement.
    REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::PendingMailboxData);
    REQUIRE(target.graph.extract_midi(target.midi_out, extracted));
    REQUIRE(extracted.size() == 1);
    extracted.clear();
    REQUIRE(target.graph.extract_midi(target.midi_out, extracted));
    REQUIRE(extracted.empty());
    candidate = target.edit();
    REQUIRE(candidate->commit() == SignalGraph::PreparedTopologyEdit::Result::Staged);
    target.process();
    REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::None);
    REQUIRE(target.graph.extract_midi(target.midi_out, extracted));
    REQUIRE(extracted.empty());
}

TEST_CASE("Active spectral controllers refuse teardown and graph loading",
          "[host][spectral-transition][spectral-load-refusal]") {
    ControllerGraph target, control;
    const auto node_count = target.graph.nodes().size();
    target.graph.release();
    target.graph.clear();
    REQUIRE(target.graph.nodes().size() == node_count);
    pulp::host::SignalGraph ordinary;
    ordinary.add_input_node(1);
    ordinary.add_output_node(1);
    const auto json = pulp::host::GraphSerializer::to_json(ordinary);
    const auto result = pulp::host::GraphSerializer::from_json(target.graph, json);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("spectral") != std::string::npos);
    REQUIRE(target.graph.nodes().size() == node_count);
    target.process();
    control.process();
    REQUIRE(target.output == control.output);
}

TEST_CASE("Live custom state cannot invalidate spectral publication",
          "[host][spectral-transition][spectral-custom-state]") {
    ControllerGraph target(false, true), control(false, true);
    const auto latency = target.graph.latency_samples();
    REQUIRE(latency == 320);
    REQUIRE_FALSE(target.graph.set_custom_node_state(target.custom, {1, 2, 3}));
    REQUIRE_FALSE(pulp::host::register_builtin_sample_region_types(target.graph));
    REQUIRE(target.graph.custom_node_state(target.custom).empty());
    REQUIRE(target.graph.latency_samples() == latency);
    target.process();
    control.process();
    REQUIRE(target.output == control.output);
}
