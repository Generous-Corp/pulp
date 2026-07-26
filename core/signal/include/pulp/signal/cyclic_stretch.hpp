#pragma once

/// @file cyclic_stretch.hpp
/// Fixed-cycle splice time-stretch — the S-series sampler algorithm whose
/// ARTEFACTS ARE THE PRODUCT.
///
/// This is the metallic frame-rate flutter and the doubled/stuttered consonants
/// that define the jungle and lo-fi vocal sound. It is deliberately NOT a
/// transparent stretch, and the two things it does not do are the whole design:
/// there is **no correlation search** and there is **no transient detection**.
/// Everything audible about it follows from those two absences.
///
/// ## Honest gap, stated first because it governs everything below
///
/// **No academic paper, patent, or service manual documents Akai's actual
/// timestretch algorithm.** What is documented is the FAMILY it belongs to —
/// overlap-add time-scale modification, whose literature is cited at the bottom
/// of this block — and the user-facing control surface the hardware exposed: a
/// "cycle" value the operator tuned by ear, a stretch percentage, and
/// acknowledged metallic artefacts on non-ideal cycles. The operator's manuals
/// are citable for THAT and nothing more.
///
/// So: the topology and the OLA math are grounded in published theory; the
/// splice scheduler, every frame and crossfade constant, and the √2 gain bound
/// are original design choices derived from the documented control surface and
/// the algorithm family, not from a product implementation. Where this file says
/// "faithful", it means faithful to a documented control surface and a
/// documented artefact description — never to an algorithm anyone published.
///
/// ## What the design is, in one sentence
///
/// SOLA with the similarity search deleted and the analysis grid frozen to a
/// user-set cycle.
///
/// | | SOLA / WSOLA | this |
/// |---|---|---|
/// | analysis position | search ±tolerance for best correlation | rigid snap to the `L` grid |
/// | transients | preserved, because the search aligns onsets | repeated or dropped on the grid |
/// | splice artefact | minimised | kept — it is the product |
/// | duration change | continuous hop retiming | whole-cycle repeat / omit via the snap |
///
/// Every row is the same one decision playing out, which is why the
/// reconstruction is defensible without any internals: delete the search, and
/// the rest is forced.
///
/// ## The mechanism
///
/// ```
///   L = round(fs / cycle_hz)          cycle length
///   N = grain_periods · L             grain length
///   X = round(crossfade_pct/100 · N)  splice overlap, clamped to [1, N/2]
///   S = N − X                         synthesis hop
///   f_flutter = fs / S                the metallic flutter fundamental
///
///   out_pos(g) = g·S                              fixed synthesis grid
///   in_pos(g)  = L · round_half_even(g·S / (r·L))  SNAP to the cycle grid
/// ```
///
/// Because `in_pos` is quantised to whole cycles while `out_pos` advances by a
/// fixed hop, consecutive grains sometimes land on the SAME cell — a repeated
/// cycle, lengthening — and sometimes SKIP one — an omitted cycle, shortening.
/// Averaged over many grains the input advances `S/r` per grain, so duration
/// scales by `r`; locally it is quantised to whole cycles. That single snap is
/// the entire "cyclic" idea, and it needs no accumulator and no randomness.
///
/// **Why the sidebands land at `fs/S`.** For steady input every grain carries
/// near-identical content; what recurs deterministically is the splice window,
/// with period `S` in the output. So the output is approximately the input
/// times a periodic envelope of period `S`, whose Fourier series has components
/// at `k·fs/S` — and multiplying a tone `f₀` by it produces sidebands at
/// `f₀ ± k·fs/S` exactly. Standard OLA-modulation reasoning; the novelty is only
/// that this design KEEPS the modulation instead of minimising it.
///
/// ## Five decisions that are not obvious
///
/// **Round-half-to-even is spelled out, not delegated to `llround`.** The snap
/// has to be bit-reproducible across platforms, and the two obvious library
/// calls disagree at exactly the ties the schedule hits constantly: `llround`
/// rounds half AWAY from zero, and `nearbyint` gives half-to-even only while the
/// FP rounding mode is untouched. `round_half_even` below does it in integer
/// logic so neither can bite. A tie is not a rare event here — `g·S/(r·L)`
/// lands on `.5` on a regular lattice for the small rational ratios this module
/// is used at.
///
/// **The scheduler is clamped at BOTH ends, not just below.** The source spec
/// clamps only the falling-behind case and asserts `in_pos` can never exceed
/// what has been captured "by construction". That is true for `r ≥ 1` and false
/// for `r < 1`: compression reads faster than input arrives, so `in_pos` races
/// ahead of `total_written` and would read uninitialised ring. Both clamps are
/// here, both grid-snapped so a clamp can never break the cyclic invariant, and
/// both deterministic. See `advance_read_cursor`.
///
/// **Grains are produced as late as possible, not as early as possible.** The
/// obvious loop keeps a hop of lookahead buffered so a block boundary cannot
/// starve the output. It also silently forces the latency UP by that lookahead,
/// and worse, it makes the upper clamp engage permanently even at `r = 1` —
/// the scheduler asks for samples one lookahead into the future on every grain.
/// Producing exactly the grains that the current output sample depends on keeps
/// the reported latency at `N` and keeps the clamp disengaged whenever the
/// ratio does not actually require it.
///
/// **A splice's two halves are matched to each other, not to the current
/// parameters.** Each grain fades in over exactly as many samples as its
/// predecessor faded out. At settled lengths those are the same number and the
/// rule is invisible; while a length control is moving it is the difference
/// between a complementary splice and a level jump. See the gain note below.
///
/// **Splice weights are evaluated per sample, not tabulated.** A fade table
/// would be faster in the overlap, but it would make every `set_*` an O(X)
/// transcendental loop — up to 76 800 `sin`/`cos` at 192 kHz — and `set_*` is
/// called per block by the catalog node. Per-sample evaluation costs about two
/// transcendentals per output sample in the worst regime and keeps every setter
/// O(1).
///
/// ## Levels, aliasing, determinism
///
/// **Oversampling policy: none, and the reason is that there is nothing to
/// oversample.** The signal path is multiply-by-weight and sum. No waveshaper,
/// no saturator, no filter, no fractional-rate resampling — grains are read at
/// integer offsets at 1×, which is also why this stretch does not shift pitch.
/// Series law 4 is satisfied vacuously and the policy is stated rather than
/// omitted. (The splice discontinuity does generate broadband energy; that is
/// the artefact, not aliasing, and no amount of oversampling would or should
/// remove it.)
///
/// **Worst-case gain is √2 at settled lengths, and it is proven rather than
/// measured.** In a
/// splice the output is `a·w_out + b·w_in` for source samples `a`, `b` bounded
/// by `A`. Equal-gain has `w_in + w_out = 1` identically, so it can never
/// boost. Equal-power has `cos(πθ/2) + sin(πθ/2)`, maximised at `θ = ½` at
/// `2·sin(π/4) = √2`. For a blended shape the sum is
/// `(1−shape) + shape·(sin+cos) ≤ (1−shape) + shape·√2 ≤ √2`, and the triangle
/// inequality carries that to arbitrary `a`, `b`. Since `X ≤ N/2` is clamped,
/// never more than two grains overlap, so there is no third term. `√2` is
/// therefore a hard bound at SETTLED resolved lengths — not a measured maximum
/// — and `worst_case_gain()` returns it. The suite asserts it against
/// adversarial input rather than trusting the algebra alone, and finds it
/// attained to nine digits (1.414213538) by a DC input. The boost is KEPT
/// rather than normalised away: it is part of the character, and `output_db`
/// exists to make room for it.
///
/// **While the LENGTH controls are moving, the bound is 2, not √2.** This is
/// the one place the derivation has a seam and it is worth being exact about.
/// `cycle_hz`, `grain_periods` and `crossfade_pct` change the grain geometry
/// underneath grains that are already in flight, so the outgoing fade and the
/// incoming fade can stop being complementary — the outgoing grain is still at
/// unity while the incoming one is already up. Two mechanisms keep that
/// bounded. Each grain fades IN over exactly as many samples as its predecessor
/// faded OUT, so a splice stays complementary whenever the geometry allows it
/// at all; and a grain may not start before the grain two back has ENDED, which
/// is the two-grain invariant enforced directly instead of inferred from the
/// hop. Neither guard binds at settled lengths (`2X ≤ N` makes the second one
/// vacuous), so nothing about the steady-state sound changes. With both in
/// place the ceiling is the trivial one — two grains, each weighted at most 1 —
/// and a deliberately violent per-block sweep of every length control across
/// its full range measures 1.975. Without them the same sweep reached 2.35,
/// with four and five grains summing at once. `stretch_ratio`,
/// `crossfade_shape`, `mix` and `output_db` change no length and are free to
/// automate at audio rate.
///
/// **Determinism (series law 2) is structural: there is no RNG anywhere.** The
/// schedule is a pure integer function of `g` while the clamps are disengaged,
/// and of `(g, total_written)` when they engage — and `total_written` is just a
/// count of samples fed in. Nothing to seed. Because production depends only on
/// absolute counters, block size cannot change the result either.
///
/// **Latency is `N`, and that is only the initial fill.** The scheduler cannot
/// emit grain 0 until its `N` source samples exist. There is no steady-state
/// constant group delay for `r ≠ 1` — a time-scaling process does not have one —
/// and this block says so rather than pretending otherwise. The dry path is
/// delayed by exactly `N` so `mix` is phase-honest.
///
/// ## Relationship to `offline_stretch.hpp`
///
/// No overlap. That engine is a whole-input, non-realtime, frequency-domain
/// phase-vocoder chain (STFT/WOLA, peak phase locking, formant shifting,
/// transient relocation, exact output-length locking) whose entire purpose is
/// transparency. This is a realtime, streaming, time-domain, search-free splicer
/// whose purpose is the opposite. Different domain, different execution model,
/// different goal. They are not two implementations of one thing and neither
/// should be expressed in terms of the other.
///
/// RT contract: `prepare(sample_rate)` sizes the capture ring for the maximum
/// `capture_ms` and the grain accumulator for the maximum `grain_periods · L`;
/// it is the only method that allocates. `set_*` recompute the resolved lengths
/// (`L, N, X, S`) and clamp in place — no allocation, no reset. `process` and
/// `reset` are allocation-free and lock-free; `reset` clears the ring,
/// accumulator, pointers and grain counter. The class holds no global/static
/// state and no RNG, so a zero-initialized instance is a valid, silent,
/// deterministic one.
///
/// References. S. Roucos and A. M. Wilgus, "High Quality Time-Scale Modification
/// for Speech", ICASSP-85, vol. 10, pp. 493–496, 1985 — SOLA, the overlap-add
/// time-scaling family this degenerates from. E. Moulines and F. Charpentier,
/// "Pitch-synchronous waveform processing techniques for text-to-speech
/// synthesis using diphones", Speech Communication 9(5–6), pp. 453–467, 1990 —
/// PSOLA, cited as the CONTRAST case: it tracks pitch to place its marks, and
/// this deliberately does not. W. Verhelst and M. Roelands, "An overlap-add
/// technique based on waveform similarity (WSOLA) for high quality time-scale
/// modification of speech", ICASSP-93, vol. 2, pp. 554–557, 1993 — the
/// similarity search this omits on purpose. Akai S950 / S1000 operator's
/// manuals — cited for the CONTROL SURFACE ONLY (a user-tuned cycle, a stretch
/// percentage, documented metallic artefacts); see the honest gap above.
/// Equal-gain and equal-power crossfade weight laws are common practice and are
/// composed from `crossfade.hpp` rather than re-derived.

