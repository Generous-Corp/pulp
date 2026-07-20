#include <catch2/catch_test_macros.hpp>

#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

using namespace pulp::host;

namespace {

class LatencySlot final : public PluginSlot {
public:
    LatencySlot(int latency, LatencyQuery query)
        : latency_(latency), query_(query) {
        info_.name = "Latency fixture";
        info_.format = PluginFormat::CLAP;
        info_.num_inputs = 1;
        info_.num_outputs = 1;
        info_.category = "Effect";
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&, int samples) override {
        const std::size_t channels = std::min(output.num_channels(), input.num_channels());
        for (std::size_t channel = 0; channel < channels; ++channel) {
            std::copy_n(input.channel_ptr(channel), samples,
                        output.channel_ptr(channel));
        }
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(std::uint32_t) const override { return 0.0f; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    int latency_samples() const override {
        latency_calls.fetch_add(1, std::memory_order_relaxed);
        return legacy_latency_;
    }
    int tail_samples() const override { return 0; }
    LatencyQuery latency_query() const override {
        query_calls.fetch_add(1, std::memory_order_relaxed);
        return legacy_query_;
    }
    LatencyReport latency_report() const override {
        report_calls.fetch_add(1, std::memory_order_relaxed);
        return {query_, latency_};
    }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

    mutable std::atomic<int> query_calls{0};
    mutable std::atomic<int> latency_calls{0};
    mutable std::atomic<int> report_calls{0};

    void set_legacy_split(int samples, LatencyQuery query) {
        legacy_latency_ = samples;
        legacy_query_ = query;
    }

private:
    PluginInfo info_;
    int latency_ = 0;
    LatencyQuery query_ = LatencyQuery::Available;
    int legacy_latency_ = latency_;
    LatencyQuery legacy_query_ = query_;
};

class LegacyLatencySlot final : public PluginSlot {
public:
    LegacyLatencySlot() {
        info_.name = "Legacy latency fixture";
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&, int samples) override {
        std::copy_n(input.channel_ptr(0), samples, output.channel_ptr(0));
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(std::uint32_t) const override { return 0.0f; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    int latency_samples() const override { return 17; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

private:
    PluginInfo info_;
};

void require_result(const LatencyToOutputResult& result,
                    LatencyToOutputResult::Status status,
                    std::int64_t samples = 0,
                    NodeId offending = 0) {
    REQUIRE(result.status == status);
    REQUIRE(result.samples == samples);
    REQUIRE(result.offending_node == offending);
}

} // namespace

TEST_CASE("latency-to-output distinguishes known zero from unavailable reports",
          "[host][signal-graph][latency-schedule]") {
    using Status = LatencyToOutputResult::Status;

    for (const auto query : {PluginSlot::LatencyQuery::Available,
                             PluginSlot::LatencyQuery::Unsupported,
                             PluginSlot::LatencyQuery::QueryFailed}) {
        SignalGraph graph;
        const auto input = graph.add_input_node(1);
        auto slot = std::make_unique<LatencySlot>(0, query);
        auto* observed = slot.get();
        const auto plugin = graph.add_plugin_node(std::move(slot), 1, 1);
        const auto output = graph.add_output_node(1);
        REQUIRE(graph.connect(input, 0, plugin, 0));
        REQUIRE(graph.connect(plugin, 0, output, 0));
        REQUIRE(graph.prepare(48000.0, 64));

        if (query == PluginSlot::LatencyQuery::Available) {
            require_result(graph.latency_to_output(input), Status::Available, 0);
        } else if (query == PluginSlot::LatencyQuery::Unsupported) {
            require_result(graph.latency_to_output(input), Status::Unsupported,
                           0, plugin);
        } else {
            require_result(graph.latency_to_output(input), Status::QueryFailed,
                           0, plugin);
        }
        REQUIRE(observed->latency_calls.load(std::memory_order_relaxed) == 0);
        REQUIRE(observed->query_calls.load(std::memory_order_relaxed) == 0);
        REQUIRE(observed->report_calls.load(std::memory_order_relaxed) == 1);
    }
}

TEST_CASE("latency capture uses the coherent one-shot report",
          "[host][signal-graph][latency-schedule][metadata-cache]") {
    using Status = LatencyToOutputResult::Status;
    SignalGraph graph;
    auto slot = std::make_unique<LatencySlot>(42, PluginSlot::LatencyQuery::Available);
    auto* observed = slot.get();
    observed->set_legacy_split(0, PluginSlot::LatencyQuery::Available);
    const auto input = graph.add_input_node(1);
    const auto plugin = graph.add_plugin_node(std::move(slot), 1, 1);
    const auto output = graph.add_output_node(1);
    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 64));

    require_result(graph.latency_to_output(input), Status::Available, 42);
    REQUIRE(observed->latency_calls.load(std::memory_order_relaxed) == 0);
    REQUIRE(observed->query_calls.load(std::memory_order_relaxed) == 0);
    REQUIRE(observed->report_calls.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("legacy split latency methods retain prepared graph PDC",
          "[host][signal-graph][latency-schedule][compatibility]") {
    using Status = LatencyToOutputResult::Status;
    SignalGraph graph;
    const auto input = graph.add_input_node(1);
    const auto plugin = graph.add_plugin_node(
        std::make_unique<LegacyLatencySlot>(), 1, 1);
    const auto output = graph.add_output_node(1);
    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 64));

    REQUIRE(graph.latency_samples() == 17);
    require_result(graph.latency_to_output(input), Status::Available, 17);
}

TEST_CASE("latency-to-output includes serial latency and compiled fan-in PDC",
          "[host][signal-graph][latency-schedule][pdc]") {
    using Status = LatencyToOutputResult::Status;
    SignalGraph graph;
    const auto input = graph.add_input_node(1);
    const auto short_path = graph.add_plugin_node(
        std::make_unique<LatencySlot>(10, PluginSlot::LatencyQuery::Available), 1, 1);
    const auto serial = graph.add_plugin_node(
        std::make_unique<LatencySlot>(5, PluginSlot::LatencyQuery::Available), 1, 1);
    const auto long_path = graph.add_plugin_node(
        std::make_unique<LatencySlot>(20, PluginSlot::LatencyQuery::Available), 1, 1);
    const auto output = graph.add_output_node(1);
    REQUIRE(graph.connect(input, 0, short_path, 0));
    REQUIRE(graph.connect(short_path, 0, serial, 0));
    REQUIRE(graph.connect(serial, 0, output, 0));
    REQUIRE(graph.connect(input, 0, long_path, 0));
    REQUIRE(graph.connect(long_path, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 64));

    require_result(graph.latency_to_output(input), Status::Available, 20);
    require_result(graph.latency_to_output(short_path), Status::Available, 20);
    require_result(graph.latency_to_output(serial), Status::Available, 10);
    require_result(graph.latency_to_output(long_path), Status::Available, 20);
    require_result(graph.latency_to_output(output), Status::Available, 0);
}

TEST_CASE("latency-to-output reports unreachable and divergent output paths",
          "[host][signal-graph][latency-schedule]") {
    using Status = LatencyToOutputResult::Status;
    SignalGraph graph;
    const auto input = graph.add_input_node(1);
    const auto ten = graph.add_plugin_node(
        std::make_unique<LatencySlot>(10, PluginSlot::LatencyQuery::Available), 1, 1);
    const auto twenty = graph.add_plugin_node(
        std::make_unique<LatencySlot>(20, PluginSlot::LatencyQuery::Available), 1, 1);
    const auto orphan = graph.add_gain_node("Orphan");
    const auto output_a = graph.add_output_node(1);
    const auto output_b = graph.add_output_node(1);
    REQUIRE(graph.connect(input, 0, ten, 0));
    REQUIRE(graph.connect(ten, 0, output_a, 0));
    REQUIRE(graph.connect(input, 0, twenty, 0));
    REQUIRE(graph.connect(twenty, 0, output_b, 0));
    REQUIRE(graph.prepare(48000.0, 64));

    require_result(graph.latency_to_output(input), Status::AmbiguousOutputLatency);
    require_result(graph.latency_to_output(orphan), Status::NoOutputPath);
    require_result(graph.latency_to_output(999999), Status::UnknownNode, 0, 999999);
}

TEST_CASE("latency-to-output distinguishes custom nodes without a latency contract",
          "[host][signal-graph][latency-schedule]") {
    using Boundary = NodeLatencyBoundary;
    using EditResult = SignalGraph::PreparedTopologyEdit::Result;
    using Status = LatencyToOutputResult::Status;
    SignalGraph graph;
    CustomNodeType type;
    type.type_id = "test.custom-latency-unknown";
    type.version = 1;
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.process = [](pulp::audio::BufferView<float>& output,
                      const pulp::audio::BufferView<const float>& input,
                      int samples) {
        std::copy_n(input.channel_ptr(0), samples, output.channel_ptr(0));
    };
    REQUIRE(graph.register_custom_node_type(std::move(type)));
    const auto input = graph.add_input_node(1);
    const auto custom = graph.add_custom_node("test.custom-latency-unknown");
    const auto latent = graph.add_plugin_node(
        std::make_unique<LatencySlot>(11, PluginSlot::LatencyQuery::Available), 1, 1);
    const auto output = graph.add_output_node(1);
    REQUIRE(graph.connect(input, 0, custom, 0));
    REQUIRE(graph.connect(custom, 0, output, 0));
    REQUIRE(graph.connect(input, 0, latent, 0));
    REQUIRE(graph.connect(latent, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 64));

    require_result(graph.latency_to_output(input), Status::Unsupported, 0, custom);
    require_result(graph.latency_to_output(custom, Boundary::Input),
                   Status::Unsupported, 0, custom);
    require_result(graph.latency_to_output(custom, Boundary::Output),
                   Status::Available, 11);

    auto edit = graph.begin_prepared_topology_edit();
    require_result(edit->prepared_latency_to_output(custom, Boundary::Output),
                   Status::NoCompiledSnapshot);
    REQUIRE(edit->prepare(48000.0, 64) == EditResult::Prepared);
    require_result(edit->prepared_latency_to_output(custom, Boundary::Input),
                   Status::Unsupported, 0, custom);
    require_result(edit->prepared_latency_to_output(custom, Boundary::Output),
                   Status::Available, 11);
    REQUIRE(edit->commit() == EditResult::Committed);
    require_result(edit->prepared_latency_to_output(custom, Boundary::Output),
                   Status::NoCompiledSnapshot);
}

TEST_CASE("latency-to-output distinguishes absent snapshots from graph paths",
          "[host][signal-graph][latency-schedule][snapshot]") {
    using Status = LatencyToOutputResult::Status;
    SignalGraph graph;
    const auto input = graph.add_input_node(1);
    require_result(graph.latency_to_output(input), Status::NoCompiledSnapshot);

    SignalGraph::ExecutionSnapshot empty;
    require_result(empty.latency_to_output(input), Status::NoCompiledSnapshot);
}

TEST_CASE("latency-to-output does not certify unresolved plugins as zero latency",
          "[host][signal-graph][latency-schedule]") {
    using Status = LatencyToOutputResult::Status;
    SignalGraph graph;
    const auto input = graph.add_input_node(1);
    const auto plugin = graph.add_unresolved_plugin_node({}, 1, 1, "Missing");
    const auto output = graph.add_output_node(1);
    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 64));

