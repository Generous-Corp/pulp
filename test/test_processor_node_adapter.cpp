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
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
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

using pulp::test::graph_routing::GainState;
using pulp::test::graph_routing::make_pool;
using pulp::test::graph_routing::make_snapshot;
using pulp::test::graph_routing::RoutedHarness;
using pulp::test::graph_routing::routing_gain;
using pulp::test::graph_routing::test_signal;

constexpr double kSr = 48000.0;
constexpr float kPole = 0.3f; // fixed coefficient -> sample-rate independent

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

    void prepare(const PrepareContext&) override {
        z_ = 0.0f;
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const ProcessContext&) override {
        if (output.num_channels() == 0)
            return;
        const std::size_t frames = output.num_samples();
        float* out = output.channel_ptr(0);
        const float* in = input.num_channels() > 0 ? input.channel_ptr(0) : nullptr;
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

class SidechainProbe final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "SidechainProbe";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.sidechainprobe";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {
            {"Main In", 1, false, pulp::format::BusRole::Main},
            // Deliberately use the legacy three-field form.  Optional
            // secondary inputs must retain their historical sidechain role.
            {"Sidechain", 1, true},
        };
        d.output_buses = {{"Main Out", 1, false}};
        return d;
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&, const ProcessContext&) override {
    }

    void process(pulp::format::ProcessBuffers& audio, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const ProcessContext&) override {
        auto* main = audio.main_input();
        auto* sidechain = audio.sidechain_input();
        auto* output = audio.main_output();
        if (!output)
            return;
        for (std::size_t frame = 0; frame < output->num_samples(); ++frame) {
            const float main_sample = main ? main->channel_ptr(0)[frame] : 0.0f;
            const float side_sample = sidechain ? sidechain->channel_ptr(0)[frame] : 0.0f;
            output->channel_ptr(0)[frame] = main_sample + side_sample;
        }
    }
};

struct RouteProviderFixture {
    pulp::audio::BufferView<const float> route;
    std::size_t calls = 0;

    static bool read(void* context, std::size_t bus_index, std::size_t frame_count,
                     pulp::audio::BufferView<const float>& output) noexcept {
        auto& fixture = *static_cast<RouteProviderFixture*>(context);
        ++fixture.calls;
        if (bus_index != 1 || fixture.route.num_samples() != frame_count ||
            fixture.route.num_channels() != 1)
            return false;
        output = fixture.route;
        return true;
    }
};

class DenseGainMono final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "DenseGainMono";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.densegainmono";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Main In", 1, false}};
        d.output_buses = {{"Main Out", 1, false}};
        d.node_capabilities.consumes_audio_rate_modulations = true;
        return d;
    }

    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo gain;
        gain.id = 7;
        gain.name = "Dense Gain";
        gain.range = {0.0f, 1.0f, 0.5f};
        gain.rate = pulp::state::ParamRate::AudioRate;
        store.add_parameter(gain);
    }

    void prepare(const PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&, const ProcessContext&) override {
    }

    bool process_block(pulp::format::ProcessBlock& block) override {
        if (block.buses == nullptr || block.events == nullptr ||
            block.events->audio_rate_modulations.size() != 1 ||
            block.events->parameter_event_count() != expected_sparse_events) {
            return false;
        }
        const auto& lane = block.events->audio_rate_modulations.front();
        if (lane.param_id != 7 || lane.values.size() != block.frame_count ||
            block.frame_count > last_lane.size())
            return false;
        const auto* input =
            block.buses->first(pulp::format::BusDirection::Input, pulp::format::BusRole::Main);
        auto* output =
            block.buses->first(pulp::format::BusDirection::Output, pulp::format::BusRole::Main);
        if (input == nullptr || output == nullptr)
            return false;
        for (std::uint32_t frame = 0; frame < block.frame_count; ++frame) {
            last_lane[frame] = lane.values[frame];
            output->output.channel_ptr(0)[frame] =
                input->input.channel_ptr(0)[frame] * lane.values[frame];
        }
        last_lane_count = block.frame_count;
        saw_dense = true;
        saw_sparse = expected_sparse_events > 0;
        return true;
    }

    bool saw_dense = false;
    bool saw_sparse = false;
    std::size_t expected_sparse_events = 0;
    std::array<float, 4096> last_lane{};
    std::uint32_t last_lane_count = 0;
};

