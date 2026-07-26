#pragma once

#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/frequency_shifter.hpp>
#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pulp::signal::drum {

/// A cymbal, built from a bank of comb filters and then deliberately
/// de-tuned out of any musical relationship.
///
/// A comb filter is a delay fed back on itself, so it rings at every multiple
/// of one over its delay — a harmonic series. Eight of them at plate-like
/// ratios gives a dense metallic wash, which is most of the way to a cymbal.
/// The problem is that it is still *chordal*: each comb is individually
/// harmonic, so the bank rings on a set of pitches and reads as tuned metal
/// rather than as a crash.
///
/// The fix is the interesting part. A frequency shifter adds a constant number
/// of hertz to everything rather than multiplying by a ratio, so a comb's
/// series at 400, 800, 1200 Hz becomes 437, 837, 1237 Hz — no longer multiples
/// of anything, and with no fundamental left to hear as a pitch. Shifting by a
/// few tens of hertz is what turns the bank from a chord into a cymbal, and no
/// amount of detuning or filtering does the same job, because detuning moves
/// the series and shifting destroys it.
///
/// Per-hit variation comes from advancing the noise seed, so two crashes in a
/// row differ the way two real strikes do; setting variation to zero makes the
/// voice bit-reproducible again.
///
/// The parallel-comb cymbal technique is credited to Zion Jaymes' cymbal
/// synthesis tutorial. The implementation here is original, and the frequency
/// shifter it depends on is Pulp's own
/// (`pulp::signal::FrequencyShifterT`).
///
/// RT contract: `prepare()` allocates the delay lines. Everything else,
/// including `note_on` and `process`, allocates nothing.
class CymbalVoice : public Voice {
public:
    /// Number of parallel combs.
    static constexpr int comb_count = 8;

    /// Plate-like ratios for the comb delays. Deliberately not a harmonic
    /// series — a bank at integer ratios would reinforce one pitch, which is
    /// the opposite of what a cymbal does.
    static constexpr double comb_ratios[comb_count] = {1.00, 1.47, 1.83, 2.21,
                                                        2.59, 3.16, 3.71, 4.31};

    CymbalVoice() {
        VelocityResponse r;
        r.level_db = 15.0f;
        r.brightness_octaves = 1.2f;
        set_velocity_response(r);
    }

    /// Frequency the lowest comb rings at, in Hz.
    void set_tune_hz(double hz) { tune_hz_ = std::clamp(hz, 40.0, 2000.0); }

    /// How long the bank rings, as a T60 in milliseconds.
    void set_decay_ms(double ms) { decay_ms_ = std::clamp(ms, 50.0, 8000.0); }

    /// How much faster each successive comb dies than the one below it. Higher
    /// modes losing energy first is what makes a crash settle into a wash.
    void set_decay_tilt(double tilt) { decay_tilt_ = std::clamp(tilt, 0.5, 1.0); }

    /// Extra high-mode level at the top comb, interpolated from zero dB at the
    /// bottom comb. This changes the plate balance without changing the shared
    /// output tone filter.
    void set_high_mode_emphasis_db(double db) {
        high_mode_emphasis_db_ = std::clamp(db, -18.0, 18.0);
    }

    /// Velocity-dependent feedback and high-mode emphasis. Both are explicit
    /// because feedback also changes ring time: zero keeps decay independent
    /// of velocity; one doubles the top comb's T60 at full velocity. A positive
    /// value opts into the harder-hit-rings-harder cymbal behavior.
    void set_velocity_feedback(double amount) {
        velocity_feedback_ = std::clamp(amount, 0.0, 1.0);
    }
    void set_velocity_high_mode_db(double db) {
        velocity_high_mode_db_ = std::clamp(db, 0.0, 18.0);
    }

    /// Base cutoff of the extra high-pass stages in the upper half of the comb
    /// bank. Successive upper modes rise from this corner; zero bypasses them.
    void set_upper_highpass_hz(double hz) {
        upper_highpass_hz_ = std::clamp(hz, 0.0, 8000.0);
    }

    /// How far the combs are spread from unison (0) to their full plate ratios
    /// (1).
    void set_spread(double amount) { spread_ = std::clamp(amount, 0.0, 1.0); }

    /// How far the ratios are pushed further apart still, progressively.
    void set_inharmonicity(double amount) {
        inharmonicity_ = std::clamp(amount, 0.0, 1.0);
    }

    /// How many hertz every partial is shifted by. This is the control that
    /// turns tuned metal into a cymbal; at 0 the bank is audibly chordal.
    void set_shift_hz(double hz) { shift_hz_ = std::clamp(hz, -400.0, 400.0); }

