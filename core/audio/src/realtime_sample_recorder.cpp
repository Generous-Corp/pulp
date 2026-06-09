#include <pulp/audio/realtime_sample_recorder.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::audio {

bool RealtimeSampleRecorder::valid_config(
    const RealtimeSampleRecorderConfig& config) const noexcept {
    return config.num_channels > 0 && config.max_frames > 0 &&
           config.sample_rate > 0.0 && std::isfinite(config.sample_rate);
}

bool RealtimeSampleRecorder::prepare(const RealtimeSampleRecorderConfig& config) {
    if (!valid_config(config)) {
        release();
        return false;
    }

    if (config.max_frames >
        std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(config.num_channels)) {
        release();
        return false;
    }

    try {
        storage_.assign(static_cast<std::size_t>(config.num_channels) *
                            static_cast<std::size_t>(config.max_frames),
                        0.0f);
        mutable_ptrs_.assign(config.num_channels, nullptr);
    } catch (...) {
        release();
        return false;
    }

    num_channels_ = config.num_channels;
    max_frames_ = config.max_frames;
    sample_rate_ = config.sample_rate;
    prepared_ = true;
    reset();
    return true;
}

void RealtimeSampleRecorder::release() noexcept {
    std::vector<float>().swap(storage_);
    std::vector<float*>().swap(mutable_ptrs_);
    state_ = RealtimeSampleRecorderState::Idle;
    num_channels_ = 0;
    max_frames_ = 0;
    target_frames_ = 0;
    frames_recorded_ = 0;
    active_sequence_id_ = 0;
    sample_rate_ = 0.0;
    cancel_on_transport_jump_ = true;
    prepared_ = false;
    state_snapshot_.store(static_cast<std::uint8_t>(RealtimeSampleRecorderState::Idle),
                          std::memory_order_release);
    frames_recorded_snapshot_.store(0, std::memory_order_release);
    total_recorded_frames_.store(0, std::memory_order_relaxed);
    dropped_commands_.store(0, std::memory_order_relaxed);
    dropped_events_.store(0, std::memory_order_relaxed);
}

void RealtimeSampleRecorder::reset() noexcept {
    while (command_queue_.try_pop().has_value()) {}
    while (event_queue_.try_pop().has_value()) {}
    clear_storage();
    set_state(RealtimeSampleRecorderState::Idle);
    target_frames_ = max_frames_;
    frames_recorded_ = 0;
    active_sequence_id_ = 0;
    publish_frames_recorded();
    cancel_on_transport_jump_ = true;
    total_recorded_frames_.store(0, std::memory_order_relaxed);
    dropped_commands_.store(0, std::memory_order_relaxed);
    dropped_events_.store(0, std::memory_order_relaxed);
}

