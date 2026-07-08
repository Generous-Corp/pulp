#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph.hpp>

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
};

class StagingSlot final : public PluginSlot {
public:
    StagingSlot(SlotBehavior behavior, std::shared_ptr<SlotStats> stats)
        : behavior_(std::move(behavior)), stats_(std::move(stats)) {
        for (const auto& p : behavior_.params) params_[p.id] = p.default_value;
        current_state_ = behavior_.state;
    }

    ~StagingSlot() override {
        std::lock_guard<std::mutex> lock(stats_->mu);
        ++stats_->destroyed;
        stats_->destroy_thread = std::this_thread::get_id();
    }

    const PluginInfo& info() const override { return behavior_.info; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return behavior_.prepare_ok; }
    void release() override {}

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&,
                 int n) override {
        {
            std::lock_guard<std::mutex> lock(stats_->mu);
            ++stats_->process_calls;
            stats_->process_threads.push_back(std::this_thread::get_id());
        }
        if (behavior_.block_process ||
            stats_->block_process.load(std::memory_order_acquire)) {
            stats_->process_entered.store(true, std::memory_order_release);
            while (!stats_->release_process.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
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
            if (!params_.empty()) gain = params_.begin()->second;
        }

        const std::size_t copied = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < copied; ++c) {
            const float* src = in.channel_ptr(c);
            float* dst = out.channel_ptr(c);
            for (int i = 0; i < n; ++i) dst[static_cast<std::size_t>(i)] =
                src[static_cast<std::size_t>(i)] * gain;
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
    bool is_bypassed() const override { return false; }

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

    int latency_samples() const override { return behavior_.latency; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

private:
    SlotBehavior behavior_;
    std::shared_ptr<SlotStats> stats_;
    mutable std::mutex param_mu_;
    std::unordered_map<std::uint32_t, float> params_;
    mutable std::mutex state_mu_;
    std::vector<std::uint8_t> current_state_;
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
    for (int i = 0; i < blocks; ++i) graph.process(ov, iv, kFrames);
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
    SignalGraph::PluginCatalogToken token;
};

std::unique_ptr<StageSetup>
make_stage_setup(SlotBehavior old_behavior,
                 SlotBehavior replacement_behavior,
                 NodeLiveSwapPolicy policy = allowing_policy()) {
    auto s = std::make_unique<StageSetup>();
    s->info = old_behavior.info;
    s->old_stats = std::make_shared<SlotStats>();
    const auto in = s->graph.add_input_node(2, "In");
    s->plugin = s->graph.add_plugin_node(
        std::make_unique<StagingSlot>(std::move(old_behavior), s->old_stats),
        2,
        2,
        "P");
    const auto out = s->graph.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(s->graph.connect(in, c, s->plugin, c));
        REQUIRE(s->graph.connect(s->plugin, c, out, c));
    }
    REQUIRE(s->graph.prepare(kSr, kFrames));

    s->replacement_stats = std::make_shared<SlotStats>();
    s->token = s->graph.register_scanned_plugin(replacement_behavior.info);
    s->graph.set_live_swap_plugin_loader_for_test(
        [replacement_behavior = std::move(replacement_behavior),
         stats = s->replacement_stats](const PluginInfo&) mutable {
            return std::make_unique<StagingSlot>(replacement_behavior, stats);
        });
    REQUIRE(s->graph.set_node_live_swap_policy(s->plugin, std::move(policy)));
    return s;
}

void expect_reason(const SignalGraph& graph,
                   LiveSwapFallbackReason reason,
                   NodeId node) {
    const auto diagnostics = graph.last_swap_diagnostics();
    CHECK(diagnostics.reason == reason);
    CHECK(diagnostics.offending_node == node);
    CHECK_FALSE(diagnostics.message.empty());
}

int destroyed_count(const std::shared_ptr<SlotStats>& stats) {
    std::lock_guard<std::mutex> lock(stats->mu);
    return stats->destroyed;
}

int process_call_count(const std::shared_ptr<SlotStats>& stats) {
    std::lock_guard<std::mutex> lock(stats->mu);
    return stats->process_calls;
}

bool all_process_calls_on_thread(const std::shared_ptr<SlotStats>& stats,
                                 std::thread::id thread) {
    std::lock_guard<std::mutex> lock(stats->mu);
    return !stats->process_threads.empty() &&
        std::all_of(stats->process_threads.begin(),
                    stats->process_threads.end(),
                    [&](std::thread::id seen) { return seen == thread; });
}

}  // namespace

