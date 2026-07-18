#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/note_renderer.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::host {

namespace detail {
struct TimelineGraphSharedBlockState;
struct TimelineGraphBoundTrack;
} // namespace detail

/// One phase-1 track's lowering into the desktop SignalGraph adapter.
/// `audio_destination` is required and receives `audio_channels` consecutive
/// ports. `midi_destination == 0` keeps the separately-rendered note stream
/// available at the track's MidiInput node without connecting it onward.
struct TimelineTrackGraphRoute {
    timeline::ItemId track_id;
    NodeId audio_destination = 0;
    PortIndex audio_destination_first_port = 0;
    NodeId midi_destination = 0;
};

struct TimelineGraphBindingConfig {
    std::uint32_t audio_channels = 2;
    std::size_t maximum_note_events_per_track_per_block = 1024;
    playback::AudioRendererLimits audio_limits{};
};

enum class TimelineGraphAdmissionCode : std::uint8_t {
    Accepted,
    InvalidConfiguration,
    DuplicateTrack,
    MissingTrack,
    MissingDestination,
    DestinationPortRange,
    NodeLimitExceeded,
    ConnectionLimitExceeded,
    PerNodePortLimitExceeded,
    TotalPortLimitExceeded,
    RoutedPlanRejected,
    RoutedTopologyIneligible,
    GraphMutationFailed,
    GraphPrepareFailed,
    SampleRateMismatch,
    NoteCapacityExceeded,
};

/// Structured admission result. Capacity failures always report the exact
/// candidate value and the limit that rejected it; `item`/`node` identify the
/// relevant route where one exists.
struct TimelineGraphAdmission {
    TimelineGraphAdmissionCode code = TimelineGraphAdmissionCode::Accepted;
    std::uint64_t actual = 0;
    std::uint64_t limit = 0;
    timeline::ItemId item;
    NodeId node = 0;
    constexpr explicit operator bool() const noexcept {
        return code == TimelineGraphAdmissionCode::Accepted;
    }
};

enum class TimelineGraphProcessCode : std::uint8_t {
    Ok,
    MissingProgram,
    InvalidTransport,
    NoteRenderFailed,
    MidiInjectionFailed,
    AudioRenderFailed,
    CapacityExceeded,
    InputShapeMismatch,
    TopologyChanged,
    RoutedDispatchFailed,
};

struct TimelineGraphProcessResult {
    TimelineGraphProcessCode code = TimelineGraphProcessCode::Ok;
    std::uint32_t emitted_note_events = 0;
    std::uint32_t dropped_note_events = 0;
    constexpr explicit operator bool() const noexcept {
        return code == TimelineGraphProcessCode::Ok;
    }
};

/// Desktop adapter that lowers phase-1 timeline tracks into stable SignalGraph
/// nodes. The graph and program store must outlive this binding. Topology work
/// is control-thread-only; process() is the single audio-thread entry point.
class TimelineGraphPlaybackBinding {
  public:
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;
    static constexpr std::size_t maximum_graph_midi_events_per_block = 1024;

    TimelineGraphPlaybackBinding(SignalGraph& graph, const playback::PlaybackProgramStore& store);
    ~TimelineGraphPlaybackBinding();
    TimelineGraphPlaybackBinding(const TimelineGraphPlaybackBinding&) = delete;
    TimelineGraphPlaybackBinding& operator=(const TimelineGraphPlaybackBinding&) = delete;

    TimelineGraphAdmission preflight(const playback::PlaybackProgram& program,
                                     std::span<const TimelineTrackGraphRoute> routes,
                                     const TimelineGraphBindingConfig& config,
                                     int maximum_block_size) const;

    /// Reconciles stable ItemId-keyed nodes, pins canonical routed execution,
    /// and prepares the graph. Existing nodes for unchanged track IDs retain
    /// their SignalGraph NodeIds across calls.
    TimelineGraphAdmission prepare(const playback::PlaybackProgram& program,
                                   std::span<const TimelineTrackGraphRoute> routes,
                                   const TimelineGraphBindingConfig& config, double sample_rate,
                                   int maximum_block_size);

    TimelineGraphProcessResult process(audio::BufferView<float>& output,
                                       const audio::BufferView<const float>& input,
                                       const playback::TransportSnapshot& transport) noexcept;

    NodeId audio_node_for(timeline::ItemId track_id) const noexcept;
    NodeId midi_input_node_for(timeline::ItemId track_id) const noexcept;
    playback::RendererProgramKey renderer_key_for(timeline::ItemId track_id) const noexcept;
    playback::RendererCarryState renderer_state_for(timeline::ItemId track_id) const noexcept;

  private:
    void remove_all_owned_nodes() noexcept;

    SignalGraph& graph_;
    const playback::PlaybackProgramStore& store_;
    playback::PlaybackProgramBlockLatch latch_;
    std::shared_ptr<detail::TimelineGraphSharedBlockState> shared_;
    std::vector<std::unique_ptr<detail::TimelineGraphBoundTrack>> tracks_;
    TimelineGraphBindingConfig config_{};
    std::vector<timeline::ItemId> prepared_track_ids_;
    std::uint64_t binding_instance_id_ = 0;
    double prepared_sample_rate_ = 0.0;
    std::uint32_t prepared_max_block_size_ = 0;
    bool prepared_ = false;
};

using TimelineGraphBinding = TimelineGraphPlaybackBinding;

} // namespace pulp::host
