// BakedGraphProcessor parity + refusal proof.
//
// A fully-lowerable SignalGraph (AudioInput / AudioOutput / Gain, with fan-in
// and a feedback edge) baked into a BakedGraphProcessor must produce output
// bit-identical to the live graph, because both drive the SAME
// GraphRuntimeExecutor::process_routed over the same frozen plan — so any
// per-sample difference is a real bake/ownership bug (a mis-copied gain, a
// dangling atomic, a wrongly sized pool, or lost feedback state). The refusal
// cases prove non-lowerable graphs (hosted Plugin, Custom, unprepared) are
// rejected loudly with a null processor and a specific reason, never silently
// mis-baked.

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using pulp::host::BakedGraphProcessor;
using pulp::host::LowerRejectReason;
using pulp::host::LowerResult;
using pulp::host::SignalGraph;
using pulp::host::bake;
using pulp::host::lowerability_of;
using pulp::host::PluginInfo;
using pulp::host::CustomNodeType;

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;

std::vector<float> ramp(int n, float seed) {
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        v[static_cast<std::size_t>(i)] =
            (static_cast<float>((i * 2654435761u) & 0xFFFF) / 32768.0f - 1.0f) * seed;
    }
    return v;
}

// Drive SignalGraph's own block walk; returns per-channel output.
std::vector<std::vector<float>> run_graph(SignalGraph& g, int frames,
                                          const std::vector<std::vector<float>>& in,
                                          std::size_t out_channels) {
    std::vector<std::vector<float>> ins = in;
    std::vector<std::vector<float>> outs(out_channels,
                                         std::vector<float>(static_cast<std::size_t>(frames), 0.0f));
    std::vector<const float*> in_ptrs;
    std::vector<float*> out_ptrs;
    for (auto& c : ins) in_ptrs.push_back(c.data());
    for (auto& c : outs) out_ptrs.push_back(c.data());
    pulp::audio::BufferView<const float> in_view(in_ptrs.data(), in_ptrs.size(),
                                                 static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> out_view(out_ptrs.data(), out_ptrs.size(),
                                            static_cast<std::uint32_t>(frames));
    g.process(out_view, in_view, frames);
    return outs;
}

// Drive a baked Processor for one block; returns per-channel output.
std::vector<std::vector<float>> run_baked(pulp::format::Processor& proc, int frames,
                                          const std::vector<std::vector<float>>& in,
                                          std::size_t out_channels) {
    std::vector<std::vector<float>> ins = in;
    std::vector<std::vector<float>> outs(out_channels,
                                         std::vector<float>(static_cast<std::size_t>(frames), 0.0f));
    std::vector<const float*> in_ptrs;
    std::vector<float*> out_ptrs;
    for (auto& c : ins) in_ptrs.push_back(c.data());
    for (auto& c : outs) out_ptrs.push_back(c.data());
    pulp::audio::BufferView<const float> in_view(in_ptrs.data(), in_ptrs.size(),
                                                 static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> out_view(out_ptrs.data(), out_ptrs.size(),
                                            static_cast<std::uint32_t>(frames));
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = kSr;
    ctx.num_samples = frames;
    proc.process(out_view, in_view, midi_in, midi_out, ctx);
    return outs;
}

void expect_equal(const std::vector<std::vector<float>>& a,
                  const std::vector<std::vector<float>>& b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t c = 0; c < a.size(); ++c) {
        REQUIRE(a[c].size() == b[c].size());
        for (std::size_t i = 0; i < a[c].size(); ++i) REQUIRE(a[c][i] == b[c][i]);
    }
}

pulp::format::PrepareContext make_prepare_ctx(int channels) {
    pulp::format::PrepareContext ctx;
    ctx.sample_rate = kSr;
    ctx.max_buffer_size = kFrames;
    ctx.input_channels = channels;
    ctx.output_channels = channels;
    return ctx;
}

// A minimal live PluginSlot: full-write pass-through. Its mere presence as a
// hosted node must make the graph non-self-contained and refuse to bake.
using pulp::host::PluginFormat;
using pulp::host::PluginInfo;
using pulp::host::PluginSlot;

PluginInfo passthrough_info(int in_ch, int out_ch) {
    PluginInfo info{};
    info.name = "BakeRefusalMock";
    info.format = PluginFormat::CLAP;
    info.num_inputs = in_ch;
    info.num_outputs = out_ch;
    info.category = "Fx";
    return info;
}

class PassthroughPlugin final : public PluginSlot {
public:
    PassthroughPlugin(int in_ch, int out_ch) : info_(passthrough_info(in_ch, out_ch)) {}
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&, int n) override {
        const std::size_t chs = out.num_channels();
        for (std::size_t c = 0; c < chs; ++c) {
            float* o = out.channel_ptr(c);
            const float* i = c < in.num_channels() ? in.channel_ptr(c) : nullptr;
            for (int s = 0; s < n; ++s) {
                o[static_cast<std::size_t>(s)] =
                    i ? i[static_cast<std::size_t>(s)] : 0.0f;
            }
        }
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override { return {}; }
    float get_parameter(std::uint32_t) const override { return 0.0f; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

private:
    PluginInfo info_;
};

} // namespace

TEST_CASE("Baked graph matches the live graph bit-exactly across blocks",
          "[host][graph][bake][parity]") {
    // in -> {g1, g2, g3} -> out:0 (three-way fan-in sum, distinct gains so the
    // mix order is observable), plus a self-feedback edge on g1 so one-block-delay
    // state is compared across block boundaries.
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto g1 = g.add_gain_node("G1");
    const auto g2 = g.add_gain_node("G2");
    const auto g3 = g.add_gain_node("G3");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, g1, 0));
    REQUIRE(g.connect(in, 0, g2, 0));
    REQUIRE(g.connect(in, 0, g3, 0));
    REQUIRE(g.connect_feedback(g1, 0, g1, 0));  // feedback state on a live gain
    REQUIRE(g.connect(g1, 0, out, 0));
    REQUIRE(g.connect(g2, 0, out, 0));
    REQUIRE(g.connect(g3, 0, out, 0));
    REQUIRE(g.set_node_gain(g1, 0.4101011f));
    REQUIRE(g.set_node_gain(g2, 0.1717171f));
    REQUIRE(g.set_node_gain(g3, 0.7373737f));
    // Drive the live graph through the canonical executor too, so both the live
    // and baked sides run the identical process_routed plan with feedback state
    // seeded from zero — any difference is then purely a bake/ownership bug.
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));

    auto r = bake(g);
    REQUIRE(r.accepted);
    REQUIRE(r.processor);
    REQUIRE(r.reason == LowerRejectReason::None);
    r.processor->prepare(make_prepare_ctx(1));

    // Identical per-block inputs to both paths; require bit-exact output every
    // sample across enough blocks that the feedback state crosses boundaries.
    float peak = 0.0f;
    for (int blk = 0; blk < 6; ++blk) {
        INFO("block " << blk);
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.55f + 0.07f * static_cast<float>(blk))};
        const auto live = run_graph(g, kFrames, input, 1);
        const auto baked = run_baked(*r.processor, kFrames, input, 1);
        expect_equal(live, baked);
        for (float s : baked[0]) peak = std::max(peak, std::fabs(s));
    }
    // Non-vacuity guard: both paths must actually produce signal, so a "both
    // silent" regression can't pass the bit-exact comparison trivially.
    CHECK(peak > 0.1f);
}

