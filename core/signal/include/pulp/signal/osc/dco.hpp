#pragma once

/// @file dco.hpp
/// OSC-DCO: a divider-clocked oscillator front-end. A DCO of the late-1970s /
/// early-1980s lineage is NOT a free-running analog oscillator whose frequency is
/// a control voltage — it is a timing front-end. A crystal-derived master clock
/// `f_clk` is divided down by a programmable counter to produce one reset
/// (terminal-count) event per output cycle, and a waveshaper builds the audio from
/// that periodic reset. OSC-DCO owns exactly that timing front-end; the actual
/// waveform is shaped by the SHARED bandlimited stack (`VaOscillator` — itself
/// `PhaseAccumulator` + the BLEP/BLAMP kernels), which OSC-DCO composes and does
/// not re-implement.
///
/// ── What a DCO is, and what makes the model faithful ──────────────────────
///
/// Two properties are the *point* of the architecture, not defects to smooth away:
///
///   1. **Pitch is quantized.** An integer divider can only realise the discrete
///      set `f_clk / N`. The achievable pitch nearest an equal-tempered note
///      carries a residual error that is LARGER at high notes (small N, widely
///      spaced) than low ones (large N, closely spaced). A faithful DCO computes
///      its increment from the quantized divider `N` (i.e. from `f_clk / N`), not
///      from the commanded note, so the quantization falls out for free. A model
///      that renders every note dead-on-pitch has idealized away the one thing
///      that makes a DCO a DCO. The worst-case error is
///
///          |e|_max <= 865.617 * f_note / f_clk   cents,
///
///      inversely proportional to the master clock and directly proportional to
///      the note frequency (it doubles for every octave up).
///
///   2. **No drift.** The reference is a crystal-stable clock, so a DCO has NO
///      per-voice pitch drift of the kind a VCO exhibits. Its characteristic
///      imperfection is quantization, NOT drift. This is the clean line separating
///      OSC-DCO's timing concerns from OSC-VCO's analog-character concerns: an
///      OSC-DCO profile carries no drift / jitter-depth parameter, and adding one
///      would contradict the architecture. `DcoOscillator` accordingly exposes no
///      such knob, and its rendered pitch is stationary over any render length.
///
/// ── The two divider schemes ───────────────────────────────────────────────
///
/// **Integer-N.** `N = round(f_clk / f_note)`; the reset interval is exactly `N`
/// master clocks, i.e. perfectly periodic in continuous time. This maps to the
/// free-running path: `increment = (f_clk / N) / f_s` and `PhaseAccumulator::
/// advance(increment)` — the natural `wrap_forward` IS the divider reset, band-
/// limited by the shared BLEP path. No forced sync is required, and this is the
/// honest statement rather than a shortcut: because the increment is built from
/// the quantized `f_clk / N`, the note-dependent quantization error is reproduced
/// exactly, and the period carries no jitter.
///
/// **Fractional-N.** A later refinement replaces the fixed integer divider with a
/// `B`-bit accumulator that adds a tuning word `Δ = round(f_note · 2^B / f_clk)`
/// each master-clock tick and resets on carry-out of the top bit, giving
/// `f_osc = f_clk · Δ / 2^B`. The average pitch can be placed arbitrarily close to
/// the note (the frequency step is a constant `f_clk / 2^B` in Hz, so the residual
/// cents error `≈ 865.617 · f_clk / (2^B · f_note)` shrinks with `B` and is
/// LARGEST at low notes — the opposite note-dependence of integer-N). That
/// accuracy is bought with a deterministic **period jitter**: to realise a
/// non-integer average, the reset interval alternates between `floor(f_clk/f_osc)`
/// and `ceil(f_clk/f_osc)` master clocks, and each reset lands on an integer
/// master-clock edge — a timing deviation bounded by one clock. The accumulator is
/// integer arithmetic, so the reset schedule is exact and bit-identical across
/// platforms (`reset_intervals()` exposes it), and the reset is driven as a
/// `PhaseAccumulator::advance_synced(increment, sync_frac, 0.0)` at the clock-grid
/// position, re-anchoring the phase every cycle so the ±1-clock jitter is rendered
/// band-limited rather than smeared or idealized.
///
/// The per-cycle increment is `f_s`-clocks / this-cycle's-clock-count, so the ramp
/// reaches the reset exactly at the grid edge and the reset is a single clean step
/// there. This differs from a strictly constant-slope (`f_osc/f_s`) ramp by the
/// per-cycle slope variation of ±1/N_avg; the constant-slope accumulator-MSB
/// micro-structure is a sub-clock detail safe to idealize, and the per-cycle
/// increment is what lets the shared BLEP path place one clean, correctly-jittered
/// step per cycle without a caller-supplied-slope shaper extraction.
///
/// ── The seam ──────────────────────────────────────────────────────────────
///
/// OSC-DCO produces the phase and `PhaseEvent`s; the shared `VaOscillator` shapes
/// them. OSC-DCO shapes nothing — shape, pulse width, and band-limited correction
/// are the core's. This is why the front-end is thin.
///
/// RT contract: no allocation, no locks, no I/O on the per-sample path. `double`
/// throughout, matching the core. The reset schedule is exact integer arithmetic
/// plus the core's libcalls (`sin` on the sine shape); none of it allocates,
/// locks, or blocks.

