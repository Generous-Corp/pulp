#pragma once

#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/drum/fm_tables.hpp>
#include <pulp/signal/drum/layers.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/fm_operator_engine.hpp>
#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/svf.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pulp::signal::drum {

/// A two-operator FM percussion voice.
///
/// Frequency modulation earns its place in a drum kit for one reason: the
/// sidebands it produces are spaced by the modulator's frequency, so setting a
/// non-integer ratio puts partials at frequencies that belong to no harmonic
/// series. That is inharmonicity generated rather than filtered, and it is why
/// two operators can produce a bell, a clank or a metallic tom that a filtered
/// oscillator cannot reach at any setting.
///
/// The **ratio** decides where the partials land — integer ratios stay
/// harmonic, ratios near a simple fraction beat, and irrational-ish ratios
/// clang. The **index** decides how many there are, and because it is enveloped
/// separately from amplitude the spectrum collapses while the note is still
/// sounding. The shared 26-wave table and per-operator warp envelopes broaden
/// the strike vocabulary without changing those FM fundamentals.
///
/// The modulator feeds back into itself through a one-sample delay. As that
/// feedback rises the modulator's own waveform moves continuously from a sine
/// toward a saw and then into noise, which is a cheap and controllable way to
/// reach a noise-like strike without a separate noise source.
///
/// Lineage: Chowning, "The Synthesis of Complex Audio Spectra by Means of
/// Frequency Modulation", JAES 21(7), 1973. Implemented from the published
/// technique.
///
/// RT contract: `prepare()` allocates nothing; every other method, including
/// `note_on` and `process`, allocates nothing and takes no locks.
class FmDrumVoice : public Voice {
public:
    FmDrumVoice() {
        VelocityResponse r;
        r.level_db = 15.0f;
        r.bend_octaves = 0.1f;
        // Velocity opening the index is the whole point: a harder hit is a
        // brighter, more complex spectrum, not the same spectrum turned up.
        r.brightness_octaves = 1.0f;
        set_velocity_response(r);

        click_.set_level(0.2);
        click_.set_cutoff_hz(6000.0);
        click_.set_decay_ms(1.5);
    }

    /// Carrier frequency, in Hz.
    void set_tune_hz(double hz) { tune_hz_ = std::clamp(hz, 20.0, 4000.0); }

    /// Modulator frequency as a ratio of the carrier. Integer ratios keep the
    /// result harmonic; everything else is where the metal lives.
    void set_ratio(double ratio) { ratio_ = std::clamp(ratio, 0.1, 24.0); }

    /// Modulation index at the strike, and how fast it collapses. The index
    /// envelope is deliberately separate from the amplitude envelope.
    void set_index(double index) { index_ = std::clamp(index, 0.0, 24.0); }
    void set_index_ms(double ms) { index_ms_ = std::clamp(ms, 0.5, 2000.0); }

    /// How long the note lasts, as a T60.
    void set_decay_ms(double ms) { decay_ms_ = std::clamp(ms, 10.0, 4000.0); }

    /// Pitch envelope depth in octaves and its time constant.
    void set_pitch_sweep_octaves(double octaves) {
        pitch_sweep_oct_ = std::clamp(octaves, 0.0, 6.0);
    }
    void set_pitch_sweep_ms(double ms) { pitch_sweep_ms_ = std::clamp(ms, 0.5, 500.0); }

    /// Modulator self-feedback, 0 to 1. Moves the modulator from a sine toward
    /// a saw and then into noise.
    void set_feedback(double amount) { feedback_ = std::clamp(amount, 0.0, 1.0); }

    /// The 26 shared phase-readable waves and each operator's decaying
    /// Casio-family phase warp.
    void set_carrier_wave(int wave) {
        carrier_wave_ = std::clamp(wave, 0, FmWaveTable::wave_count - 1);
    }
    void set_modulator_wave(int wave) {
        modulator_wave_ = std::clamp(wave, 0, FmWaveTable::wave_count - 1);
    }
    void set_carrier_warp(double amount) {
        carrier_warp_ = std::clamp(amount, 0.0, 1.0);
    }
    void set_modulator_warp(double amount) {
        modulator_warp_ = std::clamp(amount, 0.0, 1.0);
    }
    void set_carrier_warp_ms(double ms) {
        carrier_warp_ms_ = std::clamp(ms, 0.5, 2000.0);
    }
    void set_modulator_warp_ms(double ms) {
        modulator_warp_ms_ = std::clamp(ms, 0.5, 2000.0);
    }

