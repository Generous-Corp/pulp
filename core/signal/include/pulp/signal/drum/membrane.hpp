#pragma once

#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/lowpass_gate.hpp>
#include <pulp/signal/modal_bank.hpp>
#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace pulp::signal::drum {

/// What strikes the modal membrane.
enum class MembraneExciter {
    /// A short filtered-noise burst: stick or hand depending on its length.
    noise_burst,
    /// One filtered sample: a true impulse that rings the body without a
    /// continuing noise texture.
    pluck,
};

/// A struck membrane, modelled as its modes rather than as an oscillator.
///
/// Everything the voice does follows from one idea: a drumhead has a set of
/// resonances, striking it excites all of them at once, and they die away at
/// different rates. Nothing here has a waveform. The character comes from
/// which frequencies the modes sit at and how their decays differ, so the
/// controls are physical (how big, how damped, where it was struck) rather
/// than synthesis controls (which oscillator, which filter).
///
/// **`structure` is the important one.** At 0 the modes are a harmonic series,
/// which is what a string does and what almost every synthesised drum
/// implicitly assumes. At 1 they are the ratios of an ideal circular membrane —
/// 1, 1.593, 2.135, 2.295, 2.653, 2.917 — which are not integer multiples of
/// anything. That is why a real drum has no clear pitch while a harmonic bank
/// plays a note, and moving between them is the difference between a tom and a
/// tuned percussion instrument. The membrane ratios are published physics
/// (they are the zeros of a Bessel function; see Fletcher & Rossing) rather
/// than anything fitted.
///
/// **`position` is a comb, not a level.** A mode with a node at the strike
/// point cannot be excited by a strike there, so the position silently removes
/// whole partials rather than changing the level: at the midpoint every
/// even-numbered mode disappears entirely. That is why moving the strike point
/// changes which drum it sounds like and not how loud it is.
///
/// The weighting is the one-dimensional `|sin(pi * n * position)|` — the same
/// comb a struck string has. A circular head's true weighting is a Bessel
/// function of the strike radius, which nulls a different set of modes; the
/// sine comb is the standard approximation and is what the mode ratios above
/// are paired with in practice.
///
/// The lowpass gate at the end is what stops the result sounding like a bank
/// of filters: it couples level to brightness, so the tail darkens as it dies
/// the way a physical body does.
///
/// RT contract: `prepare()` sizes the mode bank and allocates. Everything
/// else, including `note_on` and `process`, allocates nothing.
class MembraneVoice : public Voice {
public:
    /// Number of modes the body carries.
    static constexpr int mode_count = 6;

    /// Frequency ratios of an ideal circular membrane's first six modes,
    /// relative to its fundamental. Published physics, not fitted values.
    static constexpr double membrane_ratios[mode_count] = {1.0,   1.5933, 2.1355,
                                                            2.2954, 2.6531, 2.9173};

    MembraneVoice() {
        VelocityResponse r;
        r.level_db = 16.0f;
        // A harder strike tightens the head slightly and excites more of the
        // high modes -- both are brightness rather than level.
        r.bend_octaves = 0.15f;
        r.brightness_octaves = 1.6f;
        set_velocity_response(r);
    }

    /// Fundamental of the body, in Hz.
    void set_tune_hz(double hz) { tune_hz_ = std::clamp(hz, 20.0, 2000.0); }

    /// Blend from a harmonic series (0) to ideal-membrane ratios (1).
    void set_structure(double amount) { structure_ = std::clamp(amount, 0.0, 1.0); }

    /// Extra progressive sharpening of the upper modes, as a stiff plate has.
    void set_stretch(double amount) { stretch_ = std::clamp(amount, 0.0, 1.0); }

    /// How long the fundamental rings, as a T60 in milliseconds.
    void set_decay_ms(double ms) { decay_ms_ = std::clamp(ms, 20.0, 8000.0); }

    /// How much faster the upper modes die than the fundamental. At 0 every
    /// mode rings equally long, which sounds like a bell rather than a drum.
    void set_damping(double amount) { damping_ = std::clamp(amount, 0.0, 1.0); }

    /// Relative level of the upper modes in the strike.
    void set_brightness(double amount) { brightness_ = std::clamp(amount, 0.0, 1.0); }

