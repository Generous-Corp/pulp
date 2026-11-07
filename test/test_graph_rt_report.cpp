// Static realtime report: declared contracts, latency semantics, measured
// counted work, and the "holds at block size N" verdict.

#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/graph_rt_report.hpp>
#include <pulp/host/signal_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <memory>
#include <string>

namespace {

using pulp::audio::RtSafetyClass;
using pulp::host::CustomNodeType;
using pulp::host::GraphRtAllocationStatus;
using pulp::host::GraphRtContractSource;
using pulp::host::GraphRtVerdict;
using pulp::host::NodeId;
using pulp::host::SignalGraph;
using pulp::signal::rt::RtWorkCounts;

constexpr double kSampleRate = 48000.0;
constexpr int kMaxBlock = 512;

// A passthrough whose process() counts one FFT per call, so the measured pass
// has something deterministic to observe in a counting build.
CustomNodeType counted_type(bool declared, RtSafetyClass safety, bool allocation_free) {
    CustomNodeType t;
    t.type_id = "pulp.test.counted";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Counted";
    t.process = [](pulp::audio::BufferView<float>& out,
                   const pulp::audio::BufferView<const float>& in, int n) {
        pulp::signal::rt::count_fft();
        std::copy(in.channel_ptr(0), in.channel_ptr(0) + n, out.channel_ptr(0));
    };
    t.latency_samples = [](double) { return 32; };
    t.intentional_delay_samples = [](double) { return 4800; };
    t.rt_contract.declared = declared;
    t.rt_contract.safety_class = safety;
    t.rt_contract.allocation_free = allocation_free;
    t.rt_contract.worst_case_work = [](int) { return RtWorkCounts{1, 0, 0}; };
    t.rt_contract.loop_bound = "one transform per process() call";
    return t;
}

struct Fixture {
    SignalGraph graph;
    NodeId in = 0, node = 0, gain = 0, out = 0;

    explicit Fixture(const CustomNodeType& type) {
        REQUIRE(graph.register_custom_node_type(type));
        in = graph.add_input_node(1, "In");
        node = graph.add_custom_node(type.type_id, "Counted");
        gain = graph.add_gain_node("Dry");
        out = graph.add_output_node(1, "Out");
        REQUIRE(graph.connect(in, 0, node, 0));
        REQUIRE(graph.connect(in, 0, gain, 0));
        REQUIRE(graph.connect(node, 0, out, 0));
        REQUIRE(graph.connect(gain, 0, out, 0));
        REQUIRE(graph.prepare(kSampleRate, kMaxBlock));
    }
};

const pulp::host::GraphRtNodeReport* find_node(const pulp::host::GraphRtReport& report, NodeId id) {
    for (const auto& node : report.nodes)
        if (node.node_id == id)
            return &node;
    return nullptr;
}

bool has_reason_containing(const pulp::host::GraphRtReport& report, const std::string& text) {
    return std::any_of(report.reasons.begin(), report.reasons.end(),
                       [&](const std::string& r) { return r.find(text) != std::string::npos; });
}

} // namespace

TEST_CASE("Graph realtime report holds for declared realtime-safe nodes",
          "[host][graph][rt-report]") {
    Fixture f(counted_type(true, RtSafetyClass::AudioCallbackSafeAfterPrepare, true));
    const auto report = pulp::host::build_graph_rt_report(f.graph);

    REQUIRE(report.prepared);
    CHECK(report.block_size == kMaxBlock);
    CHECK(report.verdict == GraphRtVerdict::Holds);
    CHECK(report.reasons.empty());
    CHECK(report.total_latency_samples == 32);
    CHECK(report.total_intentional_delay_samples == 4800);
    CHECK(report.delayed_connections == 1);
    CHECK(report.compensation_delay_samples == 32);
    REQUIRE(report.declared_worst_case_work.has_value());
    CHECK(*report.declared_worst_case_work == RtWorkCounts{1, 0, 0});

    const auto* counted = find_node(report, f.node);
    REQUIRE(counted != nullptr);
    CHECK(counted->contract_source == GraphRtContractSource::Declared);
    CHECK(counted->allocation == GraphRtAllocationStatus::Declared);
    CHECK(counted->artifact_latency_samples == 32);
    CHECK(counted->intentional_delay_samples == 4800);
    CHECK(counted->loop_bound == "one transform per process() call");

    const auto* gain = find_node(report, f.gain);
    REQUIRE(gain != nullptr);
    CHECK(gain->contract_source == GraphRtContractSource::Framework);
    CHECK(gain->allocation == GraphRtAllocationStatus::Proven);
}

TEST_CASE("Graph realtime report is unknown when a node declares no contract",
          "[host][graph][rt-report]") {
    Fixture f(counted_type(false, RtSafetyClass::AudioCallbackSafe, true));
    const auto report = pulp::host::build_graph_rt_report(f.graph);
    CHECK(report.verdict == GraphRtVerdict::Unknown);
    CHECK(has_reason_containing(report, "declares no realtime contract"));
    CHECK_FALSE(report.declared_worst_case_work.has_value());
}