TEST_CASE("live plugin swap staging restores state and resyncs parameters",
          "[host][graph][live-swap]") {
    SlotBehavior old_behavior{.info = make_info("same")};
    SlotBehavior replacement_behavior{.info = make_info("same")};
    auto s = make_stage_setup(std::move(old_behavior),
                              std::move(replacement_behavior));
    render_blocks(s->graph, 10);

    std::shared_ptr<PluginSlot> observed_old;
    std::shared_ptr<PluginSlot> observed_new;
    NodeLiveSwapPolicy policy = allowing_policy();
    policy.on_instance_swapped =
        [&](NodeId id,
            std::shared_ptr<PluginSlot> old_slot,
            std::shared_ptr<PluginSlot> new_slot) {
            CHECK(id == s->plugin);
            observed_old = std::move(old_slot);
            observed_new = std::move(new_slot);
            CHECK(s->graph.set_node_parameter(s->plugin, 7, 0.5f));
        };
    REQUIRE(s->graph.set_node_live_swap_policy(s->plugin, policy));

    s->graph.begin_swap_edit();
    REQUIRE(s->graph.stage_plugin_replacement(s->plugin, s->token)
            == SignalGraph::SwapResult::Staged);
    REQUIRE(s->graph.set_node_parameter(s->plugin, 7, 0.66f));
    REQUIRE(s->graph.prepare_swap(kSr, kFrames)
            == SignalGraph::SwapResult::Swapped);

    REQUIRE(observed_old);
    REQUIRE(observed_new);
    auto* new_slot = dynamic_cast<StagingSlot*>(observed_new.get());
    REQUIRE(new_slot != nullptr);
    CHECK(new_slot->get_parameter(7) == 0.5f);
    {
        std::lock_guard<std::mutex> lock(s->replacement_stats->mu);
        CHECK(s->replacement_stats->restore_calls == 1);
        CHECK(s->replacement_stats->restored_state == std::vector<uint8_t>{1, 2, 3});
    }
    CHECK(s->graph.last_swap_diagnostics().reason
          == LiveSwapFallbackReason::None);
}

TEST_CASE("live plugin swap staging commits a warmed different identity",
          "[host][graph][live-swap]") {
    SlotBehavior old_behavior{.info = make_info("old")};
    SlotBehavior replacement_behavior{.info = make_info("new")};
    auto s = make_stage_setup(std::move(old_behavior),
                              std::move(replacement_behavior));
    render_blocks(s->graph, 10);

    std::shared_ptr<PluginSlot> observed_new;
    NodeLiveSwapPolicy policy = allowing_policy();
    policy.on_instance_swapped =
        [&](NodeId,
            std::shared_ptr<PluginSlot>,
            std::shared_ptr<PluginSlot> new_slot) {
            observed_new = std::move(new_slot);
        };
    REQUIRE(s->graph.set_node_live_swap_policy(s->plugin, policy));

    s->graph.begin_swap_edit();
    REQUIRE(s->graph.stage_plugin_replacement(s->plugin, s->token)
            == SignalGraph::SwapResult::Staged);
    CHECK(s->graph.prepare_swap(kSr, kFrames)
          == SignalGraph::SwapResult::Swapped);
    REQUIRE(observed_new);
    CHECK(observed_new->info().unique_id == "new");
    CHECK(s->graph.last_swap_diagnostics().reason
          == LiveSwapFallbackReason::None);
}

TEST_CASE("live plugin swap staging rejects warmed different identity over budget",
          "[host][graph][live-swap]") {
    SlotBehavior old_behavior{.info = make_info("old")};
    SlotBehavior replacement_behavior{
        .info = make_info("new"),
        .process_sleep = std::chrono::milliseconds(3),
    };
    NodeLiveSwapPolicy policy = allowing_policy();
    policy.headroom_threshold = 0.05f;
    auto s = make_stage_setup(std::move(old_behavior),
                              std::move(replacement_behavior),
                              policy);
    render_blocks(s->graph, 10);

    s->graph.begin_swap_edit();
    REQUIRE(s->graph.stage_plugin_replacement(s->plugin, s->token)
            == SignalGraph::SwapResult::Staged);
    CHECK(process_call_count(s->replacement_stats) >= 8);
    CHECK(s->graph.prepare_swap(kSr, kFrames)
          == SignalGraph::SwapResult::NeedsEagerPrepare);
    expect_reason(s->graph,
                  LiveSwapFallbackReason::OverBudget,
                  s->plugin);
    CHECK(destroyed_count(s->replacement_stats) == 1);
}

