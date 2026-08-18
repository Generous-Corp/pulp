#pragma once

// One-shot transport-aware offline renderer for a prepared Timeline graph.
//
// WHY THIS EXISTS AND WHY IT IS NOT OfflineSignalGraphHost.
//
// OfflineSignalGraphHost deliberately drives the *no-transport*
// SignalGraph::process() overload: its whole contract is "block partitioning is
// the only difference between online and offline", which requires that nodes
// receive nothing else that varies. Timeline graph nodes are the opposite case
// — TimelineGraphPlaybackBinding::process() needs the exact block and transport
// state for that block, because note scheduling, automation evaluation and PDC
// all read it. Driving a Timeline slice through the legacy offline host does not
// fail loudly; it renders silence. So this is a separate renderer whose block
// loop is MasterTransport::begin_block() -> TimelineGraphPlaybackBinding::
// process(). OfflineSignalGraphHost and core/playback are unchanged.
//
// The renderer is one-shot and fail-closed: it either returns a complete render
// or a status and no audio. There is no partial success, because a truncated
// bounce that looks like a short file is the failure mode this is meant to make
// impossible.
//
// Bounds are authored in ticks and derived to samples ONLY through the program's
// own compiled tempo map, so a tempo change inside the region is honoured
// exactly rather than approximated from a nominal rate.

#include <pulp/audio/audio_file.hpp>
#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/playback/program.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>

#include <cstdint>
#include <span>

namespace pulp::host {

/// Why a render did not produce audio. Ok is the only success value.
enum class TimelineOfflineRenderCode : std::uint8_t {
    Ok = 0,
    /// Bounds were rejected before any work: end <= start, negative ticks, or a
    /// tick range whose sample derivation saturates the tempo map.
    InvalidRange,
    /// A limit was zero, negative, or exceeded (channels, block frames, or the
    /// caller's output-frame ceiling).
    InvalidLimits,
    /// The request's sample rate disagrees with the program's compiled tempo map.
    SampleRateMismatch,
    /// The program has no tracks, no tempo map, or no routes were supplied.
    InvalidProgram,
    /// TimelineGraphPlaybackBinding::prepare()/adopt failed; see `admission`.
    BindingRejected,
    /// MasterTransport::prepare()/seek()/set_playing() failed; see `transport_error`.
    TransportRejected,
    /// A block failed mid-render; see `process`. The partial buffer is discarded.
    ProcessFailed,
};

/// Prepare-time limits. All are required; none has a silent default that would
/// let an under-specified request render something plausible but wrong.
struct TimelineOfflineRenderConfig {
    double sample_rate = 0.0;
    int block_frames = 0;
    int output_channels = 0;
    /// Hard ceiling on rendered frames including the tail pad. A region that
    /// would exceed it fails closed rather than silently truncating.
    std::uint64_t max_output_frames = 0;
    /// Note-event capacity per track per block, forwarded to the binding.
    std::uint32_t maximum_note_events_per_track_per_block = 256;
};

/// One render request over a half-open tick region [start_tick, end_tick).
struct TimelineOfflineRenderOptions {
    timebase::TickPosition start_tick{0};
    timebase::TickPosition end_tick{0};
    /// Frames rendered after the transport stops at end_tick. Zero means the
    /// output ends exactly at end_tick with no pad at all.
    std::uint32_t tail_frames = 0;
};

/// Result of a one-shot render. `audio` is populated only when `code == Ok`.
struct TimelineOfflineRenderResult {
    TimelineOfflineRenderCode code = TimelineOfflineRenderCode::Ok;
    audio::AudioFileData audio;
    /// Frames of `audio` that precede the tail pad. Equals the region length.
    std::uint64_t region_frames = 0;
    /// Frames of tail pad actually rendered. Equals options.tail_frames on Ok.
    std::uint64_t tail_frames = 0;
    /// Populated when code == BindingRejected.
    TimelineGraphAdmission admission{};
    /// Populated when code == ProcessFailed.
    TimelineGraphProcessResult process{};
    /// Populated when code == TransportRejected.
    playback::TransportError transport_error = playback::TransportError::None;

    constexpr explicit operator bool() const noexcept {
        return code == TimelineOfflineRenderCode::Ok;
    }
};

/// Renders `[start_tick, end_tick)` of `program` through `graph`, plus a fixed
/// tail, and returns deterministic Float32 audio.
///
/// `graph` must be freshly built or quiesced: the renderer prepares the binding
/// itself and drives its own transport, so a graph already bound to a live
/// transport would be double-driven.
///
/// A region whose start is not block-aligned is rendered by pre-rolling from
/// tick origin with output discarded, so stateful nodes (delays, PDC) enter the
/// region with exactly the history a full bounce would have given them. That is
/// the difference between "renders a region" and "renders a region correctly",
/// and it is why the region slice equals the corresponding slice of a full
/// bounce rather than merely resembling it.
TimelineOfflineRenderResult render_timeline_offline(
    SignalGraph& graph, TimelineGraphPlaybackBinding& binding,
    const playback::PlaybackProgram& program,
    std::span<const TimelineTrackGraphRoute> routes,
    const TimelineOfflineRenderConfig& config,
    const TimelineOfflineRenderOptions& options);

} // namespace pulp::host
