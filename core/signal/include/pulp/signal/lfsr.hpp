#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pulp::signal {

/// Configurable Fibonacci linear-feedback shift register with a weighted
/// register-to-output projection.
///
/// The register shifts toward the most-significant bit. On each `clock()`, the
/// new bit is the parity of `state & feedback_mask`, optionally XOR-ed with an
/// external bit. Bit weights are expressed directly in caller-defined output
/// units; `weighted_output()` is `offset + sum(weight[i])` for every high bit.
/// Its exact range is therefore reported by `minimum_output()` and
/// `maximum_output()`.
///
/// Length is 2..32 bits. A zero seed is intentionally legal and absorbing
/// unless an external high bit is clocked; this preserves Rungler patch
/// semantics. A maximal-period sequence requires a nonzero seed and a
/// primitive-polynomial feedback mask chosen by the caller. The default mask
/// 0x8e is audited by the direct full-period oracle as a primitive 8-bit
/// configuration; arbitrary caller masks intentionally make no period claim.
///
/// RT contract: all methods are bounded scalar/array work with no allocation,
/// locks, I/O, or hidden entropy. `reset()` restores the public seed exactly.
/// Latency and tail are both zero samples.
template <typename SampleType = float> class LfsrT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    static constexpr int kMinLength = 2;
    static constexpr int kMaxLength = 32;
    static constexpr int kDefaultLength = 8;
    static constexpr std::uint32_t kDefaultFeedbackMask = 0x8Eu;
    static constexpr std::uint32_t kDefaultSeed = 0b10110100u;

    bool configure(int length, std::uint32_t feedback_mask, std::uint32_t seed) noexcept {
        if (!valid_length(length) || !mask_fits(feedback_mask, length) ||
            !projection_is_finite_for(length))
            return false;
        length_ = length;
        feedback_mask_ = feedback_mask;
        seed_ = seed & register_mask();
        state_ = seed_;
        return true;
    }

    bool set_length(int length) noexcept {
        if (!valid_length(length) || !projection_is_finite_for(length))
            return false;
        length_ = length;
        feedback_mask_ &= register_mask();
        seed_ &= register_mask();
        state_ = seed_;
        return true;
    }

    bool set_feedback_mask(std::uint32_t mask) noexcept {
        if (!mask_fits(mask, length_))
            return false;
        feedback_mask_ = mask;
        return true;
    }

    void set_seed(std::uint32_t seed) noexcept {
        seed_ = seed & register_mask();
        reset();
    }

    bool set_weight(std::size_t bit, SampleType weight) noexcept {
        if (bit >= static_cast<std::size_t>(length_) || !std::isfinite(static_cast<double>(weight)))
            return false;
        const SampleType previous = weights_[bit];
        weights_[bit] = weight;
        if (!projection_is_finite()) {
            weights_[bit] = previous;
            return false;
        }
        return true;
    }

    void clear_weights() noexcept {
        weights_.fill(SampleType{});
    }

    bool set_offset(SampleType offset) noexcept {
        if (!std::isfinite(static_cast<double>(offset)))
            return false;
        const SampleType previous = offset_;
        offset_ = offset;
        if (!projection_is_finite()) {
            offset_ = previous;
            return false;
        }
        return true;
    }

    void reset() noexcept {
        state_ = seed_;
    }

    std::uint32_t clock(bool external_bit = false) noexcept {
        const auto parity = std::popcount(state_ & feedback_mask_) & 1;
        const auto next_bit = static_cast<std::uint32_t>(parity ^ (external_bit ? 1 : 0));
        state_ = ((state_ << 1u) | next_bit) & register_mask();
        return state_;
    }

    SampleType weighted_output() const noexcept {
        long double output = static_cast<long double>(offset_);
        for (int bit = 0; bit < length_; ++bit) {
            if ((state_ & (std::uint32_t{1} << bit)) != 0u)
                output += static_cast<long double>(weights_[static_cast<std::size_t>(bit)]);
        }
        return static_cast<SampleType>(output);
    }

    SampleType minimum_output() const noexcept {
        long double output = static_cast<long double>(offset_);
        for (int bit = 0; bit < length_; ++bit)
            if (weights_[static_cast<std::size_t>(bit)] < SampleType{})
                output += static_cast<long double>(weights_[static_cast<std::size_t>(bit)]);
        return static_cast<SampleType>(output);
    }

    SampleType maximum_output() const noexcept {
        long double output = static_cast<long double>(offset_);
        for (int bit = 0; bit < length_; ++bit)
            if (weights_[static_cast<std::size_t>(bit)] > SampleType{})
                output += static_cast<long double>(weights_[static_cast<std::size_t>(bit)]);
        return static_cast<SampleType>(output);
    }

    int length() const noexcept {
        return length_;
    }
    std::uint32_t feedback_mask() const noexcept {
        return feedback_mask_;
    }
    std::uint32_t seed() const noexcept {
        return seed_;
    }
    std::uint32_t state() const noexcept {
        return state_;
    }
    SampleType offset() const noexcept {
        return offset_;
    }

    static constexpr int latency_samples() noexcept {
        return 0;
    }
    static constexpr int tail_samples() noexcept {
        return 0;
    }

  private:
    static constexpr bool valid_length(int length) noexcept {
        return length >= kMinLength && length <= kMaxLength;
    }

    static constexpr std::uint32_t mask_for(int length) noexcept {
        return length == 32 ? 0xFFFFFFFFu : ((std::uint32_t{1} << length) - 1u);
    }

    static constexpr bool mask_fits(std::uint32_t mask, int length) noexcept {
        return valid_length(length) && (mask & ~mask_for(length)) == 0u;
    }

    constexpr std::uint32_t register_mask() const noexcept {
        return mask_for(length_);
    }

    bool projection_is_finite_for(int length) const noexcept {
        long double minimum = static_cast<long double>(offset_);
        long double maximum = minimum;
        for (int bit = 0; bit < length; ++bit) {
            const long double weight =
                static_cast<long double>(weights_[static_cast<std::size_t>(bit)]);
            if (weight < 0.0L)
                minimum += weight;
            if (weight > 0.0L)
                maximum += weight;
        }
        const long double limit = static_cast<long double>(std::numeric_limits<SampleType>::max());
        return minimum >= -limit && maximum <= limit;
    }

    bool projection_is_finite() const noexcept {
        return projection_is_finite_for(length_);
    }

    std::array<SampleType, kMaxLength> weights_{};
    SampleType offset_{};
    int length_ = kDefaultLength;
    std::uint32_t feedback_mask_ = kDefaultFeedbackMask;
    std::uint32_t seed_ = kDefaultSeed;
    std::uint32_t state_ = kDefaultSeed;
};

using Lfsr = LfsrT<float>;
using Lfsr64 = LfsrT<double>;

} // namespace pulp::signal
