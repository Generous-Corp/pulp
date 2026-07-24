#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#include <pulp/runtime/exceptions.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::audio {

namespace {

bool multiply_overflows(std::uint64_t a, std::uint64_t b, std::uint64_t& result) noexcept {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) return true;
    result = a * b;
    return false;
}

bool same_snapshot(const RollingAudioCaptureSnapshot& a,
                   const RollingAudioCaptureSnapshot& b) noexcept {
    return a.valid == b.valid &&
           a.generation == b.generation &&
           a.num_channels == b.num_channels &&
           a.capacity_frames == b.capacity_frames &&
           a.start_frame == b.start_frame &&
           a.frame_count == b.frame_count &&
           a.end_frame == b.end_frame;
}

}  // namespace

RollingAudioCaptureHold::RollingAudioCaptureHold(
    RollingAudioCaptureBuffer& buffer,
    RollingAudioCaptureSnapshot snapshot,
    std::uint64_t hold_id) noexcept
    : buffer_(snapshot.valid ? &buffer : nullptr)
    , snapshot_(snapshot)
    , hold_id_(snapshot.valid ? hold_id : 0) {}

RollingAudioCaptureHold::RollingAudioCaptureHold(
    RollingAudioCaptureHold&& other) noexcept
    : buffer_(other.buffer_)
    , snapshot_(other.snapshot_)
    , hold_id_(other.hold_id_) {
    other.buffer_ = nullptr;
    other.snapshot_ = {};
    other.hold_id_ = 0;
}

RollingAudioCaptureHold& RollingAudioCaptureHold::operator=(
    RollingAudioCaptureHold&& other) noexcept {
    if (this == &other) return *this;
    release();
    buffer_ = other.buffer_;
    snapshot_ = other.snapshot_;
    hold_id_ = other.hold_id_;
    other.buffer_ = nullptr;
    other.snapshot_ = {};
    other.hold_id_ = 0;
    return *this;
}

RollingAudioCaptureHold::~RollingAudioCaptureHold() noexcept {
    release();
}

void RollingAudioCaptureHold::release() noexcept {
    if (buffer_ != nullptr && snapshot_.valid) {
        buffer_->end_hold_if_current(snapshot_, hold_id_);
    }
    buffer_ = nullptr;
    snapshot_ = {};
    hold_id_ = 0;
}