    /// Level of the noise part of the strike, and of the swept tone under it.
    void set_noise_level(double level) { noise_level_ = std::max(level, 0.0); }
    void set_strike_level(double level) { strike_level_ = std::max(level, 0.0); }
    void set_strike_ms(double ms) { strike_ms_ = std::clamp(ms, 0.5, 200.0); }

    /// Output tone controls: a lowpass on top and a highpass to clear the mud
    /// the shifter leaves at the bottom.
    void set_tone_hz(double hz) { tone_hz_ = std::clamp(hz, 500.0, 20000.0); }
    void set_low_cut_hz(double hz) { low_cut_hz_ = std::clamp(hz, 20.0, 2000.0); }

    /// How much successive hits differ, 0 (bit-identical) to 1.
    void set_variation(double amount) { variation_ = std::clamp(amount, 0.0, 1.0); }

    void set_noise_color(NoiseColor color) { noise_.set_color(color); }

    OutputStage& output() { return output_; }

protected:
    void on_prepare(double sample_rate) override {
        noise_.prepare(sample_rate);
        strike_env_.set_sample_rate(sample_rate);
        // The longest delay the lowest tuning can ask for, plus headroom.
        const int longest = static_cast<int>(sample_rate / 40.0) + 4;
        for (auto& comb : combs_) comb.prepare(longest);
        for (auto& damper : dampers_) damper.prepare(static_cast<float>(sample_rate));
        for (auto& highpass : upper_highpasses_) {
            highpass.prepare(static_cast<float>(sample_rate));
        }
        shifter_.set_sample_rate(sample_rate);
        tone_.prepare(static_cast<float>(sample_rate));
        low_cut_.prepare(static_cast<float>(sample_rate));
        output_.prepare(sample_rate);
    }

    void on_reset() override {
        for (auto& comb : combs_) comb.reset();
        for (auto& damper : dampers_) damper.reset();
        for (auto& highpass : upper_highpasses_) highpass.reset();
        strike_env_.reset();
        shifter_.reset();
        tone_.reset();
        low_cut_.reset();
        output_.reset();
        noise_.reset();
        seed_advance_ = 0;
        level_ = 0.0;
        strike_phase_ = 0.0;
    }

    void on_note_on(float velocity) override {
        output_.reset();
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);
        const double brightness = response.brightness_scale(velocity);
        velocity_ = velocity;

        // Variation advances the seed per hit, so two crashes differ; at zero
        // the seed is fixed and the voice is reproducible.
        if (variation_ > 0.0) {
            ++seed_advance_;
            noise_.set_seed(NoiseSource::default_seed +
                            static_cast<std::uint32_t>(seed_advance_ * 2654435761u));
        } else {
            noise_.set_seed(NoiseSource::default_seed);
        }
        noise_.reset();

        update_combs();
        shifter_.set_shift_hz(shift_hz_);
        tone_.set_cutoff(static_cast<float>(
            std::min(tone_hz_ * brightness, 0.49 * sample_rate())));
        low_cut_.set_cutoff(static_cast<float>(low_cut_hz_));

        strike_env_.set_attack_ms(0.0);
        strike_env_.set_decay_time_constant_ms(strike_ms_);
        strike_env_.trigger();
        strike_phase_ = 0.0;
        level_ = 1.0;
    }

    bool on_is_active() const override {
        return strike_env_.is_active() || level_ > kSilenceLevel ||
               output_.has_tail();
    }

    void render_add(float* out, int num_samples) override {
        for (int i = 0; i < num_samples; ++i) {
            const double strike = strike_env_.is_active() ? strike_env_.process() : 0.0;

            double excitation = 0.0;
            if (strike > 0.0) {
                excitation = static_cast<double>(noise_.process()) * noise_level_;
                if (strike_level_ > 0.0) {
                    // A short downward chirp under the noise: the sound of the
                    // stick itself rather than of the metal.
                    strike_phase_ += tune_hz_ * (1.0 + 3.0 * strike) / sample_rate();
                    if (strike_phase_ >= 1.0) strike_phase_ -= std::floor(strike_phase_);
                    excitation += std::sin(2.0 * 3.14159265358979323846 * strike_phase_) *
                                  strike_level_;
                }
                excitation *= strike;
            }

            double summed = 0.0;
            for (int c = 0; c < comb_count; ++c) {
                const auto index = static_cast<std::size_t>(c);
                const float delayed =
                    combs_[index].read(static_cast<float>(delay_samples_[index]));
                float damped = dampers_[index].process_lowpass(delayed);
                if (c >= kUpperCombFirst) {
                    const float highpassed =
                        upper_highpasses_[index].process_highpass(damped);
                    if (upper_highpass_hz_ > 0.0) damped = highpassed;
                }
                combs_[index].push(static_cast<float>(
                    excitation + static_cast<double>(damped) * feedback_[index]));
                summed += static_cast<double>(delayed) * mode_gain_[index];
            }
            summed /= static_cast<double>(comb_count);

            // The de-harmoniser. Everything above it is a chord; everything
            // after it is a cymbal.
            const double shifted =
                shift_hz_ != 0.0
                    ? static_cast<double>(shifter_.process(static_cast<float>(summed)))
                    : summed;

            const double cleared = low_cut_.process_highpass(static_cast<float>(shifted));
            const double toned = tone_.process_lowpass(static_cast<float>(cleared));
            const double shaped =
                output_.process(static_cast<float>(toned)) * velocity_gain_;

            out[i] += static_cast<float>(shaped);
            level_ = std::max(std::fabs(shaped), level_ * kLevelDecay);
        }
    }

