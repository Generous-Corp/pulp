#pragma once

#include <pulp/signal/detail/modular_sequencing_common.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

// ── Quantizer ─────────────────────────────────────────────────────────────

/// How a `QuantizeScaleT` divides the octave.
enum class QuantizeMode {
    edo,         ///< `N` equal steps per octave.
    scale_mask,  ///< 12-TET, then snapped to a 12-bit pitch-class mask.
};

/// CV-to-scale quantizer on the 1 V/octave standard.
///
/// *(informative)* In 12-tone equal temperament the octave is twelve equal
/// semitones of 100 cents, so on the 1 V/oct standard a pitch plays in tune when
/// it is a multiple of 1/12 V — "steps of exactly 1/12 V = one semitone"
/// [illustrated by public quantizer documentation, e.g. Doepfer A-156; 12-TET
/// itself is common practice, cited for illustration only]. The pitch-class
/// bitmask convention is shared verbatim with the MIDI series' scale lock, so a
/// scale authored on either side means the same thing here.
///
/// **Hysteresis is the load-bearing part.** A quantizer without it chatters
/// whenever its input hovers on a step boundary — an LFO peak, a slow envelope,
/// a noisy CV — and the chatter is audible as a trill, not as jitter. A step
/// change therefore requires the input to cross the boundary by
/// `hyst_cents`, and the latch is the block's only state.
///
/// The latch holds the **chromatic** (pre-mask) step rather than the snapped
/// output. That is what makes the hysteresis window a fixed number of cents of
/// *input* travel: if it latched the output, the window would silently widen and
/// narrow with the gaps in the scale, so a semitone-wide leading tone and a
/// minor-third gap would debounce differently.
///
/// **Glide is not baked in.** The output is a stepped voltage; a caller wanting
/// smooth transitions puts a `SlewLimiterT` (kit) downstream, which keeps this
/// block pure and lets the same quantizer feed a glide and a hard-stepped path.
///
/// **USE.** *Taming the rungler or the Cartesian walk* — any wild CV becomes an
/// in-key melody. *Sample-and-hold pitch* — a `SampleHoldT` (kit) on an LFO into
/// the quantizer into an oscillator is the classic random arpeggio.
/// *Microtonal* — EDO-N mode reaches 24-tone quarter tones and 19- or 31-tone
/// tunings without a lookup table.
///
/// RT contract: as the header. One `llround`-free floor and a bounded search.
template <typename SampleType = float>
class QuantizeScaleT {
public:
    /// Steps per octave in `edo` mode. 12 is 12-TET (cited); the cap is ours.
    /// [design parameter] default 12, range 1 .. 48.
    static constexpr int kDefaultEdo = 12;
    static constexpr int kMaxEdo = 48;

    /// Pitch classes of the major scale — bits 0, 2, 4, 5, 7, 9, 11.
    static constexpr std::uint16_t kMajorMask = 0b0000'1010'1011'0101u;

    /// Extra travel, in cents, an input must make past a step boundary before
    /// the output follows it.
    /// [design parameter] default 20 cents, range 0 .. 50 cents.
    static constexpr double kHystCents = 20.0;

    /// Ceiling on the hysteresis setting, one octave. Beyond a full octave the
    /// value is meaningless anyway, since `kMaxHystSteps` caps the window that
    /// actually applies; this bound exists so the arithmetic stays finite.
    /// [design parameter] default 1200 cents ceiling, range 100 .. 12000 cents.
    static constexpr double kMaxHystCents = 1200.0;

    /// Ceiling on the hysteresis window expressed as a fraction of one step.
    ///
    /// A window in *cents* and a step count are independent knobs, and above
    /// EDO-30 the shipped 20-cent default is wider than the half-step boundary
    /// it debounces: one EDO-31 step is 38.71 cents, the boundary sits 19.35
    /// cents away, and 19.35 + 20 = 39.35 cents of required travel exceeds the
    /// 38.71 cents an adjacent step is away. The quantizer would then be unable
    /// to reach the next step *at all* and would lag a monotone input by one
    /// step forever — not anti-chatter, a stuck output. The window is therefore
    /// capped so an adjacent step always stays reachable with margin: required
    /// travel is `0.5 + kMaxHystSteps` = 0.95 of a step, leaving 5 % of a step
    /// of headroom against float noise at the comparison.
    /// [design parameter] default 0.45 step, range 0.1 .. 0.499 step.
    static constexpr double kMaxHystSteps = 0.45;

