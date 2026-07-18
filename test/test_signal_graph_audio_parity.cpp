// Signal-level parity for SignalGraph's multi-path audio routing.
//
// WHY AN ARITHMETIC ORACLE, NOT A RECORDING: every expectation here is derived
// from the stimulus by hand, using the same float operations in the same order
// the graph performs them. Nothing in this file is captured from a previous
// build, so no implementation change can move the bar with it. A dropped path, a
// swapped path, a reordered serial stage, a crossed channel, or a per-path piece
// of state left behind by a routing rebuild all move the render away from that
// oracle.
//
// WHY BIT-EXACT: the topology is built from scalar multiply / add / negate /
// square over the same operands in the same order — no smoothing ramp, no
// reassociation, no accumulation across blocks. Under IEEE-754 that is
// reproducible to the last bit, so a tolerance would only hide a real change.
// The one fan-in point sums exactly two contributions, and float addition of two
// operands is commutative bit-for-bit, so the summation order at that port is
// not a source of drift either.
//
// WHY THE PATHS ARE DISTINCT: each path carries a different gain and a different
// (non-commutative) transfer function, so no two paths can be interchanged
// without changing the result. The "expectation has teeth" case below proves
// that claim instead of assuming it.
//
// WHY THE FALLBACK COUNTER IS CHECKED: SignalGraph's legacy walk is BOTH the
// reference path and the routed path's fallback, so a routed-vs-walk comparison
// passes vacuously if routing silently stopped engaging. routed_walk_fallbacks()
// and routing_executor_stats() are asserted so a routed case that quietly
// degraded into the walk fails instead of going green.

#include "support/audio_signal_generators.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/analysis/audio_metrics.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using pulp::host::CustomNodeType;
using pulp::host::NodeId;
using pulp::host::SignalGraph;

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;

// Distinct per-path gains: no two are equal, so interchanging any two paths
// changes the render. Chosen as exact binary fractions so the oracle's multiply
// carries no rounding the graph does not also carry.
constexpr float kGainA = 0.5f;
constexpr float kGainB = 0.25f;
constexpr float kGainC = 0.75f;
constexpr float kBias = 0.25f;

// Per-path transfer functions. Deliberately non-commutative with the gain stage:
// bias-then-gain differs from gain-then-bias, and square-after-gain differs from
// square-before-gain, so a reordered serial chain cannot land on the oracle.
float bias_fn(float x) { return x + kBias; }
float square_fn(float x) { return x * x; }
float invert_fn(float x) { return -x; }

CustomNodeType make_type(std::string type_id, float (*fn)(float)) {
    CustomNodeType t;
    t.type_id = std::move(type_id);
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = t.type_id;
    t.process = [fn](pulp::audio::BufferView<float>& out,
                     const pulp::audio::BufferView<const float>& in,
                     int n) {
        if (out.num_channels() == 0) return;
        float* o = out.channel_ptr(0);
        if (in.num_channels() == 0) {
            for (int i = 0; i < n; ++i) o[static_cast<std::size_t>(i)] = fn(0.0f);
            return;
        }
        const float* ip = in.channel_ptr(0);
        for (int i = 0; i < n; ++i) {
            o[static_cast<std::size_t>(i)] = fn(ip[static_cast<std::size_t>(i)]);
        }
    };
    return t;
}

// The topology under test. Three paths that share one 2-channel input node and
// one 2-channel output node:
//
//   path A (serial chain, order-sensitive):
//       in:0 -> bias -> gainA -> square -> out:0
//   path B (parallel branch, fans into the SAME output port as A):
//       in:0 -> invert -> gainB -> out:0
//   path C (the other channel, must stay independent of ch0):
//       in:1 -> gainC -> out:1
//
// out:0 therefore exercises fan-out from one source port, a serial chain whose
// stage order matters, and fan-in of two differently-shaped paths. out:1
// exercises channel independence and a cross-port connection (a Gain node's
// output port 0 into the output node's port 1).
struct Topology {
    NodeId in{};
    NodeId out{};
    NodeId gain_a{};
    NodeId gain_b{};
    NodeId gain_c{};
};

