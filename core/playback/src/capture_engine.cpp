#include <pulp/playback/capture_engine.hpp>

#include <pulp/runtime/exceptions.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::playback {
namespace {

std::int64_t ceil_multiple(std::int64_t value, std::int64_t interval) noexcept {
    const auto remainder = value % interval;
    if (remainder == 0)
        return value;
    if (value >= 0) {
        if (value > std::numeric_limits<std::int64_t>::max() - (interval - remainder))
            return std::numeric_limits<std::int64_t>::max();
        return value + interval - remainder;
    }
    return value - remainder;
}

bool add_storage_bytes(std::uint64_t count, std::uint64_t element_size, std::uint64_t maximum,
                       std::uint64_t& total) noexcept {
    if (element_size != 0 && count > (maximum - total) / element_size)
        return false;
    total += count * element_size;
    return true;
}

} // namespace

bool CaptureEngine::valid_config(const CaptureEngineConfig& config) noexcept {
    if (!config.sample_rate.valid() || config.maximum_block_size == 0 ||
        config.maximum_take_frames == 0 || config.take_slots_per_track == 0 ||
        config.take_slots_per_track > kMaximumTakeSlotsPerTrack ||
        config.maximum_preallocated_bytes == 0 || config.tracks.empty() ||
        config.tracks.size() > kMaximumTracks)
        return false;
    if (config.maximum_take_frames >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return false;
    if (config.take_slots_per_track >
        std::numeric_limits<std::uint32_t>::max() / config.tracks.size())
        return false;
    const auto slot_count =
        config.tracks.size() * static_cast<std::size_t>(config.take_slots_per_track);
    if (slot_count > std::vector<TakeSlot>{}.max_size() ||
        config.tracks.size() > std::vector<TrackRuntime>{}.max_size() ||
        config.midi_events_per_take > std::vector<CapturedMidiEvent>{}.max_size())
        return false;
    std::uint64_t storage_bytes = 0;
    if (!add_storage_bytes(config.tracks.size(), sizeof(TrackRuntime),
                           config.maximum_preallocated_bytes, storage_bytes) ||
        !add_storage_bytes(slot_count, sizeof(TakeSlot), config.maximum_preallocated_bytes,
                           storage_bytes))
        return false;
    for (const auto& track : config.tracks) {
        if (!track.track_id.valid() || !track.take_lane_id.valid() || track.channel_count == 0)
            return false;
        if (config.maximum_take_frames >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) /
                track.channel_count)
            return false;
        const auto samples_per_slot =
            config.maximum_take_frames * static_cast<std::uint64_t>(track.channel_count);
        if (samples_per_slot > std::vector<float>{}.max_size())
            return false;
        const auto track_slot_count = static_cast<std::uint64_t>(config.take_slots_per_track);
        if (samples_per_slot > std::numeric_limits<std::uint64_t>::max() / track_slot_count)
            return false;
        if (!add_storage_bytes(samples_per_slot * track_slot_count, sizeof(float),
                               config.maximum_preallocated_bytes, storage_bytes) ||
            !add_storage_bytes(
                static_cast<std::uint64_t>(config.midi_events_per_take) * track_slot_count,
                sizeof(CapturedMidiEvent), config.maximum_preallocated_bytes, storage_bytes))
            return false;
    }
    return true;
}

bool CaptureEngine::valid_session(const CaptureSession& session) noexcept {
    if (session.punch_in < session.count_in_start ||
        (session.has_punch_out && session.punch_out <= session.punch_in))
        return false;
    if (session.loop_enabled &&
        (session.loop_end <= session.loop_start || session.punch_in < session.loop_start ||
         (session.has_punch_out && session.punch_out > session.loop_end)))
        return false;
    return !session.metronome_enabled ||
           (session.metronome_interval.value > 0 && std::isfinite(session.metronome_level));
}

timebase::SamplePosition CaptureEngine::add_frames(timebase::SamplePosition start,
                                                   std::uint32_t frames) noexcept {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto signed_frames = static_cast<std::int64_t>(frames);
    if (start.value > maximum - signed_frames)
        return {maximum};
    return {start.value + signed_frames};
}

