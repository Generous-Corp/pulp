// I2 parity for the in-process ProcessorNode adapter.
//
// The payoff this file proves: the SAME pulp::format::Processor produces
// bit-identical output whether it is driven standalone (HeadlessHost, the
// oracle) or as a node inside the routed graph (GraphRuntimeExecutor::
// process_routed driving ProcessorNode::process_binding, the implementation).
// A divergence here means the node adapter is not a faithful in-graph stand-in
// for the standalone processor.
//
// The oracle deliberately uses HeadlessHost, NOT process_processor_block, so the
// comparison is not circular: ProcessorNode is built on process_processor_block,
// while HeadlessHost reaches the processor through the independent ProcessBuffers
// process() path.
//
// Scope mirrors the adapter: mono audio in -> out, no params/MIDI/state recall.

#include "harness/graph_routing_harness.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/state/store.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using pulp::format::GraphRuntimeExecutor;
using pulp::format::GraphRuntimeNodeBinding;
using pulp::format::GraphRuntimeSnapshot;
using pulp::format::HeadlessHost;
using pulp::format::PluginCategory;
using pulp::format::PluginDescriptor;
using pulp::format::PrepareContext;
using pulp::format::ProcessContext;
using pulp::format::Processor;
using pulp::format::ProcessorNode;
using pulp::graph::GraphRuntimeConnectionSpec;
using pulp::graph::GraphRuntimeNodeKind;
using pulp::graph::GraphRuntimeNodeSpec;

using pulp::test::graph_routing::make_pool;
using pulp::test::graph_routing::make_snapshot;
using pulp::test::graph_routing::RoutedHarness;
using pulp::test::graph_routing::test_signal;

constexpr double kSr = 48000.0;
constexpr float kPole = 0.3f;  // fixed coefficient -> sample-rate independent

// Deterministic, stateful, parameter-free mono processor. A one-pole low-pass
// with a fixed coefficient: output depends on input history but NOT on transport
// context, parameters, or the exact sample rate, so the two drive paths must
// agree bit-for-bit as long as each starts from a freshly prepared (zeroed)
// state and sees the same input block.
class OnePoleMono : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "OnePoleMono";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.onepolemono";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Main In", 1, false}};
        d.output_buses = {{"Main Out", 1, false}};
        return d;
    }

    void define_parameters(pulp::state::StateStore&) override {}

    void prepare(const PrepareContext&) override { z_ = 0.0f; }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ProcessContext&) override {
        if (output.num_channels() == 0) return;
        const std::size_t frames = output.num_samples();
        float* out = output.channel_ptr(0);
        const float* in =
            input.num_channels() > 0 ? input.channel_ptr(0) : nullptr;
        for (std::size_t i = 0; i < frames; ++i) {
            const float x = in ? in[i] : 0.0f;
            z_ = kPole * x + (1.0f - kPole) * z_;
            out[i] = z_;
        }
    }

    static std::unique_ptr<Processor> create() {
        return std::make_unique<OnePoleMono>();
    }

private:
    float z_ = 0.0f;
};

// Standalone oracle: run one mono block of `x` through a freshly prepared
// HeadlessHost and return its output.
std::vector<float> oracle_block(const std::vector<float>& x, int frames) {
    HeadlessHost host(&OnePoleMono::create);
    host.prepare(kSr, frames, /*input_channels=*/1, /*output_channels=*/1);

    std::vector<float> in = x;
    std::vector<float> out(static_cast<std::size_t>(frames), 0.0f);
    std::array<const float*, 1> in_ch{in.data()};
    std::array<float*, 1> out_ch{out.data()};
    pulp::audio::BufferView<const float> in_view(
        in_ch.data(), 1, static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> out_view(
        out_ch.data(), 1, static_cast<std::uint32_t>(frames));
    host.process(out_view, in_view);
    return out;
}

// Bind a standalone Processor instance to a StateStore the way the framework
// would before processing. The processor and store are owned by the caller.
void bind_processor(Processor& processor, pulp::state::StateStore& store) {
    processor.set_state_store(&store);
    processor.define_parameters(store);
}

// in (AudioInput, mono) -> ProcessorNode (Processor, 1->1) -> out (AudioOutput).
const std::array kNodes = {
    GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
    GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
    GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
};
const std::array kConns = {
    GraphRuntimeConnectionSpec{1, 0, 2, 0},
    GraphRuntimeConnectionSpec{2, 0, 3, 0},
};

} // namespace

