// Acceptance coverage for anticipative rendering wired into SignalGraph::process:
// with anticipation enabled, the eligible latent interior is
// pre-rendered ahead of the deadline off the audio thread and spliced into the
// live graph, so the audio block never runs the interior itself. The result must
// be bit-identical to the canonical (interior-live) executor render, and a
// counting generator proves the interior is advanced exactly once per block (by
// the producer pump), never twice (which a double-render would cause).

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace pulp::host;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 64;

PluginInfo gen_info() {
    PluginInfo info{};
    info.name = "AnticipationCountingGen";
    info.format = PluginFormat::CLAP;
    info.num_inputs = 0;
    info.num_outputs = 2;
    info.category = "Generator";
    return info;
}

// Stateful source: block n, channel c -> (c+1)*10 + n. Counts process() calls so a
// test can prove how many times the interior was actually rendered.
class CountingGen final : public PluginSlot {
public:
    explicit CountingGen(int latency = 0) : info_(gen_info()), latency_(latency) {}
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
        calls.fetch_add(1, std::memory_order_relaxed);
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    int latency_samples() const override { return latency_; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

    std::atomic<int> calls{0};

private:
    PluginInfo info_;
    int latency_ = 0;
    int block_ = 0;
};

// gen(0/2) -> gain(2/2) -> out(2/0). Interior = {gen, gain}; out is the live sink.
// Returns the raw generator pointer (owned by the graph) for call-count checks.
CountingGen* build(SignalGraph& g) {
    auto gen = std::make_unique<CountingGen>();
    CountingGen* raw = gen.get();
    const auto gen_id = g.add_plugin_node(std::move(gen), 0, 2, "Gen");
    const auto gain = g.add_gain_node("G");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(gen_id, c, gain, c));
        REQUIRE(g.connect(gain, c, out, c));
    }
    REQUIRE(g.set_node_gain(gain, 0.5f));
    return raw;
}

std::vector<std::vector<float>> render_block(SignalGraph& g) {
    std::vector<float> l(kFrames, 0.0f), r(kFrames, 0.0f);
    std::array<float*, 2> oc{l.data(), r.data()};
    std::vector<float> zi(kFrames, 0.0f);
    std::array<const float*, 2> ic{zi.data(), zi.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);
    g.process(ov, iv, kFrames);
    return {std::move(l), std::move(r)};
}

}  // namespace

TEST_CASE("SignalGraph anticipation matches the canonical interior-live render",
          "[host][anticipation][signal-graph]") {
    constexpr int kBlocks = 12;

    // Oracle: canonical executor, interior rendered live each block.
    SignalGraph oracle;
    CountingGen* ogen = build(oracle);
    oracle.set_canonical_executor_routing_enabled(true);
    REQUIRE(oracle.prepare(kSr, kFrames));
    std::vector<std::vector<float>> expected(2);
    for (int b = 0; b < kBlocks; ++b) {
        const auto blk = render_block(oracle);
        for (int c = 0; c < 2; ++c)
            expected[c].insert(expected[c].end(), blk[c].begin(), blk[c].end());
    }
    CHECK(ogen->calls.load() == kBlocks);  // interior ran once per block, live

    // Anticipated: same graph, interior pre-rendered ahead and spliced.
    SignalGraph antic;
    CountingGen* agen = build(antic);
    antic.set_canonical_executor_routing_enabled(true);
    antic.set_anticipation_enabled(true);
    REQUIRE(antic.prepare(kSr, kFrames));
    std::vector<std::vector<float>> got(2);
    for (int b = 0; b < kBlocks; ++b) {
        antic.pump_anticipation(8);  // producer: render the interior ahead
        const auto blk = render_block(antic);
        for (int c = 0; c < 2; ++c)
            got[c].insert(got[c].end(), blk[c].begin(), blk[c].end());
    }

    // Bit-identical to the live render — which also proves the interior advanced
    // exactly once per block: a double-render (pump + live) would desync the
    // counting generator and diverge.
    for (int c = 0; c < 2; ++c) {
        REQUIRE(got[c].size() == expected[c].size());
        for (std::size_t i = 0; i < got[c].size(); ++i)
            REQUIRE(got[c][i] == expected[c][i]);
    }
    // The interior generator was advanced only by the producer pump, which renders
    // AHEAD — so it ran at least once per consumed block and at most a lead's worth
    // beyond. (That process() never ran it is already proven by the bit-exact match
    // above: a second, live render would desync the counting generator.)
    CHECK(agen->calls.load() >= kBlocks);
    CHECK(agen->calls.load() <= kBlocks + 4);  // + the lead the ring holds ahead
    // Distinct per channel: real per-channel splice, not an all-same comparison.
    CHECK(got[0][0] != got[1][0]);
}

