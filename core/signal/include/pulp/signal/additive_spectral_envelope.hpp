#pragma once

/// @file additive_spectral_envelope.hpp
/// Public spectral-envelope value types used by the additive bank.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

///
/// This enum exists because "sampling a spectral envelope makes timbre
/// pitch-independent" is only true under a condition, and the two readings of
/// that sentence are genuinely different sounds:
///
///   - **`absolute_hz`** (default) — break-points are frequencies in Hz, and a
///     partial reads the envelope at its own absolute frequency. This is what
///     Rodet & Depalle mean by a spectral envelope, and it is what a FORMANT
///     is: a resonance of a fixed body, which does NOT move when the singer
///     changes note. In this domain the relative spectrum legitimately changes
///     with pitch — that is the vowel staying the same vowel.
///   - **`relative_to_f0`** — break-points are multiples of the fundamental,
///     and a partial reads the envelope at `f_p / f0`. The shape now slides
///     with pitch, so the relative spectrum is identical at every fundamental.
///     This is the fully scale-invariant reading: one morph knob producing the
///     same timbral gesture at any pitch.
///
/// Neither is more correct; they answer different questions, and a spec that
/// says both "the vowel tracks pitch" and "unlike a fixed filter" is asking for
/// both at once. Note that the `spectral_tilt_db_oct` term is measured against
/// `log2(f_p/f0)` and so is scale-invariant in BOTH domains — a pure tilt is
/// the one envelope shape whose relative spectrum never depends on pitch, which
/// is why the scale-invariance test can use tilt-like envelopes in either mode.
enum class SpectralDomain : std::uint8_t { absolute_hz, relative_to_f0 };

/// A break-point spectral envelope: gain in dB as a function of frequency,
/// interpolated linearly in the (log2 abscissa, dB) domain — the perceptually
/// correct axes on both sides.
///
/// Fixed capacity, so it is a POD-ish value type that never allocates and can
/// be handed to a prepared bank from the audio thread.
class SpectralEnvelope {
public:
    /// Break-point capacity. [design parameter] default 64; 16 is the
    /// documented working size and 2 is the minimum that defines a slope.
    static constexpr int kMaxBreakpoints = 64;

    /// Removes every break-point. An empty envelope is a flat 0 dB — the
    /// identity, so a default-constructed envelope changes nothing.
    void clear() { count_ = 0; }

    int size() const { return count_; }

    /// Appends a break-point. Abscissae must be strictly ascending; a
    /// non-ascending or over-capacity point is rejected and reported, rather
    /// than silently corrupting the interpolation's monotonicity assumption.
    bool add(double abscissa, double gain_db) {
        if (count_ >= kMaxBreakpoints) return false;
        if (!(std::isfinite(abscissa) && abscissa > 0.0) || !std::isfinite(gain_db))
            return false;
        if (count_ > 0 && abscissa <= x_[static_cast<std::size_t>(count_ - 1)])
            return false;
        x_[static_cast<std::size_t>(count_)] = abscissa;
        db_[static_cast<std::size_t>(count_)] = gain_db;
        ++count_;
        return true;
    }

    /// Gain in dB at an abscissa. Outside the break-point span the endpoint
    /// value is held rather than extrapolated: extrapolating a slope past the
    /// last point is how an envelope quietly produces +40 dB at the top of the
    /// bank.
    double gain_db_at(double abscissa) const {
        if (count_ == 0) return 0.0;
        if (count_ == 1 || abscissa <= x_[0]) return db_[0];
        const auto last = static_cast<std::size_t>(count_ - 1);
        if (abscissa >= x_[last]) return db_[last];

        std::size_t hi = 1;
        while (hi < last && x_[hi] < abscissa) ++hi;
        const double lx = std::log2(x_[hi - 1]);
        const double hx = std::log2(x_[hi]);
        const double t = (std::log2(abscissa) - lx) / (hx - lx);
        return db_[hi - 1] + t * (db_[hi] - db_[hi - 1]);
    }

    /// Builds a constant-slope envelope — `db_per_octave` measured from
    /// `reference` — as two break-points spanning the audio band. The one
    /// envelope shape whose RELATIVE spectrum is pitch-independent in either
    /// `SpectralDomain`, because it is affine in log2 frequency.
    static SpectralEnvelope tilt(double db_per_octave, double reference = 1.0) {
        SpectralEnvelope e;
        const double lo = reference * 0.001;
        const double hi = reference * 1000.0;
        e.add(lo, db_per_octave * std::log2(lo / reference));
        e.add(hi, db_per_octave * std::log2(hi / reference));
        return e;
    }

private:
    std::array<double, kMaxBreakpoints> x_{};
    std::array<double, kMaxBreakpoints> db_{};
    int count_ = 0;
};

}  // namespace pulp::signal

