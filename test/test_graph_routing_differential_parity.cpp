// Differential parity safety-net for the inter-node routing backend.
//
// The fixed-topology cases in test_graph_executor_routing.cpp prove specific
// shapes (chain, diamond, split-lifetime, feedback). This file fuzzes the
// routing path: it generates many random *audio-only* DAGs of mono gain nodes
// and drives the SAME topology through both pulp::host::SignalGraph (the
// established oracle) and the canonical GraphRuntimeExecutor routing path,
// asserting their outputs agree. A routing bug (wrong slot, missed gather,
// wrong fan-in summation, broken scratch reuse, mis-handled feedback) surfaces
// as a divergence on some seed even when the hand-written shapes pass.
//
// Scope is exactly the semantics both engines represent today: audio edges,
// fan-in summing, buffer clearing, per-node gain, scratch reuse, and one-block
// feedback. Latency/PDC, sidechain, MIDI, automation, stereo/multi-port routing,
// and hosted plugin/custom nodes are intentionally out of scope — those join
// this harness as the routed plan gains the schema to express them. This is a
// routing-path safety-net, not a claim of full graph-runtime parity.
//
// Equivalence is asserted BIT-EXACTLY. Both engines materialize the same edge
// list, preserve connection order into their inbound-edge lists, and sum
// linearly in that order, so there is no associativity freedom for the two to
// diverge on — any difference is a real routing/gain bug, not float noise. A
// relative-tolerance class is only warranted once an edge kind whose summation
// order genuinely differs (e.g. a parallel schedule) is added.
//
// The generator is itself guarded: the suite asserts post-hoc coverage counters
// so it cannot silently degenerate into trivial graphs that pass vacuously.

#include "harness/graph_routing_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace {

using pulp::format::GraphRuntimeExecutor;
using pulp::format::GraphRuntimeNodeBinding;
using pulp::format::GraphRuntimeSnapshot;
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

// Edge in the shared topology description: source node id -> dest node id, all
// on mono port 0. The same edge list builds both engines so the topology is
// identical by construction.
struct Edge {
    std::uint32_t src;
    std::uint32_t dst;
};

// One randomly generated mono audio-only DAG: input(id=1) -> K gain nodes
// (ids 2..K+1) -> output(id=K+2). Edges only ever go from a lower id to a
// higher id, so the graph is acyclic by construction.
struct RandomGraph {
    std::uint32_t gain_count = 0;
    std::uint32_t input_id = 1;
    std::uint32_t output_id = 0;          // input_id + gain_count + 1
    std::vector<float> gains;             // one per gain node, index 0 -> id 2
    std::vector<Edge> edges;              // in generation order; shared by both

    std::uint32_t gain_id(std::uint32_t idx) const { return 2 + idx; }
    bool is_gain_id(std::uint32_t id) const { return id >= 2 && id < output_id; }
};

RandomGraph generate(std::mt19937& rng) {
    RandomGraph g;
    g.gain_count = std::uniform_int_distribution<std::uint32_t>(2, 6)(rng);
    g.output_id = g.input_id + g.gain_count + 1;

    // Mix in exact 0.0 (mutes a branch) and strictly-above-1.0 (amplifies) gains
    // so those boundary behaviors are always exercised, not just approached.
    std::uniform_int_distribution<int> bucket(0, 5);
    std::uniform_real_distribution<float> any_gain(0.0f, 2.0f);
    std::uniform_real_distribution<float> hot_gain(1.0f, 2.0f);
    for (std::uint32_t i = 0; i < g.gain_count; ++i) {
        const int b = bucket(rng);
        g.gains.push_back(b == 0 ? 0.0f : (b == 1 ? hot_gain(rng) : any_gain(rng)));
    }

    // Inbound edges for one node: pick a random non-empty subset of the input
    // plus every strictly-earlier gain node (keeps the graph acyclic).
    const auto pick_into = [&](std::uint32_t up_to_gain_idx, std::uint32_t dst_id) {
        std::vector<std::uint32_t> candidates{g.input_id};
        for (std::uint32_t j = 0; j < up_to_gain_idx; ++j) candidates.push_back(g.gain_id(j));
        const std::uint32_t max_m =
            std::min<std::uint32_t>(3, static_cast<std::uint32_t>(candidates.size()));
        const std::uint32_t m = std::uniform_int_distribution<std::uint32_t>(1, max_m)(rng);
        std::shuffle(candidates.begin(), candidates.end(), rng);
        candidates.resize(m);
        for (auto src : candidates) g.edges.push_back({src, dst_id});
    };

    for (std::uint32_t i = 0; i < g.gain_count; ++i) pick_into(i, g.gain_id(i));
    pick_into(g.gain_count, g.output_id);  // output draws from input + all gains
    return g;
}

