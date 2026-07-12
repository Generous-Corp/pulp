#pragma once

#include <cstdint>

#include <pulp/audio/planar_audio_ring_buffer.hpp>

namespace pulp::audio {

/// Fixed-capacity, single-producer/single-consumer audio analysis tap.
/// prepare() is off-RT. push() and drain() are allocation-free afterwards.
/// Overflow is measured and dropped in whole frames, so channels cannot drift.
class AudioTap {
public:
    bool prepare(std::uint32_t channels, std::uint64_t capacity_frames) {
        return ring_.prepare(channels, capacity_frames);
    }
    void release() noexcept { ring_.release(); }
    void reset() noexcept { ring_.reset(); }

    std::uint64_t push(BufferView<const float> source) noexcept {
        return ring_.write(source, source.num_samples());
    }
    std::uint64_t push(BufferView<float> source) noexcept {
        return ring_.write(source, source.num_samples());
    }
    bool read(BufferView<float> destination) noexcept {
        return ring_.read(destination, destination.num_samples());
    }

    std::uint32_t num_channels() const noexcept { return ring_.num_channels(); }
    std::uint64_t available_frames() const noexcept { return ring_.available_frames(); }
    PlanarAudioRingBufferStats stats() const noexcept { return ring_.stats(); }

private:
    PlanarAudioRingBuffer ring_;
};

} // namespace pulp::audio
