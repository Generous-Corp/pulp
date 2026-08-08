#pragma once

#include <pulp/host/signal_graph_executor_routing.hpp>
#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/playback/realtime_stretch_renderer.hpp>

#include "timeline_automation_delivery.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pulp::host {

namespace detail::timeline_graph_binding {

inline TimelineGraphAdmission reject(TimelineGraphAdmissionCode code, std::uint64_t actual = 0,
                                     std::uint64_t limit = 0, timeline::ItemId item = {},
                                     NodeId node = 0) noexcept {
    return {code, actual, limit, item, node};
}

inline bool checked_add(std::uint64_t& value, std::uint64_t add) noexcept {
    if (add > std::numeric_limits<std::uint64_t>::max() - value)
        return false;
    value += add;
    return true;
}

inline std::string custom_type_id(std::uint64_t binding_instance_id, timeline::ItemId id) {
    return "pulp.timeline.arrangement-audio-track." + std::to_string(binding_instance_id) + "." +
           std::to_string(id.value);
}

inline std::string mixer_custom_type_id(std::uint64_t binding_instance_id, timeline::ItemId id) {
    return "pulp.timeline.post-device-track-mixer." + std::to_string(binding_instance_id) + "." +
           std::to_string(id.value);
}

inline double sample_rate_double(timebase::RationalRate rate) noexcept {
    return static_cast<double>(rate.as_long_double());
}

inline void saturating_add(std::uint32_t& destination, std::uint32_t value) noexcept {
    destination = value > std::numeric_limits<std::uint32_t>::max() - destination
                      ? std::numeric_limits<std::uint32_t>::max()
                      : destination + value;
}

struct DetachedAudioEdge {
    NodeId source_node = 0;
    PortIndex source_port = 0;
    NodeId dest_node = 0;
    PortIndex dest_port = 0;

    constexpr bool operator==(const DetachedAudioEdge&) const = default;
};

template <typename Range>
inline bool contains_detached_audio_edge(const Range& edges,
                                         const DetachedAudioEdge& edge) noexcept {
    // Keep this as a predicate search: clang-cl with MSVC STL 14.51 routes
    // std::find for this 16-byte record through an unsupported vectorized path.
    return std::any_of(edges.begin(), edges.end(),
                       [&](const DetachedAudioEdge& candidate) { return candidate == edge; });
}

inline bool is_plain_audio_edge(const Connection& connection,
                                const DetachedAudioEdge& edge) noexcept {
    return connection.source_node == edge.source_node &&
           connection.source_port == edge.source_port && connection.dest_node == edge.dest_node &&
           connection.dest_port == edge.dest_port && !connection.feedback && !connection.midi &&
           !connection.automation && !connection.audio_rate_modulation && !connection.sidechain;
}

} // namespace detail::timeline_graph_binding

struct detail::TimelineGraphSharedBlockState {
    std::atomic<const playback::PlaybackProgramBlock*> block{nullptr};
    std::atomic<const playback::TransportSnapshot*> transport{nullptr};
    std::atomic<playback::RealtimeStretchProgramRuntime*> realtime_stretch{nullptr};
    std::atomic<TimelineGraphProcessCode> audio_code{TimelineGraphProcessCode::Ok};

    void report_audio_code(TimelineGraphProcessCode code) noexcept {
        if (code == TimelineGraphProcessCode::Ok)
            return;
        auto observed = audio_code.load(std::memory_order_relaxed);
        for (;;) {
            const bool observed_is_hard = observed != TimelineGraphProcessCode::Ok &&
                                          observed != TimelineGraphProcessCode::RealtimeStretchGap;
            if (observed_is_hard || (observed == TimelineGraphProcessCode::RealtimeStretchGap &&
                                     code == TimelineGraphProcessCode::RealtimeStretchGap))
                return;
            if (audio_code.compare_exchange_weak(observed, code, std::memory_order_relaxed))
                return;
        }
    }
};

namespace detail::timeline_graph_binding {

struct AudioNodeInstance {
    AudioNodeInstance(std::shared_ptr<TimelineGraphSharedBlockState> shared,
                      std::shared_ptr<playback::ArrangementAudioTrackRenderer> renderer,
                      playback::AudioRendererLimits limits) noexcept
        : shared(std::move(shared)), renderer(std::move(renderer)), limits(limits) {}

