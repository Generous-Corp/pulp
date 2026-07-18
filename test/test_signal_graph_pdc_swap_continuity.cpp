#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace pulp::host;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr int kDelay = 97;

enum class Mode { Legacy, RoutedSerial, RoutedParallel };

class LatencySilenceSlot final : public PluginSlot {
  public:
    LatencySilenceSlot() {
        info_.name = "LatencySilence";
        info_.unique_id = "pulp.test.latency-silence";
        info_.format = PluginFormat::CLAP;
        info_.is_effect = true;
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }

    const PluginInfo& info() const override {
        return info_;
    }
    bool is_loaded() const override {
        return true;
    }
    bool prepare(double, int) override {
        return true;
    }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out, const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&, const ParameterEventQueue&,
                 int frames) override {
        for (std::size_t channel = 0; channel < out.num_channels(); ++channel) {
            std::fill_n(out.channel_ptr(channel), frames, 0.0f);
        }
    }
    std::vector<HostParamInfo> parameters() const override {
        return {};
    }
    float get_parameter(std::uint32_t) const override {
        return 0.0f;
    }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override {
        return false;
    }
    std::vector<std::uint8_t> save_state() const override {
        return {};
    }
    bool restore_state(const std::vector<std::uint8_t>&) override {
        return true;
    }
    int latency_samples() const override {
        return kDelay;
    }
    int tail_samples() const override {
        return 0;
    }
    bool has_editor() const override {
        return false;
    }
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}

  private:
    PluginInfo info_;
};

struct GraphFixture {
    SignalGraph graph;
    NodeId input = 0;
    NodeId output = 0;
    NodeId spare_source = 0;
    NodeId spare_dest = 0;

    explicit GraphFixture(Mode mode, bool connect_spare = false);
};

void select_mode(SignalGraph& graph, Mode mode) {
    graph.set_anticipation_enabled(false);
    graph.set_canonical_executor_routing_enabled(mode != Mode::Legacy);
    graph.set_parallel_routing_enabled(mode == Mode::RoutedParallel);
    if (mode == Mode::RoutedParallel)
        graph.set_parallel_min_work_units(0);
}

GraphFixture::GraphFixture(Mode mode, bool connect_spare) {
    select_mode(graph, mode);
    input = graph.add_input_node(1, "Input");
    const auto latent =
        graph.add_plugin_node(std::make_unique<LatencySilenceSlot>(), 1, 1, "Latent");
    output = graph.add_output_node(1, "Output");
    spare_source = graph.add_gain_node("Spare source");
    spare_dest = graph.add_gain_node("Spare destination");

    // When requested, put an isolated zero-delay edge before every main-path
    // edge. Removing it during the swap shifts the delayed edge's connection
    // index and proves state follows private identity rather than vector index.
    if (connect_spare) REQUIRE(graph.connect(spare_source, 0, spare_dest, 0));
    REQUIRE(graph.connect(input, 0, latent, 0));
    REQUIRE(graph.connect(latent, 0, output, 0));
    REQUIRE(graph.connect(input, 0, output, 0));
    REQUIRE(graph.prepare(kSampleRate, kBlockSize));
}

bool render_block_matches(SignalGraph& graph, std::uint64_t absolute_frame) {
    std::array<float, kBlockSize> input{};
    std::array<float, kBlockSize> output{};
    for (int i = 0; i < kBlockSize; ++i) {
        input[static_cast<std::size_t>(i)] =
            static_cast<float>(absolute_frame + static_cast<std::uint64_t>(i) + 1);
    }
    const std::array<const float*, 1> input_channels{input.data()};
    const std::array<float*, 1> output_channels{output.data()};
    pulp::audio::BufferView<const float> input_view(input_channels.data(), 1, kBlockSize);
    pulp::audio::BufferView<float> output_view(output_channels.data(), 1, kBlockSize);
    graph.process(output_view, input_view, kBlockSize);

    for (int i = 0; i < kBlockSize; ++i) {
        const auto frame = absolute_frame + static_cast<std::uint64_t>(i);
        const float expected = frame < kDelay ? 0.0f : static_cast<float>(frame - kDelay + 1);
        if (output[static_cast<std::size_t>(i)] != expected) return false;
    }
    return true;
}

void render_checked_block(SignalGraph& graph, std::uint64_t absolute_frame) {
    REQUIRE(render_block_matches(graph, absolute_frame));
}

} // namespace

