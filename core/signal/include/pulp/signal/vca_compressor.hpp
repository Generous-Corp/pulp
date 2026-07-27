#pragma once

/// @file vca_compressor.hpp
/// The VCA compressor: the dbx/Blackmer lineage, as a true-RMS, log-domain,
/// single-time-constant feedforward design.
///
/// This is NOT the transparent modern compressor with a different colour
/// stage bolted on. `FeedforwardCompressorT` and this class differ in WHERE
/// the ballistics live, and that one difference is the whole lineage:
///
/// ```
///   FeedforwardCompressorT   detector → gain computer → TWO-STAGE SMOOTHER → VCA
///   VcaCompressorT           MEAN-SQUARE ONE-POLE → gain computer → VCA
/// ```
///
/// Here the gain computer is strictly memoryless. Every sample of attack and
/// release "feel" is produced once, by the detector's single one-pole running
/// on the INSTANTANEOUS MEAN SQUARE, and nothing downstream re-smooths it.
/// Change the threshold mid-render and the gain moves on the very next sample,
/// because there is no post-gain-computer filter left to ramp through. The
/// modern design cannot do that, and a test asserts the difference rather than
/// leaving it as prose.
///
/// ## Why the lineage is shaped this way
///
/// **True RMS, in the log domain.** Blackmer's detector patent (David E.
/// Blackmer, US 3,681,618, "RMS Circuits with Bipolar Logarithmic Converter",
/// filed 1971, granted 1972-08-01; expired 1989-08-01 under the pre-GATT
/// 17-years-from-grant term) replaces the resistor of a conventional RC
/// averager with a diode at a fixed idle current, so the averaging time
/// constant is set by a current and — driven through a log/antilog pair — the
/// network averages in the LOG domain. That is mathematically true RMS, not
/// peak with a fudge factor. The digital reformulation is the one-pole on
/// `x²` below, read out as `10·log10`. Cited for topology and concept only; no
/// component values, SPICE models or trim data from any commercial part are
/// used here.
///
/// **One time constant, not two.** The averaging RC is a single physical
/// network; it does not know "rising" from "falling". So the user surface here
/// is ONE `time_ms` control, not an attack knob and a release knob. It is not
/// literally symmetric — a real diode conducts forward and leaks in reverse at
/// different rates, and Blackmer's network is documented as fast-attack /
/// slow-decay LOCKED TOGETHER — so the direction split survives as a single
/// fixed internal ratio `k` (release τ = k · attack τ), not as a second knob.
/// The falsifiable form of "the dbx feel" is therefore `release_time /
/// attack_time == k`, and that is what the suite measures.
///
/// **A dB-linear VCA.** The Blackmer gain cell's control law is exponential —
/// gain in dB is linear in control voltage (0.33 dB/mV at 300 K for the
/// four-transistor cell). THAT Corporation's 2180-series ICs, the licensed
/// modern continuation, carry it forward (>120 dB dynamic range, >130 dB gain
/// range, <0.01 % THD; THAT Corporation, "2180-Series Pre-Trimmed Blackmer®
/// VCA" datasheet). In a digital build that collapses to `10^(g/20)` with no
/// free parameters — but it is WHY the whole chain stays in dB from the
/// detector's `10·log10` to that single conversion, with no linear-domain
/// divide-by-ratio anywhere.
///
/// The static characteristic itself is the published feedforward hard/soft-knee
/// gain computer of Giannoulis, Massberg & Reiss, "Digital Dynamic Range
/// Compressor Design — A Tutorial and Analysis", JAES 60(6), pp. 399–408, 2012.
/// It is written out here rather than borrowed from `FeedforwardCompressorT`
/// for two reasons that are not stylistic: that class clamps ratio to `≥ 1`, so
/// it structurally cannot express the negative-ratio mode below, and its
/// accessor is bound to its own parameter set.
///
/// **"OverEasy"** is dbx's trademark for a continuous soft knee (dbx 160,
/// 1976). It is documented, non-numeric product language, so it is treated
/// here as a NAMED MODE — knee width > 0 — whose shape comes from the cited
/// paper, never from a proprietary curve.
///
/// ## Sign convention, stated because it is easy to get backwards
///
/// `gain_reduction_db()` here is SIGNED and `≤ 0`: it is the number added to
/// the makeup gain before the dB→linear conversion, exactly as §3.2/§3.3 of the
/// spec define it. `FeedforwardCompressorT::gain_reduction_db()` returns the
/// opposite convention — a positive magnitude — because its decoupled detector
/// runs a `max()` that is only correct on positives. The two classes disagree
/// deliberately; a caller metering both must not assume one convention. Nothing
/// in THIS class runs a `max()` over the gain signal, so there is no ordering
/// hazard here to justify flipping the sign, and matching the spec's own
/// equations is worth more than matching the sibling class.
///
/// ## Where the direction split biases the RMS reading
///
/// Because `k > 1`, the detector charges faster than it discharges, so on a
/// signal whose instantaneous power VARIES it settles ABOVE the true mean
/// square. On a sine at equilibrium the offset `c = (ȳ − m)/m`, with `m` the
/// true mean square, solves
///
/// ```
///   ( √(1 − c²) − c·arccos(c) ) / π  =  c·r / (1 − r),   r = release_a / attack_a
/// ```
///
/// which at the default `k = 4` puts the reading ~1.5 dB above true RMS. That
/// is a property of the topology, not a defect: a constant-power signal (DC,
/// square wave) still reads its exact true RMS, and it is what makes this
/// detector's number differ from an unbiased RMS average. The suite asserts the
/// measured offset against that closed form, because a criterion demanding
/// true-RMS accuracy from a `k > 1` detector is not achievable by any correct
/// implementation of this topology.
///
/// ## Oversampling policy (series law 4)
///
/// **Not applicable.** The only nonlinearity is the static gain computer, and
/// it operates on the CONTROL-rate `level_db` signal, never on the audio
/// waveform. The audio path is one per-sample linear multiply. There is no
/// audio-rate nonlinearity to alias, so no oversampling and no ADAA.
///
/// ## Topology and worst-case gain (series law 8)
///
/// Feedforward: the detector reads the INPUT, never this class's own output.
/// There is no loop, so there is no stability argument and no small-signal gain
/// to compensate. The gain command is bounded above by `makeup_db` alone and
/// below by `−ceiling_db`; both bounds are asserted by the suite, not assumed.
///
/// RT contract: `prepare()` may allocate — it sizes the lookahead ring buffer
/// for the maximum `lookahead_ms` of the declared range, which is what lets
/// `set_lookahead_ms()` be audio-thread safe. `set_*`, `process*`, `reset()`
/// and every accessor never allocate, never lock, never throw. `process()` is a
/// pure function of (state, input) with no hidden globals. All state is POD; a
/// zero-initialised instance is a valid silent-passthrough instance at unity
/// gain before `prepare()` is called.
///
/// Control-domain values are `double` throughout, matching the house reference
/// `feedforward_compressor.hpp`. The spec sketch writes them as `SampleType`,
/// which would round a `float` instantiation's threshold and time constants
/// through `float` for no benefit — the detector's one-pole coefficient at
/// 500 ms and 192 kHz is ~1e-5, where `float` starts to matter.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/dynamics_core.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::signal {