#include <pulp/signal/crossfade.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// Round half to even, in integer logic.
///
/// Spelled out rather than delegated because the two obvious library calls
/// disagree at exactly the values this schedule hits: `std::llround` rounds half
/// AWAY from zero, and `std::nearbyint` only rounds half-to-even while the FP
/// rounding mode is at its default — which a host, a plugin loaded alongside, or
/// a `-ffast-math` translation unit may have changed. The snap has to be
/// bit-reproducible across platforms, so it does not depend on either.
inline long long round_half_even(double x) noexcept {
    const double floored = std::floor(x);
    const double fraction = x - floored;
    const auto below = static_cast<long long>(floored);
    if (fraction > 0.5) return below + 1;
    if (fraction < 0.5) return below;
    return (below & 1LL) == 0 ? below : below + 1;  // tie → the even neighbour
}

/// One of the two documented sonic regimes. They differ only in dimensionless
/// shape — cycles per grain, overlap fraction, and fade law — so nothing is
/// interpolated between fitted sets (series law 7); they are the same formulas
/// evaluated at two points.
struct CyclicStretchRegime {
    double cycle_hz = 200.0;
    int grain_periods = 1;
    double crossfade_pct = 25.0;
    double crossfade_shape = 1.0;
};

/// "Clean-ish short frame" — one cycle per grain, a generous equal-power splice,
/// `cycle_hz` meant to be tuned to the material's fundamental. At 48 kHz this
/// resolves to `L = 240, N = 240, X = 108, S = 132`, so the flutter sits at
/// 363.6 Hz — high enough to read as a metallic sheen above the pitch rather
/// than a discrete buzz. The "it almost sounds like a real stretch" setting.
/// [design parameters; see the node's parameter table for ranges]
inline constexpr CyclicStretchRegime kCyclicStretchShortFrame{200.0, 1, 45.0, 1.0};

