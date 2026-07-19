#include <pulp/playback/automation_cursor.hpp>

#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>

namespace pulp::playback {
namespace {

struct EvaluatedPoint {
    float value = 0.0f;
    std::size_t segment_index = 0;
};

struct RangeTopology {
    std::uint32_t mandatory = 1;
    std::uint32_t refinable = 0;
};

timebase::SamplePosition add_saturating(timebase::SamplePosition start,
                                        std::uint32_t offset) noexcept {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (start.value > maximum - static_cast<std::int64_t>(offset))
        return {maximum};
    return {start.value + static_cast<std::int64_t>(offset)};
}

std::size_t segment_for_sample(std::span<const AutomationProgramSegment> segments,
                               timebase::SamplePosition sample) noexcept {
    const auto found = std::upper_bound(
        segments.begin(), segments.end(), sample,
        [](timebase::SamplePosition value, const AutomationProgramSegment& segment) {
            return value < segment.end_sample;
        });
    return found == segments.end() ? segments.size() - 1u
                                   : static_cast<std::size_t>(found - segments.begin());
}

template <typename Visitor>
void visit_unique_knots(std::span<const AutomationProgramSegment> segments,
                        Visitor&& visitor) noexcept {
    if (segments.empty())
        return;
    auto previous = segments.front().start_sample;
    visitor(previous);
    for (const auto& segment : segments) {
        if (segment.end_sample == previous)
            continue;
        previous = segment.end_sample;
        visitor(previous);
    }
}

std::span<const AutomationProgramSegment>
segments_intersecting(const AutomationProgram& program, const TransportRange& range,
                      std::uint32_t frames) noexcept {
    const auto segments = program.segments();
    const auto first = std::lower_bound(
        segments.begin(), segments.end(), range.timeline_sample_start,
        [](const AutomationProgramSegment& segment, timebase::SamplePosition sample) {
            return segment.end_sample < sample;
        });
    const auto range_last = add_saturating(range.timeline_sample_start, frames - 1u);
    const auto last = std::upper_bound(
        first, segments.end(), range_last,
        [](timebase::SamplePosition sample, const AutomationProgramSegment& segment) {
            return sample < segment.start_sample;
        });
    return {first, last};
}

bool frame_offset_for(const TransportRange& range, std::uint32_t frames,
                      timebase::SamplePosition sample, std::uint32_t& offset) noexcept {
    if (sample < range.timeline_sample_start)
        return false;
    const auto distance = static_cast<std::uint64_t>(sample.value) -
                          static_cast<std::uint64_t>(range.timeline_sample_start.value);
    if (distance >= frames)
        return false;
    offset = static_cast<std::uint32_t>(distance);
    return true;
}

bool refinable_interval(const AutomationProgramSegment& segment, const TransportRange& range,
                        std::uint32_t frames, std::uint32_t& first,
                        std::uint32_t& count) noexcept {
    if (segment.interpolation != timeline::AutomationInterpolation::Continuous ||
        std::bit_cast<std::uint32_t>(segment.start_value) ==
            std::bit_cast<std::uint32_t>(segment.end_value) ||
        segment.end_sample <= range.timeline_sample_start) {
        return false;
    }

    const auto range_last = add_saturating(range.timeline_sample_start, frames - 1u);
    if (segment.start_sample > range_last)
        return false;
    const auto clipped_start = std::max(segment.start_sample, range.timeline_sample_start);
    const auto clipped_end = std::min(segment.end_sample, range_last);
    std::uint32_t start_offset = 0;
    std::uint32_t end_offset = 0;
    if (!frame_offset_for(range, frames, clipped_start, start_offset) ||
        !frame_offset_for(range, frames, clipped_end, end_offset) || start_offset >= end_offset) {
        return false;
    }

    first = start_offset + 1u;
    auto last = end_offset;
    if (clipped_end == segment.end_sample) {
        if (last == 0)
            return false;
        --last;
    }
    if (first > last)
        return false;
    count = last - first + 1u;
    return true;
}

RangeTopology range_topology(const AutomationProgram& program, const TransportRange& range,
                             std::uint32_t frames) noexcept {
    RangeTopology topology;
    const auto segments = segments_intersecting(program, range, frames);
    visit_unique_knots(segments, [&](timebase::SamplePosition sample) noexcept {
        std::uint32_t offset = 0;
        if (frame_offset_for(range, frames, sample, offset) && offset != 0)
            ++topology.mandatory;
    });
    for (const auto& segment : segments) {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        if (refinable_interval(segment, range, frames, first, count))
            topology.refinable += count;
    }
    return topology;
}

std::uint32_t refinement_rank(std::uint32_t selection, std::uint32_t selected_count,
                              std::uint32_t candidate_count) noexcept {
    const auto numerator = static_cast<std::uint64_t>(selection + 1u) *
                           (static_cast<std::uint64_t>(candidate_count) + 1u);
    const auto rank = numerator / (static_cast<std::uint64_t>(selected_count) + 1u) - 1u;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(rank, candidate_count - 1u));
}

std::uint32_t populate_range_selection(const AutomationProgram& program,
                                       const TransportRange& range, std::uint32_t frames,
                                       std::uint32_t refinable_count,
                                       std::uint32_t selected_refinements,
                                       std::span<AutomationBlockEvent> output) noexcept {
    std::uint32_t written = 0;
    output[written++] = {range.sample_offset, 0.0f, AutomationTransition::Seed};
    const auto segments = segments_intersecting(program, range, frames);
    visit_unique_knots(segments, [&](timebase::SamplePosition sample) noexcept {
        std::uint32_t offset = 0;
        if (frame_offset_for(range, frames, sample, offset) && offset != 0) {
            output[written++] = {range.sample_offset + offset, 0.0f,
                                 AutomationTransition::Immediate};
        }
    });

    std::uint32_t selection = 0;
    std::uint32_t candidates_before = 0;
    auto next_rank = selected_refinements == 0
                         ? std::numeric_limits<std::uint32_t>::max()
                         : refinement_rank(0, selected_refinements, refinable_count);
    for (const auto& segment : segments) {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        if (!refinable_interval(segment, range, frames, first, count))
            continue;
        while (selection < selected_refinements && next_rank < candidates_before + count) {
            const auto offset = first + (next_rank - candidates_before);
            output[written++] = {range.sample_offset + offset, 0.0f,
                                 AutomationTransition::LinearRamp};
            ++selection;
            next_rank = selection < selected_refinements
                            ? refinement_rank(selection, selected_refinements, refinable_count)
                            : std::numeric_limits<std::uint32_t>::max();
        }
        candidates_before += count;
    }
    std::sort(output.begin(), output.begin() + written,
              [](const auto& lhs, const auto& rhs) { return lhs.sample_offset < rhs.sample_offset; });
    return written;
}

EvaluatedPoint evaluate(const AutomationProgram& program, timebase::SamplePosition sample,
                        timebase::TempoCursor& tempo, bool cold,
                        std::size_t& segment_index) noexcept {
    const auto segments = program.segments();
    if (cold) {
        segment_index = segment_for_sample(segments, sample);
    } else {
        while (segment_index + 1u < segments.size() &&
               sample >= segments[segment_index].end_sample) {
            ++segment_index;
        }
    }
    if (sample < segments.front().start_sample)
        return {program.leading_value(), segment_index};

    const auto tick = cold ? tempo.seek(sample).tick : tempo.advance(sample).tick;
    const auto& segment = segments[segment_index];
    if (segment.start_sample == segment.end_sample)
        return {segment.end_value, segment_index};
    if (segment.start_tick == segment.end_tick || tick <= segment.start_tick)
        return {segment.start_value, segment_index};
    if (tick >= segment.end_tick)
        return {segment.end_value, segment_index};
    if (segment.interpolation == timeline::AutomationInterpolation::Hold)
        return {segment.start_value, segment_index};
    return {timeline::evaluate_continuous_automation_segment(tick, segment.start_tick,
                                                             segment.end_tick, segment.start_value,
                                                             segment.end_value, segment.curvature),
            segment_index};
}

AutomationTransition transition_between(std::span<const AutomationProgramSegment> segments,
                                        std::size_t from, std::size_t to) noexcept {
    const auto last = std::min(to > from ? to - 1u : from, segments.size() - 1u);
    for (auto index = from; index <= last; ++index) {
        if (segments[index].start_sample < segments[index].end_sample &&
            segments[index].interpolation == timeline::AutomationInterpolation::Hold)
            return AutomationTransition::Immediate;
    }
    return AutomationTransition::LinearRamp;
}

} // namespace

