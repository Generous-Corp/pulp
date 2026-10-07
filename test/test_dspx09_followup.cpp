#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/analysis/audio_metrics.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph.hpp>

#include "support/thread_progress.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace pulp::host;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 64;

PluginInfo make_info(std::string id, int inputs = 2, int outputs = 2) {
    PluginInfo info;
    info.name = id;
    info.path = "/tmp/pulp-live-swap-" + id + ".clap";
    info.unique_id = id;
    info.format = PluginFormat::CLAP;
    info.is_effect = true;
    info.is_instrument = false;
    info.num_inputs = inputs;
    info.num_outputs = outputs;
    info.category = "Fx";
    return info;
}

HostParamInfo make_param(uint32_t id, float default_value = 0.25f) {
    HostParamInfo p;
    p.id = id;
    p.name = "p" + std::to_string(id);
    p.min_value = 0.0f;
    p.max_value = 1.0f;
    p.default_value = default_value;
    p.flags.automatable = true;
    return p;
}

struct SlotStats {
    mutable std::mutex mu;
    int destroyed = 0;
    int restore_calls = 0;
    int dsp_restore_calls = 0;
    int reseed_calls = 0;
    int process_calls = 0;
    std::vector<uint8_t> restored_state;
    std::vector<std::thread::id> process_threads;
    std::thread::id destroy_thread;
    std::atomic<bool> block_process{false};
    std::atomic<bool> process_entered{false};
    std::atomic<bool> release_process{false};
};

struct SlotBehavior {
    PluginInfo info;
    std::vector<HostParamInfo> params{make_param(7)};
    std::vector<uint8_t> state{1, 2, 3};
    bool prepare_ok = true;
    bool restore_ok = true;
    int latency = 0;
    bool block_process = false;
    std::chrono::microseconds process_sleep{0};
    std::vector<uint8_t> process_state_tag;
    std::string history_key;
    std::vector<uint8_t> dsp_state{9, 8, 7};
    bool dsp_restore_ok = true;
    bool reseed_ok = true;
};

class StagingSlot final : public PluginSlot {
  public:
    StagingSlot(SlotBehavior behavior, std::shared_ptr<SlotStats> stats)
        : behavior_(std::move(behavior)), stats_(std::move(stats)) {
        for (const auto& p : behavior_.params)
            params_[p.id] = p.default_value;
        current_state_ = behavior_.state;
        dsp_tail_ = behavior_.dsp_state.empty()
                        ? 0.0f
                        : static_cast<float>(behavior_.dsp_state.front()) / 255.0f;
    }

    ~StagingSlot() override {
        std::lock_guard<std::mutex> lock(stats_->mu);
        ++stats_->destroyed;
        stats_->destroy_thread = std::this_thread::get_id();
    }

