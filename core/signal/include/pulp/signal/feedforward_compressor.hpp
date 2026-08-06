#pragma once

/// @file feedforward_compressor.hpp
/// The transparent, modern compressor: a log-domain feedforward design with a
/// soft-knee gain computer and a decoupled detector.
///
/// "Feedforward" is the whole topology in one word — the sidechain reads the
/// INPUT, never the compressor's own output. There is no loop, so there is no
/// stability story to tell and no small-signal gain to compensate; the gain
/// trajectory is a pure function of the input.
///
/// Every equation in the signal path comes from Giannoulis, Massberg & Reiss,
/// "Digital Dynamic Range Compressor Design — A Tutorial and Analysis", JAES
/// 60(6), pp. 399–408, 2012. Anything not from that paper is tagged
/// `[design parameter]` with a default and a range.
///
/// ## Why the detector is two stages
///
/// The obvious detector is one pole whose coefficient switches between attack
/// and release depending on whether the level is rising. That design has a
/// discontinuous derivative exactly at the crossover, and material sitting near
/// the threshold crosses it constantly — the artefact is audible as distortion
/// on precisely the signals a compressor is most often asked to handle. The
/// paper's fix, used here from the outset, is to decouple the two:
///
/// ```
/// y1[n] = max( r[n],  α_R·y1[n−1] + (1 − α_R)·r[n] )   // release branch
/// r_s[n] = α_A·r_s[n−1] + (1 − α_A)·y1[n]              // attack smoothing
/// ```
///
/// Stage 1 already picks the faster of "follow immediately" and "release toward
/// it", so stage 2 always has a continuous input and there is no branch for the
/// signal to chatter across.
///
/// **Sign convention, stated because it is easy to get backwards.** `r[n]` above
/// is the gain reduction as a POSITIVE magnitude in dB — the paper's convention.
/// The gain computer naturally produces `g_c = y_L − x_L ≤ 0`, so this class
/// negates once, at the boundary, and the detector works in positives
/// throughout. Running the same `max()` on the negative quantity would make the
/// detector snap instantly toward LESS reduction and ease into more, i.e. swap
/// attack and release. That version still compresses, still sounds like a
/// compressor, and fails a step-response test immediately — which is why there
/// is one.
///
/// ## What the options are for
///
/// - **RMS vs peak.** Peak reads transients literally. RMS integrates them into
///   one program-loudness figure, which is what bus material wants: a peak
///   detector chases the kick, the snare and every guitar transient separately
///   and the result reads as pumping.
/// - **Program-dependent release.** A release that is fast right after a
///   transient and slower once compression has been sustained, avoiding both
///   the thump of recovering under a still-loud passage and the pumping of
///   holding gain down after a transient has passed. The *effect* is documented
///   hardware behaviour; the constants here are original engineering, because
///   no hardware unit's actual time constants are published.
/// - **Lookahead.** Delays the audio, not the sidechain, so gain reduction has
///   already ramped by the time the peak reaches the multiplier. This is what
///   makes a near-zero attack time safe.
/// - **Stereo link.** Both channels' detectors fed by the louder one, so a
///   hard-panned hit does not pull the stereo image toward centre every time it
///   lands. The 0..1 blend crosses the two DETECTOR INPUTS, not two finished
///   gain-reduction values — crossfading after the detector would reintroduce
///   exactly the image shift the link exists to prevent.
///
/// ## Use-case starting points
///
/// - **Vocal levelling** — `−20 dB`, `3:1`, knee 12 dB, attack 15 ms, release
///   150 ms, program-dependent, peak. Wide knee and moderate ratio read as
///   levelling rather than squashing.
/// - **Bus glue** — `−12 dB`, `2:1`, knee 18 dB, attack 30 ms, release 400 ms,
///   RMS at 20 ms.
/// - **Peak-safe limiting** — ratio 100, knee 0 (engage at the ceiling, not
///   early), attack at the 0.05 ms minimum, release 50 ms, lookahead 5 ms,
///   program-dependent OFF (limiting wants one predictable release).
/// - **Parallel compression** — `−30 dB`, `10:1`, attack 3 ms, release 80 ms,
///   auto-makeup OFF and makeup set deliberately hot: the output is blended
///   under the dry signal, so makeup is a mix control, not a restore control.
///
/// ## Out of scope, and why
///
/// Feedback topology is a different design with a different static-curve
/// mapping, not a mode switch. Multiband is N instances plus the existing
/// crossovers. An external sidechain key is a graph-layer routing concern.
/// Analog saturation is explicitly excluded by the clean/modern brief.
/// **Oversampling does not apply** (series law 4, stated rather than silently
/// omitted): there is no waveshaping nonlinearity in the audio path — the only
/// nonlinearity is a smoothed multiplicative gain trajectory, band-limited by
/// the detector's own smoothing at any musically sensible attack time.
/// True-peak (inter-sample) detection would need an oversampled estimator; peak
/// mode here is sample-peak only, flagged rather than folded in silently.
///
/// This ships ALONGSIDE `compressor.hpp` rather than replacing it: the two do
/// not share a parameter layout, so repointing existing callers is a migration
/// with its own compatibility audit, not a drop-in swap.
///
/// RT contract: `prepare()` may allocate — it sizes the lookahead ring buffer
/// to the worst-case `max_lookahead_ms` at the prepared sample rate, so no
/// later `set_lookahead_ms()` can allocate. `set_*`, `process*`, and `reset()`
/// never allocate, never lock, never throw. `process()` is a pure function of
/// (state, input) with no hidden globals. The stereo entry points read both
/// channels before writing either, which the link path requires.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/dynamics_core.hpp>
#include <pulp/signal/dynamics_contract.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// What the sidechain measures.
enum class CompressorDetector : std::uint8_t {
    peak,  ///< Sample peak. Literal, fastest-responding.
    rms,   ///< Running mean-square. Reads program loudness.
};