TEST_CASE("PDC state is sample continuous across a live structural edit",
          "[host][signal-graph][prepared-swap][pdc]") {
    for (const auto mode : {Mode::Legacy, Mode::RoutedSerial, Mode::RoutedParallel}) {
        GraphFixture fixture(mode, true);
        std::uint64_t frame = 0;
        render_checked_block(fixture.graph, frame);
        frame += kBlockSize;
        render_checked_block(fixture.graph, frame);
        frame += kBlockSize;

        // Remove the connection before the delayed signal path. Its vector index
        // changes, but its private identity and 97-sample history must survive.
        // 97 is deliberately not block-aligned with the 64-frame render schedule.
        fixture.graph.begin_swap_edit();
        REQUIRE(fixture.graph.disconnect(
            fixture.spare_source, 0, fixture.spare_dest, 0));
        REQUIRE(fixture.graph.prepare_swap(kSampleRate, kBlockSize) ==
                SignalGraph::SwapResult::Swapped);

        for (int block = 0; block < 6; ++block) {
            render_checked_block(fixture.graph, frame);
            frame += kBlockSize;
        }
    }
}

TEST_CASE("PDC active snapshots pin their prepared execution domain",
          "[host][signal-graph][pdc][routing]") {
    for (const auto mode : {Mode::RoutedSerial, Mode::RoutedParallel}) {
        GraphFixture fixture(mode);
        std::uint64_t frame = 0;
        render_checked_block(fixture.graph, frame);
        frame += kBlockSize;
        render_checked_block(fixture.graph, frame);
        frame += kBlockSize;

        // Each execution domain owns independent delay history. A relaxed
        // control toggle must not select a stale domain while this prepared
        // snapshot has live PDC state.
        if (mode == Mode::RoutedParallel) {
            fixture.graph.set_parallel_routing_enabled(false);
        } else {
            fixture.graph.set_canonical_executor_routing_enabled(false);
        }
        for (int block = 0; block < 6; ++block) {
            render_checked_block(fixture.graph, frame);
            frame += kBlockSize;
        }
    }
}

TEST_CASE("PDC live swap refuses a prepared execution domain change",
          "[host][signal-graph][prepared-swap][pdc][routing]") {
    GraphFixture fixture(Mode::RoutedSerial);
    render_checked_block(fixture.graph, 0);
    render_checked_block(fixture.graph, kBlockSize);

    fixture.graph.set_canonical_executor_routing_enabled(false);
    fixture.graph.begin_swap_edit();
    REQUIRE(fixture.graph.set_node_gain(fixture.spare_source, 0.5f));
    CHECK(fixture.graph.prepare_swap(kSampleRate, kBlockSize) ==
          SignalGraph::SwapResult::NeedsEagerPrepare);
}

TEST_CASE("PDC state refuses equal looking disconnect and reconnect",
          "[host][signal-graph][prepared-swap][pdc]") {
    GraphFixture fixture(Mode::RoutedSerial);
    render_checked_block(fixture.graph, 0);
    render_checked_block(fixture.graph, kBlockSize);

    fixture.graph.begin_swap_edit();
    REQUIRE(fixture.graph.disconnect(fixture.input, 0, fixture.output, 0));
    REQUIRE(fixture.graph.connect(fixture.input, 0, fixture.output, 0));
    CHECK(fixture.graph.prepare_swap(kSampleRate, kBlockSize) ==
          SignalGraph::SwapResult::NeedsEagerPrepare);
}

TEST_CASE("PDC state carry rejects feedback graphs", "[host][signal-graph][prepared-swap][pdc]") {
    GraphFixture fixture(Mode::RoutedSerial);
    REQUIRE(fixture.graph.connect_feedback(fixture.spare_source, 0, fixture.spare_dest, 0));
    REQUIRE(fixture.graph.prepare(kSampleRate, kBlockSize));
    fixture.graph.begin_swap_edit();
    REQUIRE(fixture.graph.set_node_gain(fixture.spare_source, 0.5f));
    CHECK(fixture.graph.prepare_swap(kSampleRate, kBlockSize) ==
          SignalGraph::SwapResult::NeedsEagerPrepare);
}

TEST_CASE("PDC state carry rejects routed domain changes",
          "[host][signal-graph][prepared-swap][pdc]") {
    GraphFixture fixture(Mode::RoutedSerial);
    fixture.graph.set_parallel_routing_enabled(true);
    fixture.graph.begin_swap_edit();
    REQUIRE(fixture.graph.set_node_gain(fixture.spare_source, 0.5f));
    CHECK(fixture.graph.prepare_swap(kSampleRate, kBlockSize) ==
          SignalGraph::SwapResult::NeedsEagerPrepare);
}

