// Transport plumbing for SignalGraph::process.
//
// SignalGraph gained a transport-aware process() overload that populates the
// routed ProcessBlock with the host transport (playhead, process_mode, and the
// render-speed hint via *block.transport). This suite proves the additive
// contract:
//
//   T1  The transport overload is bit-identical to the no-transport overload for
//       a graph whose nodes ignore block.transport (gain / plugin / custom — the
//       only node kinds SignalGraph builds today). Populating the block is inert
//       for non-consumers, and routed-vs-walk parity is unaffected.
//   T2  The exact block population SignalGraph applies (block.transport = &ctx,
//       block.mode = ctx.process_mode) delivers the supplied transport to the
//       documented consumer — a ProcessorNode forwarding into
//       process_processor_block / context_for_block — while a transport-absent
//       block (block.transport = nullptr) yields the transport-absent defaults
//       (tempo 120, stopped). SignalGraph exposes no ProcessorNode-backed node
//       type, so the consumer is exercised at the GraphRuntimeExecutor level the
//       same way SignalGraph drives it.
//   T3  process_mode and render_speed_hint reach the Processor via *block.transport
//       (NOT via block.render_speed, which stays the numeric 1.0 — the hint is a
//       categorical enum and is never mapped into the multiplier).
//   T4  When anticipative rendering is active the transport overload SUPPRESSES the
//       supplied transport: output stays bit-identical to the no-transport render
//       and transport_suppressed_for_anticipation() counts the suppression.

#include "harness/graph_routing_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/state/store.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace pulp::host;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 64;

// ── Shared fixtures ──────────────────────────────────────────────────────────

// A trivial deterministic PluginSlot: scales each channel by 0.5. It ignores the
// (absent) transport entirely — PluginSlot::process has no ProcessContext — so it
// is a representative "transport-inert" routed node for T1.
class ScaleSlot final : public PluginSlot {
public:
    ScaleSlot() {
        info_.name = "ScaleHalf";
        info_.format = PluginFormat::CLAP;
        info_.num_inputs = 2;
        info_.num_outputs = 2;
        info_.category = "Effect";
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&, int n) override {
        const std::size_t chs = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < chs; ++c) {
            const float* i = in.channel_ptr(c);
            float* o = out.channel_ptr(c);
            for (int k = 0; k < n; ++k)
                o[static_cast<std::size_t>(k)] = 0.5f * i[static_cast<std::size_t>(k)];
        }
        for (std::size_t c = chs; c < out.num_channels(); ++c)
            std::fill_n(out.channel_ptr(c), n, 0.0f);
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

private:
    PluginInfo info_;
};

class TransportAwareSlot final : public PluginSlot {
public:
    TransportAwareSlot() {
        info_.name = "TransportAware";
        info_.format = PluginFormat::CLAP;
        info_.num_inputs = 1;
        info_.num_outputs = 1;
        info_.category = "Effect";
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    bool wants_transport() const override { return true; }

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&, int n) override {
        ++transportless_calls;
        copy_input(out, in, n);
    }

    void process(pulp::format::ProcessBuffers& audio,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&,
                 int n,
                 const pulp::format::ProcessContext& transport) override {
        ++transport_calls;
        last_transport = transport;
        auto* out = audio.main_output();
        const auto* in = audio.main_input();
        pulp::audio::BufferView<float> empty_out;
        pulp::audio::BufferView<const float> empty_in;
        copy_input(out ? *out : empty_out, in ? *in : empty_in, n);
    }

    std::vector<pulp::host::HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

    int transport_calls = 0;
    int transportless_calls = 0;
    pulp::format::ProcessContext last_transport;

private:
    static void copy_input(pulp::audio::BufferView<float>& out,
                           const pulp::audio::BufferView<const float>& in,
                           int n) {
        const std::size_t chs = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < chs; ++c) {
            std::copy_n(in.channel_ptr(c), n, out.channel_ptr(c));
        }
        for (std::size_t c = chs; c < out.num_channels(); ++c) {
            std::fill_n(out.channel_ptr(c), n, 0.0f);
        }
    }

    PluginInfo info_;
};

