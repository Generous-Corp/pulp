#pragma once

/// @file parallel_dynamics.hpp
/// Latency-aligned stereo blending around caller-owned dynamics processing.

#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/signal/crossfade.hpp>
#include <pulp/signal/detail/audio_range.hpp>
#include <pulp/signal/path_latency_aligner.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace pulp::signal {

template <typename SampleType = float>
struct ParallelDynamicsConfigT {
    SampleType wet_mix = SampleType{0.5};
    CrossfadeGainLaw mix_law = CrossfadeGainLaw::EqualGain;
    SampleType output_gain_db = SampleType{0};
    std::size_t dry_latency_samples = 0;
    std::size_t wet_latency_samples = 0;
    std::size_t dry_tail_samples = 0;
    std::size_t wet_tail_samples = 0;
};

template <typename SampleType = float>
struct PreparedParallelDynamicsConfigT {
    ParallelDynamicsConfigT<SampleType> config{};
    SampleType dry_gain = SampleType{0.5};
    SampleType wet_gain = SampleType{0.5};
    SampleType output_gain = SampleType{1};
    std::size_t latency_samples = 0;
    std::size_t tail_samples = 0;
    std::uint64_t generation = 0;
};

/// Validate a complete control snapshot and derive its audio-rate gains.
///
/// Equal-gain is the linear law for correlated dry/processed signals. Equal-
/// power is available for decorrelated or deliberately phase-altered wet paths.
/// Both laws have exact dry-only and wet-only endpoints.
template <typename SampleType = float>
std::optional<PreparedParallelDynamicsConfigT<SampleType>>
prepare_parallel_dynamics_config(const ParallelDynamicsConfigT<SampleType>& config,
                                 std::size_t max_latency_samples) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "parallel dynamics reporting must be lock-free");
    constexpr SampleType kMinOutputGainDb = SampleType{-120};
    constexpr SampleType kMaxOutputGainDb = SampleType{24};

    if (!std::isfinite(config.wet_mix) || config.wet_mix < SampleType{0} ||
        config.wet_mix > SampleType{1} || !std::isfinite(config.output_gain_db) ||
        config.output_gain_db < kMinOutputGainDb ||
        config.output_gain_db > kMaxOutputGainDb ||
        config.dry_latency_samples > max_latency_samples ||
        config.wet_latency_samples > max_latency_samples ||
        (config.mix_law != CrossfadeGainLaw::EqualGain &&
         config.mix_law != CrossfadeGainLaw::EqualPower))
        return std::nullopt;

    PreparedParallelDynamicsConfigT<SampleType> result{};
    result.config = config;
    if (config.wet_mix == SampleType{0}) {
        result.dry_gain = SampleType{1};
        result.wet_gain = SampleType{0};
    } else if (config.wet_mix == SampleType{1}) {
        result.dry_gain = SampleType{0};
        result.wet_gain = SampleType{1};
    } else {
        crossfade_gains(config.wet_mix, config.mix_law,
                        result.dry_gain, result.wet_gain);
    }
    result.output_gain = units::db_to_linear(config.output_gain_db);
    if (!std::isfinite(result.output_gain)) return std::nullopt;

    result.latency_samples =
        std::max(config.dry_latency_samples, config.wet_latency_samples);
    const auto dry_compensation =
        result.latency_samples - config.dry_latency_samples;
    const auto wet_compensation =
        result.latency_samples - config.wet_latency_samples;
    if (config.dry_tail_samples >
            std::numeric_limits<std::size_t>::max() - dry_compensation ||
        config.wet_tail_samples >
            std::numeric_limits<std::size_t>::max() - wet_compensation)
        return std::nullopt;
    const auto aligned_dry_tail = config.dry_tail_samples + dry_compensation;
    const auto aligned_wet_tail = config.wet_tail_samples + wet_compensation;
    if (config.wet_mix == SampleType{0})
        result.tail_samples = aligned_dry_tail;
    else if (config.wet_mix == SampleType{1})
        result.tail_samples = aligned_wet_tail;
    else
        result.tail_samples = std::max(aligned_dry_tail, aligned_wet_tail);
    return result;
}