std::uint64_t RollingAudioCaptureBuffer::estimate_bytes(
    std::uint32_t num_channels,
    std::uint64_t frames,
    std::uint64_t bytes_per_sample) noexcept {
    std::uint64_t channel_frames = 0;
    if (multiply_overflows(static_cast<std::uint64_t>(num_channels), frames, channel_frames)) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    std::uint64_t bytes = 0;
    if (multiply_overflows(channel_frames, bytes_per_sample, bytes)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return bytes;
}

void RollingAudioCaptureBuffer::clear_unprepared() noexcept {
    std::vector<float>().swap(storage_);
    num_channels_ = 0;
    capacity_frames_ = 0;
    write_pos_ = 0;
    total_written_ = 0;
    held_snapshot_ = {};
    active_hold_id_ = 0;
    frames_discarded_while_held_.store(0, std::memory_order_relaxed);
    hold_active_.store(false, std::memory_order_release);
    advance_generation();
    state_.write({0, 0, generation_});
}

void RollingAudioCaptureBuffer::advance_generation() noexcept {
    ++generation_;
    if (generation_ == 0) generation_ = 1;
}

bool RollingAudioCaptureBuffer::prepare(const RollingAudioCaptureBufferConfig& config) {
    if (config.num_channels == 0 || config.max_frames == 0 ||
        config.bytes_per_sample != sizeof(float)) {
        clear_unprepared();
        return false;
    }

    const auto sample_count = estimate_bytes(config.num_channels, config.max_frames, 1);
    if (sample_count == std::numeric_limits<std::uint64_t>::max() ||
        sample_count > std::numeric_limits<std::size_t>::max()) {
        clear_unprepared();
        return false;
    }

    PULP_TRY {
        storage_.assign(static_cast<std::size_t>(sample_count), 0.0f);
    } PULP_CATCH_ALL {
        clear_unprepared();
        return false;
    }

    num_channels_ = config.num_channels;
    capacity_frames_ = config.max_frames;
    reset();
    return true;
}

RollingAudioCapturePrepareResult RollingAudioCaptureBuffer::prepare_seconds(
    std::uint32_t num_channels,
    double sample_rate,
    double seconds,
    std::uint64_t max_bytes) {
    RollingAudioCapturePrepareResult result;
    if (num_channels == 0 || sample_rate <= 0.0 || seconds <= 0.0 ||
        !std::isfinite(sample_rate) || !std::isfinite(seconds)) {
        clear_unprepared();
        return result;
    }

    const auto requested = std::ceil(sample_rate * seconds);
    if (requested <= 0.0 ||
        requested > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        clear_unprepared();
        return result;
    }

    result.requested_frames = static_cast<std::uint64_t>(requested);
    result.allocated_bytes = estimate_bytes(num_channels, result.requested_frames);
    if (result.allocated_bytes == std::numeric_limits<std::uint64_t>::max()) {
        clear_unprepared();
        return result;
    }
    if (max_bytes != 0 && result.allocated_bytes > max_bytes) {
        clear_unprepared();
        return result;
    }

    result.ok = prepare({num_channels, result.requested_frames, sizeof(float)});
    if (result.ok) result.allocated_frames = capacity_frames_;
    return result;
}

void RollingAudioCaptureBuffer::reset() noexcept {
    write_pos_ = 0;
    total_written_ = 0;
    held_snapshot_ = {};
    active_hold_id_ = 0;
    frames_discarded_while_held_.store(0, std::memory_order_relaxed);
    hold_active_.store(false, std::memory_order_release);
    advance_generation();
    state_.write({0, 0, generation_});
}

std::uint64_t RollingAudioCaptureBuffer::allocated_bytes() const noexcept {
    return estimate_bytes(num_channels_, capacity_frames_);
}

float* RollingAudioCaptureBuffer::channel_storage(std::uint32_t channel) noexcept {
    return storage_.data() +
           static_cast<std::size_t>(channel) * static_cast<std::size_t>(capacity_frames_);
}

const float* RollingAudioCaptureBuffer::channel_storage(std::uint32_t channel) const noexcept {
    return storage_.data() +
           static_cast<std::size_t>(channel) * static_cast<std::size_t>(capacity_frames_);
}

void RollingAudioCaptureBuffer::append(BufferView<const float> source,
                                       std::uint64_t frames) noexcept {
    append_impl(source, frames);
}

void RollingAudioCaptureBuffer::append(BufferView<float> source, std::uint64_t frames) noexcept {
    append_impl(source, frames);
}

template<typename SampleType>
void RollingAudioCaptureBuffer::append_impl(BufferView<SampleType> source,
                                            std::uint64_t frames) noexcept {
    if (hold_active_.load(std::memory_order_acquire)) {
        const auto discarded = std::min(
            frames,
            static_cast<std::uint64_t>(source.num_samples()));
        if (discarded > 0 && source.num_channels() > 0) {
            frames_discarded_while_held_.fetch_add(discarded,
                                                   std::memory_order_relaxed);
        }
        return;
    }
    if (num_channels_ == 0 || capacity_frames_ == 0 || frames == 0 ||
        source.num_channels() == 0 || source.num_samples() == 0) {
        return;
    }

    std::uint64_t remaining =
        std::min(frames, static_cast<std::uint64_t>(source.num_samples()));
    std::uint64_t source_offset = 0;
    while (remaining > 0) {
        const auto segment =
            std::min(remaining, capacity_frames_ - write_pos_);
        for (std::uint32_t ch = 0; ch < num_channels_; ++ch) {
            float* destination = channel_storage(ch) + write_pos_;
            if (ch < source.num_channels()) {
                const SampleType* source_ptr = source.channel_ptr(ch) + source_offset;
                std::copy_n(source_ptr, static_cast<std::size_t>(segment), destination);
            } else {
                std::fill_n(destination, static_cast<std::size_t>(segment), 0.0f);
            }
        }

        write_pos_ = (write_pos_ + segment) % capacity_frames_;
        total_written_ += segment;
        source_offset += segment;
        remaining -= segment;
    }

    state_.write({total_written_, write_pos_, generation_});
}

RollingAudioCaptureSnapshot RollingAudioCaptureBuffer::snapshot_last(
    std::uint64_t frames) const noexcept {
    const auto state = state_.read();
    const auto available = std::min(state.total_written, capacity_frames_);
    const auto count = std::min(frames, available);
    if (count == 0 || num_channels_ == 0 || capacity_frames_ == 0) {
        return {};
    }

    return {
        true,
        state.generation,
        num_channels_,
        capacity_frames_,
        state.total_written - count,
        count,
        state.total_written,
    };
}

RollingAudioCaptureHold RollingAudioCaptureBuffer::hold_last(
    std::uint64_t frames) noexcept {
    const auto snapshot = begin_hold_last(frames);
    return RollingAudioCaptureHold(*this, snapshot, active_hold_id_);
}

RollingAudioCaptureSnapshot RollingAudioCaptureBuffer::begin_hold_last(
    std::uint64_t frames) noexcept {
    const auto snapshot = snapshot_last(frames);
    if (!snapshot.valid) {
        held_snapshot_ = {};
        active_hold_id_ = 0;
        hold_active_.store(false, std::memory_order_release);
        return {};
    }

    held_snapshot_ = snapshot;
    active_hold_id_ = next_hold_id();
    hold_active_.store(true, std::memory_order_release);
    return snapshot;
}

bool RollingAudioCaptureBuffer::hold_active() const noexcept {
    return hold_active_.load(std::memory_order_acquire);
}

void RollingAudioCaptureBuffer::end_hold() noexcept {
    active_hold_id_ = 0;
    hold_active_.store(false, std::memory_order_release);
}

std::uint64_t RollingAudioCaptureBuffer::next_hold_id() noexcept {
    const auto id = next_hold_id_++;
    if (next_hold_id_ == 0) next_hold_id_ = 1;
    return id == 0 ? next_hold_id() : id;
}

void RollingAudioCaptureBuffer::end_hold_if_current(
    const RollingAudioCaptureSnapshot& snapshot,
    std::uint64_t hold_id) noexcept {
    if (hold_id != 0 && hold_id == active_hold_id_ &&
        same_snapshot(snapshot, held_snapshot_)) {
        end_hold();
    }
}

bool RollingAudioCaptureBuffer::snapshot_available(
    const RollingAudioCaptureSnapshot& snapshot,
    CursorState state) const noexcept {
    if (!snapshot.valid || snapshot.num_channels != num_channels_ ||
        snapshot.capacity_frames != capacity_frames_ || snapshot.frame_count == 0 ||
        snapshot.frame_count > capacity_frames_) {
        return false;
    }
    if (snapshot.generation != state.generation) return false;
    if (snapshot.end_frame != snapshot.start_frame + snapshot.frame_count) return false;
    if (snapshot.end_frame > state.total_written) return false;
    return state.total_written - snapshot.start_frame <= capacity_frames_;
}

RollingAudioCaptureMaterializeResult RollingAudioCaptureBuffer::materialize_held(
    const RollingAudioCaptureHold& hold,
    BufferView<float> destination) const noexcept {
    if (hold.buffer_ != this) {
        return {hold.snapshot_.valid ? RollingAudioCaptureMaterializeStatus::Overwritten
                                     : RollingAudioCaptureMaterializeStatus::InvalidSnapshot,
                0};
    }
    return materialize_held(hold.snapshot_, destination);
}

RollingAudioCaptureMaterializeResult RollingAudioCaptureBuffer::materialize_held(
    const RollingAudioCaptureSnapshot& snapshot,
    BufferView<float> destination) const noexcept {
    if (!hold_active_.load(std::memory_order_acquire)) {
        return {snapshot.valid ? RollingAudioCaptureMaterializeStatus::Overwritten
                               : RollingAudioCaptureMaterializeStatus::InvalidSnapshot,
                0};
    }
    if (!same_snapshot(snapshot, held_snapshot_)) {
        return {snapshot.valid ? RollingAudioCaptureMaterializeStatus::Overwritten
                               : RollingAudioCaptureMaterializeStatus::InvalidSnapshot,
                0};
    }

    const auto result = materialize_quiescent(snapshot, destination);
    if (result.status != RollingAudioCaptureMaterializeStatus::Ok) return result;

    if (!hold_active_.load(std::memory_order_acquire)) {
        return {RollingAudioCaptureMaterializeStatus::Overwritten, 0};
    }
    if (!same_snapshot(snapshot, held_snapshot_)) {
        return {RollingAudioCaptureMaterializeStatus::Overwritten, 0};
    }
    return result;
}

RollingAudioCaptureMaterializeResult RollingAudioCaptureBuffer::materialize_quiescent(
    const RollingAudioCaptureSnapshot& snapshot,
    BufferView<float> destination) const noexcept {
    if (destination.num_samples() < snapshot.frame_count) {
        return {RollingAudioCaptureMaterializeStatus::DestinationTooSmall, 0};
    }

    const auto before = state_.read();
    if (!snapshot_available(snapshot, before)) {
        return {snapshot.valid ? RollingAudioCaptureMaterializeStatus::Overwritten
                               : RollingAudioCaptureMaterializeStatus::InvalidSnapshot,
                0};
    }

    for (std::size_t ch = 0; ch < destination.num_channels(); ++ch) {
        float* destination_channel = destination.channel_ptr(ch);
        if (ch >= num_channels_) {
            std::fill_n(destination_channel,
                        static_cast<std::size_t>(snapshot.frame_count),
                        0.0f);
            continue;
        }

        const float* source_channel = channel_storage(static_cast<std::uint32_t>(ch));
        for (std::uint64_t i = 0; i < snapshot.frame_count; ++i) {
            const auto source_index = (snapshot.start_frame + i) % capacity_frames_;
            destination_channel[i] = source_channel[source_index];
        }
    }

    const auto after = state_.read();
    if (!snapshot_available(snapshot, after)) {
        return {RollingAudioCaptureMaterializeStatus::Overwritten, 0};
    }

    return {RollingAudioCaptureMaterializeStatus::Ok, snapshot.frame_count};
}

RollingAudioCaptureMaterializeResult RollingAudioCaptureBuffer::materialize(
    const RollingAudioCaptureSnapshot& snapshot,
    BufferView<float> destination) const noexcept {
    return materialize_quiescent(snapshot, destination);
}

template void RollingAudioCaptureBuffer::append_impl(BufferView<const float>,
                                                     std::uint64_t) noexcept;
template void RollingAudioCaptureBuffer::append_impl(BufferView<float>,
                                                     std::uint64_t) noexcept;

}  // namespace pulp::audio
