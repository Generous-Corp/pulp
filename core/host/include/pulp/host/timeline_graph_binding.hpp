#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/note_renderer.hpp>
#include <pulp/runtime/slot.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::host {

namespace detail {
struct TimelineGraphSharedBlockState;
struct TimelineGraphBoundTrack;
struct TimelineGraphBindingState;
struct TimelineGraphPreparedCandidate;
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

    /// Reconciles stable ItemId-keyed nodes through an isolated prepared graph
    /// transaction and pins canonical routed execution. Existing nodes for
    /// unchanged track IDs retain their SignalGraph NodeIds across calls. A
    /// failed mutation, prepare, routed admission, or commit leaves both the
    /// live graph and the binding's last published state untouched unless a
    /// quiesced lifecycle rollback fails, in which case both revoke publication.
    TimelineGraphAdmission prepare(const playback::PlaybackProgram& program,
                                   std::span<const TimelineTrackGraphRoute> routes,
                                   const TimelineGraphBindingConfig& config, double sample_rate,
                                   int maximum_block_size);

    /// Re-publishes content for the existing topology as one immutable binding
    /// generation. This is the only content-program adoption path; process()
    /// never independently latches PlaybackProgramStore.
    TimelineGraphAdmission adopt_program(const playback::PlaybackProgram& program);
    TimelineGraphAdmission adopt_latest_program();

    /// Dimension-changing prepare for a host lifecycle boundary where audio
    /// process(), MIDI injection, and anticipation are stopped. External plugins
    /// and retained custom nodes are re-prepared; renderer carry is deliberately
    /// reset after a successful sample-rate or maximum-block change.
    TimelineGraphAdmission prepare_quiesced(
        const playback::PlaybackProgram& program,
        std::span<const TimelineTrackGraphRoute> routes,
        const TimelineGraphBindingConfig& config, double sample_rate,
        int maximum_block_size);

    using BeforeBindingPublishHookForTest = void (*)(void*) noexcept;
    void set_before_binding_publish_hook_for_test(
        BeforeBindingPublishHookForTest hook, void* context = nullptr) noexcept {
        before_binding_publish_hook_for_test_ = hook;
        before_binding_publish_context_for_test_ = context;
    }
    void set_before_graph_commit_hook_for_test(
        BeforeBindingPublishHookForTest hook, void* context = nullptr) noexcept {
        before_graph_commit_hook_for_test_ = hook;
        before_graph_commit_context_for_test_ = context;
    }

    TimelineGraphProcessResult process(audio::BufferView<float>& output,
                                       const audio::BufferView<const float>& input,
                                       const playback::TransportSnapshot& transport) noexcept;

    NodeId audio_node_for(timeline::ItemId track_id) const noexcept;
    NodeId midi_input_node_for(timeline::ItemId track_id) const noexcept;
    playback::RendererProgramKey renderer_key_for(timeline::ItemId track_id) const noexcept;
    playback::RendererCarryState renderer_state_for(timeline::ItemId track_id) const noexcept;

  private:
    TimelineGraphAdmission build_candidate(
        const playback::PlaybackProgram& program,
        std::span<const TimelineTrackGraphRoute> routes,
        const TimelineGraphBindingConfig& config, double sample_rate,
        int maximum_block_size,
        detail::TimelineGraphPreparedCandidate& candidate) const;
    TimelineGraphAdmission prepare_impl(
        const playback::PlaybackProgram& program,
        std::span<const TimelineTrackGraphRoute> routes,
        const TimelineGraphBindingConfig& config, double sample_rate,
        int maximum_block_size, bool quiesced);
    void remove_all_owned_nodes() noexcept;

    SignalGraph& graph_;
    const playback::PlaybackProgramStore& store_;
    std::shared_ptr<detail::TimelineGraphSharedBlockState> shared_;
    runtime::Slot<const detail::TimelineGraphBindingState> state_;
    std::uint64_t binding_instance_id_ = 0;
    BeforeBindingPublishHookForTest before_binding_publish_hook_for_test_ = nullptr;
    void* before_binding_publish_context_for_test_ = nullptr;
    BeforeBindingPublishHookForTest before_graph_commit_hook_for_test_ = nullptr;
    void* before_graph_commit_context_for_test_ = nullptr;
};

} // namespace pulp::host
