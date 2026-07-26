#pragma once

/// @file phaser_stages.hpp
/// The cascaded-allpass phaser: a 4-stage "Small Stone style" sweep and a
/// general 4–12-stage digital phaser, one engine, two sets of defaults.
///
/// A phaser is a dry signal summed with a chain of first-order allpass
/// sections whose corner frequency is swept by an LFO. It is NOT a comb
/// filter. A flanger's nulls sit at odd multiples of one spacing — harmonic,
/// evenly ruled, metallic. A phaser's nulls sit wherever the cascade's
/// ACCUMULATED phase crosses an odd multiple of π, which the arctangent law
/// below spaces non-uniformly. That non-uniform spacing is the entire sonic
/// identity of the effect (Smith 1984; see the citations at the bottom).
///
/// ## Why this file is not `phaser.hpp`
///
/// `pulp/signal/phaser.hpp` already ships a `PhaserT` — an 87-line prototype
/// with a hard-coded 200 Hz–5 kHz sweep, a hand-rolled sine LFO, a direct-form
/// allpass, mono-only per-sample `process()`, and 2..8 stages. It is NOT dead
/// code: `forge_lofi_catalog.hpp`'s `make_phaser_node()` is built on it, and
/// `test/denormal_null_harness.hpp` renders it as one of the bit-exactness
/// reference paths for the `snap_to_zero` null test. Rewriting it in place
/// would change that reference render and rewrite a shipped catalog node's
/// behaviour as a side effect of adding a new module.
///
/// So this design ships ALONGSIDE, under its own name, and the old one is left
/// to a deliberate migration:
///
///   | | `PhaserT` (phaser.hpp) | `PhaserStagesT` (here) |
///   |---|---|---|
///   | stages | 2..8, any parity | 4..12, even only |
///   | allpass | direct-form `(a+z⁻¹)/(1+az⁻¹)` | TPT, modulation-stable |
///   | LFO | private `std::sin` accumulator | house `LfoT` |
///   | sweep | fixed 200 Hz..5 kHz | `center_hz` ± `depth` |
///   | channels | mono, sample at a time | stereo block, quadrature spread |
///   | feedback bound | ±0.95, unproven | ±0.9, proven and asserted |
///
/// The migration, when someone takes it: point `make_phaser_node()` at
/// `PhaserStagesT`, re-baseline the denormal-null reference in the same commit,
/// then delete `phaser.hpp`. Nothing here blocks on that, and nothing here
/// depends on it.
///
/// ## The math, and where the spec's version of it is wrong
///
/// **Notch count.** N first-order allpass sections carry total phase from 0 at
/// DC to −Nπ at Nyquist. Summed with dry, they cancel wherever that phase
/// crosses an odd multiple of π, so `num_notches = N/2` for even N — Smith's
/// "for each notch in the desired response we must add two new first-order
/// allpass sections". N is restricted to even values here because odd N puts
/// the last crossing exactly at Nyquist, where it is not a notch.
///
/// **Notch position — the analog prototype.** The continuous-time first-order
/// allpass with corner `fc` has phase `−2·arctan(f/fc)`, so an N-section
/// cascade nulls at
///
/// ```
///     f_k = fc · tan( (2k−1)·π / (2N) )          k = 1 .. N/2      (analog)
/// ```
///
/// **Notch position — what this code actually does.** The shipped stage is a
/// bilinear/TPT discretization prewarped at `fc`, so its phase is the analog
/// law with both frequencies replaced by their prewarped tangents:
/// `φ_d(ω) = −2·arctan( tan(ω/2) / tan(ω_c/2) )`. Solving the same odd-π
/// crossing gives the law this module is measured against:
///
/// ```
///     f_k = (fs/π) · arctan( tan(π·fc/fs) · tan((2k−1)·π/(2N)) )   (digital)
/// ```
///
/// Both are shipped: `notch_frequency_hz()` (digital, exact for this code) and
/// `notch_frequency_analog_hz()` (the cited prototype). They are not
/// interchangeable, and the difference is not a rounding detail:
///
/// ```
///   N=4  fc=400   k=2   analog   965.7 Hz   digital   964.6 Hz   +0.11 %
///   N=8  fc=600   k=4   analog  3016.4 Hz   digital  2979.6 Hz   +1.24 %
///   N=12 fc=400   k=6   analog  3038.3 Hz   digital  2999.9 Hz   +1.28 %
///   N=12 fc=2000  k=6   analog 15191.5 Hz   digital 12000.0 Hz  +26.6  %
///   N=12 fc=4000  k=6   analog 30383.0 Hz   digital 17022.3 Hz  +78.5  %   ← above Nyquist
/// ```
///
/// (fs = 48 kHz.) The last row is the point: `center_hz` reaches 2 kHz in the
/// catalog and `depth = 100 %` doubles it, so `fc = 4 kHz` is a reachable
/// setting at which the analog law predicts a notch above Nyquist. The digital
/// law's `arctan` cannot exceed π/2, so it places every one of the N/2 notches
/// strictly below Nyquist — always, at any `fc`. That is a property of the
/// discretization, not a clamp.
///
/// The prototype law is kept, cited, and tested against the digital one in the
/// regime where the spec quotes it (`fc = 400 Hz`, N ≤ 12: they agree to
/// 1.3 %), because it is the closed form that explains the *shape* — every
/// notch is a fixed ratio of `fc`, so the whole pattern glides rigidly in log
/// frequency as the LFO sweeps.
///
/// ## The stage
///
/// One `TptFilterT` per stage, tapped through `process_allpass()`. That is not
/// a convenience: `TptFilterT`'s coefficient (`g = tan(π·fc/fs)`,
/// `G = g/(1+g)`), its trapezoidal state update, and its `2·lowpass − input`
/// allpass tap are already exactly the topology-preserving one-pole allpass
/// this design calls for (Zavalishin 2020, ch. 2). Re-deriving it here would
/// create a second copy of the same three lines that could drift.
///
/// TPT rather than direct form because `fc` moves EVERY SAMPLE. A direct-form
/// allpass whose coefficient is rewritten per sample is a different filter each
/// sample and its state no longer means what the new coefficient assumes; the
/// audible result is a granular edge on fast sweeps. The TPT structure's state
/// is a trapezoidal integrator memory, which stays meaningful across a
/// coefficient change.
///
/// **Cost, stated plainly.** Because each stage owns its coefficient, a sweep
/// costs one `std::tan` per active stage per channel per sample — 8 per sample
/// at the 4-stage default, 24 at 12 stages. That is the price of composing the
/// shared stage instead of hoisting one shared coefficient, and it is the
/// honest cost anyway once `stagger_ratio ≠ 1`, where the stages genuinely
/// have different corners.
///
/// ## The feedback loop — small-signal gain and its bound (series law 1 / 8)
///
/// The "colour" control is a gain-carrying path, so its loop gain is stated and
/// bounded rather than tuned by ear. With `A(z)` the cascade and the feedback
/// tap reading the PREVIOUS sample's cascade output, the wet path is
///
/// ```
///     W(z) = A(z) / (1 − k·z⁻¹·A(z))
/// ```
///
/// An allpass cascade has `|A(e^{jω})| = 1` at every frequency — exactly, by
/// the allpass property, not approximately — and `|z⁻¹| = 1` on the unit
/// circle, so the loop-gain magnitude is exactly `|k|` at every frequency and
///
/// ```
///     sup_ω |W| = 1 / (1 − |k|)
/// ```
///
/// with `kFeedbackMax = 0.9` giving **10.0× (20.0 dB)**. This is a proof, not a
/// fit: the bound is an equality, ATTAINED wherever the loop phase
/// `∠(z⁻¹A)` passes through a multiple of 2π, which it does many times across
/// the band because the cascade's phase sweeps −Nπ. The module's own suite
/// asserts both halves — that the measured peak never exceeds the bound, and
/// that it reaches it — so a future change that quietly weakens the feedback
/// fails a test rather than passing a one-sided inequality.
///
/// Stability follows from the same identity without a second argument: a
/// unity-magnitude element inside a sub-unity feedback gain is the textbook
/// small-gain case. Widening `kFeedbackMax` past 1 does not merely get loud, it
/// makes `sup|W|` infinite; the constant is a stability bound and the registry
/// number is derived from it.
///
/// **What "Colour" actually does, because the usual description is backwards.**
/// It is widely written that phaser feedback deepens the notches. Measured on
/// this code at 4 stages, fc = 400 Hz, mix = 0.5, each figure relative to that
/// setting's own peak:
///
/// ```
///   feedback   deepest null   null at     response peak
///   0.00        88.5 dB        964.6 Hz    1.000  ( 0.0 dB)
///   0.65        19.9 dB       1025.3 Hz    1.929  (+5.7 dB)
///   0.90        27.5 dB       1102.4 Hz    5.499 (+14.8 dB)
/// ```
///
/// Feedback makes the nulls SHALLOWER, moves them upward, and adds a large
/// resonant peak near `fc`. That peak — not a deeper notch — is the vocal,
/// nasal character the "Color" switch is prized for. The algebra is one line:
/// `|W| = 1` only where `cos(∠A − ω) = k/2`, which at `k = 0` is every
/// frequency (hence exact cancellation is possible) and at `k ≠ 0` is a
/// handful of isolated ones (hence it is not).
///
/// The corollary matters for anyone reading the notch laws above: **they are
/// OPEN-LOOP laws.** They describe the cascade `A`, not the loop around it.
/// With feedback engaged the nulls move off them, which is why every
/// notch-position test in the suite sets feedback to zero.
///
/// ## Mix — why 50 % is the full notch
///
/// `out = (1 − mix)·dry + mix·wet`. A notch is a cancellation between two
/// paths, and cancellation is complete only when they carry equal amplitude:
/// `1 − mix = mix`, i.e. `mix = 0.5`. At any other mix the notches stay at the
/// same frequencies — the phase geometry does not depend on mix — but the null
/// only reaches `|1 − 2·mix|`, i.e. a finite dip. Since the response peaks at
/// exactly 1.0 wherever the cascade phase is a multiple of 2π, the notch depth
/// is the closed form
///
/// ```
///     depth_dB(mix) = −20·log10 |1 − 2·mix|
/// ```
///
/// — 0 dB at mix = 0 and mix = 1, 6.02 dB at 0.25 and 0.75, unbounded at 0.5.
/// Note the corollary: `mix = 1.0` is not "more phaser", it is NO phaser. It is
/// the bare allpass cascade, spectrally flat, with only its phase moving.
///
/// This law, like the notch laws, assumes the loop is open. Its premise is that
/// the wet path is unity magnitude, which is true of `A` and not of `W`.
///
/// ## Anti-aliasing policy: none needed, and why that is a claim not a shrug
///
/// Series law 4 asks for an oversampling policy wherever a nonlinearity
/// aliases. There is no nonlinearity here: allpass stages, a feedback sum, and
/// a dry/wet sum are all exactly linear, so no harmonic of an input tone is
/// generated at all and there is nothing to fold. The suite asserts this rather
/// than asserting the absence of a code path — a sine in, at full feedback,
/// produces harmonic-2 and harmonic-3 energy below −100 dB relative to the
/// fundamental.
///
/// The honest caveat the "not applicable" line usually hides: the coefficients
/// ARE modulated per sample, which makes this a linear TIME-VARYING system, and
/// those do produce new frequencies — sidebands at multiples of the LFO rate
/// around each input component. That is the effect, not an artefact. With
/// `rate_hz ≤ 10 Hz` and a phase-modulation index of a few radians, the
/// sidebands sit within a couple of hundred Hz of their carrier and cannot
/// reach Nyquist to fold. Oversampling would not change them.
///
/// ## Latency
///
/// `latency_samples() == 0`. The dry path is a straight wire and the wet path
/// is a chain of delay-free-feedback TPT sections; neither adds buffering. The
/// feedback tap's one-sample memory is the unavoidable recursion of any causal
/// feedback structure, internal to the loop, and is not reportable latency —
/// an impulse produces a non-zero output sample at index 0.
///
/// ## RT contract
///
/// Nothing in this class ever allocates — not even `prepare()`. State is a
/// fixed `std::array` of 2 × 12 `TptFilterT` stages plus two `LfoT`s, all
/// POD-initialised, so a default-constructed instance is already silent and
/// flat. `prepare()`, `reset()`, every `set_*`, and both `process` overloads
/// are allocation-free, lock-free, and I/O-free, and all are safe on the audio
/// thread. Recursive state is denormal-guarded: each stage snaps its own
/// integrator through `snap_to_zero`, and the feedback memory is snapped here.
/// `set_stage_count` and `set_stagger_ratio` are control-rate — they only
/// change which of the fixed stages the per-sample loop visits, and the loop
/// itself is branch-free over that count.
///
/// ## The calibration table — every design parameter in one place
///
/// | Name | Default | Range | Why |
/// |---|---|---|---|
/// | `kFeedbackMax` | 0.9 | fixed | the worst-case-gain proof above depends on it |
/// | `kColorOffFeedback` | 0.0 | preset point | Small Stone "Color" switch, off |
/// | `kColorOnFeedback` | 0.65 | preset point | Small Stone "Color" switch, on |
/// | `kSweepRangeRatio` | 1.0 × `center_hz` | fixed mapping | depth 100 % ⇒ `fc ∈ [0, 2·center]` |
/// | `kSweepFloorHz` | 20.0 | 5 .. 50 Hz | keeps `tan(π·fc/fs)` away from 0 |
/// | `kSweepCeilingRatio` | 0.45 × fs | 0.30 .. 0.49 | keeps the prewarp away from Nyquist |
/// | `kStaggerRatio` | 1.0 (off) | 0.85 .. 1.15 | undocumented catalog extension |
/// | `kStageCountDefault` | 4 | 4 .. 12, even | compact four-stage phaser class |
/// | `kMixDefault` | 0.5 | 0 .. 1 | the full-notch point, above |
///
/// The two Color values are original engineering choices reproducing documented
/// QUALITATIVE behaviour (a switch that makes the notches deeper and more
/// vocal). No source publishes the original network's feedback magnitude as a
/// number, and none is claimed here.
///
/// ## Presets
///
/// - **Small Stone** — `stages 4, rate 0.4 Hz, center 400 Hz, depth 100 %,
///   feedback 0 (or `kColorOnFeedback`), mix 0.5, triangle, spread 0`.
/// - **Wide shimmer** — `stages 6, rate 0.15 Hz, spread 0.25, sine`. Sine
///   lingers at its extrema where triangle glides evenly, so the sweep breathes.
/// - **Jet** — `stages 12, rate 2 Hz, feedback 0.8, mix 0.5, stagger 1.08`.
/// - **Half-notch pad** — `mix 0.25`: same notch frequencies, 6 dB dips.
///
/// ## Citations
///
/// - Smith, J.O. III (1984). *An Allpass Approach to Digital Phasing and
///   Flanging.* Proc. ICMC-84, Paris, pp. 103–109; restated in *Physical Audio
///   Signal Processing*, "Phasing with First-Order Allpass Filters"
///   (W3K, 2010) — notch-count law and the mix-for-full-notch law.
/// - Zavalishin, V. (2020). *The Art of VA Filter Design*, rev. 2.1.2, ch. 2 —
///   the TPT one-pole and its `allpass = 2·lowpass − input` tap.
/// - Aion FX, *Sunstone OTA Phaser* build documentation (a documented clone of
///   the EHX Small Stone "Issue J") — topology only: four OTA allpass stages
///   sharing one control current, and a switched feedback network on the
///   "Color" control. No component value is used. The single-control-current
///   topology is why the default sweep is linear in Hz rather than
///   logarithmic, and why all stages share one `fc` unless `stagger_ratio`
///   is deliberately moved off 1.
///
/// Ensoniq's DP/4 "Phaser-DDL" is the origin of the 12-stage ceiling as a
/// design anchor, and of "Width" as a name for what is `depth` here. Its
/// manual documents the parameter surface and the coarse topology and nothing
/// more; no public source gives its coefficient update, so none is reproduced.
/// Its distinguishing feature — the phaser feeding a delay whose output returns
/// to the phaser — is deliberately out of scope: it belongs to a delay module
/// composing this one, and its stability proof would have to account for both
/// loops multiplicatively rather than adding these bounds.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// A cascaded first-order-allpass phaser. Stereo, 4–12 stages, bounded
/// feedback, LFO sweep with quadrature stereo spread.
template <typename SampleType = float>
class PhaserStagesT {
public:
    /// The LFO shape enum, forwarded verbatim. There is deliberately no
    /// phaser-local copy of it.
    using Wave = LfoWave;