    /// Delayed, faded pitch LFO. Delay and fade are independent so vibrato can
    /// enter after the strike without stepping on.
    void set_lfo_rate_hz(double hz) { lfo_rate_hz_ = std::clamp(hz, 0.05, 40.0); }
    void set_lfo_depth_octaves(double octaves) {
        lfo_depth_octaves_ = std::clamp(octaves, 0.0, 2.0);
    }
    void set_lfo_delay_ms(double ms) { lfo_delay_ms_ = std::clamp(ms, 0.0, 4000.0); }
    void set_lfo_fade_ms(double ms) { lfo_fade_ms_ = std::clamp(ms, 0.0, 4000.0); }

    /// Reset the modulator phase whenever the carrier wraps.
    void set_hard_sync(bool sync) { hard_sync_ = sync; }

    /// Select one of the shared 24 procedural strike recipes. -1 leaves the
    /// manually configured click/noise controls in place.
    void set_transient(int index) {
        transient_ = std::clamp(index, -1,
                                static_cast<int>(kFmTransients.size()) - 1);
        if (transient_ >= 0) apply_transient();
    }
    void set_noise_level(double level) {
        transient_ = -1;
        noise_level_ = std::max(level, 0.0);
    }
    void set_noise_decay_ms(double ms) {
        transient_ = -1;
        noise_decay_ms_ = std::clamp(ms, 0.5, 500.0);
    }
    void set_noise_color(NoiseColor color) {
        transient_ = -1;
        noise_color_ = color;
        if (noise_.color() != color) noise_.set_color(color);
    }

    /// A bandpass on the output, for picking one formant out of the sidebands.
    void set_cutoff_hz(double hz) { cutoff_hz_ = std::clamp(hz, 40.0, 18000.0); }
    void set_resonance(double q) { resonance_ = std::clamp(q, 0.5, 12.0); }
    void set_bandpass(bool bandpass) { bandpass_ = bandpass; }

    void set_click_level(double level) {
        transient_ = -1;
        click_.set_level(level);
    }
    void set_click_cutoff_hz(double hz) {
        transient_ = -1;
        click_.set_cutoff_hz(hz);
    }