/// "Glitchy long frame" — tens of milliseconds per grain, a short linear splice,
/// `cycle_hz` left wherever it happens to be. At 48 kHz with the default cycle
/// this resolves to `N = 1920` (40 ms), `X = 154`, `S = 1766`, so the flutter
/// sits at 27.2 Hz — an audible robotic buzz with obvious repetition
/// granularity. The jungle / lo-fi vocal.
/// [design parameters; see the node's parameter table for ranges]
inline constexpr CyclicStretchRegime kCyclicStretchLongFrame{200.0, 8, 8.0, 0.0};

/// The fixed-cycle splice stretcher.
template <typename SampleType = float>
class CyclicStretchT {
public:
    // ── Design parameters (the complete roster) ───────────────────────────

    /// The assumed cycle pitch — the user-tuned "cycle" of the control surface.
    /// Not tracked, never inferred: setting it away from the material's
    /// fundamental is what produces the detune buzz, so tracking it would delete
    /// the effect. [design parameter] default 200 Hz, range 20 .. 2000 Hz, log.
    static constexpr double kCycleHzMin = 20.0;
    static constexpr double kCycleHzMax = 2000.0;
    static constexpr double kCycleHzDefault = 200.0;

    /// Whole cycles per grain. [design parameter] default 1, range 1 .. 16.
    static constexpr int kGrainPeriodsMin = 1;
    static constexpr int kGrainPeriodsMax = 16;
    static constexpr int kGrainPeriodsDefault = 1;

    /// Splice overlap as a percentage of the grain. The 50 % ceiling is
    /// structural, not taste: above it three grains would overlap at once, which
    /// breaks both the two-term OLA and the √2 bound derived from it.
    /// [design parameter] default 25, range 1 .. 50.
    static constexpr double kCrossfadePctMin = 1.0;
    static constexpr double kCrossfadePctMax = 50.0;
    static constexpr double kCrossfadePctDefault = 25.0;