class LifecycleProcessor final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "LifecycleProcessor";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.lifecycleprocessor";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Main In", 1, false}};
        d.output_buses = {{"Main Out", 1, false}};
        return d;
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {
        ++prepare_count;
        if (fail_prepare)
            throw std::runtime_error("prepare failure");
    }
    void release() override {
        ++release_count;
    }
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&, const ProcessContext&) override {
    }

    int prepare_count = 0;
    int release_count = 0;
    bool fail_prepare = false;
};

class FailingDenseMono final : public Processor {
  public:
    explicit FailingDenseMono(bool throws) : throws_(throws) {}

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "FailingDenseMono";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.failingdensemono";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Main In", 1, false}};
        d.output_buses = {{"Main Out", 1, false}};
        d.node_capabilities.consumes_audio_rate_modulations = true;
        return d;
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&, const ProcessContext&) override {
    }

    bool process_block(pulp::format::ProcessBlock& block) override {
        auto* output = block.buses == nullptr
                           ? nullptr
                           : block.buses->first(pulp::format::BusDirection::Output,
                                                pulp::format::BusRole::Main);
        if (output == nullptr)
            return false;
        std::fill_n(output->output.channel_ptr(0), block.frame_count, 1.0f);
        if (throws_)
            throw std::runtime_error("dense failure");
        return false;
    }

  private:
    bool throws_ = false;
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
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 1,
                                                 static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> out_view(out_ch.data(), 1, static_cast<std::uint32_t>(frames));
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

TEST_CASE("ProcessorNode routes a pre-bound host bus into a sidechain") {
    constexpr int frames = 8;
    SidechainProbe processor;
    pulp::state::StateStore store;
    bind_processor(processor, store);
    ProcessorNode node(processor);
    PrepareContext prepare;
    prepare.sample_rate = kSr;
    prepare.max_buffer_size = frames;
    prepare.input_channels = 1;
    prepare.output_channels = 1;
    REQUIRE(node.prepare(prepare));

    std::array<float, frames> main{};
    std::array<float, frames> route{};
    std::array<float, frames> output{};
    std::array<const float*, 1> main_channels{main.data()};
    std::array<const float*, 1> route_channels{route.data()};
    std::array<float*, 1> output_channels{output.data()};
    for (int frame = 0; frame < frames; ++frame) {
        main[frame] = static_cast<float>(frame + 1);
        route[frame] = static_cast<float>(10 + frame);
    }
    RouteProviderFixture provider;
    provider.route = pulp::audio::BufferView<const float>(route_channels.data(), 1, frames);
    node.set_audio_input_provider({&provider, &RouteProviderFixture::read});

    pulp::format::ProcessBlock block;
    block.sample_rate = kSr;
    block.frame_count = frames;
    pulp::format::GraphRuntimeNodeProcessContext context;
    context.routed = true;
    context.node_inputs = {main_channels.data(), 1, frames};
    context.node_outputs = {output_channels.data(), 1, frames};
    REQUIRE(ProcessorNode::process_binding(block, context, &node));
    REQUIRE(provider.calls == 1);
    for (int frame = 0; frame < frames; ++frame)
        REQUIRE(output[frame] == main[frame] + route[frame]);
}

namespace {

// Records what the processor was handed on its last block.
class ContextProbe final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "ContextProbe";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.contextprobe";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Main In", 1, false}};
        d.output_buses = {{"Main Out", 1, false}};
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in, pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out, const ProcessContext& context) override {
        seen = context;
        input_channels = in.num_channels();
        midi_in_events = midi_in.size();
        midi_out.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
        params_seen = param_events();
        sidechain_seen = sidechain_input();
        for (std::size_t i = 0; i < out.num_samples(); ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
    }
    ProcessContext seen{};
    std::size_t input_channels = 0;
    std::size_t midi_in_events = 0;
    const pulp::state::ParameterEventQueue* params_seen = nullptr;
    const pulp::audio::BufferView<const float>* sidechain_seen = nullptr;
};

} // namespace