TEST_CASE("Baked graph matches the live graph's legacy WALK bit-exactly",
          "[host][graph][bake][parity]") {
    // The baked Processor's documented contract is "matches the live graph's walk."
    // Canonical-executor routing is ON by default, so force the live graph OFF to
    // render the legacy WALK and compare its output to the baked Processor (which
    // runs process_routed). Bit-exact here proves baked == walk directly, not
    // merely baked == executor.
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto g1 = g.add_gain_node("G1");
    const auto g2 = g.add_gain_node("G2");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, g1, 0));
    REQUIRE(g.connect(in, 0, g2, 0));
    REQUIRE(g.connect_feedback(g1, 0, g1, 0));  // feedback exercised across blocks
    REQUIRE(g.connect(g1, 0, out, 0));
    REQUIRE(g.connect(g2, 0, out, 0));
    REQUIRE(g.set_node_gain(g1, 0.3838383f));
    REQUIRE(g.set_node_gain(g2, 0.6262626f));
    g.set_canonical_executor_routing_enabled(false);  // force the legacy walk
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE_FALSE(g.canonical_executor_routing_enabled());  // really the walk

    auto r = bake(g);
    REQUIRE(r.accepted);
    REQUIRE(r.processor);
    r.processor->prepare(make_prepare_ctx(1));

    float peak = 0.0f;
    for (int blk = 0; blk < 6; ++blk) {
        INFO("block " << blk);
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.5f + 0.06f * static_cast<float>(blk))};
        const auto walk = run_graph(g, kFrames, input, 1);   // legacy walk output
        const auto baked = run_baked(*r.processor, kFrames, input, 1);
        expect_equal(walk, baked);
        for (float s : baked[0]) peak = std::max(peak, std::fabs(s));
    }
    CHECK(peak > 0.1f);  // non-vacuity: real signal on both paths
}

TEST_CASE("Baked processor process() is allocation-free on the audio thread",
          "[host][graph][bake][rt-safety]") {
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto gn = g.add_gain_node("G");
    const auto out = g.add_output_node(2, "Out");
    REQUIRE(g.connect(in, 0, gn, 0));
    REQUIRE(g.connect(in, 1, gn, 1));
    REQUIRE(g.connect(gn, 0, out, 0));
    REQUIRE(g.connect(gn, 1, out, 1));
    REQUIRE(g.set_node_gain(gn, 0.5f));
    REQUIRE(g.prepare(kSr, kFrames));

    auto r = bake(g);
    REQUIRE(r.accepted);
    r.processor->prepare(make_prepare_ctx(2));

    // Build every buffer up front so the only work inside the probe is the
    // process() call itself (run_baked allocates its scratch vectors and would
    // mask the measurement).
    std::vector<float> li = ramp(kFrames, 0.8f), ri = ramp(kFrames, 0.6f);
    std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
    std::array<const float*, 2> ic{li.data(), ri.data()};
    std::array<float*, 2> oc{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = kSr;
    ctx.num_samples = kFrames;

    r.processor->process(ov, iv, midi_in, midi_out, ctx);  // warm-up outside the probe
    {
        pulp::test::RtAllocationProbe probe;
        r.processor->process(ov, iv, midi_in, midi_out, ctx);
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

TEST_CASE("Bake refuses a hosted Plugin node loudly",
          "[host][graph][bake][refusal]") {
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p = g.add_plugin_node(
        std::make_unique<PassthroughPlugin>(2, 2), 2, 2, "P");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));
        REQUIRE(g.connect(p, c, out, c));
    }
    REQUIRE(g.prepare(kSr, kFrames));

    auto r = bake(g);
    REQUIRE_FALSE(r.accepted);
    REQUIRE(r.processor == nullptr);
    REQUIRE(r.reason == LowerRejectReason::HostedPluginNotSelfContained);
    REQUIRE(r.offending_node == p);
    REQUIRE_FALSE(r.message.empty());
}