TEST_CASE("SignalGraph anticipation producer pump and audio process are race-free",
          "[host][anticipation][signal-graph][threads][rt-safety]") {
    // pump_anticipation on a background thread (the producer) while process() runs
    // on the audio thread (the consumer) — the lane's SPSC ring + the snapshot
    // reader pin must hold. Surfaces a race under TSan.
    SignalGraph g;
    build(g);
    g.set_canonical_executor_routing_enabled(true);
    g.set_anticipation_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));

    std::atomic<bool> stop{false};
    std::thread producer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            g.pump_anticipation(4);
            std::this_thread::yield();
        }
    });
    std::uint64_t blocks = 0;
    for (int i = 0; i < 3000; ++i) {
        (void)render_block(g);
        ++blocks;
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    CHECK(blocks > 0);
}

TEST_CASE("SignalGraph anticipation off leaves the canonical render unchanged",
          "[host][anticipation][signal-graph]") {
    // Sanity: with anticipation disabled, process() takes the ordinary canonical
    // path and pump_anticipation is a no-op.
    SignalGraph g;
    CountingGen* gen = build(g);
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));
    CHECK(g.pump_anticipation(8) == 0);  // disabled -> no-op
    const auto blk = render_block(g);
    CHECK(gen->calls.load() == 1);  // interior ran live
    CHECK(blk[0][0] == (10.0f + 0.0f) * 0.5f);
    CHECK(blk[1][0] == (20.0f + 0.0f) * 0.5f);
}

namespace {

// Drive `g` for `blocks` blocks, pumping the producer first when `anticipated`.
// Returns each channel's concatenated output.
std::vector<std::vector<float>> run_session(SignalGraph& g, int blocks,
                                            bool anticipated, int out_channels) {
    std::vector<std::vector<float>> seq(static_cast<std::size_t>(out_channels));
    for (int b = 0; b < blocks; ++b) {
        if (anticipated) g.pump_anticipation(8);
        std::vector<std::vector<float>> oc(static_cast<std::size_t>(out_channels),
                                           std::vector<float>(kFrames, 0.0f));
        std::vector<float> zi(kFrames, 0.0f);
        std::vector<float*> op;
        std::vector<const float*> ip(2, zi.data());
        for (auto& c : oc) op.push_back(c.data());
        pulp::audio::BufferView<const float> iv(ip.data(), 2, kFrames);
        pulp::audio::BufferView<float> ov(op.data(), op.size(), kFrames);
        g.process(ov, iv, kFrames);
        for (int c = 0; c < out_channels; ++c)
            seq[static_cast<std::size_t>(c)].insert(
                seq[static_cast<std::size_t>(c)].end(), oc[static_cast<std::size_t>(c)].begin(),
                oc[static_cast<std::size_t>(c)].end());
    }
    return seq;
}

// Build `wire` into both a canonical graph and an anticipated graph, render
// `blocks`, and assert bit-identical output. `wire` returns nothing; it adds nodes
// + connections + gains to the passed graph (fresh plugin instances each call).
template <typename Wire>
void expect_anticipation_bit_exact(Wire&& wire, int blocks, int out_channels) {
    SignalGraph oracle;
    wire(oracle);
    oracle.set_canonical_executor_routing_enabled(true);
    REQUIRE(oracle.prepare(kSr, kFrames));
    const auto expected = run_session(oracle, blocks, /*anticipated=*/false, out_channels);

    SignalGraph antic;
    wire(antic);
    antic.set_canonical_executor_routing_enabled(true);
    antic.set_anticipation_enabled(true);
    REQUIRE(antic.prepare(kSr, kFrames));
    const auto got = run_session(antic, blocks, /*anticipated=*/true, out_channels);

    for (int c = 0; c < out_channels; ++c) {
        const auto cc = static_cast<std::size_t>(c);
        REQUIRE(got[cc].size() == expected[cc].size());
        for (std::size_t i = 0; i < got[cc].size(); ++i)
            REQUIRE(got[cc][i] == expected[cc][i]);
    }
}

}  // namespace