TEST_CASE("ProcessorNode hands a main-bus processor the block context and empty sidecars",
          "[format][graph][executor][routing][processor-node]") {
    // A node with one main input and output takes a prepared path. The
    // processor must see what process_processor_block() would hand it: the
    // transport when it is complete, the block's rate and length filling any
    // field it leaves unset, a default context when there is none, empty
    // MIDI input on every block, an empty parameter queue, and no sidechain.
    constexpr int frames = 8;
    ContextProbe processor;
    pulp::state::StateStore store;
    bind_processor(processor, store);
    ProcessorNode node(processor);
    PrepareContext prepare;
    prepare.sample_rate = kSr;
    prepare.max_buffer_size = frames;
    prepare.input_channels = 1;
    prepare.output_channels = 1;
    REQUIRE(node.prepare(prepare));

    std::array<float, frames> input{}, output{};
    std::array<const float*, 1> in_ch{input.data()};
    std::array<float*, 1> out_ch{output.data()};
    pulp::format::GraphRuntimeNodeProcessContext context;
    context.routed = true;
    context.node_inputs = {in_ch.data(), 1, frames};
    context.node_outputs = {out_ch.data(), 1, frames};
    pulp::format::ProcessBlock block;
    block.sample_rate = kSr;
    block.frame_count = frames;

    // Complete transport: handed through as is.
    ProcessContext full;
    full.sample_rate = 96000.0;
    full.num_samples = frames;
    full.tempo_bpm = 133.0;
    full.position_samples = 4242;
    block.transport = &full;
    REQUIRE(ProcessorNode::process_binding(block, context, &node));
    CHECK(processor.seen.sample_rate == 96000.0);
    CHECK(processor.seen.tempo_bpm == 133.0);
    CHECK(processor.seen.position_samples == 4242);
    CHECK(processor.input_channels == 1);
    CHECK(processor.midi_in_events == 0);
    REQUIRE(processor.params_seen != nullptr);
    CHECK(processor.params_seen->empty());
    CHECK(processor.sidechain_seen == nullptr);

    // Partial transport: the block fills the rate and length it leaves unset.
    ProcessContext partial;
    partial.tempo_bpm = 90.0;
    block.transport = &partial;
    REQUIRE(ProcessorNode::process_binding(block, context, &node));
    CHECK(processor.seen.sample_rate == kSr);
    CHECK(processor.seen.num_samples == frames);
    CHECK(processor.seen.tempo_bpm == 90.0);
    CHECK(processor.midi_in_events == 0);

    // No transport: a default context with the block's rate and length, with
    // nothing left over from the partial transport above.
    block.transport = nullptr;
    REQUIRE(ProcessorNode::process_binding(block, context, &node));
    CHECK(processor.seen.sample_rate == kSr);
    CHECK(processor.seen.num_samples == frames);
    CHECK(processor.seen.tempo_bpm == ProcessContext{}.tempo_bpm);
    CHECK(processor.midi_in_events == 0);
    // The node restored the processor's sidecars after each block.
    CHECK(processor.param_events() == nullptr);
}

namespace {

// Leaves events, sysex and a UMP attachment in both MIDI buffers it is handed,
// and records what it found in them on entry.
class MidiLeftoverProbe final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "MidiLeftoverProbe";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.midileftoverprobe";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Main In", 1, false}};
        d.output_buses = {{"Main Out", 1, false}};
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in, pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out, const ProcessContext&) override {
        for (auto* buffer : {&midi_in, &midi_out}) {
            leftover_events += buffer->size();
            leftover_sysex += buffer->sysex_size();
            leftover_ump += buffer->ump() != nullptr ? 1 : 0;
            buffer->add(pulp::midi::MidiEvent::note_on(0, 60, 100));
            buffer->add_sysex(std::vector<std::uint8_t>{0xF0, 0x7E, 0xF7});
            buffer->attach_ump(&ump);
        }
        ++blocks;
        for (std::size_t i = 0; i < out.num_samples(); ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
    }
    pulp::midi::UmpBuffer ump;
    std::size_t leftover_events = 0;
    std::size_t leftover_sysex = 0;
    std::size_t leftover_ump = 0;
    int blocks = 0;
};

} // namespace

TEST_CASE("ProcessorNode hands a processor empty MIDI buffers on every block without routed MIDI",
          "[format][graph][executor][routing][processor-node]") {
    // With no routed MIDI the node lends the processor its own buffers. Whatever
    // the processor left in them must be gone by the next block.
    constexpr int frames = 8;
    MidiLeftoverProbe processor;
    pulp::state::StateStore store;
    bind_processor(processor, store);
    ProcessorNode node(processor);
    PrepareContext prepare;
    prepare.sample_rate = kSr;
    prepare.max_buffer_size = frames;
    prepare.input_channels = 1;
    prepare.output_channels = 1;
    REQUIRE(node.prepare(prepare));

    std::array<float, frames> input{}, output{};
    std::array<const float*, 1> in_ch{input.data()};
    std::array<float*, 1> out_ch{output.data()};
    pulp::format::GraphRuntimeNodeProcessContext context;
    context.routed = true;
    context.node_inputs = {in_ch.data(), 1, frames};
    context.node_outputs = {out_ch.data(), 1, frames};
    pulp::format::ProcessBlock block;
    block.sample_rate = kSr;
    block.frame_count = frames;
    for (int i = 0; i < 4; ++i)
        REQUIRE(ProcessorNode::process_binding(block, context, &node));
    REQUIRE(processor.blocks == 4);
    CHECK(processor.leftover_events == 0);
    CHECK(processor.leftover_sysex == 0);
    CHECK(processor.leftover_ump == 0);
}

