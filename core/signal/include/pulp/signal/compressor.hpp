#pragma once

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fast_math.hpp>
#include <pulp/signal/dynamics_contract.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pulp::signal {

/// How CompressorT smooths the gain reduction its static curve asks for, in
/// the log (dB) domain after the gain computer (the two detector forms of
/// Giannoulis, Massberg and Reiss, "Digital Dynamic Range Compressor Design",
/// JAES 2012). attack_ms and release_ms are one-pole time constants (tau) in
/// both, so a steady-level step moves from 10% to 90% in about 2.2 tau.
///
/// - `decoupled` (the default): a peak hold that decays with the release
///   time, followed by an attack one-pole. The hold sits at each cycle's
///   peak, so attack follows the knob on tones as on steps, the reduction
///   settles on the static curve, and the attack stage filters the release
///   droop between peaks. Measured at 5/100 ms with a -6 dBFS tone: THD+N
///   -48.8 / -60.3 dB at 50 / 100 Hz.
/// - `branching`: one one-pole that uses the attack coefficient while more
///   reduction is asked for and the release coefficient otherwise (the
///   default before the decoupled detector existed). On a tone
///   the per-sample detector rides the waveform: only samples near each peak
///   pull toward more reduction and release acts between them, so attack is
///   slower than the knob (about 2x at 1 kHz with 5/100 ms), the reduction
///   settles short of the static curve, and the gain ripples at twice the
///   tone frequency: THD+N -39.5 / -45.5 dB at 50 / 100 Hz under the same
///   conditions.
enum class CompressorBallistics : std::uint8_t { decoupled, branching };

// Feed-forward compressor with adjustable attack/release.
//
// RT contract: parameter setters and sample/buffer process paths allocate no
// memory except `set_sample_rate()` / `set_lookahead_ms()` may allocate or
// resize the lookahead delay line. Configure lookahead off the audio thread;
// after that, process paths, `latency_samples()`, `gain_reduction_db()`, and
// `reset()` allocate no memory.
//
// Sidechain HPF (item 2.4 of macOS plan): when `set_sidechain_hpf_hz`
// is non-zero, the level-detection path runs the sidechain signal
// through a high-pass biquad before envelope-following. This makes
// the compressor less reactive to bass content — useful for
// preventing pumping on a bass-heavy mix or for sidechain-from-kick
// patterns where the kick's fundamental shouldn't trigger
// compression on top of the snare.
//
// Lookahead (item 2.4): when `set_lookahead_ms` is non-zero, the
// dry signal is delayed by that many ms so the envelope follower
// "sees the future" — peak transients are caught before they hit
// the output. Round-trip latency increases by the lookahead amount;
// callers should report it via Processor::latency_samples().
template <typename SampleType = float>
class CompressorT {
public:
    struct Params {
        SampleType threshold_db = SampleType{-20.0f};  // Compression threshold
        SampleType ratio = SampleType{4.0f};           // Compression ratio (4:1)
        // Attack and release time constants (tau) in ms: a steady-level step
        // moves 10% -> 90% in about 2.2x these (see CompressorBallistics).
        SampleType attack_ms = SampleType{5.0f};
        SampleType release_ms = SampleType{100.0f};
        SampleType knee_db = SampleType{6.0f};         // Soft knee width in dB (0 = hard knee)
        SampleType makeup_db = SampleType{0.0f};       // Makeup gain
    };

    void set_params(const Params& p) {
        params_ = p;
        update_ballistics();
    }

    /// Select how gain reduction is smoothed (see CompressorBallistics). Takes
    /// effect from the next sample; the detector state carries over.
    void set_ballistics(CompressorBallistics ballistics) noexcept {
        ballistics_ = ballistics;
        hold_db_ = envelope_db_;
    }
    CompressorBallistics ballistics() const noexcept {
        return ballistics_;
    }
    void set_sample_rate(SampleType sr) {
        sample_rate_ = sr;
        update_ballistics();
        configure_sidechain_filter();
        configure_lookahead_buffer();
    }