/// Log-domain feedforward compressor with a soft-knee gain computer.
template <typename SampleType = float>
class FeedforwardCompressorT {
public:
    using Detector = CompressorDetector;

    // ── Design parameters (the complete roster) ───────────────────────────

    /// Level-conversion floor, guarding `log10(0)`. Declared in
    /// `dynamics_core.hpp` alongside the conversions that consume it, and
    /// re-exported here so the call sites and the catalog keep reading one name.
    static constexpr double kLevelEpsilon = dynamics::kLevelEpsilon;

    /// Slow/fast release ratio for program-dependent release.
    /// **Honest gap:** no citable literature specifies this for a general
    /// algorithm — every hardware unit's constants are proprietary. Original
    /// engineering, tuned for a musically smooth transition.
    /// [design parameter] default 4.0, range 2 .. 8.
    static constexpr double kSlowReleaseRatio = 4.0;

    /// Smoothing constant of the "has compression been sustained" detector.
    /// [design parameter] default 200 ms, range 50 .. 1000 ms.
    static constexpr double kSustainTauMs = 200.0;

    /// How much gain reduction counts as sustained.
    /// [design parameter] default 3 dB, range 1 .. 12 dB.
    static constexpr double kSustainThresholdDb = 3.0;

    // Control ranges. These mirror the catalog node's parameter table; the
    // table is the canonical declaration and these are the same numbers at the
    // call site, not a second independent one.
    static constexpr double kThresholdDbMin = -60.0;
    static constexpr double kThresholdDbMax = 0.0;
    static constexpr double kRatioMin = 1.0;
    static constexpr double kRatioMax = 100.0;
    static constexpr double kKneeDbMin = 0.0;
    static constexpr double kKneeDbMax = 24.0;
    static constexpr double kAttackMsMin = 0.05;
    static constexpr double kAttackMsMax = 200.0;
    static constexpr double kReleaseMsMin = 5.0;
    static constexpr double kReleaseMsMax = 4000.0;
    static constexpr double kRmsWindowMsMin = 1.0;
    static constexpr double kRmsWindowMsMax = 50.0;
    static constexpr double kMakeupDbMax = 24.0;

    /// Ceiling on `prepare()`'s `max_lookahead_ms` argument.
    /// [design parameter] default 10 ms, range 0 .. 50 ms.
    static constexpr double kMaxLookaheadMsCeiling = 50.0;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Sizes the lookahead ring buffer for the worst case. May allocate; this
    /// is the only call that does, which is what lets `set_lookahead_ms()` be
    /// audio-thread safe.
    void prepare(double sample_rate, double max_lookahead_ms = 10.0) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        max_lookahead_ms_ = std::clamp(max_lookahead_ms, 0.0, kMaxLookaheadMsCeiling);

