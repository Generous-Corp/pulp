#pragma once

// Internal handoff between the cross-platform UmpSession and the
// OS-backed CoreMIDI / WinRT / ALSA implementations. Not part of the
// public Pulp API; only the session source + the platform .mm/.cpp
// include this.

#include <pulp/midi/ump_endpoint.hpp>
#include <pulp/midi/ump_session.hpp>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::midi::ump_os {

struct OsBackendVTable {
    bool (*init)(const UmpSessionConfig& cfg, void** out_state) = nullptr;
    void (*shutdown)(void* state) = nullptr;
    std::vector<UmpEndpointInfo> (*enumerate)(void* state) = nullptr;
    UmpEndpoint* (*open)(void* state,
                         const std::string& id,
                         UmpOpenStatus* status) = nullptr;
};

} // namespace pulp::midi::ump_os

namespace pulp::midi {

/// Install or replace the OS backend hook table. Called by the platform's
/// explicit registration anchor.
void register_ump_os_backend(const ump_os::OsBackendVTable& v);

#if defined(__APPLE__)
/// Install the CoreMIDI vtable. This explicit anchor is called by
/// UmpSession so static-library consumers retain the platform translation
/// unit even when no other CoreMIDI UMP symbol is referenced.
void register_coremidi_ump_backend();

struct CoreMidiUmpTopologyEndpoint {
    std::string endpoint_id;
    std::string entity_id;
    std::string name;
    bool is_source = false;
};

/// Pure production topology reducer, exposed internally so the Simulator
/// harness can pin multi-endpoint census and conservative pairing semantics.
inline std::vector<UmpEndpointInfo> group_coremidi_ump_topology(
    const std::vector<CoreMidiUmpTopologyEndpoint>& endpoints) {
    std::vector<UmpEndpointInfo> out;
    std::unordered_map<std::string, std::vector<std::size_t>> by_entity;
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        if (!endpoints[i].entity_id.empty()) {
            by_entity[endpoints[i].entity_id].push_back(i);
        }
    }

    std::vector<bool> consumed(endpoints.size(), false);
    for (const auto& [entity_id, indices] : by_entity) {
        std::size_t source_count = 0;
        std::size_t destination_count = 0;
        std::size_t source_index = 0;
        std::size_t destination_index = 0;
        for (const auto index : indices) {
            if (endpoints[index].is_source) {
                ++source_count;
                source_index = index;
            } else {
                ++destination_count;
                destination_index = index;
            }
        }
        // Only 1 source + 1 destination is an unambiguous logical pair.
        // Multi-endpoint entities stay fully enumerated as independent
        // directions instead of being overwritten or paired by position.
        if (source_count == 1 && destination_count == 1) {
            UmpEndpointInfo info;
            info.id = "entity:" + entity_id;
            info.name = endpoints[source_index].name.empty()
                            ? endpoints[destination_index].name
                            : endpoints[source_index].name;
            info.direction.can_receive = true;
            info.direction.can_send = true;
            out.push_back(std::move(info));
            for (const auto index : indices) consumed[index] = true;
        }
    }

    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        if (consumed[i]) continue;
        UmpEndpointInfo info;
        info.id = endpoints[i].endpoint_id;
        info.name = endpoints[i].name;
        info.direction.can_receive = endpoints[i].is_source;
        info.direction.can_send = !endpoints[i].is_source;
        out.push_back(std::move(info));
    }
    return out;
}
#endif

} // namespace pulp::midi