    /// 0 = equal-gain (linear), 1 = equal-power (cosine), blended in between.
    /// [design parameter] default 1, range 0 .. 1.
    static constexpr double kCrossfadeShapeDefault = 1.0;

    /// Output duration over input duration. 1.0 is NOT a bypass — grains are
    /// still spliced on the grid, so identity still carries the flutter, which
    /// is faithful to a machine whose 100 % setting was never transparent.
    /// A true bypass is `mix = 0`. [design parameter] default 1.0, range
    /// 0.25 .. 4.0, log.
    static constexpr double kStretchRatioMin = 0.25;
    static constexpr double kStretchRatioMax = 4.0;
    static constexpr double kStretchRatioDefault = 1.0;

    /// Capture ring capacity in ms. Bounds how far a long stretch may fall
    /// behind before it folds forward, and therefore bounds the memory.
    /// [design parameter] default 2000, range 250 .. 8000, log.
    static constexpr double kCaptureMsMin = 250.0;
    static constexpr double kCaptureMsMax = 8000.0;
    static constexpr double kCaptureMsDefault = 2000.0;

    /// Dry/wet in percent; the dry path is delayed by `N` so the blend is
    /// phase-honest. [design parameter] default 100, range 0 .. 100.
    static constexpr double kMixDefault = 100.0;

    /// Post-gain trim, in dB. Its positive half exists so a patch can pull back
    /// the +3.01 dB equal-power splice boost without pretending it is not there.
    /// [design parameter] default 0, range −24 .. +12 dB.
    static constexpr double kOutputDbMin = -24.0;
    static constexpr double kOutputDbMax = 12.0;
    static constexpr double kOutputDbDefault = 0.0;

    /// Floor on `L`. The smallest cycle length that leaves the splice (minimum
    /// `X = 1`) and the grain interior as distinct, non-degenerate regions.
    /// [design parameter, fixed] 4 samples — an engineering floor, not a tuned
    /// value, so it carries no range.
    static constexpr int kMinCycleSamples = 4;

    /// Floor on `S`. The smallest synthesis hop for which consecutive `out_pos`
    /// grid points are distinguishable samples. [design parameter, fixed] 2
    /// samples — likewise an engineering floor.
    static constexpr int kMinHopSamples = 2;

    /// The proven splice bound. See the file doc block for the derivation; this
    /// is not a measured maximum.
    static constexpr double kWorstCaseGain = 1.41421356237309504880;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Sizes the capture ring for `kCaptureMsMax` and the accumulator for the
    /// largest grain the parameter ranges permit, so no later `set_*` can
    /// reallocate. The only method that allocates.
    void prepare(double sample_rate) {
        if (std::isfinite(sample_rate) && sample_rate > 0.0) sample_rate_ = sample_rate;

        // Sized for the extremes of the declared ranges, not for the current
        // settings: `set_capture_ms` and `set_cycle_hz` must never reallocate,
        // and the only way to promise that is to buy the worst case once.
        const auto ring_needed = static_cast<std::size_t>(
            std::ceil(units::ms_to_samples(kCaptureMsMax, sample_rate_))) + 2u;
        ring_mask_ = power_of_two_mask(ring_needed);
        ring_.assign(ring_mask_ + 1u, SampleType{0});

        const auto longest_cycle = static_cast<long long>(
            std::llround(sample_rate_ / kCycleHzMin));
        max_grain_ = std::max<long long>(
            1, longest_cycle * kGrainPeriodsMax);
        // A grain writes `N` samples starting at the current write cursor, and
        // that cursor never runs more than `N` ahead of the read cursor (grains
        // are produced only as far as the current output sample needs). So the
        // live span is `N`, and twice that leaves every slot a full lap between
        // being zeroed and being written again.
        accum_mask_ = power_of_two_mask(static_cast<std::size_t>(2 * max_grain_) + 2u);
        accum_.assign(accum_mask_ + 1u, 0.0);
        accum_epoch_.assign(accum_mask_ + 1u, 0u);

        update_lengths();
        reset();
    }

    /// Never allocates. A zero-initialised instance is already in this state.
    void reset() {
        std::fill(ring_.begin(), ring_.end(), SampleType{0});
        std::fill(accum_.begin(), accum_.end(), 0.0);
        std::fill(accum_epoch_.begin(), accum_epoch_.end(), 0u);
        accum_generation_ = 1u;
        reset_runtime_state();
    }

private:
    void audio_fault_reset() noexcept {
        ++accum_generation_;
        if (accum_generation_ == 0u) accum_generation_ = 1u;
        reset_runtime_state();
    }

    void reset_runtime_state() noexcept {
        write_index_ = 0;
        total_written_ = 0;
        out_read_ = 0;
        grain_ = 0;
        grain_out_pos_ = 0;
        read_pos_ = 0;
        prev_out_pos_ = 0;
        previous_fade_out_ = crossfade_length_;
        end_previous_grain_ = 0;
        end_two_grains_ago_ = 0;
    }

public:

    // ── Parameters ────────────────────────────────────────────────────────
    //
    // None of these resets the schedule or the ring. That is deliberate: a live
    // automation move on `cycle_hz` should slide the buzz, not restart the
    // machine with a click. `reset()` is the only full clear.