    // ── Calibration constants (the table in the file doc block) ────────────

    /// Fewest stages. Below 4 the cascade produces a single notch and the
    /// effect reads as a phase shift rather than a sweep.
    static constexpr int kMinStages = 4;
    /// Most stages. The DP/4's documented stage count, used as the anchor.
    static constexpr int kMaxStages = 12;
    /// Compact four-stage phaser class: two notches.
    static constexpr int kStageCountDefault = 4;

    /// Feedback magnitude ceiling. The worst-case-gain proof is
    /// `1/(1 − kFeedbackMax)`; widening this without redoing that proof is a
    /// stability change, not a range change.
    static constexpr double kFeedbackMax = 0.9;
    /// The two documented "Color" switch positions, as preset points.
    static constexpr double kColorOffFeedback = 0.0;
    static constexpr double kColorOnFeedback = 0.65;

    /// `fc(t) = center · (1 + depth · kSweepRangeRatio · lfo(t))`.
    /// [design parameter] default 1.0, fixed — it is what makes depth 100 %
    /// mean "sweep from DC to twice centre", which the presets assume.
    static constexpr double kSweepRangeRatio = 1.0;

    /// Sweep floor in Hz. [design parameter] default 20, range 5 .. 50.
    /// The mapping above reaches 0 Hz at depth 100 %, where `tan(π·fc/fs)` is
    /// 0, the stage degenerates to a sign flip, and every notch collapses onto
    /// DC. Any floor below the bottom of hearing removes that degeneracy
    /// without being audible as a limit.
    static constexpr double kSweepFloorHz = 20.0;