// Forward (from input) or backward (from output) reachability over the edges.
std::vector<char> reachable(const RandomGraph& rg, std::uint32_t start, bool forward) {
    std::vector<char> seen(static_cast<std::size_t>(rg.output_id) + 1, 0);
    std::vector<std::uint32_t> stack{start};
    seen[start] = 1;
    while (!stack.empty()) {
        const std::uint32_t n = stack.back();
        stack.pop_back();
        for (const auto& e : rg.edges) {
            const std::uint32_t a = forward ? e.src : e.dst;
            const std::uint32_t b = forward ? e.dst : e.src;
            if (a == n && !seen[b]) {
                seen[b] = 1;
                stack.push_back(b);
            }
        }
    }
    return seen;
}

// Gain-node indices that lie on some input -> ... -> output path. Only these
// affect output, so feedback placement and "active gain" coverage use them.
std::vector<std::uint32_t> live_gain_indices(const RandomGraph& rg) {
    const auto fwd = reachable(rg, rg.input_id, /*forward=*/true);
    const auto bwd = reachable(rg, rg.output_id, /*forward=*/false);
    std::vector<std::uint32_t> live;
    for (std::uint32_t i = 0; i < rg.gain_count; ++i) {
        const std::uint32_t id = rg.gain_id(i);
        if (fwd[id] && bwd[id]) live.push_back(i);
    }
    return live;
}

// Materialize the random graph as SignalGraph node ids, in stable order.
struct SignalGraphNodes {
    pulp::host::NodeId in = 0;
    pulp::host::NodeId out = 0;
    std::vector<pulp::host::NodeId> gains;
    pulp::host::NodeId resolve(const RandomGraph& rg, std::uint32_t id) const {
        if (id == rg.input_id) return in;
        if (id == rg.output_id) return out;
        return gains[id - 2];
    }
};

void build_signal_graph(pulp::host::SignalGraph& g, const RandomGraph& rg,
                        bool feedback_on_gain, std::uint32_t feedback_idx) {
    SignalGraphNodes n;
    n.in = g.add_input_node(1, "In");
    for (std::uint32_t i = 0; i < rg.gain_count; ++i) n.gains.push_back(g.add_gain_node("G"));
    n.out = g.add_output_node(1, "Out");
    for (const auto& e : rg.edges)
        REQUIRE(g.connect(n.resolve(rg, e.src), 0, n.resolve(rg, e.dst), 0));
    if (feedback_on_gain)
        REQUIRE(g.connect_feedback(n.gains[feedback_idx], 0, n.gains[feedback_idx], 0));
    for (std::uint32_t i = 0; i < rg.gain_count; ++i)
        REQUIRE(g.set_node_gain(n.gains[i], rg.gains[i]));
}

std::vector<float> signal_graph_block(pulp::host::SignalGraph& g, int frames,
                                      const std::vector<float>& x) {
    // This is the WALK oracle: force the legacy walk so the comparison stays
    // walk-vs-executor regardless of the routing default (canonical is ON;
    // parallel/anticipation take precedence in process() when enabled).
    g.set_canonical_executor_routing_enabled(false);
    g.set_parallel_routing_enabled(false);
    g.set_anticipation_enabled(false);
    std::vector<float> xi = x;
    std::vector<float> yo(static_cast<std::size_t>(frames), 0.0f);
    std::array<const float*, 1> in_ch{xi.data()};
    std::array<float*, 1> out_ch{yo.data()};
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 1,
                                                 static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> out_view(out_ch.data(), 1,
                                            static_cast<std::uint32_t>(frames));
    g.process(out_view, in_view, frames);
    return yo;
}