    /// Widest step index the quantizer will produce, in either direction.
    ///
    /// This is the block that exists to tame *wild* CV — a rungler, a Cartesian
    /// walk, a runaway envelope — so it is the one place in the header that has
    /// to survive an input no musician would send. Casting an unbounded or
    /// non-finite `double` to `int` is undefined behaviour, not a wrong note.
    /// The bound sits far past any real pitch: the widest EDO at ten volts
    /// (EDO-48 at ±10 V) reaches ±480 steps.
    /// [design parameter] default ±4096 steps, range ±512 .. ±2^20.
    static constexpr double kMaxAbsSteps = 4096.0;

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
    }

    void set_mode(QuantizeMode mode) { mode_ = mode; }
    QuantizeMode mode() const { return mode_; }

    void set_edo(int n) { edo_ = std::clamp(n, 1, kMaxEdo); }
    int edo() const { return edo_; }

    void set_scale_mask(std::uint16_t mask) { mask_ = static_cast<std::uint16_t>(mask & 0x0FFFu); }
    std::uint16_t scale_mask() const { return mask_; }

    void set_root_pc(int pc) { root_pc_ = ((pc % 12) + 12) % 12; }
    int root_pc() const { return root_pc_; }

    void set_hysteresis_cents(double cents) {
        hyst_cents_ = seq_detail::clamp_finite(cents, 0.0, kMaxHystCents);
    }
    double hysteresis_cents() const { return hyst_cents_; }

    /// Verb 2: clears the hysteresis latch, so the next input quantizes on its
    /// own merits rather than being held by whatever the pattern left behind.
    void apply_reset_edge() { have_latch_ = false; }

    void reset() {
        have_latch_ = false;
        latched_step_ = 0;
    }

    static constexpr int latency_samples() { return 0; }

    /// The latched pre-mask step, in units of `1/N` octave (`edo` mode) or
    /// semitones (`scale_mask` mode).
    int latched_step() const { return latched_step_; }

    /// Quantizes one sample of pitch CV, in volts.
    SampleType process(SampleType cv_volts) {
        const int divisions = (mode_ == QuantizeMode::edo) ? edo_ : 12;
        const double in_steps = static_cast<double>(cv_volts) * static_cast<double>(divisions);
        const int step = latch(in_steps, divisions);
        const int out_step = (mode_ == QuantizeMode::edo) ? step : snap_to_mask(step);
        return static_cast<SampleType>(static_cast<double>(out_step) /
                                       static_cast<double>(divisions));
    }

private:
    /// Round half up, so 3.5 goes to 4 and −3.5 goes to −3.
    static int round_half_up(double x) {
        return static_cast<int>(
            std::floor(seq_detail::clamp_finite(x, -kMaxAbsSteps, kMaxAbsSteps) + 0.5));
    }

    int latch(double in_steps, int divisions) {
        const int candidate = round_half_up(in_steps);
        if (!have_latch_) {
            have_latch_ = true;
            latched_step_ = candidate;
            return latched_step_;
        }

        // One step spans 1200/divisions cents, so the window in steps is
        // `cents · divisions / 1200`. At 12-TET and the 20-cent default this is
        // 0.2 of a semitone, matching the spec's worked example. The cap only
        // engages above EDO-30 — see `kMaxHystSteps` for why it has to.
        const double window = std::min(hyst_cents_ * static_cast<double>(divisions) / 1200.0,
                                       kMaxHystSteps);
        const double here = static_cast<double>(latched_step_);
        if (candidate > latched_step_ && in_steps < here + 0.5 + window) return latched_step_;
        if (candidate < latched_step_ && in_steps > here - 0.5 - window) return latched_step_;

        latched_step_ = candidate;
        return latched_step_;
    }

    bool allowed(int semitone) const {
        int pc = (semitone - root_pc_) % 12;
        if (pc < 0) pc += 12;
        return ((mask_ >> pc) & 1u) != 0u;
    }

    int snap_to_mask(int semitone) const {
        // An empty mask has no enabled pitch class to snap to. Passing the
        // chromatic step through is the only answer that keeps the output in
        // tune; silently substituting a scale would hide the misconfiguration.
        if (mask_ == 0u) return semitone;
        for (int d = 0; d <= 12; ++d) {
            if (allowed(semitone + d)) return semitone + d;  // ties resolve upward
            if (d > 0 && allowed(semitone - d)) return semitone - d;
        }
        return semitone;
    }

    double sample_rate_ = 44100.0;
    QuantizeMode mode_ = QuantizeMode::scale_mask;
    int edo_ = kDefaultEdo;
    std::uint16_t mask_ = kMajorMask;
    int root_pc_ = 0;
    double hyst_cents_ = kHystCents;

    bool have_latch_ = false;
    int latched_step_ = 0;
};

using QuantizeScale = QuantizeScaleT<float>;
using QuantizeScale64 = QuantizeScaleT<double>;

}  // namespace pulp::signal
