#pragma once

/// @file dynamics_contract.hpp
/// Shared envelope and gain-reduction vocabulary for dynamics processors.

#include <pulp/signal/ballistics_filter.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pulp::signal {

/// Shared policy for dynamics processors with two detector channels.
enum class DynamicsStereoLink { independent, peak_linked };

/// Canonical gain-reduction meter value.
///
/// `db()` is always a non-negative magnitude: zero means unity gain and a larger
/// value means more attenuation. This deliberately does not replace legacy
/// `gain_reduction_db()` accessors whose signs are part of their public
/// contracts. Dynamics processors expose this type through `gain_reduction()`
/// so meters can consume every lineage without knowing its internal sign.
///
/// RT contract: construction and inspection are pure scalar arithmetic over a
/// scalar value. They allocate no memory, lock nothing, and perform no I/O.
class GainReduction {
  public:
    GainReduction() noexcept = default;

    /// Adapt a signed gain value, where attenuation is negative dB.
    static GainReduction from_signed_db(double signed_db) noexcept {
        return GainReduction{std::isnan(signed_db) ? 0.0 : std::max(0.0, -signed_db)};
    }

    /// Adapt an attenuation magnitude, where reduction is positive dB.
    static GainReduction from_magnitude_db(double magnitude_db) noexcept {
        return GainReduction{std::isnan(magnitude_db) ? 0.0 : std::max(0.0, magnitude_db)};
    }

    /// Non-negative attenuation magnitude in decibels.
    double db() const noexcept {
        return db_;
    }

    /// The equivalent gain value for addition in the dB domain.
    double signed_db() const noexcept {
        return -db_;
    }

    /// The equivalent linear amplitude multiplier in `[0, 1]`; infinite
    /// attenuation maps to zero.
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
/// Attack and release are exact 10-to-90-percent times in milliseconds in the
/// smoothed-state domain: amplitude for peak mode and mean-square power for RMS
/// mode. `BallisticsFilterT` remains the legacy-compatible spelling with its
/// historical nominal 2.2 exponent; this type selects the exact ln(9) contract
/// without changing existing BallisticsFilter output.
template <typename SampleType = float>
class EnvelopeFollowerT : public BallisticsFilterT<SampleType> {
  public:
    using Base = BallisticsFilterT<SampleType>;
    using Mode = typename Base::Mode;

    EnvelopeFollowerT() : Base(Base::TimeConvention::exact_10_to_90) {}

    static SampleType coefficient_for_time_ms(SampleType ms, SampleType sample_rate) {
        return Base::exact_coefficient_for_time_ms(ms, sample_rate);
    }

  private:
    template <typename> friend class StereoEnvelopeFollowerT;

    SampleType detector_state() const noexcept {
        return Base::detector_state();
    }

    void synchronize_detector_state(SampleType state) noexcept {
        Base::synchronize_detector_state(state);
    }
};

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
        if (!std::isfinite(static_cast<double>(amount)))
            return;
        const SampleType next = std::clamp(amount, SampleType{0}, SampleType{1});
        if (next == SampleType{1} && link_ != SampleType{1})
            synchronize_linked_state();
        link_ = next;
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
        const SampleType left_envelope = left_.process(left_detector);
        const SampleType right_envelope = right_.process(right_detector);
        if (link_ == SampleType{1}) {
            const SampleType linked_state =
                std::max(left_.detector_state(), right_.detector_state());
            left_.synchronize_detector_state(linked_state);
            right_.synchronize_detector_state(linked_state);
            const SampleType linked_envelope = std::max(left_envelope, right_envelope);
            return {linked_envelope, linked_envelope};
        }
        return {left_envelope, right_envelope};
    }

    std::array<SampleType, 2> current() const {
        return {left_.current(), right_.current()};
    }

    void reset() {
        left_.reset();
        right_.reset();
    }

  private:
    void synchronize_linked_state() noexcept {
        const SampleType linked_state =
            std::max(left_.detector_state(), right_.detector_state());
        left_.synchronize_detector_state(linked_state);
        right_.synchronize_detector_state(linked_state);
    }

    EnvelopeFollowerT<SampleType> left_{};
    EnvelopeFollowerT<SampleType> right_{};
    SampleType link_ = SampleType{1};
};

using StereoEnvelopeFollower = StereoEnvelopeFollowerT<float>;
using StereoEnvelopeFollower64 = StereoEnvelopeFollowerT<double>;

} // namespace pulp::signal
