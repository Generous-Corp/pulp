#pragma once

// PulpPluck — Simple plucked-string synth for web demo
// Uses Karplus-Strong algorithm: short noise burst → delay line with feedback
// 8-voice polyphony with MIDI note tracking

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <cmath>
#include <array>
#include <vector>

namespace pulp::examples {

enum PluckParams { kPluckDecay = 0, kPluckBright = 1, kPluckVolume = 2 };

class PulpPluck : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpPluck",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.pluck",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Main Out", 2, false}},
            .accepts_midi = true,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({kPluckDecay, "Decay", "s", {0.1f, 5.0f, 1.0f, 0.01f}});
        store.add_parameter({kPluckBright, "Brightness", "%", {0.0f, 100.0f, 70.0f, 1.0f}});
        store.add_parameter({kPluckVolume, "Volume", "dB", {-60.0f, 6.0f, -6.0f, 0.1f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        for (auto& v : voices_) v.active = false;
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        float decay = state().get_value(kPluckDecay);
        float bright = state().get_value(kPluckBright) / 100.0f;
        float vol_db = state().get_value(kPluckVolume);
        float vol = std::pow(10.0f, vol_db / 20.0f);

        // Process MIDI
        for (const auto& event : midi_in) {
            if (event.is_note_on()) {
                trigger_voice(event.note(), event.velocity() / 127.0f, decay, bright);
            } else if (event.is_note_off()) {
                release_voice(event.note());
            }
        }

        // Render voices
        auto out_l = output.channel(0);
        auto out_r = output.channel(output.num_channels() > 1 ? 1 : 0);

        for (size_t i = 0; i < out_l.size(); ++i) {
            float sample = 0.0f;
            for (auto& v : voices_) {
                if (!v.active) continue;
                sample += render_voice(v, bright);
            }
            out_l[i] = sample * vol;
            out_r[i] = sample * vol;
        }
    }

private:
    static constexpr int kMaxVoices = 8;
    static constexpr int kMaxDelay = 4096;

    struct Voice {
        bool active = false;
        uint8_t note = 0;
        float velocity = 0;
        std::vector<float> delay_line;
        int delay_length = 0;
        int write_pos = 0;
        float feedback = 0;
        int samples_since_trigger = 0;
    };

    std::array<Voice, kMaxVoices> voices_;
    double sample_rate_ = 48000.0;

    void trigger_voice(uint8_t note, float velocity, float decay, float bright) {
        // Find free voice or steal oldest
        Voice* voice = nullptr;
        int oldest_samples = -1;
        for (auto& v : voices_) {
            if (!v.active) { voice = &v; break; }
            if (v.samples_since_trigger > oldest_samples) {
                oldest_samples = v.samples_since_trigger;
                voice = &v;
            }
        }
        if (!voice) return;

        float freq = 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
        int delay_len = static_cast<int>(sample_rate_ / freq);
        if (delay_len < 2) delay_len = 2;
        if (delay_len > kMaxDelay) delay_len = kMaxDelay;

        voice->active = true;
        voice->note = note;
        voice->velocity = velocity;
        voice->delay_length = delay_len;
        voice->write_pos = 0;
        voice->samples_since_trigger = 0;

        // Karplus-Strong: feedback determines decay time
        float target_samples = decay * static_cast<float>(sample_rate_);
        voice->feedback = std::pow(0.001f, static_cast<float>(delay_len) / target_samples);

        // Fill delay line with noise burst (excitation)
        voice->delay_line.resize(delay_len, 0.0f);
        for (int i = 0; i < delay_len; ++i) {
            // Simple pseudo-random noise
            float noise = (static_cast<float>(rand()) / RAND_MAX) * 2.0f - 1.0f;
            voice->delay_line[i] = noise * velocity;
        }
    }

    void release_voice(uint8_t note) {
        for (auto& v : voices_) {
            if (v.active && v.note == note) {
                v.feedback *= 0.5f; // Faster decay on release
            }
        }
    }

    float render_voice(Voice& v, float bright) {
        if (v.delay_length == 0) return 0.0f;

        int pos = v.write_pos;
        int prev = (pos - 1 + v.delay_length) % v.delay_length;

        // Read current sample
        float out = v.delay_line[pos];

        // Low-pass filtered feedback (Karplus-Strong averaging)
        float filtered = v.delay_line[prev] * bright + v.delay_line[pos] * (1.0f - bright);
        v.delay_line[pos] = filtered * v.feedback;

        v.write_pos = (v.write_pos + 1) % v.delay_length;
        v.samples_since_trigger++;

        // Auto-deactivate when silent
        if (v.samples_since_trigger > v.delay_length * 100 &&
            std::abs(out) < 0.0001f) {
            v.active = false;
        }

        return out;
    }
};

inline std::unique_ptr<format::Processor> create_pulp_pluck() {
    return std::make_unique<PulpPluck>();
}

} // namespace pulp::examples
