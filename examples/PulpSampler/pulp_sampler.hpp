#pragma once

/// PulpSampler — audio file sampler with MIDI triggering and ADSR envelope.
/// Demonstrates: audio file loading, sample playback, ADSR, pitch shifting,
/// waveform editor integration, PresetManager.

#include <pulp/audio/sample_resource.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/signal/adsr.hpp>
#include <pulp/signal/smoothed_value.hpp>
#include <cmath>
#include <string>
#include <vector>

namespace pulp::examples {

enum SamplerParams : state::ParamID {
    kSamplerGain     = 1,
    kSamplerAttack   = 2,
    kSamplerDecay    = 3,
    kSamplerSustain  = 4,
    kSamplerRelease  = 5,
    kSamplerPitch    = 6,  // semitones offset
    kSamplerLoop     = 7,  // 0 = one-shot, 1 = loop
};

/// A single voice for polyphonic sample playback.
struct SamplerVoice {
    bool active = false;
    int note = -1;
    float velocity = 0;
    double position = 0;     ///< Current playback position (fractional samples)
    double speed = 1.0;      ///< Playback speed (1.0 = original pitch)
    signal::Adsr adsr;
    bool released = false;

    void start(int n, float vel, double spd, float sr) {
        note = n;
        velocity = vel;
        position = 0;
        speed = spd;
        active = true;
        released = false;
        adsr.reset();
        adsr.set_sample_rate(sr);
        adsr.note_on();
    }