    void set_cycle_hz(double hz) {
        if (!std::isfinite(hz)) return;
        cycle_hz_ = std::clamp(hz, kCycleHzMin, kCycleHzMax);
        update_lengths();
    }

    void set_grain_periods(int periods) {
        grain_periods_ = std::clamp(periods, kGrainPeriodsMin, kGrainPeriodsMax);
        update_lengths();
    }

    void set_crossfade_pct(double pct) {
        if (!std::isfinite(pct)) return;
        crossfade_pct_ = std::clamp(pct, kCrossfadePctMin, kCrossfadePctMax);
        update_lengths();
    }

    void set_crossfade_shape(double shape) noexcept {
        if (!std::isfinite(shape)) return;
        crossfade_shape_ = std::clamp(shape, 0.0, 1.0);
    }

    void set_stretch_ratio(double ratio) noexcept {
        if (!std::isfinite(ratio)) return;
        stretch_ratio_ = std::clamp(ratio, kStretchRatioMin, kStretchRatioMax);
    }

    void set_capture_ms(double ms) {
        if (!std::isfinite(ms)) return;
        capture_ms_ = std::clamp(ms, kCaptureMsMin, kCaptureMsMax);
        update_lengths();
    }

    void set_mix(double percent) noexcept {
        if (!std::isfinite(percent)) return;
        mix_ = std::clamp(percent, 0.0, 100.0) * 0.01;
    }

    void set_output_db(double db) noexcept {
        if (!std::isfinite(db)) return;
        output_gain_ = units::db_to_linear(std::clamp(db, kOutputDbMin, kOutputDbMax));
    }

    /// Apply one of the two documented regimes in a single call.
    void set_regime(const CyclicStretchRegime& regime) {
        set_cycle_hz(regime.cycle_hz);
        set_grain_periods(regime.grain_periods);
        set_crossfade_pct(regime.crossfade_pct);
        set_crossfade_shape(regime.crossfade_shape);
    }

    // ── Resolved lengths ──────────────────────────────────────────────────

    /// `L` — cycle length in samples.
    int cycle_samples() const noexcept { return static_cast<int>(cycle_length_); }
    /// `N` — grain length in samples.
    int grain_samples() const noexcept { return static_cast<int>(grain_length_); }
    /// `X` — splice overlap in samples.
    int crossfade_samples() const noexcept { return static_cast<int>(crossfade_length_); }
    /// `S` — synthesis hop in samples.
    int hop_samples() const noexcept { return static_cast<int>(hop_length_); }
    /// The metallic flutter fundamental, `fs / S`, in Hz.
    double flutter_hz() const noexcept {
        return hop_length_ > 0 ? sample_rate_ / static_cast<double>(hop_length_) : 0.0;
    }
    /// The active capture window in samples — what the falling-behind clamp
    /// folds against, which is the configured `capture_ms`, not the allocation.
    long long capture_window_samples() const noexcept { return capture_window_; }

    double sample_rate() const noexcept { return sample_rate_; }
    double stretch_ratio() const noexcept { return stretch_ratio_; }
    double crossfade_shape() const noexcept { return crossfade_shape_; }

    /// The initial fill, reported exactly (series law 5). This is the only
    /// meaningful latency a time-scaling process has: for `r ≠ 1` there is no
    /// steady-state constant group delay to report, because the output is not a
    /// delayed copy of the input at all.
    int latency_samples() const noexcept { return static_cast<int>(grain_length_); }

    /// The proven bound on `|out| / max|in|` — see the file doc block. Returns
    /// the bound for the CURRENT shape; `kWorstCaseGain` is the bound over all
    /// shapes and is what a registry cites.
    double worst_case_gain() const noexcept {
        return (1.0 - crossfade_shape_) + crossfade_shape_ * kWorstCaseGain;
    }

    /// The splice schedule at a given OUTPUT position — no clamping, no buffer
    /// state, a pure function of the resolved lengths and the ratio. Exposed so
    /// the snap rule can be asserted against its closed form directly, rather
    /// than inferred from a render where the bounded-ring clamps have already
    /// folded it.
    long long schedule_input_position(long long out_pos) const noexcept {
        const double ideal = static_cast<double>(out_pos) / stretch_ratio_;
        return cycle_length_ * round_half_even(ideal / static_cast<double>(cycle_length_));
    }

    /// The schedule at grain index `g`, valid while the resolved lengths hold
    /// still — `out_pos(g) = g·S` only as long as `S` has not moved. The live
    /// path uses the running cursor below instead, for the reason given at
    /// `grain_out_pos_`.
    long long grain_input_position(long long g) const noexcept {
        return schedule_input_position(g * hop_length_);
    }

    /// Number of grains emitted since `reset()`.
    long long grain_count() const noexcept { return grain_; }

    /// The next grain's output position, in absolute output samples.
    long long next_grain_out_pos() const noexcept { return grain_out_pos_; }

    /// The live read cursor, in absolute INPUT samples — where the last grain
    /// actually read from after the bounded-ring fold. Exposed because the fold
    /// is the one part of the design a rendered output cannot show directly:
    /// "the stretch is still stretching after ten seconds" and "the stretch
    /// quietly became 1× four seconds ago" produce very similar-looking audio,
    /// and only the cursor distinguishes them.
    long long read_position() const noexcept { return read_pos_; }

