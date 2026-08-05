#pragma once

// KELVIN — the synth voice the panel is a face for.
//
// The panel binds five macros: attack, release, cutoff, resonance, drive. Those
// map onto two processors Pulp already ships — `AdsrT` for the envelope and
// `AnalogVcfT`, which carries cutoff, resonance and drive together — so this
// file is voice allocation and an oscillator, not a filter reimplementation.
//
// A `Processor` with a fixed chain rather than a SignalGraph: polyphony is not
// runtime routing. Every voice is the same osc -> VCF -> VCA, decided here.

#include <pulp/signal/adsr.hpp>
#include <pulp/signal/analog_vcf.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace pulp::examples {

/// One voice: a saw through its own filter and envelope.
///
/// The filter is per-voice, not on the summed mix. On a resonant lowpass that
/// is the whole character — a shared filter makes a chord's resonance track
/// whatever the sum happens to be, so held notes audibly shift when a new one
/// arrives.
class KelvinVoice {
public:
    void prepare(double sample_rate, int max_block) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        env_.set_sample_rate(static_cast<float>(sample_rate_));
        vcf_.set_sample_rate(sample_rate_);
        scratch_.assign(static_cast<std::size_t>(std::max(max_block, 1)), 0.0f);
        reset();
    }

    void reset() {
        env_.reset();
        vcf_.reset();
        phase_ = 0.0f;
        note_ = -1;
        velocity_ = 0.0f;
    }

    void note_on(int note, float velocity) {
        note_ = note;
        velocity_ = std::clamp(velocity, 0.0f, 1.0f);
        // 440Hz at MIDI 69, the equal-tempered reference.
        const float hz = 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
        increment_ = hz / static_cast<float>(sample_rate_);
        env_.note_on();
    }

    void note_off() { env_.note_off(); }

    /// Sounding, or still releasing. A voice is only free once its envelope has
    /// finished — stealing it earlier truncates the release into a click.
    bool active() const { return env_.is_active(); }
    int note() const { return note_; }

    void set_envelope(float attack_seconds, float release_seconds) {
        signal::AdsrT<float>::Params p;
        p.attack = attack_seconds;
        p.release = release_seconds;
        // Held notes sustain at full level: the panel exposes attack and
        // release only, so a decay to a lower sustain would be a shape the
        // player has no control over and cannot turn off.
        p.decay = 0.0f;
        p.sustain = 1.0f;
        env_.set_params(p);
    }

    void set_filter(float cutoff01, float resonance01, float drive_db) {
        vcf_.set_cutoff(cutoff01);
        vcf_.set_resonance(resonance01);
        vcf_.set_drive_db(drive_db);
    }

    /// Adds this voice's output into `out`. Additive so the caller can mix
    /// every voice into one buffer without an intermediate sum.
    void add_to(float* out, int num_samples) {
        if (!active()) return;
        const int n = std::min(num_samples, static_cast<int>(scratch_.size()));

        for (int i = 0; i < n; ++i) {
            // Naive saw. Aliasing is real above a few kHz, and honest: this
            // example exists to prove the panel drives audible DSP, not to be
            // the oscillator someone ships. A BLEP would obscure that.
            const float saw = 2.0f * phase_ - 1.0f;
            phase_ += increment_;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
            scratch_[static_cast<std::size_t>(i)] = saw * velocity_;
        }

        // Filter before the envelope: the VCA must not be inside a resonant
        // feedback path, or a long release rings on after the note has gone.
        vcf_.process(scratch_.data(), n);

        for (int i = 0; i < n; ++i)
            out[i] += scratch_[static_cast<std::size_t>(i)] * env_.next();
    }

private:
    signal::AdsrT<float> env_;
    signal::AnalogVcfT<float> vcf_;
    std::vector<float> scratch_;
    double sample_rate_ = 48000.0;
    float phase_ = 0.0f;
    float increment_ = 0.0f;
    float velocity_ = 0.0f;
    int note_ = -1;
};

/// The instrument: eight voices, and the note routing between them.
class KelvinSynth {
public:
    static constexpr std::size_t kVoices = 8;

    void prepare(double sample_rate, int max_block) {
        for (auto& v : voices_) v.prepare(sample_rate, max_block);
        level_ = 0.0f;
    }

    void reset() {
        for (auto& v : voices_) v.reset();
        level_ = 0.0f;
    }

    void set_attack(float seconds) { attack_ = std::clamp(seconds, 0.001f, 4.0f); }
    void set_release(float seconds) { release_ = std::clamp(seconds, 0.005f, 8.0f); }
    void set_cutoff(float knob01) { cutoff_ = std::clamp(knob01, 0.0f, 1.0f); }
    void set_resonance(float knob01) { resonance_ = std::clamp(knob01, 0.0f, 1.0f); }
    /// 0..1 mapped to 0..24 dB into the filter's own drive stage.
    void set_drive(float knob01) { drive_db_ = std::clamp(knob01, 0.0f, 1.0f) * 24.0f; }

    void note_on(int note, float velocity) {
        // A note-on for a note already sounding retriggers that voice rather
        // than stacking a second one, or a held pedal doubles the level of
        // every repeated key.
        for (auto& v : voices_)
            if (v.active() && v.note() == note) { start(v, note, velocity); return; }
        for (auto& v : voices_)
            if (!v.active()) { start(v, note, velocity); return; }
        // All eight busy: steal the oldest. Voices are stolen from the front so
        // stealing is at least predictable; a real instrument would rank by
        // envelope stage.
        start(voices_[steal_ % kVoices], note, velocity);
        ++steal_;
    }

    void note_off(int note) {
        for (auto& v : voices_)
            if (v.active() && v.note() == note) v.note_off();
    }

    void all_notes_off() { for (auto& v : voices_) v.note_off(); }

    /// Renders into `out`, which the caller has already cleared.
    void render(float* out, int num_samples) {
        for (auto& v : voices_) {
            v.set_envelope(attack_, release_);
            v.set_filter(cutoff_, resonance_, drive_db_);
            v.add_to(out, num_samples);
        }
        // Output level for the panel's meter, measured from what was produced
        // rather than inferred from how many voices are notionally on.
        for (int i = 0; i < num_samples; ++i) {
            const float mag = std::fabs(out[i]);
            level_ += (mag > level_ ? 0.30f : 0.002f) * (mag - level_);
        }
    }

    float level() const { return level_; }

    /// Sounding voice count, for the panel's "voices lit" readout and for the
    /// tests, which need to distinguish "released" from "still ringing".
    std::size_t active_voices() const {
        std::size_t n = 0;
        for (const auto& v : voices_) if (v.active()) ++n;
        return n;
    }

private:
    void start(KelvinVoice& v, int note, float velocity) {
        v.set_envelope(attack_, release_);
        v.set_filter(cutoff_, resonance_, drive_db_);
        v.note_on(note, velocity);
    }

    std::array<KelvinVoice, kVoices> voices_{};
    float attack_ = 0.01f;
    float release_ = 0.30f;
    float cutoff_ = 0.6f;
    float resonance_ = 0.2f;
    float drive_db_ = 0.0f;
    float level_ = 0.0f;
    unsigned steal_ = 0;
};

}  // namespace pulp::examples