#include "phase.hpp"
#include "va.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pulp::signal::osc {

/// The divider scheme a profile selects.
enum class DcoDivider {
    integer_n,    ///< Fixed integer divider `N`; perfectly periodic, quantized pitch.
    fractional_n, ///< `B`-bit fractional accumulator; accurate average pitch, ±1-clock jitter.
};

/// OSC-DCO profile — data-driven timing parameters, no waveform or hardware
/// capture. `N` (integer) and `Δ` (fractional) are DERIVED from a commanded note
/// plus this profile; they are the equality-test targets, not stored fields.
///
/// There is deliberately NO drift / jitter-depth parameter: a DCO's clock is
/// crystal-stable, so its imperfection is quantization, not drift. Waveshaper
/// selection is not a DCO-profile field either — it belongs to the shared shaping
/// stage this profile feeds.
struct DcoProfile {
    /// `f_clk` in Hz. Sets both the pitch-quantization scale (`|e| ∝ 1/f_clk`) and
    /// the fractional jitter grid.
    double master_clock_hz = 8'000'000.0;
    /// Selects the integer-N (perfectly periodic) or fractional-N (accurate
    /// average, jittered) path.
    DcoDivider divider_scheme = DcoDivider::integer_n;
    /// `B` — accumulator width, fractional-N only. Sets the tuning resolution
    /// `f_clk / 2^B` and the reset-jitter quantum. Clamped to [1, 32] on use.
    int accumulator_bits = 24;
    /// Footage / octave selection, in octaves relative to the commanded note.
    /// Positive is up (4' is one octave above 8'). The scaling is an exact power
    /// of two, so the cents error tracks `|e|` at the *shifted* pitch — a lower
    /// footage means a smaller error.
    int octave_shift = 0;
    /// Global detune folded into the note before `N` / `Δ` derivation, in cents.
    double fine_tune_cents = 0.0;
};

/// A divider-clocked oscillator front-end composing the shared `VaOscillator`.
///
/// Typical use: `prepare(sample_rate)`, `set_profile(...)`, `set_shape(...)`,
/// `set_note_hz(...)`, then `next()` per sample. The derived divider (`divider_n`
/// / `tuning_word`), the realised frequency, and the quantization error are
/// exposed so a test can assert the quantization on the integer / rational domain
/// (which survives cross-platform) as well as on the rendered audio.
class DcoOscillator {
public:
    void prepare(double sample_rate) noexcept {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        recompute();
    }

    void set_profile(const DcoProfile& profile) noexcept {
        profile_ = profile;
        recompute();
    }
    const DcoProfile& profile() const noexcept { return profile_; }

    /// Command the pitch. The divider (`N` or `Δ`), the realised frequency, and
    /// the reset schedule are derived here from the note and the profile.
    void set_note_hz(double note_hz) noexcept {
        note_hz_ = note_hz;
        recompute();
    }
    double note_hz() const noexcept { return note_hz_; }

    // ── Shared shape stage (delegated; OSC-DCO shapes nothing) ───────────────
    void set_shape(VaShape shape) noexcept { core_.set_shape(shape); }
    VaShape shape() const noexcept { return core_.shape(); }
    void set_pulse_width(double width) noexcept { core_.set_pulse_width(width); }
    double pulse_width() const noexcept { return core_.pulse_width(); }