TEST_CASE("Routed PDC pool adoption shares only equal ring layouts",
          "[format][graph-runtime][pdc]") {
    pulp::format::GraphRuntimeBufferPool old_pool;
    const std::array<std::uint32_t, 2> old_delays{97, 0};
    REQUIRE(old_pool.reset(1, 64, old_delays));
    auto old_ring = old_pool.delay_ring(0);
    REQUIRE(old_ring.data != nullptr);
    old_ring.data[11] = 0.75f;
    *old_ring.write_pos = 23;

    pulp::format::GraphRuntimeBufferPool candidate;
    const std::array<std::uint32_t, 2> reordered_delays{0, 97};
    REQUIRE(candidate.reset(1, 64, reordered_delays));
    REQUIRE(candidate.can_adopt_delay_ring_state(1, old_pool, 0));
    REQUIRE(candidate.adopt_delay_ring_state(1, old_pool, 0));
    const auto adopted = candidate.delay_ring(1);
    CHECK(adopted.data == old_ring.data);
    CHECK(adopted.write_pos == old_ring.write_pos);
    CHECK(adopted.data[11] == 0.75f);
    CHECK(*adopted.write_pos == 23);

    pulp::format::GraphRuntimeBufferPool copied(candidate);
    const auto copied_ring = copied.delay_ring(1);
    REQUIRE(copied_ring.data != nullptr);
    CHECK(copied_ring.data != adopted.data);
    CHECK(copied_ring.write_pos != adopted.write_pos);
    CHECK(copied_ring.data[11] == 0.75f);
    CHECK(*copied_ring.write_pos == 23);
    copied_ring.data[11] = 0.25f;
    *copied_ring.write_pos = 7;
    CHECK(adopted.data[11] == 0.75f);
    CHECK(*adopted.write_pos == 23);

    pulp::format::GraphRuntimeBufferPool assigned;
    assigned = candidate;
    const auto assigned_ring = assigned.delay_ring(1);
    REQUIRE(assigned_ring.data != nullptr);
    CHECK(assigned_ring.data != adopted.data);
    CHECK(assigned_ring.write_pos != adopted.write_pos);
    CHECK(assigned_ring.data[11] == 0.75f);
    CHECK(*assigned_ring.write_pos == 23);

    pulp::format::GraphRuntimeBufferPool wrong_delay;
    const std::array<std::uint32_t, 1> delay_96{96};
    REQUIRE(wrong_delay.reset(1, 64, delay_96));
    CHECK_FALSE(wrong_delay.can_adopt_delay_ring_state(0, old_pool, 0));

    pulp::format::GraphRuntimeBufferPool wrong_size;
    const std::array<std::uint32_t, 1> delay_97{97};
    REQUIRE(wrong_size.reset(1, 32, delay_97));
    CHECK_FALSE(wrong_size.can_adopt_delay_ring_state(0, old_pool, 0));
    CHECK_FALSE(candidate.can_adopt_delay_ring_state(0, old_pool, 0));
    CHECK_FALSE(candidate.can_adopt_delay_ring_state(1, old_pool, 9));
}

TEST_CASE("PDC state carry remains race free during live swaps",
          "[host][signal-graph][prepared-swap][pdc][threads][rt-safety]") {
    GraphFixture fixture(Mode::RoutedSerial, true);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> rendered_blocks{0};
    std::atomic<bool> structural_swap{false};
    std::atomic<std::uint64_t> swaps{0};
    std::atomic<std::uint64_t> failures{0};
    std::thread editor([&] {
        while (rendered_blocks.load(std::memory_order_acquire) < 4 &&
               !stop.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
        }
        if (!stop.load(std::memory_order_relaxed)) {
            fixture.graph.begin_swap_edit();
            const bool disconnected = fixture.graph.disconnect(
                fixture.spare_source, 0, fixture.spare_dest, 0);
            const bool swapped = disconnected &&
                fixture.graph.prepare_swap(kSampleRate, kBlockSize) ==
                    SignalGraph::SwapResult::Swapped;
            structural_swap.store(swapped, std::memory_order_release);
            if (!swapped) failures.fetch_add(1, std::memory_order_relaxed);
        }
        bool high = false;
        while (!stop.load(std::memory_order_relaxed)) {
            fixture.graph.begin_swap_edit();
            fixture.graph.set_node_gain(fixture.spare_source, high ? 0.75f : 0.5f);
            high = !high;
            if (fixture.graph.prepare_swap(kSampleRate, kBlockSize) ==
                SignalGraph::SwapResult::Swapped) {
                swaps.fetch_add(1, std::memory_order_relaxed);
            } else {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    std::uint64_t frame = 0;
    bool continuous = true;
    for (int block = 0; block < 1000; ++block) {
        if (!render_block_matches(fixture.graph, frame)) continuous = false;
        frame += kBlockSize;
        rendered_blocks.fetch_add(1, std::memory_order_release);
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_relaxed);
    editor.join();
    CHECK(continuous);
    CHECK(structural_swap.load(std::memory_order_acquire));
    CHECK(swaps.load(std::memory_order_relaxed) > 0);
    CHECK(failures.load(std::memory_order_relaxed) == 0);
}