/// True-RMS, log-domain, single-time-constant feedforward compressor.
template <typename SampleType = float>
class VcaCompressorT {
public:
    // ── Design parameters (the complete roster) ───────────────────────────

    /// Mean-square floor, guarding `log10(0)` and the denormal range on the
    /// detector's recursive state. 1e-12 is −120 dB in power, far below any
    /// signal power the detector meets in practice, so it never clips a
    /// legitimate low level; the top of the range is still ~30 dB under the
    /// quietest audibly relevant level, so raising it inside the range guards
    /// harder without masking signal.
    /// [design parameter, internal — not a catalog parameter]
    /// default 1e-12, range 1e-15 .. 1e-9.
    static constexpr double kFloorLinear = 1e-12;

    /// The attack/release LOCK: release τ = `k` · attack τ. One control sets
    /// both directions in a fixed ratio, which is the architectural claim of
    /// this lineage (see the file doc block).
    /// **Honest gap:** no published number exists for a reference Blackmer
    /// asymmetry ratio, so this is original engineering — an advanced constant,
    /// not a knob users are expected to reach for.
    /// [design parameter] default 4.0, range 2 .. 8.
    static constexpr double kRatioKDefault = 4.0;
    static constexpr double kRatioKMin = 2.0;
    static constexpr double kRatioKMax = 8.0;