    // ── Derived timing facts (the exact equality-test targets) ───────────────
    /// Integer divider `N = round(f_clk / f_note_eff)`. 0 for the fractional
    /// scheme (use `tuning_word()` there).
    long long divider_n() const noexcept {
        return profile_.divider_scheme == DcoDivider::integer_n ? divider_ : 0;
    }
    /// Fractional tuning word `Δ = round(f_note_eff · 2^B / f_clk)`. 0 for the
    /// integer scheme.
    long long tuning_word() const noexcept {
        return profile_.divider_scheme == DcoDivider::fractional_n
                   ? static_cast<long long>(delta_)
                   : 0;
    }
    /// The realised frequency the divider actually produces: `f_clk / N` for
    /// integer-N, `f_clk · Δ / 2^B` (the accurate average) for fractional-N.
    double realized_hz() const noexcept { return realized_hz_; }
    /// The effective note after footage and fine-tune — the target the divider
    /// approximates.
    double effective_note_hz() const noexcept { return effective_note_hz_; }
    /// Pitch-quantization error in cents: `1200 · log2(realized / f_note_eff)`.
    /// For BOTH schemes this obeys `|detune| <= quantization_bound_cents()`
    /// whenever the divider is not clamped — integer-N: `f_note_eff < 2·f_clk`
    /// (the `N ≥ 1` lower clamp); fractional-N: additionally below the `Δ ≤ 2^B−1`
    /// upper clamp, i.e. roughly `f_note_eff < f_clk`. Both hold for any audio note
    /// well below the master clock; `quantization_bound_cents()` returns 0 (no
    /// bound) in the clamp regimes rather than a false one.
    double detune_cents() const noexcept {
        return (realized_hz_ > 0.0 && effective_note_hz_ > 0.0)
                   ? 1200.0 * std::log2(realized_hz_ / effective_note_hz_)
                   : 0.0;
    }
    /// The worst-case quantization bound in cents for the active scheme — an
    /// EXACT upper bound on `|detune_cents()|`, not a linearization.
    ///
    /// The divider quantizes to the nearest integer, so the fractional part of the
    /// ideal divider is bounded by ½ LSB; but the resulting error in cents is
    /// ASYMMETRIC in log-frequency. Rounding the divider DOWN (realized pitch
    /// HIGHER) detunes MORE than rounding it up by the same ½ LSB, because
    /// `log2(x/(x-½))` exceeds `log2((x+½)/x)`. The bound is therefore the
    /// round-down (larger) side, `1200 · log2(x / (x − ½))`, where `x` is the ideal
    /// divider: `f_clk / f_note` (integer-N) or `2^B · f_note / f_clk`
    /// (fractional-N). The older linear form `865.617/x` (= `1200 / (2 ln2 · x)`,
    /// the tangent at ½ LSB) UNDERSTATED exactly the round-down side and was
    /// violated by any note that rounds down — so it was not a true bound.
    ///
    /// The note-dependence is unchanged: integer-N grows with the note (doubles
    /// per octave up, exact at the bottom), fractional-N is largest at LOW notes
    /// and shrinks with `B`.
    double quantization_bound_cents() const noexcept {
        if (!(profile_.master_clock_hz > 0.0) || !(effective_note_hz_ > 0.0)) return 0.0;
        const double ideal_divider =
            profile_.divider_scheme == DcoDivider::integer_n
                ? profile_.master_clock_hz / effective_note_hz_
                : static_cast<double>(two_b_) * effective_note_hz_ / profile_.master_clock_hz;
        // A note at/above 2·f_clk drives the ideal divider below ½ LSB, where the
        // divider clamp — not rounding — governs and the ½-LSB envelope no longer
        // applies. Report no bound there rather than a false one (unreachable for
        // any audio note below the master clock).
        if (!(ideal_divider > 0.5)) return 0.0;
        // Fractional-N also clamps the tuning word at the TOP (`Δ ≤ 2^B − 1`): as
        // the note approaches the clock the ideal `Δ` exceeds that and the clamp,
        // not ½-LSB rounding, governs the realized pitch — so the envelope does not
        // apply there either. Report no bound rather than a false tiny one.
        if (profile_.divider_scheme == DcoDivider::fractional_n &&
            !(ideal_divider < static_cast<double>(two_b_) - 0.5))
            return 0.0;
        return 1200.0 * std::log2(ideal_divider / (ideal_divider - 0.5));
    }

    /// Fill `out` with the next divider reset intervals, in master clocks, from a
    /// fresh divider state — a pure, deterministic view of the reset schedule.
    /// Integer-N returns `N` in every slot; fractional-N alternates
    /// `floor(2^B/Δ)` / `ceil(2^B/Δ)` around the average `2^B/Δ`, which is the
    /// ±1-clock period jitter in the integer domain. Returns the number written.
    int reset_intervals(std::span<long long> out) const noexcept {
        const int n = static_cast<int>(out.size());
        if (profile_.divider_scheme == DcoDivider::integer_n) {
            for (int i = 0; i < n; ++i) out[static_cast<std::size_t>(i)] = divider_;
            return n;
        }
        std::uint64_t residue = 0;
        for (int i = 0; i < n; ++i)
            out[static_cast<std::size_t>(i)] = advance_divider(residue);
        return n;
    }

    /// Reset the phase and the divider schedule to a known start. A DCO reset is
    /// fully deterministic — no seeded state, because there is no noise.
    void reset(double phase = 0.0) noexcept {
        core_.reset(phase);
        residue_ = 0;
        if (profile_.divider_scheme == DcoDivider::fractional_n) {
            current_interval_ = advance_divider(residue_);
            clocks_to_reset_ = static_cast<double>(current_interval_);
        } else {
            current_interval_ = divider_;
            clocks_to_reset_ = 0.0;
        }
    }