    /// Sweep ceiling as a fraction of the sample rate. [design parameter]
    /// default 0.45, range 0.30 .. 0.49. The bilinear prewarp `tan(π·fc/fs)`
    /// diverges at Nyquist; 0.45 keeps the coefficient finite with margin.
    static constexpr double kSweepCeilingRatio = 0.45;

    /// Per-stage corner ratio: stage `i` runs at `fc · kStagger^i`.
    /// [design parameter] default 1.0 (off), range 0.85 .. 1.15. OFF by
    /// default and flagged as an original catalog extension: no public source
    /// documents a hardware phaser staggering its stages, so switching it on
    /// leaves the vintage-behaviour claim behind.
    static constexpr double kStaggerDefault = 1.0;
    static constexpr double kStaggerMin = 0.85;
    static constexpr double kStaggerMax = 1.15;

    /// The full-notch mix point.
    static constexpr double kMixDefault = 0.5;

    /// Default LFO rate. [design parameter] default 0.5 Hz, range
    /// 0.02 .. 10 Hz — the span a single vintage "Rate" pot covers, slow
    /// enough to read as a sweep and fast enough to reach the "jet" idiom.
    static constexpr double kRateDefaultHz = 0.5;

    // ── Notch laws (shipped, so tests compute expectations from them) ──────

