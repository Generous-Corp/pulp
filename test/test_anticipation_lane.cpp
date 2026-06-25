// Coverage for the AnticipationLane: render an eligible sub-graph ahead of the
// deadline into a ring and consume the pre-rendered blocks on the audio thread.
// The lane's consumed sequence must be bit-identical to rendering the same
// sub-graph synchronously block by block (a stateful generator proves block order
// and state evolution are preserved), consume must underrun cleanly and allocate
// nothing, and a background producer racing the consumer must be race-free.

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/host/anticipation_eligibility.hpp>
#include <pulp/host/anticipation_lane.hpp>
#include <pulp/host/anticipation_partition.hpp>
#include <pulp/host/anticipation_subgraph.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace pulp::host;
using pulp::host::PluginSlot;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 64;

PluginInfo gen_info(int out_ch) {
    PluginInfo info{};
    info.name = "CountingGenerator";
    info.format = PluginFormat::CLAP;
    info.num_inputs = 0;
    info.num_outputs = out_ch;
    info.category = "Generator";
    return info;
}

// Stateful source: block n, channel c -> (c+1)*10 + n (constant within the
// block). The per-block counter makes a dropped/duplicated/reordered block show
// up immediately as a divergence.
class CountingGenerator final : public PluginSlot {
public:
    explicit CountingGenerator(int out_ch) : info_(gen_info(out_ch)) {}
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
            const float v = static_cast<float>((c + 1) * 10) + static_cast<float>(counter_);
            for (int k = 0; k < n; ++k) o[static_cast<std::size_t>(k)] = v;
        }
        ++counter_;
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
    int counter_ = 0;
};

// gen(0/2) -> gain(2/2) -> out(2/0), stereo. Returns the nodes/connections.
struct Graph {
    std::vector<GraphNode> nodes;
    std::vector<Connection> conns;
    std::shared_ptr<CountingGenerator> gen;
};

Graph make_graph() {
    Graph g;
    g.gen = std::make_shared<CountingGenerator>(2);
    GraphNode gen_node;
    gen_node.id = 1;
    gen_node.type = NodeType::Plugin;
    gen_node.num_input_ports = 0;
    gen_node.num_output_ports = 2;
    gen_node.plugin = g.gen;
    GraphNode gain_node;
    gain_node.id = 2;
    gain_node.type = NodeType::Gain;
    gain_node.num_input_ports = 2;
    gain_node.num_output_ports = 2;
    GraphNode out_node;
    out_node.id = 3;
    out_node.type = NodeType::AudioOutput;
    out_node.num_input_ports = 2;
    out_node.num_output_ports = 0;
    g.nodes = {gen_node, gain_node, out_node};
    auto edge = [](NodeId s, PortIndex sp, NodeId d, PortIndex dp) {
        Connection c;
        c.source_node = s;
        c.source_port = sp;
        c.dest_node = d;
        c.dest_port = dp;
        return c;
    };
    g.conns = {edge(1, 0, 2, 0), edge(1, 1, 2, 1), edge(2, 0, 3, 0), edge(2, 1, 3, 1)};
    return g;
}

AnticipationSubgraph subgraph_of(const Graph& g) {
    const auto elig = analyze_anticipation_eligibility(g.nodes, g.conns);
    const auto part = build_anticipation_partition(g.nodes, g.conns, elig);
    return build_anticipation_subgraph(g.nodes, g.conns, part);
}

auto gain_for(std::atomic<float>* gain, NodeId gain_id) {
    return [gain, gain_id](NodeId id) { return id == gain_id ? gain : nullptr; };
}
auto plugin_for(CountingGenerator* gen, NodeId gen_id) {
    return [gen, gen_id](NodeId id) {
        return id == gen_id ? static_cast<PluginSlot*>(gen) : nullptr;
    };
}

// Render the sub-graph synchronously for `blocks` blocks, returning each channel's
// concatenated output (state carries across blocks).
std::vector<std::vector<float>> render_sync(const AnticipationSubgraph& sub,
                                            CountingGenerator* gen,
                                            std::atomic<float>* gain, int blocks) {
    pulp::format::GraphRuntimeSnapshot snapshot;
    std::vector<PluginBindingContext> ctx;
    PluginRoutingScratch scratch;
    REQUIRE(build_executor_snapshot(sub.nodes, sub.connections, gain_for(gain, 2),
                                    plugin_for(gen, 1), ctx, scratch, snapshot));
    pulp::format::GraphRuntimeBufferPool pool;
    REQUIRE(pool.reset(snapshot.buffer_slot_count(), kFrames,
                       snapshot.buffer_assignment().connection_delay_samples));
    const std::size_t chs = sub.outputs.size();
    std::vector<std::vector<float>> seq(chs);
    for (int b = 0; b < blocks; ++b) {
        std::vector<std::vector<float>> in_ch(2, std::vector<float>(kFrames, 0.0f));
        std::vector<std::vector<float>> out_ch(chs, std::vector<float>(kFrames, 0.0f));
        std::vector<const float*> inp;
        std::vector<float*> outp;
        for (auto& c : in_ch) inp.push_back(c.data());
        for (auto& c : out_ch) outp.push_back(c.data());
        pulp::audio::BufferView<const float> iv(inp.data(), inp.size(), kFrames);
        pulp::audio::BufferView<float> ov(outp.data(), outp.size(), kFrames);
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
        for (std::size_t c = 0; c < chs; ++c)
            seq[c].insert(seq[c].end(), out_ch[c].begin(), out_ch[c].end());
    }
    return seq;
}

}  // namespace

