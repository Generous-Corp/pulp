// Render-path proof for the anticipation sub-graph (Phase 6): the sub-graph
// carved from an eligible latent partition, when driven through the ordinary
// executor (build_executor_snapshot + process_routed), must produce the SAME
// boundary signals the full graph produces at those points — each on its own
// output channel. This is the guard the structural extraction tests cannot give:
// it would catch a regression of the channel-collision bug (every captured port
// summed onto channel 0) that structural assertions alone missed.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/host/anticipation_eligibility.hpp>
#include <pulp/host/anticipation_partition.hpp>
#include <pulp/host/anticipation_subgraph.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>

#include <atomic>
#include <vector>

using namespace pulp::host;
using pulp::host::PluginSlot;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 64;

PluginInfo gen_info(int out_ch) {
    PluginInfo info{};
    info.name = "AnticipationGenerator";
    info.format = PluginFormat::CLAP;
    info.num_inputs = 0;
    info.num_outputs = out_ch;
    info.category = "Generator";
    return info;
}

// A deterministic source: ignores input, writes a per-channel signal distinct
// across channels (channel c, sample k -> (c+1) + k*0.01), so a bug that summed
// or swapped channels shows up immediately.
class GeneratorPlugin final : public PluginSlot {
public:
    explicit GeneratorPlugin(int out_ch) : info_(gen_info(out_ch)) {}
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
            for (int k = 0; k < n; ++k) {
                o[static_cast<std::size_t>(k)] =
                    static_cast<float>(c + 1) + static_cast<float>(k) * 0.01f;
            }
        }
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

// Render `nodes`/`connections` through the executor, resolving the generator slot
// and the gain atomic by id, into `out_channels` of output. Returns per-channel
// output. REQUIRE-fails if the snapshot/pool can't be built.
std::vector<std::vector<float>> render(std::span<const GraphNode> nodes,
                                       std::span<const Connection> conns,
                                       NodeId gen_id, GeneratorPlugin* gen,
                                       NodeId gain_id, std::atomic<float>* gain,
                                       int out_channels) {
    pulp::format::GraphRuntimeSnapshot snapshot;
    std::vector<PluginBindingContext> plugin_ctx;
    PluginRoutingScratch scratch;
    REQUIRE(build_executor_snapshot(
        nodes, conns, [&](NodeId id) { return id == gain_id ? gain : nullptr; },
        [&](NodeId id) { return id == gen_id ? static_cast<PluginSlot*>(gen) : nullptr; },
        plugin_ctx, scratch, snapshot));

    pulp::format::GraphRuntimeBufferPool pool;
    REQUIRE(pool.reset(snapshot.buffer_slot_count(), kFrames,
                       snapshot.buffer_assignment().connection_delay_samples));

    std::vector<std::vector<float>> in_ch(2, std::vector<float>(kFrames, 0.0f));
    std::vector<std::vector<float>> out_ch(
        static_cast<std::size_t>(out_channels), std::vector<float>(kFrames, 0.0f));
    std::vector<const float*> in_ptrs;
    std::vector<float*> out_ptrs;
    for (auto& c : in_ch) in_ptrs.push_back(c.data());
    for (auto& c : out_ch) out_ptrs.push_back(c.data());
    pulp::audio::BufferView<const float> iv(in_ptrs.data(), in_ptrs.size(), kFrames);
    pulp::audio::BufferView<float> ov(out_ptrs.data(), out_ptrs.size(), kFrames);

    pulp::format::BusBufferSet buses;
    REQUIRE(buses.add_input("main", iv, pulp::format::BusRole::Main));
    REQUIRE(buses.add_output("main", ov, pulp::format::BusRole::Main));
    pulp::format::ProcessBlock block;
    block.sample_rate = kSr;
    block.frame_count = kFrames;
    block.buses = &buses;
    REQUIRE(block.validate());

    pulp::format::GraphRuntimeExecutor exec;
    REQUIRE(exec.process_routed(block, snapshot, pool).ok());
    return out_ch;
}

}  // namespace

TEST_CASE("Anticipation sub-graph renders each boundary on its own channel",
          "[host][anticipation][subgraph][render]") {
    // gen(0/2) -> gain(2/2) -> out(2/0). The interior {gen, gain} renders ahead;
    // its boundary is gain's two output ports. The sub-graph render must put each
    // captured port on its OWN channel and match the full graph's output there.
    auto gen = std::make_shared<GeneratorPlugin>(2);
    std::atomic<float> gain_value{0.5f};

    GraphNode gen_node;
    gen_node.id = 1;
    gen_node.type = NodeType::Plugin;
    gen_node.num_input_ports = 0;
    gen_node.num_output_ports = 2;
    gen_node.plugin = gen;  // node_eligible requires a live slot on the node
    GraphNode gain_node;
    gain_node.id = 2;
    gain_node.type = NodeType::Gain;
    gain_node.num_input_ports = 2;
    gain_node.num_output_ports = 2;
    gain_node.gain = 0.5f;
    GraphNode out_node;
    out_node.id = 3;
    out_node.type = NodeType::AudioOutput;
    out_node.num_input_ports = 2;
    out_node.num_output_ports = 0;
    std::vector<GraphNode> nodes{gen_node, gain_node, out_node};

    auto edge = [](NodeId s, PortIndex sp, NodeId d, PortIndex dp) {
        Connection c;
        c.source_node = s;
        c.source_port = sp;
        c.dest_node = d;
        c.dest_port = dp;
        return c;
    };
    std::vector<Connection> conns{edge(1, 0, 2, 0), edge(1, 1, 2, 1),
                                  edge(2, 0, 3, 0), edge(2, 1, 3, 1)};

    // Full-graph render (the oracle): out channel c = gen_c * 0.5.
    const auto full = render(nodes, conns, 1, gen.get(), 2, &gain_value, 2);

    // Extract + render the sub-graph.
    const auto elig = analyze_anticipation_eligibility(nodes, conns);
    const auto part = build_anticipation_partition(nodes, conns, elig);
    const auto sub = build_anticipation_subgraph(nodes, conns, part);
    REQUIRE(sub.ok);
    REQUIRE(sub.outputs.size() == 2);
    const auto subout = render(sub.nodes, sub.connections, 1, gen.get(), 2, &gain_value, 2);

    // Each boundary lands on its own channel and equals the full-graph signal.
    for (int k = 0; k < kFrames; ++k) {
        const auto i = static_cast<std::size_t>(k);
        CHECK(subout[0][i] == full[0][i]);
        CHECK(subout[1][i] == full[1][i]);
    }
    // M1 guard: the two channels are genuinely distinct (gen ch0 != ch1), so a
    // regression that summed both onto channel 0 (silencing channel 1) would fail.
    const float expect0 = (1.0f + 0.0f) * 0.5f;  // (c=0 -> 1) * gain at k=0
    const float expect1 = (2.0f + 0.0f) * 0.5f;  // (c=1 -> 2) * gain at k=0
    CHECK(subout[0][0] == expect0);
    CHECK(subout[1][0] == expect1);
    CHECK(subout[0][0] != subout[1][0]);
}