TEST_CASE("live plugin swap staging restores state after warm residue",
          "[host][graph][live-swap]") {
    const std::vector<std::uint8_t> restored_state{42, 7, 3};
    const std::vector<std::uint8_t> warm_residue{9, 9, 9};
    SlotBehavior old_behavior{.info = make_info("old"),
                              .state = restored_state};
    SlotBehavior replacement_behavior{.info = make_info("new"),
                                      .process_state_tag = warm_residue};
    auto s = make_stage_setup(std::move(old_behavior),
                              std::move(replacement_behavior));
    render_blocks(s->graph, 10);

    std::shared_ptr<PluginSlot> observed_new;
    NodeLiveSwapPolicy policy = allowing_policy();
    policy.on_instance_swapped =
        [&](NodeId,
            std::shared_ptr<PluginSlot>,
            std::shared_ptr<PluginSlot> new_slot) {
            observed_new = std::move(new_slot);
        };
    REQUIRE(s->graph.set_node_live_swap_policy(s->plugin, policy));

    s->graph.begin_swap_edit();
    REQUIRE(s->graph.stage_plugin_replacement(s->plugin, s->token)
            == SignalGraph::SwapResult::Staged);
    CHECK(process_call_count(s->replacement_stats) >= 8);
    CHECK(s->graph.prepare_swap(kSr, kFrames)
          == SignalGraph::SwapResult::Swapped);
    REQUIRE(observed_new);
    CHECK(observed_new->save_state() == restored_state);
    CHECK(observed_new->save_state() != warm_residue);
    {
        std::lock_guard<std::mutex> lock(s->replacement_stats->mu);
        CHECK(s->replacement_stats->restore_calls == 1);
        CHECK(s->replacement_stats->restored_state == restored_state);
    }
}

TEST_CASE("live plugin swap staging pre-warms enough callbacks on the control thread",
          "[host][graph][live-swap][rt-safety]") {
    SlotBehavior old_behavior{.info = make_info("old")};
    SlotBehavior replacement_behavior{.info = make_info("new")};
    auto s = make_stage_setup(std::move(old_behavior),
                              std::move(replacement_behavior));
    render_blocks(s->graph, 10);

    const auto staging_thread = std::this_thread::get_id();
    s->graph.begin_swap_edit();
    REQUIRE(s->graph.stage_plugin_replacement(s->plugin, s->token)
            == SignalGraph::SwapResult::Staged);

    // Stage runs on this control thread; candidate warm-up is not dispatched
    // through graph.process().
    CHECK(process_call_count(s->replacement_stats) >= 8);
    CHECK(all_process_calls_on_thread(s->replacement_stats, staging_thread));
    CHECK(s->graph.prepare_swap(kSr, kFrames)
          == SignalGraph::SwapResult::Swapped);
}

TEST_CASE("live plugin swap staging rejects default-off nodes",
          "[host][graph][live-swap]") {
    SlotBehavior old_behavior{.info = make_info("same")};
    SlotBehavior replacement_behavior{.info = make_info("same")};
    auto s = make_stage_setup(std::move(old_behavior),
                              std::move(replacement_behavior),
                              NodeLiveSwapPolicy{});

    s->graph.begin_swap_edit();
    CHECK(s->graph.stage_plugin_replacement(s->plugin, s->token)
          == SignalGraph::SwapResult::NeedsEagerPrepare);
    expect_reason(s->graph,
                  LiveSwapFallbackReason::NotOptedIn,
                  s->plugin);
    CHECK(destroyed_count(s->replacement_stats) == 0);
}

