// SignalGraph latency semantics: artifact latency is compensated where paths
// reconverge, intentional delay never is, multi-output nodes and multiple
// audio outputs are equalised to one reported total, and a latency change
// takes effect only through a re-prepare. Every rendering case runs on both the
// legacy reference walk and the routed executor.

#include <pulp/host/signal_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace {

using pulp::host::CustomNodeType;
using pulp::host::NodeId;
using pulp::host::SignalGraph;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;

// A delay line of `delay` samples per channel. Used both as an intentional echo
// (declared intentional) and as a lookahead stage (declared artifact latency).
struct DelayLine {
    explicit DelayLine(int delay_samples, int channels)
        : delay(static_cast<std::size_t>(delay_samples)),
          rings(static_cast<std::size_t>(channels),
                std::vector<float>(std::max<std::size_t>(1, delay), 0.0f)),
          positions(static_cast<std::size_t>(channels), 0) {}
    std::size_t delay;
    std::vector<std::vector<float>> rings;
    std::vector<std::size_t> positions;

    void run(std::size_t channel, const float* in, float* out, int n) {
        if (delay == 0) {
            std::copy(in, in + n, out);
            return;
        }
        auto& ring = rings[channel];
        auto& pos = positions[channel];
        for (int i = 0; i < n; ++i) {
            const float x = in[i];
            out[i] = ring[pos];
            ring[pos] = x;
            pos = (pos + 1) % delay;
        }
    }
};

CustomNodeType delay_type(const char* id, int delay, bool intentional) {
    CustomNodeType t;
    t.type_id = id;
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = id;
    t.create = [delay]() -> void* { return new DelayLine(delay, 1); };
    t.destroy = [](void* p) { delete static_cast<DelayLine*>(p); };
    t.process_instance = [](void* p, pulp::audio::BufferView<float>& out,
                            const pulp::audio::BufferView<const float>& in, int n) {
        static_cast<DelayLine*>(p)->run(0, in.channel_ptr(0), out.channel_ptr(0), n);
    };
    if (intentional)
        t.intentional_delay_samples = [delay](double) { return delay; };
    else
        t.latency_samples = [delay](double) { return delay; };
    return t;
}

// One input, two outputs: port 0 is the input delayed by `port0_delay` (for
// example a linear-phase band), port 1 is the input unchanged.
CustomNodeType split_type(int port0_delay) {
    CustomNodeType t;
    t.type_id = "pulp.test.split";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 2;
    t.default_name = "Split";
    t.create = [port0_delay]() -> void* { return new DelayLine(port0_delay, 1); };
    t.destroy = [](void* p) { delete static_cast<DelayLine*>(p); };
    t.process_instance = [](void* p, pulp::audio::BufferView<float>& out,
                            const pulp::audio::BufferView<const float>& in, int n) {
        static_cast<DelayLine*>(p)->run(0, in.channel_ptr(0), out.channel_ptr(0), n);
        std::copy(in.channel_ptr(0), in.channel_ptr(0) + n, out.channel_ptr(1));
    };
    t.output_port_latency_samples = [port0_delay](double, int port) {
        return port == 0 ? port0_delay : 0;
    };
    return t;
}

// Renders `frames` samples of a unit impulse at sample 0 through a mono graph.
std::vector<float> render_impulse(SignalGraph& graph, int frames) {
    std::vector<float> result;
    std::vector<float> in(kBlock, 0.0f), out(kBlock, 0.0f);
    std::array<const float*, 1> in_ptrs{in.data()};
    std::array<float*, 1> out_ptrs{out.data()};
    for (int done = 0; done < frames; done += kBlock) {
        std::fill(in.begin(), in.end(), 0.0f);
        if (done == 0)
            in[0] = 1.0f;
        pulp::audio::BufferView<const float> iv(in_ptrs.data(), 1, kBlock);
        pulp::audio::BufferView<float> ov(out_ptrs.data(), 1, kBlock);
        graph.process(ov, iv, kBlock);
        result.insert(result.end(), out.begin(), out.end());
    }
    return result;
}

std::vector<std::size_t> nonzero_positions(const std::vector<float>& signal) {
    std::vector<std::size_t> positions;
    for (std::size_t i = 0; i < signal.size(); ++i)
        if (std::fabs(signal[i]) > 1e-6f)
            positions.push_back(i);
    return positions;
}

void configure(SignalGraph& graph, bool routed) {
    graph.set_canonical_executor_routing_enabled(routed);
    graph.set_parallel_routing_enabled(false);
    graph.set_anticipation_enabled(false);
}

