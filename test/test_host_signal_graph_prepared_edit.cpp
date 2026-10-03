// SignalGraph prepared topology and Processor-node lifecycle contracts.
// SignalGraph tests live separately from test_host.cpp so the host test surface
// stays reviewable. Self-contained: uses
// SignalGraph from pulp/host/signal_graph.hpp (in the shared host includes) and
// carries its own interleaved helper namespaces.
#include "harness/rt_allocation_probe.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>

#include "support/thread_progress.hpp"
#if defined(__unix__) || defined(__APPLE__)
#include "native_components/rt_test_scope.hpp"
#endif

#include "support/thread_progress.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pulp::host;
using Catch::Matchers::WithinAbs;

namespace {
PluginInfo make_plugin_info(std::string name,
                            int num_inputs = 0,
                            int num_outputs = 0,
                            std::string category = "Fx") {
    PluginInfo info{};
    info.name = std::move(name);
    info.format = PluginFormat::CLAP;
    info.num_inputs = num_inputs;
    info.num_outputs = num_outputs;
    info.category = std::move(category);
    return info;
}
} // namespace

namespace {
class MockLatencyPlugin final : public PluginSlot {
public:
    MockLatencyPlugin(int latency, int num_ch)
        : latency_(latency), num_ch_(num_ch) {
        info_.name = "MockLatency";
        info_.num_inputs = num_ch;
        info_.num_outputs = num_ch;
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue& /*pe*/,
                 int n) override {
        // Actually delay the audio by the reported latency samples so PDC
        // behavior can be measured end-to-end: the plugin's internal delay
        // line and the host's reported-latency view agree.
        if (rings_.size() != (size_t)num_ch_) {
            rings_.assign((size_t)num_ch_,
                          std::vector<float>((size_t)std::max(1, latency_ + 1), 0.f));
            wp_ = 0;
        }
        for (int c = 0; c < num_ch_ && (size_t)c < out.num_channels(); ++c) {
            const float* s = (size_t)c < in.num_channels() ? in.channel_ptr((size_t)c) : nullptr;
            float* d = out.channel_ptr((size_t)c);
            if (latency_ <= 0) {
                if (s) std::memcpy(d, s, sizeof(float) * (size_t)n);
                else std::memset(d, 0, sizeof(float) * (size_t)n);
                continue;
            }
            const int ring_size = (int)rings_[(size_t)c].size();
            int wp = wp_;
            int rp = wp - latency_;
            if (rp < 0) rp += ring_size;
            for (int i = 0; i < n; ++i) {
                rings_[(size_t)c][(size_t)wp] = s ? s[i] : 0.f;
                d[i] = rings_[(size_t)c][(size_t)rp];
                if (++wp == ring_size) wp = 0;
                if (++rp == ring_size) rp = 0;
            }
        }
        // Advance write pointer by one block (shared across channels).
        wp_ = (wp_ + n) % (int)std::max<size_t>(1, rings_.empty() ? 1 : rings_[0].size());
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return latency_; }
    int tail_samples() const override { return 0; }
private:
    PluginInfo info_;
    int latency_ = 0;
    int num_ch_ = 2;
    std::vector<std::vector<float>> rings_;
    int wp_ = 0;
};
class MidiForwarder final : public PluginSlot {
public:
    MidiForwarder() { last_seen_.attach_ump(&last_seen_ump_); }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::host::ParameterEventQueue& /*pe*/,
                 int n) override {
        last_seen_.clear();
        last_seen_.clear_sysex();
        last_seen_ump_.clear();
        for (const auto& ev : midi_in) {
            last_seen_.add(ev);
            midi_out.add(ev);
        }
        for (const auto& sx : midi_in.sysex()) {
            last_seen_.add_sysex_copy(sx.data.data(), sx.data.size(),
                                      sx.sample_offset, sx.timestamp);
            midi_out.add_sysex_copy(sx.data.data(), sx.data.size(),
                                    sx.sample_offset, sx.timestamp);
        }
        if (const auto* in_ump = midi_in.ump()) {
            auto* out_ump = midi_out.ump();
            for (const auto& ev : *in_ump) {
                last_seen_ump_.add(ev);
                if (out_ump) out_ump->add(ev);
            }
        }
        for (size_t c = 0; c < out.num_channels(); ++c) {
            std::memset(out.channel_ptr(c), 0, sizeof(float) * (size_t)n);
        }
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
    const pulp::midi::MidiBuffer& last_seen() const { return last_seen_; }
    const pulp::midi::UmpBuffer& last_seen_ump() const { return last_seen_ump_; }
private:
    PluginInfo info_ = make_plugin_info("MidiFwd", 0, 0, "MidiEffect");
    pulp::midi::MidiBuffer last_seen_;
    pulp::midi::UmpBuffer last_seen_ump_;
};

class MidiFlooder final : public PluginSlot {
public:
    explicit MidiFlooder(std::size_t event_count) : event_count_(event_count) {}
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::host::ParameterEventQueue&,
                 int n) override {
        const auto block = static_cast<std::size_t>(std::max(1, n));
        for (std::size_t i = 0; i < event_count_; ++i) {
            auto ev = pulp::midi::MidiEvent::note_on(
                0, static_cast<int>(i % 127), 100);
            ev.sample_offset = static_cast<int32_t>(i % block);
            midi_out.add(ev);
        }
        for (size_t c = 0; c < out.num_channels(); ++c) {
            std::memset(out.channel_ptr(c), 0, sizeof(float) * static_cast<size_t>(n));
        }
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
private:
    PluginInfo info_ = make_plugin_info("MidiFlood", 0, 0, "MidiGenerator");
    std::size_t event_count_ = 0;
};
class ParameterMailboxProbe final : public PluginSlot {
public:
    static constexpr uint32_t kParamId = 42;

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue& pe,
                 int n) override {
        received_count_ = 0;
        overflowed_ = pe.overflowed();
        dropped_ = pe.dropped_event_count();
        for (const auto& event : pe) {
            if (received_count_ < received_.size()) {
                received_[received_count_++] = event;
            }
        }
        for (size_t c = 0; c < out.num_channels(); ++c) {
            std::memset(out.channel_ptr(c), 0, sizeof(float) * (size_t)n);
        }
    }

    std::vector<HostParamInfo> parameters() const override {
        HostParamInfo p;
        p.id = kParamId;
        p.name = "mailbox";
        p.min_value = 0.0f;
        p.max_value = 1.0f;
        p.default_value = 0.0f;
        p.flags.automatable = true;
        p.flags.modulatable = true;
        p.rate = ParamRate::AudioRate;
        return {p};
    }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

    std::size_t received_count() const { return received_count_; }
    const pulp::host::ParameterEvent& received(std::size_t index) const {
        return received_[index];
    }
    bool overflowed() const { return overflowed_; }
    std::uint32_t dropped() const { return dropped_; }

private:
    PluginInfo info_ = make_plugin_info("ParameterMailboxProbe", 1, 1);
    std::array<pulp::host::ParameterEvent,
               pulp::host::ParameterEventQueue::kCapacity> received_{};
    std::size_t received_count_ = 0;
    bool overflowed_ = false;
    std::uint32_t dropped_ = 0;
};
} // namespace