TEST_CASE("ProcessorNode in a routed graph matches the standalone processor",
          "[format][graph][executor][routing][processor-node][parity][i2]") {
    GraphRuntimeExecutor exec;
    for (int frames : {1, 64, 256}) {
        for (float seed : {0.8f, 0.35f}) {
            CAPTURE(frames, seed);
            const auto x = test_signal(frames, seed);

            // Implementation path: a real processor wrapped as a graph node.
            OnePoleMono processor;
            pulp::state::StateStore store;
            bind_processor(processor, store);
            ProcessorNode node(processor);
            PrepareContext prepare_ctx;
            prepare_ctx.sample_rate = kSr;
            prepare_ctx.max_buffer_size = frames;
            prepare_ctx.input_channels = 1;
            prepare_ctx.output_channels = 1;
            REQUIRE(node.prepare(prepare_ctx));

            const std::array bindings = {
                GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
                GraphRuntimeNodeBinding{2, ProcessorNode::process_binding, &node, true},
                GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
            };
            GraphRuntimeSnapshot snapshot;
            REQUIRE(make_snapshot(snapshot, kNodes, kConns, bindings));
            auto pool = make_pool(snapshot, frames);

            const std::vector<std::vector<float>> in{x};
            RoutedHarness h(kSr, frames, in, /*out_channels=*/1);
            REQUIRE(h.run(exec, snapshot, pool).ok());

            const auto ref = oracle_block(x, frames);
            REQUIRE(ref.size() == h.outs[0].size());
            for (std::size_t i = 0; i < ref.size(); ++i) {
                // Both paths reach the identical Processor::process over the same
                // input from a zeroed state, so this is bit-exact, not toleranced.
                REQUIRE(ref[i] == h.outs[0][i]);
            }
        }
    }
}

TEST_CASE("ProcessorNode binding processes audio without the routed view it requires",
          "[format][graph][executor][routing][processor-node]") {
    // The binding implements only the routed contract; on the shared-block path
    // (routed == false) it must refuse rather than misread the block's buses.
    OnePoleMono processor;
    pulp::state::StateStore store;
    bind_processor(processor, store);
    ProcessorNode node(processor);
    PrepareContext prepare_ctx;
    prepare_ctx.sample_rate = kSr;
    prepare_ctx.max_buffer_size = 64;
    prepare_ctx.input_channels = 1;
    prepare_ctx.output_channels = 1;
    REQUIRE(node.prepare(prepare_ctx));

    pulp::format::ProcessBlock block;
    block.sample_rate = kSr;
    block.frame_count = 64;
    pulp::format::GraphRuntimeNodeProcessContext ctx;
    ctx.routed = false;
    REQUIRE_FALSE(ProcessorNode::process_binding(block, ctx, &node));
}

TEST_CASE("ProcessorNode routed binding is allocation-free after warm-up",
          "[format][graph][executor][routing][processor-node][rt-safety]") {
    constexpr int kFrames = 256;
    GraphRuntimeExecutor exec;

    OnePoleMono processor;
    pulp::state::StateStore store;
    bind_processor(processor, store);
    ProcessorNode node(processor);
    PrepareContext prepare_ctx;
    prepare_ctx.sample_rate = kSr;
    prepare_ctx.max_buffer_size = kFrames;
    prepare_ctx.input_channels = 1;
    prepare_ctx.output_channels = 1;
    REQUIRE(node.prepare(prepare_ctx));

    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, ProcessorNode::process_binding, &node, true},
        GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, kNodes, kConns, bindings));
    auto pool = make_pool(snapshot, kFrames);

    const std::vector<std::vector<float>> in{test_signal(kFrames, 0.8f)};
    RoutedHarness h(kSr, kFrames, in, 1);
    REQUIRE(h.run(exec, snapshot, pool).ok());  // warm-up outside the probe
    {
        pulp::test::RtAllocationProbe probe;
        const auto result = h.run(exec, snapshot, pool);
        REQUIRE(result.ok());
        REQUIRE_FALSE(probe.saw_allocation());
    }
}
