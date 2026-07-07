#pragma once

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/delay_line.hpp>

#include <cmath>
#include <algorithm>

namespace pulp::signal {

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
        SampleType attack_ms = SampleType{5.0f};       // Attack time in ms
        SampleType release_ms = SampleType{100.0f};    // Release time in ms
        SampleType knee_db = SampleType{6.0f};         // Soft knee width in dB (0 = hard knee)
        SampleType makeup_db = SampleType{0.0f};       // Makeup gain
    };

    void set_params(const Params& p) { params_ = p; }
    void set_sample_rate(SampleType sr) {
        sample_rate_ = sr;
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
        // Apply sidechain HPF if configured.
        const SampleType detector = sidechain_hpf_active_
            ? sidechain_hpf_.process(sidechain)
            : sidechain;

        const SampleType input_db =
            SampleType{20.0f} *
            std::log10(std::max(std::abs(detector), SampleType{1e-10f}));
        const SampleType gain_db = compute_gain(input_db);

        const SampleType coeff = (gain_db < envelope_db_)
            ? attack_coeff()
            : release_coeff();
        envelope_db_ = envelope_db_ + coeff * (gain_db - envelope_db_);

        const SampleType gain_linear =
            std::pow(SampleType{10.0f},
                     (envelope_db_ + params_.makeup_db) / SampleType{20.0f});

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

    void process(SampleType* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    /// Process a block with a separate sidechain input. Both buffers
    /// must be at least @p num_samples long.
    void process_with_sidechain(SampleType* buffer, const SampleType* sidechain,
                                int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process_with_sidechain(buffer[i], sidechain[i]);
    }

    SampleType gain_reduction_db() const { return envelope_db_; }

    void reset() {
        envelope_db_ = SampleType{0.0f};
        sidechain_hpf_.reset();
        lookahead_buffer_.reset();
    }

private:
    Params params_;
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType envelope_db_ = SampleType{0.0f};

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

    SampleType compute_gain(SampleType input_db) const {
        SampleType knee = params_.knee_db;
        SampleType threshold = params_.threshold_db;
        SampleType ratio = params_.ratio;

        if (knee <= SampleType{0.0f}) {
            // Hard knee
            if (input_db <= threshold) return SampleType{0.0f};
            return (input_db - threshold) *
                   (SampleType{1.0f} / ratio - SampleType{1.0f});
        }

        // Soft knee
        SampleType half_knee = knee / SampleType{2.0f};
        if (input_db < threshold - half_knee)
            return SampleType{0.0f};
        if (input_db > threshold + half_knee)
            return (input_db - threshold) *
                   (SampleType{1.0f} / ratio - SampleType{1.0f});

        // In the knee region
        SampleType x = input_db - threshold + half_knee;
        return (SampleType{1.0f} / ratio - SampleType{1.0f}) * x * x /
               (SampleType{2.0f} * knee);
    }

    SampleType attack_coeff() const {
        if (params_.attack_ms <= SampleType{0.0f}) return SampleType{1.0f};
        return SampleType{1.0f} -
               std::exp(SampleType{-1.0f} /
                        (params_.attack_ms * SampleType{0.001f} * sample_rate_));
    }

    SampleType release_coeff() const {
        if (params_.release_ms <= SampleType{0.0f}) return SampleType{1.0f};
        return SampleType{1.0f} -
               std::exp(SampleType{-1.0f} /
                        (params_.release_ms * SampleType{0.001f} * sample_rate_));
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
    }
    void set_release_ms(SampleType ms) { release_ms_ = ms; }
    void set_sample_rate(SampleType sr) { sample_rate_ = sr; }

    SampleType process(SampleType input) {
        SampleType abs_input = std::abs(input);

        // Envelope follower
        if (abs_input > envelope_)
            envelope_ = abs_input; // Instant attack
        else {
            SampleType coeff = SampleType{1.0f} -
                std::exp(SampleType{-1.0f} /
                         (release_ms_ * SampleType{0.001f} * sample_rate_));
            envelope_ += coeff * (abs_input - envelope_);
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

    void reset() { envelope_ = SampleType{0.0f}; }

private:
    SampleType threshold_ = SampleType{1.0f};
    SampleType release_ms_ = SampleType{50.0f};
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType envelope_ = SampleType{0.0f};
};

using Limiter = LimiterT<float>;
using Limiter64 = LimiterT<double>;

} // namespace pulp::signal