std::int64_t max_connection_delay(const SignalGraph& graph) {
    const auto snapshot = graph.prepared_latency_snapshot();
    REQUIRE(snapshot.has_value());
    std::int64_t worst = 0;
    for (const auto& edge : snapshot->connections)
        worst = std::max(worst, edge.delay_samples);
    return worst;
}

} // namespace

TEST_CASE("Intentional 500 ms echo on a wet branch never delays the dry path",
          "[host][graph][pdc][latency-semantics]") {
    constexpr int kEcho = 24000; // 500 ms at 48 kHz
    for (const bool routed : {false, true}) {
        CAPTURE(routed);
        SignalGraph graph;
        REQUIRE(graph.register_custom_node_type(delay_type("pulp.test.echo", kEcho, true)));
        const auto in = graph.add_input_node(1, "In");
        const auto echo = graph.add_custom_node("pulp.test.echo", "Echo");
        const auto dry = graph.add_gain_node("Dry");
        const auto out = graph.add_output_node(1, "Out");
        REQUIRE(graph.connect(in, 0, echo, 0));
        REQUIRE(graph.connect(in, 0, dry, 0));
        REQUIRE(graph.connect(echo, 0, out, 0));
        REQUIRE(graph.connect(dry, 0, out, 0));
        configure(graph, routed);
        REQUIRE(graph.prepare(kSampleRate, kBlock));

        CHECK(graph.latency_samples() == 0);
        CHECK(max_connection_delay(graph) == 0);
        const auto snapshot = graph.prepared_latency_snapshot();
        const auto echo_latency = snapshot->node(echo);
        REQUIRE(echo_latency.has_value());
        CHECK(echo_latency->intentional_delay_samples == kEcho);
        CHECK(echo_latency->artifact_latency_samples == 0);
        CHECK(echo_latency->output_latency_samples == 0);

        // Dry impulse at 0, echo at exactly 500 ms: nothing was compensated.
        const auto rendered = render_impulse(graph, kEcho + kBlock);
        CHECK(nonzero_positions(rendered) == std::vector<std::size_t>{0, kEcho});
    }
}

TEST_CASE("Lookahead latency on one branch delays the parallel dry path",
          "[host][graph][pdc][latency-semantics]") {
    constexpr int kLookahead = 64;
    for (const bool routed : {false, true}) {
        CAPTURE(routed);
        SignalGraph graph;
        REQUIRE(
            graph.register_custom_node_type(delay_type("pulp.test.lookahead", kLookahead, false)));
        const auto in = graph.add_input_node(1, "In");
        const auto limiter = graph.add_custom_node("pulp.test.lookahead", "Limiter");
        const auto dry = graph.add_gain_node("Dry");
        const auto out = graph.add_output_node(1, "Out");
        REQUIRE(graph.connect(in, 0, limiter, 0));
        REQUIRE(graph.connect(in, 0, dry, 0));
        REQUIRE(graph.connect(limiter, 0, out, 0));
        REQUIRE(graph.connect(dry, 0, out, 0));
        REQUIRE(graph.set_node_gain(dry, 0.5f));
        configure(graph, routed);
        REQUIRE(graph.prepare(kSampleRate, kBlock));

        CHECK(graph.latency_samples() == kLookahead);
        const auto snapshot = graph.prepared_latency_snapshot();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->node(limiter)->artifact_latency_samples == kLookahead);
        pulp::host::Connection dry_edge{};
        dry_edge.source_node = dry;
        dry_edge.source_port = 0;
        dry_edge.dest_node = out;
        dry_edge.dest_port = 0;
        CHECK(snapshot->connection_delay(dry_edge) == kLookahead);

        const auto rendered = render_impulse(graph, 2 * kBlock);
        CHECK(nonzero_positions(rendered) == std::vector<std::size_t>{kLookahead});
        CHECK_THAT(rendered[kLookahead], Catch::Matchers::WithinAbs(1.5, 1e-6));
    }
}

