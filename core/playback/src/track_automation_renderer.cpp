#include <pulp/playback/track_automation_renderer.hpp>

#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace pulp::playback {
namespace {

runtime::Result<TrackAutomationRendererAdoption, TrackAutomationRendererError>
adoption_error(TrackAutomationRendererCode code, timeline::ItemId track_id = {}) {
    return runtime::Err(TrackAutomationRendererError{code, track_id, {}});
}

TrackAutomationRendererCode cursor_code(AutomationCursorCode code) noexcept {
    switch (code) {
    case AutomationCursorCode::InvalidTransport:
        return TrackAutomationRendererCode::InvalidTransport;
    case AutomationCursorCode::TempoMapMismatch:
        return TrackAutomationRendererCode::TempoMapMismatch;
    case AutomationCursorCode::AdoptionRejected:
        return TrackAutomationRendererCode::LaneAdoptionRejected;
    case AutomationCursorCode::InsufficientCapacity:
        return TrackAutomationRendererCode::LaneCapacityExceeded;
    case AutomationCursorCode::Ok:
    case AutomationCursorCode::Coalesced:
        return TrackAutomationRendererCode::Ok;
    }
    return TrackAutomationRendererCode::LaneCapacityExceeded;
}

std::uint64_t selected_rank(std::uint32_t selection, std::uint32_t selected_count,
                            std::uint64_t candidate_count) noexcept {
    if (selected_count <= 1u || candidate_count <= 1u)
        return candidate_count == 0 ? 0 : candidate_count - 1u;
    return static_cast<std::uint64_t>(selection) * (candidate_count - 1u) /
           (selected_count - 1u);
}

} // namespace

runtime::Result<TrackAutomationRenderer, TrackAutomationRendererError>
TrackAutomationRenderer::create(std::shared_ptr<const TrackAutomationProgram> program) {
    if (!program)
        return runtime::Err(
            TrackAutomationRendererError{TrackAutomationRendererCode::MissingProgram, {}, {}});
    TrackAutomationRenderer renderer;
    auto adopted = renderer.adopt(std::move(program));
    if (!adopted)
        return runtime::Err(adopted.error());
    return runtime::Ok(std::move(renderer));
}

runtime::Result<TrackAutomationRendererAdoption, TrackAutomationRendererError>
TrackAutomationRenderer::adopt(std::shared_ptr<const TrackAutomationProgram> program) {
    if (!program)
        return adoption_error(TrackAutomationRendererCode::MissingProgram);
    if (program_ && program->track_id() != program_->track_id())
        return adoption_error(TrackAutomationRendererCode::TrackMismatch, program->track_id());
    if (program == program_)
        return runtime::Ok(TrackAutomationRendererAdoption::Unchanged);

    std::vector<LaneState> next_lanes;
    next_lanes.reserve(program->programs().size());
    for (const auto& next_program : program->programs()) {
        LaneState state;
        state.program = next_program;
        const auto found = std::lower_bound(
            lanes_.begin(), lanes_.end(), next_program->lane_id(),
            [](const LaneState& lane, timeline::ItemId id) {
                return lane.program->lane_id() < id;
            });
        if (found != lanes_.end() && found->program->lane_id() == next_program->lane_id()) {
            const auto active_generation = found->cursor.active_generation();
            if (found->program != next_program && active_generation != 0 &&
                next_program->generation() <= active_generation) {
                return runtime::Err(TrackAutomationRendererError{
                    TrackAutomationRendererCode::LaneAdoptionRejected, program->track_id(),
                    next_program->lane_id()});
            }
            state.cursor = found->cursor;
        }
        next_lanes.push_back(std::move(state));
    }

    std::vector<timeline::ItemId> device_ids;
    device_ids.reserve(program->programs().size());
    for (const auto& next_program : program->programs())
        device_ids.push_back(next_program->target().device_placement_id());
    std::sort(device_ids.begin(), device_ids.end());
    device_ids.erase(std::unique(device_ids.begin(), device_ids.end()), device_ids.end());

    std::vector<DeviceState> next_devices;
    next_devices.reserve(device_ids.size());
    for (const auto device_id : device_ids)
        next_devices.push_back(DeviceState{device_id, {}, {}});
    for (std::size_t lane_index = 0; lane_index < next_lanes.size(); ++lane_index) {
        const auto device_id = next_lanes[lane_index].program->target().device_placement_id();
        const auto found = std::lower_bound(
            next_devices.begin(), next_devices.end(), device_id,
            [](const DeviceState& device, timeline::ItemId id) {
                return device.device_placement_id < id;
            });
        found->lane_indices.push_back(lane_index);
    }
    for (auto& device : next_devices)
        device.merge_heap.resize(device.lane_indices.size());

    std::vector<DeviceAutomationBatch> next_views(next_devices.size());
    program_ = std::move(program);
    lanes_ = std::move(next_lanes);
    devices_ = std::move(next_devices);
    batch_views_ = std::move(next_views);
    return runtime::Ok(TrackAutomationRendererAdoption::Adopted);
}