    /// Input samples captured since `reset()`.
    long long total_captured() const noexcept { return total_written_; }

    // ── Audio ─────────────────────────────────────────────────────────────

    /// One block. Allocation-free, lock-free, and independent of block size:
    /// every decision is driven by absolute counters, so splitting a render into
    /// different block sizes cannot change a sample of it.
    void process(const SampleType* in, SampleType* out, int n) {
        if (n <= 0) return;
        if (ring_.empty() || accum_.empty() || in == nullptr) {
            std::fill_n(out, n, SampleType{0});
            return;
        }
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(static_cast<double>(in[i]))) {
                audio_fault_reset();
                out[i] = SampleType{0};
                continue;
            }
            ring_[write_index_] = in[i];
            write_index_ = (write_index_ + 1u) & ring_mask_;
            ++total_written_;

            // The initial fill. Grain 0 cannot exist until its N source samples
            // do, and that wait IS the reported latency.
            if (total_written_ <= grain_length_) {
                out[i] = SampleType{0};
                continue;
            }

            // Produce exactly the grains the current output sample depends on.
            // Output sample p is final once every grain starting at or before p
            // has been written, because a later grain starts later than p by
            // construction.
            while (grain_out_pos_ <= out_read_) emit_grain();

            const auto slot = static_cast<std::size_t>(out_read_) & accum_mask_;
            const double wet = accum_epoch_[slot] == accum_generation_
                                   ? snap_to_zero(accum_[slot])
                                   : 0.0;
            accum_[slot] = 0.0;
            accum_epoch_[slot] = accum_generation_;
            ++out_read_;

            // The dry path is delayed by exactly N, so `mix` blends two things
            // that are talking about the same moment. The `+ 1` is load-bearing
            // and was measured, not reasoned: `write_index_` has ALREADY been
            // advanced past the sample just captured, so the newest sample sits
            // at `write_index_ - 1`, and reading `write_index_ - N` delays by
            // N−1. That off-by-one is invisible on a tone and shows up as a
            // one-sample pre-echo on an impulse at `mix` between 0 and 1.
            const auto dry_index = static_cast<std::size_t>(
                (write_index_ + ring_mask_ + 1u -
                 static_cast<std::size_t>(grain_length_ + 1)) & ring_mask_);
            const double dry = static_cast<double>(ring_[dry_index]);

            const double result = output_gain_ * (mix_ * wet + (1.0 - mix_) * dry);
            if (!std::isfinite(result)) {
                audio_fault_reset();
                out[i] = SampleType{0};
            } else {
                out[i] = static_cast<SampleType>(result);
            }
        }
    }