namespace {
struct PreparedEditLevel {
    float level = 1.0f;
};

CustomNodeType make_prepared_edit_level_type(
    std::string id, float level, std::atomic<int>* creates = nullptr,
    std::atomic<int>* prepares = nullptr, std::atomic<int>* destroys = nullptr,
    bool fail_create = false, std::atomic<bool>* process_entered = nullptr,
    std::atomic<bool>* allow_process_exit = nullptr,
    std::atomic<bool>* process_exited = nullptr) {
    CustomNodeType type;
    type.type_id = std::move(id);
    type.version = 1;
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.default_name = "Prepared edit level";
    type.create = [level, creates, fail_create]() -> void* {
        if (creates) creates->fetch_add(1, std::memory_order_relaxed);
        return fail_create ? nullptr : static_cast<void*>(new PreparedEditLevel{level});
    };
    type.destroy = [destroys](void* raw) {
        if (destroys) destroys->fetch_add(1, std::memory_order_relaxed);
        delete static_cast<PreparedEditLevel*>(raw);
    };
    type.prepare = [prepares](void*, double, int) {
        if (prepares) prepares->fetch_add(1, std::memory_order_relaxed);
    };
    type.process_instance = [process_entered, allow_process_exit, process_exited](
        void* raw, pulp::audio::BufferView<float>& output,
        const pulp::audio::BufferView<const float>& input, int frames) {
        if (process_entered)
            process_entered->store(true, std::memory_order_release);
        while (allow_process_exit && !allow_process_exit->load(std::memory_order_acquire))
            std::this_thread::yield();
        const float level = static_cast<PreparedEditLevel*>(raw)->level;
        for (int i = 0; i < frames; ++i) {
            output.channel_ptr(0)[i] = input.channel_ptr(0)[i] * level;
        }
        if (process_exited)
            process_exited->store(true, std::memory_order_release);
    };
    return type;
}

class PreparedEditCountingPlugin final : public PluginSlot {
public:
    explicit PreparedEditCountingPlugin(std::atomic<int>& prepare_calls,
                                        std::atomic<int>* release_calls = nullptr,
                                        bool prepare_result = true)
        : prepare_calls_(prepare_calls), release_calls_(release_calls),
          prepare_result_(prepare_result) {
        info_.name = "PreparedEditCountingPlugin";
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override {
        prepare_calls_.fetch_add(1, std::memory_order_relaxed);
        return prepare_result_;
    }
    void release() override {
        if (release_calls_)
            release_calls_->fetch_add(1, std::memory_order_relaxed);
    }
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&, int frames) override {
        for (int i = 0; i < frames; ++i) {
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
        }
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

private:
    std::atomic<int>& prepare_calls_;
    std::atomic<int>* release_calls_ = nullptr;
    bool prepare_result_ = true;
    PluginInfo info_;
};

class PreparedEditOwnedBuiltInPlugin final : public PluginSlot {
public:
    PreparedEditOwnedBuiltInPlugin(std::atomic<int>& prepare_calls,
                                   std::atomic<int>& release_calls,
                                   std::atomic<int>& destroy_calls,
                                   bool prepare_result = true,
                                   float level = 1.0f)
        : prepare_calls_(prepare_calls), release_calls_(release_calls),
          destroy_calls_(destroy_calls), prepare_result_(prepare_result),
          level_(level) {
        info_.name = "PreparedEditOwnedBuiltInPlugin";
        info_.format = PluginFormat::BuiltIn;
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }
    ~PreparedEditOwnedBuiltInPlugin() override {
        destroy_calls_.fetch_add(1, std::memory_order_relaxed);
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override {
        prepare_calls_.fetch_add(1, std::memory_order_relaxed);
        return prepare_result_;
    }
    void release() override {
        release_calls_.fetch_add(1, std::memory_order_relaxed);
    }
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&, int frames) override {
        for (int i = 0; i < frames; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i] * level_;
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

private:
    std::atomic<int>& prepare_calls_;
    std::atomic<int>& release_calls_;
    std::atomic<int>& destroy_calls_;
    bool prepare_result_ = true;
    float level_ = 1.0f;
    PluginInfo info_;
};

class PreparedEditDimensionPlugin final : public PluginSlot {
public:
    PreparedEditDimensionPlugin() {
        info_.name = "PreparedEditDimensionPlugin";
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double sample_rate, int max_block_size) override {
        if (fail_sample_rate.load(std::memory_order_relaxed) == sample_rate)
            return false;
        prepared_sample_rate.store(sample_rate, std::memory_order_relaxed);
        prepared_max_block.store(max_block_size, std::memory_order_relaxed);
        level_.store(sample_rate == 48'000.0 && max_block_size == 64 ? 1.0f : 2.0f,
                     std::memory_order_relaxed);
        return true;
    }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&, int frames) override {
        const float level = level_.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i] * level;
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