    void process(audio::BufferView<float>& output) noexcept {
        // Each graph node owns its renderer, so parallel nodes share only the
        // atomic block pointers and failure code.
        const auto* block = shared->block.load(std::memory_order_acquire);
        const auto* transport = shared->transport.load(std::memory_order_acquire);
        auto* realtime_stretch = shared->realtime_stretch.load(std::memory_order_acquire);
        if (block == nullptr || transport == nullptr) {
            output.clear();
            shared->report_audio_code(TimelineGraphProcessCode::MissingProgram);
            return;
        }
        const auto status = renderer->process(*block, *transport, output, limits, realtime_stretch);
        if (status == playback::AudioRenderStatus::RealtimeStretchGap) {
            shared->report_audio_code(TimelineGraphProcessCode::RealtimeStretchGap);
            return;
        }
        if (status == playback::AudioRenderStatus::RealtimeStretchStateRequired) {
            shared->report_audio_code(TimelineGraphProcessCode::RealtimeStretchStateRequired);
            return;
        }
        if (status == playback::AudioRenderStatus::RealtimeStretchStalePublication) {
            shared->report_audio_code(TimelineGraphProcessCode::RealtimeStretchStalePublication);
            return;
        }
        if (status == playback::AudioRenderStatus::RealtimeStretchImpossibleRatio) {
            shared->report_audio_code(TimelineGraphProcessCode::RealtimeStretchImpossibleRatio);
            return;
        }
        if (status == playback::AudioRenderStatus::RealtimeStretchBackpressure) {
            shared->report_audio_code(TimelineGraphProcessCode::RealtimeStretchBackpressure);
            return;
        }
        if (status == playback::AudioRenderStatus::RealtimeStretchUnderflow) {
            shared->report_audio_code(TimelineGraphProcessCode::RealtimeStretchUnderflow);
            return;
        }
        if (status == playback::AudioRenderStatus::RealtimeStretchUnsupportedScrubbing) {
            shared->report_audio_code(
                TimelineGraphProcessCode::RealtimeStretchUnsupportedScrubbing);
            return;
        }
        if (status != playback::AudioRenderStatus::Rendered &&
            status != playback::AudioRenderStatus::Silent) {
            shared->report_audio_code(TimelineGraphProcessCode::AudioRenderFailed);
        }
    }

    std::shared_ptr<TimelineGraphSharedBlockState> shared;
    std::shared_ptr<playback::ArrangementAudioTrackRenderer> renderer;
    playback::AudioRendererLimits limits;
};

struct MixerNodeInstance {
    MixerNodeInstance(std::shared_ptr<TimelineGraphSharedBlockState> shared,
                      std::shared_ptr<playback::TrackMixerTrackRenderer> renderer,
                      playback::AudioRendererLimits limits) noexcept
        : shared(std::move(shared)), renderer(std::move(renderer)), limits(limits) {}

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input) noexcept {
        const auto* block = shared->block.load(std::memory_order_acquire);
        const auto* transport = shared->transport.load(std::memory_order_acquire);
        if (block == nullptr || transport == nullptr) {
            output.clear();
            shared->report_audio_code(TimelineGraphProcessCode::MissingProgram);
            return;
        }
        const auto status = renderer->process(*block, *transport, output, input, limits);
        if (status != playback::AudioRenderStatus::Rendered &&
            status != playback::AudioRenderStatus::Silent) {
            output.clear();
            shared->report_audio_code(TimelineGraphProcessCode::AudioRenderFailed);
        }
    }

    std::shared_ptr<TimelineGraphSharedBlockState> shared;
    std::shared_ptr<playback::TrackMixerTrackRenderer> renderer;
    playback::AudioRendererLimits limits;
};

} // namespace detail::timeline_graph_binding

struct detail::TimelineGraphBoundTrack {
    timeline::ItemId id;
    NodeId audio_node = 0;
    NodeId mixer_node = 0;
    NodeId midi_node = 0;
    std::shared_ptr<playback::ArrangementAudioTrackRenderer> audio_renderer;
    std::shared_ptr<playback::TrackMixerTrackRenderer> mixer_renderer;
    std::unique_ptr<playback::ArrangementNoteRenderer> note_renderer;
};

