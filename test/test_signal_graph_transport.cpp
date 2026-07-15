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
//   T4  Per-node transport sensitivity (PluginSlot::wants_transport() and a
//       transport-aware custom callback): a node opts in, the routed binding
//       forwards the live transport to it, and a transport-absent block yields
//       the transport-absent defaults. A node that does NOT opt in is byte-for-
//       byte unchanged.
//   T5  Under active anticipation a transport-sensitive node is forced exterior
//       (AnticipationExclusion::TransportSensitive) and RECEIVES the live
//       transport, while a transport-insensitive interior stays ahead-rendered
//       and is unaffected. Output beside an anticipated latent branch stays
//       bit-identical to the no-transport render; transport_suppressed_for_
//       anticipation() now counts the transport-sensitive nodes forced exterior
//       (the former blanket per-block suppression is retired).

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

// A transport-sensitive 2-in/2-out effect: opts in via wants_transport() and
// records the ProcessContext the routed binding forwards to it. Its audio output
// is IDENTICAL on both the transport and no-transport paths (scale by 0.5), so a
// graph using it is bit-identical regardless of whether transport is supplied —
// only `last`/`got_transport` differ. Used to prove the opt-in forwarding.
class TransportRecordingSlot final : public PluginSlot {
public:
    TransportRecordingSlot() {
        info_.name = "TransportRecorder";
        info_.format = PluginFormat::CLAP;
        info_.num_inputs = 2;
        info_.num_outputs = 2;
        info_.category = "Effect";
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    bool wants_transport() const override { return true; }
    // No-transport path (the routed multi-bus default projects here): scale only.
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&, int n) override {
        ++calls;
        scale(out, in, n);
    }
    // Transport path: record the supplied context, then the SAME scale.
    void process(pulp::format::ProcessBuffers& audio,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&, int n,
                 const pulp::format::ProcessContext& transport) override {
        ++calls;
        ++transport_calls;
        last = transport;
        got_transport = true;
        auto* output = audio.main_output();
        const auto* input = audio.main_input();
        pulp::audio::BufferView<float> empty_out;
        pulp::audio::BufferView<const float> empty_in;
        scale(output ? *output : empty_out, input ? *input : empty_in, n);
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

    pulp::format::ProcessContext last;
    bool got_transport = false;
    int calls = 0;
    int transport_calls = 0;

private:
    static void scale(pulp::audio::BufferView<float>& out,
                      const pulp::audio::BufferView<const float>& in, int n) {
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
    PluginInfo info_;
};

// A transport-sensitive 0-in/2-out GENERATOR for the anticipation coexistence
// test: deterministic per-block output ((c+1)*100 + block) advanced once per
// process() call on BOTH overloads (so its audio is identical with or without
// transport), recording the ProcessContext on the transport path.
class TransportGen final : public PluginSlot {
public:
    TransportGen() {
        info_.name = "TransportGen";
        info_.format = PluginFormat::CLAP;
        info_.num_inputs = 0;
        info_.num_outputs = 2;
        info_.category = "Generator";
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    bool wants_transport() const override { return true; }
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&, int n) override {
        emit(out, n);
    }
    void process(pulp::format::ProcessBuffers& audio,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&, int n,
                 const pulp::format::ProcessContext& transport) override {
        last = transport;
        got_transport = true;
        auto* output = audio.main_output();
        pulp::audio::BufferView<float> empty_out;
        emit(output ? *output : empty_out, n);
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

    pulp::format::ProcessContext last;
    bool got_transport = false;

private:
    void emit(pulp::audio::BufferView<float>& out, int n) {
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            float* o = out.channel_ptr(c);
            const float v = static_cast<float>((c + 1) * 100) + static_cast<float>(block_);
            for (int k = 0; k < n; ++k) o[static_cast<std::size_t>(k)] = v;
        }
        ++block_;
    }
    PluginInfo info_;
    int block_ = 0;
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

// ── T4: per-node opt-in (plugin + custom) ────────────────────────────────────

namespace {

// in(2) -> ts-plugin -> out(2). The plugin opts into transport. Returns a raw
// pointer to the slot (the graph keeps it alive) so the test can read what it
// received.
TransportRecordingSlot* wire_transport_plugin(SignalGraph& g) {
    auto slot = std::make_unique<TransportRecordingSlot>();
    auto* raw = slot.get();
    const auto in = g.add_input_node(2, "In");
    const auto plug = g.add_plugin_node(std::move(slot), 2, 2, "TS");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, plug, c));
        REQUIRE(g.connect(plug, c, out, c));
    }
    return raw;
}

} // namespace

