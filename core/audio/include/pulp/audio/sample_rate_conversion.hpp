#pragma once

#include <pulp/audio/audio_file.hpp>
#include <pulp/signal/sinc_resampler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <vector>

namespace pulp::audio {

class SampleRateConversionBuilder;

// Converter byte budgets include conservative allocator bookkeeping for every
// retained allocation plus the shared-ownership control block. Payloads use
// actual container capacity once construction completes.
inline constexpr std::uint64_t kConverterAllocationOverhead = 4u * sizeof(void*);
inline constexpr std::uint64_t kConverterSharedOwnershipBytes = 8u * sizeof(void*);

/// Immutable, allocation-free read kernel after control-thread construction.
///
/// `cutoff` is normalized to the source Nyquist. Construction allocates the
/// phase table and is not audio-thread safe; read() is bounded and RT-safe.
class PreparedSampleRateConversion {
  public:
    // Keep at least four sinc zero crossings on each side of the read point.
    // Narrower fixed kernels cannot provide a meaningful decimation stopband.
    static constexpr double kMinimumSupportedCutoff = 0.125;

    explicit PreparedSampleRateConversion(double cutoff) {
        if (!std::isfinite(cutoff))
            cutoff = 1.0;
        if (cutoff >= kMinimumSupportedCutoff)
            kernel_.build(32, 512, 10.0, cutoff);
    }

    bool ready() const noexcept {
        return kernel_.ready();
    }

    static std::uint64_t estimated_prepared_bytes() noexcept;

    std::uint64_t prepared_bytes() const noexcept {
        return sizeof(*this) + kConverterAllocationOverhead + kConverterSharedOwnershipBytes +
               kernel_.prepared_bytes() +
               (kernel_.prepared_bytes() != 0 ? kConverterAllocationOverhead : 0u);
    }

    float read(std::span<const float> source, double position) const noexcept {
        if (source.empty() ||
            source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            !std::isfinite(position))
            return 0.0f;
        position = std::clamp(position, 0.0, static_cast<double>(source.size() - 1));
        return kernel_.read(source.data(), static_cast<int>(source.size()), position);
    }

  private:
    friend class SampleRateConversionBuilder;
    struct IncrementalBuildTag {};

    PreparedSampleRateConversion(IncrementalBuildTag, double cutoff) {
        if (std::isfinite(cutoff) && cutoff >= kMinimumSupportedCutoff)
            kernel_.begin_build(32, 512, 10.0, cutoff);
    }

    bool build_step() {
        return kernel_.build_step();
    }

    signal::SincResampler kernel_;
};

class SampleRateConversionBuilder {
  public:
    explicit SampleRateConversionBuilder(double cutoff) {
        if (!std::isfinite(cutoff) ||
            cutoff < PreparedSampleRateConversion::kMinimumSupportedCutoff)
            return;
        result_.reset(new PreparedSampleRateConversion(
            PreparedSampleRateConversion::IncrementalBuildTag{}, cutoff));
    }

    bool valid() const noexcept {
        return result_ != nullptr;
    }

    bool step() {
        return !result_ || result_->build_step();
    }

    std::shared_ptr<const PreparedSampleRateConversion> take() {
        return result_ && result_->ready() ? std::move(result_) : nullptr;
    }

  private:
    std::shared_ptr<PreparedSampleRateConversion> result_;
};

/// Prepared reconstruction kernels for a runtime-varying read rate.
///
/// Construction is control-thread only. `read()` chooses the narrowest
/// prebuilt cutoff that does not exceed the source-position step, so host-tempo
/// playback can decimate without allocating or rebuilding a filter on the audio
/// thread. Steps beyond the finite prepared bank fail closed to silence instead
/// of selecting a filter whose cutoff would permit aliases.
class PreparedVariableRateConversion;

class VariableRateConversionBuilder {
  public:
    /// Invalid/null sources and out-of-range slices produce an invalid builder;
    /// step() then completes immediately and take() returns null.
    VariableRateConversionBuilder(std::shared_ptr<const AudioFileData> source,
                                  std::uint64_t source_start, std::uint64_t source_frames);