TEST_CASE("A chain with no parallel branch adds no alignment delay",
          "[host][graph][pdc][latency-semantics]") {
    for (const bool routed : {false, true}) {
        CAPTURE(routed);
        SignalGraph graph;
        REQUIRE(graph.register_custom_node_type(delay_type("pulp.test.la32", 32, false)));
        REQUIRE(graph.register_custom_node_type(delay_type("pulp.test.la16", 16, false)));
        const auto in = graph.add_input_node(1, "In");
        const auto a = graph.add_custom_node("pulp.test.la32", "A");
        const auto b = graph.add_custom_node("pulp.test.la16", "B");
        const auto out = graph.add_output_node(1, "Out");
        REQUIRE(graph.connect(in, 0, a, 0));
        REQUIRE(graph.connect(a, 0, b, 0));
        REQUIRE(graph.connect(b, 0, out, 0));
        configure(graph, routed);
        REQUIRE(graph.prepare(kSampleRate, kBlock));

        // The latency is real and reported, but no edge carries compensation.
        CHECK(graph.latency_samples() == 48);
        CHECK(max_connection_delay(graph) == 0);
        CHECK(graph.prepared_stats().delay_buffer_bytes == 0);
        const auto rendered = render_impulse(graph, kBlock);
        CHECK(nonzero_positions(rendered) == std::vector<std::size_t>{48});
    }
}

TEST_CASE("A node's outputs with different artifact latency are equalised to one scalar",
          "[host][graph][pdc][latency-semantics]") {
    constexpr int kBand = 10;
    for (const bool routed : {false, true}) {
        CAPTURE(routed);
        SignalGraph graph;
        REQUIRE(graph.register_custom_node_type(split_type(kBand)));
        const auto in = graph.add_input_node(1, "In");
        const auto split = graph.add_custom_node("pulp.test.split", "Split");
        const auto dry = graph.add_gain_node("Dry");
        const auto out = graph.add_output_node(1, "Out");
        REQUIRE(graph.connect(in, 0, split, 0));
        REQUIRE(graph.connect(in, 0, dry, 0));
        REQUIRE(graph.connect(split, 0, out, 0));
        REQUIRE(graph.connect(split, 1, out, 0));
        REQUIRE(graph.connect(dry, 0, out, 0));
        configure(graph, routed);
        REQUIRE(graph.prepare(kSampleRate, kBlock));

        CHECK(graph.latency_samples() == kBand);
        const auto snapshot = graph.prepared_latency_snapshot();
        const auto node = snapshot->node(split);
        REQUIRE(node.has_value());
        CHECK(node->artifact_latency_samples == kBand);
        CHECK(node->output_port_latency_samples == std::vector<std::int64_t>{kBand, 0});

        // Both ports and the compensated dry path arrive together.
        const auto rendered = render_impulse(graph, 2 * kBlock);
        CHECK(nonzero_positions(rendered) == std::vector<std::size_t>{kBand});
        CHECK_THAT(rendered[kBand], Catch::Matchers::WithinAbs(3.0, 1e-6));
    }
}

TEST_CASE("Multiple audio outputs are aligned to the one reported latency",
          "[host][graph][pdc][latency-semantics]") {
    for (const bool routed : {false, true}) {
        CAPTURE(routed);
        SignalGraph graph;
        REQUIRE(graph.register_custom_node_type(delay_type("pulp.test.la8", 8, false)));
        const auto in = graph.add_input_node(1, "In");
        const auto la = graph.add_custom_node("pulp.test.la8", "Lookahead");
        const auto out_a = graph.add_output_node(1, "OutA");
        const auto out_b = graph.add_output_node(1, "OutB");
        REQUIRE(graph.connect(in, 0, la, 0));
        REQUIRE(graph.connect(la, 0, out_a, 0));
        REQUIRE(graph.connect(in, 0, out_b, 0));
        configure(graph, routed);
        REQUIRE(graph.prepare(kSampleRate, kBlock));

        CHECK(graph.latency_samples() == 8);
        const auto rendered = render_impulse(graph, kBlock);
        CHECK(nonzero_positions(rendered) == std::vector<std::size_t>{8});
        CHECK_THAT(rendered[8], Catch::Matchers::WithinAbs(2.0, 1e-6));
    }
}