private:
    static std::size_t power_of_two_mask(std::size_t at_least) {
        std::size_t size = 2;
        while (size < at_least) size <<= 1;
        return size - 1u;
    }

    /// Advance the read cursor by what the schedule wants, then fold it back
    /// into the span the capture ring actually holds.
    ///
    /// **Why the cursor is stateful rather than recomputed.** The obvious
    /// implementation evaluates `schedule_input_position(out_pos)` fresh each
    /// grain and clamps the result. That produces the wrong behaviour in a way
    /// that takes a while to notice, because it only shows up after several
    /// seconds: the schedule position grows monotonically away from the write
    /// pointer, so once it is out of range it is out of range FOREVER, and the
    /// clamp pins it to the edge — where it then advances at exactly the rate
    /// the input arrives. The stretch silently becomes 1×. Measured: at `r = 2`
    /// with the default 2000 ms capture, the pin engages after 4 s and every
    /// second after that is unstretched. The source spec's prose asks for the
    /// opposite ("very long stretches LOOP within the trailing window"), and a
    /// loop is what a stateful cursor gives: it is jumped, then keeps
    /// advancing from where it was jumped to, so the jump recurs as a sawtooth
    /// instead of latching.
    ///
    /// Folding by the usable SPAN rather than by the capture window matters
    /// too: a fold of one whole window overshoots the far edge by the grain
    /// length and would need a second clamp to fix, which is how a fold turns
    /// back into a pin.
    ///
    ///   * **Fell behind** (`r > 1`): jump forward one span. Very long stretches
    ///     cycle the trailing captured region rather than demanding a buffer
    ///     nobody has — the bounded-memory promise, kept.
    ///   * **Ran ahead** (`r < 1`): jump back one span. The source spec asserts
    ///     this case cannot arise ("`in_pos` is always <= total_written by
    ///     construction"); it arises on EVERY compression setting, immediately,
    ///     because compression consumes input faster than it arrives. Without
    ///     this branch the module reads ring slots that have not been written
    ///     yet.
    ///
    /// Both jumps are grid-snapped, because the snap is the whole algorithm and
    /// a fold that left the cycle grid would quietly turn the module into a
    /// plain OLA at exactly the settings where it is working hardest.
    void advance_read_cursor(long long out_pos) noexcept {
        read_pos_ += schedule_input_position(out_pos) -
                     schedule_input_position(prev_out_pos_);
        prev_out_pos_ = out_pos;

        const long long newest = total_written_ - grain_length_;
        const long long oldest = std::max<long long>(0, total_written_ - capture_window_);
        const long long span = std::max<long long>(cycle_length_, newest - oldest);

        if (read_pos_ > newest) {
            const long long laps = (read_pos_ - newest + span - 1) / span;
            read_pos_ -= laps * span;
        } else if (read_pos_ < oldest) {
            const long long laps = (oldest - read_pos_ + span - 1) / span;
            read_pos_ += laps * span;
        }

        // Grid-snap, then a final hard clamp: the snap can push a position that
        // was exactly on an edge one cycle past it.
        read_pos_ = (read_pos_ / cycle_length_) * cycle_length_;
        read_pos_ = std::clamp(read_pos_, std::max<long long>(0, oldest),
                               std::max<long long>(0, newest));
    }

    /// Splice weights at overlap phase `theta`, blended between the two laws.
    /// Both endpoints come from `crossfade.hpp` rather than being re-derived —
    /// there is one gain law in this library and this is not the place to grow a
    /// second one.
    void splice_weights(double theta, double& fade_in, double& fade_out) const noexcept {
        double linear_out = 0.0, linear_in = 0.0;
        double power_out = 0.0, power_in = 0.0;
        crossfade_gains(theta, CrossfadeGainLaw::EqualGain, linear_out, linear_in);
        crossfade_gains(theta, CrossfadeGainLaw::EqualPower, power_out, power_in);
        fade_in = linear_in + crossfade_shape_ * (power_in - linear_in);
        fade_out = linear_out + crossfade_shape_ * (power_out - linear_out);
    }

    /// Overlap-add one grain. The only place the schedule is consulted.
    void emit_grain() noexcept {
        // A grain may not start before the one BEFORE its predecessor has
        // finished. That is the two-grain overlap invariant stated directly,
        // and it is what every gain bound in this file rests on.
        //
        // At settled lengths it provably never binds: the grain two back ended
        // at `out_pos − 2S + N = out_pos − N + 2X`, and `2X ≤ N` is clamped, so
        // it ended at or before `out_pos` already. It binds only while the
        // length controls are being swept, which is exactly when a long grain
        // can still be sounding while short grains march past it at a hop that
        // has nothing to do with the length of the one in flight. Measured
        // without this guard, a full-range per-block sweep of every length
        // control reached 2.35× — well past the √2 the design proves — because
        // four and five grains were summing at once.
        const long long out_pos = std::max(grain_out_pos_, end_two_grains_ago_);
        grain_out_pos_ = out_pos;
        advance_read_cursor(out_pos);
        const long long in_pos = read_pos_;

        // This grain fades IN over exactly as many samples as its predecessor
        // faded OUT, not over its own `X`.
        //
        // At settled parameters those are the same number and this is a no-op.
        // They differ only while `crossfade_pct`, `cycle_hz` or `grain_periods`
        // are being automated, and that is precisely when it matters: the two
        // halves of a splice have to be COMPLEMENTARY for their weights to sum
        // to the bound the design proves. Give a splice a short fade-out and a
        // long fade-in and the outgoing grain is still at unity while the
        // incoming one is already most of the way up — the pair sums toward 2,
        // not √2, and the level jumps audibly on an automation move. Matching
        // the pair to the outgoing side keeps every splice complementary no
        // matter how violently the lengths are swept.
        // The first grain has no predecessor to match, so it uses the current
        // overlap. Deriving that from `grain_` rather than from a value seeded
        // at `reset()` is deliberate: a setter called AFTER `prepare()` — which
        // is the normal way to configure this thing — would otherwise leave the
        // seeded value describing the lengths that were in force during
        // `prepare`, and a later `reset()` would re-seed it from the CURRENT
        // lengths instead. Two renders that should be identical then differ in
        // their very first grain.
        const long long previous_fade =
            grain_ == 0 ? crossfade_length_ : previous_fade_out_;
        const long long fade_in = std::clamp<long long>(
            previous_fade, 1, std::max<long long>(1, grain_length_ - crossfade_length_));
        const long long fade_out = crossfade_length_;
        const long long interior_start = grain_length_ - fade_out;

        for (long long k = 0; k < grain_length_; ++k) {
            const auto source = static_cast<std::size_t>(in_pos + k) & ring_mask_;
            double weight = 1.0;
            if (k < fade_in) {
                double rising = 0.0, falling = 0.0;
                splice_weights(static_cast<double>(k) / static_cast<double>(fade_in),
                               rising, falling);
                weight = rising;
            } else if (k >= interior_start) {
                double rising = 0.0, falling = 0.0;
                splice_weights(static_cast<double>(k - interior_start) /
                                   static_cast<double>(fade_out),
                               rising, falling);
                weight = falling;
            }
            const auto slot = static_cast<std::size_t>(out_pos + k) & accum_mask_;
            if (accum_epoch_[slot] != accum_generation_) {
                accum_[slot] = 0.0;
                accum_epoch_[slot] = accum_generation_;
            }
            accum_[slot] += static_cast<double>(ring_[source]) * weight;
        }

        // Advance so the NEXT grain's fade-in lands exactly on this grain's
        // fade-out. The `fade_in` floor is the anti-pile-up guard: without it a
        // hop that has just been automated far below the previous grain's fade
        // length would let a third grain start before the first had finished,
        // and three overlapping grains break the two-term sum the bound rests
        // on. At settled parameters `fade_in ≤ X ≤ N/2 ≤ S`, so the floor never
        // binds and the hop is exactly `S`.
        grain_out_pos_ += std::max(hop_length_, fade_in);
        previous_fade_out_ = fade_out;
        end_two_grains_ago_ = end_previous_grain_;
        end_previous_grain_ = out_pos + grain_length_;
        ++grain_;
    }

    /// Resolve `L, N, X, S` from the controls, with every floor applied. Called
    /// by every setter that can move them; allocation-free by construction,
    /// because `prepare` already bought the worst case.
    void update_lengths() noexcept {
        const double max_cycle_hz = sample_rate_ / static_cast<double>(kMinCycleSamples);
        const double hz = std::min(cycle_hz_, max_cycle_hz);
        cycle_length_ = std::max<long long>(kMinCycleSamples,
                                            std::llround(sample_rate_ / hz));

        grain_length_ = cycle_length_ * grain_periods_;
        if (max_grain_ > 0) grain_length_ = std::min(grain_length_, max_grain_);

        crossfade_length_ = std::llround(crossfade_pct_ * 0.01 *
                                         static_cast<double>(grain_length_));
        // The 50 % ceiling keeps the overlap-add to two grains, which is what
        // both the interior-unity assumption and the √2 bound rest on.
        crossfade_length_ = std::clamp<long long>(crossfade_length_, 1, grain_length_ / 2);

        hop_length_ = std::max<long long>(kMinHopSamples,
                                          grain_length_ - crossfade_length_);

        capture_window_ = std::max<long long>(
            grain_length_ + 1,
            static_cast<long long>(std::llround(
                units::ms_to_samples(capture_ms_, sample_rate_))));
        capture_window_ = std::min<long long>(
            capture_window_, static_cast<long long>(ring_mask_));
    }

    // Controls.
    double sample_rate_ = 48000.0;
    double cycle_hz_ = kCycleHzDefault;
    int grain_periods_ = kGrainPeriodsDefault;
    double crossfade_pct_ = kCrossfadePctDefault;
    double crossfade_shape_ = kCrossfadeShapeDefault;
    double stretch_ratio_ = kStretchRatioDefault;
    double capture_ms_ = kCaptureMsDefault;
    double mix_ = kMixDefault * 0.01;
    double output_gain_ = 1.0;

    // Resolved lengths.
    long long cycle_length_ = 240;
    long long grain_length_ = 240;
    long long crossfade_length_ = 60;
    long long hop_length_ = 180;
    long long capture_window_ = 96000;
    long long max_grain_ = 0;

    // Buffers and cursors. `total_written_` and `out_read_` are ABSOLUTE, which
    // is what makes the render independent of block size.
    /// Captured audio, stored at the instantiation's own precision — storing
    /// it as `double` regardless would be lossless but would also double the
    /// dominant allocation for no benefit, since the input arrives as
    /// `SampleType` in the first place. The OLA accumulator stays `double`
    /// because that one really is summing.
    std::vector<SampleType> ring_{};
    std::vector<double> accum_{};
    std::vector<std::uint64_t> accum_epoch_{};
    std::uint64_t accum_generation_ = 1u;
    std::size_t ring_mask_ = 0;
    std::size_t accum_mask_ = 0;
    std::size_t write_index_ = 0;
    long long total_written_ = 0;
    long long out_read_ = 0;
    long long grain_ = 0;

    /// The next grain's output position, carried as a RUNNING cursor rather than
    /// recomputed as `g·S`.
    ///
    /// Those are the same number only while `S` holds still, and `S` moves the
    /// moment anyone automates `cycle_hz`, `grain_periods` or `crossfade_pct` —
    /// which the spec explicitly wants to be click-free and non-resetting. If
    /// the position were derived, shrinking `S` would make `g·S` jump BACKWARDS
    /// past the read cursor: the loop would fire off dozens of grains at once,
    /// all of them writing into accumulator slots that had already been read and
    /// zeroed, where the writes would sit silently until the ring lapped and
    /// played them back as garbage. A running cursor only ever moves forward.
    long long grain_out_pos_ = 0;

    /// The live read cursor and the output position its last advance was taken
    /// from. Together these turn the stateless schedule into a cursor that can
    /// be jumped without latching — see `advance_read_cursor`.
    long long read_pos_ = 0;
    long long prev_out_pos_ = 0;

    /// The fade-out length the previous grain used, which becomes the next
    /// grain's fade-in length so every splice pair is complementary even
    /// across a parameter change. See `emit_grain`.
    long long previous_fade_out_ = 60;

    /// Output positions where the last two grains ended, so the two-grain
    /// overlap invariant can be enforced directly rather than inferred from
    /// the hop. See the guard at the top of `emit_grain`.
    long long end_previous_grain_ = 0;
    long long end_two_grains_ago_ = 0;
};

using CyclicStretch = CyclicStretchT<float>;
using CyclicStretch64 = CyclicStretchT<double>;

}  // namespace pulp::signal
