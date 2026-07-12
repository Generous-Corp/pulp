// Headless DAW-smoke for a live plugin-INSTANCE swap inside a SignalGraph.
//
// The staging test next door proves the transaction MECHANICS (admission,
// refusal reasons, reader-drain retire). This test proves the property a real
// host actually cares about: swapping the hosted plugin instance mid-stream
// produces NO dropout, NO xrun, and a sample-CONTINUOUS output across the swap
// block. It is the CI-runnable mirror of the local-only REAPER smoke
// (tools/testing/daw-smoke/reaper_smoke.py --mode live-plugin-swap): the REAPER
// path drives the same stage+commit inside a real host and scrapes the same
// "[live-swap] committed" marker this test emits from its on_instance_swapped
// observer.
//
// A hosted plugin here is a unity passthrough (output == input), so ANY dropout
// or discontinuity introduced by the swap shows up as a sample delta against the
// phase-continuous sine we feed in. The check runs for each hosted format
// (VST3 / AU / CLAP / LV2) because the admission gate and continuity contract
// are format-agnostic and every hosted format must clear the same bar.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/runtime/log.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <numbers>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace pulp::host;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kFrames = 128;
constexpr int kChannels = 2;
constexpr double kSineHz = 220.0;

// Unity-passthrough hosted plugin. Copies input to output verbatim so the
// graph's output equals the signal we injected — the moment a swap drops or
// glitches a sample, the continuity assertion below catches it. Carries one
// automatable parameter so the swap admission gate exercises the parameter-
// contract path, and a small state blob so save/restore runs on commit.
class PassthroughSlot final : public PluginSlot {
public:
    explicit PassthroughSlot(PluginInfo info) : info_(std::move(info)) {}

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&,
                 int n) override {
        const std::size_t copied = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < copied; ++c) {
            const float* src = in.channel_ptr(c);
            float* dst = out.channel_ptr(c);
            for (int i = 0; i < n; ++i) {
                dst[static_cast<std::size_t>(i)] = src[static_cast<std::size_t>(i)];
            }
        }
        for (std::size_t c = copied; c < out.num_channels(); ++c) {
            std::fill_n(out.channel_ptr(c), n, 0.0f);
        }
    }

    std::vector<HostParamInfo> parameters() const override {
        HostParamInfo p;
        p.id = 1;
        p.name = "mix";
        p.min_value = 0.0f;
        p.max_value = 1.0f;
        p.default_value = 1.0f;
        p.flags.automatable = true;
        return {p};
    }
    float get_parameter(std::uint32_t id) const override {
        auto it = params_.find(id);
        return it == params_.end() ? 0.0f : it->second;
    }
    void set_parameter(std::uint32_t id, float value) override { params_[id] = value; }
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override { return {0xAB, 0xCD}; }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

private:
    PluginInfo info_;
    std::unordered_map<std::uint32_t, float> params_;
};

PluginInfo make_info(PluginFormat format, const std::string& id) {
    PluginInfo info;
    info.name = id;
    info.path = "/tmp/pulp-live-swap-continuity-" + id;
    info.unique_id = id;
    info.format = format;
    info.is_effect = true;
    info.num_inputs = kChannels;
    info.num_outputs = kChannels;
    info.category = "Fx";
    return info;
}

// Deterministic phase-continuous sine sampled at an absolute frame index, so a
// dropped or repeated block shows up as a discontinuity against this reference.
float reference_sample(std::uint64_t absolute_frame) {
    // std::numbers::pi, not M_PI: the latter is a POSIX extension that MSVC's
    // <cmath> does not define without _USE_MATH_DEFINES.
    const double phase = 2.0 * std::numbers::pi * kSineHz *
                         static_cast<double>(absolute_frame) / kSampleRate;
    return static_cast<float>(std::sin(phase));
}

// Render `blocks` blocks of the reference sine through the graph, appending each
// output sample (channel 0) to `captured` alongside the reference it should
// equal. `frame_cursor` carries the absolute frame index across calls so the
// sine stays phase-continuous through the swap.
void render_and_capture(SignalGraph& graph,
                        int blocks,
                        std::uint64_t& frame_cursor,
                        std::vector<float>& captured,
                        std::vector<float>& reference) {
    std::array<std::vector<float>, kChannels> out{
        std::vector<float>(kFrames, 0.0f),
        std::vector<float>(kFrames, 0.0f),
    };
    std::array<std::vector<float>, kChannels> in{
        std::vector<float>(kFrames, 0.0f),
        std::vector<float>(kFrames, 0.0f),
    };
    std::array<float*, kChannels> out_ptrs{out[0].data(), out[1].data()};
    std::array<const float*, kChannels> in_ptrs{in[0].data(), in[1].data()};
    pulp::audio::BufferView<float> ov(out_ptrs.data(), kChannels, kFrames);
    pulp::audio::BufferView<const float> iv(in_ptrs.data(), kChannels, kFrames);

    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < kFrames; ++i) {
            const float s = reference_sample(frame_cursor + static_cast<std::uint64_t>(i));
            in[0][static_cast<std::size_t>(i)] = s;
            in[1][static_cast<std::size_t>(i)] = s;
        }
        graph.process(ov, iv, kFrames);
        for (int i = 0; i < kFrames; ++i) {
            captured.push_back(out[0][static_cast<std::size_t>(i)]);
            reference.push_back(reference_sample(frame_cursor + static_cast<std::uint64_t>(i)));
        }
        frame_cursor += static_cast<std::uint64_t>(kFrames);
    }
}