TEST_CASE("An oversampling-factor latency change takes effect only through re-prepare",
          "[host][graph][pdc][latency-semantics]") {
    // The factor is a realization choice captured at prepare, the way an
    // oversampler's filter group delay is. Changing it must never alter the
    // reported latency of the running snapshot.
    auto factor = std::make_shared<int>(2);
    CustomNodeType t;
    t.type_id = "pulp.test.oversampled";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Oversampled";
    t.process = [](pulp::audio::BufferView<float>& out,
                   const pulp::audio::BufferView<const float>& in, int n) {
        std::copy(in.channel_ptr(0), in.channel_ptr(0) + n, out.channel_ptr(0));
    };
    t.latency_samples = [factor](double) { return 6 * *factor; };

    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(t));
    const auto in = graph.add_input_node(1, "In");
    const auto os = graph.add_custom_node("pulp.test.oversampled", "OS");
    const auto dry = graph.add_gain_node("Dry");
    const auto out = graph.add_output_node(1, "Out");
    REQUIRE(graph.connect(in, 0, os, 0));
    REQUIRE(graph.connect(in, 0, dry, 0));
    REQUIRE(graph.connect(os, 0, out, 0));
    REQUIRE(graph.connect(dry, 0, out, 0));
    REQUIRE(graph.prepare(kSampleRate, kBlock));
    REQUIRE(graph.latency_samples() == 12);

    *factor = 4;
    (void)render_impulse(graph, kBlock);
    CHECK(graph.latency_samples() == 12); // no silent change

    // A gap-free swap refuses a latency change and asks for an eager prepare.
    graph.begin_swap_edit();
    REQUIRE(graph.set_node_gain(dry, 0.5f));
    CHECK(graph.prepare_swap(kSampleRate, kBlock) == SignalGraph::SwapResult::NeedsEagerPrepare);

    REQUIRE(graph.prepare(kSampleRate, kBlock));
    CHECK(graph.latency_samples() == 24);
}

TEST_CASE("Latency declarations that the graph cannot honour are refused at registration",
          "[host][graph][pdc][latency-semantics]") {
    auto base = split_type(4);
    CHECK(base.is_valid_registration());

    auto with_scalar = base;
    with_scalar.latency_samples = [](double) { return 4; };
    CHECK_FALSE(with_scalar.is_valid_registration());

    auto with_block_scalar = base;
    with_block_scalar.latency_samples_for_block = [](double, int) { return 4; };
    CHECK_FALSE(with_block_scalar.is_valid_registration());

    auto lowerable = base;
    lowerable.lowerable = true;
    CHECK_FALSE(lowerable.is_valid_registration());

    // A latent node with only a transport-aware path has no stable fallback.
    CustomNodeType transport_only;
    transport_only.type_id = "pulp.test.transport-only";
    transport_only.num_input_ports = 1;
    transport_only.num_output_ports = 2;
    transport_only.process_transport = [](pulp::audio::BufferView<float>&,
                                          const pulp::audio::BufferView<const float>&, int,
                                          const pulp::format::ProcessContext&) {};
    transport_only.output_port_latency_samples = [](double, int) { return 4; };
    CHECK_FALSE(transport_only.is_valid_registration());

    // Intentional delay alone never makes a registration invalid.
    auto echo = delay_type("pulp.test.echo-only", 100, true);
    CHECK(echo.is_valid_registration());
    SignalGraph graph;
    CHECK_FALSE(graph.register_custom_node_type(with_scalar));
    CHECK(graph.register_custom_node_type(echo));
}

TEST_CASE("A gap-free swap keeps the port equaliser history of an unchanged node",
          "[host][graph][pdc][latency-semantics]") {
    constexpr int kBand = 300; // longer than one block, so history spans a swap
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(split_type(kBand)));
    const auto in = graph.add_input_node(1, "In");
    const auto split = graph.add_custom_node("pulp.test.split", "Split");
    const auto gain = graph.add_gain_node("Trim");
    const auto out = graph.add_output_node(1, "Out");
    REQUIRE(graph.connect(in, 0, split, 0));
    REQUIRE(graph.connect(split, 1, gain, 0));
    REQUIRE(graph.connect(gain, 0, out, 0));
    REQUIRE(graph.prepare(kSampleRate, kBlock));
    REQUIRE(graph.latency_samples() == kBand);

    std::vector<float> in_block(kBlock, 0.0f), out_block(kBlock, 0.0f);
    std::array<const float*, 1> in_ptrs{in_block.data()};
    std::array<float*, 1> out_ptrs{out_block.data()};
    pulp::audio::BufferView<const float> iv(in_ptrs.data(), 1, kBlock);
    pulp::audio::BufferView<float> ov(out_ptrs.data(), 1, kBlock);
    in_block[0] = 1.0f;
    graph.process(ov, iv, kBlock);
    CHECK(nonzero_positions(out_block).empty());

    // A gain change is reinit-free; the equalised port must keep its pending
    // impulse rather than restart from silence.
    graph.begin_swap_edit();
    REQUIRE(graph.set_node_gain(gain, 0.5f));
    REQUIRE(graph.prepare_swap(kSampleRate, kBlock) == SignalGraph::SwapResult::Swapped);

    in_block[0] = 0.0f;
    graph.process(ov, iv, kBlock);
    const auto hits = nonzero_positions(out_block);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0] == static_cast<std::size_t>(kBand - kBlock));
}
