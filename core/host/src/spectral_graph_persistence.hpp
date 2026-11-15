#pragma once

#include <pulp/host/graph_serializer.hpp>

#include <memory>
#include <string_view>

namespace pulp::host::spectral_persistence {

bool contains_spectral_nodes(const SignalGraph& graph);
std::string write(const SignalGraph& graph,
                  const std::unordered_map<NodeId, std::pair<float, float>>& layout);

struct Candidate {
    bool handled = false;
    GraphSerializer::LoadResult result;
    std::unique_ptr<SignalGraph> graph;
};

// Build and validate an isolated stopped graph. No caller-owned graph, plugin,
// custom registration or live state is touched, including on malformed input.
Candidate read(std::string_view json);

} // namespace pulp::host::spectral_persistence
