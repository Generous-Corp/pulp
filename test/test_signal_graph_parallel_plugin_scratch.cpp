#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>
#include <pulp/midi/buffer.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

using pulp::host::HostParamInfo;
using pulp::host::ParameterEventQueue;
using pulp::host::PluginFormat;
using pulp::host::PluginInfo;
using pulp::host::PluginSlot;
using pulp::host::SignalGraph;
using pulp::host::signal_graph_executor_eligible;

constexpr double kSampleRate = 48000.0;
constexpr int kFrames = 128;

PluginInfo make_plugin_info() {
    PluginInfo info{};
    info.name = "FallbackScratchProbe";
    info.format = PluginFormat::CLAP;
    info.num_inputs = 1;
    info.num_outputs = 1;
    info.category = "Fx";
    return info;
}

std::vector<float> ramp(int n, float seed) {
    std::vector<float> values(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        values[static_cast<std::size_t>(i)] =
            (static_cast<float>((i * 2654435761u) & 0xFFFF) / 32768.0f - 1.0f) *
            seed;
    }
    return values;
}

class FallbackScratchProbePlugin final : public PluginSlot {
public:
    FallbackScratchProbePlugin(std::atomic<const pulp::midi::MidiBuffer*>& seen,
                               float value)
        : seen_(seen), value_(value), info_(make_plugin_info()) {}

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer& midi_out,
                 const ParameterEventQueue&,
                 int n) override {
        seen_.store(&midi_out, std::memory_order_release);
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            float* dst = out.channel_ptr(c);
            const float* src = c < in.num_channels() ? in.channel_ptr(c) : nullptr;
            for (int s = 0; s < n; ++s) {
                const auto i = static_cast<std::size_t>(s);
                dst[i] = (src ? src[i] : 0.0f) + value_;
            }
        }
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
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
    std::atomic<const pulp::midi::MidiBuffer*>& seen_;
    float value_;
    PluginInfo info_;
};

} // namespace

TEST_CASE("SignalGraph parallel plugin bindings use isolated fallback scratch",
          "[host][graph][executor][routing][parallel][plugin]") {
    SignalGraph graph;
    std::atomic<const pulp::midi::MidiBuffer*> seen_a{nullptr};
    std::atomic<const pulp::midi::MidiBuffer*> seen_b{nullptr};

    const auto in = graph.add_input_node(1, "In");
    const auto a = graph.add_plugin_node(
        std::make_unique<FallbackScratchProbePlugin>(seen_a, 0.1f), 1, 1, "A");
    const auto b = graph.add_plugin_node(
        std::make_unique<FallbackScratchProbePlugin>(seen_b, 0.2f), 1, 1, "B");
    const auto out = graph.add_output_node(1, "Out");
    REQUIRE(graph.connect(in, 0, a, 0));
    REQUIRE(graph.connect(in, 0, b, 0));
    REQUIRE(graph.connect(a, 0, out, 0));
    REQUIRE(graph.connect(b, 0, out, 0));
    graph.set_parallel_routing_enabled(true);
    REQUIRE(graph.prepare(kSampleRate, kFrames));
    REQUIRE(signal_graph_executor_eligible(graph));

    std::vector<float> input = ramp(kFrames, 0.5f);
    std::vector<float> output(static_cast<std::size_t>(kFrames), 0.0f);
    const float* in_ptrs[1] = {input.data()};
    float* out_ptrs[1] = {output.data()};
    pulp::audio::BufferView<const float> input_view(in_ptrs, 1, kFrames);
    pulp::audio::BufferView<float> output_view(out_ptrs, 1, kFrames);

    graph.process(output_view, input_view, kFrames);

    const auto* scratch_a = seen_a.load(std::memory_order_acquire);
    const auto* scratch_b = seen_b.load(std::memory_order_acquire);
    REQUIRE(scratch_a != nullptr);
    REQUIRE(scratch_b != nullptr);
    REQUIRE(scratch_a != scratch_b);
    const auto stats = graph.routing_executor_stats();
    REQUIRE(stats.blocks_processed == 1);
    if (std::thread::hardware_concurrency() > 1) {
        REQUIRE(stats.parallel_levels_dispatched > 0);
    }
}