    std::atomic<double> prepared_sample_rate{0.0};
    std::atomic<int> prepared_max_block{0};
    std::atomic<double> fail_sample_rate{0.0};

private:
    std::atomic<float> level_{0.0f};
    PluginInfo info_;
};
} // namespace

TEST_CASE("SignalGraph prepared edit keeps owned built-in additions candidate-local until commit",
          "[host][graph][prepared-edit][plugin][builtin]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> prepares{0};
    std::atomic<int> releases{0};
    std::atomic<int> destroys{0};
    SignalGraph graph;

    auto edit = graph.begin_prepared_topology_edit();
    const auto input = edit->add_input_node(1, "input");
    const auto plugin = edit->add_owned_builtin_plugin_node(
        std::make_unique<PreparedEditOwnedBuiltInPlugin>(prepares, releases, destroys),
        1, 1, "owned built-in");
    const auto output = edit->add_output_node(1, "output");
    REQUIRE(plugin != 0);
    REQUIRE(edit->connect(input, 0, plugin, 0));
    REQUIRE(edit->connect(plugin, 0, output, 0));
    REQUIRE(edit->node(plugin) != nullptr);
    REQUIRE(edit->node(plugin)->plugin_info.format == PluginFormat::BuiltIn);

    REQUIRE(graph.nodes().empty());
    REQUIRE(graph.connections().empty());
    REQUIRE_FALSE(graph.is_prepared());
    REQUIRE(prepares.load(std::memory_order_relaxed) == 0);

    REQUIRE(edit->prepare(48'000.0, 8) == Result::Prepared);
    REQUIRE(prepares.load(std::memory_order_relaxed) == 1);
    REQUIRE(releases.load(std::memory_order_relaxed) == 0);
    REQUIRE(graph.nodes().empty());
    REQUIRE(edit->commit() == Result::Committed);
    REQUIRE(graph.nodes().size() == 3);
    REQUIRE(graph.node(plugin) != nullptr);
    REQUIRE(graph.node(plugin)->plugin_info.format == PluginFormat::BuiltIn);
    REQUIRE(graph.is_prepared());

    std::array<float, 8> source{};
    source.fill(0.375f);
    std::array<float, 8> rendered{};
    const float* source_ptrs[] = {source.data()};
    float* rendered_ptrs[] = {rendered.data()};
    pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
    pulp::audio::BufferView<float> out(rendered_ptrs, 1, rendered.size());
    graph.process(out, in, 8);
    REQUIRE(rendered == source);

    edit.reset();
    graph.release();
    REQUIRE(releases.load(std::memory_order_relaxed) == 1);
    REQUIRE(destroys.load(std::memory_order_relaxed) == 0);
    graph.clear();
    REQUIRE(releases.load(std::memory_order_relaxed) == 1);
    REQUIRE(destroys.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("SignalGraph prepared edit rolls back a failed owned built-in prepare exactly once",
          "[host][graph][prepared-edit][plugin][builtin][negative]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> prepares{0};
    std::atomic<int> releases{0};
    std::atomic<int> destroys{0};
    SignalGraph graph;

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->add_owned_builtin_plugin_node(
                std::make_unique<PreparedEditOwnedBuiltInPlugin>(
                    prepares, releases, destroys, false),
                1, 1, "failing owned built-in") != 0);
    REQUIRE(edit->prepare(48'000.0, 8)
            == Result::ExternalPluginReprepareRequired);
    REQUIRE(edit->prepare(48'000.0, 8)
            == Result::ExternalPluginReprepareRequired);
    REQUIRE(prepares.load(std::memory_order_relaxed) == 1);
    REQUIRE(releases.load(std::memory_order_relaxed) == 0);
    REQUIRE(destroys.load(std::memory_order_relaxed) == 0);

    edit.reset();
    REQUIRE(graph.nodes().empty());
    REQUIRE_FALSE(graph.is_prepared());
    REQUIRE(releases.load(std::memory_order_relaxed) == 1);
    REQUIRE(destroys.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("SignalGraph stale prepared edit releases its owned built-in exactly once",
          "[host][graph][prepared-edit][plugin][builtin][negative]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> prepares{0};
    std::atomic<int> releases{0};
    std::atomic<int> destroys{0};
    SignalGraph graph;

    auto edit = graph.begin_prepared_topology_edit();
    const auto stale_plugin = edit->add_owned_builtin_plugin_node(
        std::make_unique<PreparedEditOwnedBuiltInPlugin>(prepares, releases, destroys),
        1, 1, "stale owned built-in");
    REQUIRE(stale_plugin != 0);
    REQUIRE(edit->prepare(48'000.0, 8) == Result::Prepared);
    REQUIRE(prepares.load(std::memory_order_relaxed) == 1);
    REQUIRE(releases.load(std::memory_order_relaxed) == 0);
    REQUIRE(destroys.load(std::memory_order_relaxed) == 0);

    const auto winner = graph.add_gain_node("newer authoring state");
    REQUIRE(winner != 0);
    REQUIRE(edit->commit() == Result::StaleBase);
    REQUIRE(graph.node(winner) != nullptr);
    REQUIRE(graph.node(winner)->type == NodeType::Gain);
    REQUIRE(graph.nodes().size() == 1);
    REQUIRE(std::none_of(graph.nodes().begin(), graph.nodes().end(),
                         [](const GraphNode& node) {
                             return node.type == NodeType::Plugin;
                         }));
    REQUIRE(releases.load(std::memory_order_relaxed) == 0);
    REQUIRE(destroys.load(std::memory_order_relaxed) == 0);

    edit.reset();
    REQUIRE(releases.load(std::memory_order_relaxed) == 1);
    REQUIRE(destroys.load(std::memory_order_relaxed) == 1);
    graph.clear();
    REQUIRE(releases.load(std::memory_order_relaxed) == 1);
    REQUIRE(destroys.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("SignalGraph owned built-in removal and replacement wait for the old execution snapshot",
          "[host][graph][prepared-edit][plugin][builtin][snapshot]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> prepares{0};
    std::atomic<int> releases{0};
    std::atomic<int> destroys{0};
    SignalGraph graph;

    auto add = graph.begin_prepared_topology_edit();
    const auto input = add->add_input_node(1, "input");
    const auto plugin = add->add_owned_builtin_plugin_node(
        std::make_unique<PreparedEditOwnedBuiltInPlugin>(
            prepares, releases, destroys, true, 0.25f),
        1, 1, "owned built-in");
    const auto output = add->add_output_node(1, "output");
    REQUIRE(plugin != 0);
    REQUIRE(add->connect(input, 0, plugin, 0));
    REQUIRE(add->connect(plugin, 0, output, 0));
    REQUIRE(add->prepare(48'000.0, 8) == Result::Prepared);
    REQUIRE(add->commit() == Result::Committed);
    auto pinned = add->committed_execution_snapshot();
    REQUIRE(pinned);
    add.reset();

    SECTION("removal") {
        auto remove = graph.begin_prepared_topology_edit();
        REQUIRE(remove->remove_node(plugin));
        REQUIRE(remove->prepare(48'000.0, 8) == Result::Prepared);
        REQUIRE(remove->commit() == Result::Committed);
        remove.reset();
        REQUIRE(graph.node(plugin) == nullptr);
        REQUIRE(releases.load(std::memory_order_relaxed) == 0);
        REQUIRE(destroys.load(std::memory_order_relaxed) == 0);

        pinned = {};
        REQUIRE(releases.load(std::memory_order_relaxed) == 1);
        REQUIRE(destroys.load(std::memory_order_relaxed) == 1);
    }

    SECTION("replacement") {
        std::atomic<int> replacement_prepares{0};
        std::atomic<int> replacement_releases{0};
        std::atomic<int> replacement_destroys{0};
        auto replace = graph.begin_prepared_topology_edit();
        REQUIRE(replace->remove_node(plugin));
        const auto replacement = replace->add_owned_builtin_plugin_node(
            std::make_unique<PreparedEditOwnedBuiltInPlugin>(
                replacement_prepares, replacement_releases, replacement_destroys,
                true, 0.75f),
            1, 1, "replacement owned built-in");
        REQUIRE(replacement != 0);
        REQUIRE(replace->connect(input, 0, replacement, 0));
        REQUIRE(replace->connect(replacement, 0, output, 0));
        REQUIRE(replace->prepare(48'000.0, 8) == Result::Prepared);
        REQUIRE(replacement_prepares.load(std::memory_order_relaxed) == 1);
        REQUIRE(replace->commit() == Result::Committed);
        replace.reset();

        REQUIRE(graph.node(plugin) == nullptr);
        REQUIRE(graph.node(replacement) != nullptr);
        REQUIRE(releases.load(std::memory_order_relaxed) == 0);
        REQUIRE(destroys.load(std::memory_order_relaxed) == 0);
        REQUIRE(replacement_releases.load(std::memory_order_relaxed) == 0);
        REQUIRE(replacement_destroys.load(std::memory_order_relaxed) == 0);

        std::array<float, 8> source{};
        source.fill(0.5f);
        std::array<float, 8> old_render{};
        std::array<float, 8> new_render{};
        const float* source_ptrs[] = {source.data()};
        float* old_ptrs[] = {old_render.data()};
        float* new_ptrs[] = {new_render.data()};
        pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
        pulp::audio::BufferView<float> old_out(old_ptrs, 1, old_render.size());
        pulp::audio::BufferView<float> new_out(new_ptrs, 1, new_render.size());
        pinned.process(old_out, in, 8);
        graph.process(new_out, in, 8);
        std::array<float, 8> expected_old{};
        expected_old.fill(0.125f);
        std::array<float, 8> expected_new{};
        expected_new.fill(0.375f);
        REQUIRE(old_render == expected_old);
        REQUIRE(new_render == expected_new);
        REQUIRE(old_render != new_render);
        REQUIRE(releases.load(std::memory_order_relaxed) == 0);
        REQUIRE(destroys.load(std::memory_order_relaxed) == 0);

        pinned = {};
        REQUIRE(releases.load(std::memory_order_relaxed) == 1);
        REQUIRE(destroys.load(std::memory_order_relaxed) == 1);
        REQUIRE(replacement_releases.load(std::memory_order_relaxed) == 0);
        REQUIRE(replacement_destroys.load(std::memory_order_relaxed) == 0);

        graph.release();
        REQUIRE(releases.load(std::memory_order_relaxed) == 1);
        REQUIRE(destroys.load(std::memory_order_relaxed) == 1);
        REQUIRE(replacement_releases.load(std::memory_order_relaxed) == 1);
        REQUIRE(replacement_destroys.load(std::memory_order_relaxed) == 0);
        graph.clear();
        REQUIRE(replacement_releases.load(std::memory_order_relaxed) == 1);
        REQUIRE(replacement_destroys.load(std::memory_order_relaxed) == 1);
    }
}

TEST_CASE("SignalGraph owned built-ins do not relax ordinary baseline plugin removal",
          "[host][graph][prepared-edit][plugin][builtin][negative]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> prepares{0};
    std::atomic<int> releases{0};
    SignalGraph graph;
    const auto plugin = graph.add_plugin_node(
        std::make_unique<PreparedEditCountingPlugin>(prepares, &releases),
        1, 1, "ordinary external plugin");
    REQUIRE(graph.prepare(48'000.0, 8));

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->remove_node(plugin));
    REQUIRE(edit->prepare(48'000.0, 8)
            == Result::BaselinePluginRemovalRequiresRelease);
    edit.reset();
    REQUIRE(graph.node(plugin) != nullptr);
    REQUIRE(graph.is_prepared());
    REQUIRE(releases.load(std::memory_order_relaxed) == 0);

    graph.release();
    REQUIRE(releases.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("SignalGraph prepared edit first prepare failure is a full rollback",
          "[host][graph][prepared-edit][transaction]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;

    SECTION("custom creation fails after a complete candidate was authored") {
        std::atomic<int> creates{0};
        const auto json_before = GraphSerializer::to_json(graph);
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->register_custom_node_type(
            make_prepared_edit_level_type("pulp.test.prepared.fail", 2.0f,
                                          &creates, nullptr, nullptr, true)));
        const auto input = edit->add_input_node(1, "input");
        const auto custom = edit->add_custom_node("pulp.test.prepared.fail");
        const auto output = edit->add_output_node(1, "output");
        REQUIRE(input != 0);
        REQUIRE(custom != 0);
        REQUIRE(output != 0);
        REQUIRE(edit->connect(input, 0, custom, 0));
        REQUIRE(edit->connect(custom, 0, output, 0));
        edit->set_canonical_executor_routing_enabled(false);

        REQUIRE(edit->prepare(48000.0, 8)
                == Result::CustomInstanceCreateFailed);
        REQUIRE(edit->prepare(48000.0, 8)
                == Result::CustomInstanceCreateFailed);
        REQUIRE(creates.load(std::memory_order_relaxed) == 1);
        edit.reset();

        REQUIRE(graph.nodes().empty());
        REQUIRE(graph.connections().empty());
        REQUIRE(graph.custom_node_type_count() == 0);
        REQUIRE_FALSE(graph.is_prepared());
        REQUIRE(graph.canonical_executor_routing_enabled());
        REQUIRE(GraphSerializer::to_json(graph) == json_before);
        REQUIRE(graph.add_input_node(1, "first real node") == 1);
    }

    SECTION("one failed route poisons the whole candidate") {
        const auto json_before = GraphSerializer::to_json(graph);
        auto edit = graph.begin_prepared_topology_edit();
        const auto input = edit->add_input_node(1, "input");
        REQUIRE(input == 1);
        REQUIRE_FALSE(edit->connect(input, 0, 9999, 0));
        REQUIRE(edit->prepare(48000.0, 8) == Result::InvalidMutation);
        edit.reset();
        REQUIRE(graph.nodes().empty());
        REQUIRE(graph.connections().empty());
        REQUIRE(GraphSerializer::to_json(graph) == json_before);
        REQUIRE(graph.add_input_node(1, "first real node") == 1);
    }

    SECTION("a stale candidate cannot overwrite newer authoring state") {
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->add_output_node(1, "candidate output") == 1);
        REQUIRE(graph.add_input_node(1, "newer owner input") == 1);
        const auto owner_json = GraphSerializer::to_json(graph);

        REQUIRE(edit->prepare(48000.0, 8) == Result::StaleBase);
        REQUIRE(edit->commit() == Result::NotPrepared);
        REQUIRE(GraphSerializer::to_json(graph) == owner_json);
        REQUIRE(graph.nodes().size() == 1);
        REQUIRE(graph.nodes().front().name == "newer owner input");
        REQUIRE(graph.add_output_node(1, "next owner output") == 2);
    }

    SECTION("the first successful graph is installed only at commit") {
        auto edit = graph.begin_prepared_topology_edit();
        const auto input = edit->add_input_node(1, "input");
        const auto output = edit->add_output_node(1, "output");
        REQUIRE(edit->connect(input, 0, output, 0));
        edit->set_parallel_routing_enabled(true);

        REQUIRE_FALSE(edit->routed_execution_ready(8));
        REQUIRE(edit->prepare(48000.0, 8) == Result::Prepared);
        REQUIRE(edit->routed_execution_ready(8));
        REQUIRE_FALSE(edit->routed_execution_ready(9));
        REQUIRE(graph.nodes().empty());
        REQUIRE_FALSE(graph.is_prepared());
        REQUIRE(edit->commit() == Result::Committed);
        REQUIRE(graph.nodes().size() == 2);
        REQUIRE(graph.connections().size() == 1);
        REQUIRE(graph.is_prepared());
        std::array<float, 8> source{};
        source.fill(0.25f);
        std::array<float, 8> rendered{};
        const float* source_ptrs[] = {source.data()};
        float* rendered_ptrs[] = {rendered.data()};
        pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
        pulp::audio::BufferView<float> out(rendered_ptrs, 1, rendered.size());
        graph.process(out, in, 8);
        REQUIRE(rendered == source);
        REQUIRE(graph.routing_executor_stats().serial_levels_run > 0);
        REQUIRE(graph.routed_walk_fallbacks() == 0);
        REQUIRE(graph.add_gain_node("next owner node") == 3);
    }
}

TEST_CASE("SignalGraph prepared edit publishes old or new topology without silence",
          "[host][graph][prepared-edit][transaction][threading]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> creates{0};
    std::atomic<int> prepares{0};
    std::atomic<int> destroys{0};
    std::atomic<bool> old_entered{false};
    std::atomic<bool> allow_old_exit{false};
    std::atomic<bool> old_exited{false};
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(make_prepared_edit_level_type(
        "pulp.test.prepared.level.1", 1.0f, &creates, &prepares, &destroys,
        false, &old_entered, &allow_old_exit, &old_exited)));
    const auto input = graph.add_input_node(1, "input");
    auto current = graph.add_custom_node("pulp.test.prepared.level.1");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, current, 0));
    REQUIRE(graph.connect(current, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 16));

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->register_custom_node_type(make_prepared_edit_level_type(
        "pulp.test.prepared.level.2", 2.0f, &creates, &prepares, &destroys)));
    REQUIRE(edit->remove_node(current));
    const auto replacement =
        edit->add_custom_node("pulp.test.prepared.level.2");
    REQUIRE(replacement != 0);
    REQUIRE(edit->connect(input, 0, replacement, 0));
    REQUIRE(edit->connect(replacement, 0, output, 0));
    REQUIRE(edit->prune_unused_custom_node_types() == 1);

    std::atomic<std::size_t> audio_thread_allocations{0};
    std::array<float, 16> old_rendered{};
    std::array<float, 16> new_rendered{};
    REQUIRE(edit->prepare(48000.0, 16) == Result::Prepared);
    std::thread audio([&] {
        std::array<float, 16> source{};
        source.fill(1.0f);
        const float* source_ptrs[] = {source.data()};
        pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
        pulp::test::RtAllocationProbe allocation_probe;
        float* old_ptrs[] = {old_rendered.data()};
        pulp::audio::BufferView<float> old_out(old_ptrs, 1, old_rendered.size());
        graph.process(old_out, in, 16);
        float* new_ptrs[] = {new_rendered.data()};
        pulp::audio::BufferView<float> new_out(new_ptrs, 1, new_rendered.size());
        graph.process(new_out, in, 16);
        audio_thread_allocations.store(
            allocation_probe.allocation_count(), std::memory_order_relaxed);
    });

    const auto entry_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!old_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entry_deadline) {
        std::this_thread::yield();
    }
    if (!old_entered.load(std::memory_order_acquire)) {
        allow_old_exit.store(true, std::memory_order_release);
        audio.join();
        REQUIRE(old_entered.load(std::memory_order_acquire));
    }
    std::atomic<bool> commit_done{false};
    Result commit_result = Result::NotPrepared;
    std::thread committer([&] {
        commit_result = edit->commit();
        commit_done.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!commit_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    // Commit returned while the audio callback is deliberately parked in the
    // old snapshot: publication retired it without waiting or invalidating the
    // in-flight block.
    const bool old_was_pinned_across_commit =
        commit_done.load(std::memory_order_acquire) &&
        old_entered.load(std::memory_order_acquire) &&
        !old_exited.load(std::memory_order_acquire);
    allow_old_exit.store(true, std::memory_order_release);
    audio.join();
    committer.join();

    REQUIRE(commit_result == Result::Committed);
    REQUIRE(old_was_pinned_across_commit);
    REQUIRE(old_exited.load(std::memory_order_acquire));
    REQUIRE(std::all_of(old_rendered.begin(), old_rendered.end(),
                        [](float sample) { return sample == 1.0f; }));
    REQUIRE(std::all_of(new_rendered.begin(), new_rendered.end(),
                        [](float sample) { return sample == 2.0f; }));
    REQUIRE(audio_thread_allocations.load(std::memory_order_relaxed) == 0);
    REQUIRE(graph.nodes().size() == 3);
    REQUIRE(graph.custom_node_type_count() == 1);
    REQUIRE(creates.load(std::memory_order_relaxed) == 2);
    REQUIRE(prepares.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("SignalGraph prepared edit changes dimensions only for lifecycle-free custom nodes",
          "[host][graph][prepared-edit][dimensions]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> creates{0};
    std::atomic<int> destroys{0};
    SignalGraph graph;
    auto type = make_prepared_edit_level_type("pulp.test.prepared.dimension", 0.5f,
                                              &creates, nullptr, &destroys);
    type.prepare = {};
    REQUIRE(graph.register_custom_node_type(std::move(type)));
    const auto input = graph.add_input_node(1, "input");
    const auto custom = graph.add_custom_node("pulp.test.prepared.dimension");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, custom, 0));
    REQUIRE(graph.connect(custom, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 8));
    REQUIRE(creates.load(std::memory_order_relaxed) == 1);

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->prepare(44100.0, 16) == Result::Prepared);
    REQUIRE(edit->routed_execution_ready(16));
    REQUIRE_FALSE(edit->routed_execution_ready(17));
    REQUIRE(graph.prepared_max_block_size() == 8);
    REQUIRE(edit->commit() == Result::Committed);
    REQUIRE(graph.prepared_max_block_size() == 16);
    REQUIRE(creates.load(std::memory_order_relaxed) == 1);

    std::array<float, 16> source{};
    source.fill(1.0f);
    std::array<float, 16> rendered{};
    const float* source_ptrs[] = {source.data()};
    float* rendered_ptrs[] = {rendered.data()};
    pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
    pulp::audio::BufferView<float> out(rendered_ptrs, 1, rendered.size());
    graph.process(out, in, 16);
    REQUIRE(std::all_of(rendered.begin(), rendered.end(),
                        [](float sample) { return sample == 0.5f; }));
}

TEST_CASE("SignalGraph quiesced prepared edit restores retained lifecycles on every exit",
          "[host][graph][prepared-edit][dimensions]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    auto plugin = std::make_unique<PreparedEditDimensionPlugin>();
    auto* plugin_ptr = plugin.get();
    const auto plugin_node =
        graph.add_plugin_node(std::move(plugin), 1, 1, "dimension plugin");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, plugin_node, 0));
    REQUIRE(graph.connect(plugin_node, 0, output, 0));
    REQUIRE(graph.prepare(48'000.0, 64));

    const auto render = [&] {
        std::array<float, 64> source{};
        source.fill(1.0f);
        std::array<float, 64> rendered{};
        const float* source_ptrs[] = {source.data()};
        float* rendered_ptrs[] = {rendered.data()};
        pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
        pulp::audio::BufferView<float> out(rendered_ptrs, 1, rendered.size());
        graph.process(out, in, 64);
        return rendered;
    };

    SECTION("abandon after successful candidate prepare restores the base dimensions") {
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(44'100.0, 128) == Result::Prepared);
        REQUIRE(plugin_ptr->prepared_sample_rate.load(std::memory_order_relaxed) == 44'100.0);
        REQUIRE(plugin_ptr->prepared_max_block.load(std::memory_order_relaxed) == 128);
        edit.reset();

        REQUIRE(plugin_ptr->prepared_sample_rate.load(std::memory_order_relaxed) == 48'000.0);
        REQUIRE(plugin_ptr->prepared_max_block.load(std::memory_order_relaxed) == 64);
        const auto rendered = render();
        REQUIRE(std::all_of(rendered.begin(), rendered.end(),
                            [](float sample) { return sample == 1.0f; }));
    }

    SECTION("commit rejection restores before returning") {
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(44'100.0, 128) == Result::Prepared);
        graph.begin_swap_edit();
        REQUIRE(edit->commit() == Result::StaleBase);
        REQUIRE(plugin_ptr->prepared_sample_rate.load(std::memory_order_relaxed) == 48'000.0);
        REQUIRE(plugin_ptr->prepared_max_block.load(std::memory_order_relaxed) == 64);
        const auto rendered = render();
        REQUIRE(std::all_of(rendered.begin(), rendered.end(),
                            [](float sample) { return sample == 1.0f; }));
        graph.abort_swap_edit();
    }

    SECTION("failed restoration revokes the owner publication") {
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(44'100.0, 128) == Result::Prepared);
        plugin_ptr->fail_sample_rate.store(48'000.0, std::memory_order_relaxed);
        edit.reset();
        REQUIRE_FALSE(graph.is_prepared());
        const auto rendered = render();
        REQUIRE(std::all_of(rendered.begin(), rendered.end(),
                            [](float sample) { return sample == 0.0f; }));
    }
}

TEST_CASE("SignalGraph quiesced abandonment restores retained custom lifecycle dimensions",
          "[host][graph][prepared-edit][dimensions]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<double> prepared_sample_rate{0.0};
    std::atomic<int> prepared_max_block{0};
    SignalGraph graph;
    auto type = make_prepared_edit_level_type("pulp.test.prepared.custom-dimensions", 1.0f);
    type.prepare = [&](void*, double sample_rate, int max_block_size) {
        prepared_sample_rate.store(sample_rate, std::memory_order_relaxed);
        prepared_max_block.store(max_block_size, std::memory_order_relaxed);
    };
    REQUIRE(graph.register_custom_node_type(std::move(type)));
    const auto custom = graph.add_custom_node("pulp.test.prepared.custom-dimensions");
    REQUIRE(custom != 0);
    REQUIRE(graph.prepare(48'000.0, 64));

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->prepare_quiesced(44'100.0, 128) == Result::Prepared);
    REQUIRE(prepared_sample_rate.load(std::memory_order_relaxed) == 44'100.0);
    REQUIRE(prepared_max_block.load(std::memory_order_relaxed) == 128);
    edit.reset();

    REQUIRE(prepared_sample_rate.load(std::memory_order_relaxed) == 48'000.0);
    REQUIRE(prepared_max_block.load(std::memory_order_relaxed) == 64);
    REQUIRE(graph.is_prepared());
}

TEST_CASE("SignalGraph quiesced abandonment restores an unprepared base lifecycle",
          "[host][graph][prepared-edit][dimensions]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;

    SECTION("retained plugins return to released state") {
        std::atomic<int> prepares{0};
        std::atomic<int> releases{0};
        SignalGraph graph;
        REQUIRE(graph.add_plugin_node(
                    std::make_unique<PreparedEditCountingPlugin>(prepares, &releases),
                    1, 1, "plugin") != 0);
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(44'100.0, 128) == Result::Prepared);
        REQUIRE(prepares.load(std::memory_order_relaxed) == 1);
        edit.reset();
        REQUIRE(releases.load(std::memory_order_relaxed) == 1);
        REQUIRE_FALSE(graph.is_prepared());
    }

    SECTION("retained custom instances return to released state") {
        bool throw_prepare = true;
        std::atomic<int> releases{0};
        SignalGraph graph;
        auto type = make_prepared_edit_level_type("pulp.test.prepared.unprepared-custom", 1.0f);
        type.prepare = [&](void*, double, int) {
            if (throw_prepare)
                throw std::runtime_error("prepare");
        };
        type.release = [&](void*) { releases.fetch_add(1, std::memory_order_relaxed); };
        REQUIRE(graph.register_custom_node_type(std::move(type)));
        REQUIRE(graph.add_custom_node("pulp.test.prepared.unprepared-custom") != 0);
        REQUIRE_THROWS(graph.prepare(48'000.0, 64));
        REQUIRE_FALSE(graph.is_prepared());

        throw_prepare = false;
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(44'100.0, 128) == Result::Prepared);
        edit.reset();
        REQUIRE(releases.load(std::memory_order_relaxed) == 1);
        REQUIRE_FALSE(graph.is_prepared());
    }

    SECTION("candidate-created instances on baseline nodes are released once") {
        std::atomic<int> creates{0};
        std::atomic<int> releases{0};
        std::atomic<int> destroys{0};
        SignalGraph graph;
        auto type = make_prepared_edit_level_type(
            "pulp.test.prepared.unprepared-baseline-custom", 1.0f,
            &creates, nullptr, &destroys);
        type.release = [&](void*) { releases.fetch_add(1, std::memory_order_relaxed); };
        REQUIRE(graph.register_custom_node_type(std::move(type)));
        REQUIRE(graph.add_custom_node(
                    "pulp.test.prepared.unprepared-baseline-custom") != 0);

        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(44'100.0, 128) == Result::Prepared);
        edit.reset();
        REQUIRE(creates.load(std::memory_order_relaxed) == 1);
        REQUIRE(releases.load(std::memory_order_relaxed) == 1);
        REQUIRE(destroys.load(std::memory_order_relaxed) == 1);
        REQUIRE_FALSE(graph.is_prepared());
    }
}

TEST_CASE("SignalGraph quiesced rollback balances only entered lifecycle callbacks",
          "[host][graph][prepared-edit][dimensions]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;

    SECTION("preflight rejection leaves every retained lifecycle untouched") {
        std::array<std::atomic<int>, 2> prepares{};
        std::array<std::atomic<int>, 2> releases{};
        SignalGraph graph;
        REQUIRE(graph.add_plugin_node(
                    std::make_unique<PreparedEditCountingPlugin>(
                        prepares[0], &releases[0]),
                    1, 1, "first") != 0);
        REQUIRE(graph.add_plugin_node(
                    std::make_unique<PreparedEditCountingPlugin>(
                        prepares[1], &releases[1]),
                    1, 1, "second") != 0);
        auto limits = graph.limits();
        limits.max_block_size = 64;
        graph.set_limits(limits);

        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(44'100.0, 128) ==
                Result::ExternalPluginReprepareRequired);
        edit.reset();

        for (std::size_t i = 0; i < prepares.size(); ++i) {
            REQUIRE(prepares[i].load(std::memory_order_relaxed) == 0);
            REQUIRE(releases[i].load(std::memory_order_relaxed) == 0);
        }
    }

    SECTION("plugin failure releases entered plugins and leaves later plugins untouched") {
        std::array<std::atomic<int>, 3> prepares{};
        std::array<std::atomic<int>, 3> releases{};
        SignalGraph graph;
        REQUIRE(graph.add_plugin_node(
                    std::make_unique<PreparedEditCountingPlugin>(
                        prepares[0], &releases[0], true),
                    1, 1, "successful first") != 0);
        REQUIRE(graph.add_plugin_node(
                    std::make_unique<PreparedEditCountingPlugin>(
                        prepares[1], &releases[1], false),
                    1, 1, "failing second") != 0);
        REQUIRE(graph.add_plugin_node(
                    std::make_unique<PreparedEditCountingPlugin>(
                        prepares[2], &releases[2], true),
                    1, 1, "untouched third") != 0);

        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(44'100.0, 128) ==
                Result::ExternalPluginReprepareRequired);
        edit.reset();

        REQUIRE(prepares[0].load(std::memory_order_relaxed) == 1);
        REQUIRE(prepares[1].load(std::memory_order_relaxed) == 1);
        REQUIRE(prepares[2].load(std::memory_order_relaxed) == 0);
        REQUIRE(releases[0].load(std::memory_order_relaxed) == 1);
        REQUIRE(releases[1].load(std::memory_order_relaxed) == 1);
        REQUIRE(releases[2].load(std::memory_order_relaxed) == 0);
    }

    SECTION("custom throw releases entered customs and leaves later customs untouched") {
        std::array<std::atomic<int>, 3> prepares{};
        std::array<std::atomic<int>, 3> releases{};
        bool throw_second = false;
        SignalGraph graph;
        for (std::size_t i = 0; i < prepares.size(); ++i) {
            auto type = make_prepared_edit_level_type(
                "pulp.test.prepared.touch." + std::to_string(i), 1.0f);
            type.prepare = [&, i](void*, double, int) {
                prepares[i].fetch_add(1, std::memory_order_relaxed);
                if (throw_second && i == 1)
                    throw std::runtime_error("prepare");
            };
            type.release = [&, i](void*) {
                releases[i].fetch_add(1, std::memory_order_relaxed);
            };
            REQUIRE(graph.register_custom_node_type(std::move(type)));
            REQUIRE(graph.add_custom_node(
                        "pulp.test.prepared.touch." + std::to_string(i)) != 0);
        }
        REQUIRE(graph.prepare(48'000.0, 64));
        graph.release();
        for (std::size_t i = 0; i < prepares.size(); ++i) {
            prepares[i].store(0, std::memory_order_relaxed);
            releases[i].store(0, std::memory_order_relaxed);
        }
        throw_second = true;

        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(44'100.0, 128) ==
                Result::ExternalPluginReprepareRequired);
        edit.reset();

        REQUIRE(prepares[0].load(std::memory_order_relaxed) == 1);
        REQUIRE(prepares[1].load(std::memory_order_relaxed) == 1);
        REQUIRE(prepares[2].load(std::memory_order_relaxed) == 0);
        REQUIRE(releases[0].load(std::memory_order_relaxed) == 1);
        REQUIRE(releases[1].load(std::memory_order_relaxed) == 1);
        REQUIRE(releases[2].load(std::memory_order_relaxed) == 0);
    }
}

TEST_CASE("SignalGraph prepared edit surface is fail closed and releases abandoned instances",
          "[host][graph][prepared-edit][surface]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;

    SECTION("all owned node and route wrappers stay candidate local") {
        auto edit = graph.begin_prepared_topology_edit();
        auto type = make_prepared_edit_level_type("pulp.test.prepared.surface", 1.0f);
        REQUIRE(edit->register_custom_node_type(std::move(type)));
        const auto input = edit->add_input_node(1, "input");
        const auto output = edit->add_output_node(1, "output");
        const auto gain = edit->add_gain_node("gain");
        const auto midi_input = edit->add_midi_input_node("midi input");
        const auto midi_output = edit->add_midi_output_node("midi output");
        const auto custom = edit->add_custom_node("pulp.test.prepared.surface", 1);
        const auto unresolved = edit->add_unresolved_custom_node(
            "pulp.test.prepared.missing", 1, 1, 1, "missing");
        REQUIRE(input != 0);
        REQUIRE(output != 0);
        REQUIRE(gain != 0);
        REQUIRE(custom != 0);
        REQUIRE(unresolved != 0);
        REQUIRE(edit->connect(input, 0, gain, 0));
        REQUIRE(edit->connect(gain, 0, output, 0));
        REQUIRE(edit->connect_feedback(gain, 0, gain, 0));
        REQUIRE(edit->connect_midi(midi_input, midi_output));
        REQUIRE(edit->set_node_gain(gain, 0.25f));
        edit->set_canonical_executor_routing_enabled(false);
        edit->set_parallel_routing_enabled(false);
        edit->set_anticipation_enabled(false);
        REQUIRE(edit->node(custom) != nullptr);
        REQUIRE(edit->nodes().size() == 7);
        REQUIRE(edit->connections().size() == 4);
        REQUIRE_FALSE(edit->unregister_custom_node_type(
            "pulp.test.prepared.surface", 1));
        REQUIRE(edit->prepare(48000.0, 8) == Result::InvalidMutation);
        REQUIRE(graph.nodes().empty());
    }

    SECTION("unused types can be explicitly unregistered") {
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->register_custom_node_type(
            make_prepared_edit_level_type("pulp.test.prepared.unused", 1.0f)));
        REQUIRE(edit->custom_node_type_count() == 1);
        REQUIRE(edit->unregister_custom_node_type("pulp.test.prepared.unused", 1));
        REQUIRE(edit->custom_node_type_count() == 0);
        REQUIRE(edit->prune_unused_custom_node_types() == 0);
        REQUIRE(edit->commit() == Result::NotPrepared);
    }

    SECTION("an abandoned prepared instance is released and destroyed once") {
        std::atomic<int> creates{0};
        std::atomic<int> prepares{0};
        std::atomic<int> releases{0};
        std::atomic<int> destroys{0};
        auto type = make_prepared_edit_level_type(
            "pulp.test.prepared.abandoned", 1.0f, &creates, &prepares, &destroys);
        type.release = [&](void*) {
            releases.fetch_add(1, std::memory_order_relaxed);
        };
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->register_custom_node_type(std::move(type)));
        REQUIRE(edit->add_custom_node("pulp.test.prepared.abandoned") != 0);
        REQUIRE(edit->prepare(48000.0, 8) == Result::Prepared);
        REQUIRE(edit->add_gain_node("too late") == 0);
        edit.reset();
        REQUIRE(creates.load(std::memory_order_relaxed) == 1);
        REQUIRE(prepares.load(std::memory_order_relaxed) == 1);
        REQUIRE(releases.load(std::memory_order_relaxed) == 1);
        REQUIRE(destroys.load(std::memory_order_relaxed) == 1);
        REQUIRE(graph.nodes().empty());
    }

    SECTION("a quiesced prepare failure releases custom instances created before it") {
        std::atomic<int> creates{0};
        std::atomic<int> releases{0};
        std::atomic<int> destroys{0};
        auto type = make_prepared_edit_level_type(
            "pulp.test.prepared.quiesced-failure", 1.0f, &creates, nullptr,
            &destroys);
        type.prepare = [](void*, double, int) { throw std::runtime_error("prepare"); };
        type.release = [&](void*) {
            releases.fetch_add(1, std::memory_order_relaxed);
        };
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->register_custom_node_type(std::move(type)));
        REQUIRE(edit->add_custom_node("pulp.test.prepared.quiesced-failure") != 0);

        REQUIRE(edit->prepare_quiesced(48000.0, 8)
                == Result::ExternalPluginReprepareRequired);
        edit.reset();

        REQUIRE(creates.load(std::memory_order_relaxed) == 1);
        REQUIRE(releases.load(std::memory_order_relaxed) == 1);
        REQUIRE(destroys.load(std::memory_order_relaxed) == 1);
        REQUIRE(graph.nodes().empty());
    }
}

TEST_CASE("SignalGraph prepared edit registry remains bounded under binding churn",
          "[host][graph][prepared-edit][registry]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> creates{0};
    std::atomic<int> prepares{0};
    std::atomic<int> destroys{0};
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(make_prepared_edit_level_type(
        "pulp.test.prepared.churn.0", 1.0f, &creates, &prepares, &destroys)));
    const auto input = graph.add_input_node(1, "input");
    auto current = graph.add_custom_node("pulp.test.prepared.churn.0");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, current, 0));
    REQUIRE(graph.connect(current, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 8));

    constexpr int kReplacements = 64;
    for (int i = 1; i <= kReplacements; ++i) {
        auto edit = graph.begin_prepared_topology_edit();
        const std::string type_id =
            "pulp.test.prepared.churn." + std::to_string(i);
        REQUIRE(edit->register_custom_node_type(make_prepared_edit_level_type(
            type_id, 1.0f + static_cast<float>(i), &creates, &prepares,
            &destroys)));
        REQUIRE(edit->remove_node(current));
        const auto next = edit->add_custom_node(type_id);
        REQUIRE(next != 0);
        REQUIRE(edit->connect(input, 0, next, 0));
        REQUIRE(edit->connect(next, 0, output, 0));
        REQUIRE(edit->prune_unused_custom_node_types() == 1);
        REQUIRE(edit->custom_node_type_count() == 1);
        REQUIRE(edit->prepare(48000.0, 8) == Result::Prepared);
        REQUIRE(edit->commit() == Result::Committed);
        REQUIRE(graph.custom_node_type_count() == 1);
        current = next;
    }
    REQUIRE(creates.load(std::memory_order_relaxed) == kReplacements + 1);
    REQUIRE(prepares.load(std::memory_order_relaxed) == kReplacements + 1);
}

TEST_CASE("SignalGraph prepared edit rejects external plugin reprepare before mutation",
          "[host][graph][prepared-edit][plugin]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> prepare_calls{0};
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    const auto plugin = graph.add_plugin_node(
        std::make_unique<PreparedEditCountingPlugin>(prepare_calls), 1, 1,
        "plugin");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 8));
    REQUIRE(prepare_calls.load(std::memory_order_relaxed) == 1);

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->add_gain_node("candidate only") != 0);
    REQUIRE(edit->prepare(48000.0, 16)
            == Result::ExternalPluginReprepareRequired);
    edit.reset();

    REQUIRE(prepare_calls.load(std::memory_order_relaxed) == 1);
    REQUIRE(graph.nodes().size() == 3);
    REQUIRE(graph.connections().size() == 2);
    REQUIRE(graph.is_prepared());
}

TEST_CASE("SignalGraph prepared edit rejects baseline lifecycle removals before mutation",
          "[host][graph][prepared-edit][lifecycle]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;

    SECTION("plugin removal leaves ordinary release as the single callback owner") {
        for (const bool quiesced : {false, true}) {
            std::atomic<int> prepares{0};
            std::atomic<int> releases{0};
            SignalGraph graph;
            const auto plugin = graph.add_plugin_node(
                std::make_unique<PreparedEditCountingPlugin>(prepares, &releases), 1, 1,
                "plugin");
            REQUIRE(graph.prepare(48000.0, 8));
            const auto before = GraphSerializer::to_json(graph);

            auto edit = graph.begin_prepared_topology_edit();
            REQUIRE(edit->remove_node(plugin));
            const auto result = quiesced ? edit->prepare_quiesced(48000.0, 8)
                                         : edit->prepare(48000.0, 8);
            REQUIRE(result == Result::BaselinePluginRemovalRequiresRelease);
            edit.reset();
            REQUIRE(GraphSerializer::to_json(graph) == before);
            REQUIRE(graph.nodes().size() == 1);
            REQUIRE(graph.is_prepared());
            REQUIRE(releases.load(std::memory_order_relaxed) == 0);

            graph.release();
            REQUIRE(releases.load(std::memory_order_relaxed) == 1);
        }
    }

    SECTION("custom removal with release leaves ordinary release as callback owner") {
        for (const bool quiesced : {false, true}) {
            std::atomic<int> releases{0};
            SignalGraph graph;
            auto type = make_prepared_edit_level_type("pulp.test.prepared.release", 1.0f);
            type.release = [&](void*) { releases.fetch_add(1, std::memory_order_relaxed); };
            REQUIRE(graph.register_custom_node_type(std::move(type)));
            const auto custom = graph.add_custom_node("pulp.test.prepared.release");
            REQUIRE(graph.prepare(48000.0, 8));
            const auto before = GraphSerializer::to_json(graph);

            auto edit = graph.begin_prepared_topology_edit();
            REQUIRE(edit->remove_node(custom));
            const auto result = quiesced ? edit->prepare_quiesced(48000.0, 8)
                                         : edit->prepare(48000.0, 8);
            REQUIRE(result == Result::BaselineCustomRemovalRequiresRelease);
            edit.reset();
            REQUIRE(GraphSerializer::to_json(graph) == before);
            REQUIRE(graph.nodes().size() == 1);
            REQUIRE(graph.is_prepared());
            REQUIRE(releases.load(std::memory_order_relaxed) == 0);

            graph.release();
            REQUIRE(releases.load(std::memory_order_relaxed) == 1);
        }
    }
}

TEST_CASE("SignalGraph prepared edit rejects snapshot-local MIDI output before mutation",
          "[host][graph][prepared-edit][midi-output]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;

    SECTION("candidate-only MIDI output is rejected without consuming an ID") {
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->add_midi_output_node("candidate output") == 1);
        REQUIRE(edit->prepare(48000.0, 8)
                == Result::MidiOutputSnapshotLocalRequired);
        edit.reset();
        REQUIRE(graph.nodes().empty());
        REQUIRE_FALSE(graph.is_prepared());
        REQUIRE(graph.add_input_node(1, "owner input") == 1);
    }

    SECTION("live pending note-off remains on the old snapshot and drains once") {
        const auto midi_input = graph.add_midi_input_node("midi input");
        const auto midi_output = graph.add_midi_output_node("midi output");
        REQUIRE(graph.connect_midi(midi_input, midi_output));
        REQUIRE(graph.prepare(48000.0, 8));

        pulp::midi::MidiBuffer pending;
        pending.reserve(1);
        pending.add(pulp::midi::MidiEvent::note_off(0, 64, 0));
        REQUIRE(graph.inject_midi(midi_input, pending));
        float input_sample = 0.0f;
        float output_sample = 0.0f;
        const float* input_ptrs[] = {&input_sample};
        float* output_ptrs[] = {&output_sample};
        pulp::audio::BufferView<const float> in(input_ptrs, 0, 8);
        pulp::audio::BufferView<float> out(output_ptrs, 0, 8);
        graph.process(out, in, 8);

        const auto graph_before = GraphSerializer::to_json(graph);
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->remove_node(midi_output));
        REQUIRE(edit->prepare(48000.0, 8)
                == Result::MidiOutputSnapshotLocalRequired);
        edit.reset();
        REQUIRE(GraphSerializer::to_json(graph) == graph_before);
        REQUIRE(graph.nodes().size() == 2);
        REQUIRE(graph.connections().size() == 1);
        REQUIRE(graph.is_prepared());

        pulp::midi::MidiBuffer arrived;
        arrived.reserve(1);
        REQUIRE(graph.extract_midi(midi_output, arrived));
        REQUIRE(arrived.size() == 1);
        REQUIRE(arrived[0].is_note_off());
        arrived.clear();
        graph.process(out, in, 8);
        REQUIRE(graph.extract_midi(midi_output, arrived));
        REQUIRE(arrived.empty());
    }
}

TEST_CASE("SignalGraph prepared edit publishes pending ingress exactly once",
          "[host][graph][prepared-edit][ingress]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;

    SECTION("MIDI input published after prepare reaches the retained plugin once") {
        SignalGraph graph;
        const auto midi_input = graph.add_midi_input_node("midi input");
        auto slot = std::make_unique<MidiForwarder>();
        auto* probe = slot.get();
        const auto plugin = graph.add_plugin_node(std::move(slot), 0, 0, "probe");
        REQUIRE(graph.connect_midi(midi_input, plugin));
        REQUIRE(graph.prepare(48000.0, 8));

        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->add_gain_node("candidate only") != 0);
        REQUIRE(edit->prepare(48000.0, 8) == Result::Prepared);
        pulp::midi::MidiBuffer pending;
        pending.reserve(1);
        auto note = pulp::midi::MidiEvent::note_on(0, 67, 64);
        note.sample_offset = 3;
        pending.add(note);
        REQUIRE(graph.inject_midi(midi_input, pending));
        REQUIRE(edit->commit() == Result::Committed);

        pulp::audio::BufferView<const float> in;
        pulp::audio::BufferView<float> out;
        graph.process(out, in, 8);
        REQUIRE(probe->last_seen().size() == 1);
        REQUIRE(probe->last_seen()[0].is_note_on());
        REQUIRE(probe->last_seen()[0].sample_offset == 3);
        graph.process(out, in, 8);
        REQUIRE(probe->last_seen().empty());
    }

    SECTION("parameter batch published after prepare reaches the retained plugin once") {
        SignalGraph graph;
        auto slot = std::make_unique<ParameterMailboxProbe>();
        auto* probe = slot.get();
        const auto plugin = graph.add_plugin_node(std::move(slot), 1, 1, "probe");
        REQUIRE(graph.prepare(48000.0, 8));

        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->add_gain_node("candidate only") != 0);
        REQUIRE(edit->prepare(48000.0, 8) == Result::Prepared);
        pulp::host::ParameterEventQueue pending;
        REQUIRE(pending.push({ParameterMailboxProbe::kParamId, 3, 0.75f, 0}));
        REQUIRE(graph.inject_parameter_events(plugin, pending));
        REQUIRE(edit->commit() == Result::Committed);

        pulp::audio::BufferView<const float> in;
        pulp::audio::BufferView<float> out;
        graph.process(out, in, 8);
        REQUIRE(probe->received_count() == 1);
        REQUIRE(probe->received(0).sample_offset == 3);
        REQUIRE(probe->received(0).value == 0.75f);
        graph.process(out, in, 8);
        REQUIRE(probe->received_count() == 0);
    }
}

TEST_CASE("SignalGraph prepared edit preserves a concurrent telemetry toggle",
          "[host][graph][prepared-edit][telemetry]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    static_assert(noexcept(
        std::declval<pulp::audio::LiveDspTelemetryStore&>().set_enabled(true)));

    SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    const auto gain = graph.add_gain_node("gain");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 8));
    REQUIRE_FALSE(graph.live_dsp_telemetry_enabled());

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->prepare(48000.0, 8) == Result::Prepared);
    std::size_t toggle_allocations = 1;
    {
        pulp::test::RtAllocationProbe probe;
        graph.set_live_dsp_telemetry_enabled(true);
        toggle_allocations = probe.allocation_count();
    }
    REQUIRE(toggle_allocations == 0);
    REQUIRE(edit->commit() == Result::Committed);

    std::array<float, 8> source{};
    source.fill(0.25f);
    std::array<float, 8> rendered{};
    const float* source_ptrs[] = {source.data()};
    float* rendered_ptrs[] = {rendered.data()};
    pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
    pulp::audio::BufferView<float> out(rendered_ptrs, 1, rendered.size());
    graph.process(out, in, 8);

    const auto telemetry = graph.poll_live_dsp_telemetry();
    REQUIRE(graph.live_dsp_telemetry_enabled());
    REQUIRE(telemetry.enabled);
    REQUIRE(telemetry.blocks_written == 1);
    REQUIRE(telemetry.blocks_drained == 1);

    // The opposite linearization is equally coherent: a toggle after prepared
    // publication updates the newly live snapshot, not the retired one.
    graph.set_live_dsp_telemetry_enabled(false);
    graph.process(out, in, 8);
    const auto disabled_telemetry = graph.poll_live_dsp_telemetry();
    REQUIRE_FALSE(graph.live_dsp_telemetry_enabled());
    REQUIRE_FALSE(disabled_telemetry.enabled);
    REQUIRE(disabled_telemetry.blocks_written == 1);
    REQUIRE(disabled_telemetry.blocks_drained == 1);
}

TEST_CASE("SignalGraph prepared publication serializes telemetry toggle overlap",
          "[host][graph][prepared-edit][telemetry][threading]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 8));

    constexpr int kIterations = 64;
    for (int i = 0; i < kIterations; ++i) {
        graph.set_live_dsp_telemetry_enabled(false);
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare(48000.0, 8) == Result::Prepared);

        std::atomic<bool> start{false};
        std::thread toggler([&] {
            // unbounded-wait: allow the flag is published immediately by this test thread before join
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            graph.set_live_dsp_telemetry_enabled(true);
        });
        start.store(true, std::memory_order_release);
        REQUIRE(edit->commit() == Result::Committed);
        toggler.join();

        const auto telemetry = graph.poll_live_dsp_telemetry();
        REQUIRE(graph.live_dsp_telemetry_enabled());
        REQUIRE(telemetry.enabled);
    }
}

TEST_CASE("SignalGraph prepared edit adopts only unchanged PDC intersection",
          "[host][graph][prepared-edit][pdc]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    const auto latency = graph.add_plugin_node(
        std::make_unique<MockLatencyPlugin>(2, 1), 1, 1, "latency");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, output, 0));
    REQUIRE(graph.connect(input, 0, latency, 0));
    REQUIRE(graph.connect(latency, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 4));
    REQUIRE(graph.latency_samples() == 2);

    std::array<float, 4> source{};
    source.fill(1.0f);
    std::array<float, 4> rendered{};
    const float* source_ptrs[] = {source.data()};
    float* rendered_ptrs[] = {rendered.data()};
    pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
    pulp::audio::BufferView<float> out(rendered_ptrs, 1, rendered.size());
    graph.process(out, in, 4); // prime the plugin and host delay histories

    auto add = graph.begin_prepared_topology_edit();
    const auto gain = add->add_gain_node("new PDC branch");
    REQUIRE(add->connect(input, 0, gain, 0));
    REQUIRE(add->connect(gain, 0, output, 0));
    REQUIRE(add->prepare(48000.0, 4) == Result::Prepared);
    REQUIRE(add->routed_execution_ready(4));
    REQUIRE(add->commit() == Result::Committed);
    REQUIRE(graph.is_prepared());
    REQUIRE(graph.latency_samples() == 2);
    REQUIRE(graph.connections().size() == 5);
    graph.process(out, in, 4);
    REQUIRE(graph.routed_walk_fallbacks() == 0);
    // Existing direct-edge history and plugin state carry (2.0 throughout);
    // the newly delayed gain branch starts at zero and reaches 1.0 after two.
    REQUIRE(rendered == (std::array<float, 4>{2.0f, 2.0f, 3.0f, 3.0f}));
    auto remove = graph.begin_prepared_topology_edit();
    REQUIRE(remove->remove_node(gain));
    REQUIRE(remove->prepare(48000.0, 4) == Result::Prepared);
    REQUIRE(remove->routed_execution_ready(4));
    REQUIRE(remove->commit() == Result::Committed);
    REQUIRE(graph.connections().size() == 3);
    graph.process(out, in, 4);
    REQUIRE(graph.routed_walk_fallbacks() == 0);
    REQUIRE(rendered == (std::array<float, 4>{2.0f, 2.0f, 2.0f, 2.0f}));

    auto reconnect = graph.begin_prepared_topology_edit();
    REQUIRE(reconnect->disconnect(input, 0, output, 0));
    REQUIRE(reconnect->connect(input, 0, output, 0)); // equal value, new identity
    REQUIRE(reconnect->prepare(48000.0, 4) == Result::Prepared);
    REQUIRE(reconnect->routed_execution_ready(4));
    REQUIRE(reconnect->commit() == Result::Committed);
    graph.process(out, in, 4);
    REQUIRE(graph.routed_walk_fallbacks() == 0);
    // The equal-looking reconnected edge must not receive the retired ring.
    REQUIRE(rendered == (std::array<float, 4>{1.0f, 1.0f, 2.0f, 2.0f}));
}

TEST_CASE("SignalGraph prepared edit feedback rejection preserves live graph",
          "[host][graph][prepared-edit][feedback]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    const auto gain = graph.add_gain_node("gain");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    REQUIRE(graph.connect_feedback(gain, 0, gain, 0));
    REQUIRE(graph.prepare(48000.0, 4));
    const auto connections_before = graph.connections();

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->add_gain_node("candidate only") != 0);
    REQUIRE(edit->prepare(48000.0, 4) == Result::RuntimeAdoptionFailed);
    edit.reset();
    REQUIRE(graph.connections() == connections_before);
    REQUIRE(graph.nodes().size() == 3);
    REQUIRE(graph.is_prepared());
}

namespace {
constexpr std::uint32_t kDenseHostGainParam = 0x4844u;

class DenseHostGainProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor descriptor;
        descriptor.name = "DenseHostGain";
        descriptor.manufacturer = "Pulp";
        descriptor.bundle_id = "dev.pulp.test.dense-host-gain";
        descriptor.version = "1.0.0";
        descriptor.category = pulp::format::PluginCategory::Effect;
        descriptor.input_buses = {{"Main In", 1, false}};
        descriptor.output_buses = {{"Main Out", 1, false}};
        descriptor.node_capabilities.consumes_audio_rate_modulations = true;
        return descriptor;
    }

    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo gain;
        gain.id = kDenseHostGainParam;
        gain.name = "Gain";
        gain.range = {0.0f, 1.0f, 0.25f};
        gain.rate = pulp::state::ParamRate::AudioRate;
        store.add_parameter(gain);
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
    bool process_block(pulp::format::ProcessBlock& block) override {
        if (block.buses == nullptr || block.events == nullptr ||
            block.events->audio_rate_modulations.size() != 1) {
            return false;
        }
        const auto& lane = block.events->audio_rate_modulations.front();
        const auto* input =
            block.buses->first(pulp::format::BusDirection::Input, pulp::format::BusRole::Main);
        auto* output =
            block.buses->first(pulp::format::BusDirection::Output, pulp::format::BusRole::Main);
        if (lane.param_id != kDenseHostGainParam || input == nullptr || output == nullptr ||
            lane.values.size() != block.frame_count) {
            return false;
        }
        for (std::uint32_t frame = 0; frame < block.frame_count; ++frame) {
            output->output.channel_ptr(0)[frame] =
                input->input.channel_ptr(0)[frame] * lane.values[frame];
        }
        return true;
    }
};

struct ProcessorLifecycleCounts {
    int prepares = 0;
    int releases = 0;
    int destructions = 0;
};

class LifecycleHostProcessor final : public pulp::format::Processor {
  public:
    explicit LifecycleHostProcessor(std::shared_ptr<ProcessorLifecycleCounts> counts)
        : counts_(std::move(counts)) {}
    ~LifecycleHostProcessor() override {
        ++counts_->destructions;
    }

    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor descriptor;
        descriptor.name = "LifecycleHostProcessor";
        descriptor.manufacturer = "Pulp";
        descriptor.bundle_id = "dev.pulp.test.lifecycle-host-processor";
        descriptor.version = "1.0.0";
        descriptor.category = pulp::format::PluginCategory::Effect;
        descriptor.input_buses = {{"Main In", 1, false}};
        descriptor.output_buses = {{"Main Out", 1, false}};
        return descriptor;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {
        ++counts_->prepares;
    }
    void release() override {
        ++counts_->releases;
    }
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        for (std::size_t channel = 0;
             channel < std::min(output.num_channels(), input.num_channels()); ++channel) {
            std::copy_n(input.channel_ptr(channel), output.num_samples(),
                        output.channel_ptr(channel));
        }
    }

  private:
    std::shared_ptr<ProcessorLifecycleCounts> counts_;
};
} // namespace

TEST_CASE("SignalGraph authors and prepares a dense Processor node",
          "[host][signal-graph][processor-node][audio-rate]") {
    constexpr int frames = 4096;
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "audio + modulation");
    auto instance =
        pulp::format::ProcessorNodeInstance::create(std::make_unique<DenseHostGainProcessor>());
    REQUIRE(instance);
    const auto processor = graph.add_processor_node(instance);
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(processor != 0);
    REQUIRE(graph.add_processor_node(instance) == 0);
    REQUIRE(graph.is_processor_node(processor));
    REQUIRE(graph.connect(input, 0, processor, 0));
    REQUIRE(
        graph.connect_audio_rate_modulation(input, 1, processor, kDenseHostGainParam, 0.0f, 1.0f));
    REQUIRE(graph.connect(processor, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, frames));
    const auto baked = bake(graph);
    REQUIRE_FALSE(baked.accepted);
    REQUIRE(baked.reason == LowerRejectReason::HostedPluginNotSelfContained);

    std::vector<float> audio(frames, 0.5f);
    std::vector<float> modulation(frames);
    std::vector<float> rendered(frames, -1.0f);
    for (int frame = 0; frame < frames; ++frame) {
        modulation[frame] = static_cast<float>(frame) / static_cast<float>(frames - 1);
    }
    const float* input_channels[] = {audio.data(), modulation.data()};
    float* output_channels[] = {rendered.data()};
    pulp::audio::BufferView<const float> in(input_channels, 2, frames);
    pulp::audio::BufferView<float> out(output_channels, 1, frames);
    graph.process(out, in, frames);
    REQUIRE(graph.routed_walk_fallbacks() == 0);
    for (int frame = 0; frame < frames; ++frame) {
        REQUIRE_THAT(rendered[frame], WithinAbs(0.5f * modulation[frame], 1.0e-7f));
    }
}

TEST_CASE("PreparedTopologyEdit mirrors Processor-node authoring",
          "[host][signal-graph][processor-node][prepared-edit]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;
    REQUIRE(graph.prepare(48000.0, 64));
    auto edit = graph.begin_prepared_topology_edit();
    const auto input = edit->add_input_node(2, "input");
    const auto processor = edit->add_processor_node(std::make_unique<DenseHostGainProcessor>());
    const auto output = edit->add_output_node(1, "output");
    REQUIRE(processor != 0);
    REQUIRE(edit->connect(input, 0, processor, 0));
    REQUIRE(edit->connect(processor, 0, output, 0));
    REQUIRE(edit->prepare(48000.0, 64) == Result::Prepared);
    REQUIRE(edit->commit() == Result::Committed);
    REQUIRE(graph.is_processor_node(processor));
}

TEST_CASE("Processor-node release is balanced across graph and edit lifetimes",
          "[host][signal-graph][processor-node][lifecycle]") {
    SECTION("removal waits for the last executable snapshot despite caller retention") {
        auto counts = std::make_shared<ProcessorLifecycleCounts>();
        auto instance = pulp::format::ProcessorNodeInstance::create(
            std::make_unique<LifecycleHostProcessor>(counts));
        REQUIRE(instance);
        SignalGraph graph;
        REQUIRE(graph.prepare(48000.0, 64));
        auto edit = graph.begin_prepared_topology_edit();
        const auto processor = edit->add_processor_node(instance);
        REQUIRE(processor != 0);
        REQUIRE(edit->prepare(48000.0, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
        REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
        auto pinned = edit->committed_execution_snapshot();
        REQUIRE(pinned);
        edit.reset();
        REQUIRE(counts->prepares == 1);

        REQUIRE(graph.remove_node(processor));
        REQUIRE(counts->releases == 0);
        pinned = {};
        REQUIRE(counts->releases == 1);

        graph.release();
        instance.reset();
        REQUIRE(counts->releases == 1);
        REQUIRE(counts->destructions == 1);
    }

    SECTION("graph release is idempotent after snapshot quiescence") {
        auto counts = std::make_shared<ProcessorLifecycleCounts>();
        auto instance = pulp::format::ProcessorNodeInstance::create(
            std::make_unique<LifecycleHostProcessor>(counts));
        REQUIRE(instance);
        SignalGraph graph;
        REQUIRE(graph.add_processor_node(instance) != 0);
        REQUIRE(graph.prepare(48000.0, 64));
        REQUIRE(counts->prepares == 1);
        graph.release();
        graph.release();
        REQUIRE(counts->releases == 1);
        graph.clear();
        instance.reset();
        REQUIRE(counts->destructions == 1);
    }

    SECTION("prepared-edit rollback releases a caller-retained instance") {
        auto counts = std::make_shared<ProcessorLifecycleCounts>();
        auto instance = pulp::format::ProcessorNodeInstance::create(
            std::make_unique<LifecycleHostProcessor>(counts));
        REQUIRE(instance);
        SignalGraph graph;
        REQUIRE(graph.prepare(48000.0, 64));
        {
            auto edit = graph.begin_prepared_topology_edit();
            REQUIRE(edit->add_processor_node(instance) != 0);
            REQUIRE(edit->prepare(48000.0, 64) ==
                    SignalGraph::PreparedTopologyEdit::Result::Prepared);
            REQUIRE(counts->prepares == 1);
        }
        REQUIRE(counts->releases == 1);
        instance.reset();
        REQUIRE(counts->destructions == 1);
    }

    SECTION("quiesced rollback restores a retained instance to base dimensions") {
        auto counts = std::make_shared<ProcessorLifecycleCounts>();
        auto instance = pulp::format::ProcessorNodeInstance::create(
            std::make_unique<LifecycleHostProcessor>(counts));
        REQUIRE(instance);
        SignalGraph graph;
        REQUIRE(graph.add_processor_node(instance) != 0);
        REQUIRE(graph.prepare(48000.0, 64));
        {
            auto edit = graph.begin_prepared_topology_edit();
            REQUIRE(edit->add_gain_node("candidate") != 0);
            REQUIRE(edit->prepare_quiesced(96000.0, 128) ==
                    SignalGraph::PreparedTopologyEdit::Result::Prepared);
            REQUIRE(counts->prepares == 2);
            REQUIRE(counts->releases == 1);
        }
        REQUIRE(counts->prepares == 3);
        REQUIRE(counts->releases == 2);
        graph.release();
        REQUIRE(counts->releases == 3);
        graph.clear();
        instance.reset();
        REQUIRE(counts->destructions == 1);
    }
}
