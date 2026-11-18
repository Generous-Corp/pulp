#pragma once

// Value-owned graph edit history and logical-identity document diff.
//
// Both surfaces work on saved-identity documents (GraphSerializer::
// to_saved_document writes v7), so a recorded state never holds a pointer
// into a live SignalGraph and survives any later mutation. NodeIds are lookup
// keys only: documents are compared by authored node identity (the persisted
// authored ID in v7, the structural identity in v6), so two documents that
// differ only in NodeId numbering or editor layout are semantically equal,
// and a renamed node is one changed node. A v6 and a v7 document are not
// compared with each other. Spectral v7 documents use the same model: a
// spectral node's identity, stage configuration and asset references are node
// fields, typed spectral edges are connections, and embedded assets are
// compared by asset id.

#include <pulp/host/graph_identity.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::host {

using GraphEditorLayout = std::unordered_map<NodeId, std::pair<float, float>>;

// ── Document diff ───────────────────────────────────────────────────────────

enum class GraphDocumentDiffCode : std::uint8_t {
    Ok,
    // One side was refused by GraphSerializer::inspect_document, or the two
    // sides use different identity schemas (refusal
    // UnsupportedDocumentProfile on AfterRefused); the refusal and the side
    // are recorded on the result.
    BeforeRefused,
    AfterRefused,
    // A side is a valid document without a saved-identity envelope, so
    // its nodes have no authored identity to compare by.
    LogicalIdentityMissing,
    AllocationFailed,
};

const char* graph_document_diff_code_name(GraphDocumentDiffCode) noexcept;

struct GraphNodeChange {
    std::string identity;
    // Top-level node record members whose values differ, sorted. A Processor
    // node's processor record is compared under the member name "processor".
    // A spectral node's spectral record is compared member by member, as
    // "spectral.<member>": spectral.contract_hex and spectral.identity (the
    // frame contract), spectral.stage and spectral.gains (stage kind and
    // gains), spectral.asset, spectral.asset_a, spectral.asset_b (the asset
    // ids it reads) and spectral.finite_policy.
    std::vector<std::string> fields;
};

struct GraphDocumentDiff {
    GraphDocumentDiffCode code = GraphDocumentDiffCode::AllocationFailed;
    GraphDocumentRefusal refusal = GraphDocumentRefusal::None;
    std::string detail;
    // Digests of each compared view (node fields and connections keyed by
    // authored identity, other sections as compared). Equal documents have
    // equal digests, including when only NodeId numbering or layout differs.
    std::string before_digest;
    std::string after_digest;

    std::vector<std::string> nodes_added;
    std::vector<std::string> nodes_removed;
    std::vector<GraphNodeChange> nodes_changed;
    // Identities whose editor layout differs. Layout is not semantic, so it
    // never makes equal() false.
    std::vector<std::string> layout_changed;
    // Connection keys spell both endpoints by authored identity and port
    // index: "<source identity>[<port>] -> <destination identity>[<port>]".
    std::vector<std::string> connections_added;
    std::vector<std::string> connections_removed;
    std::vector<std::string> connections_changed;
    // Graph event connections (the document's event_connections records),
    // keyed by the endpoints' authored identities and ports:
    // "<producer identity>[<port>] => <consumer identity>[<port>]". A record
    // whose endpoints and ports are unchanged but whose kind differs is
    // reported as changed.
    std::vector<std::string> event_connections_added;
    std::vector<std::string> event_connections_removed;
    std::vector<std::string> event_connections_changed;
    // Embedded spectral assets (the document's spectral_assets rows), keyed by
    // asset id. An asset id digests the asset schema, source length, frame
    // contract and encoded bytes, so changing an asset's bytes is one asset
    // removed and one added, and every node that reads it reports its
    // spectral.asset field changed. A row whose id is unchanged but whose
    // other members differ is reported as changed.
    std::vector<std::string> spectral_assets_added;
    std::vector<std::string> spectral_assets_removed;
    std::vector<std::string> spectral_assets_changed;
    // Other top-level document members (for example format_version,
    // sample_regions, subgraphs) whose canonical values differ. NodeIds in
    // sample_regions are compared as the authored identities they bind; the
    // other sections are compared verbatim.
    std::vector<std::string> sections_changed;

    bool ok() const noexcept {
        return code == GraphDocumentDiffCode::Ok;
    }
    bool equal() const noexcept {
        return ok() && nodes_added.empty() && nodes_removed.empty() && nodes_changed.empty() &&
               connections_added.empty() && connections_removed.empty() &&
               connections_changed.empty() && event_connections_added.empty() &&
               event_connections_removed.empty() && event_connections_changed.empty() &&
               spectral_assets_added.empty() && spectral_assets_removed.empty() &&
               spectral_assets_changed.empty() && sections_changed.empty();
    }
};

// Data-only: inspects both documents, never loads plugins, invokes factories,
// restores state, or touches a SignalGraph. Safe on untrusted bytes.
GraphDocumentDiff diff_graph_documents(std::string_view before, std::string_view after);

// Authored port identity of every node port, qualified by the node's saved
// identity ("<identity>/in/<port>" or "<identity>/out/<port>"). Suitable for
// snapshot_port_identity and diff_port_identities across NodeId renumbering.
std::vector<PortHandle> qualified_port_identities(const SignalGraph& graph);

// ── Edit history ────────────────────────────────────────────────────────────