    OutputStage& output() { return output_; }
    const OutputStage& output() const { return output_; }
    OutputStage* output_stage() noexcept override { return &output_; }
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
        index_env_.set_sample_rate(sample_rate);
        pitch_env_.set_sample_rate(sample_rate);
        carrier_warp_env_.set_sample_rate(sample_rate);
        modulator_warp_env_.set_sample_rate(sample_rate);
        noise_env_.set_sample_rate(sample_rate);
        click_.prepare(sample_rate);
        filter_.set_sample_rate(static_cast<float>(sample_rate));
        output_.prepare(sample_rate);
    }

    void on_reset() override {
        amp_env_.reset();
        index_env_.reset();
        pitch_env_.reset();
        carrier_warp_env_.reset();
        modulator_warp_env_.reset();
        noise_env_.reset();
        click_.reset();
        filter_.reset();
        output_.reset();
        noise_.reset();
        carrier_phase_ = 0.0;
        modulator_phase_ = 0.0;
        modulator_z_ = 0.0;
        lfo_phase_ = 0.0;
        samples_since_note_ = 0;
    }

    void on_note_on(float velocity) override {
        output_.reset_nonlinear_state();
        output_.trigger();
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);
        applied_bend_ = pitch_sweep_oct_ + response.bend(velocity);
        applied_index_ =
            index_ * static_cast<double>(response.brightness_scale(velocity));

        noise_.reset();
        carrier_phase_ = 0.0;
        modulator_phase_ = 0.0;
        modulator_z_ = 0.0;
        lfo_phase_ = 0.0;
        samples_since_note_ = 0;
        lfo_delay_samples_ =
            static_cast<int>(0.001 * lfo_delay_ms_ * sample_rate());
        lfo_fade_samples_ =
            std::max(1, static_cast<int>(0.001 * lfo_fade_ms_ * sample_rate()));

        filter_.set_mode(bandpass_ ? Svf::Mode::bandpass : Svf::Mode::lowpass);
        filter_.set_frequency(
            static_cast<float>(std::min(cutoff_hz_, 0.49 * sample_rate())));
        filter_.set_resonance(static_cast<float>(resonance_));

        pitch_env_.set_attack_ms(0.0);
        pitch_env_.set_decay_time_constant_ms(pitch_sweep_ms_);
        pitch_env_.trigger();

        index_env_.set_attack_ms(0.0);
        index_env_.set_decay_time_constant_ms(index_ms_);
        index_env_.trigger();

        carrier_warp_env_.set_attack_ms(0.0);
        carrier_warp_env_.set_decay_time_constant_ms(carrier_warp_ms_);
        carrier_warp_env_.trigger();
        modulator_warp_env_.set_attack_ms(0.0);
        modulator_warp_env_.set_decay_time_constant_ms(modulator_warp_ms_);
        modulator_warp_env_.trigger();

        amp_env_.set_attack_ms(0.3);
        amp_env_.set_decay_t60_ms(decay_ms_);
        amp_env_.trigger();

        noise_env_.set_attack_ms(0.0);
        noise_env_.set_decay_time_constant_ms(noise_decay_ms_);
        if (noise_level_ > 0.0) noise_env_.trigger();
        click_.trigger();
    }

    bool on_is_active() const override {
        return amp_env_.is_active() || noise_env_.is_active() ||
               click_.is_active() || output_.has_tail();
    }

    void render_add(float* out, int num_samples) override {
        for (int i = 0; i < num_samples; ++i) {
            const double pitch = pitch_env_.process();
            const double index = applied_index_ * index_env_.process();
            const double amplitude = amp_env_.process();
            const double carrier_warp =
                carrier_warp_ * carrier_warp_env_.process();
            const double modulator_warp =
                modulator_warp_ * modulator_warp_env_.process();

            double lfo = 0.0;
            if (lfo_depth_octaves_ > 0.0 &&
                samples_since_note_ >= lfo_delay_samples_) {
                const double fade = std::min(
                    static_cast<double>(samples_since_note_ - lfo_delay_samples_) /
                        static_cast<double>(lfo_fade_samples_),
                    1.0);
                lfo = lfo_depth_octaves_ * fade *
                      std::sin(2.0 * 3.14159265358979323846 * lfo_phase_);
            }
            lfo_phase_ += lfo_rate_hz_ / sample_rate();
            if (lfo_phase_ >= 1.0) lfo_phase_ -= std::floor(lfo_phase_);
            ++samples_since_note_;

            const double carrier_hz =
                std::min(tune_hz_ * std::exp2(applied_bend_ * pitch + lfo),
                         0.49 * sample_rate());
            const double modulator_hz = std::min(carrier_hz * ratio_, 0.49 * sample_rate());

            carrier_phase_ += carrier_hz / sample_rate();
            const bool carrier_wrapped = carrier_phase_ >= 1.0;
            if (carrier_wrapped) carrier_phase_ -= std::floor(carrier_phase_);

            if (hard_sync_ && carrier_wrapped) {
                modulator_phase_ = 0.0;
            } else {
                modulator_phase_ += modulator_hz / sample_rate();
            }
            if (modulator_phase_ >= 1.0) modulator_phase_ -= std::floor(modulator_phase_);
            // The modulator reads its own previous output. One sample of delay
            // is what makes the loop computable at all; without it the operator
            // would need its own output to produce its output.
            const double modulator_phase =
                FmWaveTable::warp(
                    modulator_phase_ +
                        feedback_ * kFeedbackDepth * modulator_z_ /
                            (2.0 * 3.14159265358979323846),
                    modulator_warp);
            const double modulator =
                FmWaveTable::read(modulator_wave_, modulator_phase,
                                  modulator_hz / sample_rate());
            modulator_z_ = modulator;

            const double carrier_phase =
                FmWaveTable::warp(
                    carrier_phase_ + index * modulator /
                                         (2.0 * 3.14159265358979323846),
                    carrier_warp);
            const double carrier =
                FmWaveTable::read(carrier_wave_, carrier_phase,
                                  carrier_hz / sample_rate());

            const double body = filter_.process(static_cast<float>(carrier)) * amplitude;
            const double strike = click_.process(static_cast<double>(noise_.white()));
            const double noise_envelope =
                noise_env_.is_active() ? noise_env_.process() : 0.0;
            const double noise =
                noise_level_ > 0.0
                    ? static_cast<double>(noise_.process()) *
                          noise_envelope * noise_level_
                    : 0.0;

            out[i] += static_cast<float>(
                output_.process(static_cast<float>(body + strike + noise)) *
                velocity_gain_);
        }
    }