// Stateful counting generator (mirrors the anticipation suite's CountingGen):
// block n, channel c -> (c+1)*10 + n. Used as an anticipation-eligible interior
// source for T4. Counts process() calls so the test can confirm the interior is
// advanced solely by the producer pump.
class CountingGen final : public PluginSlot {
public:
    CountingGen() {
        info_.name = "TransportCountingGen";
        info_.format = PluginFormat::CLAP;
        info_.num_inputs = 0;
        info_.num_outputs = 2;
        info_.category = "Generator";
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&, int n) override {
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            float* o = out.channel_ptr(c);
            const float v = static_cast<float>((c + 1) * 10) + static_cast<float>(block_);
            for (int k = 0; k < n; ++k) o[static_cast<std::size_t>(k)] = v;
        }
        ++block_;
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

private:
    PluginInfo info_;
    int block_ = 0;
};

// input(2) -> gain -> plugin(scale) -> custom(passthrough) -> output(2). Exercises
// all three transport-inert routed binding kinds (gain / plugin / custom).
void wire_inert_chain(SignalGraph& g) {
    CustomNodeType passthrough;
    passthrough.type_id = "transport.passthrough";
    passthrough.num_input_ports = 2;
    passthrough.num_output_ports = 2;
    passthrough.process = [](pulp::audio::BufferView<float>& out,
                             const pulp::audio::BufferView<const float>& in,
                             int n) {
        const std::size_t chs = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < chs; ++c)
            std::copy_n(in.channel_ptr(c), n, out.channel_ptr(c));
        for (std::size_t c = chs; c < out.num_channels(); ++c)
            std::fill_n(out.channel_ptr(c), n, 0.0f);
    };
    REQUIRE(g.register_custom_node_type(passthrough));

    const auto in = g.add_input_node(2, "In");
    const auto gain = g.add_gain_node("G");
    const auto plug = g.add_plugin_node(std::make_unique<ScaleSlot>(), 2, 2, "Scale");
    const auto cust = g.add_custom_node("transport.passthrough", "Pass");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, gain, c));
        REQUIRE(g.connect(gain, c, plug, c));
        REQUIRE(g.connect(plug, c, cust, c));
        REQUIRE(g.connect(cust, c, out, c));
    }
    REQUIRE(g.set_node_gain(gain, 0.75f));
}

// A non-default, fully-populated host transport — every field a non-consumer must
// ignore for T1, and the source of truth the consumer must echo for T2.
pulp::format::ProcessContext non_default_transport() {
    pulp::format::ProcessContext t;
    t.is_playing = true;
    t.tempo_bpm = 140.0;
    t.position_beats = 3.5;
    t.position_samples = 4096;
    t.time_sig_numerator = 3;
    t.time_sig_denominator = 8;
    return t;
}

// Render one stereo block of a deterministic ramp through `g`, optionally with
// transport, and return the two output channels.
std::array<std::vector<float>, 2> render_block(SignalGraph& g,
                                               const pulp::format::ProcessContext* transport) {
    std::vector<float> li(kFrames), ri(kFrames);
    for (int k = 0; k < kFrames; ++k) {
        li[static_cast<std::size_t>(k)] = static_cast<float>(k) * 0.01f;
        ri[static_cast<std::size_t>(k)] = static_cast<float>(k) * -0.02f + 1.0f;
    }
    std::array<const float*, 2> ic{li.data(), ri.data()};
    std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
    std::array<float*, 2> oc{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);
    if (transport)
        g.process(ov, iv, kFrames, *transport);
    else
        g.process(ov, iv, kFrames);
    return {std::move(lo), std::move(ro)};
}

} // namespace

// ── T1: inertness / parity ───────────────────────────────────────────────────

TEST_CASE("SignalGraph transport overload is bit-identical for transport-inert nodes",
          "[host][signal-graph][transport]") {
    // Two identical graphs rendered block-for-block: one via the no-transport
    // overload, one via the transport overload with a NON-default transport. The
    // gain / plugin / custom bindings ignore block.transport, so the populated
    // transport must be inert — bit-identical output proves it, and proves the
    // routed-vs-walk parity the populate did not perturb.
    SignalGraph plain;
    SignalGraph withT;
    plain.set_canonical_executor_routing_enabled(true);
    withT.set_canonical_executor_routing_enabled(true);
    wire_inert_chain(plain);
    wire_inert_chain(withT);
    REQUIRE(plain.prepare(kSr, kFrames));
    REQUIRE(withT.prepare(kSr, kFrames));

    const auto transport = non_default_transport();
    for (int block = 0; block < 4; ++block) {
        const auto a = render_block(plain, nullptr);
        const auto b = render_block(withT, &transport);
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < kFrames; ++k)
                REQUIRE(a[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)] ==
                        b[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)]);
    }
    // No anticipation in play, so nothing was suppressed.
    REQUIRE(withT.transport_suppressed_for_anticipation() == 0);
}

