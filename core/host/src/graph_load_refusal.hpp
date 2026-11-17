#pragma once

#include <pulp/host/graph_serializer.hpp>

#include <string>
#include <utility>

namespace pulp::host::detail {

// The one refusal path for every graph load (from_json, from_file,
// from_saved_document and the subgraph restore). A refused LoadResult carries
// only its error: node maps, editor layout and per-node diagnostic lists are
// emptied, so no caller can read a handle into a graph the load no longer
// owns. `touched_graph` is the caller's graph once the load has started
// mutating it, and is then cleared; a refusal before that point passes
// nullptr and leaves the caller's graph exactly as it was.
inline void refuse_load(GraphSerializer::LoadResult& result, SignalGraph* touched_graph) {
    if (touched_graph != nullptr)
        touched_graph->clear();
    result.ok = false;
    result.missing_plugins.clear();
    result.state_restore_failures.clear();
    result.missing_custom_node_types.clear();
    result.editor_layout.clear();
    result.persisted_node_ids.clear();
    result.unrestored_node_ids.clear();
    result.identity_node_ids.clear();
}

inline void refuse_load(GraphSerializer::LoadResult& result, SignalGraph* touched_graph,
                        std::string error) {
    refuse_load(result, touched_graph);
    result.error = std::move(error);
}

} // namespace pulp::host::detail
