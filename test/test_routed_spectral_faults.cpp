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
    edge->stamps[1].epoch = 99;
    f.process(f.stage, input, output);
    REQUIRE(f.bank.find(f.stage)->sequence == 8);
    REQUIRE(f.bank.find(f.stage)->first_error == pulp::host::SpectralError::MissingFrame);
    REQUIRE(f.bank.find(f.stage)->errors == 1);
    REQUIRE(std::equal(saved.begin(), saved.end(), sibling->bins.begin()));
    const auto* replaced = f.bank.find(f.wet)->input;
    REQUIRE(std::equal(saved.begin(), saved.begin() + replaced->bins_per_frame,
                       replaced->bins.begin()));
    for (size_t i = replaced->bins_per_frame; i < 2 * replaced->bins_per_frame; ++i)
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
    static bool swap_edit_active(SignalGraph& g) {
        return g.in_swap_edit_;
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
    ControllerGraph(bool with_midi = false, bool with_custom = false, bool optimizations = false) {
        using namespace pulp::host;
        graph.set_parallel_routing_enabled(optimizations);
        graph.set_anticipation_enabled(optimizations);
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

TEST_CASE("Legacy swap entry refuses a spectral controller without changing its generation",
          "[host][spectral-transition][spectral-swap-refusal]") {
    using namespace pulp::host;
    SignalGraph ordinary;
    ordinary.begin_swap_edit();
    REQUIRE(SpectralControllerTestAccess::swap_edit_active(ordinary));
    ordinary.abort_swap_edit();
    REQUIRE_FALSE(SpectralControllerTestAccess::swap_edit_active(ordinary));

    ControllerGraph target, control;
    auto candidate = target.graph.begin_prepared_topology_edit();
    REQUIRE(candidate);
    target.graph.begin_swap_edit();
    REQUIRE_FALSE(SpectralControllerTestAccess::swap_edit_active(target.graph));
    REQUIRE(target.graph.spectral_error() == SpectralError::UnsupportedOperation);
    REQUIRE(candidate->prepare(48000, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    for (int block = 0; block < 12; ++block) {
        target.process();
        control.process();
        REQUIRE(target.output == control.output);
    }
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

TEST_CASE("Frame tuples are partition invariant and restart on a fresh bank epoch",
          "[host][spectral-fault][spectral-tuples]") {
    using pulp::graph::SpectralFrameStamp;
    const auto collect = [](int partition, std::uint64_t epoch) {
        FaultGraph f(63, 4096);
        f.bank.reset(epoch);
        std::vector<SpectralFrameStamp> stamps;
        std::vector<float> input(8192, 0.25f), scratch(4096);
        for (int pos = 0; pos < 8192;) {
            const int count = std::min(8192 - pos, partition ? partition : 1 + pos * 17 % 4096);
            f.bank.begin_block();
            f.process(f.analysis, std::span(input).subspan(pos, count),
                      std::span(scratch).first(count));
            const auto* batch = f.bank.find(f.stage)->input;
            stamps.insert(stamps.end(), batch->stamps.begin(),
                          batch->stamps.begin() + batch->count);
            REQUIRE(f.bank.find(f.analysis)->first_error == pulp::host::SpectralError::None);
            pos += count;
        }
        return stamps;
    };
    const auto fixed = collect(64, 17);
    REQUIRE_FALSE(fixed.empty());
    REQUIRE(collect(4096, 17) == fixed);
    REQUIRE(collect(0, 17) == fixed);
    const auto fresh = collect(0, 18);
    REQUIRE(fresh.size() == fixed.size());
    const auto first = pulp::signal::spectral_frame_engine_first_frame_start(256, 63, true);
    for (std::size_t i = 0; i < fixed.size(); ++i) {
        const SpectralFrameStamp expected{17, i, first + static_cast<std::int64_t>(i) * 63,
                                          first + static_cast<std::int64_t>(i) * 63 + 256};
        REQUIRE(fixed[i] == expected);
        auto next = expected;
        next.epoch = 18;
        REQUIRE(fresh[i] == next);
    }
}

TEST_CASE("Distinct spectral controllers retain distinct epoch namespaces across reset",
          "[host][spectral-transition][spectral-namespace]") {
    using namespace pulp::host;
    ControllerGraph first, second;
    const auto epoch = [](ControllerGraph& g) {
        auto reader = SpectralControllerTestAccess::reader(g.graph);
        REQUIRE(reader);
        return reader->spectral->nodes.front()->epoch;
    };
    const auto first_before = epoch(first), second_before = epoch(second);
    REQUIRE(first_before != second_before);
    REQUIRE((first_before >> 32) != (second_before >> 32));
    pulp::format::ProcessContext reset;
    reset.reset_requested = true;
    first.process(&reset);
    second.process(&reset);
    REQUIRE(epoch(first) == first_before + 1);
    REQUIRE(epoch(second) == second_before + 1);
    REQUIRE(epoch(first) != epoch(second));
}

TEST_CASE("Spectral sequence exhaustion refuses before publishing a wrapped successor",
          "[host][spectral-fault][spectral-sequence-limit]") {
    FaultGraph f;
    auto* node = f.bank.find(f.analysis);
    node->sequence = std::numeric_limits<std::uint64_t>::max();
    const auto origin = node->origin;
    REQUIRE_FALSE(node->advance());
    REQUIRE(node->sequence == std::numeric_limits<std::uint64_t>::max());
    REQUIRE(node->origin == origin);
    REQUIRE(node->first_error == pulp::host::SpectralError::ChronologyExhausted);
    REQUIRE(node->outputs.front()->fatal);
}

TEST_CASE("Spectral retained storage cap rejects a bounded oversized candidate",
          "[host][spectral-fault][spectral-memory-cap]") {
    using namespace pulp::host;
    for (int stages : {1, 17}) {
        SignalGraph graph;
        const auto shape = *SpectralIdentity::make(256, 1, 1, 48000).value;
        const auto analysis = *graph.add_spectral_analysis(shape, "analysis").value;
        const auto synthesis = *graph.add_spectral_synthesis(shape, "synthesis").value;
        auto previous = analysis.frames;
        for (int i = 0; i < stages; ++i) {
            const auto stage =
                *graph
                     .add_spectral_stage(shape, *QualifiedSpectralStage::identity_v1(shape).value,
                                         "identity")
                     .value;
            REQUIRE(graph.connect(previous, stage.frames_in));
            previous = stage.frames_out;
        }
        REQUIRE(graph.connect(previous, synthesis.frames));
        SpectralRuntimeBank bank;
        const auto result = bank.prepare(graph.nodes(), graph.connections(), 48000, 4096);
        REQUIRE(result == (stages == 1 ? SpectralError::None : SpectralError::CapacityExceeded));
        REQUIRE(bank.charged_bytes <= SpectralRuntimeBank::memory_limit);
    }
}

TEST_CASE("Untimed spectral telemetry keeps sticky errors and labels records retained across reset",
          "[host][spectral-transition][spectral-telemetry]") {
    using namespace pulp::host;
    ControllerGraph target;
    REQUIRE_FALSE(target.graph.live_dsp_telemetry_enabled());
    target.input.fill(std::numeric_limits<float>::quiet_NaN());
    for (int i = 0; i < 8; ++i)
        target.process();
    const auto failed = target.graph.poll_live_dsp_telemetry();
    REQUIRE(failed.available);
    REQUIRE_FALSE(failed.enabled);
    std::uint64_t failed_node = 0, old_epoch = 0, error_count = 0;
    std::int64_t error_position = 0;
    for (const auto& diagnostic : failed.spectral) {
        if (diagnostic.first_error == static_cast<std::uint8_t>(SpectralError::InvalidFrame)) {
            failed_node = diagnostic.node;
            old_epoch = diagnostic.epoch;
            error_count = diagnostic.errors;
            error_position = diagnostic.first_error_position;
            break;
        }
    }
    REQUIRE(failed_node != 0);
    REQUIRE(error_count > 0);
    target.input.fill(0.25f);
    for (int i = 0; i < 300; ++i)
        target.process();
    pulp::format::ProcessContext reset;
    reset.reset_requested = true;
    target.process(&reset);
    const auto retained = target.graph.poll_live_dsp_telemetry();
    REQUIRE(retained.blocks_dropped > 0);
    REQUIRE(retained.age_blocks > 0);
    bool found = false;
    for (const auto& diagnostic : retained.spectral) {
        if (diagnostic.node != failed_node)
            continue;
        found = true;
        REQUIRE(diagnostic.epoch == old_epoch);
        REQUIRE(diagnostic.first_error == static_cast<std::uint8_t>(SpectralError::InvalidFrame));
        REQUIRE(diagnostic.first_error_position == error_position);
        REQUIRE(diagnostic.errors >= error_count);
    }
    REQUIRE(found);
    target.process();
    const auto fresh = target.graph.poll_live_dsp_telemetry();
    REQUIRE(fresh.available);
    REQUIRE(fresh.age_blocks == 0);
    REQUIRE(fresh.sequence_number > retained.sequence_number);
    for (const auto& diagnostic : fresh.spectral) {
        if (!diagnostic.node)
            continue;
        REQUIRE(diagnostic.epoch == old_epoch + 1);
        REQUIRE(diagnostic.first_error == 0);
        REQUIRE(diagnostic.errors == 0);
    }
}

TEST_CASE("Retained legacy processing handles prevent spectral conversion until released",
          "[host][spectral-transition][spectral-retained-handle]") {
    using namespace pulp::host;
    SignalGraph graph;
    const auto input = graph.add_input_node(1), output = graph.add_output_node(1);
    REQUIRE(graph.connect(input, 0, output, 0));
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->prepare(48000, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    auto retained = edit->committed_execution_snapshot();
    REQUIRE(retained);
    edit.reset();
    graph.release();
    graph.clear();
    const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
    const auto analysis = *graph.add_spectral_analysis(shape, "analysis").value;
    const auto synthesis = *graph.add_spectral_synthesis(shape, "synthesis").value;
    REQUIRE(graph.connect(analysis.frames, synthesis.frames));
    REQUIRE_FALSE(graph.prepare(48000, 64));
    REQUIRE(graph.spectral_error() == SpectralError::RequiresStoppedController);
    std::array<float, 64> in, out{};
    in.fill(0.25f);
    const float* ip[] = {in.data()};
    float* op[] = {out.data()};
    pulp::audio::BufferView<const float> iv{ip, 1, 64};
    pulp::audio::BufferView<float> ov{op, 1, 64};
    retained.process(ov, iv, 64);
    REQUIRE(out == in);
    retained = {};
    REQUIRE(graph.prepare(48000, 64));
}

TEST_CASE("Spectral pinned execution refusal is named at every handoff phase",
          "[host][spectral-transition][spectral-pinned-diagnostic]") {
    using namespace pulp::host;
    ControllerGraph target;
    auto edit = target.edit();
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Staged);
    REQUIRE_FALSE(edit->committed_execution_snapshot());
    REQUIRE(target.graph.spectral_error() == SpectralError::SpectralPinnedExecutionUnsupported);
    target.process();
    REQUIRE_FALSE(edit->committed_execution_snapshot());
    REQUIRE(target.graph.spectral_error() == SpectralError::SpectralPinnedExecutionUnsupported);
    REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::None);
    REQUIRE_FALSE(edit->committed_execution_snapshot());
    REQUIRE(target.graph.spectral_error() == SpectralError::SpectralPinnedExecutionUnsupported);
}

TEST_CASE("Spectral preparation excludes alternate executors despite optimization requests",
          "[host][spectral-transition][spectral-serial-eligibility]") {
    using namespace pulp::host;
    ControllerGraph target(false, false, true), control;
    {
        auto snapshot = SpectralControllerTestAccess::reader(target.graph);
        REQUIRE(snapshot);
        REQUIRE(snapshot->pdc_execution_domain ==
                decltype(snapshot->pdc_execution_domain)::RoutedSerial);
        REQUIRE(snapshot->routed.serial.valid);
        REQUIRE_FALSE(snapshot->routed.parallel.valid);
        REQUIRE_FALSE(snapshot->anticipation.valid);
        const auto& plan = snapshot->routed.serial.snapshot.plan();
        const auto& delays =
            snapshot->routed.serial.snapshot.buffer_assignment().connection_delay_samples;
        REQUIRE(delays.size() == plan.connections.size());
        for (std::size_t i = 0; i < plan.connections.size(); ++i)
            if (pulp::graph::is_spectral(plan.connections[i]))
                REQUIRE(delays[i] == 0);
    }
    for (int block = 0; block < 12; ++block) {
        target.graph.set_parallel_routing_enabled(block % 2 == 0);
        target.graph.set_anticipation_enabled(block % 2 != 0);
        target.process();
        control.process();
        REQUIRE(target.output == control.output);
        REQUIRE(target.graph.routed_walk_fallbacks() == 0);
    }
}

TEST_CASE(
    "An old metadata reader pins an unchanged retired spectral arena while new audio advances",
    "[host][spectral-transition][spectral-retired-reader]") {
    using namespace pulp::host;
    ControllerGraph target, control;
    auto candidate = target.edit();
    REQUIRE(candidate->commit() == SignalGraph::PreparedTopologyEdit::Result::Staged);
    auto reference_candidate = control.edit();
    REQUIRE(reference_candidate->commit() == SignalGraph::PreparedTopologyEdit::Result::Staged);
    {
        auto retired = SpectralControllerTestAccess::reader(target.graph);
        REQUIRE(retired);
        const auto old_epoch = retired->spectral->nodes.front()->epoch;
        const auto old_sequence = retired->spectral->nodes.front()->sequence;
        target.process();
        control.process();
        REQUIRE(target.output == control.output);
        {
            auto active = SpectralControllerTestAccess::reader(target.graph);
            REQUIRE(active);
            REQUIRE(active.get() != retired.get());
            REQUIRE(active->spectral.get() != retired->spectral.get());
            REQUIRE(active->spectral->nodes.front()->epoch == old_epoch + 1);
        }
        REQUIRE(retired->spectral->nodes.front()->epoch == old_epoch);
        REQUIRE(retired->spectral->nodes.front()->sequence == old_sequence);
        REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::Busy);
        REQUIRE(control.graph.finalize_spectral_transition() == SpectralError::None);
        for (int block = 0; block < 12; ++block) {
            target.process();
            control.process();
            REQUIRE(target.output == control.output);
            REQUIRE(retired->spectral->nodes.front()->epoch == old_epoch);
            REQUIRE(retired->spectral->nodes.front()->sequence == old_sequence);
        }
    }
    REQUIRE(target.graph.finalize_spectral_transition() == SpectralError::None);
}

TEST_CASE("Failed and cancelled preparations preserve active waveform without a reset",
          "[host][spectral-transition][spectral-candidate-isolation]") {
    using namespace pulp::host;
    for (int disposition : {0, 1, 2}) {
        ControllerGraph target, control;
        const auto before_epoch = SpectralControllerTestAccess::epoch(target.graph);
        {
            auto candidate = target.graph.begin_prepared_topology_edit();
            REQUIRE(candidate);
            if (disposition == 0)
                REQUIRE(candidate->prepare(44100, 64) !=
                        SignalGraph::PreparedTopologyEdit::Result::Prepared);
            else if (disposition == 1)
                REQUIRE(candidate->prepare(48000, 64) ==
                        SignalGraph::PreparedTopologyEdit::Result::Prepared);
        }
        REQUIRE(SpectralControllerTestAccess::epoch(target.graph) == before_epoch);
        REQUIRE(target.graph.spectral_transition_state() ==
                SignalGraph::SpectralTransitionState::Idle);
        target.input.fill(0);
        control.input.fill(0);
        for (int block = 0; block < 12; ++block) {
            target.process();
            control.process();
            REQUIRE(target.output == control.output);
            REQUIRE(SpectralControllerTestAccess::epoch(target.graph) ==
                    SpectralControllerTestAccess::epoch(control.graph));
        }
    }
}

TEST_CASE("Failed or cancelled candidates paused around validation cannot change an active seek",
          "[host][spectral-transition][spectral-validation-reset]") {
    using namespace pulp::host;
    for (bool pause_after_validation : {false, true}) {
        for (bool fail_validation : {false, true}) {
            ControllerGraph target, control;
            const auto node_count = target.graph.nodes().size();
            const auto edge_count = target.graph.connections().size();
            auto candidate = target.graph.begin_prepared_topology_edit();
            REQUIRE(candidate);
            std::latch paused{1}, resume{1};
            SignalGraph::PreparedTopologyEdit::Result result{};
            std::thread preparer([&] {
                if (!pause_after_validation) {
                    paused.count_down();
                    resume.wait();
                }
                result = candidate->prepare(fail_validation ? 44100 : 48000, 64);
                if (pause_after_validation) {
                    paused.count_down();
                    resume.wait();
                }
                candidate.reset();
            });
            paused.wait();
            pulp::format::ProcessContext seek;
            seek.transport_jump = true;
            target.process(&seek);
            control.process(&seek);
            const auto actual = target.output;
            const auto actual_epoch = SpectralControllerTestAccess::epoch(target.graph);
            resume.count_down();
            preparer.join();
            REQUIRE((result == SignalGraph::PreparedTopologyEdit::Result::Prepared) ==
                    !fail_validation);
            REQUIRE(actual == control.output);
            REQUIRE(actual_epoch == SpectralControllerTestAccess::epoch(control.graph));
            REQUIRE(SpectralControllerTestAccess::epoch(target.graph) ==
                    SpectralControllerTestAccess::epoch(control.graph));
            REQUIRE(target.graph.nodes().size() == node_count);
            REQUIRE(target.graph.connections().size() == edge_count);
            for (int block = 0; block < 12; ++block) {
                target.process();
                control.process();
                REQUIRE(target.output == control.output);
            }
        }
    }
}

TEST_CASE("Output clock exhaustion stops publication before arithmetic overflows",
          "[host][spectral-fault][spectral-output-clock]") {
    FaultGraph f;
    std::array<float, 512> input{}, output{};
    auto* node = f.bank.find(f.analysis);
    node->output_clock = std::numeric_limits<std::int64_t>::max() - 1;
    const auto sequence = node->sequence;
    f.process(f.analysis, input, output);
    REQUIRE(node->first_error == pulp::host::SpectralError::ChronologyExhausted);
    REQUIRE(node->sequence == sequence);
    REQUIRE(node->outputs.front()->count == 0);
    REQUIRE(node->outputs.front()->fatal);
}

TEST_CASE("Failed candidate at the last transition serial cannot consume or wrap it",
          "[host][spectral-transition][spectral-failed-exhaustion]") {
    using namespace pulp::host;
    ControllerGraph target, control;
    SpectralControllerTestAccess::exhaust(target.graph);
    SpectralControllerTestAccess::exhaust(control.graph);
    {
        auto candidate = target.graph.begin_prepared_topology_edit();
        REQUIRE(candidate);
        REQUIRE(candidate->prepare(44100, 64) !=
                SignalGraph::PreparedTopologyEdit::Result::Prepared);
    }
    REQUIRE(SpectralControllerTestAccess::epoch(target.graph) == UINT32_MAX);
    for (int block = 0; block < 12; ++block) {
        target.process();
        control.process();
        REQUIRE(target.output == control.output);
        REQUIRE_FALSE(SpectralControllerTestAccess::terminal(target.graph));
    }
    pulp::format::ProcessContext reset;
    reset.transport_jump = true;
    target.process(&reset);
    control.process(&reset);
    REQUIRE(target.output == control.output);
    REQUIRE(SpectralControllerTestAccess::terminal(target.graph));
    REQUIRE(target.graph.spectral_error() == SpectralError::EpochExhausted);
    REQUIRE(SpectralControllerTestAccess::epoch(target.graph) == UINT32_MAX);
}

TEST_CASE("Candidate preparation revalidates owned mask values independently of factories",
          "[host][spectral-fault][spectral-config-revalidation]") {
    using namespace pulp::host;
    FaultGraph f;
    const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
    for (float gain : {0.0f, 1.0f, -0.25f, 1.25f, std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::quiet_NaN()}) {
        auto nodes = f.graph.nodes();
        std::vector<float> gains(shape.bins(), 0.5f);
        auto token = *QualifiedSpectralStage::real_mask_v1(shape, gains).value;
        // The token is a mutable local object. Corrupt its owned payload to
        // exercise the independent preparation validator, bypassing its factory.
        const_cast<float*>(token.gains().data())[3] = gain;
        for (auto& node : nodes)
            if (node.id == f.stage)
                node.spectral_stage = token;
        SpectralRuntimeBank candidate;
        const auto result = candidate.prepare(nodes, f.graph.connections(), 48000, 512);
        REQUIRE(result == ((std::isfinite(gain) && gain >= 0 && gain <= 1)
                               ? SpectralError::None
                               : SpectralError::InvalidMask));
    }
}
