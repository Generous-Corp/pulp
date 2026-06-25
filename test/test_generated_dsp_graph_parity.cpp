// Parity proof for WS-E: "one path for generated DSP".
//
// A generated/native DSP core reaches a graph through the SAME ProcessorNode
// path as any hand-written pulp::format::Processor — there is no separate
// generated-DSP runtime. The core here implements the language-neutral C ABI
// (pulp/native_components/native_core.h) inline, exactly as a Rust / C / Zig /
// FAUST·Cmajor-generated core would, and is owned by a NativeCoreProcessor.
//
// The payoff this file proves: that NativeCoreProcessor produces bit-identical
// output whether it is driven standalone (HeadlessHost, the oracle) or wrapped
// in a ProcessorNode inside the routed GraphRuntimeExecutor (the implementation).
// A divergence means the generated-DSP core does NOT share the Processor graph
// path faithfully.
//
// The oracle deliberately uses HeadlessHost, NOT process_processor_block, so the
// comparison is not circular: ProcessorNode is built on process_processor_block,
// while HeadlessHost reaches the processor through the independent process()
// path that WS-D exercised.
//
// Scope mirrors the ProcessorNode adapter: mono audio in -> out, no params/MIDI.

#include "harness/graph_routing_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/format/native_core_processor.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/native_components/native_core.h>
#include <pulp/state/store.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace pulp;