/// Blend true-stereo dry input with an already-processed true-stereo wet path.
///
/// The caller owns the compressor, expander, or upward-dynamics algorithm. This
/// class owns only two-path latency alignment and final gain staging. A complete
/// configuration is published by one control writer and adopted by the audio
/// reader at a block boundary. Live publication is latency-stable. Changing
/// latency requires `configure_latency_and_publish()` while audio is stopped;
/// it resets alignment history without moving unbounded work into `process()`.
///
/// Output channels may exactly alias their same-channel dry or wet input.
/// Partial overlap and every cross-channel overlap are rejected. The process
/// path is allocation-free after `prepare()`.
template <typename SampleType = float>
class ParallelDynamicsMixerT {
public:
    using Config = ParallelDynamicsConfigT<SampleType>;
    using PreparedConfig = PreparedParallelDynamicsConfigT<SampleType>;

    static_assert(std::is_floating_point_v<SampleType>);
    ParallelDynamicsMixerT() noexcept : published_(default_config()) {}

    bool prepare(std::size_t max_latency_samples, std::size_t max_block_size) {
        if (max_block_size == 0 ||
            max_block_size > std::numeric_limits<std::size_t>::max() / kStreamCount)
            return false;

        const auto scratch_samples = max_block_size * kStreamCount;
        auto replacement_scratch =
            std::unique_ptr<SampleType[]>(new (std::nothrow) SampleType[scratch_samples]);
        if (!replacement_scratch) return false;
        std::fill_n(replacement_scratch.get(), scratch_samples, SampleType{0});

        PathLatencyAlignerT<SampleType, 2, 2> replacement_aligner;
        if (!replacement_aligner.prepare(2, 2, max_latency_samples, max_block_size))
            return false;

        scratch_ = std::move(replacement_scratch);
        aligner_ = std::move(replacement_aligner);
        max_latency_samples_ = max_latency_samples;
        max_block_size_ = max_block_size;
        configured_dry_latency_ = 0;
        configured_wet_latency_ = 0;
        reported_latency_samples_.store(0, std::memory_order_relaxed);
        reported_tail_samples_.store(0, std::memory_order_relaxed);
        prepared_ = true;
        active_ = {};

        auto initial = default_config();
        initial.generation = next_generation();
        published_.write(initial);
        return true;
    }

    bool publish_config(const Config& config) noexcept {
        if (!prepared_) return false;
        auto prepared =
            prepare_parallel_dynamics_config(config, max_latency_samples_);
        if (!prepared || config.dry_latency_samples != configured_dry_latency_ ||
            config.wet_latency_samples != configured_wet_latency_)
            return false;
        prepared->generation = next_generation();
        published_.write(*prepared);
        return true;
    }

    /// Change alignment and publish a validated snapshot while audio is stopped.
    bool configure_latency_and_publish(const Config& config) noexcept {
        if (!prepared_) return false;
        auto prepared =
            prepare_parallel_dynamics_config(config, max_latency_samples_);
        if (!prepared) return false;
        const std::array<std::size_t, 2> latencies{
            config.dry_latency_samples, config.wet_latency_samples};
        if (!aligner_.configure_latencies(latencies)) return false;
        configured_dry_latency_ = config.dry_latency_samples;
        configured_wet_latency_ = config.wet_latency_samples;
        prepared->generation = next_generation();
        published_.write(*prepared);
        active_ = *prepared;
        publish_reporting(*prepared);
        return true;
    }

    bool process(const SampleType* const* dry,
                 const SampleType* const* wet,
                 SampleType* const* output,
                 std::size_t frames) noexcept {
        if (!validate_layout(dry, wet, output, frames)) return false;
        if (!sync_config()) return false;
        if (frames == 0) return true;

        std::array<const SampleType*, kStreamCount> inputs{
            dry[0], dry[1], wet[0], wet[1]};
        std::array<SampleType*, kStreamCount> aligned{};
        for (std::size_t stream = 0; stream < kStreamCount; ++stream)
            aligned[stream] = scratch_.get() + stream * max_block_size_;

        if (!aligner_.process(inputs.data(), inputs.size(), aligned.data(),
                              aligned.size(), frames))
            return false;

        for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
            const auto* aligned_dry = aligned[channel];
            const auto* aligned_wet = aligned[kChannelCount + channel];
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const auto dry_sample = finite_or_zero(aligned_dry[frame]);
                const auto wet_sample = finite_or_zero(aligned_wet[frame]);
                const auto mixed = (dry_sample * active_.dry_gain +
                                    wet_sample * active_.wet_gain) *
                                   active_.output_gain;
                output[channel][frame] = finite_or_zero(mixed);
            }
        }
        return true;
    }

    void reset() noexcept {
        if (!prepared_) return;
        if (!sync_config()) return;
        aligner_.reset();
        std::fill_n(scratch_.get(), max_block_size_ * kStreamCount, SampleType{0});
    }

    std::size_t latency_samples() const noexcept {
        return reported_latency_samples_.load(std::memory_order_acquire);
    }
    std::size_t tail_samples() const noexcept {
        return reported_tail_samples_.load(std::memory_order_acquire);
    }
    std::size_t max_block_size() const noexcept { return max_block_size_; }
    bool prepared() const noexcept { return prepared_; }