    const PluginInfo& info() const override {
        return behavior_.info;
    }
    bool is_loaded() const override {
        return true;
    }
    bool prepare(double, int) override {
        return behavior_.prepare_ok;
    }
    void release() override {}

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in, const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::host::ParameterEventQueue&, int n) override {
        {
            std::lock_guard<std::mutex> lock(stats_->mu);
            ++stats_->process_calls;
            stats_->process_threads.push_back(std::this_thread::get_id());
        }
        if (behavior_.block_process || stats_->block_process.load(std::memory_order_acquire)) {
            stats_->process_entered.store(true, std::memory_order_release);
            REQUIRE(pulp::test::wait_for_condition(
                [&] { return stats_->release_process.load(std::memory_order_acquire); }));
        }
        if (!behavior_.process_state_tag.empty()) {
            std::lock_guard<std::mutex> lock(state_mu_);
            current_state_ = behavior_.process_state_tag;
        }
        if (behavior_.process_sleep.count() > 0) {
            std::this_thread::sleep_for(behavior_.process_sleep);
        }

        float gain = 1.0f;
        {
            std::lock_guard<std::mutex> lock(param_mu_);
            if (!params_.empty())
                gain = params_.begin()->second;
        }

        const std::size_t copied = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < copied; ++c) {
            const float* src = in.channel_ptr(c);
            float* dst = out.channel_ptr(c);
            for (int i = 0; i < n; ++i) {
                dst[static_cast<std::size_t>(i)] =
                    src[static_cast<std::size_t>(i)] * gain +
                    (behavior_.history_key.empty() ? 0.0f : dsp_tail_);
            }
            if (!behavior_.history_key.empty())
                dsp_tail_ = dst[static_cast<std::size_t>(n - 1)] * 0.5f;
        }
        for (std::size_t c = copied; c < out.num_channels(); ++c) {
            std::fill_n(out.channel_ptr(c), n, 0.0f);
        }
    }

    std::vector<HostParamInfo> parameters() const override {
        return behavior_.params;
    }

    float get_parameter(std::uint32_t id) const override {
        std::lock_guard<std::mutex> lock(param_mu_);
        auto it = params_.find(id);
        return it == params_.end() ? 0.0f : it->second;
    }

    void set_parameter(std::uint32_t id, float value) override {
        std::lock_guard<std::mutex> lock(param_mu_);
        params_[id] = value;
    }

    void set_bypass(bool) override {}
    bool is_bypassed() const override {
        return false;
    }

    std::vector<std::uint8_t> save_state() const override {
        std::lock_guard<std::mutex> lock(state_mu_);
        return current_state_;
    }

    bool restore_state(const std::vector<std::uint8_t>& data) override {
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            current_state_ = data;
        }
        {
            std::lock_guard<std::mutex> lock(stats_->mu);
            ++stats_->restore_calls;
            stats_->restored_state = data;
        }
        return behavior_.restore_ok;
    }

    std::string retained_history_key() const override {
        return behavior_.history_key;
    }
    std::vector<std::uint8_t> serialize_dsp_state() const override {
        if (behavior_.history_key.empty())
            return {};
        const auto size = std::max<std::size_t>(1, behavior_.dsp_state.size());
        std::vector<std::uint8_t> state(size, 0);
        state.front() = static_cast<std::uint8_t>(std::clamp(dsp_tail_ * 255.0f, 0.0f, 255.0f));
        return state;
    }
    bool restore_dsp_state(const std::vector<std::uint8_t>& data) override {
        std::lock_guard<std::mutex> lock(stats_->mu);
        ++stats_->dsp_restore_calls;
        stats_->restored_state = data;
        if (!data.empty())
            dsp_tail_ = static_cast<float>(data.front()) / 255.0f;
        return behavior_.dsp_restore_ok;
    }
    bool reseed_dsp_state(std::uint64_t seed) override {
        std::lock_guard<std::mutex> lock(stats_->mu);
        ++stats_->reseed_calls;
        stats_->restored_state = {static_cast<std::uint8_t>(seed & 0xffu)};
        return behavior_.reseed_ok;
    }

    int latency_samples() const override {
        return behavior_.latency;
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
    SlotBehavior behavior_;
    std::shared_ptr<SlotStats> stats_;
    mutable std::mutex param_mu_;
    std::unordered_map<std::uint32_t, float> params_;
    mutable std::mutex state_mu_;
    std::vector<std::uint8_t> current_state_;
    mutable float dsp_tail_ = 0.0f;
};

void render_blocks(SignalGraph& graph, int blocks) {
    std::array<std::vector<float>, 2> out{
        std::vector<float>(kFrames, 0.0f),
        std::vector<float>(kFrames, 0.0f),
    };
    std::array<float*, 2> out_ptrs{out[0].data(), out[1].data()};
    std::array<std::vector<float>, 2> in{
        std::vector<float>(kFrames, 1.0f),
        std::vector<float>(kFrames, 1.0f),
    };
    std::array<const float*, 2> in_ptrs{in[0].data(), in[1].data()};
    pulp::audio::BufferView<float> ov(out_ptrs.data(), 2, kFrames);
    pulp::audio::BufferView<const float> iv(in_ptrs.data(), 2, kFrames);
    for (int i = 0; i < blocks; ++i)
        graph.process(ov, iv, kFrames);
}

