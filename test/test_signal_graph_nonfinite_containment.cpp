// Bounded fault-containment fixture for the existing SignalGraph/PluginSlot
// seams.  This deliberately does not add a non-finite policy to production:
// it proves what a host can detect and recover from today using the documented
// PluginSlot bypass control and a prepared graph.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace {

using namespace pulp::host;
constexpr double kSampleRate = 48'000.0;
constexpr int kBlock = 32;

class NonFiniteSlot final : public PluginSlot {
  public:
    enum class Mode { NonFinite, Finite };

    NonFiniteSlot() {
        info_.name = "nonfinite-fixture";
        info_.format = PluginFormat::CLAP;
        info_.num_inputs = 2;
        info_.num_outputs = 2;
        info_.is_effect = true;
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

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in, const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const ParameterEventQueue&, int n) override {
        const auto channels = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < channels; ++c) {
            const auto* src = in.channel_ptr(c);
            auto* dst = out.channel_ptr(c);
            for (int i = 0; i < n; ++i) {
                if (bypassed_) {
                    dst[static_cast<std::size_t>(i)] = src[static_cast<std::size_t>(i)];
                } else if (mode_ == Mode::NonFinite) {
                    // Put both classes of non-finite value in every block so a
                    // detector cannot accidentally only handle one spelling.
                    dst[static_cast<std::size_t>(i)] = (i & 1) == 0
                                                           ? std::numeric_limits<float>::quiet_NaN()
                                                           : std::numeric_limits<float>::infinity();
                } else {
                    dst[static_cast<std::size_t>(i)] = src[static_cast<std::size_t>(i)] * 0.5f;
                }
            }
        }
        for (std::size_t c = channels; c < out.num_channels(); ++c)
            std::fill_n(out.channel_ptr(c), n, 0.0f);
    }

    std::vector<HostParamInfo> parameters() const override {
        return {};
    }
    float get_parameter(std::uint32_t) const override {
        return 0.0f;
    }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool value) override {
        bypassed_ = value;
    }
    bool is_bypassed() const override {
        return bypassed_;
    }
    std::vector<std::uint8_t> save_state() const override {
        return {};
    }
    bool restore_state(const std::vector<std::uint8_t>&) override {
        return true;
    }
    int latency_samples() const override {
        return 0;
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

    void set_mode(Mode mode) {
        mode_ = mode;
    }

  private:
    PluginInfo info_;
    Mode mode_ = Mode::NonFinite;
    bool bypassed_ = false;
};

struct Rendered {
    std::vector<float> left;
    std::vector<float> right;
};

Rendered render(SignalGraph& graph, bool* allocation_free = nullptr) {
    std::vector<float> in_l(kBlock), in_r(kBlock), out_l(kBlock), out_r(kBlock);
    for (int i = 0; i < kBlock; ++i) {
        in_l[static_cast<std::size_t>(i)] = 0.25f + 0.01f * static_cast<float>(i);
        in_r[static_cast<std::size_t>(i)] = -0.5f + 0.02f * static_cast<float>(i);
    }
    std::array<const float*, 2> inputs{in_l.data(), in_r.data()};
    std::array<float*, 2> outputs{out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> input(inputs.data(), 2, kBlock);
    pulp::audio::BufferView<float> output(outputs.data(), 2, kBlock);
    pulp::test::RtAllocationProbe probe;
    graph.process(output, input, kBlock);
    if (allocation_free != nullptr)
        *allocation_free = !probe.saw_allocation();
    return {std::move(out_l), std::move(out_r)};
}

std::uint64_t hash(const Rendered& rendered) {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto* channel : {&rendered.left, &rendered.right}) {
        for (float sample : *channel) {
            h ^= static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(sample));
            h *= 1099511628211ull;
        }
    }
    return h;
}

} // namespace

TEST_CASE("SignalGraph host can detect and contain non-finite plugin output",
          "[host][signal-graph][fault-containment]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "Input");
    auto slot = std::make_unique<NonFiniteSlot>();
    auto* fixture = slot.get();
    const auto plugin = graph.add_plugin_node(std::move(slot), 2, 2, "Fault");
    const auto output = graph.add_output_node(2, "Output");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(graph.connect(input, c, plugin, c));
        REQUIRE(graph.connect(plugin, c, output, c));
    }
    REQUIRE(graph.prepare(kSampleRate, kBlock));
    const auto live_snapshot = graph.live_snapshot_handle();
    REQUIRE(live_snapshot != nullptr);
    auto* live_slot = graph.live_plugin_slot(plugin);
    REQUIRE(live_slot != nullptr);

    const auto fault = render(graph);
    bool saw_nan = false;
    bool saw_inf = false;
    for (float sample : fault.left) {
        saw_nan |= std::isnan(sample);
        saw_inf |= std::isinf(sample);
    }
    CHECK(saw_nan);
    CHECK(saw_inf);

    // Bypass is an existing control-thread containment seam.  The documented
    // contract is exact input passthrough, so this also supplies a finite
    // planted-negative control for the detector.
    live_slot->set_bypass(true);
    bool contained_allocation_free = false;
    const auto contained = render(graph, &contained_allocation_free);
    CHECK(contained_allocation_free);
    CHECK(std::all_of(contained.left.begin(), contained.left.end(),
                      [](float sample) { return std::isfinite(sample); }));
    CHECK(std::all_of(contained.right.begin(), contained.right.end(),
                      [](float sample) { return std::isfinite(sample); }));
    CHECK(contained.left[0] == 0.25f);
    CHECK(contained.right[0] == -0.5f);

    // Recovery is deterministic once the host clears bypass and the prepared
    // slot returns finite output.  Hashing two identical blocks catches stale
    // state or an accidental one-block recovery difference without creating a
    // compatibility promise for machine-specific execution plans.
    fixture->set_mode(NonFiniteSlot::Mode::Finite);
    live_slot->set_bypass(false);
    bool recovery_a_allocation_free = false;
    bool recovery_b_allocation_free = false;
    const auto recovered_a = render(graph, &recovery_a_allocation_free);
    const auto recovered_b = render(graph, &recovery_b_allocation_free);
    CHECK(recovery_a_allocation_free);
    CHECK(recovery_b_allocation_free);
    CHECK(std::all_of(recovered_a.left.begin(), recovered_a.left.end(),
                      [](float sample) { return std::isfinite(sample); }));
    CHECK(std::all_of(recovered_a.right.begin(), recovered_a.right.end(),
                      [](float sample) { return std::isfinite(sample); }));
    for (int i = 0; i < kBlock; ++i) {
        const auto index = static_cast<std::size_t>(i);
        CHECK(recovered_a.left[index] == (0.25f + 0.01f * static_cast<float>(i)) * 0.5f);
        CHECK(recovered_a.right[index] == (-0.5f + 0.02f * static_cast<float>(i)) * 0.5f);
    }
    CHECK(hash(recovered_a) == hash(recovered_b));
    CHECK(hash(recovered_a) != hash(fault));
}