TEST_CASE("SignalGraph forwards transport to an opted-in plugin node",
          "[host][signal-graph][transport]") {
    // A plugin overriding wants_transport()->true and the transport process()
    // overload receives the live host transport on the routed path; a
    // transport-absent block falls back to the no-transport overload and the
    // documented transport-absent defaults.
    SECTION("transport supplied -> the plugin sees the live playhead") {
        SignalGraph g;
        g.set_canonical_executor_routing_enabled(true);
        auto* slot = wire_transport_plugin(g);
        REQUIRE(g.prepare(kSr, kFrames));
        const auto t = non_default_transport();
        (void)render_block(g, &t);
        REQUIRE(slot->got_transport == true);
        REQUIRE(slot->transport_calls == 1);
        REQUIRE(slot->last.is_playing == true);
        REQUIRE(slot->last.tempo_bpm == 140.0);
        REQUIRE(slot->last.position_samples == 4096);
    }
    SECTION("no transport -> the plugin runs the transport-less overload") {
        SignalGraph g;
        g.set_canonical_executor_routing_enabled(true);
        auto* slot = wire_transport_plugin(g);
        REQUIRE(g.prepare(kSr, kFrames));
        (void)render_block(g, nullptr);
        REQUIRE(slot->got_transport == false);   // transport overload never invoked
        REQUIRE(slot->transport_calls == 0);
        REQUIRE(slot->calls == 1);               // the no-transport overload ran
    }
}

TEST_CASE("SignalGraph forwards transport to opted-in plugins on the parallel path",
          "[host][signal-graph][transport][parallel]") {
    // The opt-in capability bit is resolved into the binding context at
    // snapshot-build time for BOTH the serial and the levelized-parallel routed
    // snapshots, so opted-in plugins must receive the live transport when the
    // parallel path dispatches the block, exactly as on the serial path. Two
    // sibling plugins fed from the same input form one parallel level so the
    // worker pool actually dispatches it (a single-node chain would run serially
    // and never exercise the parallel binding wiring).
    SignalGraph g;
    g.set_canonical_executor_routing_enabled(true);
    g.set_parallel_routing_enabled(true);
    g.set_parallel_min_work_units(0);  // force the parallel path past the break-even gate
    auto slotA = std::make_unique<TransportRecordingSlot>();
    auto slotB = std::make_unique<TransportRecordingSlot>();
    auto* a = slotA.get();
    auto* b = slotB.get();
    const auto in = g.add_input_node(2, "In");
    const auto pA = g.add_plugin_node(std::move(slotA), 2, 2, "TSA");
    const auto pB = g.add_plugin_node(std::move(slotB), 2, 2, "TSB");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, pA, c));
        REQUIRE(g.connect(in, c, pB, c));
        REQUIRE(g.connect(pA, c, out, c));  // fan-in sum at the output
        REQUIRE(g.connect(pB, c, out, c));
    }
    REQUIRE(g.prepare(kSr, kFrames));
    const auto t = non_default_transport();
    (void)render_block(g, &t);
    // The parallel path must have actually dispatched a level (pA and pB are
    // siblings at one level), else this would silently degrade to the serial
    // path and stop testing the parallel binding wiring.
    REQUIRE(g.routing_executor_stats().parallel_levels_dispatched > 0);
    for (auto* slot : {a, b}) {
        REQUIRE(slot->got_transport == true);
        REQUIRE(slot->transport_calls == 1);
        REQUIRE(slot->last.is_playing == true);
        REQUIRE(slot->last.tempo_bpm == 140.0);
        REQUIRE(slot->last.position_samples == 4096);
    }
}