private:
    void apply_transient() {
        const auto& recipe =
            kFmTransients[static_cast<std::size_t>(transient_)];
        noise_color_ = recipe.color;
        noise_level_ = recipe.noise_level;
        noise_decay_ms_ = recipe.noise_decay_ms;
        click_.set_level(recipe.click_level);
        click_.set_cutoff_hz(recipe.click_cutoff_hz);
        click_.set_decay_ms(recipe.click_decay_ms);
        if (noise_.color() != noise_color_)
            noise_.set_color(noise_color_);
    }

    // How far full feedback drives the modulator. Beyond roughly this the
    // operator is already producing noise, so more range would only add
    // settings that all sound the same.
    static constexpr double kFeedbackDepth = 6.0;

    double tune_hz_ = 110.0;
    double ratio_ = 1.41;
    double index_ = 6.0;
    double index_ms_ = 40.0;
    double decay_ms_ = 400.0;
    double pitch_sweep_oct_ = 0.0;
    double pitch_sweep_ms_ = 30.0;
    double feedback_ = 0.0;
    double cutoff_hz_ = 12000.0;
    double resonance_ = 0.8;
    bool bandpass_ = false;
    int carrier_wave_ = 0;
    int modulator_wave_ = 0;
    double carrier_warp_ = 0.0;
    double modulator_warp_ = 0.0;
    double carrier_warp_ms_ = 40.0;
    double modulator_warp_ms_ = 40.0;
    double lfo_rate_hz_ = 5.0;
    double lfo_depth_octaves_ = 0.0;
    double lfo_delay_ms_ = 0.0;
    double lfo_fade_ms_ = 0.0;
    bool hard_sync_ = false;
    int transient_ = -1;
    NoiseColor noise_color_ = NoiseColor::white;
    double noise_level_ = 0.0;
    double noise_decay_ms_ = 10.0;

    NoiseSource noise_;
    DecayEnvelope64 amp_env_;
    DecayEnvelope64 index_env_;
    DecayEnvelope64 pitch_env_;
    DecayEnvelope64 carrier_warp_env_;
    DecayEnvelope64 modulator_warp_env_;
    DecayEnvelope64 noise_env_;
    ClickLayer click_;
    Svf filter_;
    OutputStage output_;

    double velocity_gain_ = 1.0;
    double applied_bend_ = 0.0;
    double applied_index_ = 6.0;
    double carrier_phase_ = 0.0;
    double modulator_phase_ = 0.0;
    double modulator_z_ = 0.0;
    double lfo_phase_ = 0.0;
    int samples_since_note_ = 0;
    int lfo_delay_samples_ = 0;
    int lfo_fade_samples_ = 1;
};