TEST_CASE("Graph realtime report does not hold for an unsafe or allocating node",
          "[host][graph][rt-report]") {
    SECTION("control-thread-only class") {
        Fixture f(counted_type(true, RtSafetyClass::ControlThreadOnly, true));
        const auto report = pulp::host::build_graph_rt_report(f.graph);
        CHECK(report.verdict == GraphRtVerdict::DoesNotHold);
        CHECK(has_reason_containing(report, "control-thread-only"));
    }
    SECTION("may allocate") {
        Fixture f(counted_type(true, RtSafetyClass::AudioCallbackSafe, false));
        const auto report = pulp::host::build_graph_rt_report(f.graph);
        CHECK(report.verdict == GraphRtVerdict::DoesNotHold);
        CHECK(has_reason_containing(report, "allocation-free"));
    }
}

TEST_CASE("Graph realtime report checks block size and work budget", "[host][graph][rt-report]") {
    Fixture f(counted_type(true, RtSafetyClass::AudioCallbackSafe, true));
    pulp::host::GraphRtReportOptions options;

    options.block_size = kMaxBlock * 2;
    auto report = pulp::host::build_graph_rt_report(f.graph, options);
    CHECK(report.verdict == GraphRtVerdict::DoesNotHold);
    CHECK(has_reason_containing(report, "outside the prepared range"));

    options.block_size = 128;
    options.work_budget_per_block = RtWorkCounts{0, 100, 100};
    report = pulp::host::build_graph_rt_report(f.graph, options);
    CHECK(report.block_size == 128);
    CHECK(report.verdict == GraphRtVerdict::DoesNotHold);
    CHECK(has_reason_containing(report, "declared worst-case work exceeds"));

    options.work_budget_per_block = RtWorkCounts{1, 0, 0};
    report = pulp::host::build_graph_rt_report(f.graph, options);
    CHECK(report.verdict == GraphRtVerdict::Holds);
}

TEST_CASE("Graph realtime report measures counted work without allocating",
          "[host][graph][rt-report]") {
    Fixture f(counted_type(true, RtSafetyClass::AudioCallbackSafeAfterPrepare, true));
    auto report = pulp::host::build_graph_rt_report(f.graph);

    pulp::test::RtAllocationProbe probe;
    pulp::host::GraphRtMeasureOptions measure;
    measure.blocks = 8;
    measure.allocation_count = [&probe] {
        return static_cast<std::uint64_t>(probe.allocation_count());
    };
    const auto measured = pulp::host::measure_graph_rt_work(f.graph, measure);
    REQUIRE(measured.has_value());
    CHECK(measured->blocks == 8);
    REQUIRE(measured->allocations.has_value());
    CHECK(*measured->allocations == 0);
    if (pulp::signal::rt::kWorkCountersEnabled) {
        CHECK(measured->work_counters_enabled);
        CHECK(measured->worst_block.fft == 1);
        CHECK(measured->total.fft == 8);
    }

    pulp::host::apply_graph_rt_measurement(report, *measured);
    CHECK(report.verdict == GraphRtVerdict::Holds);

    pulp::host::GraphRtMeasurement allocating = *measured;
    allocating.allocations = 3;
    pulp::host::apply_graph_rt_measurement(report, allocating);
    CHECK(report.verdict == GraphRtVerdict::DoesNotHold);
    CHECK(has_reason_containing(report, "allocated 3"));
}

TEST_CASE("Graph realtime report JSON is stable and machine-readable", "[host][graph][rt-report]") {
    Fixture f(counted_type(true, RtSafetyClass::AudioCallbackSafeAfterPrepare, true));
    const auto json =
        pulp::host::graph_rt_report_to_json(pulp::host::build_graph_rt_report(f.graph));
    const auto doc = choc::json::parse(json);
    CHECK(doc["schema"].getString() == "pulp.graph.rt-report");
    CHECK(doc["schema_version"].getWithDefault<int>(0) == 1);
    CHECK(doc["verdict"].getString() == "holds");
    CHECK(doc["latency"]["total_latency_samples"].getWithDefault<std::int64_t>(-1) == 32);
    CHECK(doc["latency"]["total_intentional_delay_samples"].getWithDefault<std::int64_t>(-1) ==
          4800);
    CHECK(doc["measured"].isVoid());
    REQUIRE(doc["nodes"].isArray());
    bool saw_custom = false;
    for (const auto node : doc["nodes"]) {
        if (node["kind"].getString() != "custom")
            continue;
        saw_custom = true;
        CHECK(node["contract"].getString() == "declared");
        CHECK(node["rt_safety_class"].getString() == "audio-callback-safe-after-prepare");
        CHECK(node["declared_worst_case_work"]["fft"].getWithDefault<std::int64_t>(-1) == 1);
        CHECK(node["intentional_delay_samples"].getWithDefault<std::int64_t>(-1) == 4800);
    }
    CHECK(saw_custom);
}

TEST_CASE("Graph realtime report of an unprepared graph is unknown", "[host][graph][rt-report]") {
    SignalGraph graph;
    graph.add_input_node(1, "In");
    const auto report = pulp::host::build_graph_rt_report(graph);
    CHECK_FALSE(report.prepared);
    CHECK(report.verdict == GraphRtVerdict::Unknown);
    CHECK(has_reason_containing(report, "not prepared"));

    SignalGraph mutable_graph;
    CHECK_FALSE(pulp::host::measure_graph_rt_work(mutable_graph).has_value());
}