enum class GraphHistoryCode : std::uint8_t {
    Ok,
    // commit(): the edit produced no semantic change and was not recorded.
    NoChange,
    NothingToUndo,
    NothingToRedo,
    // A checkpoint or the current graph could not be captured as a saved
    // document with a valid authored port identity.
    CaptureFailed,
    // The live graph no longer matches the state history expects, so it was
    // edited outside this history; undo/redo refuses instead of discarding it.
    Diverged,
    // The recorded document could not be restored faithfully: it fails to
    // load, a node's state is rejected, or a node that is live now would come
    // back unresolved (missing plugin, Processor, or custom node type). See
    // GraphHistoryResult::graph_untouched and rolled_back.
    RestoreFailed,
    // The restored graph's authored ports differ from the recorded snapshot.
    PortIdentityMismatch,
    // commit(): recording the edit would take the history's retained bytes
    // past its byte budget. Nothing was recorded and no entry was dropped, so
    // every recorded undo and redo step still works; the live graph keeps the
    // edit, which undo then reports as Diverged until clear() is called.
    ByteBudgetExceeded,
};

const char* graph_history_code_name(GraphHistoryCode) noexcept;

// A value-owned pre-edit capture. Take one before mutating the graph and pass
// it to GraphEditHistory::commit once the edit is done.
struct GraphEditCheckpoint {
    std::string document;
    PortIdentityUndo ports;
    bool valid() const noexcept {
        return ports.valid && !document.empty();
    }
};

struct GraphHistoryResult {
    GraphHistoryCode code = GraphHistoryCode::CaptureFailed;
    std::string detail;
    std::string label;
    // Authored ports added and removed by this undo/redo, compared with the
    // graph immediately before the call.
    PortIdentityDiff port_changes;
    // Editor layout recorded with the restored document.
    GraphEditorLayout editor_layout;
    // Every restore reloads the graph and renumbers its NodeIds. When the
    // graph was reloaded (Ok, or a failure that ran a rollback) this maps
    // each node's authored identity to its NodeId after the call; map a
    // handle held from before the call through saved_node_identities() taken
    // before it. Empty when the graph was not touched.
    std::unordered_map<std::string, NodeId> node_ids;
    // On failure: the graph is in its pre-call state. Either the failure was
    // found by a trial restore into a scratch graph (graph_untouched is also
    // set, and live plugin instances were never destroyed), or the live
    // graph was reloaded from its pre-call document with no node newly
    // unresolved, no state rejected, and the same authored ports, or the live
    // load was refused without changing the graph's saved document (a
    // spectral graph whose controller is active refuses every load; stop it
    // with SignalGraph::stop_spectral_controller before undo or redo).
    bool rolled_back = false;
    // On failure: the live graph was never modified.
    bool graph_untouched = false;
    bool ok() const noexcept {
        return code == GraphHistoryCode::Ok;
    }
};

// Undo and redo over saved documents, for every graph family the saved
// document writer admits, including spectral graphs (whose documents embed
// their assets). Two bounds hold: at most `capacity` undo entries (beyond it
// the oldest undo entry is dropped, as in any bounded undo list), and at most
// `byte_budget` retained bytes in total across the undo and redo stacks. An
// entry's retained bytes are its label plus both recorded documents. A commit
// that would exceed the byte budget is refused with ByteBudgetExceeded rather
// than evicting entries to make room.
class GraphEditHistory {
  public:
    static constexpr std::size_t default_capacity = 64;
    // Room for two entries whose documents are each at the serializer's file
    // limit.
    static constexpr std::size_t default_byte_budget =
        std::size_t{4} * GraphSerializer::max_file_bytes;

    explicit GraphEditHistory(std::size_t capacity = default_capacity,
                              std::size_t byte_budget = default_byte_budget);

    static GraphEditCheckpoint checkpoint(const SignalGraph& graph,
                                          const GraphEditorLayout& editor_layout = {});

    // Record the edit between `before` and the current state of `graph`.
    // Clears the redo stack. The oldest entry is dropped beyond capacity. When
    // the entry would take retained_bytes() past byte_budget() (counting the
    // redo entries this commit would clear and the capacity drop), returns
    // ByteBudgetExceeded and changes nothing.
    GraphHistoryResult commit(GraphEditCheckpoint before, const SignalGraph& graph,
                              std::string label, const GraphEditorLayout& editor_layout = {});

    // Undo and redo first trial-restore the recorded document into a scratch
    // graph that carries `graph`'s custom node types, so every plugin and
    // Processor factory the document names is instantiated twice on success.
    // Only a document that restores faithfully there is loaded into `graph`.
    GraphHistoryResult undo(SignalGraph& graph, const GraphLoadOptions& options = {});
    GraphHistoryResult redo(SignalGraph& graph, const GraphLoadOptions& options = {});

    bool can_undo() const noexcept {
        return !undo_.empty();
    }
    bool can_redo() const noexcept {
        return !redo_.empty();
    }
    std::size_t undo_depth() const noexcept {
        return undo_.size();
    }
    std::size_t redo_depth() const noexcept {
        return redo_.size();
    }
    std::size_t capacity() const noexcept {
        return capacity_;
    }
    std::size_t byte_budget() const noexcept {
        return byte_budget_;
    }
    // Retained bytes of every undo and redo entry.
    std::size_t retained_bytes() const noexcept {
        return retained_bytes_;
    }
    std::string_view undo_label() const noexcept;
    std::string_view redo_label() const noexcept;
    void clear() noexcept;

  private:
    struct Entry {
        std::string label;
        GraphEditCheckpoint before;
        GraphEditCheckpoint after;
        std::size_t bytes() const noexcept {
            return label.size() + before.document.size() + after.document.size();
        }
    };
    GraphHistoryResult step(SignalGraph& graph, const GraphLoadOptions& options, bool undo);

    std::size_t capacity_;
    std::size_t byte_budget_;
    std::size_t retained_bytes_ = 0;
    std::deque<Entry> undo_;
    std::deque<Entry> redo_;
};

} // namespace pulp::host