    // Control ranges. These mirror the catalog node's parameter table; the
    // table is the canonical declaration and these are the same numbers at the
    // call site, not a second independent one.
    static constexpr double kThresholdDbMin = -60.0;
    static constexpr double kThresholdDbMax = 0.0;
    static constexpr double kRatioMin = 1.0;
    static constexpr double kRatioMax = 20.0;
    static constexpr double kNegRatioMin = -20.0;
    static constexpr double kNegRatioMax = -1.0;
    static constexpr double kKneeDbMin = 0.0;
    static constexpr double kKneeDbMax = 24.0;
    static constexpr double kTimeMsMin = 1.0;
    static constexpr double kTimeMsMax = 500.0;
    static constexpr double kMakeupDbMax = 24.0;
    static constexpr double kLookaheadMsMax = 10.0;

    /// Depth beyond which further gain reduction is not applied, as a POSITIVE
    /// magnitude; the clamp is `−ceiling_db`. Without it the negative-ratio
    /// mode's curve is unbounded below. 96 dB is deeper than the noise floor of
    /// any programme this will meet, so the clamp is inaudible where it is not
    /// wanted and finite where it is.
    /// [design parameter] default 96 dB, range 60 .. 144 dB.
    static constexpr double kCeilingDbDefault = 96.0;
    static constexpr double kCeilingDbMin = 60.0;
    static constexpr double kCeilingDbMax = 144.0;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Sizes the lookahead ring for the worst case in the declared range. May
    /// allocate; it is the only call that does, which is what makes
    /// `set_lookahead_ms()` audio-thread safe.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;

        const auto capacity = static_cast<std::size_t>(std::llround(
                                  units::ms_to_samples(kLookaheadMsMax, sample_rate_))) +
                              1;
        lookahead_.assign(capacity, SampleType{0});