TEST_CASE("ProcessorNode reuses its bus set without carrying a route into the next block",
          "[format][graph][executor][routing][processor-node]") {
    // The node keeps one bus set across blocks. A sidechain that was active in
    // one block and missing in the next must reach the processor as missing,
    // and come back when the route does.
    constexpr int frames = 8;
    SidechainProbe processor;
    pulp::state::StateStore store;
    bind_processor(processor, store);
    ProcessorNode node(processor);
    PrepareContext prepare;
    prepare.sample_rate = kSr;
    prepare.max_buffer_size = frames;
    prepare.input_channels = 1;
    prepare.output_channels = 1;
    REQUIRE(node.prepare(prepare));

    std::array<float, frames> main{};
    std::array<float, frames> route{};
    std::array<float, frames> output{};
    std::array<const float*, 1> main_channels{main.data()};
    std::array<const float*, 1> route_channels{route.data()};
    std::array<float*, 1> output_channels{output.data()};
    for (int frame = 0; frame < frames; ++frame) {
        main[frame] = static_cast<float>(frame + 1);
        route[frame] = static_cast<float>(10 + frame);
    }
    RouteProviderFixture provider;
    node.set_audio_input_provider({&provider, &RouteProviderFixture::read});

    pulp::format::ProcessBlock block;
    block.sample_rate = kSr;
    block.frame_count = frames;
    pulp::format::GraphRuntimeNodeProcessContext context;
    context.routed = true;
    context.node_inputs = {main_channels.data(), 1, frames};
    context.node_outputs = {output_channels.data(), 1, frames};
    const pulp::audio::BufferView<const float> live(route_channels.data(), 1, frames);
    for (const bool routed : {true, false, true, false}) {
        provider.route = routed ? live : pulp::audio::BufferView<const float>{};
        output.fill(-1.0f);
        REQUIRE(ProcessorNode::process_binding(block, context, &node));
        for (int frame = 0; frame < frames; ++frame)
            REQUIRE(output[frame] == main[frame] + (routed ? route[frame] : 0.0f));
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

TEST_CASE("ProcessorNode refuses binding before preparation",
          "[format][graph][executor][routing][processor-node][lifecycle]") {
    OnePoleMono processor;
    ProcessorNode node(processor);
    const auto binding = node.binding(42);
    REQUIRE(binding.node_id == 42);
    REQUIRE(binding.required);
    REQUIRE(binding.process == nullptr);
    REQUIRE(binding.user_data == nullptr);
}

TEST_CASE("ProcessorNodeInstance balances repeated and failed preparation",
          "[format][graph][executor][routing][processor-node][lifecycle]") {
    auto processor = std::make_unique<LifecycleProcessor>();
    auto* observed = processor.get();
    auto instance = pulp::format::ProcessorNodeInstance::create(std::move(processor));
    REQUIRE(instance);
    REQUIRE(instance->binding(7).process == nullptr);

    PrepareContext context;
    context.sample_rate = kSr;
    context.max_buffer_size = 64;
    context.input_channels = 1;
    context.output_channels = 1;
    REQUIRE(instance->prepare(context));
    REQUIRE(observed->prepare_count == 1);
    REQUIRE(observed->release_count == 0);
    REQUIRE(instance->binding(7).process != nullptr);

    REQUIRE(instance->prepare(context));
    REQUIRE(observed->prepare_count == 2);
    REQUIRE(observed->release_count == 1);

    observed->fail_prepare = true;
    REQUIRE_FALSE(instance->prepare(context));
    REQUIRE(observed->prepare_count == 3);
    REQUIRE(observed->release_count == 3);
    REQUIRE(instance->binding(7).process == nullptr);
}

TEST_CASE("ProcessorNode dense failures clear routed output and fail closed",
          "[format][graph][executor][routing][processor-node][audio-rate][failure]") {
    for (const bool throws : {false, true}) {
        INFO("throws=" << throws);
        FailingDenseMono processor(throws);
        ProcessorNode node(processor);
        PrepareContext prepare;
        prepare.sample_rate = kSr;
        prepare.max_buffer_size = 32;
        prepare.input_channels = 1;
        prepare.output_channels = 1;
        REQUIRE(node.prepare(prepare));

        std::array<float, 32> input{};
        std::array<float, 32> output{};
        output.fill(0.5f);
        std::array<const float*, 1> input_channels{input.data()};
        std::array<float*, 1> output_channels{output.data()};

        pulp::format::ProcessBlock block;
        block.sample_rate = kSr;
        block.frame_count = 32;
        pulp::format::GraphRuntimeNodeProcessContext context;
        context.routed = true;
        context.node_inputs = {input_channels.data(), 1, 32};
        context.node_outputs = {output_channels.data(), 1, 32};

        REQUIRE_FALSE(ProcessorNode::process_binding(block, context, &node));
        for (const float sample : output)
            REQUIRE(sample == 0.0f);
    }
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
    REQUIRE(h.run(exec, snapshot, pool).ok()); // warm-up outside the probe
    {
        pulp::test::RtAllocationProbe probe;
        const auto result = h.run(exec, snapshot, pool);
        REQUIRE(result.ok());
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

TEST_CASE("ProcessorNode dense binding receives the exact gathered waveform",
          "[format][graph][executor][routing][processor-node][audio-rate]") {
    for (const int frames : {64, 2048, 4096}) {
        INFO("frames=" << frames);
        GraphRuntimeExecutor exec;

        auto processor = std::make_unique<DenseGainMono>();
        auto* observed = processor.get();
        observed->expected_sparse_events = 2;
        auto instance = pulp::format::ProcessorNodeInstance::create(std::move(processor));
        REQUIRE(instance);
        const auto base_generation = instance->state_store().state_generation();
        REQUIRE(instance->state_store().get_value(7) == 0.5f);
        PrepareContext prepare_ctx;
        prepare_ctx.sample_rate = kSr;
        prepare_ctx.max_buffer_size = frames;
        prepare_ctx.input_channels = 1;
        prepare_ctx.output_channels = 1;
        REQUIRE(instance->prepare(prepare_ctx));

        const std::array nodes = {
            GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 4},
            GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
            GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
        };
        auto modulation = GraphRuntimeConnectionSpec{1, 1, 2, 0};
        modulation.kind = pulp::graph::GraphRuntimeConnectionKind::Automation;
        modulation.automation.param_id = 7;
        modulation.automation.audio_rate = true;
        modulation.automation.range_lo = -0.5f;
        modulation.automation.range_hi = 1.5f;
        modulation.automation.bounds_lo = 0.1f;
        modulation.automation.bounds_hi = 0.8f;
        auto add = GraphRuntimeConnectionSpec{1, 2, 2, 0};
        add.kind = pulp::graph::GraphRuntimeConnectionKind::Automation;
        add.automation.param_id = 7;
        add.automation.audio_rate = true;
        add.automation.mix_add = true;
        add.automation.range_lo = 0.0f;
        add.automation.range_hi = 1.0f;
        add.automation.bounds_lo = 0.1f;
        add.automation.bounds_hi = 0.8f;
        auto sparse = GraphRuntimeConnectionSpec{1, 3, 2, 0};
        sparse.kind = pulp::graph::GraphRuntimeConnectionKind::Automation;
        sparse.automation.param_id = 8;
        sparse.automation.range_lo = 0.0f;
        sparse.automation.range_hi = 1.0f;
        sparse.automation.bounds_lo = 0.0f;
        sparse.automation.bounds_hi = 1.0f;
        const std::array conns = {
            GraphRuntimeConnectionSpec{1, 0, 2, 0}, modulation, add, sparse,
            GraphRuntimeConnectionSpec{2, 0, 3, 0},
        };
        const std::array bindings = {
            GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
            instance->binding(2),
            GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
        };
        GraphRuntimeSnapshot snapshot;
        REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
        auto pool = make_pool(snapshot, frames);
        pulp::format::GraphRuntimeAutomationScratch automation;
        REQUIRE(automation.reset(snapshot.plan(), snapshot.bindings(), frames));

        std::vector<float> audio(static_cast<std::size_t>(frames));
        std::vector<float> dense(static_cast<std::size_t>(frames));
        std::vector<float> added(static_cast<std::size_t>(frames), 0.25f);
        std::vector<float> sparse_values(static_cast<std::size_t>(frames), 0.25f);
        for (int frame = 0; frame < frames; ++frame) {
            audio[static_cast<std::size_t>(frame)] = 0.25f + 0.0001f * frame;
            dense[static_cast<std::size_t>(frame)] =
                static_cast<float>(frame) / static_cast<float>(frames - 1);
        }
        RoutedHarness h(kSr, frames, {audio, dense, added, sparse_values}, 1);
        REQUIRE(exec.process_routed(h.block, snapshot, pool, nullptr, &automation).ok());
        {
            pulp::test::RtAllocationProbe probe;
            const auto result = exec.process_routed(h.block, snapshot, pool, nullptr, &automation);
            REQUIRE(result.ok());
            REQUIRE_FALSE(probe.saw_allocation());
        }
        REQUIRE(observed->saw_dense);
        REQUIRE(observed->saw_sparse);
        REQUIRE(instance->state_store().get_value(7) == 0.5f);
        REQUIRE(instance->state_store().state_generation() == base_generation);
        for (int frame = 0; frame < frames; ++frame) {
            const float mapped = -0.5f + dense[static_cast<std::size_t>(frame)] * 2.0f;
            const float expected_lane = std::clamp(mapped + 0.25f, 0.1f, 0.8f);
            REQUIRE(h.outs[0][static_cast<std::size_t>(frame)] ==
                    audio[static_cast<std::size_t>(frame)] * expected_lane);
        }
    }
}

TEST_CASE("ProcessorNode dense oracle rejects the required planted mutations",
          "[format][graph][executor][routing][processor-node][audio-rate][mutation]") {
    constexpr int kFrames = 2048;
    auto processor = std::make_unique<DenseGainMono>();
    auto* observed = processor.get();
    auto instance = pulp::format::ProcessorNodeInstance::create(std::move(processor));
    REQUIRE(instance);

    PrepareContext prepare_ctx;
    prepare_ctx.sample_rate = kSr;
    prepare_ctx.max_buffer_size = kFrames;
    prepare_ctx.input_channels = 1;
    prepare_ctx.output_channels = 1;
    REQUIRE(instance->prepare(prepare_ctx));

    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 2},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    auto modulation = GraphRuntimeConnectionSpec{1, 1, 2, 0};
    modulation.kind = pulp::graph::GraphRuntimeConnectionKind::Automation;
    modulation.automation.param_id = 7;
    modulation.automation.audio_rate = true;
    modulation.automation.range_lo = -0.5f;
    modulation.automation.range_hi = 1.5f;
    modulation.automation.bounds_lo = 0.1f;
    modulation.automation.bounds_hi = 0.8f;
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        modulation,
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
    };
    const std::array dense_bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        instance->binding(2),
        GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, dense_bindings));
    auto pool = make_pool(snapshot, kFrames);
    pulp::format::GraphRuntimeAutomationScratch automation;
    REQUIRE(automation.reset(snapshot.plan(), snapshot.bindings(), kFrames));

    std::vector<float> audio(kFrames, 1.0f);
    std::vector<float> source(kFrames);
    for (int frame = 0; frame < kFrames; ++frame)
        source[static_cast<std::size_t>(frame)] =
            static_cast<float>(frame) / static_cast<float>(kFrames - 1);
    RoutedHarness harness(kSr, kFrames, {audio, source}, 1);
    GraphRuntimeExecutor executor;
    REQUIRE(executor.process_routed(harness.block, snapshot, pool, nullptr, &automation).ok());
    REQUIRE(observed->saw_dense);

    const auto matches_oracle = [&](std::span<const float> candidate) {
        if (candidate.size() != source.size())
            return false;
        for (std::size_t frame = 0; frame < source.size(); ++frame) {
            const float mapped = -0.5f + source[frame] * 2.0f;
            if (candidate[frame] != std::clamp(mapped, 0.1f, 0.8f))
                return false;
        }
        return true;
    };
    REQUIRE(matches_oracle(harness.outs[0]));

    std::vector<float> dropped(kFrames, 0.0f);
    REQUIRE_FALSE(matches_oracle(dropped));

    auto reversed = harness.outs[0];
    std::reverse(reversed.begin(), reversed.end());
    REQUIRE_FALSE(matches_oracle(reversed));

    std::vector<float> unclamped(kFrames);
    for (std::size_t frame = 0; frame < source.size(); ++frame)
        unclamped[frame] = -0.5f + source[frame] * 2.0f;
    REQUIRE_FALSE(matches_oracle(unclamped));

    auto legacy_bindings = dense_bindings;
    legacy_bindings[1].audio_rate_modulation_delivery =
        pulp::format::AudioRateModulationDelivery::LegacyParameterEvents;
    GraphRuntimeSnapshot legacy_snapshot;
    REQUIRE(make_snapshot(legacy_snapshot, nodes, conns, legacy_bindings));
    pulp::format::GraphRuntimeAutomationScratch legacy_automation;
    REQUIRE_FALSE(
        legacy_automation.reset(legacy_snapshot.plan(), legacy_snapshot.bindings(), kFrames));
}

