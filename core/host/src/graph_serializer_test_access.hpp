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
    // The next ordinary load throws std::bad_alloc while planning authored IDs.
    static void fail_next_authored_id_plan() noexcept;
    // The next from_saved_document throws std::bad_alloc while recording an
    // identity's restored NodeId.
    static void fail_next_identity_record() noexcept;
    // The next ordinary load throws std::bad_alloc while parsing the original
    // document, before any migration or authored ID planning.
    static void fail_next_document_parse() noexcept;
    // The next spectral load throws std::bad_alloc after building its
    // isolated candidate's nodes, before restoring any connection.
    static void fail_next_spectral_candidate_build() noexcept;
    // The next load of a document with event_connections throws
    // std::bad_alloc while rebinding them, after every node and connection
    // was restored.
    static void fail_next_event_rebind() noexcept;
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