private:
    static constexpr double kSilenceLevel = 1e-5;
    static constexpr double kLevelDecay = 0.99995;

    void update_combs() {
        for (int c = 0; c < comb_count; ++c) {
            const auto index = static_cast<std::size_t>(c);
            // Spread interpolates each comb from unison to its plate ratio;
            // inharmonicity then pushes the upper ones further out still.
            const double base = 1.0 + spread_ * (comb_ratios[index] - 1.0);
            const double pushed =
                base * (1.0 + inharmonicity_ * 0.01 * static_cast<double>(c * c));
            const double f = std::min(tune_hz_ * pushed, 0.45 * sample_rate());
            delay_samples_[index] = std::max(sample_rate() / f, 2.0);

            // A comb's feedback reaching -60 dB after `decay_ms` means losing
            // that much per round trip, and the trip rate is the comb's own
            // frequency -- so this is derived rather than dialled, and stays
            // meaningful when the tuning moves. (Jot's law, applied per comb: a
            // raw coefficient would mean very different decay lengths at
            // different comb frequencies.) The modal tilt sits on top of it.
            const double trips = std::max(0.001 * decay_ms_ * f, 1.0);
            const double tilt = std::pow(decay_tilt_, static_cast<double>(c));
            const double upper_position =
                static_cast<double>(c) / static_cast<double>(comb_count - 1);
            const double velocity_ring =
                1.0 + velocity_feedback_ * velocity_ * upper_position;
            feedback_[index] =
                std::min(std::pow(10.0, -3.0 / (trips * tilt * velocity_ring)),
                         0.9995);

            mode_gain_[index] = std::pow(
                10.0, (high_mode_emphasis_db_ +
                       velocity_high_mode_db_ * velocity_) *
                          upper_position / 20.0);

            // Higher combs are damped harder in the loop, so the wash darkens
            // as it decays instead of staying uniformly bright.
            const double corner =
                std::min(0.45 * sample_rate(), 16000.0 * std::pow(0.72, static_cast<double>(c)));
            dampers_[index].set_cutoff(static_cast<float>(corner));
            if (c >= kUpperCombFirst) {
                const double highpass_corner =
                    std::min(0.45 * sample_rate(),
                             std::max(upper_highpass_hz_, 1.0) *
                                 std::pow(1.35, static_cast<double>(c - kUpperCombFirst)));
                upper_highpasses_[index].set_cutoff(
                    static_cast<float>(highpass_corner));
            }
        }
    }

    static constexpr int kUpperCombFirst = comb_count / 2;

    double tune_hz_ = 320.0;
    double decay_ms_ = 1800.0;
    double decay_tilt_ = 0.93;
    double high_mode_emphasis_db_ = 0.0;
    double velocity_feedback_ = 0.35;
    double velocity_high_mode_db_ = 4.0;
    double upper_highpass_hz_ = 500.0;
    double spread_ = 1.0;
    double inharmonicity_ = 0.4;
    double shift_hz_ = 45.0;
    double noise_level_ = 0.7;
    double strike_level_ = 0.3;
    double strike_ms_ = 6.0;
    double tone_hz_ = 12000.0;
    double low_cut_hz_ = 300.0;
    double variation_ = 0.5;

    NoiseSource noise_;
    std::array<DelayLine, comb_count> combs_;
    std::array<TptFilter, comb_count> dampers_;
    std::array<TptFilter, comb_count> upper_highpasses_;
    DecayEnvelope64 strike_env_;
    FrequencyShifter shifter_;
    TptFilter tone_;
    TptFilter low_cut_;
    OutputStage output_;

    std::array<double, comb_count> delay_samples_{};
    std::array<double, comb_count> feedback_{};
    std::array<double, comb_count> mode_gain_{};

    double velocity_gain_ = 1.0;
    double velocity_ = 1.0;
    double level_ = 0.0;
    double strike_phase_ = 0.0;
    unsigned seed_advance_ = 0;
};

}  // namespace pulp::signal::drum
