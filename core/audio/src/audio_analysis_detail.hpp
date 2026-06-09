#pragma once

#include <pulp/audio/buffer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::audio::detail {

inline std::uint64_t clamp_frame(std::uint64_t frame,
                                 std::uint64_t source_frames) noexcept {
    if (source_frames == 0) return 0;
    return std::min(frame, source_frames - 1);
}

inline std::uint64_t range_begin(std::uint64_t hint,
                                 std::uint64_t radius) noexcept {
    return hint > radius ? hint - radius : 0;
}

inline std::uint64_t range_end(std::uint64_t hint,
                               std::uint64_t radius,
                               std::uint64_t source_frames) noexcept {
    if (source_frames == 0) return 0;
    const auto max_frame = source_frames - 1;
    if (hint > max_frame) hint = max_frame;
    if (radius > max_frame - hint) return max_frame;
    return hint + radius;
}

inline double aggregate_sample(BufferView<const float> source,
                               std::uint64_t frame) noexcept {
    if (source.num_channels() == 0 || source.num_samples() == 0) return 0.0;
    const auto index = static_cast<std::size_t>(
        clamp_frame(frame, static_cast<std::uint64_t>(source.num_samples())));
    double sum = 0.0;
    for (std::size_t ch = 0; ch < source.num_channels(); ++ch) {
        sum += static_cast<double>(source.channel_ptr(ch)[index]);
    }
    return sum / static_cast<double>(source.num_channels());
}

inline double average_abs_delta(BufferView<const float> source,
                                std::uint64_t a,
                                std::uint64_t b) noexcept {
    if (source.num_channels() == 0 || source.num_samples() == 0) return 0.0;
    const auto frames = static_cast<std::uint64_t>(source.num_samples());
    const auto index_a = static_cast<std::size_t>(clamp_frame(a, frames));
    const auto index_b = static_cast<std::size_t>(clamp_frame(b, frames));
    double sum = 0.0;
    for (std::size_t ch = 0; ch < source.num_channels(); ++ch) {
        sum += std::abs(static_cast<double>(source.channel_ptr(ch)[index_a]) -
                        static_cast<double>(source.channel_ptr(ch)[index_b]));
    }
    return sum / static_cast<double>(source.num_channels());
}

inline double average_abs_sample(BufferView<const float> source,
                                 std::uint64_t frame) noexcept {
    if (source.num_channels() == 0 || source.num_samples() == 0) return 0.0;
    const auto frames = static_cast<std::uint64_t>(source.num_samples());
    const auto index = static_cast<std::size_t>(clamp_frame(frame, frames));
    double sum = 0.0;
    for (std::size_t ch = 0; ch < source.num_channels(); ++ch) {
        sum += std::abs(static_cast<double>(source.channel_ptr(ch)[index]));
    }
    return sum / static_cast<double>(source.num_channels());
}

inline double slope_at(BufferView<const float> source,
                       std::uint64_t frame) noexcept {
    const auto frames = static_cast<std::uint64_t>(source.num_samples());
    if (frames < 2) return 0.0;
    const auto current = clamp_frame(frame, frames);
    if (current + 1 < frames) {
        return aggregate_sample(source, current + 1) -
               aggregate_sample(source, current);
    }
    return aggregate_sample(source, current) -
           aggregate_sample(source, current - 1);
}

inline double rms_window(BufferView<const float> source,
                         std::uint64_t start,
                         std::uint64_t frames) noexcept {
    if (source.num_channels() == 0 || source.num_samples() == 0 || frames == 0) {
        return 0.0;
    }
    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    start = std::min(start, source_frames - 1);
    frames = std::min(frames, source_frames - start);

    double energy = 0.0;
    std::uint64_t count = 0;
    for (std::size_t ch = 0; ch < source.num_channels(); ++ch) {
        const auto* data = source.channel_ptr(ch);
        for (std::uint64_t i = 0; i < frames; ++i) {
            const auto value = static_cast<double>(data[start + i]);
            energy += value * value;
            ++count;
        }
    }
    return count == 0 ? 0.0 : std::sqrt(energy / static_cast<double>(count));
}

inline double window_correlation(BufferView<const float> source,
                                 std::uint64_t a,
                                 std::uint64_t b,
                                 std::uint64_t frames) noexcept {
    if (source.num_channels() == 0 || source.num_samples() == 0 || frames == 0) {
        return 0.0;
    }
    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    if (a >= source_frames || b >= source_frames) return 0.0;
    frames = std::min(frames, source_frames - a);
    frames = std::min(frames, source_frames - b);
    if (frames == 0) return 0.0;

    double dot = 0.0;
    double energy_a = 0.0;
    double energy_b = 0.0;
    for (std::size_t ch = 0; ch < source.num_channels(); ++ch) {
        const auto* data = source.channel_ptr(ch);
        for (std::uint64_t i = 0; i < frames; ++i) {
            const auto va = static_cast<double>(data[a + i]);
            const auto vb = static_cast<double>(data[b + i]);
            dot += va * vb;
            energy_a += va * va;
            energy_b += vb * vb;
        }
    }
    if (energy_a <= 1.0e-18 && energy_b <= 1.0e-18) return 1.0;
    if (energy_a <= 1.0e-18 || energy_b <= 1.0e-18) return 0.0;
    return dot / std::sqrt(energy_a * energy_b);
}

inline bool is_zero_crossing(BufferView<const float> source,
                             std::uint64_t frame) noexcept {
    return average_abs_sample(source, frame) <= 1.0e-6;
}

inline std::uint64_t snap_to_zero_crossing(BufferView<const float> source,
                                           std::uint64_t frame,
                                           std::uint64_t radius) noexcept {
    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    if (source_frames == 0) return 0;
    frame = clamp_frame(frame, source_frames);
    if (is_zero_crossing(source, frame)) return frame;

    for (std::uint64_t distance = 1; distance <= radius; ++distance) {
        if (frame >= distance && is_zero_crossing(source, frame - distance)) {
            return frame - distance;
        }
        if (frame + distance < source_frames &&
            is_zero_crossing(source, frame + distance)) {
            return frame + distance;
        }
    }
    return frame;
}

inline double zero_crossing_cost(BufferView<const float> source,
                                 std::uint64_t frame) noexcept {
    // Integer-frame loop endpoints are only click-free when the actual sample
    // value at that frame is close to zero on every channel. A sign change
    // between adjacent full-scale samples is a crossing between frames, not a
    // zero-cost integer boundary.
    return average_abs_sample(source, frame);
}

}  // namespace pulp::audio::detail