TrackAutomationRenderResult
TrackAutomationRenderer::process(const TransportSnapshot& transport) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    TrackAutomationRenderResult result;
    if (!program_) {
        result.code = TrackAutomationRendererCode::MissingProgram;
        return result;
    }
    for (std::size_t index = 0; index < devices_.size(); ++index) {
        devices_[index].event_count = 0;
        devices_[index].coalesced = false;
        batch_views_[index] = {devices_[index].device_placement_id, {}, false};
    }
    if (!valid_transport_ranges(transport)) {
        result.code = TrackAutomationRendererCode::InvalidTransport;
        return result;
    }
    if (transport.tempo_map != &program_->tempo_map()) {
        result.code = TrackAutomationRendererCode::TempoMapMismatch;
        return result;
    }

    bool lane_coalesced = false;
    for (auto& lane : lanes_) {
        lane.next_cursor = lane.cursor;
        const auto rendered = lane.next_cursor.process(*lane.program, transport, lane.scratch);
        if (rendered.code != AutomationCursorCode::Ok &&
            rendered.code != AutomationCursorCode::Coalesced) {
            result.code = cursor_code(rendered.code);
            result.failed_lane_id = lane.program->lane_id();
            return result;
        }
        lane.event_count = rendered.emitted_events;
        lane.coalesced = rendered.code == AutomationCursorCode::Coalesced;
        lane_coalesced = lane_coalesced || lane.coalesced;
    }

    std::uint64_t total_candidates = 0;
    std::uint64_t total_emitted = 0;
    for (std::size_t device_index = 0; device_index < devices_.size(); ++device_index) {
        auto& device = devices_[device_index];
        std::uint64_t candidate_count = 0;
        std::uint64_t mandatory_count = 0;
        bool device_lane_coalesced = false;
        for (const auto lane_index : device.lane_indices) {
            candidate_count += lanes_[lane_index].event_count;
            device_lane_coalesced = device_lane_coalesced || lanes_[lane_index].coalesced;
            for (std::uint32_t event_index = 0; event_index < lanes_[lane_index].event_count;
                 ++event_index) {
                if (lanes_[lane_index].scratch[event_index].transition !=
                    AutomationTransition::LinearRamp) {
                    ++mandatory_count;
                }
            }
        }
        if (mandatory_count > kEventsPerDevice) {
            result.code = TrackAutomationRendererCode::DeviceCapacityExceeded;
            result.failed_device_placement_id = device.device_placement_id;
            return result;
        }

        const auto optional_count = candidate_count - mandatory_count;
        const auto selected_optional_count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(optional_count, kEventsPerDevice - mandatory_count));
        const auto selected_count =
            static_cast<std::uint32_t>(mandatory_count) + selected_optional_count;
        device.event_count = selected_count;
        device.coalesced = device_lane_coalesced || candidate_count > kEventsPerDevice;
        std::size_t heap_size = 0;
        for (const auto lane_index : device.lane_indices) {
            auto& lane = lanes_[lane_index];
            lane.merge_position = 0;
            lane.selected_sample_offset = 0;
            lane.has_selected_event = false;
            if (lane.event_count != 0)
                device.merge_heap[heap_size++] = lane_index;
        }
        const auto lane_less = [&](std::size_t lhs_index, std::size_t rhs_index) noexcept {
            const auto& lhs = lanes_[lhs_index];
            const auto& rhs = lanes_[rhs_index];
            const auto& lhs_event = lhs.scratch[lhs.merge_position];
            const auto& rhs_event = rhs.scratch[rhs.merge_position];
            return lhs_event.sample_offset < rhs_event.sample_offset ||
                   (lhs_event.sample_offset == rhs_event.sample_offset &&
                    lhs.program->lane_id() < rhs.program->lane_id());
        };
        const auto sift_down = [&](std::size_t root) noexcept {
            while (true) {
                const auto left = root * 2u + 1u;
                if (left >= heap_size)
                    return;
                const auto right = left + 1u;
                auto smallest = left;
                if (right < heap_size &&
                    lane_less(device.merge_heap[right], device.merge_heap[left])) {
                    smallest = right;
                }
                if (!lane_less(device.merge_heap[smallest], device.merge_heap[root]))
                    return;
                std::swap(device.merge_heap[root], device.merge_heap[smallest]);
                root = smallest;
            }
        };
        if (heap_size > 1u) {
            for (auto parent = heap_size / 2u; parent > 0; --parent)
                sift_down(parent - 1u);
        }

        std::uint64_t optional_rank = 0;
        std::uint32_t optional_selection = 0;
        std::uint32_t queued_count = 0;
        while (heap_size != 0) {
            const auto best_lane = device.merge_heap[0];
            auto& lane = lanes_[best_lane];
            const auto lane_sequence = lane.merge_position;
            const auto& event = lane.scratch[lane.merge_position++];
            const bool mandatory = event.transition != AutomationTransition::LinearRamp;
            bool selected = mandatory;
            if (!mandatory) {
                const auto wanted_rank =
                    selected_optional_count == 0
                        ? std::numeric_limits<std::uint64_t>::max()
                        : selected_rank(optional_selection, selected_optional_count,
                                        optional_count);
                selected = optional_selection < selected_optional_count &&
                           optional_rank == wanted_rank;
                if (selected)
                    ++optional_selection;
                ++optional_rank;
            }
            if (selected) {
                auto& queued = device.queued_events[queued_count++];
                auto sample_offset = event.sample_offset;
                std::uint32_t ramp_duration = 0;
                if (event.transition == AutomationTransition::LinearRamp &&
                    lane.has_selected_event) {
                    sample_offset = lane.selected_sample_offset;
                    ramp_duration = event.sample_offset - lane.selected_sample_offset;
                }
                queued = {{lane.program->lane_id(), lane.program->target().param_id,
                           sample_offset, event.value, ramp_duration},
                          lane_sequence};
                lane.selected_sample_offset = event.sample_offset;
                lane.has_selected_event = true;
            }
            if (lane.merge_position < lane.event_count) {
                sift_down(0);
            } else {
                --heap_size;
                if (heap_size != 0) {
                    device.merge_heap[0] = device.merge_heap[heap_size];
                    sift_down(0);
                }
            }
        }
        std::sort(device.queued_events.begin(), device.queued_events.begin() + queued_count,
                  [](const DeviceState::QueuedEvent& lhs,
                     const DeviceState::QueuedEvent& rhs) noexcept {
                      if (lhs.event.sample_offset != rhs.event.sample_offset)
                          return lhs.event.sample_offset < rhs.event.sample_offset;
                      if (lhs.event.lane_id != rhs.event.lane_id)
                          return lhs.event.lane_id < rhs.event.lane_id;
                      return lhs.lane_sequence < rhs.lane_sequence;
                  });
        for (std::uint32_t index = 0; index < queued_count; ++index)
            device.events[index] = device.queued_events[index].event;
        device.event_count = queued_count;

        batch_views_[device_index] = {
            device.device_placement_id,
            std::span<const TrackAutomationEvent>(device.events.data(), device.event_count),
            device.coalesced,
        };
        total_candidates += candidate_count;
        total_emitted += queued_count;
    }

    for (auto& lane : lanes_)
        lane.cursor = lane.next_cursor;

    result.candidate_events = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(total_candidates, std::numeric_limits<std::uint32_t>::max()));
    result.emitted_events = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(total_emitted, std::numeric_limits<std::uint32_t>::max()));
    result.code = lane_coalesced || total_candidates > total_emitted
                      ? TrackAutomationRendererCode::Coalesced
                      : TrackAutomationRendererCode::Ok;
    return result;
}

void TrackAutomationRenderer::reset() noexcept {
    for (auto& lane : lanes_)
        lane.cursor.reset();
    for (auto& device : devices_) {
        device.event_count = 0;
        device.coalesced = false;
    }
    for (auto& batch : batch_views_)
        batch = {batch.device_placement_id, {}, false};
}

timeline::ItemId TrackAutomationRenderer::track_id() const noexcept {
    return program_ ? program_->track_id() : timeline::ItemId{};
}

} // namespace pulp::playback
