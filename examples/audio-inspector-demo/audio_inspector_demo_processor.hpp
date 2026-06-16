#pragma once

// Shared processor for the live Audio Inspector demo and its offline proof
// renderer. The renderer lets validation-video proofs mux audio that came from
// the same Pulp Processor path the standalone window is showing.

#include <pulp/format/processor.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

namespace pulp::examples {

enum AudioInspectorDemoParams : state::ParamID {
    kFrequency = 5001,
    kLevelDb = 5002,
};

class AudioInspectorDemoProcessor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "AudioInspectorDemo",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.audio-inspector-demo",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = false,
            .produces_midi = false,
            .tail_samples = -1,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kFrequency,
            .name = "Frequency",
            .unit = "Hz",
            .range = {55.0f, 1760.0f, 440.0f, 1.0f},
        });
        store.add_parameter({
            .id = kLevelDb,
            .name = "Level",
            .unit = "dB",
            .range = {-60.0f, -3.0f, -12.0f, 0.1f},
        });
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate > 0.0 ? ctx.sample_rate : 48000.0;
        phase_ = 0.0;
    }

    format::ViewSize view_size() const override {
        return {520, 360, 360, 260, 900, 640};
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        constexpr double kTwoPi = 6.28318530717958647692;

        const double hz = std::clamp<double>(
            static_cast<double>(state().get_value(kFrequency)), 20.0, 20000.0);
        const double level_db = std::clamp<double>(
            static_cast<double>(state().get_value(kLevelDb)), -90.0, 0.0);
        const float amp = static_cast<float>(std::pow(10.0, level_db / 20.0));
        const double phase_inc = kTwoPi * hz / sample_rate_;

        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            const float sample = amp * static_cast<float>(std::sin(phase_));
            phase_ += phase_inc;
            if (phase_ >= kTwoPi)
                phase_ = std::fmod(phase_, kTwoPi);

            for (std::size_t ch = 0; ch < output.num_channels(); ++ch)
                output.channel(ch)[i] = sample;
        }
    }

private:
    double sample_rate_ = 48000.0;
    double phase_ = 0.0;
};

inline std::unique_ptr<format::Processor> create_audio_inspector_demo() {
    return std::make_unique<AudioInspectorDemoProcessor>();
}

}  // namespace pulp::examples
