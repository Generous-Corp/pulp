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
#include <pulp/host/processor_restore.hpp>
#include <pulp/runtime/result.hpp>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulp::host {

class HostScanCache;
class ScanBlacklist;

/// Refusal reasons for the data-only saved-document inspection surface.
/// Inspection never loads plugins, restores Processor state, or mutates a
/// SignalGraph; callers can therefore safely use it on untrusted bytes.
enum class GraphDocumentRefusal : std::uint8_t {
    None,
    EmptyDocument,
    DocumentTooLarge,
    InvalidJson,
    RootNotObject,
    InvalidFormatVersion,
    UnsupportedFutureVersion,
    LogicalIdentityMissing,
    UnsupportedDocumentProfile,
    IdentityEnvelopeInvalid,
    IdentityBindingDangling,
    IdentityBindingDuplicate,
    IdentityBindingUnknown,
    DuplicateJsonKey,
    NestingLimitExceeded,
    NonFiniteNumber,
    AllocationFailed,
    SubgraphRecordInvalid,
    SubgraphBindingDangling,
    SubgraphBindingDuplicate,
    // A node id in a saved-identity v6 document (a node record, identity
    // entry, or binding) is not an integer in 1..NodeId max, or two node
    // records share one id.
    NodeIdInvalid,
    // One JSON object holds more than max_object_members members, or the
    // document holds more than max_document_members in total. The parser's
    // duplicate-key check is linear per member, so an unbounded object makes
    // parsing quadratic in the document size.
    MemberLimitExceeded,
};

const char* graph_document_refusal_name(GraphDocumentRefusal) noexcept;

/// Immutable, value-owned identity input produced by inspect_document().
/// The canonical bytes are sorted by object key while array order is kept;
/// no parser/vendor value or runtime NodeId escapes this type.
class GraphDocument {
  public:
    int format_version() const noexcept {
        return format_version_;
    }
    std::string_view profile() const noexcept {
        return profile_;
    }
    std::string_view canonical_bytes() const noexcept {
        return canonical_bytes_;
    }
    // Digest of the canonical document with node layout removed. It keeps
    // raw NodeIds, so two documents that differ only in NodeId numbering
    // have different digests; diff_graph_documents reports a
    // numbering-independent digest for that comparison.
    std::string_view semantic_digest() const noexcept {
        return semantic_digest_;
    }

  private:
    int format_version_ = 0;
    std::string profile_;
    std::string canonical_bytes_;
    std::string semantic_digest_;
    friend class GraphSerializer;
};