    /// Set the sidechain high-pass cutoff (Hz). 0 disables the HPF
    /// and the sidechain detector sees the raw signal. Typical values:
    /// 80–200 Hz for "duck on bass" prevention. The filter is a
    /// second-order Butterworth biquad (Q ≈ 0.707).
    void set_sidechain_hpf_hz(SampleType hz) {
        sidechain_hpf_hz_ = std::max(SampleType{0.0f}, hz);
        configure_sidechain_filter();
    }
    SampleType sidechain_hpf_hz() const { return sidechain_hpf_hz_; }

    /// Set the lookahead in milliseconds. The dry input is delayed by
    /// this amount so the envelope follower can react before the
    /// transient arrives. 0 disables lookahead (default).
    /// Maximum supported lookahead is 50 ms — beyond that the
    /// internal ring buffer is too large for low-latency contexts.
    void set_lookahead_ms(SampleType ms) {
        lookahead_ms_ = std::clamp(ms, SampleType{0.0f}, SampleType{50.0f});
        configure_lookahead_buffer();
    }
    SampleType lookahead_ms() const { return lookahead_ms_; }

    /// Latency reported to the host = lookahead in samples (0 when off).
    int latency_samples() const {
        return static_cast<int>(
            std::round(lookahead_ms_ * SampleType{0.001f} * sample_rate_));
    }

    /// Process one sample. The sidechain signal defaults to @p input,
    /// matching the original non-sidechain behavior.
    SampleType process(SampleType input) {
        return process_with_sidechain(input, input);
    }

    /// Process one sample with an explicit sidechain detector input.
    /// `sidechain` is the signal that drives the envelope follower;
    /// `input` is the signal that gets the gain applied.
    SampleType process_with_sidechain(SampleType input, SampleType sidechain) {
        // A non-finite program or detector sample is not meaningful audio. It
        // must not enter the HPF, delay line, or recursive envelope because one
        // poisoned state sample would otherwise make later finite audio and
        // telemetry non-finite. Recover owned signal history, retain all user
        // configuration, and emit finite silence for the rejected sample.
        if (!std::isfinite(input) || !std::isfinite(sidechain)) {
            reset();
            return SampleType{0};
        }

        // Apply sidechain HPF if configured.
        const SampleType detector = sidechain_hpf_active_
            ? sidechain_hpf_.process(sidechain)
            : sidechain;

        const SampleType gain_db = compute_gain(level_db(detector));

        // Snap the recursive gain-reduction state: when the signal sits above
        // threshold it settles toward 0 dB and otherwise stalls in denormals
        // with no FTZ guard. No-op above 1e-15.
        if (ballistics_ == CompressorBallistics::decoupled) {
            hold_db_ = snap_to_zero(decoupled_hold(hold_db_, gain_db, release_coeff_));
            envelope_db_ = snap_to_zero(envelope_db_ + attack_coeff_ * (hold_db_ - envelope_db_));
        } else {
            const SampleType coeff = (gain_db < envelope_db_) ? attack_coeff_ : release_coeff_;
            envelope_db_ = snap_to_zero(envelope_db_ + coeff * (gain_db - envelope_db_));
        }

        const SampleType gain_linear = db_to_gain(envelope_db_ + params_.makeup_db);

        // Lookahead: push input into the delay, read out the
        // sample-from-the-past, apply the gain that the envelope
        // followed off the un-delayed sidechain.
        if (lookahead_samples_ > 0) {
            lookahead_buffer_.push(input);
            const SampleType delayed =
                lookahead_buffer_.read(static_cast<SampleType>(lookahead_samples_));
            return delayed * gain_linear;
        }
        return input * gain_linear;
    }

    /// Process a block in place; the buffer is also the sidechain. Same as
    /// process_with_sidechain(buffer, buffer, num_samples).
    void process(SampleType* buffer, int num_samples) {
        run_chunks(buffer, buffer, num_samples);
    }

