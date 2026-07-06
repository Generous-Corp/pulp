#pragma once

#include <cmath>
#include <algorithm>

namespace pulp::signal {

/// Stereo pan law selector.
///
/// `Sin3dB` and `EqualPower` are aliases.
/// Each law hits hard-left at pan=-1 and hard-right at pan=+1; they
/// differ in the centre behavior:
/// - `Linear`:      additive law. Constant amplitude sum, -6 dB notch at centre.
/// - `Balanced`:    full level on the panned-toward side until centre,
///                  then ramp. Useful as a stereo *balance* control.
/// - `Sin3dB`:      sin/cos law. Constant power, -3 dB notch at centre.
/// - `Sin4_5dB`:    sin/cos law shaped by exponent 1.5 → -4.5 dB notch.
/// - `Sin6dB`:      sin/cos law squared → -6 dB notch.
/// - `Sqrt3dB`:     sqrt law → -3 dB notch.
/// - `Sqrt4_5dB`:   sqrt law with offset → -4.5 dB notch.
enum class PanLaw {
    Linear,
    Balanced,
    Sin3dB,
    EqualPower = Sin3dB,
    Sin4_5dB,
    Sin6dB,
    Sqrt3dB,
    Sqrt4_5dB,
};

// Stereo panner with selectable pan law.
// pan = -1 (full left), 0 (centre), +1 (full right)
//
// RT contract: setters, gain computation, and process paths are scalar-only and
// allocate no memory.
template <typename SampleType = float>
class PannerT {
public:
    void set_pan(SampleType pan) {
        pan_ = std::clamp(pan, SampleType{-1.0f}, SampleType{1.0f});
    }
    SampleType pan() const { return pan_; }

    void set_law(PanLaw law) { law_ = law; }
    PanLaw law() const { return law_; }

    struct StereoSample {
        SampleType left, right;
    };

    /// Pan a mono signal to stereo.
    StereoSample process(SampleType input) const {
        SampleType l_gain, r_gain;
        compute_gains(l_gain, r_gain);
        return {input * l_gain, input * r_gain};
    }

    /// Pan stereo in-place (adjust balance).
    void process(SampleType& left, SampleType& right) const {
        SampleType l_gain, r_gain;
        compute_gains(l_gain, r_gain);
        left *= l_gain;
        right *= r_gain;
    }

    /// Compute per-channel gains for the current pan + law. Exposed so
    /// callers can ramp / smooth the gain themselves.
    void compute_gains(SampleType& l_gain, SampleType& r_gain) const {
        constexpr SampleType kHalfPi = SampleType{1.57079632679489661923f};
        const SampleType position =
            (pan_ + SampleType{1.0f}) * SampleType{0.5f}; // [0, 1] : 0 = full L, 1 = full R
        const SampleType theta = position * kHalfPi;      // [0, pi/2]
        switch (law_) {
            case PanLaw::Linear:
                l_gain = SampleType{1.0f} - position;
                r_gain = position;
                break;
            case PanLaw::Balanced:
                l_gain = pan_ <= SampleType{0.0f}
                    ? SampleType{1.0f}
                    : (SampleType{1.0f} - pan_);
                r_gain = pan_ >= SampleType{0.0f}
                    ? SampleType{1.0f}
                    : (SampleType{1.0f} + pan_);
                break;
            case PanLaw::Sin3dB: // == EqualPower
                l_gain = std::cos(theta);
                r_gain = std::sin(theta);
                break;
            case PanLaw::Sin4_5dB: {
                // Clamp before pow to guard against tiny negative
                // floating-point residue at the endpoints (cos(pi/2)).
                const SampleType c = std::max(SampleType{0.0f}, std::cos(theta));
                const SampleType s = std::max(SampleType{0.0f}, std::sin(theta));
                l_gain = std::pow(c, SampleType{1.5f});
                r_gain = std::pow(s, SampleType{1.5f});
                break;
            }
            case PanLaw::Sin6dB:
                l_gain = std::cos(theta) * std::cos(theta);
                r_gain = std::sin(theta) * std::sin(theta);
                break;
            case PanLaw::Sqrt3dB:
                l_gain = std::sqrt(SampleType{1.0f} - position);
                r_gain = std::sqrt(position);
                break;
            case PanLaw::Sqrt4_5dB:
                // -4.5 dB = geometric mean of -3 dB sqrt and -6 dB linear
                // → exponent 0.75. At centre yields 0.5^0.75 ≈ 0.5946
                // ≈ -4.51 dB per channel, matching the documented notch.
                l_gain = std::pow(SampleType{1.0f} - position, SampleType{0.75f});
                r_gain = std::pow(position, SampleType{0.75f});
                break;
        }
    }

private:
    SampleType pan_ = SampleType{0.0f};
    PanLaw law_ = PanLaw::EqualPower;
};

using Panner = PannerT<float>;
using Panner64 = PannerT<double>;

} // namespace pulp::signal
