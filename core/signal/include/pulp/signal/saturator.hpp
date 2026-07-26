#pragma once

/// @file saturator.hpp
/// The memoryless saturation stage every drive, fuzz, tube and tape-warmth
/// block in the catalog composes instead of hand-rolling a `tanh(x)` call.
///
/// Saturation is the most-reused nonlinearity there is: a drive pedal, a tape
/// stage, a tube preamp, transformer colour and mix-bus glue are one waveshaper
/// wearing different clothes. What differentiates them is bias, tone
/// bracketing, and an antialiasing policy — not the core curve. Getting that
/// core right once means every later module specifies only its own character
/// mapping.
///
/// Four shapes, chosen to span the useful design space with textbook functions
/// only (Arfib 1979 / Le Brun 1979 waveshaping lineage): `tanh` (bounded,
/// symmetric), `atan` (bounded, softer knee), a cubic soft clip (exact
/// polynomial, cheapest, hardest character) and `asinh` (unbounded, gentlest —
/// saturation character that never fully clips).
///
/// ## The four contracts this block keeps
///
/// **Unity small-signal gain, at every drive setting** (series law 1). All
/// four shapes have `f'(0) = 1`, and the drive law is `shaped(x) = f(d·x)/d`,
/// so `shaped'(0) = f'(0) = 1` identically. The drive knob changes how much of
/// the curve's bend a signal reaches, never the pass-through gain of a quiet
/// one.
///
/// **Unbiased, that also makes 1.0 the SUPREMUM**, since every shape is
/// globally compressive past the origin: gain only ever falls below 1. **With
/// bias it does not**, and this is worth stating plainly because it is easy to
/// assume otherwise. Normalising by the local slope `f'(b)` means the curve is
/// expansive on the side of the operating point that runs back toward the
/// origin, where the slope is steeper. The true supremum is
///
/// ```
/// sup |shaped(x) / x|  =  f'(0) / f'(b)  =  1 / f'(b)
/// ```
///
/// — 1.0 at zero bias, 2.38 for `tanh` at full bias, and 25× for `cubic_soft`
/// at its clamp, where `f'(b) = 1 − b²` is deliberately near zero. That is not
/// a defect to fix; it is what "normalise the gain at the operating point"
/// means, and a caller running a heavy bias is asking for a hot stage. It IS a
/// number the caller must be told rather than left to discover, so
/// `worst_case_gain()` computes it and the test suite asserts it holds across
/// the whole grid. That asserted bound — not a flat 1.0 — is what Forge's
/// registry cites (series law 8).
///
/// **Bias without a DC offset.** Raw `f(u + b)` would hand the rest of the
/// chain a DC step to eat, and subtracting `f(b)` alone would still break the
/// drive law, because the local slope at the operating point is `f'(b)`, not 1.
/// The construction normalises by the local slope:
///
/// ```
/// shaped_biased(x) = [ f(d·x + b) − f(b) ] / [ d · f'(b) ]
/// ```
///
/// which is exactly zero at `x = 0` for every bias and every drive, collapses
/// to the unbiased form at `b = 0` (since `f'(0) = 1`), and keeps unity gain at
/// the operating point — so bias can be swept live without a click or a level
/// jump. Asymmetry is what produces even harmonics alongside the odd ones, the
/// documented single-ended-triode character.
///
/// **A stated antialiasing policy** (series law 4). The default is first-order
/// antiderivative antialiasing, which costs zero latency — the one-sample
/// history is state, not delay. `oversample_2x` keeps the house half-band path
/// available for chains that must stay phase-coherent with another
/// oversampled stage; it reports its latency exactly rather than estimating.
/// `off` exists only for tests and the CPU floor.
///
/// **Tone bracketing that actually cancels.** Pre-emphasis boosts above a
/// corner so high content hits the knee hotter and generates proportionally
/// more upper harmonics; de-emphasis applies the inverse shelf. With matching
/// corners the pair is an *exact* inverse (an RBJ shelf at `−G` is the
/// algebraic reciprocal of the same shelf at `+G`), so a linear pass-through
/// measures flat end to end.
///
/// ## Usage recipes
///
/// - **Transparent gain-staging pad** — `drive_db = −12`, no bias, tone off.
///   The block stays in the graph for automation continuity while doing
///   audibly nothing.
/// - **Tape-adjacent warmth** — `tanh_soft`, `+6..+12 dB`, bias 5–10 %, tone
///   corner ~4 kHz. The cheap version of the physically-modelled tape tier.
/// - **Fuzz / hard character** — `cubic_soft` or `atan_soft`, `+24..+36 dB`,
///   tone corner 2–5 kHz so the fuzz reads bright-then-dark.
/// - **Wide-headroom glue** — `sinh_arc`, `0..+6 dB`, no bias. Never hard
///   limits, so transients round off rather than slamming an asymptote.
/// - **Single-ended growl** — `tanh_soft` or `sinh_arc`, `+18..+30 dB`, bias
///   30–60 %, chasing even-harmonic-forward character.
///
/// ## Out of scope, deliberately
///
/// Memory/state-dependent nonlinearities (hysteresis, thermal lag) are the
/// tape physical tier's job. Multiband drive is N instances plus the existing
/// crossovers. Hard clipping is excluded: its discontinuous derivative
/// generates a far wider, more alias-prone spectrum for the same antialiasing
/// budget, and it deserves its own justification rather than riding in here.
///
/// RT contract: `prepare()` may allocate (it sizes the oversampler's filter
/// state and the dry-path alignment delay). `set_*`, `process()` and `reset()`
/// never allocate, never lock, never perform I/O, and are safe per sample on
/// the audio thread. All state is POD; zero-init is a valid fresh instance at
/// the neutral setting (`tanh_soft`, 0 dB drive, no bias, tone off, ADAA,
/// fully wet). `process()` costs one or two transcendental evaluations
/// depending on the alias policy.
///
/// References: Pakarinen & Yeh, "A Review of Digital Techniques for Modeling
/// Vacuum-Tube Guitar Amplifiers", Computer Music Journal 33(2), 2009 (cited
/// for the published concepts — asymmetry produces even harmonics; linear
/// filtering brackets a nonlinear stage — never for any amplifier's fitted
/// curve). Arfib, JAES 27(10), 1979, and Le Brun, JAES 27(4), 1979, for the
/// waveshaping method. Parker, Zavalishin & Le Bivic, DAFx-16, and Bilbao,
/// Esqueda, Parker & Välimäki, IEEE SPL 24(7), 2017, for antiderivative
/// antialiasing. RBJ "Audio EQ Cookbook" for the shelf topology family.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/oversampling.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// Which member of the waveshaper family a `SaturatorT` evaluates.
enum class SaturatorShape : std::uint8_t {
    tanh_soft,   ///< `tanh(u)`. The canonical smooth saturator.
    atan_soft,   ///< `atan(u)`. Softer knee than tanh at the shoulder.
    cubic_soft,  ///< `u − u³/3`, clipping at `|u| ≥ 1`. Hardest character.
    sinh_arc,    ///< `asinh(u)`. Unbounded and sub-linear; never hard-limits.
};