TEST_CASE("live plugin swap staging reports refusal reasons and releases candidates",
          "[host][graph][live-swap]") {
    SECTION("unknown catalog token") {
        SlotBehavior old_behavior{.info = make_info("same")};
        SlotBehavior replacement_behavior{.info = make_info("same")};
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior));
        s->graph.begin_swap_edit();
        CHECK(s->graph.stage_plugin_replacement(
                  s->plugin,
                  SignalGraph::PluginCatalogToken{99999})
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::UntrustedIdentity,
                      s->plugin);
        CHECK(destroyed_count(s->replacement_stats) == 0);
    }

    SECTION("load failed") {
        SlotBehavior old_behavior{.info = make_info("same")};
        SlotBehavior replacement_behavior{.info = make_info("same")};
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior));
        s->graph.set_live_swap_plugin_loader_for_test(
            [](const PluginInfo&) -> std::unique_ptr<PluginSlot> {
                return nullptr;
            });
        s->graph.begin_swap_edit();
        CHECK(s->graph.stage_plugin_replacement(s->plugin, s->token)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::LoadFailed,
                      s->plugin);
    }

    SECTION("prepare failed") {
        SlotBehavior old_behavior{.info = make_info("same")};
        SlotBehavior replacement_behavior{.info = make_info("same"),
                                          .prepare_ok = false};
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior));
        s->graph.begin_swap_edit();
        CHECK(s->graph.stage_plugin_replacement(s->plugin, s->token)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::PrepareFailed,
                      s->plugin);
        CHECK(destroyed_count(s->replacement_stats) == 1);
    }

    SECTION("state too large") {
        SlotBehavior old_behavior{.info = make_info("same"),
                                  .state = {1, 2, 3, 4}};
        SlotBehavior replacement_behavior{.info = make_info("same")};
        NodeLiveSwapPolicy policy = allowing_policy();
        policy.max_state_bytes = 2;
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior),
                                  policy);
        s->graph.begin_swap_edit();
        CHECK(s->graph.stage_plugin_replacement(s->plugin, s->token)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::StateTooLarge,
                      s->plugin);
        CHECK(destroyed_count(s->replacement_stats) == 1);
    }

    SECTION("restore failed") {
        SlotBehavior old_behavior{.info = make_info("same")};
        SlotBehavior replacement_behavior{.info = make_info("same"),
                                          .restore_ok = false};
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior));
        s->graph.begin_swap_edit();
        CHECK(s->graph.stage_plugin_replacement(s->plugin, s->token)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::StateRestoreFailed,
                      s->plugin);
        CHECK(destroyed_count(s->replacement_stats) == 1);
    }

    SECTION("shape mismatch") {
        SlotBehavior old_behavior{.info = make_info("same")};
        SlotBehavior replacement_behavior{.info = make_info("same", 1, 2)};
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior));
        s->graph.begin_swap_edit();
        CHECK(s->graph.stage_plugin_replacement(s->plugin, s->token)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::ShapeMismatch,
                      s->plugin);
        CHECK(destroyed_count(s->replacement_stats) == 1);
    }

    SECTION("latency changed") {
        SlotBehavior old_behavior{.info = make_info("same")};
        SlotBehavior replacement_behavior{.info = make_info("same"),
                                          .latency = 32};
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior));
        s->graph.begin_swap_edit();
        CHECK(s->graph.stage_plugin_replacement(s->plugin, s->token)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::LatencyChanged,
                      s->plugin);
        CHECK(destroyed_count(s->replacement_stats) == 1);
    }

    SECTION("parameter contract mismatch") {
        SlotBehavior old_behavior{.info = make_info("old")};
        SlotBehavior replacement_behavior{.info = make_info("new"),
                                          .params = {make_param(99)}};
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior));
        s->graph.begin_swap_edit();
        CHECK(s->graph.stage_plugin_replacement(s->plugin, s->token)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::ParamContractMismatch,
                      s->plugin);
        CHECK(destroyed_count(s->replacement_stats) == 1);
    }

    SECTION("hosted editor open") {
        SlotBehavior old_behavior{.info = make_info("same")};
        SlotBehavior replacement_behavior{.info = make_info("same")};
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior));
        REQUIRE(s->graph.set_node_hosted_editor_open(s->plugin, true));
        s->graph.begin_swap_edit();
        CHECK(s->graph.stage_plugin_replacement(s->plugin, s->token)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::EditorOpen,
                      s->plugin);
        CHECK(destroyed_count(s->replacement_stats) == 0);
    }

    SECTION("feedback edge") {
        SlotBehavior old_behavior{.info = make_info("same")};
        SlotBehavior replacement_behavior{.info = make_info("same")};
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior));
        REQUIRE(s->graph.connect_feedback(s->plugin, 0,
                                                 s->plugin, 0));
        REQUIRE(s->graph.prepare(kSr, kFrames));
        s->graph.begin_swap_edit();
        CHECK(s->graph.stage_plugin_replacement(s->plugin, s->token)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::FeedbackNotSwappable,
                      s->plugin);
        CHECK(destroyed_count(s->replacement_stats) == 0);
    }
}