private:
    static constexpr std::size_t kChannelCount = 2;
    static constexpr std::size_t kStreamCount = 4;

    static PreparedConfig default_config() noexcept {
        return *prepare_parallel_dynamics_config(Config{}, 0);
    }

    std::uint64_t next_generation() noexcept {
        ++writer_generation_;
        if (writer_generation_ == 0) ++writer_generation_;
        return writer_generation_;
    }

    static SampleType finite_or_zero(SampleType sample) noexcept {
        return std::isfinite(sample) ? sample : SampleType{0};
    }

    bool sync_config() noexcept {
        const auto& latest = published_.read();
        if (latest.generation == active_.generation) return true;
        active_ = latest;
        publish_reporting(active_);
        return true;
    }

    void publish_reporting(const PreparedConfig& config) noexcept {
        reported_latency_samples_.store(config.latency_samples,
                                        std::memory_order_release);
        reported_tail_samples_.store(config.tail_samples,
                                     std::memory_order_release);
    }

    bool validate_layout(const SampleType* const* dry,
                         const SampleType* const* wet,
                         SampleType* const* output,
                         std::size_t frames) const noexcept {
        if (!prepared_ || frames > max_block_size_ || dry == nullptr ||
            wet == nullptr || output == nullptr)
            return false;
        for (std::size_t channel = 0; channel < kChannelCount; ++channel)
            if (dry[channel] == nullptr || wet[channel] == nullptr ||
                output[channel] == nullptr)
                return false;

        if (frames == 0) return true;
        const std::array<const SampleType*, 3> left{dry[0], wet[0], output[0]};
        const std::array<const SampleType*, 3> right{dry[1], wet[1], output[1]};
        for (const auto* left_range : left)
            for (const auto* right_range : right)
                if (detail::audio_ranges_overlap(left_range, right_range, frames))
                    return false;

        for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
            if (dry[channel] != wet[channel] &&
                detail::audio_ranges_overlap(dry[channel], wet[channel], frames))
                return false;
            if (output[channel] != dry[channel] &&
                detail::audio_ranges_overlap(output[channel], dry[channel], frames))
                return false;
            if (output[channel] != wet[channel] &&
                detail::audio_ranges_overlap(output[channel], wet[channel], frames))
                return false;
        }
        return true;
    }

    PathLatencyAlignerT<SampleType, 2, 2> aligner_;
    std::unique_ptr<SampleType[]> scratch_;
    pulp::runtime::TripleBuffer<PreparedConfig> published_;
    PreparedConfig active_{};
    std::size_t max_latency_samples_ = 0;
    std::size_t max_block_size_ = 0;
    std::size_t configured_dry_latency_ = 0;
    std::size_t configured_wet_latency_ = 0;
    std::atomic<std::size_t> reported_latency_samples_{0};
    std::atomic<std::size_t> reported_tail_samples_{0};
    std::uint64_t writer_generation_ = 0;
    bool prepared_ = false;
};

using ParallelDynamicsConfig = ParallelDynamicsConfigT<float>;
using ParallelDynamicsConfig64 = ParallelDynamicsConfigT<double>;
using PreparedParallelDynamicsConfig = PreparedParallelDynamicsConfigT<float>;
using PreparedParallelDynamicsConfig64 = PreparedParallelDynamicsConfigT<double>;
using ParallelDynamicsMixer = ParallelDynamicsMixerT<float>;
using ParallelDynamicsMixer64 = ParallelDynamicsMixerT<double>;

} // namespace pulp::signal