        update_coefficients();
        update_lookahead();
        reset();
    }

    /// Zeroes the detector and the delay line. Never allocates.
    void reset() {
        mean_square_ = 0.0;
        level_db_ = 10.0 * std::log10(kFloorLinear);
        gain_reduction_db_ = 0.0;
        gain_linear_ = units::db_to_linear(makeup_db_);
        std::fill(lookahead_.begin(), lookahead_.end(), SampleType{0});
        write_index_ = 0;
        lookahead_valid_ = 0;
    }

    // ── Controls (real units throughout) ──────────────────────────────────

    void set_threshold_db(double db) {
        threshold_db_ = std::clamp(dynamics::retain_finite(db, threshold_db_),
                                   kThresholdDbMin, kThresholdDbMax);
    }

    /// Positive compression ratio, used when negative-ratio mode is off.
    void set_ratio(double r) {
        ratio_ = std::clamp(dynamics::retain_finite(r, ratio_), kRatioMin, kRatioMax);
    }

    /// "Infinity+": lets the static curve run with a NEGATIVE ratio, so output
    /// keeps falling as input rises past the knee.
    /// **Honest gap:** no citable primary literature parameterises this. It is
    /// an original creative mode — the same continuous gain computer, evaluated
    /// at `R < 0` and floored by `ceiling_db` — not a modelled circuit.
    void set_negative_ratio_mode(bool enabled) { negative_ratio_ = enabled; }

    /// The ratio used when negative-ratio mode is on. Kept separate from
    /// `set_ratio()` so flipping the mode does not destroy the positive setting
    /// the user was on, and so neither setter needs a sign-dependent clamp.
    void set_neg_ratio_amount(double r) {
        neg_ratio_ = std::clamp(dynamics::retain_finite(r, neg_ratio_),
                                kNegRatioMin, kNegRatioMax);
    }

    /// 0 = hard knee; > 0 = OverEasy-style soft knee, total width in dB
    /// (`±width/2` around threshold).
    void set_knee_db(double db) {
        knee_db_ = std::clamp(dynamics::retain_finite(db, knee_db_),
                              kKneeDbMin, kKneeDbMax);
    }

    /// THE time control — one knob for both directions. Sets the release time
    /// constant directly; attack is `time_ms / k`.
    void set_time_ms(double ms) {
        time_ms_ = std::clamp(dynamics::retain_finite(ms, time_ms_),
                              kTimeMsMin, kTimeMsMax);
        update_coefficients();
    }

    /// The fixed attack/release lock. Advanced: exposed so the house
    /// calibration table can tune the asymmetry, not as a performance control.
    void set_attack_release_ratio_k(double k) {
        ratio_k_ = std::clamp(dynamics::retain_finite(k, ratio_k_),
                              kRatioKMin, kRatioKMax);
        update_coefficients();
    }

    void set_makeup_db(double db) {
        makeup_db_ = std::clamp(dynamics::retain_finite(db, makeup_db_),
                                -kMakeupDbMax, kMakeupDbMax);
    }

    /// Clamped to the ceiling `prepare()` sized for, so this can never need an
    /// allocation.
    void set_lookahead_ms(double ms) {
        lookahead_ms_ = std::clamp(dynamics::retain_finite(ms, lookahead_ms_),
                                   0.0, kLookaheadMsMax);
        update_lookahead();
    }

    /// Dry/wet. 1 = fully compressed (series compression); below 1 blends the
    /// EQUALLY DELAYED dry signal, so the mix cannot comb-filter.
    void set_mix(double mix01) {
        mix_ = std::clamp(dynamics::retain_finite(mix01, mix_), 0.0, 1.0);
    }

    void set_ceiling_db(double db) {
        ceiling_db_ = std::clamp(dynamics::retain_finite(db, ceiling_db_),
                                 kCeilingDbMin, kCeilingDbMax);
    }

    // ── Read-only taps ────────────────────────────────────────────────────

    /// The module's entire latency contribution: the lookahead delay, exactly.
    /// The detector and gain computer are zero-latency, so this is 0 at the
    /// default 0 ms lookahead (series law 5). Integer samples, no fractional
    /// interpolation, so the figure is exact rather than nominal.
    int latency_samples() const noexcept { return lookahead_samples_; }

    /// The static characteristic's output level for an input level, in dB — the
    /// gain computer alone, memoryless, with no detector in front of it.
    /// Exposed so a caller or a test can evaluate the curve directly.
    /// The negative-ratio mode passes a NEGATIVE ratio straight through: the
    /// shared equation is continuous across the sign, so "infinity+" needs no
    /// branch of its own — only the `−ceiling_db` floor that
    /// `gain_computer_db()` applies below.
    double static_curve_db(double input_db) const {
        return dynamics::soft_knee_output_db(input_db, threshold_db_, knee_db_, active_ratio());
    }

    /// Gain reduction in dB (SIGNED, ≤ 0 — see the file doc block) the static
    /// curve commands at `input_db`, after the `−ceiling_db` floor.
    double gain_computer_db(double input_db) const {
        return std::max(static_curve_db(input_db) - input_db, -ceiling_db_);
    }

    /// The same value UNCLAMPED, so a test can show where the floor engages and
    /// where it does not. Not part of the audio path.
    double gain_computer_unclamped_db(double input_db) const {
        return static_curve_db(input_db) - input_db;
    }

    /// The ratio currently in force: the negative amount in "infinity+" mode,
    /// the positive ratio otherwise.
    double active_ratio() const noexcept { return negative_ratio_ ? neg_ratio_ : ratio_; }

    /// Metering taps, as of the last processed sample.
    double gain_reduction_db() const noexcept { return gain_reduction_db_; }
    double level_db() const noexcept { return level_db_; }
    double mean_square() const noexcept { return mean_square_; }
    double current_gain_linear() const noexcept { return gain_linear_; }

    /// The detector's two one-pole coefficients, in the `a` of `y += a·(x − y)`
    /// convention of `units::ms_to_onepole_coef`. Exposed because the ballistics
    /// ratio is the module's defining claim, and a test that recomputes the
    /// coefficients itself would be agreeing with its own arithmetic instead of
    /// checking the wiring.
    double attack_coef() const noexcept { return attack_coef_; }
    double release_coef() const noexcept { return release_coef_; }

    // ── Processing ────────────────────────────────────────────────────────

    SampleType process(SampleType input) {
        // Do not admit a non-finite sample into either the direction-switched
        // detector or the lookahead ring: both are recursive state and would
        // otherwise remain poisoned after the bad sample has passed.
        if (!std::isfinite(static_cast<double>(input))) {
            recover_audio_fault();
            return SampleType{0};
        }
        level_db_ = detect_level_db(static_cast<double>(input));
        gain_reduction_db_ = gain_computer_db(level_db_);
        gain_linear_ = units::db_to_linear(gain_reduction_db_ + makeup_db_);

        // The detector read `x[n]` un-delayed above; the AUDIO is what the
        // lookahead delays, which is the whole point — it gives the detector a
        // head start on the sample it is about to gate.
        const double dry = static_cast<double>(delayed(input));
        return static_cast<SampleType>(dry * (mix_ * gain_linear_ + (1.0 - mix_)));
    }

    void process_block(SampleType* io, int n) {
        for (int i = 0; i < n; ++i) io[i] = process(io[i]);
    }

