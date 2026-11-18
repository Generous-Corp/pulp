#pragma once

#include <pulp/host/graph_serializer.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace pulp::host::spectral_persistence {

bool contains_spectral_nodes(const SignalGraph& graph);
// Write a graph that holds spectral nodes. With `saved_identity` false this is
// the format 6 document (asset morph, stages and synthesis, no node IDs); with
// it true, the saved-identity v7 document, which also carries authored node
// IDs, every spectral node's readable identity (including the exact sample
// rate bits), live analysis declarations and the single-stream asset source
// with its finite-source policy version. On refusal returns an empty string
// and sets `reason` to the rule the graph breaks; `reason` is cleared on
// success.
std::string write(const SignalGraph& graph,
                  const std::unordered_map<NodeId, std::pair<float, float>>& layout,
                  std::string& reason, bool saved_identity = false);

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
// registration or live state is touched, including on malformed input. On
// success `result.persisted_node_ids` maps each record's persisted id to its
// candidate node; the caller assigns authored IDs. Every refusal goes through
// detail::refuse_load.
Candidate read(std::string_view json, int admission_block = max_admission_block);

// Test hook: the next read() throws std::bad_alloc after it has built the
// candidate's nodes, before any connection is restored.
void fail_next_candidate_build() noexcept;

} // namespace pulp::host::spectral_persistence
