#pragma once

// Authored subgraph persistence in the saved-identity document (v7 written,
// v6 still restored).
//
// A SubgraphInstance expands into ordinary graph nodes, so the flattened
// graph already round-trips through GraphSerializer. What a plain document
// loses is the instance itself: which nodes belong to which template
// instance, its identity and index, its nested child identities, and its
// declared parameter values. These functions record that binding beside the
// flattened graph and rebuild live SubgraphInstance values on restore.
//
// A record never contains a runtime NodeId. Members are named by the
// logical identities in the envelope (authored IDs in v7, structural
// identities in v6), in expansion order. On restore the
// caller supplies the templates; each record is checked against a fresh
// expansion of its template (member count, node type and name, nested child
// identities, internal edges, declared parameter set and bounds), and any
// mismatch refuses the whole document and leaves the graph empty.
// That expansion runs on a scratch graph, so any Processor factory in a
// template is invoked during restore, on the calling (control) thread.

#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>

#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::host {

/// Write a saved-identity v7 document that also records each instance.
/// Parameter values are read from the live graph (the target Gain node's
/// gain or the target Processor parameter), not from the instance's cached
/// `parameters`. Returns an empty string when an instance is not part of
/// `graph` (a stale or foreign NodeId, or an exposed port whose owner or
/// incarnation no longer matches the live node), two instances share an
/// identity or a node, a live parameter value is non-finite or outside its
/// declared bounds, or the graph itself cannot be serialized.
std::string to_saved_subgraph_document(
    const SignalGraph& graph, std::span<const SubgraphInstance> subgraphs,
    const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout = {});

struct SubgraphDocumentLoad {
    GraphSerializer::LoadResult load;
    // Restored instances, ordered by instance identity, bound to the live
    // NodeIds of `graph`. Empty whenever load.ok is false.
    std::vector<SubgraphInstance> subgraphs;

    bool ok() const noexcept {
        return load.ok;
    }
};

/// Restore a saved-identity v6 or v7 document and rebind its subgraph records to
/// `templates`. Template lookup, document inspection, and record shape are
/// checked before the graph is touched, and those refusals leave `graph`
/// unchanged; conformance to each template is checked after the flattened
/// graph loads, and a refusal from then on clears `graph` and empties every
/// map and list `load` returned, as any refused LoadResult. Every refusal
/// names its rule in load.error (`subgraph_template_unknown`,
/// `subgraph_template_duplicate`, `subgraph_instance_mismatch`,
/// `subgraph_state_rejected`, or a document inspection refusal).
SubgraphDocumentLoad from_saved_subgraph_document(SignalGraph& graph, const std::string& json,
                                                  std::span<const SubgraphTemplate> templates,
                                                  const GraphLoadOptions& options = {});

} // namespace pulp::host