/// An eight-operator FM percussion voice with a fixed algorithm table.
///
/// Two operators reach bells and clanks; eight reach the dense, evolving
/// spectra that a single modulator cannot, because each operator can modulate
/// several others and the resulting sidebands modulate each other in turn.
///
/// **Every operator reads every modulator's output one sample late.** That
/// single decision is what makes an arbitrary routing matrix computable: with
/// delayed reads there is no ordering constraint at all, so operators can be
/// evaluated in any order and an algorithm may contain cycles that a
/// topological sort would reject outright. The cost is one sample of phase
/// error per modulation hop, which at audio rates is far below what the
/// modulation itself is doing. It is applied uniformly rather than only to
/// paths that would otherwise be circular, so the routing table needs no
/// analysis and adding an algorithm cannot introduce an ordering bug.
///
/// An algorithm is a bitmask per operator saying which others feed it, plus a
/// flag saying whether it reaches the output. The sixteen below are chosen to
/// span the useful shapes — deep serial stacks for clangorous metal, wide
/// parallel banks for additive-like bodies, and mixed trees for everything
/// between — rather than to reproduce any particular instrument's set.
/// Each operator independently selects one of the same 26 clean harmonic-table
/// waves as FM2, and the shared procedural transient table can add a tinted
/// noise/contact strike without routing a ninth operator.
///
/// Lineage: Chowning, JAES 21(7), 1973, as for the two-operator voice.
///
/// RT contract: `prepare()` allocates nothing; every other method allocates
/// nothing and takes no locks.
class Fm8DrumVoice : public Voice {
public:
    static constexpr int operator_count = 8;
    static constexpr int algorithm_count = 16;

    /// One routing: for each operator, a bitmask of the operators that
    /// modulate it, and whether it is heard directly.
    struct Algorithm {
        std::uint8_t modulated_by[operator_count];
        std::uint8_t carriers;  ///< bit per operator
    };

    /// The sixteen routings, ordered roughly from most parallel to most
    /// serial: 0 is eight independent carriers (additive), 15 is a single
    /// eight-deep stack (the most extreme spectrum the voice can make).
    static constexpr Algorithm algorithms[algorithm_count] = {
        {{0, 0, 0, 0, 0, 0, 0, 0}, 0xFF},                       //  0 additive
        {{0x02, 0, 0, 0, 0, 0, 0, 0}, 0xFD},                    //  1 one pair
        {{0x02, 0, 0x08, 0, 0, 0, 0, 0}, 0xF5},                 //  2 two pairs
        {{0x02, 0, 0x08, 0, 0x20, 0, 0, 0}, 0xD5},              //  3 three pairs
        {{0x02, 0, 0x08, 0, 0x20, 0, 0x80, 0}, 0x55},           //  4 four pairs
        {{0x06, 0, 0, 0, 0, 0, 0, 0}, 0xF9},                    //  5 two into one
        {{0x0E, 0, 0, 0, 0, 0, 0, 0}, 0xF1},                    //  6 three into one
        {{0x1E, 0, 0, 0, 0, 0, 0, 0}, 0xE1},                    //  7 four into one
        {{0x02, 0x04, 0, 0, 0, 0, 0, 0}, 0xF9},                 //  8 three-deep stack
        {{0x02, 0x04, 0x08, 0, 0, 0, 0, 0}, 0xF1},              //  9 four-deep stack
        {{0x02, 0x04, 0, 0x20, 0, 0, 0, 0}, 0xD9},              // 10 stack plus pair
        {{0x06, 0x18, 0, 0, 0, 0, 0, 0}, 0xE1},                 // 11 tree
        {{0x02, 0x0C, 0x10, 0, 0, 0, 0, 0}, 0xE1},              // 12 branching stack
        {{0x02, 0x04, 0x08, 0x10, 0, 0, 0, 0}, 0xE1},           // 13 five-deep stack
        {{0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0, 0}, 0x81},     // 14 seven-deep stack
        {{0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0}, 0x01},  // 15 eight-deep stack
    };

    Fm8DrumVoice() {
        VelocityResponse r;
        r.level_db = 15.0f;
        r.brightness_octaves = 1.0f;
        set_velocity_response(r);
    }

    void set_algorithm(int index) {
        algorithm_ = std::clamp(index, 0, algorithm_count - 1);
    }
    int algorithm() const { return algorithm_; }

    void set_tune_hz(double hz) { tune_hz_ = std::clamp(hz, 20.0, 4000.0); }

    /// Frequency ratio of one operator to the voice's tuning.
    void set_operator_ratio(int op, double ratio) {
        if (op < 0 || op >= operator_count) return;
        ratios_[static_cast<std::size_t>(op)] = std::clamp(ratio, 0.1, 32.0);
    }