TEST_CASE("Bake refuses a Custom node loudly",
          "[host][graph][bake][refusal]") {
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto cnode = g.add_unresolved_custom_node("test.custom", 1, 1, 1, "Custom");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, cnode, 0));
    REQUIRE(g.connect(cnode, 0, out, 0));
    REQUIRE(g.prepare(kSr, kFrames));

    auto r = bake(g);
    REQUIRE_FALSE(r.accepted);
    REQUIRE(r.processor == nullptr);
    REQUIRE(r.reason == LowerRejectReason::CustomNotYetLowerable);
    REQUIRE(r.offending_node == cnode);
    REQUIRE_FALSE(r.message.empty());
}

TEST_CASE("Bake refuses a non-audio (MIDI) lane loudly",
          "[host][graph][bake][refusal]") {
    // The routed executor accepts MIDI nodes/edges, but a BakedGraphProcessor
    // advertises no MIDI bus and process() carries no MIDI scratch, so a MIDI
    // lane must be refused rather than silently dropped.
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto gn = g.add_gain_node("G");
    const auto out = g.add_output_node(1, "Out");
    const auto min = g.add_midi_input_node("MIDI In");
    const auto mout = g.add_midi_output_node("MIDI Out");
    REQUIRE(g.connect(in, 0, gn, 0));
    REQUIRE(g.connect(gn, 0, out, 0));
    REQUIRE(g.connect_midi(min, mout));
    REQUIRE(g.set_node_gain(gn, 0.5f));
    REQUIRE(g.prepare(kSr, kFrames));

    auto r = bake(g);
    REQUIRE_FALSE(r.accepted);
    REQUIRE(r.processor == nullptr);
    REQUIRE(r.reason == LowerRejectReason::NonAudioLaneNotLowerable);
    REQUIRE((r.offending_node == min || r.offending_node == mout));
    REQUIRE_FALSE(r.message.empty());
}

TEST_CASE("Baked processor emits silence for an oversized block",
          "[host][graph][bake][rt-safety]") {
    // process() is sized for prepared_max_block_; a larger host block must clear
    // the output (process_routed reports BufferPoolTooSmall WITHOUT zeroing), not
    // leave the caller's stale buffer intact.
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto gn = g.add_gain_node("G");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, gn, 0));
    REQUIRE(g.connect(gn, 0, out, 0));
    REQUIRE(g.set_node_gain(gn, 0.5f));
    REQUIRE(g.prepare(kSr, kFrames));

    auto r = bake(g);
    REQUIRE(r.accepted);
    r.processor->prepare(make_prepare_ctx(1));  // max_buffer_size == kFrames

    const int big = kFrames * 2;  // larger than the prepared max
    std::vector<float> input(static_cast<std::size_t>(big), 0.5f);
    std::vector<float> output(static_cast<std::size_t>(big), 7.0f);  // sentinel
    const float* ip = input.data();
    float* op = output.data();
    pulp::audio::BufferView<const float> iv(&ip, 1, static_cast<std::uint32_t>(big));
    pulp::audio::BufferView<float> ov(&op, 1, static_cast<std::uint32_t>(big));
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx;
    ctx.sample_rate = kSr;
    ctx.num_samples = big;

    r.processor->process(ov, iv, midi_in, midi_out, ctx);
    for (float s : output) REQUIRE(s == 0.0f);  // the guard cleared the block
}

TEST_CASE("Bake refuses an unprepared graph loudly",
          "[host][graph][bake][refusal]") {
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto gn = g.add_gain_node("G");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, gn, 0));
    REQUIRE(g.connect(gn, 0, out, 0));
    REQUIRE(g.set_node_gain(gn, 0.5f));
    // No prepare() — the live snapshot does not exist yet.

    auto r = bake(g);
    REQUIRE_FALSE(r.accepted);
    REQUIRE(r.processor == nullptr);
    REQUIRE(r.reason == LowerRejectReason::NotPrepared);
}