TEST_CASE("ProcessorNode dense delivery is callback-partition invariant",
          "[format][graph][executor][routing][processor-node][audio-rate][partition]") {
    constexpr int kFrames = 1024;
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 2},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    auto modulation = GraphRuntimeConnectionSpec{1, 1, 2, 0};
    modulation.kind = pulp::graph::GraphRuntimeConnectionKind::Automation;
    modulation.automation.param_id = 7;
    modulation.automation.audio_rate = true;
    modulation.automation.bounds_lo = 0.0f;
    modulation.automation.bounds_hi = 1.0f;
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        modulation,
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
    };

    const auto render = [&](std::span<const int> partitions) {
        auto instance =
            pulp::format::ProcessorNodeInstance::create(std::make_unique<DenseGainMono>());
        REQUIRE(instance);
        PrepareContext prepare;
        prepare.sample_rate = kSr;
        prepare.max_buffer_size = kFrames;
        prepare.input_channels = 1;
        prepare.output_channels = 1;
        REQUIRE(instance->prepare(prepare));
        const std::array bindings = {
            GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
            instance->binding(2),
            GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
        };
        GraphRuntimeSnapshot snapshot;
        REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
        auto pool = make_pool(snapshot, kFrames);
        pulp::format::GraphRuntimeAutomationScratch automation;
        REQUIRE(automation.reset(snapshot.plan(), snapshot.bindings(), kFrames));
        GraphRuntimeExecutor executor;
        std::vector<float> rendered;
        rendered.reserve(kFrames);
        int cursor = 0;
        for (const int frames : partitions) {
            std::vector<float> audio(static_cast<std::size_t>(frames), 1.0f);
            std::vector<float> lane(static_cast<std::size_t>(frames));
            for (int frame = 0; frame < frames; ++frame)
                lane[static_cast<std::size_t>(frame)] =
                    static_cast<float>(cursor + frame) / static_cast<float>(kFrames - 1);
            RoutedHarness harness(kSr, frames, {audio, lane}, 1);
            REQUIRE(
                executor.process_routed(harness.block, snapshot, pool, nullptr, &automation).ok());
            rendered.insert(rendered.end(), harness.outs[0].begin(), harness.outs[0].end());
            cursor += frames;
        }
        REQUIRE(cursor == kFrames);
        return rendered;
    };

    const std::array regular{256, 256, 256, 256};
    const std::array irregular{1, 17, 113, 5, 257, 3, 389, 239};
    const auto regular_output = render(regular);
    const auto irregular_output = render(irregular);
    REQUIRE(regular_output == irregular_output);
}

