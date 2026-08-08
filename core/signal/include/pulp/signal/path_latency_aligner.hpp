#pragma once

#include "detail/audio_range.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <vector>

namespace pulp::signal {

/// Aligns N already-processed paths to the largest declared intrinsic latency.
/// prepare() allocates bounded ring storage on the control thread. Latency
/// changes are control-thread operations: configure_latencies() validates the
/// whole set atomically and hard-resets history, preventing mixed-delay audio.
/// process() is allocation-free and reports the exact maximum path latency.
///
/// Layout is path-major then channel-major. Each output may exactly alias its
/// corresponding input; cross-path/channel aliasing is rejected. Samples and
/// non-finite values are delayed unchanged.
template <typename SampleType = float, std::size_t MaxPaths = 16, std::size_t MaxChannels = 8,
          typename Allocator = std::allocator<SampleType>>
class PathLatencyAlignerT {
  public:
    static_assert(MaxPaths > 0 && MaxChannels > 0);
    static_assert(std::is_floating_point_v<SampleType>);
    static constexpr std::size_t max_paths = MaxPaths;
    static constexpr std::size_t max_channels = MaxChannels;

    explicit PathLatencyAlignerT(const Allocator& allocator = Allocator{}) : rings_(allocator) {}

    bool prepare(std::size_t paths, std::size_t channels, std::size_t max_latency_samples,
                 std::size_t max_block_size) {
        if (paths == 0 || paths > MaxPaths || channels == 0 || channels > MaxChannels ||
            max_block_size == 0 || max_latency_samples == std::numeric_limits<std::size_t>::max())
            return false;
        const auto stride = max_latency_samples + 1;
        if (paths > std::numeric_limits<std::size_t>::max() / channels)
            return false;
        const auto streams = paths * channels;
        if (streams > std::numeric_limits<std::size_t>::max() / stride)
            return false;
        const auto retained = streams * stride;
        if (retained > rings_.max_size())
            return false;
        std::vector<SampleType, Allocator> replacement(rings_.get_allocator());
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
#endif
            replacement.assign(retained, SampleType{0});
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        } catch (const std::bad_alloc&) {
            return false;
        }
#endif
        rings_.swap(replacement);
        paths_ = paths;
        channels_ = channels;
        max_latency_ = max_latency_samples;
        max_block_size_ = max_block_size;
        stride_ = stride;
        latencies_.fill(0);
        write_positions_.fill(0);
        reported_latency_ = 0;
        return true;
    }

    bool configure_latencies(std::span<const std::size_t> latencies) noexcept {
        if (latencies.size() != paths_ || rings_.empty())
            return false;
        for (const auto latency : latencies)
            if (latency > max_latency_)
                return false;
        reported_latency_ = 0;
        for (std::size_t path = 0; path < paths_; ++path) {
            latencies_[path] = latencies[path];
            reported_latency_ = std::max(reported_latency_, latencies[path]);
        }
        reset();
        return true;
    }

    void reset() noexcept {
        std::fill(rings_.begin(), rings_.end(), SampleType{0});
        write_positions_.fill(0);
    }

    std::size_t reported_latency_samples() const noexcept {
        return reported_latency_;
    }
    std::size_t path_latency_samples(std::size_t path) const noexcept {
        return path < paths_ ? latencies_[path] : 0;
    }
    std::size_t compensation_delay_samples(std::size_t path) const noexcept {
        return path < paths_ ? reported_latency_ - latencies_[path] : 0;
    }
    std::size_t path_count() const noexcept {
        return paths_;
    }
    std::size_t channel_count() const noexcept {
        return channels_;
    }
    std::size_t retained_samples() const noexcept {
        return rings_.size();
    }

    bool process(const SampleType* const* inputs, std::size_t input_count,
                 SampleType* const* outputs, std::size_t output_count,
                 std::size_t frames) noexcept {
        const auto stream_count = paths_ * channels_;
        if (inputs == nullptr || outputs == nullptr || input_count != stream_count ||
            output_count != stream_count || frames > max_block_size_ || rings_.empty())
            return false;
        for (std::size_t stream = 0; stream < stream_count; ++stream) {
            if (inputs[stream] == nullptr || outputs[stream] == nullptr)
                return false;
            for (std::size_t other = 0; other < stream_count; ++other) {
                if (other == stream) {
                    if (outputs[stream] != inputs[stream] &&
                        detail::audio_ranges_overlap(outputs[stream], inputs[stream], frames))
                        return false;
                } else if (detail::audio_ranges_overlap(outputs[stream], inputs[other], frames)) {
                    return false;
                }
                if (other > stream &&
                    detail::audio_ranges_overlap(outputs[stream], outputs[other], frames))
                    return false;
            }
        }

        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (std::size_t path = 0; path < paths_; ++path) {
                const auto delay = compensation_delay_samples(path);
                const auto position = write_positions_[path];
                for (std::size_t channel = 0; channel < channels_; ++channel) {
                    const auto stream = path * channels_ + channel;
                    const auto input = inputs[stream][frame];
                    if (delay == 0) {
                        outputs[stream][frame] = input;
                    } else {
                        auto* ring = rings_.data() + stream * stride_;
                        outputs[stream][frame] = ring[position];
                        ring[position] = input;
                    }
                }
                if (delay > 0)
                    write_positions_[path] = (position + 1) % delay;
            }
        }
        return true;
    }

  private:
    std::size_t paths_ = 0;
    std::size_t channels_ = 0;
    std::size_t max_latency_ = 0;
    std::size_t max_block_size_ = 0;
    std::size_t stride_ = 0;
    std::size_t reported_latency_ = 0;
    std::array<std::size_t, MaxPaths> latencies_{};
    std::array<std::size_t, MaxPaths> write_positions_{};
    std::vector<SampleType, Allocator> rings_;
};

using PathLatencyAligner = PathLatencyAlignerT<float>;
using PathLatencyAligner64 = PathLatencyAlignerT<double>;

} // namespace pulp::signal
