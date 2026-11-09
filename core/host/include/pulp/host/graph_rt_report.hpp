#pragma once

// Static realtime report for a prepared SignalGraph.
//
// The report joins three sources and labels every value with where it came
// from, so a reader never mistakes a claim for a proof:
//   - framework contracts for built-in nodes (input, output, gain, MIDI I/O,
//     spectral stages), whose process paths are covered by the graph's
//     allocation-free test suite;
//   - declared contracts on custom node types (CustomNodeType::rt_contract);
//   - an optional measured pass that renders silent-then-impulse blocks
//     through the graph and reads the rt_work_counter.hpp counters (and, when
//     the caller supplies one, an allocation counter).
// Hosted plugins are external code with no contract: they always report as
// unknown, and a graph containing one can never be reported as holding.
//
// Everything here is control-thread only. measure_graph_rt_work() calls
// SignalGraph::process() itself and must not run while the graph is live in an
// audio callback.

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/signal/rt_work_counter.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pulp::host {

// Where a node's realtime contract came from.
enum class GraphRtContractSource : std::uint8_t {
    Framework,  // built-in node; contract owned by the graph itself
    Declared,   // CustomNodeType::rt_contract with declared == true
    Undeclared, // custom node without a contract, or an unresolved type
    External,   // hosted plugin: external code, nothing can be claimed
};

enum class GraphRtAllocationStatus : std::uint8_t {
    Proven,   // framework node covered by the allocation-free test suite
    Declared, // registrar asserts allocation-free; not verified here
    Unproven, // no claim, or a claim of allocation
};

enum class GraphRtVerdict : std::uint8_t {
    Holds,       // every node is RT-safe by contract and fits the block size
    DoesNotHold, // a node is not RT-safe, or the block exceeds the prepared max
    Unknown,     // at least one node has no usable contract
};

struct GraphRtNodeReport {
    NodeId node_id = 0;
    std::string name;
    std::string kind; // "audio_input", "gain", "custom", "plugin", ...
    std::string custom_type_id;
    int custom_type_version = 0;
    GraphRtContractSource contract_source = GraphRtContractSource::Undeclared;
    std::optional<audio::RtSafetyClass> safety_class;
    GraphRtAllocationStatus allocation = GraphRtAllocationStatus::Unproven;
    // Declared worst-case counted work for one block of report.block_size.
    std::optional<signal::rt::RtWorkCounts> declared_worst_case_work;
    std::string loop_bound;
    std::int64_t artifact_latency_samples = 0;
    std::int64_t intentional_delay_samples = 0;
    std::vector<std::int64_t> output_port_latency_samples;
    std::int64_t input_latency_samples = 0;
    std::int64_t output_latency_samples = 0;
};

struct GraphRtMeasurement {
    bool work_counters_enabled = false;
    int blocks = 0;
    // Worst single block observed for each counter, and the total.
    signal::rt::RtWorkCounts worst_block{};
    signal::rt::RtWorkCounts total{};
    // Present only when the caller supplied an allocation counter.
    std::optional<std::uint64_t> allocations;
};

struct GraphRtReport {
    bool prepared = false;
    double sample_rate = 0.0;
    int prepared_max_block_size = 0;
    int block_size = 0;
    std::int64_t total_latency_samples = 0;
    std::int64_t total_intentional_delay_samples = 0; // informational sum
    int delayed_connections = 0;
    std::int64_t compensation_delay_samples = 0; // sum over delayed connections
    std::vector<GraphRtNodeReport> nodes;
    // Sum of declared worst-case work, present only when every node with
    // counted work declares it (framework nodes declare zero).
    std::optional<signal::rt::RtWorkCounts> declared_worst_case_work;
    std::optional<GraphRtMeasurement> measured;
    GraphRtVerdict verdict = GraphRtVerdict::Unknown;
    std::vector<std::string> reasons; // why the verdict is not Holds
};

struct GraphRtReportOptions {
    // Block size the estimate is made for; 0 means the prepared maximum.
    int block_size = 0;
    // Optional counted-work budget per block. When set, a declared or measured
    // worst case above any of its counters makes the verdict DoesNotHold.
    std::optional<signal::rt::RtWorkCounts> work_budget_per_block;
};

// Builds the static report from the graph's prepared snapshot and declared
// contracts. An unprepared graph returns prepared == false and verdict Unknown.
GraphRtReport build_graph_rt_report(const SignalGraph& graph,
                                    const GraphRtReportOptions& options = {});

struct GraphRtMeasureOptions {
    int blocks = 16;
    int block_size = 0; // 0 means the prepared maximum
    // Optional process-wide allocation counter (for example a test harness
    // operator-new hook). Read before and after the measured blocks.
    std::function<std::uint64_t()> allocation_count;
};

// Renders `blocks` blocks (an impulse in the first, silence after) through the
// prepared graph and records counted work per block. Returns nullopt when the
// graph is unprepared or the block size is out of range. Control thread only;
// it advances node state exactly like a real render.
std::optional<GraphRtMeasurement> measure_graph_rt_work(SignalGraph& graph,
                                                        const GraphRtMeasureOptions& options = {});

// Attaches a measurement to a report and re-evaluates the verdict against it.
void apply_graph_rt_measurement(GraphRtReport& report, const GraphRtMeasurement& measured,
                                const GraphRtReportOptions& options = {});

std::string_view graph_rt_verdict_name(GraphRtVerdict verdict) noexcept;
std::string_view graph_rt_contract_source_name(GraphRtContractSource source) noexcept;
std::string_view graph_rt_allocation_status_name(GraphRtAllocationStatus status) noexcept;

// Stable, machine-readable JSON (schema "pulp.graph.rt-report", version 1).
std::string graph_rt_report_to_json(const GraphRtReport& report);

} // namespace pulp::host
