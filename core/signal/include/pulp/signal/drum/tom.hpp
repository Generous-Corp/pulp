#pragma once

#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/drum/layers.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/ladder_filter.hpp>
#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace pulp::signal::drum {

/// A tom, and the analogue electronic-drum voice it generalises.
///
/// The defining property is that the pitch envelope and the amplitude envelope
/// are **decoupled**. In most percussion voices the body's pitch and its level
/// fall together, because one envelope drives both; here they are separate
/// controls, so the pitch can finish its dive in a few tens of milliseconds
/// while the note rings on for a second afterwards. That is the sound of the
/// early electronic drum kits: a fast downward swoop that arrives at a pitch
/// and then simply sustains there. Tying the two envelopes together cannot
/// produce it -- the swoop would take the level with it.
///
/// The generic tom and the electronic voice are the same topology, retuned. A
/// short bend and a mostly-noise balance is an acoustic-sounding tom; a deep
/// bend and a pure oscillator is the laser-like electronic voice; a long decay
/// with a high tuning is the "cosmic" end. Presets differ only in numbers, so
/// there is one implementation rather than one per named sound.
///
/// Velocity **deepens the bend** rather than only raising the level, which is
/// the physical claim: a harder strike deflects the head further, so the pitch
/// starts higher and falls through a wider range. The voice's default
/// `VelocityResponse` enables this, and it is the clearest single example of
/// the velocity-to-timbre contract in the whole percussion set.
///
/// The noise path runs through a four-pole resonant lowpass rather than the
/// two-pole used elsewhere, because at high resonance a steeper filter rings
/// enough to give the noise a pitch of its own -- which is what lets the same
/// voice cover a snare-like sound without a second oscillator.
///
/// RT contract: `prepare()` allocates nothing; every other method, including
/// `note_on` and `process`, allocates nothing and takes no locks.
class TomVoice : public Voice {
public:
    /// Body waveform. Triangle is the authentic choice for the analogue
    /// electronic voices -- their oscillators were triangle cores -- and it
    /// carries odd harmonics that survive a small speaker where a sine does
    /// not.
    enum class Wave { triangle, sine };

    enum class Preset {
        generic_tom,
        low_tom,
        mid_tom,
        hi_tom,
        bass,
        snary,
        zap,
        cosmic,
    };

    struct PresetData {
        Preset id;
        std::string_view name;
        double tune_hz;
        double bend_octaves;
        double bend_ms;
        double decay_ms;
        Wave wave;
        double noise_balance;
        double noise_cutoff_hz;
        double noise_resonance;
        double click_level;
    };

    /// Original Pulp-authored voicings of this one SDS-V-family topology. They
    /// are clean starting points, not values copied from a sample library or a
    /// commercial preset bank.
    static constexpr std::array<PresetData, 8> presets{{
        {Preset::generic_tom, "Generic Tom", 130.0, 0.35, 30.0, 450.0,
         Wave::triangle, 0.25, 2200.0, 0.25, 0.35},
        {Preset::low_tom, "LowTom", 75.0, 0.80, 45.0, 700.0,
         Wave::triangle, 0.08, 1100.0, 0.25, 0.30},
        {Preset::mid_tom, "MidTom", 110.0, 0.90, 35.0, 550.0,
         Wave::triangle, 0.10, 1600.0, 0.28, 0.32},
        {Preset::hi_tom, "HiTom", 170.0, 1.00, 28.0, 400.0,
         Wave::triangle, 0.12, 2400.0, 0.30, 0.35},
        {Preset::bass, "Bass", 55.0, 0.55, 55.0, 1100.0,
         Wave::sine, 0.04, 900.0, 0.18, 0.22},
        {Preset::snary, "Snary", 155.0, 0.25, 18.0, 320.0,
         Wave::triangle, 0.70, 2500.0, 0.82, 0.50},
        {Preset::zap, "Zap", 210.0, 3.20, 55.0, 500.0,
         Wave::triangle, 0.02, 3200.0, 0.20, 0.15},
        {Preset::cosmic, "Cosmic", 280.0, 2.40, 180.0, 1800.0,
         Wave::sine, 0.20, 6000.0, 0.70, 0.25},
    }};

    TomVoice() {
        VelocityResponse r;
        r.level_db = 15.0f;
        // The signature coupling: velocity deepens the dive.
        r.bend_octaves = 1.5f;
        r.brightness_octaves = 1.0f;
        set_velocity_response(r);

        click_.set_level(0.35);
        click_.set_cutoff_hz(5000.0);
        click_.set_decay_ms(2.0);
    }

    /// Pitch the body settles at, in Hz.
    void set_tune_hz(double hz) { tune_hz_ = std::clamp(hz, 30.0, 1200.0); }

    /// Depth of the downward dive in octaves, and how quickly it lands. This
    /// envelope is independent of the amplitude decay below -- that separation
    /// is the whole point of the voice.
    void set_bend_octaves(double octaves) { bend_octaves_ = std::clamp(octaves, 0.0, 6.0); }
    void set_bend_ms(double ms) { bend_ms_ = std::clamp(ms, 0.5, 500.0); }

    /// How long the note rings, as a T60. Unrelated to the bend.
    void set_decay_ms(double ms) { decay_ms_ = std::clamp(ms, 10.0, 4000.0); }

    void set_wave(Wave wave) { wave_ = wave; }

    /// Balance between the oscillator (0) and the filtered noise (1).
    void set_noise_balance(double amount) { noise_balance_ = std::clamp(amount, 0.0, 1.0); }