    /// Process a block with a separate sidechain input. Both buffers
    /// must be at least @p num_samples long.
    ///
    /// Equivalent to calling process_with_sidechain(SampleType, SampleType)
    /// per sample, including the reset at a non-finite sample, but run in
    /// kSnapChunkSamples chunks of three passes each (detector and static
    /// curve, envelope recursion, gain), so the transcendental work in the
    /// outer passes vectorizes. The float path computes the same values; the
    /// envelope's denormal snap runs once per chunk (see run_chunks()).
    void process_with_sidechain(SampleType* buffer, const SampleType* sidechain,
                                int num_samples) {
        run_chunks(buffer, sidechain, num_samples);
    }

    /// Envelope smoothing coefficients in force for the current parameters and
    /// sample rate: the per-sample step toward a lower (attack) or higher
    /// (release) gain target is `coefficient * (target_db - envelope_db)`.
    SampleType attack_coefficient() const {
        return attack_coeff_;
    }
    SampleType release_coefficient() const {
        return release_coeff_;
    }

    SampleType gain_reduction_db() const { return envelope_db_; }

    /// Canonical non-negative gain-reduction magnitude for shared meters.
    GainReduction gain_reduction() const noexcept {
        return GainReduction::from_signed_db(static_cast<double>(envelope_db_));
    }

    void reset() {
        envelope_db_ = SampleType{0.0f};
        hold_db_ = SampleType{0.0f};
        sidechain_hpf_.reset();
        lookahead_buffer_.reset();
    }

private:
    Params params_;
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType envelope_db_ = SampleType{0.0f};
    // Peak hold of the decoupled detector (gain change in dB, <= 0).
    SampleType hold_db_ = SampleType{0.0f};
    CompressorBallistics ballistics_ = CompressorBallistics::decoupled;

    SampleType attack_coeff_ = ballistic_coeff(params_.attack_ms, sample_rate_);
    SampleType release_coeff_ = ballistic_coeff(params_.release_ms, sample_rate_);

    SampleType sidechain_hpf_hz_ = SampleType{0.0f};
    bool sidechain_hpf_active_ = false;
    BiquadT<SampleType> sidechain_hpf_{};

    SampleType lookahead_ms_ = SampleType{0.0f};
    int lookahead_samples_ = 0;
    DelayLineT<SampleType> lookahead_buffer_{};

    void configure_sidechain_filter() {
        sidechain_hpf_active_ =
            sidechain_hpf_hz_ > SampleType{0.0f} && sample_rate_ > SampleType{0.0f};
        if (sidechain_hpf_active_) {
            sidechain_hpf_.set_coefficients(
                BiquadT<SampleType>::Type::highpass,
                sidechain_hpf_hz_, /*Q=*/SampleType{0.7071068f}, sample_rate_);
            sidechain_hpf_.reset();
        }
    }
    void configure_lookahead_buffer() {
        lookahead_samples_ = static_cast<int>(
            std::round(lookahead_ms_ * SampleType{0.001f} * sample_rate_));
        if (lookahead_samples_ > 0) {
            // DelayLine wants a buffer length >= max delay + 1.
            lookahead_buffer_.prepare(lookahead_samples_ + 1);
            lookahead_buffer_.reset();
        }
    }

    // Static curve: gain change in dB (<= 0) for a detector level in dB.
    // Written as selects over precomputed candidates rather than early
    // returns so the block path's static-curve pass vectorizes; each region's
    // expression is unchanged.
    SampleType compute_gain(SampleType input_db) const {
        const SampleType knee = params_.knee_db;
        const SampleType threshold = params_.threshold_db;
        const SampleType slope = SampleType{1.0f} / params_.ratio - SampleType{1.0f};
        const SampleType above = (input_db - threshold) * slope;

        if (knee <= SampleType{0.0f}) {
            // Hard knee
            return input_db <= threshold ? SampleType{0.0f} : above;
        }

        // Soft knee
        const SampleType half_knee = knee / SampleType{2.0f};
        const SampleType x = input_db - threshold + half_knee;
        const SampleType in_knee = slope * x * x / (SampleType{2.0f} * knee);
        const SampleType upper = input_db > threshold + half_knee ? above : in_knee;
        return input_db < threshold - half_knee ? SampleType{0.0f} : upper;
    }