bool CaptureEngine::prepare(const CaptureEngineConfig& config) {
    release();
    if (!valid_config(config))
        return false;

    CaptureEngineConfig prepared_config;
    std::vector<TrackRuntime> track_runtime;
    std::vector<TakeSlot> slots;
    const auto slot_count =
        config.tracks.size() * static_cast<std::size_t>(config.take_slots_per_track);
    PULP_TRY {
        prepared_config = config;
        prepared_config.sample_rate = config.sample_rate.normalized();
        track_runtime.resize(config.tracks.size());
        slots.resize(slot_count);
        for (std::size_t track_index = 0; track_index < config.tracks.size(); ++track_index) {
            const auto& track = config.tracks[track_index];
            for (std::uint32_t local_slot = 0; local_slot < config.take_slots_per_track;
                 ++local_slot) {
                auto& slot = slots[track_index * config.take_slots_per_track + local_slot];
                slot.track_index = static_cast<std::uint32_t>(track_index);
                slot.audio.resize(static_cast<std::size_t>(config.maximum_take_frames) *
                                  track.channel_count);
                slot.midi.reserve(config.midi_events_per_take);
            }
        }
    }
    PULP_CATCH_ALL {
        return false;
    }
    config_ = std::move(prepared_config);
    track_runtime_ = std::move(track_runtime);
    slots_ = std::move(slots);
    for (std::size_t track_index = 0; track_index < config_.tracks.size(); ++track_index)
        input_peaks_[track_index].store(0.0f, std::memory_order_relaxed);
    prepared_ = true;
    return true;
}

void CaptureEngine::release() noexcept {
    CaptureCommand command;
    while (commands_.try_pop(command)) {
    }
    CaptureEvent event;
    while (events_.try_pop(event)) {
    }
    commands_.reset_overflow_count();
    events_.reset_overflow_count();
    config_ = {};
    track_runtime_.clear();
    slots_.clear();
    session_ = {};
    active_sequence_ = 0;
    expected_timeline_sample_ = {};
    has_expected_timeline_sample_ = false;
    recording_ = false;
    prepared_ = false;
    recording_snapshot_.store(false, std::memory_order_release);
    completed_takes_.store(0, std::memory_order_relaxed);
    dropped_commands_.store(0, std::memory_order_relaxed);
    dropped_events_.store(0, std::memory_order_relaxed);
    capacity_failures_.store(0, std::memory_order_relaxed);
}