Topology build(SignalGraph& g) {
    REQUIRE(g.register_custom_node_type(make_type("bias", bias_fn)));
    REQUIRE(g.register_custom_node_type(make_type("square", square_fn)));
    REQUIRE(g.register_custom_node_type(make_type("invert", invert_fn)));

    Topology t;
    t.in = g.add_input_node(2, "In");
    t.out = g.add_output_node(2, "Out");

    const NodeId bias = g.add_custom_node("bias");
    const NodeId square = g.add_custom_node("square");
    const NodeId invert = g.add_custom_node("invert");
    t.gain_a = g.add_gain_node("GainA");
    t.gain_b = g.add_gain_node("GainB");
    t.gain_c = g.add_gain_node("GainC");

    // Path A.
    REQUIRE(g.connect(t.in, 0, bias, 0));
    REQUIRE(g.connect(bias, 0, t.gain_a, 0));
    REQUIRE(g.connect(t.gain_a, 0, square, 0));
    REQUIRE(g.connect(square, 0, t.out, 0));
    // Path B.
    REQUIRE(g.connect(t.in, 0, invert, 0));
    REQUIRE(g.connect(invert, 0, t.gain_b, 0));
    REQUIRE(g.connect(t.gain_b, 0, t.out, 0));
    // Path C.
    REQUIRE(g.connect(t.in, 1, t.gain_c, 0));
    REQUIRE(g.connect(t.gain_c, 0, t.out, 1));

    REQUIRE(g.set_node_gain(t.gain_a, kGainA));
    REQUIRE(g.set_node_gain(t.gain_b, kGainB));
    REQUIRE(g.set_node_gain(t.gain_c, kGainC));
    return t;
}

// The oracle. Same operations, same order, same operands as the graph — derived
// from the topology on paper, never from a render.
std::vector<std::vector<float>> expected(const std::vector<std::vector<float>>& in,
                                         float gain_a, float gain_b, float gain_c) {
    const std::size_t n = in[0].size();
    std::vector<std::vector<float>> e(2, std::vector<float>(n, 0.0f));
    for (std::size_t i = 0; i < n; ++i) {
        const float x0 = in[0][i];
        const float path_a = square_fn(bias_fn(x0) * gain_a);
        const float path_b = invert_fn(x0) * gain_b;
        e[0][i] = path_a + path_b;
        e[1][i] = in[1][i] * gain_c;
    }
    return e;
}

std::vector<std::vector<float>> stimulus(std::uint64_t seed, int frames) {
    // Seeded generator from the shared signals layer — no clock, no
    // random_device. Amplitude 0.5 keeps `square` well inside range so the
    // oracle never depends on saturation behavior.
    auto buf = pulp::test::audio::make_white_noise(2, frames, seed, 0.5f);
    std::vector<std::vector<float>> in(2);
    for (std::size_t c = 0; c < 2; ++c) {
        auto ch = buf.channel(c);
        in[c].assign(ch.begin(), ch.end());
    }
    return in;
}

// Render `frames` of `in` through `g` in blocks of `block`, appending into the
// returned per-channel vectors. Every node in the topology is memoryless, so a
// partitioned render must equal a single-block render exactly.
std::vector<std::vector<float>> render(SignalGraph& g,
                                       const std::vector<std::vector<float>>& in,
                                       int block) {
    const int frames = static_cast<int>(in[0].size());
    std::vector<std::vector<float>> outs(2, std::vector<float>(in[0].size(), 0.0f));
    for (int start = 0; start < frames; start += block) {
        const int n = std::min(block, frames - start);
        std::vector<std::vector<float>> ins(2, std::vector<float>(static_cast<std::size_t>(n)));
        for (std::size_t c = 0; c < 2; ++c) {
            for (int i = 0; i < n; ++i) {
                ins[c][static_cast<std::size_t>(i)] =
                    in[c][static_cast<std::size_t>(start + i)];
            }
        }
        std::vector<std::vector<float>> blk(2, std::vector<float>(static_cast<std::size_t>(n), 0.0f));
        std::vector<const float*> in_ptrs{ins[0].data(), ins[1].data()};
        std::vector<float*> out_ptrs{blk[0].data(), blk[1].data()};
        pulp::audio::BufferView<const float> in_view(in_ptrs.data(), in_ptrs.size(),
                                                     static_cast<std::uint32_t>(n));
        pulp::audio::BufferView<float> out_view(out_ptrs.data(), out_ptrs.size(),
                                                static_cast<std::uint32_t>(n));
        g.process(out_view, in_view, n);
        for (std::size_t c = 0; c < 2; ++c) {
            for (int i = 0; i < n; ++i) {
                outs[c][static_cast<std::size_t>(start + i)] =
                    blk[c][static_cast<std::size_t>(i)];
            }
        }
    }
    return outs;
}