/// How a `SaturatorT` keeps the harmonics it generates from folding back.
enum class SaturatorAliasPolicy : std::uint8_t {
    adaa,          ///< First-order antiderivative antialiasing. Zero latency.
    oversample_2x, ///< House linear-phase half-band pair. Latency reported exactly.
    off,           ///< Raw per-sample evaluation. Tests and the CPU floor only.
};

namespace detail {

/// The waveshaper family's three closed forms per member: the curve `f`, its
/// derivative `f'` (needed for the bias normaliser), and its antiderivative
/// `F1` (needed for ADAA).
///
/// `F1` is EVEN for every member, because the antiderivative of an odd
/// function is even. That is worth stating because it is easy to get wrong:
/// writing the cubic's clipped branch as `sign(u)·[(2/3)(|u|−1) + 5/12]` makes
/// it odd, and an odd `F1` feeds ADAA a sign-flipped difference on every
/// negative excursion — which does not look like a crash, it looks like
/// distortion, and would be mistaken for the effect working.
struct SaturatorShapeMath {
    /// `f(u)`.
    static double f(SaturatorShape shape, double u) {
        switch (shape) {
            case SaturatorShape::tanh_soft: return std::tanh(u);
            case SaturatorShape::atan_soft: return std::atan(u);
            case SaturatorShape::cubic_soft:
                if (u > 1.0) return 2.0 / 3.0;
                if (u < -1.0) return -2.0 / 3.0;
                return u - u * u * u / 3.0;
            case SaturatorShape::sinh_arc: return std::asinh(u);
        }
        return u;
    }