// Direct unit coverage of lowerability_of() — the shared gate bake() consults to
// prove a topology is bakeable — asserting the accept case and each refusal reason
// independently of bake()'s is_prepared() precondition.
TEST_CASE("lowerability_of proves the bakeable subset and refuses with a reason",
          "[host][bake][lowerability]") {
    SECTION("audio I/O + Gain + plain-audio connections are lowerable") {
        SignalGraph g;
        const auto in = g.add_input_node(1, "In");
        const auto gn = g.add_gain_node("G");
        const auto out = g.add_output_node(1, "Out");
        REQUIRE(g.connect(in, 0, gn, 0));
        REQUIRE(g.connect(gn, 0, out, 0));
        const auto proof = lowerability_of(g.nodes(), g.connections());
        CHECK(proof.accepted);
        CHECK(proof.reason == LowerRejectReason::None);
    }
    SECTION("a Plugin node -> HostedPluginNotSelfContained") {
        SignalGraph g;
        const auto in = g.add_input_node(1, "In");
        const auto p = g.add_unresolved_plugin_node(PluginInfo{}, 1, 1, "P");
        const auto out = g.add_output_node(1, "Out");
        REQUIRE(g.connect(in, 0, p, 0));
        REQUIRE(g.connect(p, 0, out, 0));
        const auto proof = lowerability_of(g.nodes(), g.connections());
        CHECK_FALSE(proof.accepted);
        CHECK(proof.reason == LowerRejectReason::HostedPluginNotSelfContained);
        CHECK(proof.offending_node == p);
    }
    SECTION("a MIDI node -> NonAudioLaneNotLowerable") {
        SignalGraph g;
        (void)g.add_input_node(1, "In");
        const auto mi = g.add_midi_input_node("MI");
        (void)g.add_output_node(1, "Out");
        const auto proof = lowerability_of(g.nodes(), g.connections());
        CHECK_FALSE(proof.accepted);
        CHECK(proof.reason == LowerRejectReason::NonAudioLaneNotLowerable);
        CHECK(proof.offending_node == mi);
    }
}

TEST_CASE("Baked graph with a lowerable Custom node matches the live graph bit-exactly",
          "[host][graph][bake][custom][parity]") {
    // A stateless, transport-independent Custom type that opts into baking. bake()
    // captures a copy of its resolved process callback; the baked Processor runs the
    // same callback, so output must match the live graph sample-for-sample.
    SignalGraph g;
    CustomNodeType t;
    t.type_id = "bakegain";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.lowerable = true;
    t.process = [](pulp::audio::BufferView<float>& out,
                   const pulp::audio::BufferView<const float>& in, int n) {
        const std::size_t chs = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < chs; ++c) {
            const float* i = in.channel_ptr(c);
            float* o = out.channel_ptr(c);
            for (int k = 0; k < n; ++k)
                o[static_cast<std::size_t>(k)] = 0.5f * i[static_cast<std::size_t>(k)];
        }
    };
    REQUIRE(g.register_custom_node_type(t));
    const auto in = g.add_input_node(1, "In");
    const auto cn = g.add_custom_node("bakegain", 1, "C");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, cn, 0));
    REQUIRE(g.connect(cn, 0, out, 0));
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));

    auto r = bake(g);
    REQUIRE(r.accepted);
    REQUIRE(r.processor);
    REQUIRE(r.reason == LowerRejectReason::None);
    r.processor->prepare(make_prepare_ctx(1));

    float peak = 0.0f;
    for (int blk = 0; blk < 4; ++blk) {
        INFO("block " << blk);
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.6f + 0.05f * static_cast<float>(blk))};
        const auto live = run_graph(g, kFrames, input, 1);
        const auto baked = run_baked(*r.processor, kFrames, input, 1);
        expect_equal(live, baked);
        for (float s : baked[0]) peak = std::max(peak, std::fabs(s));
    }
    CHECK(peak > 0.1f);
}

TEST_CASE("bake refuses a non-lowerable or transport-sensitive Custom node",
          "[host][graph][bake][custom]") {
    SECTION("lowerable=false -> CustomNotLowerable") {
        SignalGraph g;
        CustomNodeType t;
        t.type_id = "notlower";
        t.version = 1;
        t.num_input_ports = 1;
        t.num_output_ports = 1;
        t.lowerable = false;
        t.process = [](pulp::audio::BufferView<float>&,
                       const pulp::audio::BufferView<const float>&, int) {};
        REQUIRE(g.register_custom_node_type(t));
        const auto in = g.add_input_node(1, "In");
        const auto cn = g.add_custom_node("notlower", 1, "C");
        const auto out = g.add_output_node(1, "Out");
        REQUIRE(g.connect(in, 0, cn, 0));
        REQUIRE(g.connect(cn, 0, out, 0));
        REQUIRE(g.prepare(kSr, kFrames));
        auto r = bake(g);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(r.reason == LowerRejectReason::CustomNotLowerable);
    }
    SECTION("transport-sensitive -> CustomTransportNotLowerable") {
        SignalGraph g;
        CustomNodeType t;
        t.type_id = "xport";
        t.version = 1;
        t.num_input_ports = 1;
        t.num_output_ports = 1;
        t.lowerable = true;
        t.process = [](pulp::audio::BufferView<float>&,
                       const pulp::audio::BufferView<const float>&, int) {};
        t.process_transport = [](pulp::audio::BufferView<float>&,
                                 const pulp::audio::BufferView<const float>&, int,
                                 const pulp::format::ProcessContext&) {};
        REQUIRE(g.register_custom_node_type(t));
        const auto in = g.add_input_node(1, "In");
        const auto cn = g.add_custom_node("xport", 1, "C");
        const auto out = g.add_output_node(1, "Out");
        REQUIRE(g.connect(in, 0, cn, 0));
        REQUIRE(g.connect(cn, 0, out, 0));
        REQUIRE(g.prepare(kSr, kFrames));
        auto r = bake(g);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(r.reason == LowerRejectReason::CustomTransportNotLowerable);
    }
}

