#pragma once

// UMP Endpoint — abstract handle to a MIDI 2.0 UMP source/destination.
//
// macOS plan item 8.1 (Endpoint half). Lives in `pulp::midi`. A UMP
// endpoint is a single bidirectional logical port — on CoreMIDI 2.0 it
// wraps a `MIDIEndpointRef`, on Windows it wraps a MIDIEndpointConnection
// (WinRT MIDI 2.0), on Linux it wraps an ALSA Rawmidi UMP node. The
// Pulp surface is intentionally OS-agnostic: callers see UmpPacket in,
// UmpPacket out.
//
// Design notes:
//   - Endpoints are owned by a UmpSession. Calling code never news them
//     directly; it asks the session for one (or registers a virtual one).
//   - The receive callback fires on whatever thread the OS chose for
//     MIDI delivery (CoreMIDI: a dedicated MIDIClient thread; WinRT: the
//     UMP callback thread). Callers must not block.
//   - Send is non-blocking and lock-free where the OS allows it.
//   - Endpoints expose a stable string id (CoreMIDI unique-id; WinRT
//     EndpointDeviceId; ALSA card:device) so callers can persist a
//     selection across runs.
//
// Versus the older `MidiInput` / `MidiOutput` pair: those operate on
// MIDI 1.0 byte streams (with the WinRT/CoreMIDI backends translating
// internally). `UmpEndpoint` is the UMP-native surface; both layers
// will coexist while Pulp finishes the UMP migration.

#include <pulp/midi/ump.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulp::midi {

/// Direction the endpoint exposes. CoreMIDI 2.0 treats sources and
/// destinations as separate `MIDIEndpointRef`s; WinRT and many virtual
/// endpoints are bidirectional. We model it as a flag set rather than
/// a single enum so a single endpoint can be both.
struct UmpEndpointDirection {
    bool can_receive = false;  // we can read UMP packets from it
    bool can_send    = false;  // we can write UMP packets to it
};

/// Static description of an endpoint discovered on the system or
/// registered virtually. Cheap to copy.
struct UmpEndpointInfo {
    /// Stable identifier (CoreMIDI unique-id as decimal, WinRT
    /// EndpointDeviceId, virtual endpoint name, etc).
    std::string id;
    /// Human-readable display name.
    std::string name;
    /// True if this endpoint exists only in-process (registered via
    /// `UmpSession::register_virtual_endpoint`). Useful for tests and
    /// for documentation rendering — virtual endpoints don't survive
    /// process restart.
    bool is_virtual = false;
    UmpEndpointDirection direction;
};

/// Receive callback. Fired on an OS thread; must not block. The packet
/// is delivered as already-parsed UmpPacket so the caller doesn't have
/// to repeat the per-type word-count switch.
using UmpReceiveCallback = std::function<void(const UmpPacket& packet,
                                              double timestamp_sec)>;

/// Polymorphic endpoint handle. The concrete subclass lives in the
/// platform backend (or `VirtualUmpEndpoint` for the in-process kind).
class UmpEndpoint {
public:
    virtual ~UmpEndpoint() = default;

    /// Snapshot of the endpoint's static metadata.
    virtual const UmpEndpointInfo& info() const noexcept = 0;

    /// Register the receive callback. Pass nullptr to detach. Calling
    /// this while the endpoint is open is permitted: the new callback
    /// takes effect for subsequent deliveries (callers that care about
    /// strict ordering should detach, drain, then re-attach).
    ///
    /// On endpoints that lack a receive direction this is a no-op.
    virtual void set_receive_callback(UmpReceiveCallback cb) = 0;

    /// Send a UMP packet. Returns false if the endpoint is not open
    /// for sending or the OS rejected the write. The packet's
    /// `word_count` must be 1..4; senders are responsible for choosing
    /// the right MessageType.
    virtual bool send(const UmpPacket& packet) = 0;

    /// True iff the endpoint is currently usable (port connected,
    /// virtual endpoint live).
    virtual bool is_open() const noexcept = 0;
};

} // namespace pulp::midi
