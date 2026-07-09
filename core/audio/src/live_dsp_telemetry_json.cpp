// live_dsp_telemetry_json.cpp — snapshot → JSON for the live per-node DSP
// telemetry dump.
//
// See live_dsp_telemetry_json.hpp for the schema contract. CHOC-first
// (choc::value / choc::json), mirroring audio_probe_json.cpp. NON-RT only —
// runs after drain() has published a snapshot on the owner thread.

#include <pulp/audio/live_dsp_telemetry_json.hpp>

#include <choc/text/choc_JSON.h>

namespace pulp::audio {

namespace {

choc::value::Value node_to_value(const LiveDspNodeSummary& node) {
    auto obj = choc::value::createObject("");
    obj.setMember("node_id", static_cast<std::int64_t>(node.node_id));
    obj.setMember("kind", std::string(to_string(node.kind)));
    obj.setMember("name", std::string(node.name));
    obj.setMember("sample_count", static_cast<std::int64_t>(node.sample_count));
    obj.setMember("last_elapsed_ns", static_cast<std::int64_t>(node.last_elapsed_ns));
    obj.setMember("min_elapsed_ns", static_cast<std::int64_t>(node.min_elapsed_ns));
    obj.setMember("max_elapsed_ns", static_cast<std::int64_t>(node.max_elapsed_ns));
    obj.setMember("mean_elapsed_ns", static_cast<std::int64_t>(node.mean_elapsed_ns));
    obj.setMember("p50_elapsed_ns", static_cast<std::int64_t>(node.p50_elapsed_ns));
    obj.setMember("p95_elapsed_ns", static_cast<std::int64_t>(node.p95_elapsed_ns));
    obj.setMember("p99_elapsed_ns", static_cast<std::int64_t>(node.p99_elapsed_ns));
    obj.setMember("jitter_ns", static_cast<std::int64_t>(node.jitter_ns));
    obj.setMember("last_budget_fraction", static_cast<double>(node.last_budget_fraction));
    obj.setMember("over_budget_attributions",
                  static_cast<std::int64_t>(node.over_budget_attributions));
    return obj;
}

}  // namespace

std::string live_dsp_telemetry_snapshot_to_json(const LiveDspTelemetrySnapshot& snapshot,
                                                bool pretty) {
    auto root = choc::value::createObject("");

    // Publication + liveness flags.
    root.setMember("available", snapshot.available);
    root.setMember("enabled", snapshot.enabled);
    root.setMember("sequence_number", static_cast<std::int64_t>(snapshot.sequence_number));

    // Graph-level counters.
    root.setMember("node_count", static_cast<std::int64_t>(snapshot.node_count));
    root.setMember("blocks_written", static_cast<std::int64_t>(snapshot.blocks_written));
    root.setMember("blocks_drained", static_cast<std::int64_t>(snapshot.blocks_drained));
    root.setMember("blocks_dropped", static_cast<std::int64_t>(snapshot.blocks_dropped));
    root.setMember("graph_over_budget_blocks",
                   static_cast<std::int64_t>(snapshot.graph_over_budget_blocks));

    // Last-block context.
    root.setMember("sample_rate", snapshot.sample_rate);
    root.setMember("last_frame_count", static_cast<std::int64_t>(snapshot.last_frame_count));
    root.setMember("last_available_ns", static_cast<std::int64_t>(snapshot.last_available_ns));
    root.setMember("last_graph_elapsed_ns",
                   static_cast<std::int64_t>(snapshot.last_graph_elapsed_ns));
    root.setMember("last_graph_load", static_cast<double>(snapshot.last_graph_load));

    // Per-node summaries.
    auto nodes = choc::value::createEmptyArray();
    for (const auto& node : snapshot.nodes) {
        nodes.addArrayElement(node_to_value(node));
    }
    root.setMember("nodes", nodes);

    return choc::json::toString(root, pretty);
}

}  // namespace pulp::audio