    require_result(graph.latency_to_output(input), Status::QueryFailed, 0, plugin);
}

TEST_CASE("latency-to-output excludes feedback from the schedulable path",
          "[host][signal-graph][latency-schedule][feedback]") {
    using Status = LatencyToOutputResult::Status;
    SignalGraph graph;
    const auto plugin = graph.add_plugin_node(
        std::make_unique<LatencySlot>(9, PluginSlot::LatencyQuery::Available), 1, 1);
    const auto output = graph.add_output_node(1);
    REQUIRE(graph.connect_feedback(plugin, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 64));

    require_result(graph.latency_to_output(plugin), Status::NoOutputPath);
}

TEST_CASE("latency-to-output status precedence is deterministic",
          "[host][signal-graph][latency-schedule]") {
    using Status = LatencyToOutputResult::Status;
    SignalGraph graph;
    const auto input = graph.add_input_node(1);
    const auto unsupported_high = graph.add_plugin_node(
        std::make_unique<LatencySlot>(0, PluginSlot::LatencyQuery::Unsupported), 1, 1);
    const auto failed = graph.add_plugin_node(
        std::make_unique<LatencySlot>(0, PluginSlot::LatencyQuery::QueryFailed), 1, 1);
    const auto unsupported_low = graph.add_plugin_node(
        std::make_unique<LatencySlot>(0, PluginSlot::LatencyQuery::Unsupported), 1, 1);
    const auto failed_high = graph.add_plugin_node(
        std::make_unique<LatencySlot>(0, PluginSlot::LatencyQuery::QueryFailed), 1, 1);
    const auto output = graph.add_output_node(1);
    for (const auto plugin : {unsupported_high, failed, unsupported_low, failed_high}) {
        REQUIRE(graph.connect(input, 0, plugin, 0));
        REQUIRE(graph.connect(plugin, 0, output, 0));
    }
    REQUIRE(graph.prepare(48000.0, 64));

    require_result(graph.latency_to_output(input), Status::QueryFailed, 0, failed);
}

TEST_CASE("execution snapshot keeps its latency schedule across a prepared edit",
          "[host][signal-graph][latency-schedule][snapshot]") {
    using EditResult = SignalGraph::PreparedTopologyEdit::Result;
    using Status = LatencyToOutputResult::Status;
    SignalGraph graph;
    const auto input = graph.add_input_node(1);
    const auto plugin = graph.add_plugin_node(
        std::make_unique<LatencySlot>(12, PluginSlot::LatencyQuery::Available), 1, 1);
    const auto output = graph.add_output_node(1);
    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 64));

    auto pin_edit = graph.begin_prepared_topology_edit();
    REQUIRE(pin_edit->prepare(48000.0, 64) == EditResult::Prepared);
    REQUIRE(pin_edit->commit() == EditResult::Committed);
    const auto pinned = pin_edit->committed_execution_snapshot();
    require_result(pinned.latency_to_output(input), Status::Available, 12);
    require_result(pinned.latency_to_output(999999), Status::UnknownNode, 0, 999999);

    auto disconnect_edit = graph.begin_prepared_topology_edit();
    REQUIRE(disconnect_edit->disconnect(plugin, 0, output, 0));
    REQUIRE(disconnect_edit->prepare(48000.0, 64) == EditResult::Prepared);
    REQUIRE(disconnect_edit->commit() == EditResult::Committed);

    require_result(graph.latency_to_output(input), Status::NoOutputPath);
    require_result(pinned.latency_to_output(input), Status::Available, 12);
}

TEST_CASE("latency schedule queries never poll a prepared plugin again",
          "[host][signal-graph][latency-schedule][metadata-cache]") {
    SignalGraph graph;
    auto slot = std::make_unique<LatencySlot>(7, PluginSlot::LatencyQuery::Available);
    auto* observed = slot.get();
    const auto input = graph.add_input_node(1);
    const auto plugin = graph.add_plugin_node(std::move(slot), 1, 1);
    const auto output = graph.add_output_node(1);
    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 64));
    observed->query_calls.store(0, std::memory_order_relaxed);
    observed->latency_calls.store(0, std::memory_order_relaxed);
    observed->report_calls.store(0, std::memory_order_relaxed);

    for (int iteration = 0; iteration < 32; ++iteration) {
        REQUIRE(graph.latency_to_output(input).samples == 7);
    }
    REQUIRE(observed->query_calls.load(std::memory_order_relaxed) == 0);
    REQUIRE(observed->latency_calls.load(std::memory_order_relaxed) == 0);
    REQUIRE(observed->report_calls.load(std::memory_order_relaxed) == 0);
}