    /// Where the head is struck, 0 (edge) to 0.5 (midpoint). At the midpoint
    /// every even-numbered mode is nulled and the fundamental is at its
    /// strongest, so the strike point selects which partials exist rather than
    /// how loud the hit is.
    void set_position(double position) { position_ = std::clamp(position, 0.02, 0.5); }

    /// Random detuning of the modes, drawn once per hit. A real head is never
    /// perfectly uniform, and a bank at exact ratios sounds synthetic.
    void set_spread(double amount) { spread_ = std::clamp(amount, 0.0, 1.0); }

    /// Length of the exciter burst and the corner it is filtered at. A short
    /// bright burst is a stick; a longer darker one is a hand.
    void set_exciter_ms(double ms) { exciter_ms_ = std::clamp(ms, 0.1, 50.0); }
    void set_exciter_cutoff_hz(double hz) {
        exciter_cutoff_hz_ = std::clamp(hz, 100.0, 18000.0);
    }
    void set_exciter(MembraneExciter exciter) { exciter_ = exciter; }

    /// Optional layers around the modal body. The sub follows the body's
    /// decay at half the fundamental; air is a short high-passed noise wash;
    /// click is a still-shorter filtered-noise strike.
    void set_sub_level(double level) { sub_level_ = std::max(level, 0.0); }
    void set_air_level(double level) { air_level_ = std::max(level, 0.0); }
    void set_air_decay_ms(double ms) { air_decay_ms_ = std::clamp(ms, 1.0, 500.0); }
    void set_click_level(double level) { click_level_ = std::max(level, 0.0); }
    void set_click_decay_ms(double ms) {
        click_decay_ms_ = std::clamp(ms, 0.1, 50.0);
    }

    /// Sum of squared mode gains after the constant-energy normalization.
    /// Exposed for renderers and matchers that need to distinguish a changed
    /// strike position from an unintended level change.
    double mode_gain_energy() const { return mode_gain_energy_; }

    /// The lowpass gate that couples the tail's level to its brightness.
    LowpassGate& gate() { return gate_; }
    OutputStage& output() { return output_; }

protected:
    void on_prepare(double sample_rate) override {
        noise_.prepare(sample_rate);
        bank_.prepare(sample_rate, mode_count, 1);
        exciter_env_.set_sample_rate(sample_rate);
        gate_env_.set_sample_rate(sample_rate);
        sub_env_.set_sample_rate(sample_rate);
        air_env_.set_sample_rate(sample_rate);
        click_env_.set_sample_rate(sample_rate);
        exciter_filter_.prepare(static_cast<float>(sample_rate));
        air_filter_.prepare(static_cast<float>(sample_rate));
        click_filter_.prepare(static_cast<float>(sample_rate));
        gate_.set_sample_rate(sample_rate);
        output_.prepare(sample_rate);
        scratch_.assign(kBlock, 0.0f);
        modes_.fill(ModalMode{});
    }

    void on_reset() override {
        bank_.reset();
        exciter_env_.reset();
        gate_env_.reset();
        sub_env_.reset();
        air_env_.reset();
        click_env_.reset();
        exciter_filter_.reset();
        air_filter_.reset();
        click_filter_.reset();
        gate_.reset();
        output_.reset();
        noise_.reset();
        air_noise_.set_seed(NoiseSource::default_seed ^ 0xA17F32D1u);
        click_noise_.set_seed(NoiseSource::default_seed ^ 0xC11C74E5u);
        air_noise_.reset();
        click_noise_.reset();
        pluck_pending_ = false;
        sub_phase_ = 0.0;
        applied_fundamental_hz_ = tune_hz_;
        level_ = 0.0;
    }

    void on_note_on(float velocity) override {
        output_.reset();
        output_.trigger();
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);
        const double tension = std::exp2(response.bend(velocity));
        applied_fundamental_hz_ = tune_hz_ * tension;
        const double exciter_scale = response.brightness_scale(velocity);

        noise_.reset();
        air_noise_.reset();
        click_noise_.reset();
        build_modes(tension);
        bank_.set_modes(std::span<const ModalMode>(modes_.data(), mode_count));

