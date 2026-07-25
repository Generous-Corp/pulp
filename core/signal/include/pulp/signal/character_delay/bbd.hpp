#pragma once

// BBD character — a bucket-brigade device modeled as what it physically is.
//
// Structure and concepts follow the open BBD-modeling literature: Raffel &
// Smith (DAFx-10) for the device model, compander and the roles of the input /
// output filters; Holters & Parker (DAFx-18) for the variable-sample-rate view
// of the clocked line; the NE570/SA571 datasheet for compander topology; the
// Panasonic MN300x datasheets for stage counts.
//
// The device is N charge buckets clocked at f_clock, so total delay = N /
// f_clock. Modeling the CLOCK rather than a delay-in-samples is what makes the
// character behave like the hardware for free:
//
//   * Time changes move the clock, so the contents of the buckets repitch —
//     a BBD delay glides when you turn the time knob, it does not crossfade.
//   * Usable bandwidth is tied to the clock (a fixed fraction of f_clock/2, the
//     aliasing constraint both papers impose), so short delays are bright and
//     long delays go dark ON THEIR OWN. That relationship is the sound of the
//     device and it falls out of the model instead of being drawn as a curve.
//
// The compander is the other half of the sound. It is a companding NOISE
// reduction system wrapped around a noisy line, and it is imperfect: the
// expander's envelope lags the signal, so the noise floor audibly breathes
// around transients. That pumping is not an artifact to remove.

#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/character_delay/tables.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal::chardelay {

/// One channel of the clocked BBD core, compander included.
class BbdChannel {
public:
    void prepare(double fs) {
        sample_rate_ = fs;
        oversampled_rate_ = fs * static_cast<double>(kBbdOversample);
        compander_coefficient_ =
            std::exp(-1.0 / (kBbdCompanderTauMs * 0.001 * fs));
        pre_highpass_.set_highpass(20.0, 0.70710678, fs);
        update(0.0, 0.35);
        reset();
    }

    void reset() noexcept {
        buckets_.fill(0.0f);
        write_ = 0;
        time_accumulator_ = 0.0;
        jitter_state_ = 0.0;
        previous_input_ = 0.0;
        compress_envelope_ = kBbdCompanderFloor;
        expand_envelope_ = kBbdCompanderFloor;
        pre_lowpass_.reset();
        pre_highpass_.reset();
        post_lowpass_.reset();
        rng_.reset();
    }

    void set_seed(std::uint32_t seed) noexcept { rng_.reseed(seed); }

    /// Test hook: bypass the compander so the raw bucket-line noise floor is
    /// observable. The compander's whole job is to hide that floor, so proving
    /// it works means measuring against a build where it is not there.
    void set_compander_enabled(bool enabled) noexcept { compander_enabled_ = enabled; }

