#pragma once

#include <pulp/audio/buffer.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::audio {

inline constexpr double kSampleHeritageCyclicStretchMinimumFactor = 0.25;
inline constexpr double kSampleHeritageCyclicStretchMaximumFactor = 20.0;

enum class SampleHeritageLiveCyclicShuffle : std::uint8_t {
    Identity,
    FisherYates,
};

enum class SampleHeritageLiveCyclicStatus : std::uint8_t {
    Ok,
    NotPrepared,
    InvalidConfiguration,
    SizeOverflow,
    AllocationFailed,
    InvalidDimensions,
    InputFrameMismatch,
    SourceUnavailable,
};

struct SampleHeritageLiveCyclicConfig {
    double factor = 1.0;
    std::size_t cycle_samples = 256;
    std::size_t crossfade_samples = 16;
    std::size_t shuffle_divisions = 1;
    bool linked_channels = true;
    std::uint64_t seed = 0;
    SampleHeritageLiveCyclicShuffle shuffle = SampleHeritageLiveCyclicShuffle::Identity;
    std::size_t max_block_samples = 512;
    std::size_t channel_count = 2;
};

struct SampleHeritageLiveCyclicResources {
    SampleHeritageLiveCyclicStatus status = SampleHeritageLiveCyclicStatus::InvalidConfiguration;
    std::size_t maximum_input_frames = 0;
    std::size_t ring_capacity_frames = 0;
    std::size_t persistent_bytes = 0;
    std::size_t scratch_bytes = 0;

    bool valid() const noexcept {
        return status == SampleHeritageLiveCyclicStatus::Ok;
    }
};

struct SampleHeritageLiveCyclicPlan {
    SampleHeritageLiveCyclicStatus status = SampleHeritageLiveCyclicStatus::NotPrepared;
    std::size_t output_frames = 0;
    std::size_t input_frames = 0;
    std::size_t cold_lookahead_frames = 0;

    bool valid() const noexcept {
        return status == SampleHeritageLiveCyclicStatus::Ok;
    }
};

struct SampleHeritageLiveCyclicRngContinuation {
    std::uint64_t seed = 0;
    std::uint64_t next_cycle_index = 0;
};

/// Fixed-capacity cyclic resynthesis for realtime sample playback.
///
/// The caller supplies exactly plan().input_frames new source frames, in source
/// order, for each output block. The processor consumes 1/factor source frames
/// per output frame over time and retains the bounded neighborhood needed by
/// its cyclic reads. prepare() is the only allocating operation.
class SampleHeritageLiveCyclicStretch {
  public:
    static constexpr std::size_t kInterpolatorGuardFrames = 2;

    SampleHeritageLiveCyclicStretch() = default;
    SampleHeritageLiveCyclicStretch(const SampleHeritageLiveCyclicStretch&) = delete;
    SampleHeritageLiveCyclicStretch& operator=(const SampleHeritageLiveCyclicStretch&) = delete;
    SampleHeritageLiveCyclicStretch(SampleHeritageLiveCyclicStretch&&) = delete;
    SampleHeritageLiveCyclicStretch& operator=(SampleHeritageLiveCyclicStretch&&) = delete;

    static SampleHeritageLiveCyclicResources
    resources_for(const SampleHeritageLiveCyclicConfig& config) noexcept;

    SampleHeritageLiveCyclicStatus prepare(const SampleHeritageLiveCyclicConfig& config) noexcept;
    void release() noexcept;
    void reset() noexcept;
    void reset_with_rng_continuation(SampleHeritageLiveCyclicRngContinuation continuation) noexcept;

    bool prepared() const noexcept {
        return prepared_;
    }
    bool exact_bypass() const noexcept {
        return exact_bypass_;
    }
    double source_frames_per_output_frame() const noexcept {
        return source_ratio_;
    }
    std::size_t cold_lookahead_frames() const noexcept {
        return cold_lookahead_;
    }
    const SampleHeritageLiveCyclicResources& resources() const noexcept {
        return resources_;
    }

    SampleHeritageLiveCyclicPlan plan(std::size_t output_frames) const noexcept;
    SampleHeritageLiveCyclicStatus process(BufferView<const float> input,
                                           BufferView<float> output) noexcept;
    SampleHeritageLiveCyclicStatus process(BufferView<const float> input,
                                           BufferView<float> output,
                                           std::size_t valid_input_frames,
                                           bool end_of_source) noexcept;
    std::size_t last_valid_output_frames() const noexcept {
        return last_valid_output_frames_;
    }
    std::uint64_t remaining_output_frames() const noexcept {
        return source_ended_ && target_output_frames_ > rendered_output_frames_
            ? target_output_frames_ - rendered_output_frames_
            : 0;
    }

    bool division_permutation(std::uint64_t cycle_index, std::size_t channel,
                              std::span<std::uint32_t> destination) const noexcept;
    SampleHeritageLiveCyclicRngContinuation capture_next_cycle_rng_continuation() const noexcept;

  private:
    static constexpr std::size_t kMaximumCycleSamples = 1u << 20u;
    static constexpr std::size_t kMaximumDivisions = 1024;
    static constexpr std::size_t kMaximumChannels = 64;

    bool fill_permutation(std::uint64_t cycle_index, std::size_t channel,
                          std::uint32_t* destination) const noexcept;
    std::size_t mapped_cycle_offset(std::size_t phase, std::uint64_t cycle_index,
                                    std::size_t channel, bool& join_crossfade,
                                    std::size_t& alternate_offset, float& join_weight) noexcept;
    float read_linear(std::size_t channel, double position, bool& ok) const noexcept;
    void write_input(BufferView<const float> input) noexcept;
    std::uint64_t source_anchor(std::uint64_t cycle_index, bool& overflow) const noexcept;
    std::uint64_t required_source_through(std::uint64_t first_output, std::uint64_t output_frames,
                                          bool& overflow) const noexcept;
    std::uint32_t* permutation_slot(std::size_t channel) noexcept;

    SampleHeritageLiveCyclicConfig config_{};
    SampleHeritageLiveCyclicResources resources_{};
    std::vector<float> ring_;
    std::vector<std::uint32_t> permutation_;
    double source_ratio_ = 1.0;
    std::size_t cold_lookahead_ = 0;
    std::uint64_t accepted_source_frames_ = 0;
    std::uint64_t rendered_output_frames_ = 0;
    std::uint64_t real_source_frames_ = 0;
    std::uint64_t target_output_frames_ = 0;
    std::uint64_t cycle_index_offset_ = 0;
    std::uint64_t permutation_cycle_ = static_cast<std::uint64_t>(-1);
    bool prepared_ = false;
    bool exact_bypass_ = false;
    bool source_ended_ = false;
    std::size_t last_valid_output_frames_ = 0;
};

} // namespace pulp::audio