TEST_CASE("SignalGraph leaves a plugin that does not opt in byte-for-byte unchanged",
          "[host][signal-graph][transport]") {
    // A plugin that does NOT override wants_transport() (ScaleSlot) is bit-
    // identical between the transport and no-transport overloads — the no-bit
    // oracle: opting in is the ONLY thing that changes a slot's behavior.
    SignalGraph plain;
    SignalGraph withT;
    plain.set_canonical_executor_routing_enabled(true);
    withT.set_canonical_executor_routing_enabled(true);
    for (SignalGraph* g : {&plain, &withT}) {
        const auto in = g->add_input_node(2, "In");
        const auto plug = g->add_plugin_node(std::make_unique<ScaleSlot>(), 2, 2, "Scale");
        const auto out = g->add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g->connect(in, c, plug, c));
            REQUIRE(g->connect(plug, c, out, c));
        }
    }
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
}

namespace {

// A transport-aware custom type that records the ProcessContext it receives into
// a caller-owned sink, and passes audio through. The plain `process` (no
// transport) marks the sink seen=false; `process_transport` records the context.
struct CustomTransportSink {
    pulp::format::ProcessContext last;
    bool got_transport = false;
    int plain_calls = 0;
};

void register_transport_custom(SignalGraph& g, CustomTransportSink* sink,
                               bool with_transport_cb) {
    CustomNodeType ty;
    ty.type_id = "transport.recorder";
    ty.num_input_ports = 2;
    ty.num_output_ports = 2;
    auto passthrough = [](pulp::audio::BufferView<float>& out,
                          const pulp::audio::BufferView<const float>& in, int n) {
        const std::size_t chs = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < chs; ++c)
            std::copy_n(in.channel_ptr(c), n, out.channel_ptr(c));
        for (std::size_t c = chs; c < out.num_channels(); ++c)
            std::fill_n(out.channel_ptr(c), n, 0.0f);
    };
    ty.process = [sink, passthrough](pulp::audio::BufferView<float>& out,
                                     const pulp::audio::BufferView<const float>& in,
                                     int n) {
        ++sink->plain_calls;
        passthrough(out, in, n);
    };
    if (with_transport_cb) {
        ty.process_transport = [sink, passthrough](
                                   pulp::audio::BufferView<float>& out,
                                   const pulp::audio::BufferView<const float>& in,
                                   int n,
                                   const pulp::format::ProcessContext& transport) {
            sink->last = transport;
            sink->got_transport = true;
            passthrough(out, in, n);
        };
    }
    REQUIRE(g.register_custom_node_type(ty));
}

} // namespace

TEST_CASE("SignalGraph forwards transport to an opted-in custom node",
          "[host][signal-graph][transport]") {
    // A custom type that registers process_transport receives the live transport;
    // a custom type with only the plain `process` is transport-unaware and never
    // sees it (the no-bit oracle for custom nodes).
    SECTION("process_transport set -> the custom node sees the transport") {
        CustomTransportSink sink;
        SignalGraph g;
        g.set_canonical_executor_routing_enabled(true);
        register_transport_custom(g, &sink, /*with_transport_cb=*/true);
        const auto in = g.add_input_node(2, "In");
        const auto cust = g.add_custom_node("transport.recorder", "Rec");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, cust, c));
            REQUIRE(g.connect(cust, c, out, c));
        }
        REQUIRE(g.prepare(kSr, kFrames));
        const auto t = non_default_transport();
        (void)render_block(g, &t);
        REQUIRE(sink.got_transport == true);
        REQUIRE(sink.last.tempo_bpm == 140.0);
        REQUIRE(sink.plain_calls == 0);  // the transport callback took over
    }
    SECTION("plain process only -> the custom node is transport-unaware") {
        CustomTransportSink sink;
        SignalGraph g;
        g.set_canonical_executor_routing_enabled(true);
        register_transport_custom(g, &sink, /*with_transport_cb=*/false);
        const auto in = g.add_input_node(2, "In");
        const auto cust = g.add_custom_node("transport.recorder", "Rec");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, cust, c));
            REQUIRE(g.connect(cust, c, out, c));
        }
        REQUIRE(g.prepare(kSr, kFrames));
        const auto t = non_default_transport();
        (void)render_block(g, &t);
        REQUIRE(sink.got_transport == false);
        REQUIRE(sink.plain_calls == 1);  // the plain process ran with transport present
    }
}