        exciter_filter_.reset();
        exciter_filter_.set_cutoff(static_cast<float>(
            std::min(exciter_cutoff_hz_ * exciter_scale, 0.49 * sample_rate())));

        pluck_pending_ = exciter_ == MembraneExciter::pluck;
        if (exciter_ == MembraneExciter::noise_burst) {
            exciter_env_.set_attack_ms(0.0);
            exciter_env_.set_decay_time_constant_ms(exciter_ms_);
            exciter_env_.trigger();
        } else {
            exciter_env_.reset();
        }

        sub_phase_ = 0.0;
        sub_env_.reset();
        sub_env_.set_attack_ms(0.0);
        sub_env_.set_decay_t60_ms(decay_ms_);
        if (sub_level_ > 0.0) sub_env_.trigger();

        air_filter_.reset();
        air_filter_.set_cutoff(static_cast<float>(
            std::min(4000.0 * exciter_scale, 0.49 * sample_rate())));
        air_env_.reset();
        air_env_.set_attack_ms(0.0);
        air_env_.set_decay_time_constant_ms(air_decay_ms_);
        if (air_level_ > 0.0) air_env_.trigger();

        click_filter_.reset();
        click_filter_.set_cutoff(static_cast<float>(
            std::min(8000.0 * exciter_scale, 0.49 * sample_rate())));
        click_env_.reset();
        click_env_.set_attack_ms(0.0);
        click_env_.set_decay_time_constant_ms(click_decay_ms_);
        if (click_level_ > 0.0) click_env_.trigger();

        // The gate's control is a fast one-shot: it opens on the strike and the
        // vactrol's own slow fall does the rest, which is the point of using a
        // gate rather than an envelope.
        gate_env_.set_attack_ms(0.5);
        gate_env_.set_decay_t60_ms(decay_ms_ * 0.5);
        gate_env_.trigger();
        level_ = 1.0;
    }

    bool on_is_active() const override {
        return pluck_pending_ || exciter_env_.is_active() || gate_env_.is_active() ||
               (sub_level_ > 0.0 && sub_env_.is_active()) ||
               (air_level_ > 0.0 && air_env_.is_active()) ||
               (click_level_ > 0.0 && click_env_.is_active()) ||
               level_ > kSilenceLevel || output_.has_tail();
    }

    void render_add(float* out, int num_samples) override {
        int done = 0;
        while (done < num_samples) {
            const int n = std::min<int>(kBlock, num_samples - done);

            // The bank renders a block at a time, so the exciter is built into
            // a scratch first rather than sample-interleaved.
            for (int i = 0; i < n; ++i) {
                double burst = 0.0;
                if (pluck_pending_) {
                    burst = 1.0;
                    pluck_pending_ = false;
                } else if (exciter_env_.is_active()) {
                    burst = static_cast<double>(noise_.white()) *
                            exciter_env_.process();
                }
                scratch_[static_cast<std::size_t>(i)] =
                    exciter_filter_.process_lowpass(static_cast<float>(burst));
            }

            std::fill(body_.begin(), body_.begin() + n, 0.0f);
            bank_.process_add(scratch_.data(), body_.data(), n);

            for (int i = 0; i < n; ++i) {
                const double gated = gate_.process(body_[static_cast<std::size_t>(i)],
                                                   gate_env_.process());
                double layers = 0.0;
                if (sub_level_ > 0.0 && sub_env_.is_active()) {
                    sub_phase_ +=
                        0.5 * applied_fundamental_hz_ / sample_rate();
                    if (sub_phase_ >= 1.0) sub_phase_ -= std::floor(sub_phase_);
                    layers += std::sin(2.0 * 3.14159265358979323846 * sub_phase_) *
                              sub_env_.process() * sub_level_;
                }
                if (air_level_ > 0.0 && air_env_.is_active()) {
                    layers += static_cast<double>(air_filter_.process_highpass(
                                  air_noise_.white())) *
                              air_env_.process() * air_level_;
                }
                if (click_level_ > 0.0 && click_env_.is_active()) {
                    layers += static_cast<double>(click_filter_.process_lowpass(
                                  click_noise_.white())) *
                              click_env_.process() * click_level_;
                }
                const double shaped =
                    output_.process(static_cast<float>(gated + layers)) * velocity_gain_;
                out[done + i] += static_cast<float>(shaped);
                level_ = std::max(std::fabs(shaped), level_ * kLevelDecay);
            }
            done += n;
        }
    }