    /// `f'(u)`. Unity at the origin for all four members, by construction —
    /// that identity is what makes one drive law work across the family.
    static double df(SaturatorShape shape, double u) {
        switch (shape) {
            case SaturatorShape::tanh_soft: {
                const double c = std::cosh(u);
                return 1.0 / (c * c);
            }
            case SaturatorShape::atan_soft: return 1.0 / (1.0 + u * u);
            case SaturatorShape::cubic_soft: return std::abs(u) < 1.0 ? 1.0 - u * u : 0.0;
            case SaturatorShape::sinh_arc: return 1.0 / std::sqrt(1.0 + u * u);
        }
        return 1.0;
    }

    /// `F1(u) = ∫f(u)du`, with an arbitrary constant — ADAA only ever takes
    /// differences of it, so the constant cancels.
    static double antiderivative(SaturatorShape shape, double u) {
        switch (shape) {
            case SaturatorShape::tanh_soft:
                // ln(cosh u), computed so large |u| does not overflow cosh:
                // ln(cosh u) = |u| + ln((1 + e^(−2|u|)) / 2).
                {
                    const double a = std::abs(u);
                    return a + std::log1p(std::exp(-2.0 * a)) - 0.6931471805599453;
                }
            case SaturatorShape::atan_soft:
                return u * std::atan(u) - 0.5 * std::log1p(u * u);
            case SaturatorShape::cubic_soft: {
                const double a = std::abs(u);
                if (a < 1.0) return u * u * 0.5 - u * u * u * u / 12.0;
                // Even, not odd: integrating the constant ±2/3 outward from
                // |u| = 1 in both directions gives the same magnitude. The
                // 5/12 matches the interior branch at |u| = 1 (1/2 − 1/12), so
                // F1 is continuous there.
                return (2.0 / 3.0) * (a - 1.0) + 5.0 / 12.0;
            }
            case SaturatorShape::sinh_arc:
                return u * std::asinh(u) - std::sqrt(1.0 + u * u);
        }
        return 0.5 * u * u;
    }
};

}  // namespace detail

/// The memoryless saturation stage.
template <typename SampleType = float>
class SaturatorT {
public:
    using Shape = SaturatorShape;
    using AliasPolicy = SaturatorAliasPolicy;

    // ── Design parameters (the complete roster; see the file doc block) ────

    /// Drive range in dB. The floor is below 0 dB on purpose: proving unity
    /// gain at a point *below* unity is more convincing than proving it at
    /// unity, where "correct" and "coincidentally close" look the same.
    /// [design parameter] default 0 dB, range −12 .. +36 dB.
    static constexpr double kDriveDbMin = -12.0;
    static constexpr double kDriveDbMax = 36.0;
    static constexpr double kDriveDbDefault = 0.0;

    /// Bias magnitude ceiling. [design parameter] default 1.0, fixed.
    static constexpr double kBiasMax = 1.0;

    /// The cubic's own clip boundary is at `|u| = 1`, where `f'(b) = 1 − b²`
    /// reaches zero — and the bias normaliser divides by it. Clamping short of
    /// the boundary keeps the divisor away from zero without special-casing.
    /// [design parameter] default 0.98, range 0.5 .. 0.999.
    static constexpr double kCubicBiasClamp = 0.98;

    /// Pre-emphasis corner. 0 disables the pair entirely.
    /// [design parameter] default 3000 Hz, range 0 (off) .. 8000 Hz, log.
    static constexpr double kTonePreHzDefault = 3000.0;
    static constexpr double kTonePreHzMax = 8000.0;

    /// Lowest corner the shelf will actually be placed at. Below this a "high
    /// shelf" is a broadband gain, which is not what the control means.
    static constexpr double kTonePreHzFloor = 20.0;

    /// Shelf gain magnitude: boost before the shaper, cut after.
    /// [design parameter] default +9 dB, range 0 .. +18 dB.
    static constexpr double kPreBoostDbDefault = 9.0;
    static constexpr double kPreBoostDbMax = 18.0;

    /// Shelf Q. Butterworth-flat, the neutral choice for a pair whose whole
    /// job is to cancel. [design parameter] default 0.7071, range 0.3 .. 2.0.
    static constexpr double kShelfQ = 0.70710678118654752;