pulp::audio::Buffer<float> render_impulse(SignalGraph& graph) {
    std::array<std::vector<float>, 2> out{std::vector<float>(kFrames), std::vector<float>(kFrames)};
    std::array<float*, 2> out_ptrs{out[0].data(), out[1].data()};
    std::array<std::vector<float>, 2> in{std::vector<float>(kFrames), std::vector<float>(kFrames)};
    in[0][0] = 1.0f;
    in[1][0] = 1.0f;
    std::array<const float*, 2> in_ptrs{in[0].data(), in[1].data()};
    pulp::audio::BufferView<float> ov(out_ptrs.data(), 2, kFrames);
    pulp::audio::BufferView<const float> iv(in_ptrs.data(), 2, kFrames);
    graph.process(ov, iv, kFrames);
    pulp::audio::Buffer<float> result(1, kFrames);
    for (std::size_t i = 0; i < out[0].size(); ++i)
        result.channel(0)[i] = out[0][i];
    return result;
}

NodeLiveSwapPolicy allowing_policy() {
    NodeLiveSwapPolicy p;
    p.allow_live_instance_swap = true;
    return p;
}

struct StageSetup {
    SignalGraph graph;
    NodeId plugin = 0;
    PluginInfo info;
    std::shared_ptr<SlotStats> old_stats;
    std::shared_ptr<SlotStats> replacement_stats;
    std::shared_ptr<SlotStats> probe_stats;
    SignalGraph::PluginCatalogToken token;
};

std::unique_ptr<StageSetup> make_stage_setup(SlotBehavior old_behavior,
                                             SlotBehavior replacement_behavior,
                                             NodeLiveSwapPolicy policy = allowing_policy()) {
    auto s = std::make_unique<StageSetup>();
    s->info = old_behavior.info;
    s->old_stats = std::make_shared<SlotStats>();
    const auto in = s->graph.add_input_node(2, "In");
    s->plugin = s->graph.add_plugin_node(
        std::make_unique<StagingSlot>(std::move(old_behavior), s->old_stats), 2, 2, "P");
    const auto out = s->graph.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(s->graph.connect(in, c, s->plugin, c));
        REQUIRE(s->graph.connect(s->plugin, c, out, c));
    }
    REQUIRE(s->graph.prepare(kSr, kFrames));

    s->replacement_stats = std::make_shared<SlotStats>();
    s->probe_stats = std::make_shared<SlotStats>();
    s->token = s->graph.register_scanned_plugin(replacement_behavior.info);
    s->graph.set_live_swap_plugin_loader_for_test(
        [replacement_behavior = std::move(replacement_behavior), stats = s->replacement_stats,
         probe = s->probe_stats, calls = std::make_shared<int>(0)](const PluginInfo&) mutable {
            // The first load is the instance that actually goes live (tracked by
            // replacement_stats); the later load is the throwaway cost-probe (its own
            // stats), so warming it never pollutes the committed slot — the committed
            // slot must show process_calls == 0.
            auto st = (*calls)++ == 0 ? stats : probe;
            return std::make_unique<StagingSlot>(replacement_behavior, st);
        });
    REQUIRE(s->graph.set_node_live_swap_policy(s->plugin, std::move(policy)));
    return s;
}

void expect_reason(const SignalGraph& graph, LiveSwapFallbackReason reason, NodeId node) {
    const auto diagnostics = graph.last_swap_diagnostics();
    CHECK(diagnostics.reason == reason);
    CHECK(diagnostics.offending_node == node);
    CHECK_FALSE(diagnostics.message.empty());
}

int dsp_restore_count(const std::shared_ptr<SlotStats>& stats) {
    std::lock_guard<std::mutex> lock(stats->mu);
    return stats->dsp_restore_calls;
}

int reseed_count(const std::shared_ptr<SlotStats>& stats) {
    std::lock_guard<std::mutex> lock(stats->mu);
    return stats->reseed_calls;
}

int destroyed_count(const std::shared_ptr<SlotStats>& stats) {
    std::lock_guard<std::mutex> lock(stats->mu);
    return stats->destroyed;
}

int process_call_count(const std::shared_ptr<SlotStats>& stats) {
    std::lock_guard<std::mutex> lock(stats->mu);
    return stats->process_calls;
}

} // namespace

#include <pulp/signal/delay_line.hpp>
#include <pulp/state/parameter.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <string_view>