    void release() {
        adsr.note_off();
        released = true;
    }
};

class PulpSamplerProcessor : public format::Processor {
public:
    static constexpr int kMaxVoices = 8;
    static constexpr int kRootNote = 60; // Middle C

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpSampler",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.sampler",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = false,
            .tail_samples = 0,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kSamplerGain, .name = "Gain",
            .unit = "dB", .range = {-60, 12, 0, 0.1f}});
        store.add_parameter({.id = kSamplerAttack, .name = "Attack",
            .unit = "ms", .range = {0, 5000, 10, 1}});
        store.add_parameter({.id = kSamplerDecay, .name = "Decay",
            .unit = "ms", .range = {0, 5000, 100, 1}});
        store.add_parameter({.id = kSamplerSustain, .name = "Sustain",
            .unit = "%", .range = {0, 100, 80, 1}});
        store.add_parameter({.id = kSamplerRelease, .name = "Release",
            .unit = "ms", .range = {0, 10000, 200, 1}});
        store.add_parameter({.id = kSamplerPitch, .name = "Pitch",
            .unit = "st", .range = {-24, 24, 0, 1}});
        store.add_parameter({.id = kSamplerLoop, .name = "Loop",
            .unit = "", .range = {0, 1, 0, 1}});
    }

    /// Load a mono sample buffer. Call from the main thread.
    void load_sample(const float* data, int num_samples, float sample_rate) {
        if (!data || num_samples <= 0) {
            sample_resource_.publish_missing({}, "empty sample buffer");
            return;
        }
        audio::AudioFileData sample;
        sample.sample_rate = static_cast<uint32_t>(sample_rate);
        sample.channels.resize(1);
        sample.channels[0].assign(data, data + num_samples);
        sample_resource_.publish_loaded(std::move(sample), "memory://mono");
    }

    /// Load a sample from interleaved stereo (takes left channel).
    void load_sample_stereo(const float* interleaved, int num_frames, float sample_rate) {
        if (!interleaved || num_frames <= 0) {
            sample_resource_.publish_missing({}, "empty stereo sample buffer");
            return;
        }
        audio::AudioFileData sample;
        sample.sample_rate = static_cast<uint32_t>(sample_rate);
        sample.channels.resize(1);
        sample.channels[0].resize(static_cast<size_t>(num_frames));
        for (int i = 0; i < num_frames; ++i) {
            sample.channels[0][static_cast<size_t>(i)] = interleaved[i * 2]; // left channel
        }
        sample_resource_.publish_loaded(std::move(sample), "memory://stereo-left");
    }

    void mark_sample_missing(std::string path, std::string reason) {
        sample_resource_.publish_missing(std::move(path), std::move(reason));
    }

    bool has_sample() const { return sample_resource_.snapshot().ready(); }
    int sample_length() const {
        return static_cast<int>(sample_resource_.snapshot().frame_count);
    }
    audio::SampleResourceDiagnostics sample_diagnostics() const {
        return sample_resource_.diagnostics();
    }

    void prepare(const format::PrepareContext& ctx) override {
        host_sample_rate_ = static_cast<float>(ctx.sample_rate);
        for (auto& v : voices_) v = {};
    }

    void process(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>&,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer&,
        const format::ProcessContext&) override
    {
        // Clear output
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            auto out = output.channel(ch);
            for (std::size_t i = 0; i < output.num_samples(); ++i) out[i] = 0;
        }

        const auto sample_snapshot = sample_resource_.snapshot();
        if (!sample_snapshot.ready() || sample_snapshot.data->channels.empty()) return;
        const auto& sample_data = sample_snapshot.data->channels[0];
        if (sample_data.empty()) return;

        // Process MIDI
        for (std::size_t i = 0; i < midi_in.size(); ++i) {
            auto& event = midi_in[i];
            if (event.message.isNoteOn()) {
                trigger_note(event.message.getNoteNumber(),
                             static_cast<float>(event.message.getVelocity()) / 127.0f);
            } else if (event.message.isNoteOff()) {
                release_note(event.message.getNoteNumber());
            }
        }

        // Read parameters
        float gain_db = state().get_value(kSamplerGain);
        float gain = std::pow(10.0f, gain_db / 20.0f);
        float attack = state().get_value(kSamplerAttack);
        float decay = state().get_value(kSamplerDecay);
        float sustain = state().get_value(kSamplerSustain) / 100.0f;
        float release = state().get_value(kSamplerRelease);
        float pitch_st = state().get_value(kSamplerPitch);
        bool loop = state().get_value(kSamplerLoop) >= 0.5f;

        int num_samples = static_cast<int>(output.num_samples());
        int sample_len = static_cast<int>(sample_data.size());

        for (auto& voice : voices_) {
            if (!voice.active) continue;

            // Update ADSR
            voice.adsr.set_params({
                attack / 1000.0f,
                decay / 1000.0f,
                sustain,
                release / 1000.0f
            });

            // Pitch: note offset + pitch param
            float note_offset = static_cast<float>(voice.note - kRootNote) + pitch_st;
            double speed = std::pow(2.0, note_offset / 12.0)
                         * (static_cast<double>(sample_snapshot.sample_rate) / static_cast<double>(host_sample_rate_));
            voice.speed = speed;

            for (int i = 0; i < num_samples; ++i) {
                float env = voice.adsr.next();

                if (env <= 0.0001f && voice.released) {
                    voice.active = false;
                    break;
                }

                int pos = static_cast<int>(voice.position);
                if (pos >= sample_len) {
                    if (loop) {
                        voice.position = std::fmod(voice.position, static_cast<double>(sample_len));
                        pos = static_cast<int>(voice.position);
                    } else {
                        voice.active = false;
                        break;
                    }
                }

                // Linear interpolation
                float frac = static_cast<float>(voice.position - static_cast<double>(pos));
                float s0 = sample_data[static_cast<size_t>(pos)];
                float s1 = (pos + 1 < sample_len) ? sample_data[static_cast<size_t>(pos + 1)] : s0;
                float sample = s0 + frac * (s1 - s0);

                float out = sample * env * voice.velocity * gain;

                // Write to all output channels
                for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
                    output.channel(ch)[static_cast<size_t>(i)] += out;
                }

                voice.position += voice.speed;
            }
        }
    }

private:
    audio::SampleResourceHandle sample_resource_;
    float host_sample_rate_ = 44100.0f;
    SamplerVoice voices_[kMaxVoices]{};

    void trigger_note(int note, float velocity) {
        // Find free voice or steal oldest
        SamplerVoice* target = nullptr;
        for (auto& v : voices_) {
            if (!v.active) { target = &v; break; }
        }
        if (!target) target = &voices_[0]; // steal first voice

        float pitch_st = state().get_value(kSamplerPitch);
        const auto sample_snapshot = sample_resource_.snapshot();
        const auto sample_rate = sample_snapshot.sample_rate != 0
            ? static_cast<float>(sample_snapshot.sample_rate)
            : host_sample_rate_;
        float note_offset = static_cast<float>(note - kRootNote) + pitch_st;
        double speed = std::pow(2.0, note_offset / 12.0)
                     * (static_cast<double>(sample_rate) / static_cast<double>(host_sample_rate_));
        target->start(note, velocity, speed, host_sample_rate_);
    }

    void release_note(int note) {
        for (auto& v : voices_) {
            if (v.active && v.note == note && !v.released) {
                v.release();
            }
        }
    }
};

inline std::unique_ptr<format::Processor> create_pulp_sampler() {
    return std::make_unique<PulpSamplerProcessor>();
}

} // namespace pulp::examples