bool CaptureEngine::enqueue_command(const CaptureCommand& command) noexcept {
    if (!commands_.try_push(command)) {
        dropped_commands_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool CaptureEngine::pop_event(CaptureEvent& event) noexcept {
    return events_.try_pop(event);
}

void CaptureEngine::push_event(const CaptureEvent& event) noexcept {
    if (!events_.try_push(event))
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
}

void CaptureEngine::push_session_event(CaptureEventType type, std::uint64_t sequence) noexcept {
    CaptureEvent event;
    event.type = type;
    event.sequence = sequence;
    push_event(event);
}

void CaptureEngine::apply_commands() noexcept {
    CaptureCommand command;
    while (commands_.try_pop(command))
        apply_command(command);
}

void CaptureEngine::apply_command(const CaptureCommand& command) noexcept {
    switch (command.type) {
    case CaptureCommandType::Start:
        start_session(command);
        break;
    case CaptureCommandType::Stop:
        stop_session(command, false);
        break;
    case CaptureCommandType::Cancel:
        stop_session(command, true);
        break;
    case CaptureCommandType::ReleaseTake:
        release_take(command.take);
        break;
    }
}

void CaptureEngine::start_session(const CaptureCommand& command) noexcept {
    if (recording_ || !valid_session(command.session)) {
        push_session_event(CaptureEventType::CommandRejected, command.sequence);
        return;
    }
    for (auto& track : track_runtime_) {
        track.active_slot = -1;
        track.capture_disabled = false;
    }
    session_ = command.session;
    active_sequence_ = command.sequence;
    expected_timeline_sample_ = {};
    has_expected_timeline_sample_ = false;
    recording_ = true;
    recording_snapshot_.store(true, std::memory_order_release);
    push_session_event(CaptureEventType::Started, command.sequence);
}

void CaptureEngine::stop_session(const CaptureCommand& command, bool cancelled) noexcept {
    if (!recording_ || (command.sequence != 0 && command.sequence != active_sequence_)) {
        push_session_event(CaptureEventType::CommandRejected, command.sequence);
        return;
    }
    if (cancelled)
        cancel_active_takes();
    else
        finalize_active_takes();
    const auto sequence = active_sequence_;
    recording_ = false;
    active_sequence_ = 0;
    has_expected_timeline_sample_ = false;
    recording_snapshot_.store(false, std::memory_order_release);
    push_session_event(cancelled ? CaptureEventType::Cancelled : CaptureEventType::Stopped,
                       sequence);
}

void CaptureEngine::release_take(CaptureTakeHandle take) noexcept {
    if (!valid_handle(take) || slots_[take.slot].state != SlotState::Completed) {
        push_session_event(CaptureEventType::CommandRejected, 0);
        return;
    }
    auto& slot = slots_[take.slot];
    const auto track_index = slot.track_index;
    slot.state = SlotState::Free;
    slot.frame_count = 0;
    slot.midi_event_count = 0;
    slot.midi.clear();
    if (recording_ && track_index < track_runtime_.size())
        track_runtime_[track_index].capture_disabled = false;
}

void CaptureEngine::monitor_inputs(const audio::BufferView<const float>& input,
                                   audio::BufferView<float>& output,
                                   std::uint32_t frames) noexcept {
    for (std::size_t track_index = 0; track_index < config_.tracks.size(); ++track_index) {
        const auto& track = config_.tracks[track_index];
        float peak = 0.0f;
        for (std::uint32_t channel = 0; channel < track.channel_count; ++channel) {
            const auto source =
                input.channel(static_cast<std::size_t>(track.input_channel) + channel);
            for (std::uint32_t frame = 0; frame < frames; ++frame)
                peak = std::max(peak, std::abs(source[frame]));
            if (!track.monitor)
                continue;
            auto destination =
                output.channel(static_cast<std::size_t>(track.output_channel) + channel);
            for (std::uint32_t frame = 0; frame < frames; ++frame)
                destination[frame] += source[frame];
        }
        input_peaks_[track_index].store(peak, std::memory_order_release);
    }
}

void CaptureEngine::render_metronome(audio::BufferView<float>& output,
                                     const TransportSnapshot& transport) noexcept {
    if (!recording_ || !session_.metronome_enabled || !transport.is_playing ||
        transport.tempo_map == nullptr ||
        session_.metronome_output_channel >= output.num_channels())
        return;
    const auto interval = session_.metronome_interval.value;
    for (std::uint8_t range_index = 0; range_index < transport.range_count; ++range_index) {
        const auto& range = transport.ranges[range_index];
        auto tick = ceil_multiple(range.timeline_tick_start.value, interval);
        while (tick < range.timeline_tick_end.value) {
            const auto sample = transport.tempo_map->ticks_to_samples({tick});
            if (sample >= session_.count_in_start &&
                (!session_.has_punch_out || sample < session_.punch_out) &&
                sample >= range.timeline_sample_start) {
                const auto delta = static_cast<std::uint64_t>(sample.value) -
                                   static_cast<std::uint64_t>(range.timeline_sample_start.value);
                if (delta < range.frame_count) {
                    output.channel(
                        session_.metronome_output_channel)[range.sample_offset +
                                                           static_cast<std::uint32_t>(delta)] +=
                        session_.metronome_level;
                }
            }
            if (tick > std::numeric_limits<std::int64_t>::max() - interval)
                break;
            tick += interval;
        }
    }
}

void CaptureEngine::handle_range_boundary(const TransportRange& range) noexcept {
    if (!has_expected_timeline_sample_)
        return;
    if (range.timeline_sample_start == expected_timeline_sample_)
        return;
    if (session_.loop_enabled && expected_timeline_sample_ == session_.loop_end &&
        range.timeline_sample_start == session_.loop_start) {
        finalize_active_takes();
    } else {
        cancel_active_takes();
    }
}

CaptureEngine::TakeSlot*
CaptureEngine::begin_take(std::size_t track_index,
                          timebase::SamplePosition placement_start) noexcept {
    const auto first = track_index * config_.take_slots_per_track;
    for (std::uint32_t local_slot = 0; local_slot < config_.take_slots_per_track; ++local_slot) {
        auto& slot = slots_[first + local_slot];
        if (slot.state != SlotState::Free)
            continue;
        ++slot.generation;
        if (slot.generation == 0)
            ++slot.generation;
        slot.state = SlotState::Recording;
        slot.placement_start = placement_start;
        slot.frame_count = 0;
        slot.midi_event_count = 0;
        slot.audio_capacity_reported = false;
        slot.midi_capacity_reported = false;
        slot.midi.clear();
        track_runtime_[track_index].active_slot = static_cast<std::int32_t>(first + local_slot);
        return &slot;
    }
    track_runtime_[track_index].capture_disabled = true;
    capacity_failures_.fetch_add(1, std::memory_order_relaxed);
    CaptureEvent event;
    event.type = CaptureEventType::NoFreeTakeSlot;
    event.sequence = active_sequence_;
    event.track_id = config_.tracks[track_index].track_id;
    event.take_lane_id = config_.tracks[track_index].take_lane_id;
    push_event(event);
    return nullptr;
}

void CaptureEngine::append_audio(TakeSlot& slot, const CaptureTrackConfig& track,
                                 const audio::BufferView<const float>& input,
                                 std::uint32_t source_offset, std::uint32_t frames) noexcept {
    const auto available = config_.maximum_take_frames - slot.frame_count;
    const auto copied = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, available));
    for (std::uint32_t channel = 0; channel < track.channel_count; ++channel) {
        const auto source = input.channel(static_cast<std::size_t>(track.input_channel) + channel);
        auto* destination = slot.audio.data() +
                            static_cast<std::size_t>(channel) *
                                static_cast<std::size_t>(config_.maximum_take_frames) +
                            static_cast<std::size_t>(slot.frame_count);
        std::copy_n(source.data() + source_offset, copied, destination);
    }
    slot.frame_count += copied;
    if (copied != frames && !slot.audio_capacity_reported) {
        slot.audio_capacity_reported = true;
        capacity_failures_.fetch_add(1, std::memory_order_relaxed);
        CaptureEvent event;
        event.type = CaptureEventType::TakeCapacityExceeded;
        event.sequence = active_sequence_;
        event.track_id = track.track_id;
        event.take_lane_id = track.take_lane_id;
        push_event(event);
    }
}