TEST_CASE("live plugin swap prepare fallback releases staged replacements",
          "[host][graph][live-swap]") {
    SECTION("no load history") {
        SlotBehavior old_behavior{.info = make_info("same")};
        SlotBehavior replacement_behavior{.info = make_info("same")};
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior));
        s->graph.begin_swap_edit();
        REQUIRE(s->graph.stage_plugin_replacement(s->plugin, s->token)
                == SignalGraph::SwapResult::Staged);
        CHECK(s->graph.prepare_swap(kSr, kFrames)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::NoLoadHistory,
                      s->plugin);
        CHECK(destroyed_count(s->replacement_stats) == 1);
    }

    SECTION("over budget") {
        SlotBehavior old_behavior{.info = make_info("same"),
                                  .process_sleep = std::chrono::microseconds(500)};
        SlotBehavior replacement_behavior{.info = make_info("same")};
        NodeLiveSwapPolicy policy = allowing_policy();
        policy.headroom_threshold = 0.0f;
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior),
                                  policy);
        render_blocks(s->graph, 10);
        s->graph.begin_swap_edit();
        REQUIRE(s->graph.stage_plugin_replacement(s->plugin, s->token)
                == SignalGraph::SwapResult::Staged);
        CHECK(s->graph.prepare_swap(kSr, kFrames)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::OverBudget,
                      s->plugin);
        CHECK(destroyed_count(s->replacement_stats) == 1);
    }

    SECTION("predicate excluded") {
        SlotBehavior old_behavior{.info = make_info("same")};
        SlotBehavior replacement_behavior{.info = make_info("same")};
        auto s = make_stage_setup(std::move(old_behavior),
                                  std::move(replacement_behavior));
        render_blocks(s->graph, 10);
        s->graph.begin_swap_edit();
        REQUIRE(s->graph.stage_plugin_replacement(s->plugin, s->token)
                == SignalGraph::SwapResult::Staged);
        const auto mi = s->graph.add_midi_input_node("MI");
        const auto mo = s->graph.add_midi_output_node("MO");
        REQUIRE(s->graph.connect_midi(mi, mo));
        CHECK(s->graph.prepare_swap(kSr, kFrames)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
        expect_reason(s->graph,
                      LiveSwapFallbackReason::PredicateExcluded,
                      0);
        CHECK(destroyed_count(s->replacement_stats) == 1);
    }
}

TEST_CASE("live plugin swap abort releases staged replacements",
          "[host][graph][live-swap]") {
    SlotBehavior old_behavior{.info = make_info("same")};
    SlotBehavior replacement_behavior{.info = make_info("same")};
    auto s = make_stage_setup(std::move(old_behavior),
                              std::move(replacement_behavior));
    s->graph.begin_swap_edit();
    REQUIRE(s->graph.stage_plugin_replacement(s->plugin, s->token)
            == SignalGraph::SwapResult::Staged);
    s->graph.abort_swap_edit();
    CHECK(destroyed_count(s->replacement_stats) == 1);
}

TEST_CASE("live plugin swap retires the old slot only after reader drain",
          "[host][graph][live-swap][rt-safety]") {
    auto old_stats = std::make_shared<SlotStats>();
    SlotBehavior old_behavior{.info = make_info("same")};
    SlotBehavior replacement_behavior{.info = make_info("same")};

    SignalGraph graph;
    const auto in = graph.add_input_node(2, "In");
    const auto plugin = graph.add_plugin_node(
        std::make_unique<StagingSlot>(old_behavior, old_stats),
        2,
        2,
        "P");
    const auto out = graph.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(graph.connect(in, c, plugin, c));
        REQUIRE(graph.connect(plugin, c, out, c));
    }
    REQUIRE(graph.prepare(kSr, kFrames));
    render_blocks(graph, 10);

    auto replacement_stats = std::make_shared<SlotStats>();
    const auto token = graph.register_scanned_plugin(replacement_behavior.info);
    graph.set_live_swap_plugin_loader_for_test(
        [replacement_behavior, replacement_stats](const PluginInfo&) {
            return std::make_unique<StagingSlot>(replacement_behavior,
                                                 replacement_stats);
        });
    REQUIRE(graph.set_node_live_swap_policy(plugin, allowing_policy()));

    old_stats->block_process.store(true, std::memory_order_release);
    std::thread audio([&] { render_blocks(graph, 1); });
    while (!old_stats->process_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    graph.begin_swap_edit();
    REQUIRE(graph.stage_plugin_replacement(plugin, token)
            == SignalGraph::SwapResult::Staged);
    CHECK(graph.prepare_swap(kSr, kFrames) == SignalGraph::SwapResult::Swapped);
    CHECK(destroyed_count(old_stats) == 0);

    old_stats->release_process.store(true, std::memory_order_release);
    audio.join();
    const auto control_thread = std::this_thread::get_id();
    graph.release();
    {
        std::lock_guard<std::mutex> lock(old_stats->mu);
        CHECK(old_stats->destroyed == 1);
        CHECK(old_stats->destroy_thread == control_thread);
    }
}
