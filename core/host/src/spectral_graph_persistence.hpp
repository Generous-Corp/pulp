#pragma once

#include <pulp/host/graph_serializer.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace pulp::host::spectral_persistence {

bool contains_spectral_nodes(const SignalGraph& graph);
std::string write(const SignalGraph& graph,
                  const std::unordered_map<NodeId, std::pair<float, float>>& layout);
// Same document as write(); on refusal returns an empty string and sets
// `reason` to the rule the graph breaks. `reason` is cleared on success.
std::string write(const SignalGraph& graph,
                  const std::unordered_map<NodeId, std::pair<float, float>>& layout,
                  std::string& reason);

struct Candidate {
    bool handled = false;
    GraphSerializer::LoadResult result;
    std::unique_ptr<SignalGraph> graph;
};

// The largest block a spectral graph admits.
inline constexpr int max_admission_block = 4096;

// The block a saved graph is admitted at: the block it is prepared for, or the
// largest block its limits allow when it is not prepared.
int admission_block_for(const SignalGraph& graph);

// Build and validate an isolated stopped graph, admitting it at
// `admission_block` samples. No caller-owned graph, plugin, custom
// registration or live state is touched, including on malformed input.
Candidate read(std::string_view json, int admission_block = max_admission_block);

} // namespace pulp::host::spectral_persistence