    bool step();
    bool valid() const noexcept {
        return result_ != nullptr;
    }
    std::shared_ptr<const PreparedVariableRateConversion> take();

  private:
    std::shared_ptr<PreparedVariableRateConversion> result_;
    signal::SincResampler decimator_;
    std::size_t kernel_level_ = 0;
    bool decimator_started_ = false;
    std::size_t level_ = 0;
    std::size_t channel_ = 0;
    std::size_t frame_ = 0;
};

class PreparedVariableRateConversion {
  public:
    static constexpr std::size_t kLinearCutoffLevels = 32;
    static constexpr std::size_t kCutoffLevels = kLinearCutoffLevels + 1;
    static constexpr std::size_t kPyramidLevels = 19;
    static constexpr std::size_t kPyramidBlockSamples = 1'024;
    static constexpr double kMaximumSourceFramesPerOutputFrame = 1'048'576.0;

    static std::shared_ptr<const PreparedVariableRateConversion>
    build(std::shared_ptr<const AudioFileData> source, std::uint64_t source_start,
          std::uint64_t source_frames) {
        VariableRateConversionBuilder builder(std::move(source), source_start, source_frames);
        while (!builder.step()) {
        }
        return builder.take();
    }

    static std::optional<std::uint64_t> prepared_bytes(std::uint64_t source_frames,
                                                       std::size_t channels) noexcept;

    std::uint64_t prepared_bytes() const noexcept {
        return prepared_bytes_;
    }

    bool matches_source_slice(const std::shared_ptr<const AudioFileData>& source,
                              std::uint64_t source_start,
                              std::uint64_t source_frames) const noexcept {
        return source_ == source && source_start_ == source_start &&
               source_frames_ == source_frames;
    }