TEST_CASE("SignalGraph routed plugin binding delivers transport to opted-in slots",
          "[host][signal-graph][transport]") {
    SignalGraph graph;
    graph.set_canonical_executor_routing_enabled(true);
    const auto in = graph.add_input_node(1, "In");
    auto slot = std::make_unique<TransportAwareSlot>();
    auto* slot_ptr = slot.get();
    const auto plug = graph.add_plugin_node(std::move(slot), 1, 1, "TransportAware");
    const auto out = graph.add_output_node(1, "Out");
    REQUIRE(graph.connect(in, 0, plug, 0));
    REQUIRE(graph.connect(plug, 0, out, 0));
    REQUIRE(graph.prepare(kSr, kFrames));

    auto transport = non_default_transport();
    transport.process_mode = pulp::format::ProcessMode::Offline;
    const auto rendered = render_block(graph, &transport);

    REQUIRE(slot_ptr->transport_calls == 1);
    REQUIRE(slot_ptr->transportless_calls == 0);
    REQUIRE(slot_ptr->last_transport.is_playing);
    REQUIRE(slot_ptr->last_transport.tempo_bpm == 140.0);
    REQUIRE(slot_ptr->last_transport.position_samples == 4096);
    REQUIRE(slot_ptr->last_transport.process_mode == pulp::format::ProcessMode::Offline);
    REQUIRE(rendered[0][0] == 0.0f);
    REQUIRE(rendered[0][1] == 0.01f);
}

// ── T2 / T3: plumbing proof at the consumer ──────────────────────────────────
//
// SignalGraph builds no ProcessorNode-backed node, so the documented transport
// consumer (format::ProcessorNode -> process_processor_block -> context_for_block)
// is exercised through GraphRuntimeExecutor::process_routed with a ProcessBlock
// populated EXACTLY as SignalGraph::process_impl populates it.

namespace {

using pulp::format::GraphRuntimeExecutor;
using pulp::format::GraphRuntimeNodeBinding;
using pulp::format::GraphRuntimeSnapshot;
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

// Records the ProcessContext it last received. Mono 1->1 passthrough.
class RecordingProcessor final : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "RecordingProcessor";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.recordingprocessor";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Main In", 1, false}};
        d.output_buses = {{"Main Out", 1, false}};
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const ProcessContext& ctx) override {
        last = ctx;
        ++calls;
        if (output.num_channels() == 0) return;
        const std::size_t frames = output.num_samples();
        float* o = output.channel_ptr(0);
        const float* i = input.num_channels() > 0 ? input.channel_ptr(0) : nullptr;
        for (std::size_t k = 0; k < frames; ++k) o[k] = i ? i[k] : 0.0f;
    }

    ProcessContext last;
    int calls = 0;
};

// in(AudioInput,1) -> ProcessorNode(1->1) -> out(AudioOutput).
const std::array kProcNodes = {
    GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
    GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
    GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
};
const std::array kProcConns = {
    GraphRuntimeConnectionSpec{1, 0, 2, 0},
    GraphRuntimeConnectionSpec{2, 0, 3, 0},
};

// Drive one routed block through a ProcessorNode, populating the ProcessBlock the
// way SignalGraph::process_impl does for the given transport (nullptr = the
// no-transport path). Returns the context the Processor recorded plus the
// block.render_speed the executor saw.
struct PlumbResult {
    ProcessContext recorded;
    double render_speed = 0.0;
};

PlumbResult drive_processor_block(RecordingProcessor& proc,
                                  const ProcessContext* transport) {
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    ProcessorNode node(proc);
    PrepareContext pctx;
    pctx.sample_rate = kSr;
    pctx.max_buffer_size = kFrames;
    pctx.input_channels = 1;
    pctx.output_channels = 1;
    REQUIRE(node.prepare(pctx));

    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, ProcessorNode::process_binding, &node, true},
        GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, kProcNodes, kProcConns, bindings));
    auto pool = make_pool(snapshot, kFrames);

    std::vector<float> in(kFrames, 0.25f);
    RoutedHarness h(kSr, kFrames, {in}, /*out_channels=*/1);
    // Mirror SignalGraph::process_impl's population precisely.
    if (transport != nullptr) {
        h.block.transport = transport;
        h.block.mode = transport->process_mode;
    }
    GraphRuntimeExecutor exec;
    REQUIRE(h.run(exec, snapshot, pool).ok());
    return {proc.last, h.block.render_speed};
}

} // namespace