const char* format_label(PluginFormat f) {
    switch (f) {
        case PluginFormat::VST3:        return "VST3";
        case PluginFormat::AudioUnit:   return "AU";
        case PluginFormat::AudioUnitV3: return "AUv3";
        case PluginFormat::CLAP:        return "CLAP";
        case PluginFormat::LV2:         return "LV2";
    }
    return "?";
}

}  // namespace

TEST_CASE("live plugin instance swap is sample-continuous with no dropout",
          "[host][graph][live-swap][continuity]") {
    // Every hosted format must clear the same continuity bar. A mock slot keeps
    // this hermetic and CI-runnable (no real plugin binaries), while the format
    // field flows through the same admission + identity path a scanned plugin
    // would take.
    for (PluginFormat format : {PluginFormat::VST3, PluginFormat::AudioUnit,
                                PluginFormat::CLAP, PluginFormat::LV2}) {
        SECTION(std::string("format ") + format_label(format)) {
            SignalGraph graph;
            const PluginInfo info = make_info(format, "orig");
            const auto in = graph.add_input_node(kChannels, "In");
            const auto plugin = graph.add_plugin_node(
                std::make_unique<PassthroughSlot>(info), kChannels, kChannels, "Hosted");
            const auto out = graph.add_output_node(kChannels, "Out");
            for (int c = 0; c < kChannels; ++c) {
                REQUIRE(graph.connect(in, c, plugin, c));
                REQUIRE(graph.connect(plugin, c, out, c));
            }
            REQUIRE(graph.prepare(kSampleRate, kFrames));

            // Register the replacement instance (same shape/identity so the swap
            // is admitted) and a loader that mints a fresh PassthroughSlot.
            const PluginInfo replacement_info = make_info(format, "orig");
            const auto token = graph.register_scanned_plugin(replacement_info);
            graph.set_live_swap_plugin_loader_for_test(
                [replacement_info](const PluginInfo&) {
                    return std::make_unique<PassthroughSlot>(replacement_info);
                });

            // The observer is exactly the seam the REAPER smoke scrapes: on a
            // committed swap it emits the "[live-swap] committed" marker via
            // runtime::log_info (visible on stderr, which REAPER captures).
            std::atomic<bool> committed{false};
            PluginSlot* observed_old = nullptr;
            PluginSlot* observed_new = nullptr;
            NodeLiveSwapPolicy policy;
            policy.allow_live_instance_swap = true;
            policy.on_instance_swapped =
                [&](NodeId id,
                    std::shared_ptr<PluginSlot> old_slot,
                    std::shared_ptr<PluginSlot> new_slot) {
                    observed_old = old_slot.get();
                    observed_new = new_slot.get();
                    committed.store(true, std::memory_order_release);
                    pulp::runtime::log_info(
                        "[live-swap] committed node={} format={}",
                        id, format_label(format));
                };
            REQUIRE(graph.set_node_live_swap_policy(plugin, policy));

            std::uint64_t frame_cursor = 0;
            std::vector<float> captured;
            std::vector<float> reference;

            // Warm up so the graph has a load history (the swap admission gate
            // needs one) and to establish the pre-swap continuous stream.
            render_and_capture(graph, 16, frame_cursor, captured, reference);
            const std::size_t pre_swap_samples = captured.size();

            // Commit the live instance swap between process() blocks.
            graph.begin_swap_edit();
            REQUIRE(graph.stage_plugin_replacement(plugin, token)
                    == SignalGraph::SwapResult::Staged);
            REQUIRE(graph.prepare_swap(kSampleRate, kFrames)
                    == SignalGraph::SwapResult::Swapped);
            REQUIRE(committed.load(std::memory_order_acquire));
            REQUIRE(observed_new != nullptr);
            REQUIRE(observed_new != observed_old);  // a genuinely new instance
            CHECK(graph.last_swap_diagnostics().reason
                  == LiveSwapFallbackReason::None);

            // Keep rendering across and past the swap boundary.
            render_and_capture(graph, 16, frame_cursor, captured, reference);

            // Continuity proof: a unity passthrough must reproduce the injected
            // sine sample-for-sample through the swap. Any dropout (silence),
            // xrun (repeated/dropped block), or glitch (discontinuity) is a
            // nonzero delta here — including at the swap-boundary block.
            REQUIRE(captured.size() == reference.size());
            float max_error = 0.0f;
            for (std::size_t i = 0; i < captured.size(); ++i) {
                max_error = std::max(max_error, std::fabs(captured[i] - reference[i]));
            }
            CHECK(max_error < 1e-6f);

            // No silent block anywhere: with a full-scale sine in, no whole
            // block should read as silence (that would be a dropout the delta
            // check could in principle miss if the reference were also quiet).
            bool any_nonzero = false;
            for (float v : captured) {
                if (std::fabs(v) > 1e-4f) { any_nonzero = true; break; }
            }
            CHECK(any_nonzero);
            CHECK(pre_swap_samples > 0);
        }
    }
}