TEST_CASE("A signed .pulpbake round-trips to a bit-identical baked Processor",
          "[host][graph][bake][codec][parity]") {
    // Full on-disk round-trip: live graph -> bake_to_plan -> write_baked_signed ->
    // load_baked (verify + rebuild + re-bake) must reproduce the live graph exactly,
    // and an untrusted signer must be refused at load.
    SignalGraph g;
    CustomNodeType t;
    t.type_id = "bakegain";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.lowerable = true;
    t.process = [](pulp::audio::BufferView<float>& out,
                   const pulp::audio::BufferView<const float>& in, int n) {
        const std::size_t chs = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < chs; ++c) {
            const float* i = in.channel_ptr(c);
            float* o = out.channel_ptr(c);
            for (int k = 0; k < n; ++k)
                o[static_cast<std::size_t>(k)] = 0.5f * i[static_cast<std::size_t>(k)];
        }
    };
    REQUIRE(g.register_custom_node_type(t));
    const auto in = g.add_input_node(1, "In");
    const auto cn = g.add_custom_node("bakegain", 1, "C");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, cn, 0));
    REQUIRE(g.connect(cn, 0, out, 0));
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));

    const auto plan = pulp::host::bake_to_plan(g);
    REQUIRE(plan.accepted);
    REQUIRE(plan.plan.has_value());

    std::array<std::uint8_t, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<std::uint8_t>(i + 7);
    const auto kp = pulp::runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp.has_value());
    const auto bytes = pulp::host::write_baked_signed(*plan.plan, kp->private_key);
    REQUIRE_FALSE(bytes.empty());

    pulp::host::BakedTrust trust;
    trust.trusted_public_keys.push_back(kp->public_key);
    auto r = pulp::host::load_baked(bytes, trust, {t});
    REQUIRE(r.accepted);
    REQUIRE(r.processor);
    r.processor->prepare(make_prepare_ctx(1));

    float peak = 0.0f;
    for (int blk = 0; blk < 4; ++blk) {
        INFO("block " << blk);
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.6f + 0.05f * static_cast<float>(blk))};
        const auto live = run_graph(g, kFrames, input, 1);
        const auto baked = run_baked(*r.processor, kFrames, input, 1);
        expect_equal(live, baked);
        for (float s : baked[0]) peak = std::max(peak, std::fabs(s));
    }
    CHECK(peak > 0.1f);

    // Untrusted signer (empty trust set) is refused at the envelope stage.
    const auto rejected = pulp::host::load_baked(bytes, pulp::host::BakedTrust{}, {t});
    CHECK_FALSE(rejected.accepted);
    CHECK(rejected.reason == LowerRejectReason::CodecRejected);
}

TEST_CASE("Baked processor survives in-place (aliased) host buffers",
          "[host][graph][bake][in-place]") {
    // Logic-style in-place hosts hand process() input and output views over the
    // SAME memory. process_routed zeroes the main output bus before gathering
    // AudioInput, so without the scratch-copy guard the input is destroyed and
    // the output is total silence. The output must be the correctly-processed
    // signal (0.5 × input), not silence.
    const float kGain = 0.5f;

    SECTION("mono") {
        SignalGraph g;
        const auto in = g.add_input_node(1, "In");
        const auto gn = g.add_gain_node("G");
        const auto out = g.add_output_node(1, "Out");
        REQUIRE(g.connect(in, 0, gn, 0));
        REQUIRE(g.connect(gn, 0, out, 0));
        REQUIRE(g.set_node_gain(gn, kGain));
        REQUIRE(g.prepare(kSr, kFrames));

        auto r = bake(g);
        REQUIRE(r.accepted);
        r.processor->prepare(make_prepare_ctx(1));

        // ONE buffer, viewed as both input and output.
        std::vector<float> shared = ramp(kFrames, 0.7f);
        const std::vector<float> original = shared;
        const float* ip = shared.data();
        float* op = shared.data();
        pulp::audio::BufferView<const float> iv(&ip, 1, kFrames);
        pulp::audio::BufferView<float> ov(&op, 1, kFrames);
        pulp::midi::MidiBuffer midi_in, midi_out;
        pulp::format::ProcessContext ctx;
        ctx.sample_rate = kSr;
        ctx.num_samples = kFrames;
        r.processor->process(ov, iv, midi_in, midi_out, ctx);

        float peak = 0.0f;
        for (int s = 0; s < kFrames; ++s) {
            REQUIRE(shared[static_cast<std::size_t>(s)] ==
                    kGain * original[static_cast<std::size_t>(s)]);
            peak = std::max(peak, std::fabs(shared[static_cast<std::size_t>(s)]));
        }
        CHECK(peak > 0.1f);  // non-vacuity: real signal, not silence == silence
    }

    SECTION("stereo, allocation-free on the audio thread") {
        SignalGraph g;
        const auto in = g.add_input_node(2, "In");
        const auto gn = g.add_gain_node("G");
        const auto out = g.add_output_node(2, "Out");
        REQUIRE(g.connect(in, 0, gn, 0));
        REQUIRE(g.connect(in, 1, gn, 1));
        REQUIRE(g.connect(gn, 0, out, 0));
        REQUIRE(g.connect(gn, 1, out, 1));
        REQUIRE(g.set_node_gain(gn, kGain));
        REQUIRE(g.prepare(kSr, kFrames));

        auto r = bake(g);
        REQUIRE(r.accepted);
        r.processor->prepare(make_prepare_ctx(2));

        // Per-channel buffers shared between the input and output views.
        std::vector<float> left = ramp(kFrames, 0.8f), right = ramp(kFrames, 0.6f);
        const std::vector<float> lorig = left, rorig = right;
        std::array<const float*, 2> ic{left.data(), right.data()};
        std::array<float*, 2> oc{left.data(), right.data()};
        pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
        pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);
        pulp::midi::MidiBuffer midi_in, midi_out;
        pulp::format::ProcessContext ctx;
        ctx.sample_rate = kSr;
        ctx.num_samples = kFrames;

        r.processor->process(ov, iv, midi_in, midi_out, ctx);  // warm-up
        // Re-seed and run the measured aliased block: the overlap detection +
        // scratch copy must not allocate on the audio thread.
        left = lorig;
        right = rorig;
        {
            pulp::test::RtAllocationProbe probe;
            r.processor->process(ov, iv, midi_in, midi_out, ctx);
            REQUIRE_FALSE(probe.saw_allocation());
        }

        float peak = 0.0f;
        for (int s = 0; s < kFrames; ++s) {
            REQUIRE(left[static_cast<std::size_t>(s)] ==
                    kGain * lorig[static_cast<std::size_t>(s)]);
            REQUIRE(right[static_cast<std::size_t>(s)] ==
                    kGain * rorig[static_cast<std::size_t>(s)]);
            peak = std::max(peak, std::fabs(left[static_cast<std::size_t>(s)]));
        }
        CHECK(peak > 0.1f);
    }
}