        const auto capacity = static_cast<std::size_t>(
            std::llround(units::ms_to_samples(max_lookahead_ms_, sample_rate_))) + 1;
        for (auto& ring : lookahead_) ring.assign(capacity, SampleType{0});

        update_coefficients();
        update_lookahead();
        update_makeup();
        reset();
    }

    /// Zeroes detector state and the lookahead buffer. Never allocates.
    void reset() {
        for (auto& ch : channels_) ch = ChannelState{};
        for (auto& ring : lookahead_) std::fill(ring.begin(), ring.end(), SampleType{0});
        write_index_[0] = 0;
        write_index_[1] = 0;
        lookahead_valid_[0] = 0;
        lookahead_valid_[1] = 0;
    }

    // ── Controls (real units throughout) ──────────────────────────────────

    void set_threshold_db(double db) {
        threshold_db_ = std::clamp(dynamics::retain_finite(db, threshold_db_),
                                   kThresholdDbMin, kThresholdDbMax);
        update_makeup();
    }

    void set_ratio(double r) {
        ratio_ = std::clamp(dynamics::retain_finite(r, ratio_), kRatioMin, kRatioMax);
        update_makeup();
    }

    void set_knee_width_db(double w) {
        knee_db_ = std::clamp(dynamics::retain_finite(w, knee_db_),
                              kKneeDbMin, kKneeDbMax);
        update_makeup();
    }

    void set_attack_ms(double ms) {
        attack_ms_ = std::clamp(dynamics::retain_finite(ms, attack_ms_),
                                kAttackMsMin, kAttackMsMax);
        update_coefficients();
    }

    void set_release_ms(double ms) {
        release_ms_ = std::clamp(dynamics::retain_finite(ms, release_ms_),
                                 kReleaseMsMin, kReleaseMsMax);
        update_coefficients();
    }

    void set_detector(Detector d) { detector_ = d; }
    Detector detector() const { return detector_; }

    void set_rms_window_ms(double ms) {
        rms_window_ms_ = std::clamp(dynamics::retain_finite(ms, rms_window_ms_),
                                    kRmsWindowMsMin, kRmsWindowMsMax);
        update_coefficients();
    }

    /// Lookahead in ms, clamped to the ceiling `prepare()` sized for — so this
    /// can never need an allocation.
    void set_lookahead_ms(double ms) {
        lookahead_ms_ = std::clamp(dynamics::retain_finite(ms, lookahead_ms_),
                                   0.0, max_lookahead_ms_);
        update_lookahead();
    }

    void set_program_dependent_release(bool on) { program_dependent_ = on; }

    void set_makeup_gain_db(double db) {
        makeup_db_ = std::clamp(dynamics::retain_finite(db, makeup_db_),
                                -kMakeupDbMax, kMakeupDbMax);
        update_makeup();
    }

    void set_auto_makeup(bool on) {
        auto_makeup_ = on;
        update_makeup();
    }

    /// 0 = fully independent per-channel detectors, 1 = one shared detector fed
    /// by the louder channel.
    void set_stereo_link(double amount) {
        stereo_link_ = std::clamp(dynamics::retain_finite(amount, stereo_link_), 0.0, 1.0);
    }

    /// The module's entire latency contribution: the lookahead delay, exactly.
    /// The gain computer and detector are zero-latency IIR stages, so this is 0
    /// at the default 0 ms lookahead (series law 5).
    int latency_samples() const noexcept { return lookahead_samples_; }

    /// The static characteristic's output for an input level in dB — the
    /// gain computer alone, with no smoothing. Exposed so a caller (or a test)
    /// can plot the curve without running audio through it.
    double static_curve_db(double input_db) const {
        return dynamics::soft_knee_output_db(input_db, threshold_db_, knee_db_, ratio_);
    }

    /// Gain reduction in dB (≤ 0) the static curve applies at `input_db`.
    double gain_computer_db(double input_db) const {
        return static_curve_db(input_db) - input_db;
    }

    /// Makeup gain currently in force, in dB — the auto value when auto-makeup
    /// is on, the manual value otherwise.
    double effective_makeup_db() const { return auto_makeup_ ? auto_makeup_db_ : makeup_db_; }

    /// Smoothed gain reduction in dB (≥ 0, a magnitude) as of the last
    /// processed sample. Exposed for metering and for tests that measure the
    /// detector rather than inferring it from the audio.
    double gain_reduction_db(int channel = 0) const {
        return channels_[static_cast<std::size_t>(channel & 1)].smoothed_reduction;
    }

    /// Canonical non-negative gain-reduction magnitude for shared meters.
    GainReduction gain_reduction(int channel = 0) const noexcept {
        return GainReduction::from_magnitude_db(gain_reduction_db(channel));
    }

    // ── Processing ────────────────────────────────────────────────────────

    /// One mono sample.
    SampleType process(SampleType input) {
        // A non-finite sample is not audio and must not become detector or
        // lookahead history. Clear the owned recursive state so the very next
        // finite sample is processed from a valid, deterministic state.
        if (!std::isfinite(static_cast<double>(input))) {
            recover_audio_fault();
            return SampleType{0};
        }
        const double level_db = detect_level_db(0, static_cast<double>(input));
        const double gain = channel_gain(0, level_db);
        return static_cast<SampleType>(delayed(0, input) * gain * makeup_linear_);
    }

    /// One stereo pair, in place. Reads both channels before writing either,
    /// which the link path requires.
    void process_stereo(SampleType& left, SampleType& right) {
        if (!std::isfinite(static_cast<double>(left)) ||
            !std::isfinite(static_cast<double>(right))) {
            recover_audio_fault();
            left = SampleType{0};
            right = SampleType{0};
            return;
        }
        const double left_db = detect_level_db(0, static_cast<double>(left));
        const double right_db = detect_level_db(1, static_cast<double>(right));

        // The blend crosses the two DETECTOR INPUTS, before the detector, not
        // two finished gain-reduction values after it.
        const double shared_db = std::max(left_db, right_db);
        const double left_used = left_db + stereo_link_ * (shared_db - left_db);
        const double right_used = right_db + stereo_link_ * (shared_db - right_db);

        const double left_gain = channel_gain(0, left_used);
        const double right_gain = channel_gain(1, right_used);

        const SampleType dry_left = delayed(0, left);
        const SampleType dry_right = delayed(1, right);
        left = static_cast<SampleType>(dry_left * left_gain * makeup_linear_);
        right = static_cast<SampleType>(dry_right * right_gain * makeup_linear_);
    }

    void process_block(SampleType* io, int n) {
        for (int i = 0; i < n; ++i) io[i] = process(io[i]);
    }

    void process_block_stereo(SampleType* left, SampleType* right, int n) {
        for (int i = 0; i < n; ++i) process_stereo(left[i], right[i]);
    }