    /// π, at the precision the notch laws need.
    static constexpr double kPi = 3.14159265358979323846;

    /// The cited analog-prototype notch law: `f_k = fc · tan((2k−1)π/2N)`.
    /// Exact for the continuous-time cascade, and the closed form that shows
    /// the notch pattern is a fixed ratio set around `fc`. It is an
    /// APPROXIMATION to what this code does — see the file doc block for where
    /// it breaks down, and prefer `notch_frequency_hz` for expectations.
    static double notch_frequency_analog_hz(int k, int stages, double fc_hz) {
        return fc_hz * std::tan(odd_phase_fraction(k, stages));
    }

    /// The exact notch law for the shipped TPT cascade:
    /// `f_k = (fs/π)·arctan( tan(π·fc/fs) · tan((2k−1)π/2N) )`.
    /// Always strictly below Nyquist, for any `fc` and any `k ≤ stages/2`,
    /// because `arctan` cannot reach π/2.
    static double notch_frequency_hz(int k, int stages, double fc_hz,
                                     double sample_rate) {
        const double prewarped = std::tan(kPi * fc_hz / sample_rate);
        return sample_rate / kPi *
               std::atan(prewarped * std::tan(odd_phase_fraction(k, stages)));
    }

    /// How many notches an N-stage cascade produces. `N/2`, for even N.
    static constexpr int notch_count(int stages) { return stages / 2; }