struct detail::TimelineGraphBindingState {
    std::vector<std::shared_ptr<TimelineGraphBoundTrack>> tracks;
    std::vector<std::shared_ptr<TimelineGraphAutomationTrack>> automation_tracks;
    TimelineGraphBindingConfig config;
    std::vector<timeline::ItemId> prepared_track_ids;
    std::vector<timeline::ItemId> post_device_routed_track_ids;
    std::vector<timeline_graph_binding::DetachedAudioEdge> detached_post_device_bypasses;
    std::shared_ptr<const playback::PlaybackProgram> program;
    std::shared_ptr<playback::RealtimeStretchProgramRuntime> realtime_stretch;
    SignalGraph::ExecutionSnapshot graph_snapshot;
    std::shared_ptr<ExactParameterIngressOwner> automation_claim_owner;
    mutable std::atomic<bool> delivery_poisoned{false};
    double prepared_sample_rate = 0.0;
    std::uint32_t prepared_max_block_size = 0;
};

struct detail::TimelineGraphPreparedCandidate {
    explicit TimelineGraphPreparedCandidate(
        runtime::Slot<const TimelineGraphBindingState>::ReadGuard previous_state)
        : previous(std::move(previous_state)) {}

    runtime::Slot<const TimelineGraphBindingState>::ReadGuard previous;
    std::shared_ptr<TimelineGraphBindingState> next;
    std::unique_ptr<SignalGraph::PreparedTopologyEdit> edit;
};

namespace detail::timeline_graph_binding {

inline TimelineGraphAdmission admit_candidate(const SignalGraph& graph,
                                              std::span<const GraphNode> nodes,
                                              std::span<const Connection> connections) {
    const auto graph_limits = graph.limits();
    if (nodes.size() > graph_limits.max_nodes)
        return reject(TimelineGraphAdmissionCode::NodeLimitExceeded, nodes.size(),
                      graph_limits.max_nodes);
    if (connections.size() > graph_limits.max_connections)
        return reject(TimelineGraphAdmissionCode::ConnectionLimitExceeded, connections.size(),
                      graph_limits.max_connections);
    std::uint64_t graph_ports = 0;
    for (const auto& node : nodes) {
        if (!checked_add(graph_ports,
                         static_cast<std::uint64_t>(std::max(0, node.num_input_ports))) ||
            !checked_add(graph_ports,
                         static_cast<std::uint64_t>(std::max(0, node.num_output_ports))))
            return reject(TimelineGraphAdmissionCode::TotalPortLimitExceeded,
                          std::numeric_limits<std::uint64_t>::max(), graph_limits.max_ports);
    }
    if (graph_ports > graph_limits.max_ports)
        return reject(TimelineGraphAdmissionCode::TotalPortLimitExceeded, graph_ports,
                      graph_limits.max_ports);

    const auto routed = validate_signal_graph_executor_topology(nodes, connections);
    switch (routed.code) {
    case ExecutorTopologyValidationCode::Accepted:
        return {};
    case ExecutorTopologyValidationCode::TopologyIneligible:
        return reject(TimelineGraphAdmissionCode::RoutedTopologyIneligible);
    case ExecutorTopologyValidationCode::NodeLimitExceeded:
        return reject(TimelineGraphAdmissionCode::NodeLimitExceeded, routed.actual, routed.limit,
                      {}, routed.node);
    case ExecutorTopologyValidationCode::ConnectionLimitExceeded:
        return reject(TimelineGraphAdmissionCode::ConnectionLimitExceeded, routed.actual,
                      routed.limit, {}, routed.node);
    case ExecutorTopologyValidationCode::PerNodePortLimitExceeded:
        return reject(TimelineGraphAdmissionCode::PerNodePortLimitExceeded, routed.actual,
                      routed.limit, {}, routed.node);
    case ExecutorTopologyValidationCode::TotalPortLimitExceeded:
        return reject(TimelineGraphAdmissionCode::TotalPortLimitExceeded, routed.actual,
                      routed.limit, {}, routed.node);
    case ExecutorTopologyValidationCode::PlanRejected:
        return reject(TimelineGraphAdmissionCode::RoutedPlanRejected, routed.index, 0, {},
                      routed.node);
    }
    return reject(TimelineGraphAdmissionCode::RoutedPlanRejected);
}

} // namespace detail::timeline_graph_binding

} // namespace pulp::host