void CaptureEngine::append_midi(TakeSlot& slot, const midi::MidiBuffer& input,
                                std::uint32_t source_offset, std::uint32_t frames) noexcept {
    const auto end = source_offset + frames;
    const auto base = slot.frame_count - frames;
    for (const auto& event : input) {
        if (event.sample_offset < 0 ||
            static_cast<std::uint32_t>(event.sample_offset) < source_offset ||
            static_cast<std::uint32_t>(event.sample_offset) >= end)
            continue;
        if (slot.midi.size() >= config_.midi_events_per_take) {
            if (!slot.midi_capacity_reported) {
                slot.midi_capacity_reported = true;
                capacity_failures_.fetch_add(1, std::memory_order_relaxed);
                CaptureEvent overflow;
                overflow.type = CaptureEventType::MidiCapacityExceeded;
                overflow.sequence = active_sequence_;
                overflow.track_id = config_.tracks[slot.track_index].track_id;
                overflow.take_lane_id = config_.tracks[slot.track_index].take_lane_id;
                push_event(overflow);
            }
            continue;
        }
        auto captured = event;
        captured.sample_offset = 0;
        slot.midi.push_back(
            {captured, base + static_cast<std::uint32_t>(event.sample_offset) - source_offset});
    }
    slot.midi_event_count = static_cast<std::uint32_t>(slot.midi.size());
}

