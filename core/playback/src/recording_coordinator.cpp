#include <pulp/playback/recording_coordinator.hpp>

#include <algorithm>
#include <limits>

namespace pulp::playback {
namespace {

runtime::Result<CommittedRecordingTake, RecordingCoordinatorError>
failure(RecordingCoordinatorError error) {
    return runtime::Result<CommittedRecordingTake, RecordingCoordinatorError>(
        runtime::Err(error));
}

RecordingMonitorPath resolve_monitoring(const RecordingTrackConfig& track,
                                        const audio::LatencySnapshot& latency) noexcept {
    switch (track.monitoring) {
    case RecordingMonitorMode::Off:
        return RecordingMonitorPath::Off;
    case RecordingMonitorMode::Direct:
        return RecordingMonitorPath::Direct;
    case RecordingMonitorMode::Software:
        return RecordingMonitorPath::Software;
    case RecordingMonitorMode::Auto:
        if (track.direct_monitoring_available)
            return RecordingMonitorPath::Direct;
        return latency.monitoring_latency_frames ? RecordingMonitorPath::Software
                                                 : RecordingMonitorPath::Off;
    }
    return RecordingMonitorPath::Off;
}

timebase::SamplePosition subtract_frames(timebase::SamplePosition position,
                                         std::uint64_t frames) noexcept {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    if (frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return {minimum};
    const auto signed_frames = static_cast<std::int64_t>(frames);
    if (position.value < minimum + signed_frames)
        return {minimum};
    return {position.value - signed_frames};
}

bool same_completion(const CaptureEvent& left, const CaptureEvent& right) noexcept {
    return left.type == right.type && left.sequence == right.sequence &&
           left.track_id == right.track_id && left.take_lane_id == right.take_lane_id &&
           left.take == right.take && left.placement_start == right.placement_start &&
           left.frame_count == right.frame_count && left.channel_count == right.channel_count &&
           left.midi_event_count == right.midi_event_count;
}

timeline::Transaction make_transaction(timeline::WriterToken& writer,
                                       timeline::DocumentRevision revision,
                                       std::vector<timeline::Command> commands) {
    timeline::Transaction transaction;
    transaction.id = writer.allocate_transaction_id();
    transaction.expected_revision = revision;
    transaction.commands.reserve(commands.size());
    for (auto& command : commands)
        transaction.commands.push_back({writer.allocate_command_id(), std::move(command)});
    return transaction;
}

} // namespace

bool RecordingCoordinator::prepare(const RecordingCoordinatorConfig& config) {
    release();
    if (!config.sample_rate.valid() || config.maximum_block_size == 0 ||
        config.maximum_take_frames == 0 || config.take_slots_per_track == 0 ||
        config.tracks.empty() || !config.latency.input_placement_offset_frames ||
        !config.latency.is_current_for(config.current_audio_timing) ||
        config.latency.sample_rate_hz !=
            static_cast<double>(config.sample_rate.normalized().numerator) /
                static_cast<double>(config.sample_rate.normalized().denominator))
        return false;

    CaptureEngineConfig capture;
    capture.sample_rate = config.sample_rate;
    capture.maximum_block_size = config.maximum_block_size;
    capture.maximum_take_frames = config.maximum_take_frames;
    capture.take_slots_per_track = config.take_slots_per_track;
    capture.midi_events_per_take = config.midi_events_per_take;
    capture.maximum_preallocated_bytes = config.maximum_preallocated_bytes;

    std::vector<RecordingMonitorPath> paths;
    paths.reserve(config.tracks.size());
    capture.tracks.reserve(config.tracks.size());
    std::size_t monitor_channels = 0;
    for (const auto& track : config.tracks) {
        if (!track.track_id.valid() || !track.take_lane_id.valid() || track.source.id.empty() ||
            track.source.channel_count == 0 ||
            (track.monitoring == RecordingMonitorMode::Direct &&
             !track.direct_monitoring_available) ||
            (track.monitoring == RecordingMonitorMode::Software &&
             !config.latency.monitoring_latency_frames))
            return false;
        const auto path = resolve_monitoring(track, config.latency);
        paths.push_back(path);
        monitor_channels = std::max(
            monitor_channels, static_cast<std::size_t>(track.source.output_channel) +
                                  static_cast<std::size_t>(track.source.channel_count));
        capture.tracks.push_back({
            .track_id = track.track_id,
            .take_lane_id = track.take_lane_id,
            .input_channel = track.source.input_channel,
            .output_channel = track.source.output_channel,
            .channel_count = track.source.channel_count,
            .armed = track.armed,
            .monitor = path == RecordingMonitorPath::Software,
            .capture_midi = track.capture_midi,
        });
    }
    if (monitor_channels > std::numeric_limits<std::size_t>::max() /
                               static_cast<std::size_t>(config.maximum_block_size))
        return false;
    const auto monitor_samples = monitor_channels * config.maximum_block_size;
    if (monitor_samples > config.maximum_preallocated_bytes / sizeof(float))
        return false;
    const auto monitor_bytes = monitor_samples * sizeof(float);
    if (monitor_bytes >= config.maximum_preallocated_bytes)
        return false;
    capture.maximum_preallocated_bytes = config.maximum_preallocated_bytes - monitor_bytes;
    audio::Buffer<float> monitor_scratch(monitor_channels, config.maximum_block_size);
    const auto slot_count = config.tracks.size() * config.take_slots_per_track;
    std::vector<CaptureEvent> completed_events(slot_count);
    std::vector<std::uint32_t> committed_generations(slot_count);
    std::vector<std::uint32_t> pending_release_generations(slot_count);
    if (!capture_.prepare(capture))
        return false;
    config_ = config;
    config_.sample_rate = config.sample_rate.normalized();
    monitor_scratch_ = std::move(monitor_scratch);
    monitoring_paths_ = std::move(paths);
    completed_events_ = std::move(completed_events);
    committed_generations_ = std::move(committed_generations);
    pending_release_generations_ = std::move(pending_release_generations);
    prepared_ = true;
    return true;
}

void RecordingCoordinator::release() noexcept {
    capture_.release();
    monitor_scratch_ = {};
    config_ = {};
    monitoring_paths_.clear();
    completed_events_.clear();
    committed_generations_.clear();
    pending_release_generations_.clear();
    prepared_ = false;
}

bool RecordingCoordinator::enqueue_command(const CaptureCommand& command) noexcept {
    flush_pending_releases();
    return prepared_ && capture_.enqueue_command(command);
}

bool RecordingCoordinator::pop_event(CaptureEvent& event) noexcept {
    flush_pending_releases();
    if (!capture_.pop_event(event))
        return false;
    if (event.type == CaptureEventType::TakeCompleted && event.take.generation != 0 &&
        event.take.slot < completed_events_.size())
        completed_events_[event.take.slot] = event;
    return true;
}

CaptureProcessResult RecordingCoordinator::process(
    const audio::BufferView<const float>& input, audio::BufferView<float>& monitor_output,
    const midi::MidiBuffer& midi_input, const TransportSnapshot& transport) noexcept {
    if (!prepared_)
        return CaptureProcessResult::NotPrepared;
    if (transport.frame_count > monitor_scratch_.num_samples())
        return CaptureProcessResult::InvalidBuffers;
    const auto output_channels = std::min(monitor_output.num_channels(),
                                          monitor_scratch_.num_channels());
    if (output_channels != 0 && monitor_output.num_samples() < transport.frame_count)
        return CaptureProcessResult::InvalidBuffers;
    for (std::size_t index = 0; index < config_.tracks.size(); ++index) {
        if (monitoring_paths_[index] != RecordingMonitorPath::Software)
            continue;
        const auto& source = config_.tracks[index].source;
        const auto output_channel = static_cast<std::size_t>(source.output_channel);
        if (monitor_output.num_samples() < transport.frame_count ||
            output_channel > monitor_output.num_channels() ||
            source.channel_count > monitor_output.num_channels() - output_channel)
            return CaptureProcessResult::InvalidBuffers;
    }
    monitor_scratch_.clear();
    auto scratch = monitor_scratch_.view();
    const auto result = capture_.process(input, scratch, midi_input, transport);
    if (result != CaptureProcessResult::Ok)
        return result;
    for (std::size_t channel = 0; channel < output_channels; ++channel) {
        auto source = scratch.channel(channel);
        auto destination = monitor_output.channel(channel);
        for (std::uint32_t frame = 0; frame < transport.frame_count; ++frame)
            destination[frame] += source[frame];
    }
    return CaptureProcessResult::Ok;
}

RecordingMonitorPath RecordingCoordinator::monitoring_path(std::size_t track_index) const noexcept {
    return track_index < monitoring_paths_.size() ? monitoring_paths_[track_index]
                                                   : RecordingMonitorPath::Off;
}

void RecordingCoordinator::flush_pending_releases() noexcept {
    if (!prepared_)
        return;
    for (std::size_t slot = 0; slot < pending_release_generations_.size(); ++slot) {
        const auto generation = pending_release_generations_[slot];
        if (generation == 0)
            continue;
        CaptureCommand command;
        command.type = CaptureCommandType::ReleaseTake;
        command.take = {static_cast<std::uint32_t>(slot), generation};
        if (!capture_.enqueue_command(command))
            return;
        pending_release_generations_[slot] = 0;
    }
}

runtime::Result<CommittedRecordingTake, RecordingCoordinatorError>
RecordingCoordinator::commit_take(const CaptureEvent& completed,
                                  RecordingTakeCommitRequest request,
                                  const audio::AudioIoTiming& current_audio_timing,
                                  RecordingMediaStager& stager,
                                  timeline::DocumentSession& session,
                                  timeline::WriterToken& writer) {
    if (!prepared_)
        return failure(RecordingCoordinatorError::NotPrepared);
    flush_pending_releases();
    if (completed.type != CaptureEventType::TakeCompleted || completed.frame_count == 0 ||
        completed.channel_count == 0 || completed.take.generation == 0 ||
        completed.take.slot >= committed_generations_.size() ||
        !same_completion(completed_events_[completed.take.slot], completed))
        return failure(RecordingCoordinatorError::InvalidTake);
    const auto track_index = static_cast<std::size_t>(completed.take.slot) /
                             static_cast<std::size_t>(config_.take_slots_per_track);
    const auto& track = config_.tracks[track_index];
    if (track.track_id != completed.track_id || track.take_lane_id != completed.take_lane_id ||
        track.source.channel_count != completed.channel_count ||
        request.track_id != completed.track_id || request.take_lane_id != completed.take_lane_id ||
        committed_generations_[completed.take.slot] == completed.take.generation)
        return failure(RecordingCoordinatorError::InvalidTake);
    if (!config_.latency.is_current_for(current_audio_timing))
        return failure(RecordingCoordinatorError::StaleLatency);

    audio::Buffer<float> captured(completed.channel_count, completed.frame_count);
    if (!capture_.copy_audio(completed.take, captured.view()))
        return failure(RecordingCoordinatorError::CaptureCopyFailed);
    const auto offset = *config_.latency.input_placement_offset_frames;
    if (completed.placement_start.value < 0)
        return failure(RecordingCoordinatorError::InvalidTake);
    const auto placement = static_cast<std::uint64_t>(completed.placement_start.value);
    const auto leading_trim = offset > placement ? offset - placement : 0;
    if (leading_trim >= completed.frame_count)
        return failure(RecordingCoordinatorError::InvalidTake);
    request.placement_start = subtract_frames(completed.placement_start,
                                              std::min(offset, placement));
    request.sample_rate = config_.sample_rate;
    auto captured_view = static_cast<const audio::Buffer<float>&>(captured).view().slice(
        static_cast<std::size_t>(leading_trim),
        static_cast<std::size_t>(completed.frame_count - leading_trim));
    auto sealed = seal_recording_take(captured_view, std::move(request));
    if (!sealed)
        return failure(RecordingCoordinatorError::SealFailed);
    if (!stager.stage_media(sealed->asset.content_hash, sealed->wav_bytes))
        return failure(RecordingCoordinatorError::MediaStageFailed);

    auto committed = session.submit(
        writer, make_transaction(writer, session.revision(), std::move(sealed->commands)));
    if (!committed)
        return failure(RecordingCoordinatorError::DocumentCommitFailed);

    committed_generations_[completed.take.slot] = completed.take.generation;
    CaptureCommand release_command;
    release_command.type = CaptureCommandType::ReleaseTake;
    release_command.take = completed.take;
    if (!capture_.enqueue_command(release_command))
        pending_release_generations_[completed.take.slot] = completed.take.generation;
    return runtime::Result<CommittedRecordingTake, RecordingCoordinatorError>(runtime::Ok(
        CommittedRecordingTake{track.source, monitoring_paths_[track_index], sealed->asset,
                               sealed->take, std::move(committed).value()}));
}

} // namespace pulp::playback