    /// Output level of one operator, before the algorithm's routing.
    void set_operator_level(int op, double level) {
        if (op < 0 || op >= operator_count) return;
        levels_[static_cast<std::size_t>(op)] = std::clamp(level, 0.0, 1.0);
    }

    /// Decay of one operator's own envelope, as a T60. Operators decaying at
    /// different rates is what makes the spectrum evolve rather than simply
    /// fade -- a modulator that dies first takes its sidebands with it.
    void set_operator_decay_ms(int op, double ms) {
        if (op < 0 || op >= operator_count) return;
        decays_[static_cast<std::size_t>(op)] = std::clamp(ms, 1.0, 4000.0);
    }

    /// Self-feedback on one operator.
    void set_operator_feedback(int op, double amount) {
        if (op < 0 || op >= operator_count) return;
        feedbacks_[static_cast<std::size_t>(op)] = std::clamp(amount, 0.0, 1.0);
    }

    void set_operator_wave(int op, int wave) {
        if (op < 0 || op >= operator_count) return;
        waves_[static_cast<std::size_t>(op)] =
            std::clamp(wave, 0, FmWaveTable::wave_count - 1);
    }

    /// Overall modulation depth, scaling every routed connection at once.
    void set_depth(double depth) { depth_ = std::clamp(depth, 0.0, 12.0); }

    /// A formant bandpass on the summed carriers.
    void set_formant_hz(double hz) { formant_hz_ = std::clamp(hz, 40.0, 18000.0); }
    void set_formant_q(double q) { formant_q_ = std::clamp(q, 0.5, 12.0); }

    void set_transient(int index) {
        transient_ = std::clamp(index, -1,
                                static_cast<int>(kFmTransients.size()) - 1);
        if (transient_ >= 0) apply_transient();
    }
    void set_noise_level(double level) {
        transient_ = -1;
        noise_level_ = std::max(level, 0.0);
    }
    void set_noise_decay_ms(double ms) {
        transient_ = -1;
        noise_decay_ms_ = std::clamp(ms, 0.5, 500.0);
    }
    void set_noise_color(NoiseColor color) {
        transient_ = -1;
        noise_color_ = color;
        if (noise_.color() != color) noise_.set_color(color);
    }
    void set_click_level(double level) {
        transient_ = -1;
        click_.set_level(level);
    }