private:
    void recover_audio_fault() noexcept {
        for (auto& ch : channels_) ch = ChannelState{};
        write_index_[0] = write_index_[1] = 0;
        lookahead_valid_[0] = lookahead_valid_[1] = 0;
    }

    struct ChannelState {
        double mean_square = 0.0;      ///< RMS detector integrator.
        double release_stage = 0.0;    ///< Decoupled detector, stage 1.
        double smoothed_reduction = 0.0;  ///< Decoupled detector, stage 2 (dB, ≥ 0).
        double sustain = 0.0;          ///< Program-dependent release blend, 0..1.
    };

    /// Input level in dB for one channel, per the selected detector.
    double detect_level_db(int channel, double x) {
        auto& ch = channels_[static_cast<std::size_t>(channel)];
        if (detector_ == Detector::peak) return dynamics::amplitude_db(x, kLevelEpsilon);

        // `power_db` is `10·log10`, not 20, because the integrator already holds
        // a squared quantity — see its note in `dynamics_core.hpp`.
        ch.mean_square = snap_to_zero(rms_coef_ * ch.mean_square + (1.0 - rms_coef_) * x * x);
        return dynamics::power_db(ch.mean_square, kLevelEpsilon);
    }

    /// One channel's linear gain for an already-blended detector input level.
    double channel_gain(int channel, double level_db) {
        auto& ch = channels_[static_cast<std::size_t>(channel)];

        // Positive magnitude from here on; see the sign-convention note in the
        // file doc block.
        const double reduction = -gain_computer_db(level_db);

        double release_coef = release_coef_;
        if (program_dependent_) {
            // The sustain detector is a one-pole over the indicator "is the
            // CURRENT reduction deep enough to count", so it rises while
            // compression is held and falls once it lets go.
            const double sustained =
                ch.smoothed_reduction > kSustainThresholdDb ? 1.0 : 0.0;
            ch.sustain = sustain_coef_ * ch.sustain + (1.0 - sustain_coef_) * sustained;
            release_coef = release_coef_ + ch.sustain * (slow_release_coef_ - release_coef_);
        }

        ch.release_stage = std::max(
            reduction, release_coef * ch.release_stage + (1.0 - release_coef) * reduction);
        ch.smoothed_reduction = snap_to_zero(
            attack_coef_ * ch.smoothed_reduction + (1.0 - attack_coef_) * ch.release_stage);

        return std::pow(10.0, -ch.smoothed_reduction / 20.0);
    }

    /// Pushes `input` into the channel's ring and returns the sample delayed by
    /// the current lookahead. A pass-through at 0 ms.
    ///
    /// Each channel carries its OWN cursor rather than sharing one. A shared
    /// cursor would have to know whether the caller is mid-frame, which means a
    /// "who advances it" rule that the mono and stereo entry points have to
    /// agree on — and a caller mixing the two would silently desynchronise the
    /// channels. Two `size_t`s are cheaper than that invariant.
    SampleType delayed(int channel, SampleType input) {
        if (lookahead_samples_ <= 0) return input;
        const auto index = static_cast<std::size_t>(channel);
        auto& ring = lookahead_[index];
        if (ring.empty()) return input;

        const std::size_t size = ring.size();
        std::size_t& write = write_index_[index];
        const std::size_t read =
            (write + size - static_cast<std::size_t>(lookahead_samples_)) % size;
        const SampleType out = lookahead_valid_[index] >= static_cast<std::size_t>(lookahead_samples_)
                                   ? ring[read]
                                   : SampleType{0};
        ring[write] = input;
        write = (write + 1) % size;
        if (lookahead_valid_[index] < size) ++lookahead_valid_[index];
        return out;
    }

    void update_coefficients() {
        attack_coef_ = one_pole(attack_ms_);
        release_coef_ = one_pole(release_ms_);
        slow_release_coef_ = one_pole(release_ms_ * kSlowReleaseRatio);
        sustain_coef_ = one_pole(kSustainTauMs);
        rms_coef_ = one_pole(rms_window_ms_);
    }

    /// The paper's coefficient mapping, in ms. RETAIN convention — see
    /// `dynamics_core.hpp` for why the name says so.
    double one_pole(double tau_ms) const {
        return dynamics::one_pole_retain(tau_ms * 0.001, sample_rate_);
    }

    void update_lookahead() {
        lookahead_samples_ = static_cast<int>(
            std::llround(units::ms_to_samples(lookahead_ms_, sample_rate_)));
        const auto capacity = lookahead_[0].size();
        if (capacity > 0 && lookahead_samples_ >= static_cast<int>(capacity))
            lookahead_samples_ = static_cast<int>(capacity) - 1;
        if (lookahead_samples_ < 0) lookahead_samples_ = 0;
    }

    void update_makeup() {
        // Exactly cancels the gain computer's own reduction at a 0 dBFS input —
        // the "unity at the reference point" convention. Closed form, evaluated
        // on a control change rather than per sample.
        auto_makeup_db_ = -gain_computer_db(0.0);
        makeup_linear_ = units::db_to_linear(effective_makeup_db());
    }

    double sample_rate_ = 44100.0;
    double max_lookahead_ms_ = 10.0;

    double threshold_db_ = -18.0;
    double ratio_ = 4.0;
    double knee_db_ = 6.0;
    double attack_ms_ = 10.0;
    double release_ms_ = 120.0;
    double rms_window_ms_ = 10.0;
    double lookahead_ms_ = 0.0;
    double makeup_db_ = 0.0;
    double stereo_link_ = 1.0;
    Detector detector_ = Detector::peak;
    bool program_dependent_ = true;
    bool auto_makeup_ = true;

    double attack_coef_ = 0.0;
    double release_coef_ = 0.0;
    double slow_release_coef_ = 0.0;
    double sustain_coef_ = 0.0;
    double rms_coef_ = 0.0;
    double auto_makeup_db_ = 0.0;
    double makeup_linear_ = 1.0;
    int lookahead_samples_ = 0;

    ChannelState channels_[2]{};
    std::vector<SampleType> lookahead_[2]{};
    std::size_t write_index_[2]{0, 0};
    std::size_t lookahead_valid_[2]{0, 0};
};

using FeedforwardCompressor = FeedforwardCompressorT<float>;
using FeedforwardCompressor64 = FeedforwardCompressorT<double>;

}  // namespace pulp::signal