    /// ADAA's near-degenerate-denominator guard. When two consecutive samples
    /// are this close, the difference quotient loses precision and the direct
    /// evaluation at their midpoint is used instead.
    ///
    /// **Honest gap:** no citable literature pins this value. Bilbao et al.
    /// establish the method and the *need* for a fallback branch, not a
    /// threshold. Any value far below a typical sample-to-sample delta and far
    /// above float rounding noise works.
    /// [design parameter] default 1e-6, range 1e-9 .. 1e-3.
    static constexpr double kAdaaEpsilon = 1e-6;

    /// Output trim range. [design parameter] default 0 dB, range ±24 dB.
    static constexpr double kOutputTrimDbMax = 24.0;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    SaturatorT() { update_shaper(); }

    /// Sizes the oversampler and the dry-path alignment delay. May allocate.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;

        // The house oversampler, at 2x with the linear-phase FIR kind. Composed
        // rather than reimplemented: its x2 standard-quality prototype is 129
        // taps, whose polyphase halves give exactly 64 samples of group delay at
        // the input rate — the value `latency_samples()` reports.
        using Os = OversamplerT<SampleType>;
        oversampler_.set_kind(Os::Kind::linear_phase_fir);
        oversampler_.set_quality(Os::Quality::standard);
        oversampler_.set_factor(Os::Factor::x2);
        oversampler_.set_sample_rate(static_cast<SampleType>(sample_rate_));
        oversample_latency_ = oversampler_.latency_samples();

        // The dry path must be delayed to match the wet path, or the dry/wet
        // mix becomes a comb filter the moment `oversample_2x` is selected.
        // Sized for the worst case so switching policy never allocates.
        dry_delay_.assign(static_cast<std::size_t>(oversample_latency_) + 1, SampleType{0});

