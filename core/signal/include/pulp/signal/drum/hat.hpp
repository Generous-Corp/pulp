#pragma once

#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/square_osc_bank.hpp>
#include <pulp/signal/svf.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::signal::drum {

/// A hi-hat, and by extension every other struck metal in a kit.
///
/// Closed hat, open hat, ride and crash are one voice with one control
/// changed. They are not different instruments: they are the same pair of
/// cymbals excited the same way and allowed to ring for different lengths of
/// time, which is exactly what `set_decay_ms` spans. Building them as four
/// voices would mean four things to keep in tune with each other; building them
/// as one means a kit pairs two instances in a choke group and gets the
/// closed-cuts-open behaviour for free.
///
/// The metallic source is a bank of square oscillators at deliberately
/// inharmonic frequencies. Two reasons it is squares and not sines: a square
/// already carries only odd harmonics, so six of them at incommensurate
/// pitches fill the spectrum densely, and the ratios below are the ones the
/// TR-808's own six-oscillator bank used. The ratios are stored relative to the
/// lowest oscillator rather than as absolute frequencies so that tuning the
/// voice moves the whole cluster and preserves its character.
///
/// Blending that cluster against filtered noise is what spans the two eras of
/// drum machine in one control: an all-oscillator hat is the 808's, an
/// all-noise hat is closer to the sampled machines that followed.
///
/// RT contract: `prepare()` sizes the oscillator bank and may allocate.
/// Everything else, including `note_on` and `process`, allocates nothing.
class HatVoice : public Voice {
public:
    /// Number of oscillators in the metallic bank.
    static constexpr std::size_t partial_count = 6;

    /// Frequency ratios of the TR-808's hi-hat oscillator bank, relative to
    /// its lowest oscillator. The absolute frequencies these come from are
    /// documented in the machine's service manual; they are stored as ratios
    /// here so `set_tune_hz` moves the cluster without changing its shape.
    static constexpr double partial_ratios[partial_count] = {
        1.0, 1.4827, 1.8003, 2.5461, 2.6303, 3.8967};

    HatVoice() {
        VelocityResponse r;
        r.level_db = 16.0f;
        r.brightness_octaves = 0.8f;
        set_velocity_response(r);
    }

    /// Frequency of the lowest oscillator in the bank, in Hz.
    void set_tune_hz(double hz) { tune_hz_ = std::clamp(hz, 40.0, 4000.0); }

    /// How long the metal rings, as a T60 in milliseconds. This single control
    /// is the closed-to-open-to-ride-to-crash continuum: tens of milliseconds
    /// is a closed hat, hundreds is open, seconds is a cymbal.
    void set_decay_ms(double ms) { decay_ms_ = std::clamp(ms, 5.0, 8000.0); }

    /// How far the bank is spread from unison (0) to its full inharmonic
    /// cluster (1). At zero every oscillator sits on the fundamental and the
    /// voice is a square wave; the metallic quality is entirely in the spread.
    void set_spread(double amount) { spread_ = std::clamp(amount, 0.0, 1.0); }

    /// Balance between filtered noise (0) and the oscillator cluster (1).
    void set_metal(double amount) { metal_ = std::clamp(amount, 0.0, 1.0); }

    /// Depth of a ring modulation applied to the source before filtering, and
    /// the modulator's ratio to the tuning. Ring modulation of an already
    /// inharmonic cluster produces sum and difference tones that belong to no
    /// series at all, which is the grit a hi-hat has and a filtered square
    /// does not.
    void set_grit(double amount) { grit_ = std::clamp(amount, 0.0, 1.0); }
    void set_grit_ratio(double ratio) { grit_ratio_ = std::clamp(ratio, 0.25, 16.0); }

    /// Corner and resonance of the output filter. A hat is defined as much by
    /// what is removed as by what is generated: the source is broadband, and
    /// the highpass is what leaves only the part that reads as metal.
    void set_cutoff_hz(double hz) { cutoff_hz_ = std::clamp(hz, 200.0, 18000.0); }
    void set_resonance(double q) { resonance_ = std::clamp(q, 0.5, 12.0); }

