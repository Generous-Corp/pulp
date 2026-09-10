#pragma once

// Builds the device-free SignalGraph topology that a Timeline bounce needs.
//
// TimelineGraphPlaybackBinding is expressed entirely in NodeIds, so every caller
// that wants to render a compiled PlaybackProgram must first stand up a graph and
// a route table. That construction is small but it is not obvious, and getting it
// subtly wrong renders silence rather than failing, so it belongs in one place
// instead of being rediscovered per call site.
//
// "Device-free" is the whole contract: no device_routes, no plugin nodes, no
// post-device or post-mixer overrides. The binding still creates the per-track
// arrangement-audio and mixer nodes itself, so this topology is at exact parity
// with ArrangementAudioRenderer over the same program - see the bit-exactness
// assertion in test/test_timeline_graph_binding.cpp. Device chains are additive
// work on top of this shape, not a different one.
//
// MIDI destinations are left unset. A device-free graph instantiates no
// instrument, so there is nothing for note events to reach; routing them at a
// node that does not exist would be a lie, not a capability.

#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/playback/program.hpp>

#include <cstdint>
#include <vector>

namespace pulp::host {

enum class TimelineDeviceFreeGraphCode : std::uint8_t {
    Ok,
    /// output_channels was zero, or wider than the graph accepts.
    InvalidChannelCount,
    /// The graph refused the output node.
    OutputNodeRejected,
};

struct TimelineDeviceFreeGraph {
    TimelineDeviceFreeGraphCode code = TimelineDeviceFreeGraphCode::InvalidChannelCount;
    NodeId output_node = 0;
    /// One route per program track, in program order. Empty when the program has
    /// no tracks, which is a valid silent render rather than an error.
    std::vector<TimelineTrackGraphRoute> routes;

    constexpr explicit operator bool() const noexcept {
        return code == TimelineDeviceFreeGraphCode::Ok;
    }
};

/// Adds a single output node to `graph` and returns one device-free route per
/// track in `program`. The caller passes the routes to
/// TimelineGraphPlaybackBinding::prepare(), which creates the per-track audio and
/// mixer nodes and connects them to the returned output node.
TimelineDeviceFreeGraph build_device_free_timeline_graph(SignalGraph& graph,
                                                         const playback::PlaybackProgram& program,
                                                         std::uint32_t output_channels);

} // namespace pulp::host