// Bit-exact comparison that names the first offending sample rather than just
// failing the whole vector.
void require_bit_identical(const std::vector<std::vector<float>>& got,
                           const std::vector<std::vector<float>>& want,
                           const char* what) {
    REQUIRE(got.size() == want.size());
    for (std::size_t c = 0; c < want.size(); ++c) {
        REQUIRE(got[c].size() == want[c].size());
        for (std::size_t i = 0; i < want[c].size(); ++i) {
            if (got[c][i] != want[c][i]) {
                INFO(what << ": channel " << c << " sample " << i << " got "
                          << got[c][i] << " want " << want[c][i]);
                REQUIRE(got[c][i] == want[c][i]);
            }
        }
    }
}

bool differs(const std::vector<std::vector<float>>& a,
             const std::vector<std::vector<float>>& b) {
    for (std::size_t c = 0; c < a.size(); ++c) {
        for (std::size_t i = 0; i < a[c].size(); ++i) {
            if (a[c][i] != b[c][i]) return true;
        }
    }
    return false;
}

// Guards every bit-exact comparison below: two silent renders are trivially
// bit-identical, so a graph that stopped producing audio must fail loudly rather
// than pass by agreeing with itself.
void require_audible(const std::vector<std::vector<float>>& out) {
    std::vector<const float*> ptrs{out[0].data(), out[1].data()};
    pulp::audio::BufferView<const float> view(ptrs.data(), ptrs.size(),
                                              static_cast<std::uint32_t>(out[0].size()));
    const auto metrics = pulp::test::audio::analyze(view, kSr);
    const auto finite = pulp::test::audio::assert_no_nan_inf(metrics);
    INFO(finite.message);
    REQUIRE(finite.passed);
    const auto loud = pulp::test::audio::assert_not_silent(metrics);
    INFO(loud.message);
    REQUIRE(loud.passed);
}

enum class Mode { Walk, RoutedSerial, RoutedParallel };

void select_mode(SignalGraph& g, Mode mode) {
    // Set every routing opt-in explicitly: the walk oracle must stay the walk
    // regardless of any default, and each routed mode must not inherit another's.
    g.set_anticipation_enabled(false);
    g.set_canonical_executor_routing_enabled(mode != Mode::Walk);
    g.set_parallel_routing_enabled(mode == Mode::RoutedParallel);
    if (mode == Mode::RoutedParallel) {
        // Run every eligible level across the pool rather than letting the
        // break-even heuristic decide — this test is about routing correctness,
        // and a threshold-driven serial fallback would silently retest the
        // serial path.
        g.set_parallel_min_work_units(0);
    }
}

const char* mode_name(Mode m) {
    switch (m) {
    case Mode::Walk: return "legacy walk";
    case Mode::RoutedSerial: return "routed serial";
    case Mode::RoutedParallel: return "routed parallel";
    }
    return "?";
}

} // namespace

TEST_CASE("SignalGraph multi-path routing renders the arithmetic oracle on every path",
          "[signal-graph][parity]") {
    const auto in = stimulus(0xA11CE, kFrames);
    const auto want = expected(in, kGainA, kGainB, kGainC);

    for (const Mode mode : {Mode::Walk, Mode::RoutedSerial, Mode::RoutedParallel}) {
        SignalGraph g;
        select_mode(g, mode);
        build(g);
        REQUIRE(g.prepare(kSr, kFrames));

        const auto got = render(g, in, kFrames);
        INFO("mode: " << mode_name(mode));
        require_audible(got);
        require_bit_identical(got, want, mode_name(mode));

        if (mode != Mode::Walk) {
            // The walk is the routed path's fallback, so an eligible graph that
            // stopped routing would still match the oracle. Prove it routed.
            REQUIRE(g.routed_walk_fallbacks() == 0);
            REQUIRE(g.routing_executor_stats().blocks_processed > 0);
        }
        if (mode == Mode::RoutedParallel) {
            // Prove the PARALLEL path actually carried a level, not just the
            // serial executor under a parallel opt-in.
            REQUIRE(g.routing_executor_stats().parallel_levels_dispatched > 0);
        }
    }
}

