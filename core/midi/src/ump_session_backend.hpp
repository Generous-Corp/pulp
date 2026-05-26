#pragma once

// Internal handoff between the cross-platform UmpSession and the
// OS-backed CoreMIDI / WinRT / ALSA implementations. Not part of the
// public Pulp API; only the session source + the platform .mm/.cpp
// include this.

#include <pulp/midi/ump_endpoint.hpp>
#include <pulp/midi/ump_session.hpp>

#include <string>
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

/// Install or replace the OS backend hook table. Called from the
/// platform .mm/.cpp file's static initialiser.
void register_ump_os_backend(const ump_os::OsBackendVTable& v);

} // namespace pulp::midi