TEST_CASE("Baked processor prepare() re-inits stateful Custom instance state",
          "[host][graph][bake][custom][lifecycle]") {
    // prepare() is a re-init boundary: a stateful Custom instance's DSP state
    // (here a delay line's contents) must NOT survive a re-prepare. Without the
    // captured lifecycle hooks the baked Processor never re-runs the type's
    // prepare/reset, so audio pushed before the re-prepare would recirculate
    // into the fresh stream.
    constexpr std::size_t kDelay = 48;
    struct DelayState {
        std::vector<float> line;
        std::size_t pos = 0;
    };
    int prepare_calls = 0;

    SignalGraph g;
    CustomNodeType t;
    t.type_id = "bakedelay";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.lowerable = true;
    t.create = []() -> void* { return new DelayState(); };
    t.destroy = [](void* p) { delete static_cast<DelayState*>(p); };
    // Allocate-only prepare (contents preserved), so clearing stale state is
    // proven to come from the reset hook, not incidentally from prepare.
    t.prepare = [&prepare_calls](void* p, double, int) {
        auto* s = static_cast<DelayState*>(p);
        if (s->line.empty()) s->line.assign(kDelay, 0.0f);
        ++prepare_calls;
    };
    t.reset = [](void* p) {
        auto* s = static_cast<DelayState*>(p);
        std::fill(s->line.begin(), s->line.end(), 0.0f);
        s->pos = 0;
    };
    t.process_instance = [](void* p, pulp::audio::BufferView<float>& out,
                            const pulp::audio::BufferView<const float>& in, int n) {
        auto* s = static_cast<DelayState*>(p);
        const float* i = in.channel_ptr(0);
        float* o = out.channel_ptr(0);
        for (int k = 0; k < n; ++k) {
            o[static_cast<std::size_t>(k)] = s->line[s->pos];
            s->line[s->pos] = i[static_cast<std::size_t>(k)];
            s->pos = (s->pos + 1) % s->line.size();
        }
    };
    REQUIRE(g.register_custom_node_type(t));
    const auto in = g.add_input_node(1, "In");
    const auto cn = g.add_custom_node("bakedelay", 1, "C");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, cn, 0));
    REQUIRE(g.connect(cn, 0, out, 0));
    REQUIRE(g.prepare(kSr, kFrames));  // instance created + prepared once here

    auto r = bake(g);
    REQUIRE(r.accepted);
    REQUIRE(r.processor);
    r.processor->prepare(make_prepare_ctx(1));
    // The baked prepare must re-run the type's prepare hook (the graph's own
    // prepare ran it once; the baked Processor must not depend on that rate).
    CHECK(prepare_calls == 2);

    // Push signal so the delay line accumulates state, and prove the delay is
    // live (the delayed ramp emerges after kDelay samples — non-vacuous state).
    const std::vector<std::vector<float>> loud{ramp(kFrames, 0.9f)};
    const auto before = run_baked(*r.processor, kFrames, loud, 1);
    float tail_peak = 0.0f;
    for (int s = static_cast<int>(kDelay); s < kFrames; ++s) {
        tail_peak = std::max(tail_peak, std::fabs(before[0][static_cast<std::size_t>(s)]));
    }
    REQUIRE(tail_peak > 0.1f);  // the line really carries state

    // Re-prepare: the boundary that must discard that state.
    r.processor->prepare(make_prepare_ctx(1));
    CHECK(prepare_calls == 3);

    // A silent block after the re-prepare must be ALL zeros — any nonzero
    // sample is pre-boundary audio recirculating out of the stale delay line.
    const std::vector<std::vector<float>> silence{
        std::vector<float>(static_cast<std::size_t>(kFrames), 0.0f)};
    const auto after = run_baked(*r.processor, kFrames, silence, 1);
    for (int s = 0; s < kFrames; ++s) {
        INFO("sample " << s);
        REQUIRE(after[0][static_cast<std::size_t>(s)] == 0.0f);
    }
}

