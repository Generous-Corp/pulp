#pragma once

/// @file commuted_string_excitation.hpp
/// Bounded preparation of body-shaped excitation for plucked-string voices.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <span>
#include <type_traits>

namespace pulp::signal {

enum class CommutedStringExcitationPrepareStatus {
    ok,
    empty_base_excitation,
    empty_body_impulse_response,
    retained_sample_limit_exceeded,
    non_finite_input,
    non_finite_output,
    input_aliases_profile,
};

/// Per-voice traversal state for an immutable prepared excitation profile.
///
/// The referenced samples must outlive the cursor. Triggering, resetting,
/// reading one sample, and rendering a span are constant-storage operations;
/// they allocate no memory, take no locks, perform no I/O, and never throw.
template <typename SampleType = float> class CommutedStringExcitationCursorT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);

    CommutedStringExcitationCursorT() noexcept = default;

    explicit CommutedStringExcitationCursorT(std::span<const SampleType> samples) noexcept
        : samples_(samples) {}

    /// Leaves the cursor silent at the beginning of the profile.
    void reset() noexcept {
        position_ = 0;
        active_ = false;
    }

    /// Starts the profile from its first sample.
    void trigger() noexcept {
        position_ = 0;
        active_ = !samples_.empty();
    }

    /// Restarts the profile even when the current traversal has not finished.
    void retrigger() noexcept {
        trigger();
    }

    [[nodiscard]] SampleType next() noexcept {
        if (!active_)
            return SampleType{0};

        const auto value = samples_[position_++];
        if (position_ == samples_.size())
            active_ = false;
        return value;
    }

    void render(std::span<SampleType> output) noexcept {
        for (auto& sample : output)
            sample = next();
    }

    [[nodiscard]] bool active() const noexcept {
        return active_;
    }
    [[nodiscard]] std::size_t position() const noexcept {
        return position_;
    }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return active_ ? samples_.size() - position_ : 0;
    }

  private:
    std::span<const SampleType> samples_{};
    std::size_t position_ = 0;
    bool active_ = false;
};

/// A fixed-capacity body-shaped excitation shared by plucked-string voices.
///
/// `prepare()` performs the finite time-domain convolution of a base excitation
/// and body impulse response. It is a control-thread operation and must not run
/// while a cursor refers to the profile. Invalid input leaves the previous
/// profile unchanged. Successful preparation exposes immutable samples; each
/// voice owns only its independent cursor and feeds `next()` to its string.
template <typename SampleType = float, std::size_t MaximumRetainedSamples = 16384>
class CommutedStringExcitationProfileT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaximumRetainedSamples > 0);

    static constexpr std::size_t maximum_retained_samples = MaximumRetainedSamples;

    [[nodiscard]] CommutedStringExcitationPrepareStatus
    prepare(std::span<const SampleType> base_excitation,
            std::span<const SampleType> body_impulse_response) noexcept {
        if (base_excitation.empty()) {
            return CommutedStringExcitationPrepareStatus::empty_base_excitation;
        }
        if (body_impulse_response.empty()) {
            return CommutedStringExcitationPrepareStatus::empty_body_impulse_response;
        }
        if (base_excitation.size() > MaximumRetainedSamples ||
            body_impulse_response.size() > MaximumRetainedSamples ||
            base_excitation.size() > MaximumRetainedSamples - (body_impulse_response.size() - 1)) {
            return CommutedStringExcitationPrepareStatus::retained_sample_limit_exceeded;
        }
        if (overlaps_storage(base_excitation) || overlaps_storage(body_impulse_response)) {
            return CommutedStringExcitationPrepareStatus::input_aliases_profile;
        }
        for (const auto sample : base_excitation) {
            if (!std::isfinite(sample)) {
                return CommutedStringExcitationPrepareStatus::non_finite_input;
            }
        }
        for (const auto sample : body_impulse_response) {
            if (!std::isfinite(sample)) {
                return CommutedStringExcitationPrepareStatus::non_finite_input;
            }
        }

        const auto retained = base_excitation.size() + body_impulse_response.size() - 1;
        for (std::size_t index = 0; index < retained; ++index) {
            const auto value = convolve_at(index, base_excitation, body_impulse_response);
            const auto stored = static_cast<SampleType>(value);
            if (!std::isfinite(value) || !std::isfinite(stored)) {
                return CommutedStringExcitationPrepareStatus::non_finite_output;
            }
        }

        samples_.fill(SampleType{0});
        for (std::size_t index = 0; index < retained; ++index) {
            samples_[index] =
                static_cast<SampleType>(convolve_at(index, base_excitation, body_impulse_response));
        }
        retained_samples_ = retained;
        return CommutedStringExcitationPrepareStatus::ok;
    }

    [[nodiscard]] std::span<const SampleType> samples() const noexcept {
        return {samples_.data(), retained_samples_};
    }

    [[nodiscard]] std::size_t retained_samples() const noexcept {
        return retained_samples_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return retained_samples_ == 0;
    }

    [[nodiscard]] CommutedStringExcitationCursorT<SampleType> make_cursor() const noexcept {
        return CommutedStringExcitationCursorT<SampleType>{samples()};
    }

  private:
    using Accumulator =
        std::conditional_t<sizeof(SampleType) < sizeof(double), double, long double>;

    [[nodiscard]] static Accumulator
    convolve_at(std::size_t output_index, std::span<const SampleType> base_excitation,
                std::span<const SampleType> body_impulse_response) noexcept {
        const auto first_base = output_index >= body_impulse_response.size() - 1
                                    ? output_index - (body_impulse_response.size() - 1)
                                    : 0;
        const auto last_base = std::min(output_index, base_excitation.size() - 1);

        Accumulator sum = 0;
        for (auto base_index = first_base; base_index <= last_base; ++base_index) {
            const auto body_index = output_index - base_index;
            sum += static_cast<Accumulator>(base_excitation[base_index]) *
                   static_cast<Accumulator>(body_impulse_response[body_index]);
        }
        return sum;
    }

    [[nodiscard]] bool overlaps_storage(std::span<const SampleType> input) const noexcept {
        if (input.empty())
            return false;

        const auto input_begin = input.data();
        const auto input_end = input_begin + input.size();
        const auto storage_begin = samples_.data();
        const auto storage_end = storage_begin + samples_.size();
        const auto less = std::less<const SampleType*>{};
        return less(input_begin, storage_end) && less(storage_begin, input_end);
    }

    std::array<SampleType, MaximumRetainedSamples> samples_{};
    std::size_t retained_samples_ = 0;
};

using CommutedStringExcitationCursor = CommutedStringExcitationCursorT<float>;
using CommutedStringExcitationCursor64 = CommutedStringExcitationCursorT<double>;
using CommutedStringExcitationProfile = CommutedStringExcitationProfileT<float>;
using CommutedStringExcitationProfile64 = CommutedStringExcitationProfileT<double>;

} // namespace pulp::signal