    /// Control-rate coefficient update. `delay_seconds` is the slewed delay
    /// time; everything the character does with bandwidth and pitch follows
    /// from the clock period it implies.
    void update(double character_amount, double delay_seconds) noexcept {
        stages_ = static_cast<std::size_t>(std::llround(
            interpolate_knots(kBbdAxis, kBbdStages, character_amount)));
        stages_ = std::clamp<std::size_t>(stages_, 1u, kBbdMaxStages);
        drive_ = interpolate_knots(kBbdAxis, kBbdDrive, character_amount);
        jitter_amount_ = interpolate_knots(kBbdAxis, kBbdJitterAmount, character_amount);

        const double delay = std::max(delay_seconds, 1e-6);
        clock_period_ = delay / static_cast<double>(stages_);
        const double clock_hz = 1.0 / clock_period_;
        bandwidth_hz_ = std::clamp(clock_hz / kBbdBandwidthDivisor, kBbdBandwidthMinHz,
                                   kBbdBandwidthMaxHz);

        // Matched anti-alias / reconstruction pair, as in the hardware and in
        // both published models. The pre-filter runs at the host rate (it
        // band-limits before the clocked write); the post-filter runs at the
        // internal oversampled rate, where it smooths the stepped read-out.
        pre_lowpass_.set_lowpass(bandwidth_hz_, 0.70710678, sample_rate_);
        post_lowpass_.set_lowpass(bandwidth_hz_, 0.70710678, oversampled_rate_);

        const double effective_drive = drive_ * kBbdCompanderDriveScale;
        shaper_gain_ = 1.0 + effective_drive * kBbdDriveGainSpan;
        shaper_offset_ = kBbdShape * 0.5;
        shaper_offset_tanh_ = fast_tanh(shaper_offset_);

        // Normalize the shaper to unity small-signal gain, measured rather than
        // derived so it tracks whatever curve the shaper actually uses. Without
        // this the shaper's drive multiplies the LOOP gain: at the top of the
        // character table the raw slope is well over 2, so a feedback setting of
        // 0.5 would put the loop above unity and the echo train would grow. The
        // character macro must change the colour of a repeat, not how long the
        // repeats last — that is what the feedback control is for. (The tape
        // saturator is DC-compensated for the same reason.)
        shaper_compensation_ = 1.0;
        constexpr double kProbe = 1e-4;
        const double slope = (waveshape(kProbe) - waveshape(-kProbe)) / (2.0 * kProbe);
        shaper_compensation_ = (std::abs(slope) > 1e-9) ? 1.0 / slope : 1.0;
    }

    double bandwidth_hz() const noexcept { return bandwidth_hz_; }
    std::size_t stages() const noexcept { return stages_; }

    double process(double x) noexcept {
        // ── Compander, compress half ──────────────────────────────────────
        // NE570-class parts close the rectifier loop around the COMPRESSED
        // signal, not the input; that feedback typology is why the compressed
        // level sits near a fixed operating point regardless of input level.
        const double compressed =
            compander_enabled_ ? x / std::max(compress_envelope_, kBbdCompanderFloor) : x;
        compress_envelope_ = flush_denormal(
            compander_coefficient_ * compress_envelope_ +
            (1.0 - compander_coefficient_) * std::abs(compressed));

        const double filtered = pre_highpass_.process(pre_lowpass_.process(compressed));

        // ── Clock-domain core ─────────────────────────────────────────────
        // The pre-filter has already band-limited the signal, so straight-line
        // interpolation across the U internal steps is a faithful, cheap
        // reconstruction of what the bucket chain samples between host frames.
        const double step =
            (filtered - previous_input_) / static_cast<double>(kBbdOversample - 1);
        double value = previous_input_;
        const double internal_period = 1.0 / oversampled_rate_;
        double first_read = 0.0;

        for (int u = 0; u < kBbdOversample; ++u) {
            time_accumulator_ += internal_period;
            // Zero, one, or several ticks: at short delays with a long bucket
            // chain the clock genuinely outruns the internal rate. The bound is
            // belt-and-braces — the delay time is clamped at 1 ms, which caps
            // the real tick count in the single digits — but an unbounded loop
            // on the audio thread is not something to leave to a clamp two
            // layers away.
            int ticks = 0;
            while (time_accumulator_ >= clock_period_ && ticks++ < kMaxTicksPerStep) {
                const double jittered = clock_period_ + next_jitter();
                buckets_[write_] = static_cast<float>(value);
                if (++write_ >= kBbdMaxStages) write_ = 0;
                time_accumulator_ -= std::max(jittered, 1e-9);
            }
            value += step;

            const std::size_t read_index =
                (write_ + kBbdMaxStages - stages_) % kBbdMaxStages;
            double read = static_cast<double>(buckets_[read_index]);
            if (drive_ > 0.0) read = waveshape(read);
            read = post_lowpass_.process(read);
            if (u == 0) first_read = read;
        }
        previous_input_ = filtered;

        // ── Compander, expand half ────────────────────────────────────────
        // The expander's envelope is floored by the SAME constant that guards
        // the compressor's divide, and for the same reason: the two halves have
        // to stay inverse. Without the floor the pair's round-trip gain is
        // env_out/env_in, which collapses to zero after silence — the compressed
        // side is multiplying by 100 while the expanded side multiplies by
        // nothing. Audibly that shows up two ways: a quiet passage after silence
        // arrives late and small while the expander catches up, and a
        // self-oscillating loop (feedback above unity, which this character is
        // specified to support) dies instead of sustaining, because each pass
        // loses the whole compander mismatch.
        //
        // The expander multiplies by the envelope as it stood BEFORE this
        // sample's update, mirroring the compressor, which divides by its
        // envelope before updating. Matching the two matters: with the
        // compressor reading the old envelope and the expander the new one, a
        // signal arriving after silence is divided by the floor (a gain of 100)
        // and multiplied by an envelope that has already jumped off the floor,
        // so the pair's round-trip gain is several times unity. In a feedback
        // loop that compounds — measured as an echo train that GREW at a
        // feedback setting of 0.5, which is a runaway, not character.
        const double gain = std::max(expand_envelope_, kBbdCompanderFloor);
        expand_envelope_ = flush_denormal(
            compander_coefficient_ * expand_envelope_ +
            (1.0 - compander_coefficient_) * std::abs(first_read));
        return compander_enabled_ ? first_read * gain : first_read;
    }

private:
    /// Hard bound on clock ticks per internal step. Reached only if the clock
    /// period is driven far below what the parameter clamps allow.
    static constexpr int kMaxTicksPerStep = 64;