    // Detector magnitude in dB, floored at -200 dB. Float uses FastMath's
    // branch-free conversion so the block path's detector pass vectorizes;
    // double has no fast path and keeps libm.
    static SampleType level_db(SampleType detector) {
        const SampleType magnitude = std::max(std::abs(detector), SampleType{1e-10f});
        if constexpr (std::is_same_v<SampleType, float>)
            return FastMath::gain_to_db_accurate(magnitude);
        else
            return SampleType{20.0f} * std::log10(magnitude);
    }

    // Linear gain for a gain change in dB (FastMath for float, as above).
    static SampleType db_to_gain(SampleType db) {
        if constexpr (std::is_same_v<SampleType, float>)
            return FastMath::db_to_gain_accurate(db);
        else
            return std::pow(SampleType{10.0f}, db / SampleType{20.0f});
    }

    // The decoupled detector's peak hold on gain changes (<= 0 dB): it jumps
    // to any deeper reduction at once and otherwise decays toward the target
    // with the release coefficient (Giannoulis et al.'s
    // y1 = max(x, aR y1 + (1 - aR) x) with the sign flipped).
    static SampleType decoupled_hold(SampleType hold, SampleType target, SampleType release) {
        const SampleType released = hold + release * (target - hold);
        return target < released ? target : released;
    }

    static SampleType ballistic_coeff(SampleType time_ms, SampleType sample_rate) {
        if (time_ms <= SampleType{0.0f})
            return SampleType{1.0f};
        return SampleType{1.0f} -
               std::exp(SampleType{-1.0f} / (time_ms * SampleType{0.001f} * sample_rate));
    }

    void update_ballistics() {
        attack_coeff_ = ballistic_coeff(params_.attack_ms, sample_rate_);
        release_coeff_ = ballistic_coeff(params_.release_ms, sample_rate_);
    }

    // The block path behind process(buffer) and process_with_sidechain(buffer,
    // sidechain). Each kSnapChunkSamples chunk runs the detector and static
    // curve over the whole chunk, then the envelope recursion, then the gain
    // stage, so the transcendental calls in the outer passes are independent
    // of one another and of the recursion.
    void run_chunks(SampleType* buffer, const SampleType* sidechain, int num_samples) {
        if (num_samples <= 0)
            return;
        SampleType level[kSnapChunkSamples];
        for (const auto [begin, end] : snap_chunks(static_cast<std::size_t>(num_samples))) {
            const int n = static_cast<int>(end - begin);
            SampleType* x = buffer + begin;
            const SampleType* sc = sidechain + begin;

            // A non-finite sample resets signal history at that exact sample,
            // which the per-sample path already implements. The scan is a
            // branch-free OR so it vectorizes; NaN fails the comparison too.
            constexpr SampleType kMaxFinite = std::numeric_limits<SampleType>::max();
            unsigned non_finite = 0;
            for (int i = 0; i < n; ++i)
                non_finite |= static_cast<unsigned>(!(std::abs(x[i]) <= kMaxFinite)) |
                              static_cast<unsigned>(!(std::abs(sc[i]) <= kMaxFinite));
            if (non_finite != 0) {
                for (int i = 0; i < n; ++i)
                    x[i] = process_with_sidechain(x[i], sc[i]);
                continue;
            }

            if (sidechain_hpf_active_) {
                for (int i = 0; i < n; ++i)
                    level[i] = sidechain_hpf_.process(sc[i]);
                for (int i = 0; i < n; ++i)
                    level[i] = compute_gain(level_db(level[i]));
            } else {
                for (int i = 0; i < n; ++i)
                    level[i] = compute_gain(level_db(sc[i]));
            }

            // The envelope is snapped once per chunk: it only alters a level
            // below 1e-15 dB. With releases longer than about 0.05 ms the
            // release pole is slow enough that a chunk-start value above that
            // cannot reach the subnormal range before the next snap; with
            // shorter ones the envelope decays through it within the chunk,
            // so subnormal residency is still bounded by one chunk.
            const SampleType attack = attack_coeff_;
            const SampleType release = release_coeff_;
            SampleType envelope = envelope_db_;
            if (ballistics_ == CompressorBallistics::decoupled) {
                SampleType hold = hold_db_;
                for (int i = 0; i < n; ++i) {
                    hold = decoupled_hold(hold, level[i], release);
                    envelope = envelope + attack * (hold - envelope);
                    level[i] = envelope;
                }
                hold_db_ = snap_to_zero(hold);
            } else {
                for (int i = 0; i < n; ++i) {
                    const SampleType coeff = (level[i] < envelope) ? attack : release;
                    envelope = envelope + coeff * (level[i] - envelope);
                    level[i] = envelope;
                }
            }
            envelope_db_ = snap_to_zero(envelope);

            const SampleType makeup = params_.makeup_db;
            if (lookahead_samples_ > 0) {
                const SampleType delay = static_cast<SampleType>(lookahead_samples_);
                for (int i = 0; i < n; ++i)
                    level[i] = db_to_gain(level[i] + makeup);
                for (int i = 0; i < n; ++i) {
                    lookahead_buffer_.push(x[i]);
                    x[i] = lookahead_buffer_.read(delay) * level[i];
                }
            } else {
                for (int i = 0; i < n; ++i)
                    x[i] = x[i] * db_to_gain(level[i] + makeup);
            }
        }
    }
};