TEST_CASE("SignalGraph multi-path oracle detects an interchanged path",
          "[signal-graph][parity]") {
    // Teeth check: the case above is only meaningful if the topology's paths are
    // genuinely distinguishable. Render the same graph with path A's and path B's
    // gains interchanged; the result must move off the original oracle. If this
    // ever passes trivially, the topology has lost its ability to catch a
    // mis-routed path and the case above is worth nothing.
    const auto in = stimulus(0xB0B, kFrames);

    SignalGraph baseline;
    select_mode(baseline, Mode::RoutedSerial);
    build(baseline);
    REQUIRE(baseline.prepare(kSr, kFrames));
    const auto base_out = render(baseline, in, kFrames);
    require_audible(base_out);

    SignalGraph swapped;
    select_mode(swapped, Mode::RoutedSerial);
    const auto t = build(swapped);
    REQUIRE(swapped.set_node_gain(t.gain_a, kGainB));
    REQUIRE(swapped.set_node_gain(t.gain_b, kGainA));
    REQUIRE(swapped.prepare(kSr, kFrames));
    const auto swapped_out = render(swapped, in, kFrames);

    REQUIRE(differs(base_out, swapped_out));
    require_bit_identical(swapped_out, expected(in, kGainB, kGainA, kGainC),
                          "interchanged gains");
}

TEST_CASE("SignalGraph multi-path routing keeps the channels independent",
          "[signal-graph][parity]") {
    // A grouped-state or slot-assignment mistake shows up as one channel's
    // stimulus leaking into the other's output. Change ONLY channel 1 and require
    // channel 0's render to be bit-identical, and vice versa.
    auto in_a = stimulus(0xC0FFEE, kFrames);
    auto in_b = in_a;
    const auto other = stimulus(0xD15EA5E, kFrames);
    in_b[1] = other[1];

    SignalGraph g;
    select_mode(g, Mode::RoutedSerial);
    build(g);
    REQUIRE(g.prepare(kSr, kFrames));

    const auto out_a = render(g, in_a, kFrames);
    const auto out_b = render(g, in_b, kFrames);
    require_audible(out_a);
    require_audible(out_b);

    REQUIRE(out_a[0] == out_b[0]);  // ch1 stimulus must not reach ch0
    REQUIRE(out_a[1] != out_b[1]);  // ...and must still reach ch1

    auto in_c = in_a;
    in_c[0] = other[0];
    const auto out_c = render(g, in_c, kFrames);
    REQUIRE(out_a[1] == out_c[1]);  // ch0 stimulus must not reach ch1
    REQUIRE(out_a[0] != out_c[0]);
}

TEST_CASE("SignalGraph multi-path routing is block-partition invariant",
          "[signal-graph][parity]") {
    // Every node here is memoryless, so partitioning the same stimulus must not
    // change a sample. This catches a scratch-pool slot that carries state across
    // blocks it should not, and a routed path that reads a stale block boundary.
    const auto in = stimulus(0xFEED, kFrames);
    const auto want = expected(in, kGainA, kGainB, kGainC);

    for (const int block : {1, 7, 32, kFrames}) {
        SignalGraph g;
        select_mode(g, Mode::RoutedSerial);
        build(g);
        REQUIRE(g.prepare(kSr, kFrames));
        const auto got = render(g, in, block);
        INFO("block size: " << block);
        require_audible(got);
        require_bit_identical(got, want, "partitioned render");
        REQUIRE(g.routed_walk_fallbacks() == 0);
    }
}

TEST_CASE("SignalGraph renders unchanged across an open and aborted swap edit",
          "[signal-graph][parity]") {
    // With no replacement staged, an open swap edit must leave the live snapshot
    // playing untouched, and aborting must return the graph to the same render
    // after a re-prepare. The oracle is the same arithmetic as every case above.
    const auto in = stimulus(0x5EED, kFrames);
    const auto want = expected(in, kGainA, kGainB, kGainC);

    SignalGraph g;
    select_mode(g, Mode::RoutedSerial);
    build(g);
    REQUIRE(g.prepare(kSr, kFrames));

    const auto before = render(g, in, kFrames);
    require_audible(before);
    require_bit_identical(before, want, "before swap edit");

    g.begin_swap_edit();
    const auto during = render(g, in, kFrames);
    require_bit_identical(during, want, "during swap edit");

    g.abort_swap_edit();
    REQUIRE(g.prepare(kSr, kFrames));
    const auto after = render(g, in, kFrames);
    require_bit_identical(after, want, "after aborted swap edit");
    REQUIRE(g.routed_walk_fallbacks() == 0);
}