private:
    void recover_audio_fault() noexcept {
        mean_square_ = 0.0;
        level_db_ = 10.0 * std::log10(kFloorLinear);
        gain_reduction_db_ = 0.0;
        gain_linear_ = units::db_to_linear(makeup_db_);
        write_index_ = 0;
        lookahead_valid_ = 0;
    }

    /// Instantaneous mean square through one direction-switched pole, read out
    /// as power dB. `10·log10`, not 20, because the integrator already holds a
    /// squared quantity.
    double detect_level_db(double x) {
        const double p = x * x;
        const double a = p > mean_square_ ? attack_coef_ : release_coef_;
        mean_square_ = snap_to_zero(mean_square_ + a * (p - mean_square_));
        return 10.0 * std::log10(std::max(mean_square_, kFloorLinear));
    }

    /// Pushes `input` into the ring and returns the sample delayed by the
    /// current lookahead. A pass-through at 0 ms.
    SampleType delayed(SampleType input) {
        if (lookahead_samples_ <= 0 || lookahead_.empty()) return input;
        const std::size_t size = lookahead_.size();
        const std::size_t read =
            (write_index_ + size - static_cast<std::size_t>(lookahead_samples_)) % size;
        const SampleType out = lookahead_valid_ >= static_cast<std::size_t>(lookahead_samples_)
                                   ? lookahead_[read]
                                   : SampleType{0};
        lookahead_[write_index_] = input;
        write_index_ = (write_index_ + 1) % size;
        if (lookahead_valid_ < size) ++lookahead_valid_;
        return out;
    }

    /// ONE user time, split by the fixed lock. `units::ms_to_onepole_coef` is
    /// the house τ (63.2 %) convention — deliberately not `BallisticsFilterT`,
    /// whose coefficient is built on an `exp(−2.2/·)` 10–90 % convention, so
    /// composing it would silently redefine what `time_ms` means by a factor of
    /// ln 9 and quietly break the time-constant test.
    void update_coefficients() {
        release_coef_ = units::ms_to_onepole_coef(time_ms_, sample_rate_);
        attack_coef_ = units::ms_to_onepole_coef(time_ms_ / ratio_k_, sample_rate_);
    }

    void update_lookahead() {
        lookahead_samples_ =
            static_cast<int>(std::llround(units::ms_to_samples(lookahead_ms_, sample_rate_)));
        const auto capacity = lookahead_.size();
        if (capacity > 0 && lookahead_samples_ >= static_cast<int>(capacity))
            lookahead_samples_ = static_cast<int>(capacity) - 1;
        if (lookahead_samples_ < 0) lookahead_samples_ = 0;
    }

    double sample_rate_ = 44100.0;

    double threshold_db_ = -20.0;
    double ratio_ = 4.0;
    double neg_ratio_ = -4.0;
    double knee_db_ = 10.0;
    double time_ms_ = 30.0;
    double ratio_k_ = kRatioKDefault;
    double makeup_db_ = 0.0;
    double lookahead_ms_ = 0.0;
    double mix_ = 1.0;
    double ceiling_db_ = kCeilingDbDefault;
    bool negative_ratio_ = false;

    double attack_coef_ = 0.0;
    double release_coef_ = 0.0;
    int lookahead_samples_ = 0;

    double mean_square_ = 0.0;
    double level_db_ = 10.0 * -12.0;  // 10·log10(kFloorLinear), as a constant expression.
    double gain_reduction_db_ = 0.0;
    double gain_linear_ = 1.0;

    std::vector<SampleType> lookahead_{};
    std::size_t write_index_ = 0;
    std::size_t lookahead_valid_ = 0;
};

using VcaCompressor = VcaCompressorT<float>;
using VcaCompressor64 = VcaCompressorT<double>;

}  // namespace pulp::signal