    OutputStage& output() { return output_; }
    const OutputStage& output() const { return output_; }
    OutputStage* output_stage() noexcept override { return &output_; }
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
        for (auto& env : envelopes_) env.set_sample_rate(sample_rate);
        noise_env_.set_sample_rate(sample_rate);
        click_.prepare(sample_rate);
        formant_.set_sample_rate(static_cast<float>(sample_rate));
        formant_.set_mode(Svf::Mode::bandpass);
        output_.prepare(sample_rate);
    }

    void on_reset() override {
        for (auto& env : envelopes_) env.reset();
        noise_env_.reset();
        click_.reset();
        noise_.reset();
        phases_.fill(0.0);
        previous_.fill(0.0);
        noise_.reset();
        formant_.reset();
        output_.reset();
    }

    void on_note_on(float velocity) override {
        output_.reset_nonlinear_state();
        output_.trigger();
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);
        applied_depth_ =
            depth_ * static_cast<double>(response.brightness_scale(velocity));

        noise_.reset();
        phases_.fill(0.0);
        previous_.fill(0.0);
        formant_.set_frequency(
            static_cast<float>(std::min(formant_hz_, 0.49 * sample_rate())));
        formant_.set_resonance(static_cast<float>(formant_q_));

        for (std::size_t op = 0; op < operator_count; ++op) {
            envelopes_[op].set_attack_ms(0.2);
            envelopes_[op].set_decay_t60_ms(decays_[op]);
            envelopes_[op].trigger();
        }

        noise_env_.set_attack_ms(0.0);
        noise_env_.set_decay_time_constant_ms(noise_decay_ms_);
        if (noise_level_ > 0.0) noise_env_.trigger();
        click_.trigger();
    }

    bool on_is_active() const override {
        for (const auto& env : envelopes_) {
            if (env.is_active()) return true;
        }
        return noise_env_.is_active() || click_.is_active() ||
               output_.has_tail();
    }

    void render_add(float* out, int num_samples) override {
        const Algorithm& alg = algorithms[static_cast<std::size_t>(algorithm_)];

        for (int i = 0; i < num_samples; ++i) {
            std::array<double, operator_count> amplitudes{};
            std::array<double, operator_count> feedback{};
            for (std::size_t op = 0; op < operator_count; ++op) {
                amplitudes[op] = envelopes_[op].process() * levels_[op];
                feedback[op] = feedbacks_[op] * kFeedbackDepth;
            }

            const auto frequency = [&](std::size_t op) {
                return tune_hz_ * ratios_[op];
            };
            const auto phase_route = [&](std::size_t destination,
                                         std::size_t source) {
                return (alg.modulated_by[destination] & (1u << source))
                           ? applied_depth_
                           : 0.0;
            };
            const auto no_frequency_route = [](std::size_t, std::size_t) {
                return 0.0;
            };
            const auto carrier = [&](std::size_t op) {
                return (alg.carriers & (1u << op)) ? 1.0 : 0.0;
            };
            const auto wave = [&](std::size_t op, double phase_cycles,
                                  double phase_offset_radians,
                                  double phase_increment) {
                return FmWaveTable::read(
                    waves_[op],
                    phase_cycles +
                        phase_offset_radians /
                            (2.0 * 3.14159265358979323846),
                    phase_increment);
            };
            const double summed = detail::render_fm_operator_sample(
                static_cast<std::size_t>(operator_count), sample_rate(),
                FmOperatorAliasPolicy::bounded, phases_, previous_, amplitudes,
                feedback, frequency, phase_route, no_frequency_route, carrier,
                wave);

            const double shaped = formant_.process(static_cast<float>(summed));
            const double noise_envelope =
                noise_env_.is_active() ? noise_env_.process() : 0.0;
            const double noise =
                noise_level_ > 0.0
                    ? static_cast<double>(noise_.process()) *
                          noise_envelope * noise_level_
                    : 0.0;
            const double click =
                click_.process(static_cast<double>(noise_.white()));
            out[i] += static_cast<float>(
                output_.process(static_cast<float>(shaped + noise + click)) *
                velocity_gain_);
        }
    }

private:
    void apply_transient() {
        const auto& recipe =
            kFmTransients[static_cast<std::size_t>(transient_)];
        noise_color_ = recipe.color;
        noise_level_ = recipe.noise_level;
        noise_decay_ms_ = recipe.noise_decay_ms;
        click_.set_level(recipe.click_level);
        click_.set_cutoff_hz(recipe.click_cutoff_hz);
        click_.set_decay_ms(recipe.click_decay_ms);
        if (noise_.color() != noise_color_)
            noise_.set_color(noise_color_);
    }

    static constexpr double kFeedbackDepth = 6.0;

    int algorithm_ = 9;
    double tune_hz_ = 110.0;
    double depth_ = 3.0;
    double formant_hz_ = 2000.0;
    double formant_q_ = 1.0;
    int transient_ = -1;
    NoiseColor noise_color_ = NoiseColor::white;
    double noise_level_ = 0.0;
    double noise_decay_ms_ = 10.0;

    std::array<double, operator_count> ratios_ = {1.0, 1.41, 2.0, 3.17,
                                                   4.0, 5.63, 7.0, 9.41};
    std::array<double, operator_count> levels_ = {1.0, 0.8, 0.7, 0.6,
                                                   0.5, 0.4, 0.35, 0.3};
    std::array<double, operator_count> decays_ = {400.0, 220.0, 160.0, 110.0,
                                                   80.0,  60.0,  45.0,  30.0};
    std::array<double, operator_count> feedbacks_{};
    std::array<int, operator_count> waves_{};

    NoiseSource noise_;
    std::array<DecayEnvelope64, operator_count> envelopes_;
    DecayEnvelope64 noise_env_;
    ClickLayer click_;
    std::array<double, operator_count> phases_{};
    std::array<double, operator_count> previous_{};
    Svf formant_;
    OutputStage output_;

    double velocity_gain_ = 1.0;
    double applied_depth_ = 3.0;
};

}  // namespace pulp::signal::drum