namespace {

using pulp::format::GraphRuntimeExecutor;
using pulp::format::GraphRuntimeNodeBinding;
using pulp::format::GraphRuntimeSnapshot;
using pulp::format::HeadlessHost;
using pulp::format::NativeCoreProcessor;
using pulp::format::PrepareContext;
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

// ── A minimal C-ABI native core: a deterministic, stateful one-pole low-pass
// over each channel. The only state is the per-channel filter memory `z`, which
// makes the output depend on input history (so a faithful adapter must preserve
// ordering and starting state) but NOT on transport context or parameters. The
// shape of a generated FAUST/Cmajor/Rust core: a POD instance behind the opaque
// handle, a process() that touches only the borrowed host buffers. ────────────

constexpr std::size_t kMaxChannels = 8;

struct OnePoleInstance {
    float z[kMaxChannels] = {};
    bool active = false;
};

const char kId[] = "com.pulp.test.generated.onepole";
const char kName[] = "Generated One-Pole";

pulp_native_descriptor_v1 g_desc = [] {
    pulp_native_descriptor_v1 d{};
    d.size = sizeof(d);
    d.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    d.id = kId;
    d.id_len = std::strlen(kId);
    d.name = kName;
    d.name_len = std::strlen(kName);
    d.plugin_version = 1;
    d.capabilities = 0;  // parameter-free, no MIDI/state needed for parity
    d.default_input_bus_count = 1;
    d.default_output_bus_count = 1;
    return d;
}();

const pulp_native_descriptor_v1* core_descriptor() { return &g_desc; }

// No parameters: the core is deterministic from input + zeroed state alone.
const pulp_native_param_v1* core_parameters(uint32_t* count) {
    if (count) *count = 0;
    return nullptr;
}

pulp_native_status core_create(const pulp_native_host_services_v1*,
                               pulp_native_instance** out) {
    *out = reinterpret_cast<pulp_native_instance*>(new OnePoleInstance());
    return PULP_NATIVE_OK;
}
void core_destroy(pulp_native_instance* i) {
    delete reinterpret_cast<OnePoleInstance*>(i);
}
pulp_native_status core_prepare(pulp_native_instance* i,
                                const pulp_native_prepare_v1*) {
    auto* self = reinterpret_cast<OnePoleInstance*>(i);
    for (float& z : self->z) z = 0.0f;  // re-prepare starts from a zeroed state
    return PULP_NATIVE_OK;
}
void core_release(pulp_native_instance*) {}
pulp_native_status core_set_bus_layout(pulp_native_instance*,
                                       const pulp_native_bus_layout_v1*) {
    return PULP_NATIVE_OK;
}
pulp_native_status core_resume(pulp_native_instance* i) {
    reinterpret_cast<OnePoleInstance*>(i)->active = true;
    return PULP_NATIVE_OK;
}
pulp_native_status core_suspend(pulp_native_instance* i) {
    reinterpret_cast<OnePoleInstance*>(i)->active = false;
    return PULP_NATIVE_OK;
}
void core_reset(pulp_native_instance* i) {
    auto* self = reinterpret_cast<OnePoleInstance*>(i);
    for (float& z : self->z) z = 0.0f;
}

pulp_native_status core_process(pulp_native_instance* inst,
                                const pulp_native_process_v1* io) {
    auto* self = reinterpret_cast<OnePoleInstance*>(inst);
    const auto* audio = io->audio;
    if (audio->output_bus_count == 0 || audio->input_bus_count == 0) {
        return PULP_NATIVE_OK;
    }
    const auto& obus = audio->outputs[0];
    const auto& ibus = audio->inputs[0];
    const uint32_t chs =
        obus.channel_count < ibus.channel_count ? obus.channel_count
                                                 : ibus.channel_count;
    for (uint32_t c = 0; c < chs && c < kMaxChannels; ++c) {
        float* out = obus.channels[c];
        const float* in = ibus.channels[c];
        float z = self->z[c];
        for (uint32_t s = 0; s < audio->frame_count; ++s) {
            z = kPole * in[s] + (1.0f - kPole) * z;
            out[s] = z;
        }
        self->z[c] = z;
    }
    return PULP_NATIVE_OK;
}

// State / latency / tail: not exercised by the parity path, but a real vtable
// always populates them. save/load are trivial no-ops (no caps advertised).
pulp_native_status core_save_state(pulp_native_instance*,
                                   pulp_native_state_out_v1* out) {
    out->bytes = nullptr;
    out->byte_len = 0;
    return PULP_NATIVE_OK;
}
void core_free_state(pulp_native_instance*, pulp_native_state_out_v1*) {}
pulp_native_status core_load_state(pulp_native_instance*,
                                   const pulp_native_state_span_v1*) {
    return PULP_NATIVE_OK;
}
uint32_t core_report_latency(pulp_native_instance*) { return 0; }
uint32_t core_report_tail(pulp_native_instance*) { return 0; }

pulp_native_core_v1 g_core = [] {
    pulp_native_core_v1 c{};
    c.size = sizeof(c);
    c.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    c.descriptor = core_descriptor;
    c.parameters = core_parameters;
    c.create = core_create;
    c.destroy = core_destroy;
    c.prepare = core_prepare;
    c.release = core_release;
    c.set_bus_layout = core_set_bus_layout;
    c.resume = core_resume;
    c.suspend = core_suspend;
    c.reset = core_reset;
    c.process = core_process;
    c.save_state = core_save_state;
    c.free_state = core_free_state;
    c.load_state = core_load_state;
    c.report_latency = core_report_latency;
    c.report_tail = core_report_tail;
    c.editor_command = nullptr;       // no editor-command capability
    c.free_editor_reply = nullptr;
    return c;
}();

// Factory for HeadlessHost: builds a NativeCoreProcessor over the generated
// core. The host owns the processor and binds its StateStore, so this is the
// SAME adapter both drive paths reach.
std::unique_ptr<Processor> make_generated_core_processor() {
    return std::make_unique<NativeCoreProcessor>(&g_core);
}

// Standalone oracle: one mono block of `x` through a freshly prepared
// HeadlessHost wrapping the generated core.
std::vector<float> oracle_block(const std::vector<float>& x, int frames) {
    HeadlessHost host(&make_generated_core_processor);
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

}  // namespace

TEST_CASE(
    "Generated DSP core reaches a graph through the same ProcessorNode path",
    "[format][graph][native-core][processor-node][parity][generated-dsp]") {
    GraphRuntimeExecutor exec;
    for (int frames : {1, 64, 256}) {
        for (float seed : {0.8f, 0.35f}) {
            CAPTURE(frames, seed);
            const auto x = test_signal(frames, seed);

            // Implementation path: the generated core, wrapped as a graph node.
            NativeCoreProcessor processor(&g_core);
            REQUIRE(processor.valid());
            state::StateStore store;
            processor.set_state_store(&store);
            processor.define_parameters(store);
            ProcessorNode node(processor);
            PrepareContext prepare_ctx;
            prepare_ctx.sample_rate = kSr;
            prepare_ctx.max_buffer_size = frames;
            prepare_ctx.input_channels = 1;
            prepare_ctx.output_channels = 1;
            REQUIRE(node.prepare(prepare_ctx));

            const std::array bindings = {
                GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
                GraphRuntimeNodeBinding{2, ProcessorNode::process_binding, &node,
                                        true},
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
                // Both paths reach the identical NativeCoreProcessor over the
                // same input from a zeroed state, so this is bit-exact.
                REQUIRE(ref[i] == h.outs[0][i]);
            }
        }
    }
}