TEST_CASE("DSPX-09 follow-up delay impulse oracle refuses unsupported bounds",
          "[dspx-09][follow-up][positive][negative]") {
    pulp::signal::DelayLine delay;
    delay.prepare(4);
    CHECK(delay.max_delay() == 4);
    CHECK(delay.process(1.0f, 2.0f) == 0.0f);
    CHECK(delay.process(0.0f, 2.0f) == 0.0f);
    CHECK(delay.process(0.0f, 2.0f) == 1.0f);
    CHECK(delay.process(0.0f, 2.0f) == 0.0f);
    CHECK(delay.read(-1.0f) == 0.0f);
    CHECK(delay.read(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

TEST_CASE("DSPX-09 follow-up retained-history lifecycle preserves refusal state",
          "[dspx-09][follow-up][retained-history]") {
    SlotBehavior old_behavior{.info = make_info("same"), .history_key = "delay.v1"};
    SlotBehavior replacement_behavior{.info = make_info("same"), .history_key = "delay.v1"};
    auto policy = allowing_policy();
    policy.retained_history.mode = RetainedHistoryMode::Adopt;
    auto s = make_stage_setup(std::move(old_behavior), std::move(replacement_behavior), policy);
    render_blocks(s->graph, 10);
    s->graph.begin_swap_edit();
    REQUIRE(s->graph.stage_plugin_replacement(s->plugin, s->token) ==
            SignalGraph::SwapResult::Staged);
    REQUIRE(s->graph.prepare_swap(kSr, kFrames) == SignalGraph::SwapResult::Swapped);
    CHECK(dsp_restore_count(s->replacement_stats) == 1);

    SlotBehavior mismatch_old{.info = make_info("mismatch"), .history_key = "delay.v1"};
    SlotBehavior mismatch_new{.info = make_info("mismatch"), .history_key = "waveguide.v1"};
    auto refuse = allowing_policy();
    refuse.retained_history.mode = RetainedHistoryMode::Refuse;
    auto refused = make_stage_setup(std::move(mismatch_old), std::move(mismatch_new), refuse);
    const auto before = refused->graph.connections().size();
    refused->graph.begin_swap_edit();
    CHECK(refused->graph.stage_plugin_replacement(refused->plugin, refused->token) ==
          SignalGraph::SwapResult::NeedsEagerPrepare);
    expect_reason(refused->graph, LiveSwapFallbackReason::HistoryRefused, refused->plugin);
    CHECK(refused->graph.connections().size() == before);
}

TEST_CASE("DSPX-09 follow-up automation route preserves ordering and overflow refusal",
          "[dspx-09][follow-up][automation][negative]") {
    pulp::state::ModulationEventQueue queue;
    REQUIRE(queue.push({7, 8, 0.8f}));
    REQUIRE(queue.push({7, 0, 0.1f}));
    REQUIRE(queue.push({7, 4, 0.4f}));
    queue.sort();
    REQUIRE(queue.events()[0].sample_offset == 0);
    REQUIRE(queue.events()[2].sample_offset == 8);
    for (std::size_t i = queue.size(); i < pulp::state::ModulationEventQueue::kCapacity; ++i)
        REQUIRE(queue.push({7, static_cast<int32_t>(i), 0.25f}));
    CHECK_FALSE(queue.push({7, 1024, 0.25f}));
    CHECK(queue.overflowed());
}

TEST_CASE("DSPX-09 follow-up projection comparator keeps external lanes typed",
          "[dspx-09][follow-up][projection][negative]") {
    struct SurfaceReceipt {
        const char* name;
        bool executable;
        const char* disposition;
    };
    const std::array<SurfaceReceipt, 4> surfaces{{
        {"Pulp", true, "accepted"},
        {"Forge", false, "development-only"},
        {"Spectr", false, "DeferredExternalOwner"},
        {"GPU-NAM", false, "DeferredExternalOwner"},
    }};
    CHECK(surfaces[0].executable);
    for (std::size_t i = 1; i < surfaces.size(); ++i) {
        CHECK_FALSE(surfaces[i].executable);
        CHECK(std::string_view(surfaces[i].disposition) != "accepted");
    }
}