// Runtime node/connection specs + gain bindings for the same graph. `states` is
// caller-owned so the GainState storage outlives the snapshot (bindings hold
// pointers into it).
struct RuntimeSpecs {
    std::vector<GraphRuntimeNodeSpec> nodes;
    std::vector<GraphRuntimeConnectionSpec> conns;
    std::vector<GraphRuntimeNodeBinding> bindings;
};

RuntimeSpecs build_runtime_specs(const RandomGraph& rg, std::vector<GainState>& states,
                                 bool feedback_on_gain, std::uint32_t feedback_idx) {
    RuntimeSpecs s;
    s.nodes.push_back({rg.input_id, GraphRuntimeNodeKind::AudioInput, 0, 1});
    for (std::uint32_t i = 0; i < rg.gain_count; ++i)
        s.nodes.push_back({rg.gain_id(i), GraphRuntimeNodeKind::Processor, 1, 1});
    s.nodes.push_back({rg.output_id, GraphRuntimeNodeKind::AudioOutput, 1, 0});

    for (const auto& e : rg.edges) s.conns.push_back({e.src, 0, e.dst, 0});
    if (feedback_on_gain) {
        const std::uint32_t id = rg.gain_id(feedback_idx);
        s.conns.push_back({id, 0, id, 0, /*feedback=*/true, /*event=*/false});
    }

    states.assign(rg.gain_count, GainState{});
    for (std::uint32_t i = 0; i < rg.gain_count; ++i) states[i].gain = rg.gains[i];
    s.bindings.push_back({rg.input_id, nullptr, nullptr, false});
    for (std::uint32_t i = 0; i < rg.gain_count; ++i)
        s.bindings.push_back({rg.gain_id(i), routing_gain, &states[i], true});
    s.bindings.push_back({rg.output_id, nullptr, nullptr, false});
    return s;
}

bool exact_equal(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;  // bit-exact: identical edge/sum order
    return true;
}

// Per-graph structural facts used to assert the fuzzer keeps real pressure.
struct Coverage {
    int multi_hop = 0;     // a gain feeds another gain (>1 processing hop)
    int wide_fan_in = 0;   // some node sums >2 inbound edges
    int fan_out = 0;       // some node feeds >=2 destinations (split lifetime)
    int active_zero = 0;   // a live gain is exactly 0.0 (mutes a real branch)
    int active_hot = 0;    // a live gain is strictly >1.0 (amplifies a branch)
};

void tally(Coverage& cov, const RandomGraph& rg) {
    bool multi_hop = false, wide = false, fan_out = false;
    std::vector<int> in_deg(static_cast<std::size_t>(rg.output_id) + 1, 0);
    std::vector<int> out_deg(static_cast<std::size_t>(rg.output_id) + 1, 0);
    for (const auto& e : rg.edges) {
        ++in_deg[e.dst];
        ++out_deg[e.src];
        if (rg.is_gain_id(e.src) && rg.is_gain_id(e.dst)) multi_hop = true;
    }
    for (std::uint32_t id = 1; id <= rg.output_id; ++id) {
        if (in_deg[id] > 2) wide = true;
        if (out_deg[id] >= 2) fan_out = true;
    }
    cov.multi_hop += multi_hop ? 1 : 0;
    cov.wide_fan_in += wide ? 1 : 0;
    cov.fan_out += fan_out ? 1 : 0;
    for (auto i : live_gain_indices(rg)) {
        if (rg.gains[i] == 0.0f) { cov.active_zero += 1; break; }
    }
    for (auto i : live_gain_indices(rg)) {
        if (rg.gains[i] > 1.0f) { cov.active_hot += 1; break; }
    }
}

} // namespace

