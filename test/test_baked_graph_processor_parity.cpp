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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using pulp::host::BakedGraphProcessor;
using pulp::host::LowerRejectReason;
using pulp::host::LowerResult;
using pulp::host::SignalGraph;
using pulp::host::bake;

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
