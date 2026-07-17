#pragma once

// BowedString -- a polyphonic bowed-string instrument built on the MSW digital
// waveguide voice (bowed_string.hpp). One Processor, a fixed pool of voices,
// all allocated in prepare(). A note-on assigns a free (or stolen) voice, tunes
// it to the note, and starts bowing; note-off lifts the bow and the string
// rings down through the bridge loss. A bow is sustained, so a held note keeps
// speaking -- this is not a struck/one-shot instrument.
//
// Fixed one-in/one-out topology, internal polyphony: a Processor, not a graph.
//
// Uniform control surface (Level/Tune/Decay/Tone) plus the two physically-real
// bow controls (Force, Position). Note-on velocity sets bow speed; CC74 (the
// standard expression "slide") sweeps bow position on the sounding notes, the
// cheap MPE-lite gesture. The continuous, glitch-free retune the waveguide's
// stateless Lagrange fractional delay allows is what makes per-note pitch motion
// (vibrato / bend) clean -- proven directly in the tests.

#include "bowed_string.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::examples {

enum BowedStringParams {
    kBowLevel = 0,
    kBowTune = 1,
    kBowDecay = 2,
    kBowTone = 3,
    kBowForce = 4,
    kBowPosition = 5,
};

class BowedStringInstrument : public format::Processor {
public:
    static constexpr int kNumVoices = 12;
    static constexpr uint8_t kCcSlide = 74;  ///< standard MPE timbre / "slide"

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "BowedString",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.bowed-string",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Main Out", 2, false}},
            .accepts_midi = true,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kBowLevel, .name = "Level", .unit = "%",
                             .range = {0.0f, 100.0f, 80.0f, 0.1f}});
        store.add_parameter({.id = kBowTune, .name = "Tune", .unit = "st",
                             .range = {-12.0f, 12.0f, 0.0f, 0.01f}});
        store.add_parameter({.id = kBowDecay, .name = "Decay", .unit = "%",
                             .range = {0.0f, 100.0f, 55.0f, 0.1f}});
        store.add_parameter({.id = kBowTone, .name = "Tone", .unit = "%",
                             .range = {0.0f, 100.0f, 55.0f, 0.1f}});
        // The two physically-real bow controls.
        store.add_parameter({.id = kBowForce, .name = "Bow Force", .unit = "%",
                             .range = {0.0f, 100.0f, 40.0f, 0.1f}});
        store.add_parameter({.id = kBowPosition, .name = "Bow Position", .unit = "%",
                             .range = {0.0f, 100.0f, 22.0f, 0.1f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        prepared_block_ = std::max(ctx.max_buffer_size, 1);
        voices_.resize(kNumVoices);
        notes_.assign(kNumVoices, kNoNote);
        ages_.assign(kNumVoices, 0);
        for (auto& v : voices_) v.prepare(sample_rate_);
        mix_.assign(static_cast<std::size_t>(prepared_block_), 0.0f);
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        const int n_samples = static_cast<int>(output.num_samples());
        if (n_samples <= 0) return;

        if (ctx.should_reset_dsp_state()) {
            for (auto& v : voices_) v.reset();
            std::fill(notes_.begin(), notes_.end(), kNoNote);
        }

        // Control-rate parameter snapshot, applied to every voice this block.
        const float level = state().get_value(kBowLevel) / 100.0f;
        tune_semitones_ = state().get_value(kBowTune);
        const double decay = state().get_value(kBowDecay) / 100.0;
        const double tone = state().get_value(kBowTone) / 100.0;
        const double force = state().get_value(kBowForce) / 100.0;
        position_ = state().get_value(kBowPosition) / 100.0;
        // Tone% -> bridge damping: up = brighter (less HF loss).
        const double damping = kDampMax - (kDampMax - kDampMin) * tone;
        for (int i = 0; i < kNumVoices; ++i) {
            voices_[i].set_decay(decay);
            voices_[i].set_tone(damping);
            voices_[i].set_bow_force(force);
            if (notes_[i] != kNoNote) apply_voice_pitch_position(i);
        }

        const int block = std::min(n_samples, static_cast<int>(mix_.size()));
        std::fill(mix_.begin(), mix_.begin() + block, 0.0f);

        int cursor = 0;
        const int event_count = static_cast<int>(midi_in.size());
        for (int e = 0; e < event_count; ++e) {
            const auto& ev = midi_in[static_cast<std::size_t>(e)];
            const int offset = std::clamp(ev.sample_offset, cursor, block);
            render_segment(cursor, offset - cursor);
            cursor = offset;
            handle_event(ev);
        }
        render_segment(cursor, block - cursor);

        auto left = output.channel(0);
        const bool stereo = output.num_channels() > 1;
        auto right = output.channel(stereo ? 1 : 0);
        for (int i = 0; i < block; ++i) {
            const float s = mix_[static_cast<std::size_t>(i)] * level;
            left[static_cast<std::size_t>(i)] = s;
            if (stereo) right[static_cast<std::size_t>(i)] = s;
        }
        for (int i = block; i < n_samples; ++i) {
            left[static_cast<std::size_t>(i)] = 0.0f;
            if (stereo) right[static_cast<std::size_t>(i)] = 0.0f;
        }

        for (auto& v : voices_) v.snap_denormals();
    }

    /// @internal Test hook: voices currently sounding.
    int active_voice_count() const {
        int n = 0;
        for (const auto& v : voices_) n += v.is_silent() ? 0 : 1;
        return n;
    }

private:
    static constexpr uint8_t kNoNote = 255;
    static constexpr double kDampMin = 0.08;  // Tone 100% -> bright
    static constexpr double kDampMax = 0.55;  // Tone 0%   -> dark

    void handle_event(const midi::MidiEvent& ev) {
        if (ev.is_note_on() && ev.velocity() > 0) {
            note_on(ev.note(), ev.velocity());
        } else if (ev.is_note_off()) {
            note_off(ev.note());
        } else if (ev.is_cc() && ev.cc_number() == kCcSlide) {
            // MPE-lite: slide sweeps bow position toward the bridge as it rises.
            slide_ = ev.cc_value() / 127.0;
            for (int i = 0; i < kNumVoices; ++i)
                if (notes_[i] != kNoNote) apply_voice_pitch_position(i);
        }
    }

    void note_on(uint8_t note, uint8_t velocity) {
        const int idx = pick_voice();
        notes_[idx] = note;
        ages_[idx] = next_age_++;
        apply_voice_pitch_position(idx);
        // Velocity -> bow speed: harder = faster bow = louder, quicker onset.
        const double speed = kMinBowSpeed +
            (kMaxBowSpeed - kMinBowSpeed) * (velocity / 127.0);
        voices_[idx].set_bow_velocity(speed);
        voices_[idx].bow_on();
    }

    void note_off(uint8_t note) {
        for (int i = 0; i < kNumVoices; ++i)
            if (notes_[i] == note) {
                voices_[i].bow_off();
                notes_[i] = kNoNote;  // free for stealing; it rings down on its own
            }
    }

    void apply_voice_pitch_position(int idx) {
        const uint8_t note = notes_[idx] == kNoNote ? last_note_[idx] : notes_[idx];
        last_note_[idx] = note;
        const double f0 = 440.0 *
            std::pow(2.0, (static_cast<double>(note) - 69.0 + tune_semitones_) / 12.0);
        voices_[idx].set_frequency(f0);
        // Bow position knob and the slide gesture combine; slide biases toward
        // the bridge (brighter). beta in [0.02, 0.5].
        double pos = std::clamp(position_ - 0.35 * slide_, 0.0, 1.0);
        double beta = kBetaMin + (kBetaMax - kBetaMin) * pos;
        voices_[idx].set_bow_position(beta);
    }

    /// Prefer a free (silent) voice; else steal the oldest.
    int pick_voice() {
        int oldest = 0;
        for (int i = 0; i < kNumVoices; ++i) {
            if (notes_[i] == kNoNote && voices_[i].is_silent()) return i;
            if (ages_[i] < ages_[oldest]) oldest = i;
        }
        return oldest;
    }

    void render_segment(int offset, int count) {
        if (count <= 0) return;
        float* out = mix_.data() + offset;
        for (auto& v : voices_) {
            if (v.is_silent()) continue;
            for (int i = 0; i < count; ++i) out[i] += static_cast<float>(v.process());
        }
    }

    static constexpr double kBetaMin = 0.02;
    static constexpr double kBetaMax = 0.5;
    static constexpr double kMinBowSpeed = 0.05;
    static constexpr double kMaxBowSpeed = 0.16;

    std::vector<BowedString> voices_;
    std::vector<uint8_t> notes_;      ///< current note per voice, or kNoNote
    std::vector<uint64_t> ages_;
    uint8_t last_note_[kNumVoices] = {};  ///< last note a voice held (for retune of a releasing tail)
    std::vector<float> mix_;

    double sample_rate_ = 48000.0;
    double tune_semitones_ = 0.0;
    double position_ = 0.22;
    double slide_ = 0.0;
    int prepared_block_ = 512;
    uint64_t next_age_ = 1;
};

inline std::unique_ptr<format::Processor> create_bowed_string() {
    return std::make_unique<BowedStringInstrument>();
}

}  // namespace pulp::examples