void CaptureEngine::process_range(const audio::BufferView<const float>& input,
                                  const midi::MidiBuffer& midi_input,
                                  const TransportRange& range) noexcept {
    handle_range_boundary(range);
    const auto range_end = add_frames(range.timeline_sample_start, range.frame_count);
    auto capture_start = std::max(range.timeline_sample_start, session_.punch_in);
    auto capture_end = range_end;
    if (session_.has_punch_out)
        capture_end = std::min(capture_end, session_.punch_out);
    if (capture_end > capture_start) {
        const auto delta = static_cast<std::uint64_t>(capture_start.value) -
                           static_cast<std::uint64_t>(range.timeline_sample_start.value);
        const auto source_offset = range.sample_offset + static_cast<std::uint32_t>(delta);
        const auto frames =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(capture_end.value) -
                                       static_cast<std::uint64_t>(capture_start.value));
        for (std::size_t track_index = 0; track_index < config_.tracks.size(); ++track_index) {
            const auto& track = config_.tracks[track_index];
            auto& runtime = track_runtime_[track_index];
            if (!track.armed || runtime.capture_disabled)
                continue;
            TakeSlot* slot = nullptr;
            if (runtime.active_slot >= 0)
                slot = &slots_[static_cast<std::size_t>(runtime.active_slot)];
            else
                slot = begin_take(track_index, capture_start);
            if (slot == nullptr)
                continue;
            const auto before = slot->frame_count;
            append_audio(*slot, track, input, source_offset, frames);
            const auto copied = static_cast<std::uint32_t>(slot->frame_count - before);
            if (track.capture_midi && copied != 0)
                append_midi(*slot, midi_input, source_offset, copied);
        }
    }
    expected_timeline_sample_ = range_end;
    has_expected_timeline_sample_ = true;
    if (session_.has_punch_out && range_end >= session_.punch_out)
        finalize_active_takes();
}

void CaptureEngine::finalize_active_takes() noexcept {
    for (std::size_t track_index = 0; track_index < track_runtime_.size(); ++track_index) {
        auto& runtime = track_runtime_[track_index];
        if (runtime.active_slot < 0)
            continue;
        auto& slot = slots_[static_cast<std::size_t>(runtime.active_slot)];
        runtime.active_slot = -1;
        if (slot.frame_count == 0) {
            slot.state = SlotState::Free;
            continue;
        }
        slot.state = SlotState::Completed;
        CaptureEvent event;
        event.type = CaptureEventType::TakeCompleted;
        event.sequence = active_sequence_;
        event.track_id = config_.tracks[track_index].track_id;
        event.take_lane_id = config_.tracks[track_index].take_lane_id;
        event.take = {static_cast<std::uint32_t>(&slot - slots_.data()), slot.generation};
        event.placement_start = slot.placement_start;
        event.frame_count = slot.frame_count;
        event.channel_count = config_.tracks[track_index].channel_count;
        event.midi_event_count = slot.midi_event_count;
        if (events_.try_push(event)) {
            completed_takes_.fetch_add(1, std::memory_order_relaxed);
        } else {
            dropped_events_.fetch_add(1, std::memory_order_relaxed);
            slot.state = SlotState::Free;
            slot.frame_count = 0;
            slot.midi.clear();
        }
    }
}

void CaptureEngine::cancel_active_takes() noexcept {
    for (std::size_t track_index = 0; track_index < track_runtime_.size(); ++track_index) {
        auto& runtime = track_runtime_[track_index];
        if (runtime.active_slot < 0)
            continue;
        auto& slot = slots_[static_cast<std::size_t>(runtime.active_slot)];
        if (slot.frame_count != 0) {
            CaptureEvent event;
            event.type = CaptureEventType::TakeCancelled;
            event.sequence = active_sequence_;
            event.track_id = config_.tracks[track_index].track_id;
            event.take_lane_id = config_.tracks[track_index].take_lane_id;
            event.frame_count = slot.frame_count;
            push_event(event);
        }
        slot.state = SlotState::Free;
        slot.frame_count = 0;
        slot.midi_event_count = 0;
        slot.midi.clear();
        runtime.active_slot = -1;
    }
}