TEST_CASE("signed bake preserves stateful Custom nodes across disk and prepare",
          "[host][graph][bake][codec][state]") {
    struct GainState {
        float gain = 0.0f;
    };
    CustomNodeType type;
    type.type_id = "pulp.test.stateful-bake-gain";
    type.version = 1;
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.lowerable = true;
    type.create = []() -> void* { return new GainState(); };
    type.destroy = [](void* state) { delete static_cast<GainState*>(state); };
    type.reset = [](void* state) { static_cast<GainState*>(state)->gain = 0.0f; };
    type.save_state = [](void* state) {
        std::vector<std::uint8_t> bytes(sizeof(float));
        const float gain = static_cast<GainState*>(state)->gain;
        std::memcpy(bytes.data(), &gain, sizeof(gain));
        return bytes;
    };
    type.load_state = [](void* state, std::span<const std::uint8_t> bytes) {
        if (bytes.size() != sizeof(float)) return false;
        float gain = 0.0f;
        std::memcpy(&gain, bytes.data(), sizeof(gain));
        if (!std::isfinite(gain) || gain < 0.0f || gain > 1.0f) return false;
        static_cast<GainState*>(state)->gain = gain;
        return true;
    };
    type.process_instance =
        [](void* state, pulp::audio::BufferView<float>& output,
           const pulp::audio::BufferView<const float>& input, int frames) {
            auto* gain_state = static_cast<GainState*>(state);
            for (int frame = 0; frame < frames; ++frame) {
                output.channel_ptr(0)[frame] =
                    input.channel_ptr(0)[frame] * gain_state->gain;
            }
            gain_state->gain = std::min(1.0f, gain_state->gain + 0.1f);
        };

    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(type));
    const auto input = graph.add_input_node(1, "In");
    const auto custom = graph.add_custom_node(type.type_id, type.version, "Stateful gain");
    const auto output = graph.add_output_node(1, "Out");
    REQUIRE(graph.connect(input, 0, custom, 0));
    REQUIRE(graph.connect(custom, 0, output, 0));

    float initial_gain = 0.75f;
    std::vector<std::uint8_t> initial_state(sizeof(initial_gain));
    std::memcpy(initial_state.data(), &initial_gain, sizeof(initial_gain));
    REQUIRE(graph.set_custom_node_state(custom, initial_state));
    REQUIRE(graph.prepare(kSr, kFrames));

    // Advance live instance state after the authored blob was staged. The bake
    // writer must retain the authored state rather than race save_state() against
    // a potentially-live audio callback.
    const std::vector<std::vector<float>> ones{
        std::vector<float>(static_cast<std::size_t>(kFrames), 1.0f)};
    const auto live = run_graph(graph, kFrames, ones, 1);
    CHECK(live[0][0] == 0.75f);

    const auto plan_result = pulp::host::bake_to_plan(graph);
    REQUIRE(plan_result.accepted);
    REQUIRE(plan_result.plan);
    const auto plan_node = std::find_if(
        plan_result.plan->nodes.begin(), plan_result.plan->nodes.end(),
        [custom](const pulp::host::BakedPlan::Node& node) {
            return node.id == custom;
        });
    REQUIRE(plan_node != plan_result.plan->nodes.end());
    REQUIRE(plan_node->custom_state.size() == sizeof(float));
    float saved_gain = 0.0f;
    std::memcpy(&saved_gain, plan_node->custom_state.data(), sizeof(saved_gain));
    CHECK(saved_gain == 0.75f);

    std::array<std::uint8_t, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<std::uint8_t>(i + 11);
    const auto kp = pulp::runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp.has_value());
    const auto bytes =
        pulp::host::write_baked_signed(*plan_result.plan, kp->private_key);
    REQUIRE_FALSE(bytes.empty());
    pulp::host::BakedTrust trust;
    trust.trusted_public_keys.push_back(kp->public_key);

    auto loaded = pulp::host::load_baked(bytes, trust, {type});
    REQUIRE(loaded.accepted);
    REQUIRE(loaded.processor);
    loaded.processor->prepare(make_prepare_ctx(1));
    auto restored = run_baked(*loaded.processor, kFrames, ones, 1);
    CHECK(restored[0][0] == 0.75f);

    // Host re-prepare runs the type's reset hook, then reapplies authenticated
    // initial state. If the ordering regresses, this block becomes silence.
    loaded.processor->prepare(make_prepare_ctx(1));
    restored = run_baked(*loaded.processor, kFrames, ones, 1);
    CHECK(restored[0][0] == 0.75f);

    const auto missing_type = pulp::host::load_baked(bytes, trust, {});
    CHECK_FALSE(missing_type.accepted);
    CHECK(missing_type.reason == LowerRejectReason::CustomNotYetLowerable);

    auto null_factory_type = type;
    null_factory_type.create = []() -> void* { return nullptr; };
    const auto null_factory =
        pulp::host::load_baked(bytes, trust, {null_factory_type});
    CHECK_FALSE(null_factory.accepted);
    CHECK(null_factory.reason ==
          LowerRejectReason::StatefulCustomNotYetLoadable);
    CHECK(null_factory.offending_node == custom);

    const auto duplicate_registry =
        pulp::host::load_baked(bytes, trust, {type, type});
    CHECK_FALSE(duplicate_registry.accepted);
    CHECK(duplicate_registry.reason == LowerRejectReason::CodecRejected);

    auto malformed_plan = *plan_result.plan;
    auto malformed_node = std::find_if(
        malformed_plan.nodes.begin(), malformed_plan.nodes.end(),
        [custom](const pulp::host::BakedPlan::Node& node) {
            return node.id == custom;
        });
    REQUIRE(malformed_node != malformed_plan.nodes.end());
    const float out_of_contract_gain = 2.0f;
    malformed_node->custom_state.resize(sizeof(out_of_contract_gain));
    std::memcpy(malformed_node->custom_state.data(), &out_of_contract_gain,
                sizeof(out_of_contract_gain));
    const auto malformed_bytes =
        pulp::host::write_baked_signed(malformed_plan, kp->private_key);
    REQUIRE_FALSE(malformed_bytes.empty());
    const auto malformed =
        pulp::host::load_baked(malformed_bytes, trust, {type});
    CHECK_FALSE(malformed.accepted);
    CHECK(malformed.reason ==
          LowerRejectReason::StatefulCustomNotYetLoadable);
    CHECK(malformed.offending_node == custom);

    auto two_node_plan = *plan_result.plan;
    const auto output_node = std::find_if(
        two_node_plan.nodes.begin(), two_node_plan.nodes.end(),
        [](const pulp::host::BakedPlan::Node& node) {
            return node.type == pulp::host::NodeType::AudioOutput;
        });
    REQUIRE(output_node != two_node_plan.nodes.end());
    const auto old_output_edge = std::find_if(
        two_node_plan.connections.begin(), two_node_plan.connections.end(),
        [custom, output_id = output_node->id](
            const pulp::host::BakedPlan::Conn& connection) {
            return connection.src_node == custom &&
                   connection.dst_node == output_id;
        });
    REQUIRE(old_output_edge != two_node_plan.connections.end());
    const auto output_id = output_node->id;
    two_node_plan.connections.erase(old_output_edge);
    const auto second_custom = output_id + 1;
    pulp::host::BakedPlan::Node second = *plan_node;
    second.id = second_custom;
    second.custom_state.resize(sizeof(out_of_contract_gain));
    std::memcpy(second.custom_state.data(), &out_of_contract_gain,
                sizeof(out_of_contract_gain));
    two_node_plan.nodes.push_back(std::move(second));
    two_node_plan.connections.push_back(
        {custom, 0, second_custom, 0, false});
    two_node_plan.connections.push_back(
        {second_custom, 0, output_id, 0, false});
    const auto two_node_bytes =
        pulp::host::write_baked_signed(two_node_plan, kp->private_key);
    REQUIRE_FALSE(two_node_bytes.empty());
    const auto later_malformed =
        pulp::host::load_baked(two_node_bytes, trust, {type});
    CHECK_FALSE(later_malformed.accepted);
    CHECK(later_malformed.reason ==
          LowerRejectReason::StatefulCustomNotYetLoadable);
    CHECK(later_malformed.offending_node == second_custom);
}

