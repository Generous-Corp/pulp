#pragma once

// PulpTone — simple oscillator synth with MIDI input
// Validates: audio output, MIDI input, note handling, parameter system
// Phase 4 validation example from the roadmap

#include <pulp/format/processor.hpp>
#include <cmath>
#include <array>
#include <numbers>

namespace pulp::examples {

enum ToneParams : state::ParamID {
    kWaveform   = 10,  // 0=sine, 1=saw, 2=square
    kVolume     = 11,
    kAttack     = 12,
    kRelease    = 13,
};

// Simple 8-voice polyphonic synth
class PulpToneProcessor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpTone",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.tone",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = false,
            .tail_samples = -1, // Infinite tail (synth)
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kWaveform,
            .name = "Waveform",
            .unit = "",
            .range = {0.0f, 2.0f, 0.0f, 1.0f}, // 0=sine, 1=saw, 2=square
        });
        store.add_parameter({
            .id = kVolume,
            .name = "Volume",
            .unit = "dB",
            .range = {-60.0f, 0.0f, -6.0f, 0.1f},
        });
        store.add_parameter({
            .id = kAttack,
            .name = "Attack",
            .unit = "ms",
            .range = {1.0f, 1000.0f, 10.0f, 1.0f},
        });
        store.add_parameter({
            .id = kRelease,
            .name = "Release",
            .unit = "ms",
            .range = {1.0f, 2000.0f, 200.0f, 1.0f},
        });
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        for (auto& v : voices_) v = {};
    }

    void process(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>&,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer&,
        const format::ProcessContext&) override
    {
        int waveform = static_cast<int>(state().get_value(kWaveform));
        float volume_db = state().get_value(kVolume);
        float volume = std::pow(10.0f, volume_db / 20.0f);
        float attack_ms = state().get_value(kAttack);
        float release_ms = state().get_value(kRelease);
        float attack_coeff = 1.0f / (static_cast<float>(sample_rate_) * attack_ms / 1000.0f);
        float release_coeff = 1.0f / (static_cast<float>(sample_rate_) * release_ms / 1000.0f);

        // Sort MIDI by sample offset for sample-accurate processing
        midi_in.sort();

        std::size_t midi_idx = 0;

        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            // Process MIDI events at this sample
            while (midi_idx < midi_in.size() &&
                   midi_in[midi_idx].sample_offset <= static_cast<int32_t>(i)) {
                const auto& evt = midi_in[midi_idx];
                if (evt.is_note_on()) {
                    note_on(evt.note(), evt.velocity() / 127.0f);
                } else if (evt.is_note_off()) {
                    note_off(evt.note());
                }
                ++midi_idx;
            }

            // Render all active voices
            float sample = 0.0f;
            for (auto& v : voices_) {
                if (!v.active && v.envelope <= 0.001f) continue;

                // Envelope
                float target = v.active ? v.velocity : 0.0f;
                if (v.envelope < target) {
                    v.envelope += attack_coeff;
                    if (v.envelope > target) v.envelope = target;
                } else {
                    v.envelope -= release_coeff;
                    if (v.envelope < 0.0f) v.envelope = 0.0f;
                }

                // Oscillator
                float osc = 0.0f;
                switch (waveform) {
                    case 0: // Sine
                        osc = static_cast<float>(std::sin(v.phase * 2.0 * std::numbers::pi_v<double>));
                        break;
                    case 1: // Saw
                        osc = static_cast<float>(2.0 * v.phase - 1.0);
                        break;
                    case 2: // Square
                        osc = v.phase < 0.5 ? 1.0f : -1.0f;
                        break;
                }

                sample += osc * v.envelope;
                v.phase += v.freq / sample_rate_;
                if (v.phase >= 1.0) v.phase -= 1.0;
            }

            sample *= volume;

            // Write to all output channels
            for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
                output.channel(ch)[i] = sample;
            }
        }

        // Process remaining MIDI events (past end of buffer)
        while (midi_idx < midi_in.size()) {
            const auto& evt = midi_in[midi_idx];
            if (evt.is_note_on()) note_on(evt.note(), evt.velocity() / 127.0f);
            else if (evt.is_note_off()) note_off(evt.note());
            ++midi_idx;
        }
    }

private:
    static constexpr int kMaxVoices = 8;

    struct Voice {
        bool active = false;
        uint8_t note = 0;
        float velocity = 0.0f;
        float freq = 0.0;
        double phase = 0.0;
        float envelope = 0.0f;
    };

    std::array<Voice, kMaxVoices> voices_{};
    double sample_rate_ = 48000.0;

    void note_on(uint8_t note, float velocity) {
        // Find a free voice or steal the oldest
        Voice* target = nullptr;
        for (auto& v : voices_) {
            if (!v.active && v.envelope <= 0.001f) { target = &v; break; }
        }
        if (!target) target = &voices_[0]; // Steal first voice

        target->active = true;
        target->note = note;
        target->velocity = velocity;
        target->freq = static_cast<float>(440.0 * std::pow(2.0, (note - 69) / 12.0));
        target->phase = 0.0;
    }

    void note_off(uint8_t note) {
        for (auto& v : voices_) {
            if (v.active && v.note == note) {
                v.active = false; // Envelope will decay via release
            }
        }
    }
};

inline std::unique_ptr<format::Processor> create_pulp_tone() {
    return std::make_unique<PulpToneProcessor>();
}

} // namespace pulp::examples