CaptureProcessResult CaptureEngine::process(const audio::BufferView<const float>& input,
                                            audio::BufferView<float>& monitor_output,
                                            const midi::MidiBuffer& midi_input,
                                            const TransportSnapshot& transport) noexcept {
    if (!prepared_)
        return CaptureProcessResult::NotPrepared;
    if (transport.frame_count == 0 || transport.frame_count > config_.maximum_block_size ||
        input.num_samples() < transport.frame_count ||
        monitor_output.num_samples() < transport.frame_count)
        return CaptureProcessResult::InvalidBuffers;
    for (const auto& track : config_.tracks) {
        const auto input_channel = static_cast<std::size_t>(track.input_channel);
        const auto output_channel = static_cast<std::size_t>(track.output_channel);
        if (input_channel > input.num_channels() ||
            track.channel_count > input.num_channels() - input_channel ||
            output_channel > monitor_output.num_channels() ||
            track.channel_count > monitor_output.num_channels() - output_channel)
            return CaptureProcessResult::InvalidBuffers;
    }
    if (!valid_transport_ranges(transport) ||
        transport.sample_rate.normalized() != config_.sample_rate)
        return CaptureProcessResult::InvalidTransport;

    apply_commands();
    monitor_inputs(input, monitor_output, transport.frame_count);
    render_metronome(monitor_output, transport);
    if (!recording_ || !transport.is_playing)
        return CaptureProcessResult::Ok;
    for (std::uint8_t range_index = 0; range_index < transport.range_count; ++range_index)
        process_range(input, midi_input, transport.ranges[range_index]);
    return CaptureProcessResult::Ok;
}

bool CaptureEngine::valid_handle(CaptureTakeHandle take) const noexcept {
    return take.generation != 0 && take.slot < slots_.size() &&
           slots_[take.slot].generation == take.generation;
}

bool CaptureEngine::copy_audio(CaptureTakeHandle take,
                               audio::BufferView<float> destination) const noexcept {
    if (!valid_handle(take))
        return false;
    const auto& slot = slots_[take.slot];
    if (slot.state != SlotState::Completed)
        return false;
    const auto channels = config_.tracks[slot.track_index].channel_count;
    if (destination.num_channels() < channels || destination.num_samples() < slot.frame_count)
        return false;
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        const auto* source =
            slot.audio.data() + static_cast<std::size_t>(channel) *
                                    static_cast<std::size_t>(config_.maximum_take_frames);
        std::copy_n(source, static_cast<std::size_t>(slot.frame_count),
                    destination.channel(channel).data());
    }
    return true;
}

bool CaptureEngine::copy_midi(CaptureTakeHandle take, std::span<CapturedMidiEvent> destination,
                              std::size_t& copied) const noexcept {
    copied = 0;
    if (!valid_handle(take))
        return false;
    const auto& slot = slots_[take.slot];
    if (slot.state != SlotState::Completed || destination.size() < slot.midi.size())
        return false;
    std::copy(slot.midi.begin(), slot.midi.end(), destination.begin());
    copied = slot.midi.size();
    return true;
}

float CaptureEngine::input_peak(std::size_t track_index) const noexcept {
    if (track_index >= config_.tracks.size())
        return 0.0f;
    return input_peaks_[track_index].load(std::memory_order_acquire);
}

CaptureEngineStats CaptureEngine::stats() const noexcept {
    return {
        completed_takes_.load(std::memory_order_relaxed),
        dropped_commands_.load(std::memory_order_relaxed),
        dropped_events_.load(std::memory_order_relaxed),
        capacity_failures_.load(std::memory_order_relaxed),
    };
}

bool CaptureEngine::recording() const noexcept {
    return recording_snapshot_.load(std::memory_order_acquire);
}

} // namespace pulp::playback