// ── T5: transport-sensitive node coexists with active anticipation ───────────

namespace {

// Two branches into one live sink:
//   gen1(CountingGen, 0/2) -> gain1 -> out   (transport-insensitive interior)
//   gen2(TransportGen,  0/2) -> gain2 -> out   (transport-sensitive, exterior)
// gen1's branch is an anticipation-eligible latent interior; gen2 opts into
// transport so it (and its gain) is forced exterior and runs live. Returns the
// transport-sensitive generator so the test can read what it received.
TransportGen* wire_two_branch_graph(SignalGraph& g) {
    auto gen2 = std::make_unique<TransportGen>();
    auto* raw2 = gen2.get();
    const auto g1 = g.add_plugin_node(std::make_unique<CountingGen>(), 0, 2, "Gen1");
    const auto gain1 = g.add_gain_node("G1");
    const auto g2 = g.add_plugin_node(std::move(gen2), 0, 2, "Gen2");
    const auto gain2 = g.add_gain_node("G2");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(g1, c, gain1, c));
        REQUIRE(g.connect(gain1, c, out, c));
        REQUIRE(g.connect(g2, c, gain2, c));
        REQUIRE(g.connect(gain2, c, out, c));
    }
    REQUIRE(g.set_node_gain(gain1, 0.5f));
    REQUIRE(g.set_node_gain(gain2, 0.25f));
    return raw2;
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

TEST_CASE("SignalGraph anticipation forces a transport-sensitive node exterior and feeds it transport",
          "[host][signal-graph][transport][anticipation]") {
    // Oracle: anticipation active, no transport. Subject: identical graph,
    // anticipation active, with a non-default transport. The transport-insensitive
    // interior (gen1/gain1) is ahead-rendered identically in both; the
    // transport-sensitive branch (gen2/gain2) runs live in both and produces
    // identical audio (TransportGen is transport-independent), so output is
    // bit-identical. Only the subject's gen2 RECEIVES the transport.
    SignalGraph oracle;
    oracle.set_canonical_executor_routing_enabled(true);
    oracle.set_anticipation_enabled(true);
    auto* oracle_gen2 = wire_two_branch_graph(oracle);
    REQUIRE(oracle.prepare(kSr, kFrames));

    SignalGraph subject;
    subject.set_canonical_executor_routing_enabled(true);
    subject.set_anticipation_enabled(true);
    auto* subject_gen2 = wire_two_branch_graph(subject);
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

    // The subject's transport-sensitive generator ran live/exterior and saw the
    // live playhead; the oracle's never had a transport supplied.
    REQUIRE(subject_gen2->got_transport == true);
    REQUIRE(subject_gen2->last.tempo_bpm == 140.0);
    REQUIRE(subject_gen2->last.position_samples == 4096);
    REQUIRE(oracle_gen2->got_transport == false);

    // The counter now reports transport-sensitive nodes FORCED EXTERIOR by
    // anticipation (a compile-time capability count), not per-block suppression.
    // Both graphs have exactly one transport-sensitive node (gen2); gain2 is a
    // plain Gain (not transport-sensitive) even though it inherits the exclusion.
    REQUIRE(subject.transport_suppressed_for_anticipation() == 1);
    REQUIRE(oracle.transport_suppressed_for_anticipation() == 1);
}