TEST_CASE("SignalGraph block population delivers transport to a ProcessorNode",
          "[host][signal-graph][transport]") {
    // With transport supplied, the Processor sees the live playhead.
    {
        RecordingProcessor proc;
        auto t = non_default_transport();
        const auto r = drive_processor_block(proc, &t);
        REQUIRE(proc.calls == 1);
        REQUIRE(r.recorded.is_playing == true);
        REQUIRE(r.recorded.tempo_bpm == 140.0);
        REQUIRE(r.recorded.position_samples == 4096);
        REQUIRE(r.recorded.process_mode == pulp::format::ProcessMode::Realtime);
    }
    // The no-transport path preserves the transport-absent defaults.
    {
        RecordingProcessor proc;
        const auto r = drive_processor_block(proc, nullptr);
        REQUIRE(proc.calls == 1);
        REQUIRE(r.recorded.is_playing == false);
        REQUIRE(r.recorded.tempo_bpm == 120.0);
        REQUIRE(r.recorded.process_mode == pulp::format::ProcessMode::Realtime);
    }
}

TEST_CASE("SignalGraph transport delivers process_mode and render-speed hint via the context",
          "[host][signal-graph][transport]") {
    RecordingProcessor proc;
    auto t = non_default_transport();
    t.process_mode = pulp::format::ProcessMode::Offline;
    t.render_speed_hint = pulp::format::RenderSpeedHint::FasterThanRealtime;

    const auto r = drive_processor_block(proc, &t);

    // process_mode + render_speed_hint reach the Processor via *block.transport.
    REQUIRE(r.recorded.process_mode == pulp::format::ProcessMode::Offline);
    REQUIRE(r.recorded.render_speed_hint == pulp::format::RenderSpeedHint::FasterThanRealtime);
    // The categorical hint is NOT mapped into the numeric block.render_speed,
    // which stays at its 1.0 identity multiplier.
    REQUIRE(r.render_speed == 1.0);
}

// ── T4: anticipation safety ──────────────────────────────────────────────────

namespace {

// gen(0/2) -> gain -> out(2). Interior = {gen, gain}; out is the live sink — an
// anticipation-eligible latent interior (no live input / feedback / sidechain).
void wire_anticipation_graph(SignalGraph& g) {
    const auto gen = g.add_plugin_node(std::make_unique<CountingGen>(), 0, 2, "Gen");
    const auto gain = g.add_gain_node("G");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(gen, c, gain, c));
        REQUIRE(g.connect(gain, c, out, c));
    }
    REQUIRE(g.set_node_gain(gain, 0.5f));
}

std::array<std::vector<float>, 2> render_generator_block(
    SignalGraph& g, const pulp::format::ProcessContext* transport) {
    std::vector<float> zi(kFrames, 0.0f);
    std::array<const float*, 2> ic{zi.data(), zi.data()};
    std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
    std::array<float*, 2> oc{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);
    if (transport)
        g.process(ov, iv, kFrames, *transport);
    else
        g.process(ov, iv, kFrames);
    return {std::move(lo), std::move(ro)};
}

} // namespace

TEST_CASE("SignalGraph anticipation suppresses a supplied transport and counts it",
          "[host][signal-graph][transport][anticipation]") {
    // Oracle: anticipation active, no-transport overload.
    SignalGraph oracle;
    oracle.set_canonical_executor_routing_enabled(true);
    oracle.set_anticipation_enabled(true);
    wire_anticipation_graph(oracle);
    REQUIRE(oracle.prepare(kSr, kFrames));

    // Subject: identical graph, anticipation active, transport overload with a
    // non-default transport that MUST be suppressed.
    SignalGraph subject;
    subject.set_canonical_executor_routing_enabled(true);
    subject.set_anticipation_enabled(true);
    wire_anticipation_graph(subject);
    REQUIRE(subject.prepare(kSr, kFrames));

    const auto transport = non_default_transport();
    for (int block = 0; block < 6; ++block) {
        oracle.pump_anticipation(8);
        subject.pump_anticipation(8);
        const auto a = render_generator_block(oracle, nullptr);
        const auto b = render_generator_block(subject, &transport);
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < kFrames; ++k)
                REQUIRE(a[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)] ==
                        b[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)]);
    }

    // The subject suppressed the transport on every one of the six interior
    // blocks (anticipation is valid from prepare, so suppression fires once per
    // process() call); the oracle never had a transport to suppress.
    REQUIRE(subject.transport_suppressed_for_anticipation() == 6);
    REQUIRE(oracle.transport_suppressed_for_anticipation() == 0);
}