TEST_CASE("AnticipationLane consumed output matches a synchronous render",
          "[host][anticipation][lane]") {
    constexpr int kBlocks = 12;

    // Oracle: synchronous sub-graph render (its own generator instance).
    auto og = make_graph();
    const auto osub = subgraph_of(og);
    REQUIRE(osub.outputs.size() == 2);
    std::atomic<float> ogain{0.5f};
    const auto expected = render_sync(osub, og.gen.get(), &ogain, kBlocks);

    // Lane: render ahead + consume, interleaved like a real producer/consumer.
    auto lg = make_graph();
    const auto lsub = subgraph_of(lg);
    std::atomic<float> lgain{0.5f};
    AnticipationLane lane;
    REQUIRE(lane.prepare(lsub, gain_for(&lgain, 2), plugin_for(lg.gen.get(), 1), kSr,
                         kFrames, /*lead_blocks=*/4));
    REQUIRE(lane.output_channels() == 2);

    std::vector<std::vector<float>> got(2);
    std::vector<float> ch0(kFrames), ch1(kFrames);
    std::array<float*, 2> outp{ch0.data(), ch1.data()};
    pulp::audio::BufferView<float> out(outp.data(), 2, kFrames);
    for (int b = 0; b < kBlocks; ++b) {
        lane.render_ahead(/*max_blocks=*/8);  // top the ring back up
        REQUIRE(lane.consume(out));            // never underruns: lead >= 1
        got[0].insert(got[0].end(), ch0.begin(), ch0.end());
        got[1].insert(got[1].end(), ch1.begin(), ch1.end());
    }

    for (std::size_t c = 0; c < 2; ++c) {
        REQUIRE(got[c].size() == expected[c].size());
        for (std::size_t i = 0; i < got[c].size(); ++i) REQUIRE(got[c][i] == expected[c][i]);
    }
    // The two channels are genuinely distinct (gen ch0 != ch1), so the per-channel
    // ring routing is exercised, not a degenerate all-same comparison.
    CHECK(got[0][0] != got[1][0]);
}

TEST_CASE("AnticipationLane consume underruns cleanly on an empty ring",
          "[host][anticipation][lane]") {
    auto g = make_graph();
    const auto sub = subgraph_of(g);
    std::atomic<float> gain{0.5f};
    AnticipationLane lane;
    REQUIRE(lane.prepare(sub, gain_for(&gain, 2), plugin_for(g.gen.get(), 1), kSr,
                         kFrames, 2));
    std::vector<float> ch0(kFrames), ch1(kFrames);
    std::array<float*, 2> outp{ch0.data(), ch1.data()};
    pulp::audio::BufferView<float> out(outp.data(), 2, kFrames);
    CHECK_FALSE(lane.consume(out));  // nothing rendered yet -> underrun
    REQUIRE(lane.render_ahead(1) == 1);
    CHECK(lane.consume(out));        // now a block is available
    CHECK_FALSE(lane.consume(out));  // drained again
}

TEST_CASE("AnticipationLane consume does not allocate on the audio thread",
          "[host][anticipation][lane][rt-safety]") {
    auto g = make_graph();
    const auto sub = subgraph_of(g);
    std::atomic<float> gain{0.5f};
    AnticipationLane lane;
    REQUIRE(lane.prepare(sub, gain_for(&gain, 2), plugin_for(g.gen.get(), 1), kSr,
                         kFrames, 4));
    lane.render_ahead(4);
    std::vector<float> ch0(kFrames), ch1(kFrames);
    std::array<float*, 2> outp{ch0.data(), ch1.data()};
    pulp::audio::BufferView<float> out(outp.data(), 2, kFrames);
    lane.consume(out);  // warm up outside the probe
    {
        pulp::test::RtAllocationProbe probe;
        lane.consume(out);
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

TEST_CASE("AnticipationLane producer and consumer run race-free",
          "[host][anticipation][lane][threads][rt-safety]") {
    // render_ahead on a background thread, consume on the audio thread — the
    // ring's SPSC contract must hold. Surfaces a producer/consumer race under TSan.
    auto g = make_graph();
    const auto sub = subgraph_of(g);
    std::atomic<float> gain{0.5f};
    AnticipationLane lane;
    REQUIRE(lane.prepare(sub, gain_for(&gain, 2), plugin_for(g.gen.get(), 1), kSr,
                         kFrames, 8));

    // Prime the ring on this thread (still single-producer: the producer thread
    // is not spawned yet) so the consumer is guaranteed pre-rendered blocks
    // regardless of how the producer thread is scheduled; the race coverage comes
    // from the concurrent refill + drain below, not from the hit count.
    REQUIRE(lane.render_ahead(8) > 0);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> hits{0};
    std::thread producer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            lane.render_ahead(4);
            std::this_thread::yield();
        }
    });
    std::vector<float> ch0(kFrames), ch1(kFrames);
    std::array<float*, 2> outp{ch0.data(), ch1.data()};
    pulp::audio::BufferView<float> out(outp.data(), 2, kFrames);
    for (int i = 0; i < 4000; ++i) {
        if (lane.consume(out)) hits.fetch_add(1, std::memory_order_relaxed);
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    CHECK(hits.load() > 0);  // the consumer saw pre-rendered blocks
}
