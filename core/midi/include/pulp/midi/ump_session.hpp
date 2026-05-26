#pragma once

// UmpSession — Pulp's MIDI 2.0 UMP session handle.
//
// macOS plan item 8.1 (Session half). One `UmpSession` per app or
// plugin instance. On macOS it owns the underlying `MIDIClientRef`
// (CoreMIDI 2.0). On platforms that don't yet have a UMP backend
// the session degrades to virtual-endpoints-only, which is enough
// for unit tests and for in-process loopback.
//
// Design notes:
//   - The session is the only entity that knows how to talk to the
//     OS MIDI subsystem; everything else hangs off it.
//   - Endpoint lifetime is tied to the session: dropping the session
//     drops every endpoint it minted.
//   - Discovery returns a snapshot. We deliberately don't expose a
//     push-style "endpoint added" stream from this surface — the
//     existing `MidiSystem::set_port_change_callback()` already
//     handles hotplug for the legacy MIDI 1.0 stream, and re-running
//     `enumerate()` is cheap.
//   - Virtual endpoints are returned as `std::shared_ptr` so a test
//     can hold them past the session's lifetime if needed (the
//     session's weak refs let it skip them during teardown).

#include <pulp/midi/ump_endpoint.hpp>
#include <pulp/midi/ump_virtual_endpoint.hpp>

#include <memory>
#include <string>
#include <vector>

namespace pulp::midi {

/// Options for creating a session. All fields have safe defaults;
/// `name` shows up in CoreMIDI's MIDI Studio as the client name.
struct UmpSessionConfig {
    std::string name = "Pulp UMP Session";
    /// If true, the session attempts to open the OS UMP backend (e.g.
    /// CoreMIDI 2.0 `MIDIClient`). If false, the session is virtual-
    /// endpoints-only — useful in tests and on platforms without UMP.
    bool enable_os_backend = true;
};

/// Outcome of creating an endpoint connection through the session.
/// Returned from `open_endpoint()` so callers can distinguish "no
/// such endpoint" from "OS rejected the connection".
enum class UmpOpenStatus {
    Ok,
    NotFound,
    OsBackendUnavailable,
    OsError,
};

class UmpSession {
public:
    UmpSession();
    explicit UmpSession(UmpSessionConfig cfg);
    ~UmpSession();

    UmpSession(const UmpSession&) = delete;
    UmpSession& operator=(const UmpSession&) = delete;

    /// Returns true if the OS-backed UMP backend is live (CoreMIDI
    /// client created etc). On a virtual-only session this is false.
    bool os_backend_active() const noexcept;

    /// Snapshot the OS endpoints known to the system, plus every
    /// virtual endpoint registered through this session. Cheap; safe
    /// to call from any thread.
    std::vector<UmpEndpointInfo> enumerate_endpoints() const;

    /// Open (or attach to) an endpoint by id. The returned endpoint
    /// pointer is owned by the session: the caller borrows it for as
    /// long as the session lives. Returns nullptr on failure; `status`
    /// (when non-null) is filled with the reason.
    UmpEndpoint* open_endpoint(const std::string& id,
                               UmpOpenStatus* status = nullptr);

    /// Register a virtual endpoint with the session. The session
    /// retains a reference (so `enumerate_endpoints()` lists it); the
    /// returned `shared_ptr` is the caller's handle for direct
    /// `send()` / `deliver()` operations.
    std::shared_ptr<VirtualUmpEndpoint> register_virtual_endpoint(
        VirtualUmpEndpointConfig cfg);

    /// Drop a virtual endpoint. Idempotent; returns true if a matching
    /// endpoint was found and removed.
    bool unregister_virtual_endpoint(const std::string& id);

    /// Number of virtual endpoints currently registered.
    std::size_t virtual_endpoint_count() const;

    /// Connect two virtual endpoints in a loopback: every packet
    /// sent into `from` is delivered to `to`. Useful for headless
    /// tests where you want a round-trip without the OS layer.
    /// Returns false if either endpoint id is unknown.
    bool wire_virtual_loopback(const std::string& from_id,
                               const std::string& to_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::midi