        update_tone();
        reset();
    }

    void set_shape(Shape shape) {
        shape_ = shape;
        update_shaper();
    }

    Shape shape() const { return shape_; }

    void set_drive_db(double db) {
        if (!std::isfinite(db)) return;
        drive_db_ = std::clamp(db, kDriveDbMin, kDriveDbMax);
        update_shaper();
    }

    double drive_db() const { return drive_db_; }

    /// Bias as a normalised `[-1, +1]`. Clamped tighter for `cubic_soft`, whose
    /// slope reaches zero at `|b| = 1`.
    void set_bias(double normalized) {
        if (!std::isfinite(normalized)) return;
        bias_request_ = std::clamp(normalized, -kBiasMax, kBiasMax);
        update_shaper();
    }

    /// The bias actually in force after any shape-specific clamp — exposed so a
    /// caller can show the effective value rather than the requested one.
    double bias() const { return bias_; }

    /// Pre-emphasis corner in Hz. 0 disables the pre/de pair.
    void set_tone_pre_hz(double hz) {
        if (!std::isfinite(hz)) return;
        tone_pre_hz_ = std::clamp(hz, 0.0, kTonePreHzMax);
        update_tone();
    }

    /// When true (the default) the de-emphasis corner follows the pre-emphasis
    /// corner, so the pair cancels exactly. Set false to shape the asymmetry's
    /// low end differently from its top end.
    void set_tone_tracking(bool tracking) {
        tone_tracking_ = tracking;
        update_tone();
    }

    /// De-emphasis corner, used only when tracking is off.
    void set_tone_de_hz(double hz) {
        if (!std::isfinite(hz)) return;
        tone_de_hz_ = std::clamp(hz, 0.0, kTonePreHzMax);
        update_tone();
    }

    void set_pre_boost_db(double db) {
        if (!std::isfinite(db)) return;
        pre_boost_db_ = std::clamp(db, 0.0, kPreBoostDbMax);
        update_tone();
    }

    void set_alias_policy(AliasPolicy policy) { alias_policy_ = policy; }
    AliasPolicy alias_policy() const { return alias_policy_; }

    void set_mix(double wet01) {
        if (!std::isfinite(wet01)) return;
        mix_ = std::clamp(wet01, 0.0, 1.0);
    }

    void set_output_trim_db(double db) {
        if (!std::isfinite(db)) return;
        output_trim_ = units::db_to_linear(std::clamp(db, -kOutputTrimDbMax, kOutputTrimDbMax));
    }

    /// Reported exactly, never estimated (series law 5). Zero for `adaa` and
    /// `off` — ADAA's one-sample history is state, not delay, exactly as a
    /// one-pole's `z⁻¹` term is. The `oversample_2x` value is the composed
    /// oversampler's own reported latency.
    int latency_samples() const noexcept {
        return alias_policy_ == AliasPolicy::oversample_2x ? oversample_latency_ : 0;
    }

    /// Never allocates; a zero-initialised instance is already in this state.
    void reset() {
        adaa_previous_ = 0.0;
        pre_filter_.reset();
        de_filter_.reset();
        oversampler_.reset();
        std::fill(dry_delay_.begin(), dry_delay_.end(), SampleType{0});
        dry_write_ = 0;
        dry_valid_ = 0;
    }

    /// One sample through the full chain: pre-emphasis, alias-safe shaping,
    /// de-emphasis, latency-aligned dry/wet, trim.
    SampleType process(SampleType input) {
        if (!std::isfinite(static_cast<double>(input))) {
            recover_audio_fault();
            return SampleType{0};
        }
        const double dry = delayed_dry(static_cast<double>(input));

        double x = static_cast<double>(input);
        if (tone_active_) x = pre_filter_.process(static_cast<SampleType>(x));

        double wet = 0.0;
        switch (alias_policy_) {
            case AliasPolicy::off:
                wet = shaped(x);
                break;
            case AliasPolicy::adaa:
                wet = shaped_adaa(x);
                break;
            case AliasPolicy::oversample_2x:
                wet = oversampler_.process(static_cast<SampleType>(x), [this](SampleType s) {
                    return static_cast<SampleType>(shaped(static_cast<double>(s)));
                });
                break;
        }

        if (tone_active_) wet = de_filter_.process(static_cast<SampleType>(wet));

        const double mixed = dry + mix_ * (wet - dry);
        const double output = snap_to_zero(mixed * output_trim_);
        if (!std::isfinite(output)) {
            recover_audio_fault();
            return SampleType{0};
        }
        return static_cast<SampleType>(output);
    }

    /// The supremum of `|shaped(x) / x|` over every input, at the current
    /// shape and bias — the bound Forge's registry cites (series law 8), and a
    /// tested invariant rather than an estimate.
    ///
    /// It is `f'(0) / f'(b) = 1 / f'(b)`: the shaper's steepest slope (always
    /// at the origin, for all four members) divided by the slope at the
    /// operating point that the bias construction normalises by. Exactly 1.0
    /// unbiased. Independent of drive, because the drive law divides by the
    /// same `d` it multiplies the argument by.
    double worst_case_gain() const { return max_gain_; }

    /// The normalised shaper itself, exposed so a caller (or a test) can
    /// evaluate the curve without running it through the alias policy or the
    /// tone pair. Unity slope at the origin; exactly 0 at 0 for every bias.
    double shaped(double x) const {
        const double u = drive_ * x + bias_;
        return (detail::SaturatorShapeMath::f(shape_, u) - bias_offset_) * inv_drive_slope_;
    }

