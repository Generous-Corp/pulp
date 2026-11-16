#pragma once

#include <pulp/host/signal_graph.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace pulp::host {

struct GraphSerializerTestAccess {
    static void set_after_first_read(std::function<void(const std::filesystem::path&)> hook);
    static void fail_next_write() noexcept;
    static void fail_next_replace() noexcept;
    // The saved-identity v6 document a pre-v7 writer produced for `graph`
    // (structural identities, no authored IDs). Production code has no v6
    // writer; tests use this to build legacy fixtures.
    static std::string legacy_v6_saved_document(
        const SignalGraph& graph,
        const std::unordered_map<NodeId, std::pair<float, float>>& editor_layout = {});
    // The structural v6 identity of every node, as legacy_v6_saved_document
    // records it.
    static std::unordered_map<NodeId, std::string> legacy_v6_identities(const SignalGraph& graph);
};

} // namespace pulp::host
