#include <pulp/audio/planar_audio_ring_buffer.hpp>

#include <new>

namespace pulp::audio {

bool PlanarAudioRingBuffer::valid_capacity(std::uint64_t usable_capacity_frames) noexcept {
    return usable_capacity_frames > 0 &&
           usable_capacity_frames <
               static_cast<std::uint64_t>(std::numeric_limits<int>::max());
}

int PlanarAudioRingBuffer::clamp_to_fifo_count(std::uint64_t frames) noexcept {
    const auto max_count = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(frames, max_count));
}

void PlanarAudioRingBuffer::clear_unprepared() noexcept {
    std::vector<float>().swap(storage_);
    fifo_.reset();
    num_channels_ = 0;
    usable_capacity_frames_ = 0;
    internal_capacity_frames_ = 0;
    underrun_frames_.store(0, std::memory_order_relaxed);
    overrun_frames_.store(0, std::memory_order_relaxed);
    dropped_write_frames_.store(0, std::memory_order_relaxed);
}

void PlanarAudioRingBuffer::release() noexcept {
    clear_unprepared();
}

bool PlanarAudioRingBuffer::prepare(std::uint32_t num_channels,
                                    std::uint64_t usable_capacity_frames) {
    if (num_channels == 0 || !valid_capacity(usable_capacity_frames)) {
        clear_unprepared();
        return false;
    }

    const auto internal_capacity = usable_capacity_frames + 1;
    if (internal_capacity >
        std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(num_channels)) {
        clear_unprepared();
        return false;
    }

    try {
        storage_.assign(static_cast<std::size_t>(num_channels) *
                            static_cast<std::size_t>(internal_capacity),
                        0.0f);
        fifo_ = std::make_unique<runtime::SpscRingIndex>(static_cast<int>(internal_capacity));
    } catch (...) {
        clear_unprepared();
        return false;
    }

    num_channels_ = num_channels;
    usable_capacity_frames_ = usable_capacity_frames;
    internal_capacity_frames_ = internal_capacity;
    reset();
    return true;
}

void PlanarAudioRingBuffer::reset() noexcept {
    if (fifo_) fifo_->reset();
    underrun_frames_.store(0, std::memory_order_relaxed);
    overrun_frames_.store(0, std::memory_order_relaxed);
    dropped_write_frames_.store(0, std::memory_order_relaxed);
}

std::uint64_t PlanarAudioRingBuffer::available_frames() const noexcept {
    return fifo_ ? static_cast<std::uint64_t>(fifo_->num_ready()) : 0;
}

std::uint64_t PlanarAudioRingBuffer::free_frames() const noexcept {
    return fifo_ ? static_cast<std::uint64_t>(fifo_->free_space()) : 0;
}

PlanarAudioRingBufferStats PlanarAudioRingBuffer::stats() const noexcept {
    return {
        underrun_frames_.load(std::memory_order_relaxed),
        overrun_frames_.load(std::memory_order_relaxed),
        dropped_write_frames_.load(std::memory_order_relaxed),
    };
}

float* PlanarAudioRingBuffer::channel_storage(std::uint32_t channel) noexcept {
    return storage_.data() +
           static_cast<std::size_t>(channel) *
               static_cast<std::size_t>(internal_capacity_frames_);
}

const float* PlanarAudioRingBuffer::channel_storage(std::uint32_t channel) const noexcept {
    return storage_.data() +
           static_cast<std::size_t>(channel) *
               static_cast<std::size_t>(internal_capacity_frames_);
}

bool PlanarAudioRingBuffer::read(BufferView<float> destination, std::uint64_t frames) noexcept {
    if (!fifo_ || frames == 0 || destination.num_channels() == 0 ||
        destination.num_samples() == 0) {
        return true;
    }

    const auto requested =
        std::min(frames, static_cast<std::uint64_t>(destination.num_samples()));
    int start_1 = 0;
    int size_1 = 0;
    int start_2 = 0;
    int size_2 = 0;
    fifo_->prepare_to_read(clamp_to_fifo_count(requested), start_1, size_1, start_2, size_2);

    const auto frames_read = static_cast<std::uint64_t>(size_1 + size_2);
    const auto copy_segment = [&](int source_start,
                                  int frame_count,
                                  std::uint64_t destination_offset) noexcept {
        if (frame_count <= 0) return;
        for (std::size_t ch = 0; ch < destination.num_channels(); ++ch) {
            float* destination_ptr = destination.channel_ptr(ch) + destination_offset;
            if (ch < num_channels_) {
                const float* source_ptr =
                    channel_storage(static_cast<std::uint32_t>(ch)) + source_start;
                std::copy_n(source_ptr, static_cast<std::size_t>(frame_count), destination_ptr);
            } else {
                std::fill_n(destination_ptr, static_cast<std::size_t>(frame_count), 0.0f);
            }
        }
    };

    copy_segment(start_1, size_1, 0);
    copy_segment(start_2, size_2, static_cast<std::uint64_t>(size_1));
    fifo_->finish_read(static_cast<int>(frames_read));

    if (frames_read < requested) {
        const auto missing = requested - frames_read;
        for (std::size_t ch = 0; ch < destination.num_channels(); ++ch) {
            std::fill_n(destination.channel_ptr(ch) + frames_read,
                        static_cast<std::size_t>(missing),
                        0.0f);
        }
        underrun_frames_.fetch_add(missing, std::memory_order_relaxed);
        return false;
    }

    return true;
}

std::uint64_t PlanarAudioRingBuffer::drain(std::uint64_t max_frames) noexcept {
    if (!fifo_ || max_frames == 0) return 0;

    int start_1 = 0;
    int size_1 = 0;
    int start_2 = 0;
    int size_2 = 0;
    fifo_->prepare_to_read(clamp_to_fifo_count(max_frames), start_1, size_1, start_2, size_2);
    const auto drained = static_cast<std::uint64_t>(size_1 + size_2);
    fifo_->finish_read(static_cast<int>(drained));
    return drained;
}

}  // namespace pulp::audio