    /// Corner and resonance of the four-pole lowpass the noise runs through.
    /// At high resonance it rings, which gives the noise a pitch.
    void set_noise_cutoff_hz(double hz) { noise_cutoff_hz_ = std::clamp(hz, 50.0, 16000.0); }
    void set_noise_resonance(double amount) {
        noise_resonance_ = std::clamp(amount, 0.0, 1.0);
    }

    void set_noise_color(NoiseColor color) { noise_.set_color(color); }

    /// The beater. Short and highpassed: a tom's attack is the stick, not the
    /// head.
    void set_click_level(double level) { click_.set_level(level); }
    void set_click_cutoff_hz(double hz) { click_.set_cutoff_hz(hz); }
    void set_click_decay_ms(double ms) { click_.set_decay_ms(ms); }

    void apply_preset(Preset preset) {
        const auto it = std::find_if(
            presets.begin(), presets.end(),
            [preset](const PresetData& data) { return data.id == preset; });
        if (it == presets.end()) return;
        set_tune_hz(it->tune_hz);
        set_bend_octaves(it->bend_octaves);
        set_bend_ms(it->bend_ms);
        set_decay_ms(it->decay_ms);
        set_wave(it->wave);
        set_noise_balance(it->noise_balance);
        set_noise_cutoff_hz(it->noise_cutoff_hz);
        set_noise_resonance(it->noise_resonance);
        set_click_level(it->click_level);
    }

    OutputStage& output() { return output_; }
    const OutputStage& output() const { return output_; }
    int latency_samples() const noexcept override {
        return output_.latency_samples();
    }
    OutputOversampling output_oversampling() const noexcept override {
        return output_.oversampling();
    }
    void set_output_oversampling(OutputOversampling factor) override {
        output_.set_oversampling(factor);
    }

protected:
    void on_prepare(double sample_rate) override {
        noise_.prepare(sample_rate);
        amp_env_.set_sample_rate(sample_rate);
        bend_env_.set_sample_rate(sample_rate);
        click_.prepare(sample_rate);
        noise_filter_.set_sample_rate(static_cast<float>(sample_rate));
        click_hp_.prepare(static_cast<float>(sample_rate));
        output_.prepare(sample_rate);
    }

    void on_reset() override {
        amp_env_.reset();
        bend_env_.reset();
        click_.reset();
        click_hp_.reset();
        output_.reset();
        noise_.reset();
        phase_ = 0.0;
    }

    void on_note_on(float velocity) override {
        output_.reset_nonlinear_state();
        output_.trigger();
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);
        // The bend deepens with velocity; the decay deliberately does not, so a
        // soft hit is a shallower swoop of the same length rather than a
        // shorter note.
        applied_bend_ = bend_octaves_ + response.bend(velocity);
        brightness_ = response.brightness_scale(velocity);

        noise_.reset();
        phase_ = 0.0;
        click_hp_.reset();

        bend_env_.set_attack_ms(0.0);
        bend_env_.set_decay_time_constant_ms(bend_ms_);
        bend_env_.trigger();

        amp_env_.set_attack_ms(0.5);
        amp_env_.set_decay_t60_ms(decay_ms_);
        amp_env_.trigger();

        click_.trigger(brightness_);

        noise_filter_.set_frequency(static_cast<float>(
            std::min(noise_cutoff_hz_ * brightness_, 0.45 * sample_rate())));
        noise_filter_.set_resonance(static_cast<float>(noise_resonance_));
    }

    bool on_is_active() const override {
        return amp_env_.is_active() || click_.is_active() || output_.has_tail();
    }

    void render_add(float* out, int num_samples) override {
        const double oscillator_mix = 1.0 - noise_balance_;

        for (int i = 0; i < num_samples; ++i) {
            // Two envelopes, read independently. The bend has usually finished
            // long before the amplitude has.
            const double bend = bend_env_.process();
            const double amplitude = amp_env_.process();

            double body = 0.0;
            if (oscillator_mix > 0.0) {
                const double f = std::min(tune_hz_ * std::exp2(applied_bend_ * bend),
                                          0.49 * sample_rate());
                phase_ += f / sample_rate();
                if (phase_ >= 1.0) phase_ -= std::floor(phase_);
                body = oscillator_mix * (wave_ == Wave::triangle
                                             ? 2.0 * std::fabs(2.0 * phase_ - 1.0) - 1.0
                                             : std::sin(2.0 * 3.14159265358979323846 * phase_));
            }

            double hiss = 0.0;
            if (noise_balance_ > 0.0) {
                hiss = noise_balance_ *
                       static_cast<double>(noise_filter_.process(noise_.process()));
            }

            const double beater =
                click_hp_.process_highpass(static_cast<float>(
                    click_.process(static_cast<double>(noise_.white()))));

            const double mixed = (body + hiss) * amplitude + beater;
            out[i] += static_cast<float>(
                output_.process(static_cast<float>(mixed)) * velocity_gain_);
        }
    }

private:
    double tune_hz_ = 120.0;
    double bend_octaves_ = 1.0;
    double bend_ms_ = 30.0;
    double decay_ms_ = 500.0;
    Wave wave_ = Wave::triangle;
    double noise_balance_ = 0.15;
    double noise_cutoff_hz_ = 1500.0;
    double noise_resonance_ = 0.3;

    NoiseSource noise_;
    DecayEnvelope64 amp_env_;
    DecayEnvelope64 bend_env_;
    ClickLayer click_;
    LadderFilter noise_filter_;
    TptFilter click_hp_;
    OutputStage output_;

    double velocity_gain_ = 1.0;
    double applied_bend_ = 1.0;
    double brightness_ = 1.0;
    double phase_ = 0.0;
};

}  // namespace pulp::signal::drum