private:
    static constexpr int kBlock = 128;
    static constexpr double kSilenceLevel = 1e-5;
    static constexpr double kLevelDecay = 0.9995;

    void build_modes(double tension) {
        // A deterministic detune per mode, drawn from the voice's own generator
        // so a hit stays reproducible.
        double raw_gain_energy = 0.0;
        for (int m = 0; m < mode_count; ++m) {
            const double harmonic = static_cast<double>(m + 1);
            const double ratio = harmonic + structure_ * (membrane_ratios[m] - harmonic);
            // Stiffness sharpens the upper modes progressively, which is what
            // separates a plate from an ideal membrane.
            const double stiffened =
                ratio * (1.0 + stretch_ * 0.02 * static_cast<double>(m * m));
            const double detune = 1.0 + spread_ * 0.03 * static_cast<double>(noise_.white());
            const double f = tune_hz_ * tension * stiffened * detune;

            // Upper modes die faster. At damping 0 they all ring equally, which
            // is a bell; a drum needs the spread.
            const double decay_scale =
                std::pow(0.55, damping_ * static_cast<double>(m));

            // Strike position is a comb: a mode with a node at the strike point
            // simply is not excited.
            const double comb = std::fabs(
                std::sin(3.14159265358979323846 * static_cast<double>(m + 1) * position_));
            const double rolloff =
                std::pow(0.6 + 0.4 * brightness_, static_cast<double>(m));

            const double raw_gain = comb * rolloff;
            raw_gain_energy += raw_gain * raw_gain;
            modes_[static_cast<std::size_t>(m)] = ModalMode{
                static_cast<float>(std::min(f, 0.49 * sample_rate())),
                static_cast<float>(0.001 * decay_ms_ * decay_scale),
                static_cast<float>(raw_gain),
            };
        }

        // Changing strike position selects modes; it must not also act as a
        // hidden master level. Preserve the energy of six equal 1/N gains.
        const double target_energy = 1.0 / static_cast<double>(mode_count);
        const double scale =
            raw_gain_energy > 0.0 ? std::sqrt(target_energy / raw_gain_energy) : 0.0;
        mode_gain_energy_ = 0.0;
        for (auto& mode : modes_) {
            mode.gain = static_cast<float>(static_cast<double>(mode.gain) * scale);
            mode_gain_energy_ +=
                static_cast<double>(mode.gain) * static_cast<double>(mode.gain);
        }
    }

    double tune_hz_ = 90.0;
    double structure_ = 1.0;
    double stretch_ = 0.0;
    double decay_ms_ = 700.0;
    double damping_ = 0.5;
    double brightness_ = 0.5;
    double position_ = 0.28;
    double spread_ = 0.1;
    double exciter_ms_ = 1.5;
    double exciter_cutoff_hz_ = 6000.0;
    MembraneExciter exciter_ = MembraneExciter::noise_burst;
    double sub_level_ = 0.0;
    double air_level_ = 0.0;
    double air_decay_ms_ = 20.0;
    double click_level_ = 0.0;
    double click_decay_ms_ = 2.0;

    NoiseSource noise_;
    NoiseSource air_noise_;
    NoiseSource click_noise_;
    ModalBank bank_;
    DecayEnvelope64 exciter_env_;
    DecayEnvelope64 gate_env_;
    DecayEnvelope64 sub_env_;
    DecayEnvelope64 air_env_;
    DecayEnvelope64 click_env_;
    TptFilter exciter_filter_;
    TptFilter air_filter_;
    TptFilter click_filter_;
    LowpassGate gate_;
    OutputStage output_;

    std::array<ModalMode, mode_count> modes_{};
    std::vector<float> scratch_;
    std::array<float, kBlock> body_{};

    double velocity_gain_ = 1.0;
    double mode_gain_energy_ = 0.0;
    double sub_phase_ = 0.0;
    double applied_fundamental_hz_ = 100.0;
    bool pluck_pending_ = false;
    double level_ = 0.0;
};

}  // namespace pulp::signal::drum
