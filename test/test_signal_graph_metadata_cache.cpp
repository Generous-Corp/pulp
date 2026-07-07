// 2.2b prerequisite (H2, planning/2026-07-07-signalgraph-swap-and-bake-implementation-plan.md).
//
// A no-silence live swap (2.2b) recompiles the graph off the audio thread while
// process() keeps running. compile_() + the executor-routing build must therefore
// make NO live PluginSlot metadata call (parameters()/latency_samples()/
// wants_transport()) — those reach into the live plugin object (e.g. VST3
// getLatencySamples()) and are unsafe concurrent with process(). Instead the
// metadata is captured ONCE at prepare() time into prepared_plugin_meta_ and read
// from there.
//
// This suite pins that contract with a PluginSlot that COUNTS its metadata calls:
// after prepare() returns, a compile MUST consult the cache, not the slot.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace pulp::host;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 64;

// 2-in/2-out passthrough slot that counts its metadata queries. The metadata
// methods are const, so the counters are mutable + atomic (also lets the TSan
// test read them from another thread).
class CountingSlot final : public PluginSlot {
public:
    CountingSlot() {
        info_.name = "Counting";
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
                o[static_cast<std::size_t>(k)] = i[static_cast<std::size_t>(k)];
        }
        for (std::size_t c = chs; c < out.num_channels(); ++c)
            std::fill_n(out.channel_ptr(c), n, 0.0f);
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override {
        params_calls.fetch_add(1, std::memory_order_relaxed);
        pulp::host::HostParamInfo p;
        p.id = 1;
        p.name = "gain";
        p.min_value = 0.0f;
        p.max_value = 1.0f;
        p.default_value = 1.0f;
        return {p};
    }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    int latency_samples() const override {
        latency_calls.fetch_add(1, std::memory_order_relaxed);
        return 128;
    }
    int tail_samples() const override { return 0; }
    bool wants_transport() const override {
        transport_calls.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

    mutable std::atomic<int> params_calls{0};
    mutable std::atomic<int> latency_calls{0};
    mutable std::atomic<int> transport_calls{0};

private:
    PluginInfo info_;
};

void render_block(SignalGraph& g) {
    std::vector<float> l(kFrames, 0.0f), r(kFrames, 0.0f);
    std::array<float*, 2> oc{l.data(), r.data()};
    std::vector<float> in_l(kFrames, 0.25f), in_r(kFrames, 0.25f);
    std::array<const float*, 2> ic{in_l.data(), in_r.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);
    g.process(ov, iv, kFrames);
}

}  // namespace

TEST_CASE("compile_() after prepare() reads cached plugin metadata, not the live slot "
          "(2.2b H2 contract)",
          "[host][signal-graph][prepared-swap][metadata-cache]") {
    SignalGraph g;
    auto slot = std::make_unique<CountingSlot>();
    CountingSlot* raw = slot.get();
    const auto in = g.add_input_node(2, "In");
    const auto plug = g.add_plugin_node(std::move(slot), 2, 2, "Counting");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, plug, c));
        REQUIRE(g.connect(plug, c, out, c));
    }
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));

    // prepare() legitimately queried the slot once (the capture). From here on, a
    // recompile must consult prepared_plugin_meta_ — zero further live calls.
    raw->params_calls.store(0, std::memory_order_relaxed);
    raw->latency_calls.store(0, std::memory_order_relaxed);
    raw->transport_calls.store(0, std::memory_order_relaxed);

    for (int i = 0; i < 8; ++i) {
        g.compile_snapshot_for_test(kSr, kFrames);
    }

    CHECK(raw->params_calls.load(std::memory_order_relaxed) == 0);
    CHECK(raw->latency_calls.load(std::memory_order_relaxed) == 0);
    CHECK(raw->transport_calls.load(std::memory_order_relaxed) == 0);

    // Cached latency propagated into the prepared graph (128 from CountingSlot).
    CHECK(g.latency_samples() == 128);
}

TEST_CASE("compile_() is race-free vs a live process() on a PLUGIN graph "
          "(2.2b H2 — cache makes swap-time recompile plugin-call-free)",
          "[host][signal-graph][prepared-swap][metadata-cache][threads][rt-safety]") {
    SignalGraph g;
    auto slot = std::make_unique<CountingSlot>();
    const auto in = g.add_input_node(2, "In");
    const auto plug = g.add_plugin_node(std::move(slot), 2, 2, "Counting");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, plug, c));
        REQUIRE(g.connect(plug, c, out, c));
    }
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> compiles{0};
    std::thread compiler([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            g.compile_snapshot_for_test(kSr, kFrames);
            compiles.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    for (int i = 0; i < 3000; ++i) render_block(g);

    stop.store(true, std::memory_order_relaxed);
    compiler.join();

    CHECK(compiles.load(std::memory_order_relaxed) > 0);
    render_block(g);  // still usable after the concurrent recompile storm
}
