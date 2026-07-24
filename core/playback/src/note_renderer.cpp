#include <pulp/playback/note_renderer.hpp>

#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>

namespace pulp::playback {
namespace {

constexpr std::size_t key_index(std::uint8_t channel, std::uint8_t pitch) noexcept {
    return static_cast<std::size_t>(channel) * 128u + pitch;
}

constexpr std::uint8_t midi1_velocity(std::uint16_t velocity) noexcept {
    const auto scaled = (static_cast<std::uint32_t>(velocity) * 127u + 32767u) / 65535u;
    // MIDI 1.0 defines note-on velocity zero as note-off. Keep a model note-on
    // semantically a note-on even when its 16-bit authored velocity is zero.
    return static_cast<std::uint8_t>(std::max<std::uint32_t>(1u, scaled));
}

} // namespace

bool ArrangementNoteRenderer::prepare(std::size_t maximum_events_per_block) {
    if (maximum_events_per_block == 0)
        return false;
    output_.reserve(maximum_events_per_block);
    output_.set_realtime_capacity_limit(true);
    ump_output_.reserve(maximum_events_per_block);
    ump_output_.set_realtime_capacity_limit(true);
    prepared_ = true;
    pending_flush_ = false;
    state_overflow_ = false;
    has_block_index_ = false;
    active_counts_.fill(0);
    return true;
}

void ArrangementNoteRenderer::record_output_drop() noexcept {
    if (dropped_events_ != std::numeric_limits<std::uint32_t>::max())
        ++dropped_events_;
}

bool ArrangementNoteRenderer::append_physical(midi::MidiEvent midi1, midi::UmpPacket midi2,
                                              std::uint32_t sample_offset) noexcept {
    // The primary MIDI-1 stream is the compatibility lane. The attached UMP
    // sidecar is the primary stream for MIDI-2-aware processors. A physical
    // event must appear in both or neither: preflight both bounded buffers
    // before mutating either one, then append without any allocating branch.
    if (output_.size() >= output_.event_capacity() ||
        ump_output_.size() >= ump_output_.capacity()) {
        record_output_drop();
        return false;
    }
    const auto signed_offset = static_cast<std::int32_t>(sample_offset);
    midi1.sample_offset = signed_offset;
    const bool midi1_added = output_.add(std::move(midi1));
    const bool midi2_added = ump_output_.add({std::move(midi2), signed_offset});
    if (!midi1_added || !midi2_added) {
        // The capacity preflight and single-thread ownership make this
        // unreachable, but keep the result fail-closed if those contracts are
        // ever changed underneath the renderer.
        record_output_drop();
        return false;
    }
    return true;
}

bool ArrangementNoteRenderer::has_active_notes() const noexcept {
    return std::any_of(active_counts_.begin(), active_counts_.end(),
                       [](std::uint16_t count) { return count != 0; });
}

void ArrangementNoteRenderer::reset() noexcept {
    shell_.reset();
    output_.clear();
    ump_output_.clear();
    active_counts_.fill(0);
    pending_flush_ = false;
    state_overflow_ = false;
    has_block_index_ = false;
    last_block_index_ = 0;
    dropped_events_ = 0;
}

bool ArrangementNoteRenderer::emit(const NoteProgramEvent& event,
                                   std::uint32_t sample_offset) noexcept {
    const auto index = key_index(event.channel, event.pitch);
    auto& active = active_counts_[index];
    if (event.kind == NoteProgramEventKind::Off) {
        // A seek may enter after the matching onset; such notes are not chased.
        if (active == 0)
            return true;
        if (active > 1) {
            --active;
            return true;
        }
    } else if (active != 0) {
        if (active == maximum_logical_overlap) {
            state_overflow_ = true;
            pending_flush_ = true;
            (void)flush(sample_offset);
            return false;
        }
        ++active;
        return true;
    }
    auto midi1 =
        event.kind == NoteProgramEventKind::Off
            ? midi::MidiEvent::note_off(event.channel, event.pitch)
            : midi::MidiEvent::note_on(event.channel, event.pitch, midi1_velocity(event.velocity));
    auto midi2 = event.kind == NoteProgramEventKind::Off
                     ? midi::UmpPacket::note_off_2(0, event.channel, event.pitch)
                     : midi::UmpPacket::note_on_2(0, event.channel, event.pitch, event.velocity);
    if (!append_physical(std::move(midi1), std::move(midi2), sample_offset))
        return false;
    if (event.kind == NoteProgramEventKind::Off)
        active = 0;
    else
        active = 1;
    return true;
}

bool ArrangementNoteRenderer::flush(std::uint32_t sample_offset) noexcept {
    for (std::size_t index = 0; index < active_counts_.size(); ++index) {
        if (active_counts_[index] == 0)
            continue;
        const auto channel = static_cast<std::uint8_t>(index / 128u);
        const auto pitch = static_cast<std::uint8_t>(index % 128u);
        auto midi1 = midi::MidiEvent::note_off(channel, pitch);
        auto midi2 = midi::UmpPacket::note_off_2(0, channel, pitch);
        if (!append_physical(std::move(midi1), std::move(midi2), sample_offset)) {
            pending_flush_ = true;
            return false;
        }
        active_counts_[index] = 0;
    }
    pending_flush_ = false;
    return true;
}

void ArrangementNoteRenderer::update_carry_state(const TransportSnapshot& transport,
                                                 std::int64_t event_cursor) noexcept {
    auto state = shell_.state_snapshot();
    if (!state.valid)
        return;
    state.event_cursor = event_cursor;
    if (transport.range_count != 0)
        state.timeline_tick = transport.ranges[transport.range_count - 1].timeline_tick_end;
    (void)shell_.end_block(state);
}

NoteRenderResult ArrangementNoteRenderer::process(const PlaybackProgramBlock& block,
                                                  const TransportSnapshot& transport) noexcept {
    runtime::ScopedNoAlloc no_alloc;
    output_.clear();
    ump_output_.clear();
    dropped_events_ = 0;
    if (!prepared_)
        return {NoteRenderCode::NotPrepared};
    state_overflow_ = false;

    if (!valid_transport_ranges(transport)) {
        (void)flush(0);
        return {dropped_events_ == 0 ? NoteRenderCode::InvalidTransport
                                     : NoteRenderCode::OutputOverflow,
                ShellAdoptionResult::Missing, static_cast<std::uint32_t>(output_.size()),
                dropped_events_};
    }
    if (block && transport.tempo_map != &block.program()->tempo_map()) {
        (void)flush(0);
        return {dropped_events_ == 0 ? NoteRenderCode::TempoMapMismatch
                                     : NoteRenderCode::OutputOverflow,
                ShellAdoptionResult::Rejected, static_cast<std::uint32_t>(output_.size()),
                dropped_events_};
    }

    const auto view = shell_.begin_block(block);
    NoteRenderResult result;
    result.adoption = view.adoption;
    if (!view.program) {
        (void)flush(0);
        result.code = view.adoption == ShellAdoptionResult::Rejected
                          ? NoteRenderCode::AdoptionRejected
                          : NoteRenderCode::MissingProgram;
        result.emitted_events = static_cast<std::uint32_t>(output_.size());
        result.dropped_events = dropped_events_;
        if (result.dropped_events != 0)
            result.code = NoteRenderCode::OutputOverflow;
        return result;
    }

    const bool block_sequence_reset =
        has_block_index_ && transport.block_index != last_block_index_ + 1;
    last_block_index_ = transport.block_index;
    has_block_index_ = true;
    if (pending_flush_ || view.adoption == ShellAdoptionResult::Adopted || block_sequence_reset) {
        if (!flush(0)) {
            result.code = NoteRenderCode::OutputOverflow;
            result.emitted_events = static_cast<std::uint32_t>(output_.size());
            result.dropped_events = dropped_events_;
            update_carry_state(transport, 0);
            return result;
        }
    }

    if (!transport.is_playing) {
        if (!flush(0))
            result.code = NoteRenderCode::OutputOverflow;
        result.emitted_events = static_cast<std::uint32_t>(output_.size());
        result.dropped_events = dropped_events_;
        update_carry_state(transport, 0);
        return result;
    }

    const auto events = view.program->arrangement_note_events();
    std::int64_t last_cursor = 0;
    for (std::uint8_t range_index = 0; range_index < transport.range_count; ++range_index) {
        const auto& range = transport.ranges[range_index];
        if (range.discontinuity && !flush(range.sample_offset))
            break;

        const auto search_sample =
            range.host_beat_mapping
                ? transport.tempo_map->ticks_to_samples(range.timeline_tick_start)
                : range.timeline_sample_start;
        const auto mapped_end_sample =
            range.host_beat_mapping ? transport.tempo_map->ticks_to_samples(range.timeline_tick_end)
                                    : range.timeline_sample_start;
        auto cursor =
            std::lower_bound(events.begin(), events.end(), search_sample,
                             [](const NoteProgramEvent& event, timebase::SamplePosition sample) {
                                 return event.sample < sample;
                             });
        last_cursor = static_cast<std::int64_t>(cursor - events.begin());
        for (; cursor != events.end(); ++cursor) {
            std::uint32_t local_offset = 0;
            if (range.host_beat_mapping) {
                if (cursor->sample > mapped_end_sample)
                    break;
                if (!host_mapped_output_offset_for_tick(range, cursor->tick, local_offset))
                    continue;
            } else {
                if (!detail::note_event_offset_in_range(cursor->sample, range.timeline_sample_start,
                                                        range.frame_count, local_offset)) {
                    if (cursor->sample >= range.timeline_sample_start)
                        break;
                    continue;
                }
            }
            const auto block_offset = range.sample_offset + local_offset;
            if (!emit(*cursor, block_offset)) {
                if (!state_overflow_)
                    pending_flush_ = true;
                break;
            }
            last_cursor = static_cast<std::int64_t>((cursor - events.begin()) + 1);
        }
        if (pending_flush_)
            break;
    }

    update_carry_state(transport, last_cursor);
    result.emitted_events = static_cast<std::uint32_t>(output_.size());
    result.dropped_events = dropped_events_;
    if (state_overflow_)
        result.code = NoteRenderCode::ActiveStateOverflow;
    else if (pending_flush_ || result.dropped_events != 0)
        result.code = NoteRenderCode::OutputOverflow;
    return result;
}

} // namespace pulp::playback
