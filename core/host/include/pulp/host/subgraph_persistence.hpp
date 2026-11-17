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
// expansion of its template (member count, node type and name, port kinds,
// exact Custom type and version, exact Processor graph type, nested child
// identities, every semantic field of each internal edge, declared parameter
// set and bounds, and Custom member state the registered type accepts), and
// any mismatch refuses the whole document.
// The whole document is first restored and checked on a candidate graph that
// carries the caller's custom node types, and only then loaded into the
// caller's graph. Template expansions run on scratch graphs too, so Processor
// factories, restore registries and Custom create/load_state/destroy probes
// run several times during one restore, all on the calling (control) thread.
// Records carry authored membership and identity only: no runtime NodeId,
// prepared instance, voice generation or pending event is persisted.

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
/// declared bounds, a Custom member is unresolved or holds state its
/// registered type refuses, or the graph itself cannot be serialized.
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
/// `templates`. Template lookup, document inspection, record shape, the
/// flattened load and conformance to each template are all checked on a
/// candidate graph first, and those refusals leave `graph` unchanged. Only a
/// candidate that passes is loaded into `graph`; should that second load or
/// rebind still fail (a factory or restore callback that behaves differently
/// the second time), the refusal clears `graph` and empties every map and
/// list `load` returned, as any refused LoadResult. Every refusal names its
/// rule in load.error (`subgraph_template_unknown`,
/// `subgraph_template_duplicate`, `subgraph_instance_mismatch`,
/// `subgraph_state_rejected`, or a document inspection or load refusal).
SubgraphDocumentLoad from_saved_subgraph_document(SignalGraph& graph, const std::string& json,
                                                  std::span<const SubgraphTemplate> templates,
                                                  const GraphLoadOptions& options = {});

} // namespace pulp::host