TEST_CASE("ProcessorNode dense binding receives per-source slew",
          "[format][graph][executor][routing][processor-node][audio-rate][slew]") {
    constexpr int kFrames = 64;
    GraphRuntimeExecutor exec;
    auto processor = std::make_unique<DenseGainMono>();
    auto instance = pulp::format::ProcessorNodeInstance::create(std::move(processor));
    REQUIRE(instance);

    PrepareContext prepare_ctx;
    prepare_ctx.sample_rate = kSr;
    prepare_ctx.max_buffer_size = kFrames;
    prepare_ctx.input_channels = 1;
    prepare_ctx.output_channels = 1;
    REQUIRE(instance->prepare(prepare_ctx));

    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 2},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    auto modulation = GraphRuntimeConnectionSpec{1, 1, 2, 0};
    modulation.kind = pulp::graph::GraphRuntimeConnectionKind::Automation;
    modulation.automation.param_id = 7;
    modulation.automation.audio_rate = true;
    modulation.automation.smoothing_ms = 1.0f;
    modulation.automation.range_lo = 0.0f;
    modulation.automation.range_hi = 1.0f;
    modulation.automation.bounds_lo = 0.0f;
    modulation.automation.bounds_hi = 1.0f;
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        modulation,
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
    };
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        instance->binding(2),
        GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    auto pool = make_pool(snapshot, kFrames);
    pulp::format::GraphRuntimeAutomationScratch automation;
    REQUIRE(automation.reset(snapshot.plan(), snapshot.bindings(), kFrames));

    std::vector<float> audio(kFrames, 1.0f);
    std::vector<float> step(kFrames, 1.0f);
    std::fill_n(step.begin(), 4, 0.0f);
    RoutedHarness h(kSr, kFrames, {audio, step}, 1);
    REQUIRE(exec.process_routed(h.block, snapshot, pool, nullptr, &automation).ok());

    float expected = 0.0f;
    constexpr float kMaxStep = 1.0f / 48.0f;
    for (int frame = 0; frame < kFrames; ++frame) {
        const float target = frame < 4 ? 0.0f : 1.0f;
        expected = std::clamp(target, expected - kMaxStep, expected + kMaxStep);
        REQUIRE(h.outs[0][static_cast<std::size_t>(frame)] == expected);
    }
}

