#pragma once

/// @file dynamics_contract.hpp
/// Shared envelope and gain-reduction vocabulary for dynamics processors.

#include <pulp/signal/ballistics_filter.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pulp::signal {

/// Canonical gain-reduction meter value.
///
/// `db()` is always a non-negative magnitude: zero means unity gain and a larger
/// value means more attenuation. This deliberately does not replace legacy
/// `gain_reduction_db()` accessors whose signs are part of their public
/// contracts. Dynamics processors expose this type through `gain_reduction()`
/// so meters can consume every lineage without knowing its internal sign.
///
/// RT contract: construction and inspection are pure scalar arithmetic over
/// scalar value. They allocate no memory, lock nothing, and perform no I/O.
class GainReduction {
  public:
    GainReduction() noexcept = default;

    /// Adapt a signed gain value, where attenuation is negative dB.
    static GainReduction from_signed_db(double signed_db) noexcept {
        return GainReduction{std::isfinite(signed_db) ? std::max(0.0, -signed_db) : 0.0};
    }

    /// Adapt an attenuation magnitude, where reduction is positive dB.
    static GainReduction from_magnitude_db(double magnitude_db) noexcept {
        return GainReduction{std::isfinite(magnitude_db) ? std::max(0.0, magnitude_db) : 0.0};
    }

    /// Non-negative attenuation magnitude in decibels.
    double db() const noexcept {
        return db_;
    }

    /// The equivalent gain value for addition in the dB domain.
    double signed_db() const noexcept {
        return -db_;
    }

    /// The equivalent linear amplitude multiplier in `[0, 1]`.
    double linear_gain() const noexcept {
        return std::pow(10.0, -db_ / 20.0);
    }

  private:
    explicit GainReduction(double db) noexcept : db_(db) {}

    double db_ = 0.0;
};

/// Discoverable name for Pulp's established peak/RMS envelope follower.
///
/// Input is a raw signed audio sample in the linear amplitude domain. Peak mode
/// rectifies it; RMS mode squares before smoothing and square-roots the result.
/// Attack and release are 10-to-90-percent times in milliseconds in the
/// smoothed-state domain: amplitude for peak mode and mean-square power for RMS
/// mode. The existing `BallisticsFilterT` name remains a compatibility spelling
/// of this same type, not a second implementation.
template <typename SampleType = float> using EnvelopeFollowerT = BallisticsFilterT<SampleType>;

using EnvelopeFollower = EnvelopeFollowerT<float>;
using EnvelopeFollower64 = EnvelopeFollowerT<double>;

/// Two-channel envelope follower with a detector-domain stereo link.
///
/// Link zero keeps independent detectors. Link one drives both followers from
/// the louder instantaneous channel magnitude, preserving the stereo image.
/// Intermediate values blend magnitudes before peak/RMS integration, so
/// opposite-polarity channels cannot cancel in the detector.
template <typename SampleType = float> class StereoEnvelopeFollowerT {
  public:
    using Mode = typename EnvelopeFollowerT<SampleType>::Mode;

    void prepare(SampleType sample_rate) {
        left_.prepare(sample_rate);
        right_.prepare(sample_rate);
    }

    void set_attack_ms(SampleType ms) {
        left_.set_attack_ms(ms);
        right_.set_attack_ms(ms);
    }

    void set_release_ms(SampleType ms) {
        left_.set_release_ms(ms);
        right_.set_release_ms(ms);
    }

    void set_mode(Mode mode) {
        left_.set_mode(mode);
        right_.set_mode(mode);
    }

    void set_link(SampleType amount) {
        if (std::isfinite(static_cast<double>(amount)))
            link_ = std::clamp(amount, SampleType{0}, SampleType{1});
    }

    SampleType link() const noexcept {
        return link_;
    }

    std::array<SampleType, 2> process(SampleType left, SampleType right) {
        if (link_ == SampleType{0})
            return {left_.process(left), right_.process(right)};

        if (!std::isfinite(static_cast<double>(left)) ||
            !std::isfinite(static_cast<double>(right))) {
            reset();
            return {SampleType{0}, SampleType{0}};
        }

        const SampleType left_magnitude = std::abs(left);
        const SampleType right_magnitude = std::abs(right);
        const SampleType shared = std::max(left_magnitude, right_magnitude);
        const SampleType left_detector = left_magnitude + link_ * (shared - left_magnitude);
        const SampleType right_detector = right_magnitude + link_ * (shared - right_magnitude);
        return {left_.process(left_detector), right_.process(right_detector)};
    }

    std::array<SampleType, 2> current() const {
        return {left_.current(), right_.current()};
    }

    void reset() {
        left_.reset();
        right_.reset();
    }

  private:
    EnvelopeFollowerT<SampleType> left_{};
    EnvelopeFollowerT<SampleType> right_{};
    SampleType link_ = SampleType{1};
};

using StereoEnvelopeFollower = StereoEnvelopeFollowerT<float>;
using StereoEnvelopeFollower64 = StereoEnvelopeFollowerT<double>;

} // namespace pulp::signal