    /// The worst-case output/input magnitude over all frequencies, mixes and
    /// feedback settings — the number Forge's registry row cites. Equality,
    /// not an estimate: see the derivation in the file doc block.
    static constexpr double worst_case_gain() {
        return 1.0 / (1.0 - kFeedbackMax);
    }

    /// Zero. The dry path is a wire and the TPT stages add no buffering.
    static constexpr int latency_samples() { return 0; }

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// A default instance is a Small Stone: 4 stages, 0.5 Hz triangle, 400 Hz
    /// centre, full depth, Color off, mix at the full-notch point, mono.
    PhaserStagesT() {
        set_stagger_ratio(kStaggerDefault);
        set_wave(Wave::triangle);
        set_rate_hz(kRateDefaultHz);
        prepare(44100.0);
    }

    /// Sets the sample rate and returns the instance to a fresh, silent state.
    /// Allocates nothing — every buffer here is a fixed-size member.
    void prepare(double sample_rate) {
        if (sample_rate > 0.0) sample_rate_ = sample_rate;
        for (auto& channel : stages_)
            for (auto& stage : channel)
                stage.prepare(static_cast<SampleType>(sample_rate_));
        for (auto& lfo : lfo_) lfo.prepare(sample_rate_);
        reset();
    }

    /// Clears every stage's integrator, the feedback memories, and both LFO
    /// phases, so a render from here is bit-identical every time.
    void reset() {
        for (auto& channel : stages_)
            for (auto& stage : channel) stage.reset();
        feedback_state_.fill(SampleType{0});
        for (auto& lfo : lfo_) lfo.reset();
        for (int ch = 0; ch < kChannels; ++ch) sweep_hz_[ch] = center_hz_;
    }