    float read(std::size_t channel, double position,
               double source_frames_per_output_frame) const noexcept {
        if (!source_ || channel >= source_->channels.size() || !std::isfinite(position) ||
            !std::isfinite(source_frames_per_output_frame) ||
            source_frames_per_output_frame <= 0.0 ||
            source_frames_per_output_frame > kMaximumSourceFramesPerOutputFrame)
            return 0.0f;

        const auto pyramid_level =
            source_frames_per_output_frame <= 2.0
                ? std::size_t{0}
                : std::min(kPyramidLevels, static_cast<std::size_t>(std::ceil(
                                               std::log2(source_frames_per_output_frame / 2.0))));
        const auto scale = std::exp2(static_cast<double>(pyramid_level));
        const auto residual_step = source_frames_per_output_frame / scale;
        const auto required_cutoff = std::min(1.0, 1.0 / residual_step);
        const auto kernel_level = static_cast<std::size_t>(
            std::ceil((1.0 - required_cutoff) * 2.0 * static_cast<double>(kLinearCutoffLevels)));
        if (kernel_level >= kernels_.size())
            return 0.0f;

        position /= scale;
        if (pyramid_level == 0) {
            const auto source = std::span<const float>(source_->channels[channel])
                                    .subspan(static_cast<std::size_t>(source_start_),
                                             static_cast<std::size_t>(source_frames_));
            if (source.empty() ||
                source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                return 0.0f;
            return kernels_[kernel_level].read(source.data(), static_cast<int>(source.size()),
                                               position);
        }
        return read_chunked(kernels_[kernel_level], pyramid_[pyramid_level - 1u][channel],
                            position);
    }

  private:
    friend class VariableRateConversionBuilder;

    struct ChunkedSamples {
        static constexpr std::size_t kFanout = 64;
        using SampleBlock = std::array<float, kPyramidBlockSamples>;

        struct LeafPage {
            ~LeafPage() {
                for (auto* block : blocks)
                    delete block;
            }
            std::array<SampleBlock*, kFanout> blocks{};
        };

        struct MiddlePage {
            ~MiddlePage() {
                for (auto* leaf : leaves)
                    delete leaf;
            }
            std::array<LeafPage*, kFanout> leaves{};
        };

        struct UpperPage {
            ~UpperPage() {
                for (auto* middle : middles)
                    delete middle;
            }
            std::array<MiddlePage*, kFanout> middles{};
        };

        ChunkedSamples() = default;
        ChunkedSamples(const ChunkedSamples&) = delete;
        ChunkedSamples& operator=(const ChunkedSamples&) = delete;

        ~ChunkedSamples() {
            for (auto* upper : uppers)
                delete upper;
        }

        bool push_back(float sample) noexcept {
            auto block_index = size / kPyramidBlockSamples;
            const auto block_slot = block_index % kFanout;
            block_index /= kFanout;
            const auto leaf_slot = block_index % kFanout;
            block_index /= kFanout;
            const auto middle_slot = block_index % kFanout;
            const auto upper_slot = block_index / kFanout;
            if (upper_slot >= kFanout)
                return false;
            auto*& upper = uppers[upper_slot];
            if (!upper) {
                upper = new (std::nothrow) UpperPage;
                if (!upper)
                    return false;
                ++upper_count;
            }
            auto*& middle = upper->middles[middle_slot];
            if (!middle) {
                middle = new (std::nothrow) MiddlePage;
                if (!middle)
                    return false;
                ++middle_count;
            }
            auto*& leaf = middle->leaves[leaf_slot];
            if (!leaf) {
                leaf = new (std::nothrow) LeafPage;
                if (!leaf)
                    return false;
                ++leaf_count;
            }
            auto*& block = leaf->blocks[block_slot];
            if (!block) {
                block = new (std::nothrow) SampleBlock;
                if (!block)
                    return false;
                ++block_count;
            }
            (*block)[size % kPyramidBlockSamples] = sample;
            ++size;
            return true;
        }

        float operator[](std::size_t index) const noexcept {
            auto block_index = index / kPyramidBlockSamples;
            const auto block_slot = block_index % kFanout;
            block_index /= kFanout;
            const auto leaf_slot = block_index % kFanout;
            block_index /= kFanout;
            const auto middle_slot = block_index % kFanout;
            const auto upper_slot = block_index / kFanout;
            return (*uppers[upper_slot]
                         ->middles[middle_slot]
                         ->leaves[leaf_slot]
                         ->blocks[block_slot])[index % kPyramidBlockSamples];
        }

        std::uint64_t allocated_bytes() const noexcept {
            const auto allocations = upper_count + middle_count + leaf_count + block_count;
            return upper_count * sizeof(UpperPage) + middle_count * sizeof(MiddlePage) +
                   leaf_count * sizeof(LeafPage) + block_count * sizeof(SampleBlock) +
                   allocations * kConverterAllocationOverhead;
        }

        std::array<UpperPage*, kFanout> uppers{};
        std::size_t size = 0;
        std::uint64_t upper_count = 0;
        std::uint64_t middle_count = 0;
        std::uint64_t leaf_count = 0;
        std::uint64_t block_count = 0;
    };

    static float read_chunked(const signal::SincResampler& kernel, const ChunkedSamples& source,
                              double position) noexcept {
        if (source.size == 0 || !std::isfinite(position))
            return 0.0f;
        position = std::clamp(position, 0.0, static_cast<double>(source.size - 1u));
        const auto center = static_cast<std::int64_t>(std::floor(position));
        const auto fraction = position - static_cast<double>(center);
        std::array<float, 64> neighborhood{};
        const auto taps = static_cast<std::size_t>(kernel.taps());
        for (std::size_t tap = 0; tap < taps; ++tap) {
            const auto index =
                std::clamp(center + static_cast<std::int64_t>(tap) - kernel.half_width() + 1,
                           std::int64_t{0}, static_cast<std::int64_t>(source.size - 1u));
            neighborhood[tap] = source[static_cast<std::size_t>(index)];
        }
        return kernel.apply(neighborhood.data(), fraction);
    }

    PreparedVariableRateConversion(std::shared_ptr<const AudioFileData> source,
                                   std::uint64_t source_start, std::uint64_t source_frames)
        : source_(std::move(source)), source_start_(source_start), source_frames_(source_frames) {
        const auto bytes = prepared_bytes(source_frames_, source_ ? source_->channels.size() : 0);
        prepared_bytes_ = bytes.value_or(0);
    }

    void refresh_prepared_bytes() noexcept {
        std::uint64_t bytes =
            sizeof(*this) + kConverterAllocationOverhead + kConverterSharedOwnershipBytes;
        for (const auto& kernel : kernels_)
            bytes += kernel.prepared_bytes() +
                     (kernel.prepared_bytes() != 0 ? kConverterAllocationOverhead : 0u);
        const auto channels = source_->channels.size();
        for (const auto& level : pyramid_) {
            if (!level)
                continue;
            bytes += channels * sizeof(ChunkedSamples) + kConverterAllocationOverhead;
            for (std::size_t channel = 0; channel < channels; ++channel)
                bytes += level[channel].allocated_bytes();
        }
        prepared_bytes_ = bytes;
    }

    std::shared_ptr<const AudioFileData> source_;
    std::uint64_t source_start_ = 0;
    std::uint64_t source_frames_ = 0;
    std::uint64_t prepared_bytes_ = 0;
    std::array<std::unique_ptr<ChunkedSamples[]>, kPyramidLevels> pyramid_;
    std::array<signal::SincResampler, kCutoffLevels> kernels_;
};

inline std::uint64_t PreparedSampleRateConversion::estimated_prepared_bytes() noexcept {
    constexpr auto kernel_table_bytes = (512u + 1u) * (2u * 32u) * sizeof(float);
    return sizeof(PreparedSampleRateConversion) + kConverterAllocationOverhead +
           kConverterSharedOwnershipBytes + kernel_table_bytes + kConverterAllocationOverhead;
}

inline std::optional<std::uint64_t>
PreparedVariableRateConversion::prepared_bytes(std::uint64_t source_frames,
                                               std::size_t channels) noexcept {
    if (source_frames == 0 ||
        source_frames > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        channels == 0)
        return std::nullopt;
    constexpr std::uint64_t kernel_table_bytes =
        kCutoffLevels * (128u + 1u) * (2u * 16u) * sizeof(float);
    std::uint64_t bytes = sizeof(PreparedVariableRateConversion) + kConverterAllocationOverhead +
                          kConverterSharedOwnershipBytes + kernel_table_bytes +
                          kCutoffLevels * kConverterAllocationOverhead;
    auto level_frames = source_frames;
    for (std::size_t level = 0; level < kPyramidLevels; ++level) {
        level_frames = level_frames / 2u + level_frames % 2u;
        const auto ceil_count = [](std::uint64_t value, std::uint64_t divisor) noexcept {
            return value / divisor + (value % divisor != 0 ? 1u : 0u);
        };
        const auto block_count = ceil_count(level_frames, kPyramidBlockSamples);
        const auto leaf_count = ceil_count(block_count, ChunkedSamples::kFanout);
        const auto middle_count = ceil_count(leaf_count, ChunkedSamples::kFanout);
        const auto upper_count = ceil_count(middle_count, ChunkedSamples::kFanout);
        const auto allocation_count = block_count + leaf_count + middle_count + upper_count;
        const auto channel_bytes = sizeof(ChunkedSamples) +
                                   block_count * sizeof(ChunkedSamples::SampleBlock) +
                                   leaf_count * sizeof(ChunkedSamples::LeafPage) +
                                   middle_count * sizeof(ChunkedSamples::MiddlePage) +
                                   upper_count * sizeof(ChunkedSamples::UpperPage) +
                                   allocation_count * kConverterAllocationOverhead;
        if (bytes > std::numeric_limits<std::uint64_t>::max() - kConverterAllocationOverhead)
            return std::nullopt;
        const auto available =
            std::numeric_limits<std::uint64_t>::max() - bytes - kConverterAllocationOverhead;
        if (channels > available / channel_bytes)
            return std::nullopt;
        bytes += channel_bytes * channels + kConverterAllocationOverhead;
    }
    return bytes;
}

inline VariableRateConversionBuilder::VariableRateConversionBuilder(
    std::shared_ptr<const AudioFileData> source, std::uint64_t source_start,
    std::uint64_t source_frames) {
    if (!source || source_frames == 0 || source_frames > std::numeric_limits<int>::max() ||
        source->channels.empty())
        return;
    const auto total_frames = source->num_frames();
    if (total_frames == 0 || source_start > total_frames ||
        source_frames > total_frames - source_start ||
        !std::all_of(
            source->channels.begin(), source->channels.end(),
            [total_frames](const auto& channel) { return channel.size() == total_frames; }))
        return;
    result_.reset(
        new PreparedVariableRateConversion(std::move(source), source_start, source_frames));
    result_->kernels_[0].begin_build(16, 128, 10.0, 1.0);
}

inline bool VariableRateConversionBuilder::step() {
    if (!result_ || level_ == PreparedVariableRateConversion::kPyramidLevels)
        return true;
    if (kernel_level_ < PreparedVariableRateConversion::kCutoffLevels) {
        if (!result_->kernels_[kernel_level_].build_step())
            return false;
        ++kernel_level_;
        if (kernel_level_ < PreparedVariableRateConversion::kCutoffLevels) {
            const auto cutoff =
                1.0 - static_cast<double>(kernel_level_) /
                          (2.0 * static_cast<double>(
                                     PreparedVariableRateConversion::kLinearCutoffLevels));
            result_->kernels_[kernel_level_].begin_build(16, 128, 10.0, cutoff);
        }
        return false;
    }
    if (!decimator_started_) {
        decimator_.begin_build(32, 128, 10.0, 0.5);
        decimator_started_ = true;
        return false;
    }
    if (!decimator_.build_step())
        return false;
    if (!result_->pyramid_[level_]) {
        auto* channels = new (std::nothrow)
            PreparedVariableRateConversion::ChunkedSamples[result_->source_->channels.size()];
        if (!channels) {
            result_.reset();
            return true;
        }
        result_->pyramid_[level_].reset(channels);
        return false;
    }
    auto& output = result_->pyramid_[level_][channel_];
    const auto source_frames =
        level_ == 0 ? result_->source_frames_ : result_->pyramid_[level_ - 1u][channel_].size;
    const auto output_frames = (source_frames + 1u) / 2u;
    if (frame_ < output_frames) {
        const auto position = static_cast<double>(frame_ * 2u);
        const auto sample =
            level_ == 0 ? decimator_.read(result_->source_->channels[channel_].data() +
                                              static_cast<std::size_t>(result_->source_start_),
                                          static_cast<int>(result_->source_frames_), position)
                        : PreparedVariableRateConversion::read_chunked(
                              decimator_, result_->pyramid_[level_ - 1u][channel_], position);
        if (!output.push_back(sample)) {
            result_.reset();
            return true;
        }
        ++frame_;
        return false;
    }
    frame_ = 0;
    ++channel_;
    if (channel_ == result_->source_->channels.size()) {
        channel_ = 0;
        ++level_;
    }
    return level_ == PreparedVariableRateConversion::kPyramidLevels;
}

inline std::shared_ptr<const PreparedVariableRateConversion> VariableRateConversionBuilder::take() {
    if (level_ != PreparedVariableRateConversion::kPyramidLevels)
        return nullptr;
    result_->refresh_prepared_bytes();
    return std::move(result_);
}

} // namespace pulp::audio
