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
// Schema changes must go through registered migrations so older saved graphs
// either upgrade deterministically or fail with an explicit load error.

#include <pulp/host/signal_graph.hpp>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::host {

class ScanBlacklist;

// How from_json turns a plugin node's stored identity into a loaded slot.
//
// The file names each plugin node's `last_path`, and loading that path as
// given would run whatever library it points at. So, in order:
//   1. Audio Units resolve through the component registry by identity; the
//      stored path is never opened.
//   2. `last_path` is loaded only when its real path (symlinks resolved) lies
//      inside a known plugin root (PluginScanner::default_paths plus
//      `plugin_roots`) and `blacklist` does not record it. After the load the
//      slot's identity must match the node's; a bundle that holds a different
//      plugin is refused.
//   3. Otherwise the node is listed in LoadResult::missing_plugins with the
//      reason and is NOT loaded. Scanning the roots to re-find the plugin by
//      (format, unique_id) runs every installed bundle's entry point, so it is
//      an explicit opt-in through `scan_mode`, never the default.
enum class GraphPluginScanMode {
    // Never enumerate the roots: an unrooted node is missing.
    none,
    // Enumerate through the pulp-scan-worker child only; when no worker can
    // be found the node is missing and nothing runs in this process.
    isolated_only,
    // Enumerate through the worker, and run CLAP entry points in this
    // process when no worker can be found. Only for tools that already run
    // arbitrary plugin code in-process by design.
    allow_in_process,
};

struct GraphLoadOptions {
    // Extra directories whose bundles may be loaded, e.g. a project's own
    // plugin folder. Each is matched after symlink resolution.
    std::vector<std::string> plugin_roots;
    // Whether the standard per-format install directories count as known
    // roots. Hermetic tools and tests turn this off so a load never touches
    // the machine's installed plugin collection.
    bool include_default_plugin_roots = true;
    // Bundles recorded here (a prior scan crash) are reported missing rather
    // than loaded. Caller-owned; must outlive from_json.
    const ScanBlacklist* blacklist = nullptr;
    // When false no plugin node is loaded at all: each stays an unresolved
    // placeholder with its identity, which is what a caller that cannot use
    // plugin slots anyway (pulp bake) wants, since loading would run code
    // for nothing.
    bool load_plugins = true;
    // Whether an unrooted node may be re-found by (format, unique_id) in the
    // known roots. See GraphPluginScanMode.
    GraphPluginScanMode scan_mode = GraphPluginScanMode::none;
    // Worker binary for a scan; empty resolves it like ScanOptions does.
    std::string scan_worker_path;
    // Load a `last_path` that lies outside every known root, for a caller
    // whose operator named that path on purpose. Trusts every path in the
    // file; a library caller should prefer registering `plugin_roots`.
    bool allow_unrooted_plugin_paths = false;
};

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
        std::vector<std::string> missing_custom_node_types;  // type_id@version
        std::unordered_map<NodeId, std::pair<float,float>> editor_layout;
    };

    // Decode JSON into a freshly cleared graph. Returns LoadResult; the
    // graph is mutated in-place. Two-pass: instantiate all nodes (resolve
    // plugins per GraphLoadOptions, then PluginSlot::load), then walk
    // connections and replay them. The overload without options uses the
    // defaults: standard roots only, no blacklist, no scanning, unrooted
    // paths refused.
    static LoadResult from_json(SignalGraph& graph, const std::string& json);
    static LoadResult from_json(SignalGraph& graph, const std::string& json,
                                const GraphLoadOptions& options);
};

} // namespace pulp::host