bool RealtimeSampleRecorder::enqueue_command(
    const RealtimeSampleRecorderCommand& command) noexcept {
    if (!prepared_) return false;
    if (!command_queue_.try_push(command)) {
        dropped_commands_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool RealtimeSampleRecorder::pop_event(RealtimeSampleRecorderEvent& event) noexcept {
    auto popped = event_queue_.try_pop();
    if (!popped.has_value()) return false;
    event = *popped;
    return true;
}

void RealtimeSampleRecorder::clear_storage() noexcept {
    std::fill(storage_.begin(), storage_.end(), 0.0f);
}

float* RealtimeSampleRecorder::mutable_channel_data(std::uint32_t channel) noexcept {
    return storage_.data() + static_cast<std::size_t>(channel) *
                                 static_cast<std::size_t>(max_frames_);
}

const float* RealtimeSampleRecorder::channel_data(std::uint32_t channel) const noexcept {
    if (channel >= num_channels_) return nullptr;
    return storage_.data() + static_cast<std::size_t>(channel) *
                                 static_cast<std::size_t>(max_frames_);
}

RealtimeSampleRecorderState RealtimeSampleRecorder::state() const noexcept {
    return static_cast<RealtimeSampleRecorderState>(
        state_snapshot_.load(std::memory_order_acquire));
}

std::uint64_t RealtimeSampleRecorder::frames_recorded() const noexcept {
    return frames_recorded_snapshot_.load(std::memory_order_acquire);
}

void RealtimeSampleRecorder::set_state(RealtimeSampleRecorderState state) noexcept {
    state_ = state;
    state_snapshot_.store(static_cast<std::uint8_t>(state), std::memory_order_release);
}

void RealtimeSampleRecorder::publish_frames_recorded() noexcept {
    frames_recorded_snapshot_.store(frames_recorded_, std::memory_order_release);
}

void RealtimeSampleRecorder::push_event(const RealtimeSampleRecorderEvent& event) noexcept {
    if (!event_queue_.try_push(event)) {
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
    }
}

void RealtimeSampleRecorder::push_simple_event(std::uint64_t sequence_id,
                                               RealtimeSampleRecorderEventType type,
                                               std::uint32_t block_offset) noexcept {
    push_event({sequence_id, type, state_, frames_recorded_, block_offset});
}

bool RealtimeSampleRecorder::collect_commands(
    std::array<TimedCommand, kCommandQueueCapacity>& commands,
    std::uint32_t& count,
    std::uint32_t block_frames) noexcept {
    count = 0;
    std::uint32_t order = 0;
    while (count < commands.size()) {
        auto popped = command_queue_.try_pop();
        if (!popped.has_value()) break;
        auto command = *popped;
        std::uint32_t offset = 0;
        if (command.timing.type == RealtimeSampleRecorderTimingType::BlockOffset) {
            offset = std::min(command.timing.frame_offset, block_frames);
        }

        commands[count++] = {command, offset, order++};
    }
    return count > 0;
}

void RealtimeSampleRecorder::sort_commands(
    std::array<TimedCommand, kCommandQueueCapacity>& commands,
    std::uint32_t count) noexcept {
    std::sort(commands.begin(), commands.begin() + count, [](const auto& a, const auto& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        return a.order < b.order;
    });
}

void RealtimeSampleRecorder::begin_recording(
    const RealtimeSampleRecorderCommand& command,
    std::uint32_t block_offset) noexcept {
    const bool inherit_armed_settings =
        state_ == RealtimeSampleRecorderState::Armed && command.max_frames == 0;

    frames_recorded_ = 0;
    publish_frames_recorded();
    if (!inherit_armed_settings) {
        target_frames_ = command.max_frames == 0 ? max_frames_ : command.max_frames;
        cancel_on_transport_jump_ = command.cancel_on_transport_jump;
    }
    active_sequence_id_ = command.sequence_id;
    set_state(RealtimeSampleRecorderState::Recording);
    push_simple_event(command.sequence_id, RealtimeSampleRecorderEventType::Started, block_offset);
}

void RealtimeSampleRecorder::complete_recording(std::uint64_t sequence_id,
                                                std::uint32_t block_offset) noexcept {
    if (state_ == RealtimeSampleRecorderState::Recording ||
        state_ == RealtimeSampleRecorderState::Completing) {
        const auto event_sequence_id =
            sequence_id == 0 ? active_sequence_id_ : sequence_id;
        set_state(RealtimeSampleRecorderState::Completing);
        set_state(RealtimeSampleRecorderState::Completed);
        push_simple_event(event_sequence_id, RealtimeSampleRecorderEventType::Completed, block_offset);
        active_sequence_id_ = 0;
    } else {
        push_simple_event(sequence_id,
                          RealtimeSampleRecorderEventType::CommandRejected,
                          block_offset);
    }
}

void RealtimeSampleRecorder::cancel_recording(std::uint64_t sequence_id,
                                              std::uint32_t block_offset) noexcept {
    if (state_ == RealtimeSampleRecorderState::Recording ||
        state_ == RealtimeSampleRecorderState::Armed) {
        const auto event_sequence_id =
            sequence_id == 0 ? active_sequence_id_ : sequence_id;
        set_state(RealtimeSampleRecorderState::Cancelled);
        push_simple_event(event_sequence_id, RealtimeSampleRecorderEventType::Cancelled, block_offset);
        active_sequence_id_ = 0;
    } else {
        push_simple_event(sequence_id,
                          RealtimeSampleRecorderEventType::CommandRejected,
                          block_offset);
    }
}

void RealtimeSampleRecorder::apply_command(const RealtimeSampleRecorderCommand& command,
                                           std::uint32_t block_offset) noexcept {
    switch (command.type) {
        case RealtimeSampleRecorderCommandType::Arm:
            if (command.max_frames > max_frames_) {
                push_simple_event(command.sequence_id,
                                  RealtimeSampleRecorderEventType::CommandRejected,
                                  block_offset);
                break;
            }
            if (state_ == RealtimeSampleRecorderState::Idle ||
                state_ == RealtimeSampleRecorderState::Cancelled) {
                set_state(RealtimeSampleRecorderState::Armed);
                target_frames_ = command.max_frames == 0 ? max_frames_ : command.max_frames;
                cancel_on_transport_jump_ = command.cancel_on_transport_jump;
                active_sequence_id_ = command.sequence_id;
                push_simple_event(command.sequence_id,
                                  RealtimeSampleRecorderEventType::CommandAccepted,
                                  block_offset);
            } else {
                push_simple_event(command.sequence_id,
                                  RealtimeSampleRecorderEventType::CommandRejected,
                                  block_offset);
            }
            break;
        case RealtimeSampleRecorderCommandType::Start:
            if (command.max_frames > max_frames_) {
                push_simple_event(command.sequence_id,
                                  RealtimeSampleRecorderEventType::CommandRejected,
                                  block_offset);
                break;
            }
            if (state_ == RealtimeSampleRecorderState::Idle ||
                state_ == RealtimeSampleRecorderState::Armed ||
                state_ == RealtimeSampleRecorderState::Cancelled) {
                begin_recording(command, block_offset);
            } else {
                push_simple_event(command.sequence_id,
                                  RealtimeSampleRecorderEventType::CommandRejected,
                                  block_offset);
            }
            break;
        case RealtimeSampleRecorderCommandType::Stop:
            complete_recording(command.sequence_id, block_offset);
            break;
        case RealtimeSampleRecorderCommandType::Cancel:
            cancel_recording(command.sequence_id, block_offset);
            break;
        case RealtimeSampleRecorderCommandType::Reset:
            set_state(RealtimeSampleRecorderState::Idle);
            frames_recorded_ = 0;
            active_sequence_id_ = 0;
            publish_frames_recorded();
            target_frames_ = max_frames_;
            cancel_on_transport_jump_ = true;
            push_simple_event(command.sequence_id,
                              RealtimeSampleRecorderEventType::CommandAccepted,
                              block_offset);
            break;
    }
}

template<typename SampleType>
void RealtimeSampleRecorder::copy_span(BufferView<SampleType> source,
                                       std::uint32_t source_offset,
                                       std::uint32_t frames) noexcept {
    if (frames == 0 || state_ != RealtimeSampleRecorderState::Recording) return;

    const auto remaining =
        frames_recorded_ >= target_frames_ ? 0 : target_frames_ - frames_recorded_;
    const auto frames_to_copy = std::min<std::uint64_t>(frames, remaining);
    if (frames_to_copy == 0) {
        complete_recording(0, source_offset);
        return;
    }

    for (std::uint32_t ch = 0; ch < num_channels_; ++ch) {
        float* destination = mutable_channel_data(ch) + frames_recorded_;
        if (ch < source.num_channels()) {
            const SampleType* input = source.channel_ptr(ch) + source_offset;
            std::copy_n(input, static_cast<std::size_t>(frames_to_copy), destination);
        } else {
            std::fill_n(destination, static_cast<std::size_t>(frames_to_copy), 0.0f);
        }
    }

    frames_recorded_ += frames_to_copy;
    publish_frames_recorded();
    total_recorded_frames_.fetch_add(frames_to_copy, std::memory_order_relaxed);
    if (frames_recorded_ >= target_frames_) {
        complete_recording(0, source_offset + static_cast<std::uint32_t>(frames_to_copy));
    }
}

void RealtimeSampleRecorder::process(BufferView<const float> source,
                                     std::uint32_t frames,
                                     bool transport_jumped) noexcept {
    process_impl(source, frames, transport_jumped);
}

void RealtimeSampleRecorder::process(BufferView<float> source,
                                     std::uint32_t frames,
                                     bool transport_jumped) noexcept {
    process_impl(source, frames, transport_jumped);
}

template<typename SampleType>
void RealtimeSampleRecorder::process_impl(BufferView<SampleType> source,
                                          std::uint32_t frames,
                                          bool transport_jumped) noexcept {
    if (!prepared_) return;
    const auto block_frames =
        source.num_channels() == 0 || source.num_samples() == 0
            ? 0
            : std::min<std::uint32_t>(frames,
                                      static_cast<std::uint32_t>(source.num_samples()));

    if (transport_jumped && cancel_on_transport_jump_ &&
        (state_ == RealtimeSampleRecorderState::Armed ||
         state_ == RealtimeSampleRecorderState::Recording)) {
        cancel_recording(0, 0);
    }

    std::array<TimedCommand, kCommandQueueCapacity> commands{};
    std::uint32_t command_count = 0;
    collect_commands(commands, command_count, block_frames);
    sort_commands(commands, command_count);

    std::uint32_t cursor = 0;
    for (std::uint32_t i = 0; i < command_count; ++i) {
        const auto offset = std::min(commands[i].offset, block_frames);
        if (offset > cursor) copy_span(source, cursor, offset - cursor);
        apply_command(commands[i].command, offset);
        cursor = offset;
    }

    if (cursor < block_frames) copy_span(source, cursor, block_frames - cursor);
}

bool RealtimeSampleRecorder::materialize_snapshot_quiescent(
    const RollingAudioCaptureBuffer& capture,
    const RollingAudioCaptureSnapshot& snapshot) noexcept {
    return materialize_snapshot_impl(capture, snapshot, false);
}

bool RealtimeSampleRecorder::materialize_held_snapshot(
    const RollingAudioCaptureBuffer& capture,
    const RollingAudioCaptureSnapshot& snapshot) noexcept {
    return materialize_snapshot_impl(capture, snapshot, true);
}

bool RealtimeSampleRecorder::materialize_snapshot_impl(
    const RollingAudioCaptureBuffer& capture,
    const RollingAudioCaptureSnapshot& snapshot,
    bool held) noexcept {
    if (!prepared_ || !snapshot.valid || snapshot.num_channels > num_channels_ ||
        snapshot.frame_count > max_frames_) {
        set_state(RealtimeSampleRecorderState::Failed);
        push_simple_event(0, RealtimeSampleRecorderEventType::Failed, 0);
        return false;
    }

    for (std::uint32_t ch = 0; ch < num_channels_; ++ch) {
        mutable_ptrs_[ch] = mutable_channel_data(ch);
    }

    BufferView<float> destination(mutable_ptrs_.data(),
                                  num_channels_,
                                  static_cast<std::size_t>(snapshot.frame_count));
    const auto result = held ? capture.materialize_held(snapshot, destination)
                             : capture.materialize_quiescent(snapshot, destination);
    if (result.status != RollingAudioCaptureMaterializeStatus::Ok) {
        set_state(RealtimeSampleRecorderState::Failed);
        push_simple_event(0, RealtimeSampleRecorderEventType::Failed, 0);
        return false;
    }

    frames_recorded_ = result.frames_copied;
    publish_frames_recorded();
    total_recorded_frames_.fetch_add(result.frames_copied, std::memory_order_relaxed);
    target_frames_ = frames_recorded_;
    set_state(RealtimeSampleRecorderState::Completing);
    set_state(RealtimeSampleRecorderState::Completed);
    push_simple_event(0, RealtimeSampleRecorderEventType::Completed, 0);
    active_sequence_id_ = 0;
    return true;
}

bool RealtimeSampleRecorder::materialize_snapshot(
    const RollingAudioCaptureBuffer& capture,
    const RollingAudioCaptureSnapshot& snapshot) noexcept {
    return materialize_snapshot_quiescent(capture, snapshot);
}

bool RealtimeSampleRecorder::consume_completed_recording() noexcept {
    if (state() != RealtimeSampleRecorderState::Completed) return false;
    set_state(RealtimeSampleRecorderState::Idle);
    frames_recorded_ = 0;
    publish_frames_recorded();
    return true;
}

RealtimeSampleRecorderStats RealtimeSampleRecorder::stats() const noexcept {
    return {
        frames_recorded(),
        total_recorded_frames_.load(std::memory_order_relaxed),
        dropped_commands_.load(std::memory_order_relaxed),
        dropped_events_.load(std::memory_order_relaxed),
    };
}

template void RealtimeSampleRecorder::process_impl<const float>(BufferView<const float>,
                                                                std::uint32_t,
                                                                bool) noexcept;
template void RealtimeSampleRecorder::process_impl<float>(BufferView<float>,
                                                          std::uint32_t,
                                                          bool) noexcept;
template void RealtimeSampleRecorder::copy_span<const float>(BufferView<const float>,
                                                             std::uint32_t,
                                                             std::uint32_t) noexcept;
template void RealtimeSampleRecorder::copy_span<float>(BufferView<float>,
                                                       std::uint32_t,
                                                       std::uint32_t) noexcept;

}  // namespace pulp::audio