    // ── Parameters ────────────────────────────────────────────────────────

    /// Active stage count. Clamped into `[4, 12]` and then rounded DOWN to the
    /// nearest even value — 5 → 4, 7 → 6, 13 → 12 — because odd counts put the
    /// last odd-π crossing exactly at Nyquist where it is not a notch. The
    /// result is readable through `stage_count()`; nothing is silently ignored.
    void set_stage_count(int n) {
        const int clamped = std::clamp(n, kMinStages, kMaxStages);
        stage_count_ = clamped - (clamped % 2);
    }
    int stage_count() const { return stage_count_; }

    /// LFO rate in Hz. Forwarded to both channels' LFOs.
    void set_rate_hz(double hz) {
        if (!std::isfinite(hz)) return;
        for (auto& lfo : lfo_) lfo.set_rate_hz(hz);
    }
    double rate_hz() const { return lfo_[0].rate_hz(); }

    /// Sweep excursion, 0..1. 1.0 swings `fc` across `[0, 2·center]` before
    /// the floor and ceiling clamps apply.
    void set_depth(float depth01) {
        if (!std::isfinite(static_cast<double>(depth01))) return;
        depth_ = std::clamp(static_cast<double>(depth01), 0.0, 1.0);
    }
    double depth() const { return depth_; }

    /// Allpass corner frequency at the LFO's midpoint.
    void set_center_hz(double fc) {
        if (!std::isfinite(fc)) return;
        center_hz_ = clamp_hz(fc);
    }
    double center_hz() const { return center_hz_; }