private:
    void recover_audio_fault() noexcept {
        adaa_previous_ = 0.0;
        pre_filter_.reset();
        de_filter_.reset();
        oversampler_.reset();
        dry_write_ = 0;
        dry_valid_ = 0;
    }

    /// `Φ(x)`, the antiderivative of `shaped(x)` with respect to `x`.
    ///
    /// For the unbiased case the chain rule gives `Φ(x) = F1(d·x)/d²`. The
    /// biased shaper carries a constant term `−f(b)/(d·f'(b))`, whose integral
    /// is linear in `x`, hence the second term.
    double antiderivative(double x) const {
        const double u = drive_ * x + bias_;
        return (detail::SaturatorShapeMath::antiderivative(shape_, u) -
                drive_ * bias_offset_ * x) *
               inv_drive_sq_slope_;
    }

    /// First-order antiderivative antialiasing. Zero latency: the one-sample
    /// history is state, not delay.
    double shaped_adaa(double x) {
        const double previous = adaa_previous_;
        adaa_previous_ = x;
        const double delta = x - previous;
        if (std::abs(delta) > kAdaaEpsilon)
            return (antiderivative(x) - antiderivative(previous)) / delta;
        // The difference quotient is numerically meaningless here; the limit it
        // is approximating is the curve evaluated at the midpoint.
        return shaped(0.5 * (x + previous));
    }

    /// Reads the dry sample aligned with the wet path's latency, then writes
    /// the new one. A no-op ring when the active policy has zero latency.
    double delayed_dry(double input) {
        const int latency = latency_samples();
        if (latency <= 0 || dry_delay_.empty()) return input;
        const std::size_t size = dry_delay_.size();
        const std::size_t read = (dry_write_ + size - static_cast<std::size_t>(latency)) % size;
        const double out = dry_valid_ >= static_cast<std::size_t>(latency)
                               ? static_cast<double>(dry_delay_[read])
                               : 0.0;
        dry_delay_[dry_write_] = static_cast<SampleType>(input);
        dry_write_ = (dry_write_ + 1) % size;
        if (dry_valid_ < size) ++dry_valid_;
        return out;
    }

    void update_shaper() {
        drive_ = units::db_to_linear(drive_db_);

        // The cubic's slope reaches zero at |b| = 1, and the normaliser divides
        // by it. Clamp per shape rather than globally so the other three keep
        // their full range.
        const double limit = shape_ == Shape::cubic_soft ? kCubicBiasClamp : kBiasMax;
        bias_ = std::clamp(bias_request_, -limit, limit);

        bias_offset_ = detail::SaturatorShapeMath::f(shape_, bias_);
        const double slope = detail::SaturatorShapeMath::df(shape_, bias_);
        // Guarded, though the per-shape clamp above should make it unreachable:
        // a zero here would be a silent inf rather than a loud failure.
        const double safe_slope = slope > 1e-12 ? slope : 1e-12;
        inv_drive_slope_ = 1.0 / (drive_ * safe_slope);
        inv_drive_sq_slope_ = 1.0 / (drive_ * drive_ * safe_slope);
        // Every shape's steepest slope is at the origin and equals 1, so the
        // largest gain the biased, slope-normalised curve can present is
        // f'(0)/f'(b) = 1/f'(b). Drive cancels out of the ratio.
        max_gain_ = 1.0 / safe_slope;
    }

    void update_tone() {
        tone_active_ = tone_pre_hz_ >= kTonePreHzFloor && pre_boost_db_ > 0.0;
        if (!tone_active_) return;

        const double de_hz = tone_tracking_ ? tone_pre_hz_
                                            : std::max(tone_de_hz_, kTonePreHzFloor);
        // An RBJ high shelf at −G is the exact algebraic reciprocal of the same
        // shelf at +G, so a tracked pair cancels to numerical precision rather
        // than to within a fitted tolerance.
        pre_filter_.set_coefficients(BiquadT<SampleType>::Type::high_shelf,
                                     static_cast<SampleType>(tone_pre_hz_),
                                     static_cast<SampleType>(kShelfQ),
                                     static_cast<SampleType>(sample_rate_),
                                     static_cast<SampleType>(pre_boost_db_));
        de_filter_.set_coefficients(BiquadT<SampleType>::Type::high_shelf,
                                    static_cast<SampleType>(de_hz),
                                    static_cast<SampleType>(kShelfQ),
                                    static_cast<SampleType>(sample_rate_),
                                    static_cast<SampleType>(-pre_boost_db_));
    }

    double sample_rate_ = 44100.0;

    Shape shape_ = Shape::tanh_soft;
    AliasPolicy alias_policy_ = AliasPolicy::adaa;

    double drive_db_ = kDriveDbDefault;
    double bias_request_ = 0.0;
    double tone_pre_hz_ = 0.0;  // off by default; the node's table supplies 3000
    double tone_de_hz_ = kTonePreHzDefault;
    double pre_boost_db_ = kPreBoostDbDefault;
    bool tone_tracking_ = true;
    bool tone_active_ = false;
    double mix_ = 1.0;
    double output_trim_ = 1.0;

    // Derived, recomputed only on a parameter change.
    double drive_ = 1.0;
    double bias_ = 0.0;
    double bias_offset_ = 0.0;
    double inv_drive_slope_ = 1.0;
    double inv_drive_sq_slope_ = 1.0;
    double max_gain_ = 1.0;

    double adaa_previous_ = 0.0;
    BiquadT<SampleType> pre_filter_{};
    BiquadT<SampleType> de_filter_{};
    OversamplerT<SampleType> oversampler_{};
    int oversample_latency_ = 0;
    std::vector<SampleType> dry_delay_{};
    std::size_t dry_write_ = 0;
    std::size_t dry_valid_ = 0;
};

using Saturator = SaturatorT<float>;
using Saturator64 = SaturatorT<double>;

}  // namespace pulp::signal