TEST_CASE("Routed executor matches SignalGraph across random audio-only DAGs",
          "[host][graph][executor][routing][parity][differential]") {
    constexpr int kGraphs = 200;
    Coverage cov;
    for (int seed = 0; seed < kGraphs; ++seed) {
        std::mt19937 rng(static_cast<std::uint32_t>(seed) + 1u);
        const RandomGraph rg = generate(rng);
        tally(cov, rg);
        for (int frames : {1, 37, 64, 256}) {
            CAPTURE(seed, frames, rg.gain_count, rg.edges.size());
            const auto x = test_signal(frames, 0.8f);

            pulp::host::SignalGraph g;
            build_signal_graph(g, rg, /*feedback_on_gain=*/false, 0);
            REQUIRE(g.prepare(kSr, frames));
            const auto ref = signal_graph_block(g, frames, x);

            std::vector<GainState> states;
            const auto specs = build_runtime_specs(rg, states, /*feedback_on_gain=*/false, 0);
            GraphRuntimeSnapshot snapshot;
            REQUIRE(make_snapshot(snapshot, specs.nodes, specs.conns, specs.bindings));
            auto pool = make_pool(snapshot, frames);
            GraphRuntimeExecutor exec;
            RoutedHarness h(kSr, frames, {x}, 1);
            REQUIRE(h.run(exec, snapshot, pool).ok());

            REQUIRE(exact_equal(ref, h.outs[0]));
        }
    }
    // Guard against a silently-degenerate generator: these floors are well below
    // the observed rates but high enough that trivial-graph regressions trip them.
    INFO("coverage: multi_hop=" << cov.multi_hop << " wide_fan_in=" << cov.wide_fan_in
         << " fan_out=" << cov.fan_out << " active_zero=" << cov.active_zero
         << " active_hot=" << cov.active_hot);
    CHECK(cov.multi_hop > 100);
    CHECK(cov.wide_fan_in > 50);
    CHECK(cov.fan_out > 80);
    CHECK(cov.active_zero > 10);
    CHECK(cov.active_hot > 80);
}

TEST_CASE("Routed executor matches SignalGraph for random graphs with one feedback edge",
          "[host][graph][executor][routing][parity][differential][feedback]") {
    // Add a single self-feedback edge on a random gain node that actually lies on
    // an input->output path, and drive the recurrence across blocks comparing
    // block-for-block. Placing feedback on a live node is what makes the
    // recurrence observable at the output; a dead node would pass vacuously.
    constexpr int kFrames = 64;
    int tested = 0;
    for (int seed = 0; seed < 120; ++seed) {
        std::mt19937 rng(static_cast<std::uint32_t>(seed) + 5000u);
        const RandomGraph rg = generate(rng);
        const auto live = live_gain_indices(rg);
        if (live.empty()) continue;  // no observable feedback site for this seed
        const std::uint32_t fb_idx =
            live[std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng)];
        CAPTURE(seed, rg.gain_count, fb_idx);
        ++tested;

        pulp::host::SignalGraph g;
        build_signal_graph(g, rg, /*feedback_on_gain=*/true, fb_idx);
        REQUIRE(g.prepare(kSr, kFrames));

        std::vector<GainState> states;
        const auto specs = build_runtime_specs(rg, states, /*feedback_on_gain=*/true, fb_idx);
        GraphRuntimeSnapshot snapshot;
        REQUIRE(make_snapshot(snapshot, specs.nodes, specs.conns, specs.bindings));
        auto pool = make_pool(snapshot, kFrames);  // persists across blocks
        GraphRuntimeExecutor exec;

        for (int blk = 0; blk < 5; ++blk) {
            CAPTURE(blk);
            const auto x = test_signal(kFrames, 0.4f + 0.1f * static_cast<float>(blk));
            const auto ref = signal_graph_block(g, kFrames, x);
            RoutedHarness h(kSr, kFrames, {x}, 1);
            REQUIRE(h.run(exec, snapshot, pool).ok());
            REQUIRE(exact_equal(ref, h.outs[0]));
        }
    }
    INFO("feedback graphs exercised: " << tested);
    CHECK(tested > 60);  // most seeds must have a live feedback site
}