    /// Feedback ("Color"), clamped to `±kFeedbackMax`. Positive adds a
    /// resonant peak — see the doc block on what it does NOT do — and moves
    /// the nulls off the open-loop law. Negative flattens the whole response
    /// and is a catalog-only extension with no hardware precedent. Both signs
    /// obey the same gain bound.
    void set_feedback(float fb) {
        if (!std::isfinite(static_cast<double>(fb))) return;
        feedback_ = std::clamp(static_cast<double>(fb), -kFeedbackMax,
                               kFeedbackMax);
    }
    double feedback() const { return feedback_; }

    /// Dry/wet, 0..1. 0.5 is the full-notch point; 1.0 is a flat allpass.
    void set_mix(float mix01) {
        if (!std::isfinite(static_cast<double>(mix01))) return;
        mix_ = std::clamp(static_cast<double>(mix01), 0.0, 1.0);
    }
    double mix() const { return mix_; }

    /// Right-channel LFO lead in CYCLES, 0..0.5. 0.25 is quadrature — the
    /// catalog's 90° default. 0 makes the two channels bit-identical.
    void set_stereo_spread(float cycles01) {
        if (!std::isfinite(static_cast<double>(cycles01))) return;
        spread_ = std::clamp(static_cast<double>(cycles01), 0.0, 0.5);
        lfo_[1].set_stereo_offset(spread_);
    }
    double stereo_spread() const { return spread_; }

    /// LFO shape, forwarded verbatim to both channels' LFOs.
    ///
    /// The catalog contract exposes triangle and sine. Direct C++ callers can
    /// pass the wider shared `LfoWave` enum; when they do, stereo spread remains
    /// literally a PHASE offset rather than a normalized-width macro. In
    /// particular, saw at 0.5 cycles is not an inversion (its discontinuity is
    /// phase-shifted), so its side/mid trajectory differs from triangle/sine.
    void set_wave(Wave wave) {
        for (auto& lfo : lfo_) lfo.set_wave(wave);
    }
    Wave wave() const { return lfo_[0].wave(); }

    /// Per-stage corner ratio; 1.0 (the default) means every stage shares one
    /// `fc`, which is the documented single-control-current topology.
    void set_stagger_ratio(double ratio) {
        if (!std::isfinite(ratio)) return;
        stagger_ = std::clamp(ratio, kStaggerMin, kStaggerMax);
        double p = 1.0;
        for (int i = 0; i < kMaxStages; ++i) {
            stagger_pow_[static_cast<std::size_t>(i)] = p;
            p *= stagger_;
        }
    }
    double stagger_ratio() const { return stagger_; }

    /// Seeds both LFOs identically, so the stochastic shapes stay a
    /// phase-shifted copy of each other rather than decorrelating. A
    /// construction/preset choice, never automated (series law 2).
    void set_seed(std::uint32_t seed) {
        for (auto& lfo : lfo_) lfo.set_seed(seed);
    }

    /// The channel's current swept corner frequency in Hz, after clamping —
    /// the quantity the notch laws take as `fc`. Exposed because it is what a
    /// host draws as a sweep readout, and what the stereo-spread and
    /// waveform-shape tests measure directly instead of trying to recover it
    /// from audio.
    double sweep_frequency_hz(int channel) const {
        return sweep_hz_[static_cast<std::size_t>(
            std::clamp(channel, 0, kChannels - 1))];
    }

    // ── Processing ────────────────────────────────────────────────────────

    /// Stereo block process. Safe in place: `out_l`/`out_r` may alias
    /// `in_l`/`in_r`, because each frame's input is read into a local before
    /// its output is written.
    void process(const SampleType* in_l, const SampleType* in_r,
                 SampleType* out_l, SampleType* out_r, int n_frames) {
        const double dry = 1.0 - mix_;
        for (int i = 0; i < n_frames; ++i) {
            const SampleType xl = in_l[i];
            const SampleType xr = in_r[i];
            if (!std::isfinite(static_cast<double>(xl)) ||
                !std::isfinite(static_cast<double>(xr))) {
                reset();
                out_l[i] = out_r[i] = SampleType{0};
                continue;
            }
            const double wl = run_cascade(0, advance_sweep(0), xl);
            const double wr = run_cascade(1, advance_sweep(1), xr);
            out_l[i] = static_cast<SampleType>(dry * static_cast<double>(xl) +
                                               mix_ * wl);
            out_r[i] = static_cast<SampleType>(dry * static_cast<double>(xr) +
                                               mix_ * wr);
        }
    }

