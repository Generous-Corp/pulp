#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/playback/automation_limits.hpp>
#include <pulp/playback/automation_cursor.hpp>
#include <pulp/playback/track_automation_program.hpp>
#include <pulp/runtime/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::playback {

struct TrackAutomationEvent {
    timeline::ItemId lane_id;
    std::uint32_t param_id = 0;
    std::uint32_t sample_offset = 0;
    float value = 0.0f;
    std::uint32_t ramp_duration_sample_frames = 0;

    constexpr bool operator==(const TrackAutomationEvent&) const = default;
};

struct DeviceAutomationBatch {
    timeline::ItemId device_placement_id;
    std::span<const TrackAutomationEvent> events;
    bool coalesced = false;
};

/// Non-owning association between a canonical device placement and the
/// transport window projected for that device.
struct DeviceAutomationTransportView {
    timeline::ItemId device_placement_id;
    const TransportSnapshot* transport = nullptr;
};

enum class TrackAutomationRendererCode : std::uint8_t {
    Ok,
    Coalesced,
    MissingProgram,
    TrackMismatch,
    InvalidTransport,
    TempoMapMismatch,
    LaneAdoptionRejected,
    LaneCapacityExceeded,
    PointCapacityExceeded,
    DeviceCapacityExceeded,
    WorkCapacityExceeded,
    DeviceScheduleMismatch,
    InvalidLimits,
};

enum class TrackAutomationRendererAdoption : std::uint8_t {
    Adopted,
    Unchanged,
};

struct TrackAutomationRendererError {
    TrackAutomationRendererCode code = TrackAutomationRendererCode::MissingProgram;
    timeline::ItemId track_id;
    timeline::ItemId lane_id;
};

struct TrackAutomationRenderResult {
    TrackAutomationRendererCode code = TrackAutomationRendererCode::Ok;
    std::uint32_t emitted_events = 0;
    std::uint32_t candidate_events = 0;
    timeline::ItemId failed_lane_id;
    timeline::ItemId failed_device_placement_id;
};

/// Allocation-free block renderer for one prepared track automation program.
/// Program adoption is a control-thread operation; it preserves cursor carry
/// for lanes with the same identity and preallocates all block storage.
class TrackAutomationRenderer {
  public:
    static constexpr std::size_t kEventsPerDevice =
        AutomationPlaybackLimits::kMaximumEventsPerDevicePerBlock;
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeWithImmutableInputs;

    static runtime::Result<TrackAutomationRenderer, TrackAutomationRendererError>
    create(std::shared_ptr<const TrackAutomationProgram> program,
           AutomationPlaybackLimits limits = AutomationPlaybackLimits::platform_defaults());

    TrackAutomationRenderer(const TrackAutomationRenderer&) = delete;
    TrackAutomationRenderer& operator=(const TrackAutomationRenderer&) = delete;
    TrackAutomationRenderer(TrackAutomationRenderer&&) noexcept = default;
    TrackAutomationRenderer& operator=(TrackAutomationRenderer&&) noexcept = default;

    /// Rebuilds prepared storage while the caller has exclusive access. A
    /// concurrently published renderer must instead be created afresh.
    runtime::Result<TrackAutomationRendererAdoption, TrackAutomationRendererError>
    adopt(std::shared_ptr<const TrackAutomationProgram> program);

    TrackAutomationRenderResult process(const TransportSnapshot& transport) noexcept;
    TrackAutomationRenderResult
    process(std::span<const DeviceAutomationTransportView> transports) noexcept;
    void reset() noexcept;

    timeline::ItemId track_id() const noexcept;
    const std::shared_ptr<const TrackAutomationProgram>& program() const noexcept {
        return program_;
    }
    const AutomationPlaybackLimits& limits() const noexcept {
        return limits_;
    }
    std::span<const DeviceAutomationBatch> batches() const noexcept {
        return batch_views_;
    }

  private:
    TrackAutomationRenderer() = default;

    TrackAutomationRenderResult
    process_impl(const TransportSnapshot* uniform_transport,
                 std::span<const DeviceAutomationTransportView> transports) noexcept;

    struct LaneState {
        std::shared_ptr<const AutomationProgram> program;
        AutomationCursor cursor;
        AutomationCursor next_cursor;
        std::array<AutomationBlockEvent, kEventsPerDevice> scratch{};
        std::uint32_t event_count = 0;
        std::uint32_t merge_position = 0;
        std::uint32_t selected_sample_offset = 0;
        bool has_selected_event = false;
        bool coalesced = false;
        std::size_t device_index = 0;
    };

    struct DeviceState {
        struct QueuedEvent {
            TrackAutomationEvent event;
            std::uint32_t lane_sequence = 0;
        };

        timeline::ItemId device_placement_id;
        std::vector<std::size_t> lane_indices;
        std::vector<std::size_t> merge_heap;
        std::array<QueuedEvent, kEventsPerDevice> queued_events{};
        std::array<TrackAutomationEvent, kEventsPerDevice> events{};
        std::uint32_t event_count = 0;
        bool coalesced = false;
    };

    std::shared_ptr<const TrackAutomationProgram> program_;
    AutomationPlaybackLimits limits_;
    std::vector<LaneState> lanes_;
    std::vector<DeviceState> devices_;
    std::vector<DeviceAutomationBatch> batch_views_;
};

} // namespace pulp::playback