void AutomationCursor::reset() noexcept {
    active_key_ = {};
    active_instance_token_ = {};
    has_block_index_ = false;
    last_block_index_ = 0;
}

AutomationCursorResult AutomationCursor::process(const AutomationProgram& program,
                                                 const TransportSnapshot& transport,
                                                 std::span<AutomationBlockEvent> output) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    AutomationCursorResult result;
    if (!valid_transport_ranges(transport)) {
        result.code = AutomationCursorCode::InvalidTransport;
        return result;
    }
    if (transport.tempo_map != &program.tempo_map()) {
        result.code = AutomationCursorCode::TempoMapMismatch;
        return result;
    }

    const RendererProgramKey candidate{program.lane_id(), program.generation()};
    const auto candidate_instance_token = program.instance_token();
    AutomationProgramAdoption adoption = AutomationProgramAdoption::Unchanged;
    if (active_key_.generation == 0) {
        adoption = AutomationProgramAdoption::Adopted;
    } else if (candidate == active_key_ && candidate_instance_token == active_instance_token_) {
        adoption = AutomationProgramAdoption::Unchanged;
    } else if (is_monotonic_renderer_adoption(active_key_, candidate)) {
        adoption = AutomationProgramAdoption::Adopted;
    } else {
        result.code = AutomationCursorCode::AdoptionRejected;
        result.adoption = AutomationProgramAdoption::Rejected;
        return result;
    }

    const bool block_sequence_reset =
        has_block_index_ && transport.block_index != last_block_index_ + 1u;
    const bool stopped_seed = adoption == AutomationProgramAdoption::Adopted ||
                              block_sequence_reset || transport.ranges[0].discontinuity;
    const auto active_ranges = static_cast<std::uint8_t>(
        transport.is_playing ? transport.range_count : (stopped_seed ? 1u : 0u));

    if (program.empty() || active_ranges == 0) {
        active_key_ = candidate;
        active_instance_token_ = candidate_instance_token;
        has_block_index_ = true;
        last_block_index_ = transport.block_index;
        result.adoption = adoption;
        return result;
    }
    std::array<RangeTopology, 2> topologies{};
    std::array<std::uint32_t, 2> selected_refinements{};
    std::uint32_t mandatory_total = 0;
    std::uint32_t refinable_total = 0;
    for (std::uint8_t index = 0; index < active_ranges; ++index) {
        const auto frames = transport.is_playing ? transport.ranges[index].frame_count : 1u;
        topologies[index] = range_topology(program, transport.ranges[index], frames);
        mandatory_total += topologies[index].mandatory;
        refinable_total += topologies[index].refinable;
    }
    result.candidate_points = mandatory_total + refinable_total;
    const auto usable_capacity = static_cast<std::uint32_t>(
        std::min<std::size_t>(output.size(), std::numeric_limits<std::uint32_t>::max()));
    if (usable_capacity < mandatory_total) {
        result.code = AutomationCursorCode::InsufficientCapacity;
        return result;
    }
    const auto selected_refinement_total =
        std::min(usable_capacity - mandatory_total, refinable_total);
    std::uint32_t selected_total = 0;
    while (selected_total < selected_refinement_total) {
        std::uint8_t best = 0;
        std::uint32_t best_remaining = 0;
        for (std::uint8_t index = 0; index < active_ranges; ++index) {
            const auto remaining = topologies[index].refinable - selected_refinements[index];
            if (remaining > best_remaining) {
                best = index;
                best_remaining = remaining;
            }
        }
        if (best_remaining == 0)
            break;
        ++selected_refinements[best];
        ++selected_total;
    }

    std::array<std::uint32_t, 2> selection_starts{};
    std::array<std::uint32_t, 2> selection_counts{};
    std::uint32_t selected = 0;
    for (std::uint8_t range_index = 0; range_index < active_ranges; ++range_index) {
        const auto& range = transport.ranges[range_index];
        const auto frames = transport.is_playing ? range.frame_count : 1u;
        selection_starts[range_index] = selected;
        const auto selection_count =
            topologies[range_index].mandatory + selected_refinements[range_index];
        selection_counts[range_index] = populate_range_selection(
            program, range, frames, topologies[range_index].refinable,
            selected_refinements[range_index], output.subspan(selected, selection_count));
        selected += selection_counts[range_index];
    }

    std::uint32_t emitted = 0;
    for (std::uint8_t range_index = 0; range_index < active_ranges; ++range_index) {
        const auto& range = transport.ranges[range_index];
        timebase::TempoCursor tempo(program.tempo_map());
        EvaluatedPoint previous;
        std::size_t segment_index = 0;
        bool have_previous = false;
        const auto selection_end = selection_starts[range_index] + selection_counts[range_index];
        for (auto selection = selection_starts[range_index]; selection < selection_end;
             ++selection) {
            const auto selected_point = output[selection];
            const auto frame = selected_point.sample_offset - range.sample_offset;
            const auto sample = add_saturating(range.timeline_sample_start, frame);
            const auto point = evaluate(program, sample, tempo,
                                        selection == selection_starts[range_index], segment_index);
            if (!have_previous) {
                output[emitted++] = {selected_point.sample_offset, point.value,
                                     AutomationTransition::Seed};
                previous = point;
                have_previous = true;
                continue;
            }
            const auto transition =
                transition_between(program.segments(), previous.segment_index, point.segment_index);
            const bool unchanged = std::bit_cast<std::uint32_t>(previous.value) ==
                                   std::bit_cast<std::uint32_t>(point.value);
            const bool mandatory = selected_point.transition == AutomationTransition::Immediate;
            if (unchanged && !mandatory)
                continue;
            output[emitted++] = {selected_point.sample_offset, point.value, transition};
            previous = point;
        }
    }

    active_key_ = candidate;
    active_instance_token_ = candidate_instance_token;
    has_block_index_ = true;
    last_block_index_ = transport.block_index;
    result.adoption = adoption;
    result.emitted_events = emitted;
    result.code = selected_refinement_total < refinable_total ? AutomationCursorCode::Coalesced
                                                              : AutomationCursorCode::Ok;
    return result;
}

} // namespace pulp::playback