using Compressor = CompressorT<float>;
using Compressor64 = CompressorT<double>;

// Brickwall limiter — hard limit at threshold with lookahead-free design.
// RT contract: setters, process paths, and reset are scalar-only and allocate
// no memory.
template <typename SampleType = float>
class LimiterT {
public:
    void set_threshold_db(SampleType db) {
        threshold_ = std::pow(SampleType{10.0f}, db / SampleType{20.0f});
        threshold_mute_ = std::isinf(db) && db < SampleType{0};
    }
    void set_release_ms(SampleType ms) { release_ms_ = ms; }
    void set_sample_rate(SampleType sr) { sample_rate_ = sr; }

    SampleType process(SampleType input) {
        // Reject before touching the recursive peak envelope. reset() clears
        // only signal history; threshold, release, and sample rate remain as
        // configured. The rejected sample is represented by finite silence.
        if (!std::isfinite(input)) {
            reset();
            return SampleType{0};
        }

        SampleType abs_input = std::abs(input);

        // Envelope follower
        if (abs_input > envelope_)
            envelope_ = abs_input; // Instant attack
        else {
            SampleType coeff = SampleType{1.0f} -
                std::exp(SampleType{-1.0f} /
                         (release_ms_ * SampleType{0.001f} * sample_rate_));
            // Snap the release envelope so a decay into silence flushes to zero
            // instead of stalling in denormals with no FTZ guard. No-op above
            // 1e-15.
            envelope_ = snap_to_zero(envelope_ + coeff * (abs_input - envelope_));
        }

        // Compute gain
        SampleType gain =
            (envelope_ > threshold_) ? threshold_ / envelope_ : SampleType{1.0f};
        return input * gain;
    }

    void process(SampleType* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    GainReduction gain_reduction() const noexcept {
        // A zero threshold is the documented -infinity-dB mute setting. Its
        // telemetry remains a mute even while the recovered envelope is zero;
        // reporting unity here would falsely describe the configured result.
        if (threshold_mute_)
            return GainReduction::from_magnitude_db(
                std::numeric_limits<double>::infinity());
        const SampleType gain =
            (envelope_ > threshold_) ? threshold_ / envelope_ : SampleType{1};
        if (!(gain > SampleType{0}))
            return GainReduction::from_magnitude_db(
                std::numeric_limits<double>::infinity());
        return GainReduction::from_signed_db(
            SampleType{20} * std::log10(gain));
    }

    void reset() { envelope_ = SampleType{0.0f}; }

private:
    SampleType threshold_ = SampleType{1.0f};
    SampleType release_ms_ = SampleType{50.0f};
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType envelope_ = SampleType{0.0f};
    bool threshold_mute_ = false;
};

using Limiter = LimiterT<float>;
using Limiter64 = LimiterT<double>;

} // namespace pulp::signal