    /// Use a bandpass rather than a highpass. Narrows the voice toward a
    /// specific pitched clank.
    void set_bandpass(bool bandpass) { bandpass_ = bandpass; }

    void set_noise_color(NoiseColor color) { noise_.set_color(color); }

    OutputStage& output() { return output_; }

protected:
    void on_prepare(double sample_rate) override {
        noise_.prepare(sample_rate);
        envelope_.set_sample_rate(sample_rate);
        bank_.prepare(static_cast<float>(sample_rate));

        SquareOscBank::Partial partials[partial_count];
        bank_.set_partials(partials, partial_count);
        apply_tuning();

        filter_.set_sample_rate(static_cast<float>(sample_rate));
        output_.prepare(sample_rate);
    }

    void on_reset() override {
        envelope_.reset();
        bank_.reset();
        filter_.reset();
        output_.reset();
        noise_.reset();
        grit_phase_ = 0.0;
    }

    void on_note_on(float velocity) override {
        output_.reset();
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);
        brightness_ = response.brightness_scale(velocity);

        noise_.reset();
        apply_tuning();
        apply_filter();

        // The oscillator bank is deliberately not reset. Its phases are what
        // make one hat hit differ from the next: a cymbal is not re-struck from
        // a known state, and restarting the cluster in phase every time is the
        // machine-gun artefact in its most audible form -- every hit would open
        // with the same broadband click.
        envelope_.set_attack_ms(0.3);
        envelope_.set_decay_t60_ms(decay_ms_);
        envelope_.trigger();
    }

    bool on_is_active() const override {
        return envelope_.is_active() || output_.has_tail();
    }

    void render_add(float* out, int num_samples) override {
        const double noise_mix = 1.0 - metal_;
        for (int i = 0; i < num_samples; ++i) {
            double source = 0.0;
            if (metal_ > 0.0) source += metal_ * static_cast<double>(bank_.process());
            if (noise_mix > 0.0) source += noise_mix * static_cast<double>(noise_.process());

            if (grit_ > 0.0) {
                grit_phase_ += tune_hz_ * grit_ratio_ / sample_rate();
                if (grit_phase_ >= 1.0) grit_phase_ -= std::floor(grit_phase_);
                const double modulator =
                    std::sin(2.0 * 3.14159265358979323846 * grit_phase_);
                source *= 1.0 + grit_ * modulator;
            }

            const double filtered = filter_.process(static_cast<float>(source));
            const double shaped = filtered * envelope_.process();
            out[i] += static_cast<float>(
                output_.process(static_cast<float>(shaped)) * velocity_gain_);
        }
    }

private:
    void apply_tuning() {
        for (std::size_t i = 0; i < partial_count; ++i) {
            // Spread interpolates each oscillator between unison and its
            // documented ratio, so the control is continuous rather than a
            // switch between "square wave" and "hi-hat".
            const double ratio = 1.0 + spread_ * (partial_ratios[i] - 1.0);
            bank_.set_frequency(i, static_cast<float>(
                std::min(tune_hz_ * ratio, 0.49 * sample_rate())));
            bank_.set_amplitude(i, 1.0f / static_cast<float>(partial_count));
        }
    }

    void apply_filter() {
        filter_.set_mode(bandpass_ ? Svf::Mode::bandpass : Svf::Mode::highpass);
        filter_.set_frequency(static_cast<float>(
            std::min(cutoff_hz_ * brightness_, 0.49 * sample_rate())));
        filter_.set_resonance(static_cast<float>(resonance_));
    }

    double tune_hz_ = 320.0;
    double decay_ms_ = 60.0;
    double spread_ = 1.0;
    double metal_ = 0.8;
    double grit_ = 0.0;
    double grit_ratio_ = 1.4;
    double cutoff_hz_ = 7000.0;
    double resonance_ = 1.2;
    bool bandpass_ = false;

    NoiseSource noise_;
    SquareOscBank bank_;
    Svf filter_;
    DecayEnvelope64 envelope_;
    OutputStage output_;

    double velocity_gain_ = 1.0;
    double brightness_ = 1.0;
    double grit_phase_ = 0.0;
};

}  // namespace pulp::signal::drum