struct GraphDocumentInspection {
    GraphDocument document;
    GraphDocumentRefusal refusal = GraphDocumentRefusal::None;
    std::string detail;
    bool ok() const noexcept {
        return refusal == GraphDocumentRefusal::None;
    }
};

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
    // Optional exact-version registry used to restore v4 Processor records.
    // When absent, Processor records are reported as unsupported.
    ProcessorRestoreRegistry* processor_registry = nullptr;
    std::vector<ProcessorTypeKey> processor_allowlist;
    // Extra directories whose bundles may be loaded, e.g. a project's own
    // plugin folder. Each is matched after symlink resolution.
    std::vector<std::string> plugin_roots;
    // Whether the standard per-format install directories count as known
    // roots. Hermetic tools and tests turn this off so a load never touches
    // the machine's installed plugin collection.
    bool include_default_plugin_roots = true;
    // Bundles recorded here (a prior scan crash) are reported missing rather
    // than loaded, and a scan run for `scan_mode` records its own crash and
    // timeout verdicts into it, so the caller can persist them. Caller-owned;
    // must outlive from_json.
    ScanBlacklist* blacklist = nullptr;
    // Earlier scan results, consulted by a `scan_mode` scan so an unchanged
    // bundle is never handed to a worker again: without it a stale last_path
    // costs one worker per installed bundle on every load. Caller-owned; must
    // outlive from_json.
    HostScanCache* scan_cache = nullptr;
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
    // File loads are bounded before parsing so a caller can safely hand a
    // user-selected .pulpgraph path to the serializer. The limit covers the
    // encoded JSON envelope (including base64 state), not decoded payloads.
    static constexpr std::size_t max_file_bytes = 64u * 1024u * 1024u;
    static constexpr std::size_t max_document_depth = 64;
    // Member bounds checked before parsing. The largest object a document
    // holds is `bindings`, one member per node, so the per-object bound must
    // admit every node a SignalGraph accepts by default (4096); the writer's
    // own node cap is lower still. A document at the node and connection
    // limits stays far below the total.
    static constexpr std::size_t max_object_members = 4096;
    static constexpr std::size_t max_document_members = 1u << 20;

    /// Inspect one bounded saved document without loading plugins, invoking
    /// factories, restoring state, preparing, or writing a file. Legacy
    /// format-version documents without an explicit identity envelope are
    /// refused with LogicalIdentityMissing.
    static GraphDocumentInspection inspect_document(std::string_view json);

    /// Add the saved-identity v6 envelope to the existing graph document.
    /// The graph payload and its format version are byte-compatible with
    /// to_json; the envelope binds deterministic authored identities to nodes.
    static std::string to_saved_document(
        const SignalGraph& graph,
        const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout = {});

    /// Deterministic authored identity for every current node, the same
    /// values to_saved_document records. NodeIds are lookup keys only and
    /// never enter an identity string.
    static std::unordered_map<NodeId, std::string> saved_node_identities(const SignalGraph& graph);

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

    struct SaveResult {
        enum class Code : std::uint8_t {
            ok,
            serialization_refused,
            file_too_large,
            parent_unavailable,
            temporary_unavailable,
            write_failed,
            replace_failed,
            directory_sync_failed,
        };

        bool ok = false;
        Code code = Code::serialization_refused;
        std::string error;
    };

    static const char* save_code_name(SaveResult::Code code) noexcept;

    // Serialize one bounded graph file. The destination is replaced only
    // after the complete canonical JSON payload has been written.
    static SaveResult
    to_file(const SignalGraph& graph, const std::filesystem::path& path,
            const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout = {});

    // Same bounded, atomic write as to_file, with the to_saved_document
    // payload, so the file keeps its saved-identity envelope.
    static SaveResult
    to_saved_file(const SignalGraph& graph, const std::filesystem::path& path,
                  const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout = {});

    // Result of a load attempt. Plugins that couldn't be re-resolved show
    // up in `missing_plugins` with their original identity tuple — the
    // graph still loads but those nodes have null PluginSlots.
    struct LoadResult {
        bool ok = false;
        std::string error;                        // populated when ok == false
        std::vector<std::string> missing_plugins; // identity strings
        // Plugins that resolved and loaded, but rejected their persisted state.
        // The graph remains loaded, but state is not guaranteed to have been
        // restored; callers decide whether recovery is required without
        // confusing this with a missing plugin.
        std::vector<std::string> state_restore_failures;    // identity strings
        std::vector<std::string> missing_custom_node_types; // type_id@version
        std::unordered_map<NodeId, std::pair<float,float>> editor_layout;
        // Persisted node id -> NodeId assigned by this load, for every node
        // the load created. Empty whenever ok is false.
        std::unordered_map<NodeId, NodeId> persisted_node_ids;
    };

    // Decode JSON into a freshly cleared graph. Returns LoadResult; the
    // graph is mutated in-place. A document that exceeds the size or shape
    // bounds, does not parse, is not an object or cannot be migrated leaves
    // the graph untouched. Two-pass: instantiate all nodes (resolve
    // plugins per GraphLoadOptions, then PluginSlot::load), then walk
    // connections and replay them. The overload without options uses the
    // defaults: standard roots only, no blacklist, no scanning, unrooted
    // paths refused.
    static LoadResult from_json(SignalGraph& graph, const std::string& json);
    static LoadResult from_json(SignalGraph& graph, const std::string& json,
                                const GraphLoadOptions& options);

    /// Validate and restore a saved-identity v6 document. Identity bindings
    /// are checked before graph mutation, and after restoration every
    /// identity must name the node its binding's persisted id was restored
    /// as, and every saved connection must be restored. The one permitted
    /// omission is the partial-load rule: an automation or audio-rate
    /// modulation edge into an unresolved plugin placeholder. A refusal
    /// after the load started leaves `graph` empty.
    static LoadResult from_saved_document(SignalGraph& graph, const std::string& json,
                                          const GraphLoadOptions& options = {});

    /// Compare a loaded graph with the document it was loaded from. Every
    /// connection the document records must exist in `graph`, with the same
    /// lane flags (including `spectral`). The one permitted omission is the
    /// partial-load rule: a parameter edge from a valid source port into an
    /// unresolved plugin placeholder. `persisted_ids` maps document node ids to
    /// live ids, as `LoadResult::persisted_node_ids` does. Returns a
    /// description of the first edge that is missing, or nullopt. A document
    /// that does not parse is reported as missing its connections.
    static std::optional<std::string>
    unrestored_connection(const SignalGraph& graph, const std::string& json,
                          const std::unordered_map<NodeId, NodeId>& persisted_ids);

    /// Read one graph file under the size bound and verify it did not change
    /// while being read (size growth or a same-size replacement). On failure
    /// returns false, clears `json` and sets `error`.
    static bool read_file_bounded(const std::filesystem::path& path, std::string& json,
                                  std::string& error);

    // Read one bounded graph file, then apply the same migration and restore
    // rules as from_json. A read or size failure leaves graph untouched.
    static LoadResult from_file(SignalGraph& graph, const std::filesystem::path& path,
                                const GraphLoadOptions& options = {});

    /// Register every custom node type (and its sample-kernel companion)
    /// that `from` holds into `to`, so a scratch graph can trial-load a
    /// document exactly as `from` would without touching `from`. Returns
    /// false when `to` refuses a registration.
    static bool copy_custom_node_types(const SignalGraph& from, SignalGraph& to);
};

} // namespace pulp::host
