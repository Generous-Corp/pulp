#pragma once

#include "detail/audio_range.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <vector>

namespace pulp::signal {

namespace detail {

template <std::size_t Inputs, std::size_t Outputs>
inline constexpr bool
    valid_audio_matrix_capacity = Inputs > 0 && Outputs > 0 &&
                                  Inputs <= std::numeric_limits<std::size_t>::max() / Outputs;

} // namespace detail

/// Raw applies the signed matrix exactly. NormalizePeak scales each output row
/// by max(1, sum(abs(gain))) so full-scale, arbitrarily correlated inputs cannot
/// exceed full scale. Neither policy clips the output.
enum class MatrixHeadroomPolicy { Raw, NormalizePeak };

/// Fixed-capacity signed audio routing matrix with sample-continuous gain ramps.
/// prepare() is control-thread-only and owns the sole allocation. All setters,
/// reset(), and process() are allocation-free afterward. Gain automation is
/// deterministic across block partitions. Non-finite gains become zero and are
/// counted; non-finite audio follows normal IEEE propagation.
///
/// Input/output aliasing, including partial overlap, is supported because every
/// input block is copied to prepared scratch before any output is written.
/// Output buffers themselves must not overlap each other.
template <typename SampleType = float, std::size_t MaxInputs = 16, std::size_t MaxOutputs = 16,
          typename Allocator = std::allocator<SampleType>>
    requires detail::valid_audio_matrix_capacity<MaxInputs, MaxOutputs>
class AudioMatrixMixerT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);
    static constexpr std::size_t max_inputs = MaxInputs;
    static constexpr std::size_t max_outputs = MaxOutputs;
    static constexpr SampleType max_abs_gain = SampleType{64};

    explicit AudioMatrixMixerT(const Allocator& allocator = Allocator{}) : scratch_(allocator) {}

    bool prepare(std::size_t max_block_size) {
        if (max_block_size == 0 ||
            max_block_size > std::numeric_limits<std::size_t>::max() / MaxInputs)
            return false;
        const auto scratch_samples = MaxInputs * max_block_size;
        if (scratch_samples > scratch_.max_size())
            return false;
        std::vector<SampleType, Allocator> replacement(scratch_.get_allocator());
        try {
            replacement.assign(scratch_samples, SampleType{0});
        } catch (const std::bad_alloc&) {
            return false;
        }
        scratch_.swap(replacement);
        max_block_size_ = max_block_size;
        settle_ramps();
        return true;
    }

    bool set_dimensions(std::size_t inputs, std::size_t outputs) noexcept {
        if (inputs == 0 || outputs == 0 || inputs > MaxInputs || outputs > MaxOutputs)
            return false;
        if (inputs == inputs_ && outputs == outputs_)
            return true;
        inputs_ = inputs;
        outputs_ = outputs;
        cells_.fill({});
        return true;
    }

    /// Headroom policy is structural, not automatable: change it while stopped
    /// or accept a block-boundary level change.
    void set_headroom_policy(MatrixHeadroomPolicy policy) noexcept {
        policy_ = policy;
    }
    MatrixHeadroomPolicy headroom_policy() const noexcept {
        return policy_;
    }

    bool set_gain(std::size_t output, std::size_t input, SampleType gain) noexcept {
        return set_gain_ramped(output, input, gain, 0);
    }

    /// The old value is used for the first sample. After ramp_samples frames the
    /// internal state equals the target, which is emitted on the following
    /// frame. Retargeting starts from the gain that would be used for the next
    /// sample, so no discontinuity is introduced.
    bool set_gain_ramped(std::size_t output, std::size_t input, SampleType gain,
                         std::size_t ramp_samples) noexcept {
        if (output >= outputs_ || input >= inputs_)
            return false;
        if (!std::isfinite(gain)) {
            gain = SampleType{0};
            ++nonfinite_gain_count_;
        }
        if (std::abs(gain) > max_abs_gain) {
            ++out_of_range_gain_count_;
            return false;
        }
        auto& cell = cells_[index(output, input)];
        cell.start = cell.current;
        cell.target = gain;
        cell.total = ramp_samples;
        cell.elapsed = 0;
        if (ramp_samples == 0) {
            cell.current = gain;
            cell.start = gain;
        }
        return true;
    }

    SampleType gain(std::size_t output, std::size_t input) const noexcept {
        return output < outputs_ && input < inputs_ ? cells_[index(output, input)].current
                                                    : SampleType{0};
    }
    SampleType target_gain(std::size_t output, std::size_t input) const noexcept {
        return output < outputs_ && input < inputs_ ? cells_[index(output, input)].target
                                                    : SampleType{0};
    }
    std::size_t input_count() const noexcept {
        return inputs_;
    }
    std::size_t output_count() const noexcept {
        return outputs_;
    }
    std::size_t max_block_size() const noexcept {
        return max_block_size_;
    }
    std::size_t nonfinite_gain_count() const noexcept {
        return nonfinite_gain_count_;
    }
    std::size_t out_of_range_gain_count() const noexcept {
        return out_of_range_gain_count_;
    }

    /// Clears scratch and settles active ramps at their targets. Matrix shape,
    /// gains, policy, and the non-finite diagnostic count are preserved.
    void reset() noexcept {
        std::fill(scratch_.begin(), scratch_.end(), SampleType{0});
        settle_ramps();
    }

    bool process(const SampleType* const* inputs, std::size_t input_count,
                 SampleType* const* outputs, std::size_t output_count,
                 std::size_t frames) noexcept {
        if (inputs == nullptr || outputs == nullptr || input_count != inputs_ ||
            output_count != outputs_ || frames > max_block_size_ || scratch_.empty())
            return false;
        for (std::size_t input = 0; input < inputs_; ++input)
            if (inputs[input] == nullptr)
                return false;
        for (std::size_t output = 0; output < outputs_; ++output) {
            if (outputs[output] == nullptr)
                return false;
            for (std::size_t other = output + 1; other < outputs_; ++other)
                if (detail::audio_ranges_overlap(outputs[output], outputs[other], frames))
                    return false;
        }

        for (std::size_t input = 0; input < inputs_; ++input)
            std::copy_n(inputs[input], frames, scratch_.data() + input * max_block_size_);

        std::array<SampleType, MaxOutputs> row_scales{};
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (policy_ == MatrixHeadroomPolicy::NormalizePeak) {
                for (std::size_t output = 0; output < outputs_; ++output) {
                    SampleType absolute_sum{};
                    for (std::size_t input = 0; input < inputs_; ++input)
                        absolute_sum += std::abs(cells_[index(output, input)].current);
                    row_scales[output] = SampleType{1} / std::max(SampleType{1}, absolute_sum);
                }
            } else {
                std::fill_n(row_scales.begin(), outputs_, SampleType{1});
            }

            for (std::size_t output = 0; output < outputs_; ++output) {
                SampleType sum{};
                for (std::size_t input = 0; input < inputs_; ++input)
                    sum += scratch_[input * max_block_size_ + frame] *
                           cells_[index(output, input)].current;
                outputs[output][frame] = sum * row_scales[output];
            }
            advance_ramps();
        }
        return true;
    }

  private:
    struct GainCell {
        SampleType current{};
        SampleType start{};
        SampleType target{};
        std::size_t total = 0;
        std::size_t elapsed = 0;
    };

    static constexpr std::size_t index(std::size_t output, std::size_t input) noexcept {
        return output * MaxInputs + input;
    }

    void advance_ramps() noexcept {
        for (auto& cell : cells_) {
            if (cell.elapsed >= cell.total)
                continue;
            ++cell.elapsed;
            if (cell.elapsed == cell.total) {
                cell.current = cell.target;
                continue;
            }
            using ProgressType =
                std::conditional_t<(sizeof(SampleType) < sizeof(double)), double, long double>;
            const auto progress =
                static_cast<ProgressType>(cell.elapsed) / static_cast<ProgressType>(cell.total);
            const auto start = static_cast<ProgressType>(cell.start);
            const auto target = static_cast<ProgressType>(cell.target);
            auto current = static_cast<SampleType>(start + (target - start) * progress);
            if ((cell.start < cell.target && current >= cell.target) ||
                (cell.start > cell.target && current <= cell.target))
                current = std::nextafter(cell.target, cell.start);
            cell.current = current;
        }
    }

    void settle_ramps() noexcept {
        for (auto& cell : cells_) {
            cell.current = cell.target;
            cell.start = cell.target;
            cell.total = 0;
            cell.elapsed = 0;
        }
    }

    std::size_t inputs_ = 0;
    std::size_t outputs_ = 0;
    std::size_t max_block_size_ = 0;
    std::size_t nonfinite_gain_count_ = 0;
    std::size_t out_of_range_gain_count_ = 0;
    MatrixHeadroomPolicy policy_ = MatrixHeadroomPolicy::Raw;
    std::array<GainCell, MaxInputs * MaxOutputs> cells_{};
    std::vector<SampleType, Allocator> scratch_;
};

using AudioMatrixMixer = AudioMatrixMixerT<float>;
using AudioMatrixMixer64 = AudioMatrixMixerT<double>;

} // namespace pulp::signal