TEST_CASE("ProcessorNode dense binding applies automation delay compensation",
          "[format][graph][executor][routing][processor-node][audio-rate][pdc]") {
    constexpr int kFrames = 16;
    GraphRuntimeExecutor exec;
    auto processor = std::make_unique<DenseGainMono>();
    auto* observed = processor.get();
    auto instance = pulp::format::ProcessorNodeInstance::create(std::move(processor));
    REQUIRE(instance);

    PrepareContext prepare_ctx;
    prepare_ctx.sample_rate = kSr;
    prepare_ctx.max_buffer_size = kFrames;
    prepare_ctx.input_channels = 1;
    prepare_ctx.output_channels = 1;
    REQUIRE(instance->prepare(prepare_ctx));

    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 3},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1, 0, 0, false, 2},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    auto replace = GraphRuntimeConnectionSpec{2, 0, 3, 0};
    replace.kind = pulp::graph::GraphRuntimeConnectionKind::Automation;
    replace.automation.param_id = 7;
    replace.automation.audio_rate = true;
    replace.automation.bounds_lo = 0.0f;
    replace.automation.bounds_hi = 1.0f;
    auto add = GraphRuntimeConnectionSpec{1, 1, 3, 0};
    add.kind = pulp::graph::GraphRuntimeConnectionKind::Automation;
    add.automation.param_id = 7;
    add.automation.audio_rate = true;
    add.automation.mix_add = true;
    add.automation.bounds_lo = 0.0f;
    add.automation.bounds_hi = 1.0f;
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 2, 2, 0},
        GraphRuntimeConnectionSpec{1, 0, 3, 0},
        replace,
        add,
        GraphRuntimeConnectionSpec{3, 0, 4, 0},
    };
    GainState unity{1.0f};
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_gain, &unity, true},
        instance->binding(3),
        GraphRuntimeNodeBinding{4, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    REQUIRE(snapshot.buffer_assignment().connection_delay_samples[3] == 2);

    pulp::format::GraphRuntimeBufferPool pool;
    REQUIRE(pool.reset(snapshot.buffer_slot_count(), kFrames,
                       snapshot.buffer_assignment().connection_delay_samples));
    pulp::format::GraphRuntimeAutomationScratch automation;
    REQUIRE(automation.reset(snapshot.plan(), snapshot.bindings(), kFrames));

    std::vector<float> audio(kFrames, 1.0f);
    std::vector<float> fast_add(kFrames, 0.2f);
    std::vector<float> slow_replace(kFrames, 0.4f);
    RoutedHarness h(kSr, kFrames, {audio, fast_add, slow_replace}, 1);
    REQUIRE(exec.process_routed(h.block, snapshot, pool, nullptr, &automation).ok());
    REQUIRE(observed->last_lane_count == kFrames);
    REQUIRE(observed->last_lane[0] == 0.4f);
    REQUIRE(observed->last_lane[1] == 0.4f);
    for (int frame = 2; frame < kFrames; ++frame)
        REQUIRE(observed->last_lane[static_cast<std::size_t>(frame)] == 0.6f);
}