    /// Mono block process, driving channel 0 only. The right channel's LFO
    /// still advances — so switching a running instance to stereo does not
    /// jump its phase — but its cascade is not run, so the mono path costs
    /// half of what the stereo one does.
    void process_mono(const SampleType* in, SampleType* out, int n_frames) {
        const double dry = 1.0 - mix_;
        for (int i = 0; i < n_frames; ++i) {
            const SampleType x = in[i];
            if (!std::isfinite(static_cast<double>(x))) {
                reset();
                out[i] = SampleType{0};
                continue;
            }
            const double w = run_cascade(0, advance_sweep(0), x);
            advance_sweep(1);
            out[i] = static_cast<SampleType>(dry * static_cast<double>(x) +
                                             mix_ * w);
        }
    }

private:
    static constexpr int kChannels = 2;

    /// `(2k − 1)·π / (2N)` — the accumulated-phase fraction the k-th notch
    /// sits at. Shared by both notch laws so they cannot disagree about it.
    static double odd_phase_fraction(int k, int stages) {
        return static_cast<double>(2 * k - 1) * kPi /
               (2.0 * static_cast<double>(stages));
    }

    double clamp_hz(double hz) const {
        return std::clamp(hz, kSweepFloorHz, kSweepCeilingRatio * sample_rate_);
    }

    /// Advances one channel's LFO by a frame and returns its clamped `fc`.
    /// Always called for BOTH channels every frame, even in mono, so the two
    /// accumulators stay in lockstep by construction rather than by care: they
    /// carry identical `double` increments from identical starts and differ
    /// only in their phase offset.
    double advance_sweep(int ch) {
        const auto c = static_cast<std::size_t>(ch);
        const double lfo = static_cast<double>(lfo_[c].next());
        const double fc =
            clamp_hz(center_hz_ * (1.0 + depth_ * kSweepRangeRatio * lfo));
        sweep_hz_[c] = fc;
        return fc;
    }

    /// Retunes one channel's active stages to `fc`, runs the feedback sum
    /// through them, and returns the wet sample.
    double run_cascade(int ch, double fc, SampleType x) {
        const auto c = static_cast<std::size_t>(ch);
        auto& stages = stages_[c];
        SampleType v = static_cast<SampleType>(
            static_cast<double>(x) +
            feedback_ * static_cast<double>(feedback_state_[c]));
        for (int i = 0; i < stage_count_; ++i) {
            const auto s = static_cast<std::size_t>(i);
            stages[s].set_cutoff(
                static_cast<SampleType>(clamp_hz(fc * stagger_pow_[s])));
            v = stages[s].process_allpass(v);
        }
        feedback_state_[c] = snap_to_zero(v);
        return static_cast<double>(v);
    }

    double sample_rate_ = 44100.0;
    double center_hz_ = 400.0;
    double depth_ = 1.0;
    double feedback_ = kColorOffFeedback;
    double mix_ = kMixDefault;
    double spread_ = 0.0;
    double stagger_ = kStaggerDefault;
    int stage_count_ = kStageCountDefault;

    std::array<double, kMaxStages> stagger_pow_{};
    std::array<double, kChannels> sweep_hz_{};
    std::array<SampleType, kChannels> feedback_state_{};
    std::array<std::array<TptFilterT<SampleType>, kMaxStages>, kChannels>
        stages_{};
    /// Always `double`, even in the `float` instantiation. The LFO is a
    /// control signal, not audio: quantising the sweep contour to `float`
    /// would put a visible staircase on a slow sweep of a high `center_hz`
    /// for no benefit, and the two channels' lockstep depends on their
    /// accumulators being identical.
    std::array<EffectLfoT<double>, kChannels> lfo_{};
};

using PhaserStages = PhaserStagesT<float>;
using PhaserStages64 = PhaserStagesT<double>;

}  // namespace pulp::signal
