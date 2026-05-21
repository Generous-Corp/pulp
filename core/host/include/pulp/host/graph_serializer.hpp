#pragma once

// GraphSerializer — encode/decode SignalGraph to .pulpgraph JSON.
//
// Captures (topology + per-node plugin state + editor layout) so a graph
// can be saved to disk, transferred between machines, and rebuilt later.
//
// Plugin re-resolution: scanner-identity-first. The on-disk format stores
// (format, unique_id, manufacturer, name, version, last_path, optional
// SHA256 hash). On load, the resolver tries the identity tuple first, then
// the path with hash verification, then surfaces a clear missing-plugin
// error. Plugin binaries are NEVER embedded — that breaks signing,
// notarization, and AU/AUv3 component-manager registration.
//
// Phase 3 of planning/signal-graph-followups-plan.md.

#include <pulp/host/signal_graph.hpp>
#include <functional>
#include <string>
#include <unordered_map>

namespace pulp::host {

class GraphSerializer {
public:
    using MigrationFn =
        std::function<bool(const std::string& source_json,
                           std::string& destination_json)>;

    static int current_format_version();
    static bool register_migration(int from_version,
                                   int to_version,
                                   MigrationFn migration);

    // Encode the entire graph as JSON. The `editor_layout` map (NodeId →
    // (x,y)) is optional — pass an empty map if the graph has no UI.
    static std::string to_json(const SignalGraph& graph,
                               const std::unordered_map<NodeId, std::pair<float,float>>& editor_layout = {});

    // Result of a load attempt. Plugins that couldn't be re-resolved show
    // up in `missing_plugins` with their original identity tuple — the
    // graph still loads but those nodes have null PluginSlots.
    struct LoadResult {
        bool ok = false;
        std::string error;  // populated when ok == false
        std::vector<std::string> missing_plugins;  // identity strings
        std::unordered_map<NodeId, std::pair<float,float>> editor_layout;
    };

    // Decode JSON into a freshly cleared graph. Returns LoadResult; the
    // graph is mutated in-place. Two-pass: instantiate all nodes (resolve
    // plugins via PluginSlot::load), then walk connections and replay them.
    static LoadResult from_json(SignalGraph& graph, const std::string& json);
};

} // namespace pulp::host
