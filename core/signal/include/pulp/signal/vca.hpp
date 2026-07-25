#pragma once

/// @file vca.hpp
/// The control-to-gain law, and the attenuverter that conditions a modulation
/// signal before it gets there.
///
/// `GainT` applies a gain a caller already decided on. `VcaT` is the other
/// half: it turns a *control signal* into that gain, and the interesting part
/// is the law, not the multiply. Two laws exist because two things are being
/// modelled:
///
///   - **`Response::linear`** — gain IS the control. Correct whenever the
///     control is already a magnitude: a band's envelope in a vocoder, a
///     rectified detector output, a normalised window. Using an exponential
///     law there would square a quantity that was already the answer.
///   - **`Response::exponential`** — gain is `control` mapped onto a decibel
///     range, so equal control travel is equal dB. Correct whenever the control
///     is a *position*: an envelope meant to sound like a fade, a pedal, an
///     LFO driving tremolo depth. A linear fade spends most of its travel in
///     the top 6 dB and reads as "nothing, nothing, nothing, then everything".
///
/// The exponential law is stated as a floor in dB rather than as a bare
/// exponent, because a floor is the thing a spec can state a range for and an
/// exponent is not: `set_range_db(60)` means "control 0 is 60 dB down", which
/// is checkable. Control 0 maps to exactly zero gain, not to the floor, so a
/// closed VCA is silent rather than −60 dB — an important difference when
/// sixteen of them sum.
///
/// **Small-signal gain (series law 1):** both laws are gain-carrying but
/// neither is a nonlinearity in the signal path — the control is not derived
/// from the signal, so there is no loop and no slope to compensate. Peak gain
/// is exactly 1 at `control = 1` in both laws, by construction, so a VCA can
/// never make a signal louder than it was. `AttenuverterT` is likewise bounded
/// by `|scale| + |offset|`.
///
/// RT contract: everything here is stateless arithmetic over POD members.
/// Nothing allocates, locks, or performs I/O. `Response::exponential` costs one
/// `std::pow` per sample; a caller modulating at block rate should compute the
/// gain once per block via `gain_for()` and apply it with `GainT`.

#include <algorithm>
#include <cmath>

namespace pulp::signal {

/// How a `VcaT` maps its control signal onto gain.
enum class VcaResponse {
    linear,       ///< gain = control. For controls that are already magnitudes.
    exponential,  ///< gain = control mapped onto a dB range. For positions.
};

/// A voltage-controlled amplifier: a stated control-to-gain law plus the
/// multiply.
template <typename SampleType = float>
class VcaT {
public:
    using Response = VcaResponse;

    /// Depth of the exponential law, in dB below unity at control → 0⁺.
    /// [design parameter] default 60 dB, range 12 .. 120 dB.
    static constexpr double kDefaultRangeDb = 60.0;

    VcaT() { update(); }

    void set_response(Response response) { response_ = response; }
    Response response() const { return response_; }

    /// Sets the exponential law's depth in dB. Ignored by `Response::linear`.
    void set_range_db(double db) {
        range_db_ = std::clamp(db, 1.0, 200.0);
        update();
    }

    double range_db() const { return range_db_; }

    /// The gain this VCA would apply for `control`, without applying it.
    /// Control is clamped to `[0, 1]`: a VCA does not invert (that is what
    /// `AttenuverterT` is for) and does not amplify past unity.
    SampleType gain_for(SampleType control) const {
        const double c = std::clamp(static_cast<double>(control), 0.0, 1.0);
        if (response_ == Response::linear) return static_cast<SampleType>(c);
        // Exactly zero at c = 0 rather than the floor gain, so a closed VCA is
        // silent. The `pow` below would give 10^(−range/20), which is small but
        // not zero, and sixteen of those sum to something audible.
        if (c <= 0.0) return SampleType{0};
        return static_cast<SampleType>(std::pow(10.0, floor_exponent_ * (1.0 - c)));
    }

    /// Applies the law to `input`.
    SampleType process(SampleType input, SampleType control) const {
        return input * gain_for(control);
    }

private:
    void update() { floor_exponent_ = -range_db_ / 20.0; }

    Response response_ = Response::linear;
    double range_db_ = kDefaultRangeDb;
    double floor_exponent_ = -kDefaultRangeDb / 20.0;
};

using Vca = VcaT<float>;
using Vca64 = VcaT<double>;

/// Scales a modulation signal by a factor in `[-1, +1]` and adds an offset —
/// the modular world's attenuverter.
///
/// The inversion is the point. Patching one LFO into two destinations with
/// opposite polarity is how a chorus gets its anti-phase voice and how a filter
/// and an amplitude move in opposition; without an attenuverter every module
/// would need its own "invert" flag. The offset then re-centres the result, so
/// a bipolar LFO can drive a unipolar destination without the destination
/// knowing anything about polarity.
///
/// RT contract: stateless arithmetic. No allocation, no locks.
template <typename SampleType = float>
class AttenuverterT {
public:
    /// Scale factor in `[-1, +1]`. Negative inverts.
    void set_scale(SampleType scale) {
        scale_ = std::clamp(static_cast<double>(scale), -1.0, 1.0);
    }

    /// DC offset added after scaling.
    void set_offset(SampleType offset) { offset_ = static_cast<double>(offset); }

    SampleType scale() const { return static_cast<SampleType>(scale_); }
    SampleType offset() const { return static_cast<SampleType>(offset_); }

    /// The largest magnitude this attenuverter can output for a unit-bounded
    /// input — `|scale| + |offset|`. Exposed because it is the bound a caller
    /// needs to state its own worst-case gain without guessing.
    SampleType worst_case_output() const {
        return static_cast<SampleType>(std::abs(scale_) + std::abs(offset_));
    }

    SampleType process(SampleType input) const {
        return static_cast<SampleType>(static_cast<double>(input) * scale_ + offset_);
    }

private:
    double scale_ = 1.0;
    double offset_ = 0.0;
};

using Attenuverter = AttenuverterT<float>;
using Attenuverter64 = AttenuverterT<double>;

}  // namespace pulp::signal
