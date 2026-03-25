#pragma once

// PulpDrums — generative drum sequencer MIDI effect
// Validates: MIDI output, pattern sequencing, parameter system
// Phase 6 validation example from the roadmap

#include <pulp/format/processor.hpp>
#include <array>
#include <cmath>

namespace pulp::examples {

enum DrumParams : state::ParamID {
    kTempo     = 200,  // BPM (60-240)
    kSwing     = 201,  // Swing amount (0-1)
    kDensity   = 202,  // Note density (0-1)
    kVelocity  = 203,  // Base velocity (0-127)
    kPattern   = 204,  // Pattern select (0-3)
    kRandomize = 205,  // Randomization amount (0-1)
};

// Simple 16-step drum sequencer that outputs MIDI
class PulpDrumsProcessor : public format::Processor {
public:
    static constexpr int kSteps = 16;
    static constexpr int kTracks = 4; // Kick, Snare, HiHat, Clap

    // MIDI note numbers for GM drum map
    static constexpr uint8_t kKick = 36;
    static constexpr uint8_t kSnare = 38;
    static constexpr uint8_t kHiHat = 42;
    static constexpr uint8_t kClap = 39;

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpDrums",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.drums",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect, // MIDI effect
            .input_buses = {{"Audio In", 2}},  // Pass-through
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = true,
            .tail_samples = 0,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({kTempo, "Tempo", "BPM", {60, 240, 120, 1}});
        store.add_parameter({kSwing, "Swing", "", {0, 1, 0, 0.01f}});
        store.add_parameter({kDensity, "Density", "", {0, 1, 0.5f, 0.01f}});
        store.add_parameter({kVelocity, "Velocity", "", {1, 127, 100, 1}});
        store.add_parameter({kPattern, "Pattern", "", {0, 3, 0, 1}});
        store.add_parameter({kRandomize, "Randomize", "", {0, 1, 0, 0.01f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        step_position_ = 0;
        samples_since_step_ = 0;
        init_patterns();
    }

    void process(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>& input,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer& midi_out,
        const format::ProcessContext&) override
    {
        // Pass audio through unchanged
        for (std::size_t ch = 0; ch < output.num_channels() && ch < input.num_channels(); ++ch) {
            for (std::size_t i = 0; i < output.num_samples(); ++i) {
                output.channel(ch)[i] = input.channel(ch)[i];
            }
        }

        // Read parameters
        float tempo = state().get_value(kTempo);
        float swing = state().get_value(kSwing);
        float density = state().get_value(kDensity);
        int velocity = static_cast<int>(state().get_value(kVelocity));
        int pattern = static_cast<int>(state().get_value(kPattern));
        float randomize = state().get_value(kRandomize);

        // Samples per 16th note
        double samples_per_step = (sample_rate_ * 60.0) / (tempo * 4.0);

        // Apply swing to even steps
        double swing_offset = 0;
        if (step_position_ % 2 == 1) {
            swing_offset = samples_per_step * swing * 0.5;
        }

        double effective_step_length = samples_per_step + swing_offset;

        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            if (samples_since_step_ >= effective_step_length) {
                // Trigger step
                generate_step(midi_out, static_cast<int32_t>(i),
                             pattern, density, velocity, randomize);

                step_position_ = (step_position_ + 1) % kSteps;
                samples_since_step_ = 0;

                // Recalculate swing for new step
                if (step_position_ % 2 == 1) {
                    swing_offset = samples_per_step * swing * 0.5;
                } else {
                    swing_offset = 0;
                }
                effective_step_length = samples_per_step + swing_offset;
            }
            ++samples_since_step_;
        }

        // Pass through incoming MIDI
        for (const auto& evt : midi_in) {
            midi_out.add(evt);
        }
    }

private:
    double sample_rate_ = 48000.0;
    int step_position_ = 0;
    double samples_since_step_ = 0;

    // 4 patterns x 4 tracks x 16 steps
    std::array<std::array<std::array<bool, kSteps>, kTracks>, 4> patterns_{};

    // Simple PRNG for randomization
    uint32_t rng_state_ = 12345;

    float next_random() {
        rng_state_ = rng_state_ * 1103515245 + 12345;
        return static_cast<float>((rng_state_ >> 16) & 0x7FFF) / 32767.0f;
    }

    void init_patterns() {
        // Pattern 0: Basic 4/4
        auto& p0 = patterns_[0];
        p0[0] = {1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0}; // Kick
        p0[1] = {0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0}; // Snare
        p0[2] = {1,0,1,0, 1,0,1,0, 1,0,1,0, 1,0,1,0}; // HiHat
        p0[3] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1}; // Clap

        // Pattern 1: Breakbeat
        auto& p1 = patterns_[1];
        p1[0] = {1,0,0,0, 0,0,1,0, 0,0,0,0, 1,0,0,0};
        p1[1] = {0,0,0,0, 1,0,0,1, 0,0,0,0, 1,0,0,0};
        p1[2] = {1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1};
        p1[3] = {0,0,0,0, 1,0,0,0, 0,0,0,0, 0,0,1,0};

        // Pattern 2: Sparse
        auto& p2 = patterns_[2];
        p2[0] = {1,0,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0};
        p2[1] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 1,0,0,0};
        p2[2] = {0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0};
        p2[3] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};

        // Pattern 3: Dense
        auto& p3 = patterns_[3];
        p3[0] = {1,0,1,0, 1,0,0,1, 1,0,1,0, 1,0,0,1};
        p3[1] = {0,0,1,0, 1,0,0,0, 0,1,0,0, 1,0,1,0};
        p3[2] = {1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1};
        p3[3] = {0,0,0,1, 0,0,0,1, 0,0,0,1, 0,0,0,1};
    }

    void generate_step(midi::MidiBuffer& midi_out, int32_t sample_offset,
                      int pattern, float density, int velocity, float randomize) {
        pattern = std::clamp(pattern, 0, 3);
        static constexpr uint8_t notes[] = {kKick, kSnare, kHiHat, kClap};

        for (int track = 0; track < kTracks; ++track) {
            bool trigger = patterns_[pattern][track][step_position_];

            // Density modulation: randomly skip or add hits
            if (trigger && next_random() > density) trigger = false;
            if (!trigger && next_random() > (1.0f - density * randomize * 0.3f)) trigger = true;

            if (trigger) {
                // Velocity variation
                int vel = velocity;
                if (randomize > 0) {
                    vel += static_cast<int>((next_random() - 0.5f) * randomize * 40);
                    vel = std::clamp(vel, 1, 127);
                }

                auto note_on = midi::MidiEvent::note_on(9, notes[track],
                    static_cast<uint8_t>(vel));
                note_on.sample_offset = sample_offset;
                midi_out.add(note_on);
            }
        }
    }
};

inline std::unique_ptr<format::Processor> create_pulp_drums() {
    return std::make_unique<PulpDrumsProcessor>();
}

} // namespace pulp::examples
