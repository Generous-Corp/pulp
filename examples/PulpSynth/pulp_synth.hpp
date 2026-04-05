#pragma once

// PulpSynth — macro oscillator synth with filter, ADSR, and presets
// Validates: pulp-signal DSP library, parameter system, state serialization
// Uses: Oscillator (polyBLEP), Svf (TPT filter), Adsr, SmoothedValue, Gain

#include <pulp/format/processor.hpp>
#include <pulp/signal/oscillator.hpp>
#include <pulp/signal/svf.hpp>
#include <pulp/signal/adsr.hpp>
#include <pulp/signal/smoothed_value.hpp>
#include <pulp/signal/gain.hpp>
#include <cmath>
#include <array>

namespace pulp::examples {

enum SynthParams : state::ParamID {
    kOscWaveform = 100,   // 0=sine, 1=saw, 2=square, 3=triangle
    kOscDetune   = 101,   // cents (-100 to +100)
    kFilterCutoff = 102,  // Hz (20 to 20000)
    kFilterReso  = 103,   // 0.1 to 10
    kFilterEnv   = 104,   // envelope amount (0 to 1)
    kAmpAttack   = 105,   // seconds
    kAmpDecay    = 106,
    kAmpSustain  = 107,
    kAmpRelease  = 108,
    kMasterGain  = 109,   // dB
};

class PulpSynthProcessor : public format::Processor {
public:
    static constexpr int kMaxVoices = 8;

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpSynth",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.synth",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = false,
            .tail_samples = -1,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({kOscWaveform, "Waveform", "", {0, 3, 1, 1}});
        store.add_parameter({kOscDetune, "Detune", "ct", {-100, 100, 0, 0.1f}});
        store.add_parameter({kFilterCutoff, "Cutoff", "Hz", {20, 20000, 5000, 1}});
        store.add_parameter({kFilterReso, "Resonance", "", {0.1f, 10, 0.707f, 0.01f}});
        store.add_parameter({kFilterEnv, "Filter Env", "", {0, 1, 0.5f, 0.01f}});
        store.add_parameter({kAmpAttack, "Attack", "s", {0.001f, 2, 0.01f, 0.001f}});
        store.add_parameter({kAmpDecay, "Decay", "s", {0.001f, 2, 0.1f, 0.001f}});
        store.add_parameter({kAmpSustain, "Sustain", "", {0, 1, 0.7f, 0.01f}});
        store.add_parameter({kAmpRelease, "Release", "s", {0.001f, 5, 0.3f, 0.001f}});
        store.add_parameter({kMasterGain, "Gain", "dB", {-60, 12, -6, 0.1f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = static_cast<float>(ctx.sample_rate);
        for (auto& v : voices_) {
            v = {};
            v.osc.set_sample_rate(sample_rate_);
            v.osc2.set_sample_rate(sample_rate_);
            v.filter.set_sample_rate(sample_rate_);
            v.env.set_sample_rate(sample_rate_);
        }
        gain_smooth_.set_ramp_time(0.01f, sample_rate_);
        cutoff_smooth_.set_ramp_time(0.005f, sample_rate_);
    }

    void process(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>&,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer&,
        const format::ProcessContext&) override
    {
        // Read parameters
        int waveform = static_cast<int>(state().get_value(kOscWaveform));
        float cutoff_hz = state().get_value(kFilterCutoff);
        float reso = state().get_value(kFilterReso);
        float filter_env = state().get_value(kFilterEnv);
        float gain_db = state().get_value(kMasterGain);

        signal::Adsr::Params env_params;
        env_params.attack = state().get_value(kAmpAttack);
        env_params.decay = state().get_value(kAmpDecay);
        env_params.sustain = state().get_value(kAmpSustain);
        env_params.release = state().get_value(kAmpRelease);

        gain_smooth_.set_target(signal::db_to_linear(gain_db));
        cutoff_smooth_.set_target(cutoff_hz);

        auto wave = static_cast<signal::Oscillator::Waveform>(
            std::clamp(waveform, 0, 3));

        midi_in.sort();
        std::size_t midi_idx = 0;

        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            // Process MIDI
            while (midi_idx < midi_in.size() &&
                   midi_in[midi_idx].sample_offset <= static_cast<int32_t>(i)) {
                auto& evt = midi_in[midi_idx];
                if (evt.is_note_on()) note_on(evt.note(), evt.velocity() / 127.0f, wave, env_params);
                else if (evt.is_note_off()) note_off(evt.note());
                ++midi_idx;
            }

            float sample = 0;
            float cut = cutoff_smooth_.next();
            float gain = gain_smooth_.next();

            for (auto& v : voices_) {
                if (!v.active && !v.env.is_active()) continue;

                v.env.set_params(env_params);
                float env_level = v.env.next();
                if (env_level < 0.0001f && !v.active) continue;

                // Oscillator (two slightly detuned for thickness)
                float osc_out = v.osc.next() * 0.5f + v.osc2.next() * 0.5f;

                // Filter with envelope modulation
                float mod_cutoff = cut + filter_env * env_level * (20000.0f - cut);
                mod_cutoff = std::clamp(mod_cutoff, 20.0f, 20000.0f);
                v.filter.set_frequency(mod_cutoff);
                v.filter.set_resonance(reso);
                float filtered = v.filter.process(osc_out);

                sample += filtered * env_level * v.velocity;
            }

            sample *= gain;

            for (std::size_t ch = 0; ch < output.num_channels(); ++ch)
                output.channel(ch)[i] = sample;
        }

        // Remaining MIDI
        while (midi_idx < midi_in.size()) {
            auto& evt = midi_in[midi_idx];
            if (evt.is_note_on()) note_on(evt.note(), evt.velocity() / 127.0f, wave, env_params);
            else if (evt.is_note_off()) note_off(evt.note());
            ++midi_idx;
        }
    }

private:
    struct Voice {
        bool active = false;
        uint8_t note = 0;
        float velocity = 0;
        signal::Oscillator osc, osc2;
        signal::Svf filter;
        signal::Adsr env;
    };

    std::array<Voice, kMaxVoices> voices_{};
    float sample_rate_ = 48000;
    signal::SmoothedValue<float> gain_smooth_{1.0f};
    signal::SmoothedValue<float> cutoff_smooth_{5000.0f};

    void note_on(uint8_t note, float vel, signal::Oscillator::Waveform wave,
                 const signal::Adsr::Params& env_params) {
        Voice* target = nullptr;
        for (auto& v : voices_)
            if (!v.active && !v.env.is_active()) { target = &v; break; }
        if (!target) target = &voices_[0];

        float freq = 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
        float detune = state().get_value(kOscDetune);
        float freq2 = freq * std::pow(2.0f, detune / 1200.0f);

        target->active = true;
        target->note = note;
        target->velocity = vel;
        target->osc.set_frequency(freq);
        target->osc.set_waveform(wave);
        target->osc.reset();
        target->osc2.set_frequency(freq2);
        target->osc2.set_waveform(wave);
        target->osc2.reset();
        target->filter.set_mode(signal::Svf::Mode::lowpass);
        target->filter.reset();
        target->env.set_params(env_params);
        target->env.set_sample_rate(sample_rate_);
        target->env.note_on();
    }

    void note_off(uint8_t note) {
        for (auto& v : voices_)
            if (v.active && v.note == note) {
                v.active = false;
                v.env.note_off();
            }
    }
};

inline std::unique_ptr<format::Processor> create_pulp_synth() {
    return std::make_unique<PulpSynthProcessor>();
}

} // namespace pulp::examples
