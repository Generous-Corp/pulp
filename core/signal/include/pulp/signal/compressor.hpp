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
class Compressor {
public:
    struct Params {
        float threshold_db = -20.0f;  // Compression threshold
        float ratio = 4.0f;           // Compression ratio (4:1)
        float attack_ms = 5.0f;       // Attack time in ms
        float release_ms = 100.0f;    // Release time in ms
        float knee_db = 6.0f;         // Soft knee width in dB (0 = hard knee)
        float makeup_db = 0.0f;       // Makeup gain
    };

    void set_params(const Params& p) { params_ = p; }
    void set_sample_rate(float sr) {
        sample_rate_ = sr;
        configure_sidechain_filter();
        configure_lookahead_buffer();
    }

    /// Set the sidechain high-pass cutoff (Hz). 0 disables the HPF
    /// and the sidechain detector sees the raw signal. Typical values:
    /// 80–200 Hz for "duck on bass" prevention. The filter is a
    /// second-order Butterworth biquad (Q ≈ 0.707).
    void set_sidechain_hpf_hz(float hz) {
        sidechain_hpf_hz_ = std::max(0.0f, hz);
        configure_sidechain_filter();
    }
    float sidechain_hpf_hz() const { return sidechain_hpf_hz_; }

    /// Set the lookahead in milliseconds. The dry input is delayed by
    /// this amount so the envelope follower can react before the
    /// transient arrives. 0 disables lookahead (default).
    /// Maximum supported lookahead is 50 ms — beyond that the
    /// internal ring buffer is too large for low-latency contexts.
    void set_lookahead_ms(float ms) {
        lookahead_ms_ = std::clamp(ms, 0.0f, 50.0f);
        configure_lookahead_buffer();
    }
    float lookahead_ms() const { return lookahead_ms_; }

    /// Latency reported to the host = lookahead in samples (0 when off).
    int latency_samples() const {
        return static_cast<int>(std::round(lookahead_ms_ * 0.001f * sample_rate_));
    }

    /// Process one sample. The sidechain signal defaults to @p input,
    /// matching the original non-sidechain behavior.
    float process(float input) {
        return process_with_sidechain(input, input);
    }

    /// Process one sample with an explicit sidechain detector input.
    /// `sidechain` is the signal that drives the envelope follower;
    /// `input` is the signal that gets the gain applied.
    float process_with_sidechain(float input, float sidechain) {
        // Apply sidechain HPF if configured.
        const float detector = sidechain_hpf_active_
            ? sidechain_hpf_.process(sidechain)
            : sidechain;

        const float input_db = 20.0f * std::log10(std::max(std::abs(detector), 1e-10f));
        const float gain_db = compute_gain(input_db);

        const float coeff = (gain_db < envelope_db_)
            ? attack_coeff()
            : release_coeff();
        envelope_db_ = envelope_db_ + coeff * (gain_db - envelope_db_);

        const float gain_linear =
            std::pow(10.0f, (envelope_db_ + params_.makeup_db) / 20.0f);

        // Lookahead: push input into the delay, read out the
        // sample-from-the-past, apply the gain that the envelope
        // followed off the un-delayed sidechain.
        if (lookahead_samples_ > 0) {
            lookahead_buffer_.push(input);
            const float delayed = lookahead_buffer_.read(
                static_cast<float>(lookahead_samples_));
            return delayed * gain_linear;
        }
        return input * gain_linear;
    }

    void process(float* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    /// Process a block with a separate sidechain input. Both buffers
    /// must be at least @p num_samples long.
    void process_with_sidechain(float* buffer, const float* sidechain,
                                  int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process_with_sidechain(buffer[i], sidechain[i]);
    }

    float gain_reduction_db() const { return envelope_db_; }

    void reset() {
        envelope_db_ = 0.0f;
        sidechain_hpf_.reset();
        lookahead_buffer_.reset();
    }

private:
    Params params_;
    float sample_rate_ = 44100.0f;
    float envelope_db_ = 0.0f;

    float sidechain_hpf_hz_ = 0.0f;
    bool sidechain_hpf_active_ = false;
    Biquad sidechain_hpf_{};

    float lookahead_ms_ = 0.0f;
    int lookahead_samples_ = 0;
    DelayLine lookahead_buffer_{};

    void configure_sidechain_filter() {
        sidechain_hpf_active_ = sidechain_hpf_hz_ > 0.0f && sample_rate_ > 0.0f;
        if (sidechain_hpf_active_) {
            sidechain_hpf_.set_coefficients(
                Biquad::Type::highpass,
                sidechain_hpf_hz_, /*Q=*/0.7071068f, sample_rate_);
            sidechain_hpf_.reset();
        }
    }
    void configure_lookahead_buffer() {
        lookahead_samples_ = static_cast<int>(
            std::round(lookahead_ms_ * 0.001f * sample_rate_));
        if (lookahead_samples_ > 0) {
            // DelayLine wants a buffer length >= max delay + 1.
            lookahead_buffer_.prepare(lookahead_samples_ + 1);
            lookahead_buffer_.reset();
        }
    }

    float compute_gain(float input_db) const {
        float knee = params_.knee_db;
        float threshold = params_.threshold_db;
        float ratio = params_.ratio;

        if (knee <= 0.0f) {
            // Hard knee
            if (input_db <= threshold) return 0.0f;
            return (input_db - threshold) * (1.0f / ratio - 1.0f);
        }

        // Soft knee
        float half_knee = knee / 2.0f;
        if (input_db < threshold - half_knee)
            return 0.0f;
        if (input_db > threshold + half_knee)
            return (input_db - threshold) * (1.0f / ratio - 1.0f);

        // In the knee region
        float x = input_db - threshold + half_knee;
        return (1.0f / ratio - 1.0f) * x * x / (2.0f * knee);
    }

    float attack_coeff() const {
        if (params_.attack_ms <= 0.0f) return 1.0f;
        return 1.0f - std::exp(-1.0f / (params_.attack_ms * 0.001f * sample_rate_));
    }

    float release_coeff() const {
        if (params_.release_ms <= 0.0f) return 1.0f;
        return 1.0f - std::exp(-1.0f / (params_.release_ms * 0.001f * sample_rate_));
    }
};

// Brickwall limiter — hard limit at threshold with lookahead-free design.
// RT contract: setters, process paths, and reset are scalar-only and allocate
// no memory.
class Limiter {
public:
    void set_threshold_db(float db) { threshold_ = std::pow(10.0f, db / 20.0f); }
    void set_release_ms(float ms) { release_ms_ = ms; }
    void set_sample_rate(float sr) { sample_rate_ = sr; }

    float process(float input) {
        float abs_input = std::abs(input);

        // Envelope follower
        if (abs_input > envelope_)
            envelope_ = abs_input; // Instant attack
        else {
            float coeff = 1.0f - std::exp(-1.0f / (release_ms_ * 0.001f * sample_rate_));
            envelope_ += coeff * (abs_input - envelope_);
        }

        // Compute gain
        float gain = (envelope_ > threshold_) ? threshold_ / envelope_ : 1.0f;
        return input * gain;
    }

    void process(float* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    void reset() { envelope_ = 0.0f; }

private:
    float threshold_ = 1.0f;
    float release_ms_ = 50.0f;
    float sample_rate_ = 44100.0f;
    float envelope_ = 0.0f;
};

} // namespace pulp::signal