    /// Asymmetric soft clip by the DC-offset method — a standard published
    /// technique for generating even harmonics from an odd-symmetric shaper.
    /// The rational tanh approximation stands in for tanh here because this
    /// runs kBbdOversample times per sample per channel; it is one of the
    /// sanctioned nonlinearities and matches tanh to well under a percent.
    double waveshape(double x) const noexcept {
        return (fast_tanh(shaper_gain_ * x + shaper_offset_) - shaper_offset_tanh_) *
               shaper_compensation_;
    }

    /// One jitter draw per CLOCK TICK, not per sample — clock jitter is a
    /// property of the oscillator, so its statistics must not change when the
    /// host sample rate does.
    double next_jitter() noexcept {
        if (jitter_amount_ <= 0.0) return 0.0;
        const double coefficient = 0.9 + kBbdJitterSmoothness * 0.0999;
        jitter_state_ = coefficient * jitter_state_ + (1.0 - coefficient) * rng_.bipolar();
        return jitter_state_ * jitter_amount_ * kBbdJitterMax * clock_period_;
    }

    std::array<float, kBbdMaxStages> buckets_{};
    std::size_t write_ = 0;
    std::size_t stages_ = kBbdMaxStages;

    Svf2 pre_lowpass_;
    Svf2 pre_highpass_;
    Svf2 post_lowpass_;
    Xorshift32 rng_{kPrngSeed};

    double sample_rate_ = 48000.0;
    double oversampled_rate_ = 48000.0 * kBbdOversample;
    double clock_period_ = 1.0 / 48000.0;
    double bandwidth_hz_ = kBbdBandwidthMaxHz;
    double time_accumulator_ = 0.0;
    double previous_input_ = 0.0;
    double jitter_state_ = 0.0;
    double jitter_amount_ = 0.0;
    double drive_ = 0.0;
    double shaper_gain_ = 1.0;
    double shaper_offset_ = 0.0;
    double shaper_offset_tanh_ = 0.0;
    double shaper_compensation_ = 1.0;
    double compander_coefficient_ = 0.0;
    double compress_envelope_ = 1.0;
    double expand_envelope_ = 1.0;
    bool compander_enabled_ = true;
};

}  // namespace pulp::signal::chardelay