    double phase() const noexcept { return core_.phase(); }

    /// Generate one sample and advance the divider by one output sample.
    double next() noexcept {
        if (profile_.divider_scheme == DcoDivider::integer_n) {
            // The natural wrap IS the reset; a constant increment keeps the period
            // exactly N clocks, so integer-N carries no jitter.
            return core_.next(increment_integer_);
        }

        // Fractional-N: the reset lands on the master-clock grid. The per-cycle
        // increment carries the phase to exactly 1.0 at the grid edge, so a single
        // clean step is emitted there; re-anchoring to 0 each cycle keeps the
        // ±1-clock jitter exact rather than accumulating slope error.
        const double increment = clocks_per_sample_ / static_cast<double>(current_interval_);
        double out;
        if (clocks_to_reset_ < clocks_per_sample_) {
            const double frac = clocks_to_reset_ > 0.0 ? clocks_to_reset_ / clocks_per_sample_ : 0.0;
            out = core_.next_synced(increment, frac, 0.0);
            const long long next_interval = advance_divider(residue_);
            clocks_to_reset_ += static_cast<double>(next_interval);
            current_interval_ = next_interval;
        } else {
            out = core_.next(increment);
        }
        clocks_to_reset_ -= clocks_per_sample_;
        return out;
    }

private:
    /// Advance the fractional accumulator by one divider cycle from `residue` (its
    /// value just past the previous carry, in [0, Δ)), returning the cycle's clock
    /// count and leaving `residue` at the next post-carry value. Pure integer
    /// arithmetic — the reset schedule is therefore bit-identical everywhere,
    /// which is what lets the quantization be asserted on the integer domain.
    long long advance_divider(std::uint64_t& residue) const noexcept {
        // Smallest m with residue + m·Δ >= 2^B (ceil division), which alternates
        // between floor(2^B/Δ) and ceil(2^B/Δ) as the residue walks.
        const std::uint64_t m = (two_b_ - residue + delta_ - 1) / delta_;
        residue = residue + m * delta_ - two_b_;
        return static_cast<long long>(m);
    }

    void recompute() noexcept {
        clocks_per_sample_ = sample_rate_ > 0.0 ? profile_.master_clock_hz / sample_rate_ : 0.0;

        const int bits = profile_.accumulator_bits < 1    ? 1
                         : profile_.accumulator_bits > 32 ? 32
                                                          : profile_.accumulator_bits;
        two_b_ = std::uint64_t{1} << bits;

        effective_note_hz_ =
            note_hz_ * std::exp2(static_cast<double>(profile_.octave_shift) +
                                 profile_.fine_tune_cents / 1200.0);

        const double f_clk = profile_.master_clock_hz;
        if (profile_.divider_scheme == DcoDivider::integer_n) {
            const double ideal = effective_note_hz_ > 0.0 ? f_clk / effective_note_hz_ : 1.0;
            divider_ = static_cast<long long>(std::llround(ideal));
            if (divider_ < 1) divider_ = 1;
            realized_hz_ = divider_ > 0 ? f_clk / static_cast<double>(divider_) : 0.0;
            increment_integer_ = sample_rate_ > 0.0 ? realized_hz_ / sample_rate_ : 0.0;
            delta_ = 1;
        } else {
            const double ideal =
                f_clk > 0.0 ? effective_note_hz_ * static_cast<double>(two_b_) / f_clk : 1.0;
            long long d = static_cast<long long>(std::llround(ideal));
            if (d < 1) d = 1;
            if (static_cast<std::uint64_t>(d) >= two_b_) d = static_cast<long long>(two_b_ - 1);
            delta_ = static_cast<std::uint64_t>(d);
            realized_hz_ = f_clk * static_cast<double>(delta_) / static_cast<double>(two_b_);
            divider_ = 0;
            increment_integer_ = 0.0;
        }

        reset(phase());
    }

    VaOscillator core_;

    DcoProfile profile_{};
    double sample_rate_ = 48000.0;
    double note_hz_ = 261.625565; // C4.

    // Derived.
    double clocks_per_sample_ = 0.0;
    double effective_note_hz_ = 261.625565;
    double realized_hz_ = 0.0;
    long long divider_ = 1;         // integer-N.
    std::uint64_t delta_ = 1;       // fractional-N tuning word Δ.
    std::uint64_t two_b_ = 1u << 24;
    double increment_integer_ = 0.0;

    // Fractional-N running schedule.
    std::uint64_t residue_ = 0;
    long long current_interval_ = 1;
    double clocks_to_reset_ = 0.0;
};

} // namespace pulp::signal::osc