TEST_CASE("SignalGraph anticipation: interior internal fan-in stays bit-exact",
          "[host][anticipation][signal-graph]") {
    // gen1, gen2 -> gain(2-in summing) -> out. Interior = {gen1, gen2, gain}.
    expect_anticipation_bit_exact(
        [](SignalGraph& g) {
            const auto a = g.add_plugin_node(std::make_unique<CountingGen>(), 0, 2, "G1");
            const auto b = g.add_plugin_node(std::make_unique<CountingGen>(), 0, 2, "G2");
            const auto mix = g.add_gain_node("Mix");
            const auto out = g.add_output_node(2, "Out");
            for (int c = 0; c < 2; ++c) {
                REQUIRE(g.connect(a, c, mix, c));
                REQUIRE(g.connect(b, c, mix, c));
                REQUIRE(g.connect(mix, c, out, c));
            }
            REQUIRE(g.set_node_gain(mix, 0.25f));
        },
        10, 2);
}

TEST_CASE("SignalGraph anticipation: one interior port feeding two sinks stays bit-exact",
          "[host][anticipation][signal-graph]") {
    // gen -> gain, gain feeds out1 AND out2 (same source ports, two consumers).
    expect_anticipation_bit_exact(
        [](SignalGraph& g) {
            const auto gen = g.add_plugin_node(std::make_unique<CountingGen>(), 0, 2, "Gen");
            const auto gain = g.add_gain_node("G");
            const auto out1 = g.add_output_node(2, "Out1");
            const auto out2 = g.add_output_node(2, "Out2");
            for (int c = 0; c < 2; ++c) {
                REQUIRE(g.connect(gen, c, gain, c));
                REQUIRE(g.connect(gain, c, out1, c));
                REQUIRE(g.connect(gain, c, out2, c));
            }
            REQUIRE(g.set_node_gain(gain, 0.5f));
        },
        10, 2);  // both AudioOutputs sum into the one main output bus
}

TEST_CASE("SignalGraph anticipation: a latency-reporting interior plugin stays bit-exact",
          "[host][anticipation][signal-graph]") {
    // gen(latency 16) -> gain -> out. Exercises per-connection PDC across the
    // anticipation boundary: the lane must not double-apply the interior latency
    // that the live splice's delay compensation also accounts for.
    expect_anticipation_bit_exact(
        [](SignalGraph& g) {
            const auto gen =
                g.add_plugin_node(std::make_unique<CountingGen>(/*latency=*/16), 0, 2, "Gen");
            const auto gain = g.add_gain_node("G");
            const auto out = g.add_output_node(2, "Out");
            for (int c = 0; c < 2; ++c) {
                REQUIRE(g.connect(gen, c, gain, c));
                REQUIRE(g.connect(gain, c, out, c));
            }
            REQUIRE(g.set_node_gain(gain, 0.5f));
        },
        16, 2);
}

TEST_CASE("SignalGraph anticipation: a short block silences the interior safely",
          "[host][anticipation][signal-graph]") {
    // A block smaller than the prepared max can't be served by the fixed-block
    // lane; the interior is silenced (never re-run live), not garbage/crash.
    SignalGraph g;
    build(g);
    g.set_canonical_executor_routing_enabled(true);
    g.set_anticipation_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));
    g.pump_anticipation(8);
    const int kShort = kFrames / 2;
    std::vector<float> l(kFrames, 9.0f), r(kFrames, 9.0f);  // pre-fill non-zero
    std::array<float*, 2> oc{l.data(), r.data()};
    std::vector<float> zi(kFrames, 0.0f);
    std::array<const float*, 2> ic{zi.data(), zi.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kShort);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kShort);
    g.process(ov, iv, kShort);
    for (int i = 0; i < kShort; ++i) {
        CHECK(l[static_cast<std::size_t>(i)] == 0.0f);  // interior silenced, not garbage
        CHECK(r[static_cast<std::size_t>(i)] == 0.0f);
    }
}