TEST_CASE("signed bake restores meaningful zero-byte Custom state",
          "[host][graph][bake][codec][state]") {
    struct EmptyState {
        float gain = 0.0f;
    };
    CustomNodeType type;
    type.type_id = "pulp.test.empty-bake-state";
    type.version = 1;
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.lowerable = true;
    type.create = []() -> void* { return new EmptyState(); };
    type.destroy = [](void* state) { delete static_cast<EmptyState*>(state); };
    type.reset = [](void* state) { static_cast<EmptyState*>(state)->gain = 0.0f; };
    type.load_state = [](void* state, std::span<const std::uint8_t> bytes) {
        if (!bytes.empty()) return false;
        static_cast<EmptyState*>(state)->gain = 0.4f;
        return true;
    };
    type.process_instance =
        [](void* state, pulp::audio::BufferView<float>& output,
           const pulp::audio::BufferView<const float>& input, int frames) {
            const float gain = static_cast<EmptyState*>(state)->gain;
            for (int frame = 0; frame < frames; ++frame)
                output.channel_ptr(0)[frame] =
                    input.channel_ptr(0)[frame] * gain;
        };

    pulp::host::BakedPlan plan;
    plan.input_channels = 1;
    plan.output_channels = 1;
    plan.nodes.push_back(
        {1, pulp::host::NodeType::AudioInput, 0, 1, 1.0f, {}, 0, {}});
    plan.nodes.push_back(
        {2, pulp::host::NodeType::Custom, 1, 1, 1.0f,
         type.type_id, type.version, {}});
    plan.nodes.push_back(
        {3, pulp::host::NodeType::AudioOutput, 1, 0, 1.0f, {}, 0, {}});
    plan.connections.push_back({1, 0, 2, 0, false});
    plan.connections.push_back({2, 0, 3, 0, false});

    std::array<std::uint8_t, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<std::uint8_t>(i + 41);
    const auto kp =
        pulp::runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp);
    const auto bytes =
        pulp::host::write_baked_signed(plan, kp->private_key);
    REQUIRE_FALSE(bytes.empty());
    pulp::host::BakedTrust trust;
    trust.trusted_public_keys.push_back(kp->public_key);

    auto loaded = pulp::host::load_baked(bytes, trust, {type});
    REQUIRE(loaded.accepted);
    REQUIRE(loaded.processor);
    loaded.processor->prepare(make_prepare_ctx(1));
    const std::vector<std::vector<float>> ones{
        std::vector<float>(static_cast<std::size_t>(kFrames), 1.0f)};
    const auto restored = run_baked(*loaded.processor, kFrames, ones, 1);
    CHECK(restored[0][0] == 0.4f);
}
