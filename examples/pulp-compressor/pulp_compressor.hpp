#pragma once

// PulpCompressor — sidechain compressor validating multi-bus architecture
// Uses two input buses: Main (audio to compress) and Sidechain (detection signal).
// When sidechain is connected, compression follows the sidechain level.
// When sidechain is not connected, compression follows the main input level.

#include <pulp/format/processor.hpp>
#include <cmath>
#include <algorithm>

namespace pulp::examples {

enum CompressorParams : state::ParamID {
    kThreshold  = 1,  // dB
    kRatio      = 2,  // ratio (1 = no compression, 20 = limiting)
    kAttack     = 3,  // ms
    kRelease    = 4,  // ms
    kMakeupGain = 5,  // dB
    kBypass     = 6,
};

class PulpCompressorProcessor : public format::Processor {
  public:
    // Reads its parameters once per block, so it opts out of per-point
    // automation: the VST3, AU v2 and CLAP adapters then write each
    // automated parameter once per block and dense automation costs nothing.
    bool wants_sample_accurate_parameters() const override {
        return false;
    }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpCompressor",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.compressor",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            // Multi-bus: main stereo input + optional sidechain
            .input_buses = {
                {"Main In", 2, false},         // Main audio to compress
                {"Sidechain", 2, true},        // Optional sidechain detection
            },
            .output_buses = {{"Main Out", 2, false}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kThreshold, .name = "Threshold", .unit = "dB",
            .range = {-60.0f, 0.0f, -20.0f, 0.1f},
        });
        store.add_parameter({
            .id = kRatio, .name = "Ratio", .unit = ":1",
            .range = {1.0f, 20.0f, 4.0f, 0.1f},
        });
        store.add_parameter({
            .id = kAttack, .name = "Attack", .unit = "ms",
            .range = {0.1f, 100.0f, 10.0f, 0.1f},
        });
        store.add_parameter({
            .id = kRelease, .name = "Release", .unit = "ms",
            .range = {10.0f, 1000.0f, 100.0f, 1.0f},
        });
        store.add_parameter({
            .id = kMakeupGain, .name = "Makeup", .unit = "dB",
            .range = {0.0f, 30.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = kBypass, .name = "Bypass", .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
        });
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        envelope_ = 0.0f;
    }

    void process(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>& input,
        midi::MidiBuffer&, midi::MidiBuffer&,
        const format::ProcessContext&) override
    {
        if (state().get_value(kBypass) >= 0.5f) {
            for (std::size_t ch = 0; ch < output.num_channels() && ch < input.num_channels(); ++ch) {
                auto in = input.channel(ch);
                auto out = output.channel(ch);
                for (std::size_t i = 0; i < output.num_samples(); ++i)
                    out[i] = in[i];
            }
            return;
        }

        float threshold_db = state().get_value(kThreshold);
        float ratio = state().get_value(kRatio);
        float attack_ms = state().get_value(kAttack);
        float release_ms = state().get_value(kRelease);
        float makeup_db = state().get_value(kMakeupGain);

        float threshold_lin = std::pow(10.0f, threshold_db / 20.0f);
        float makeup_lin = std::pow(10.0f, makeup_db / 20.0f);
        float attack_coeff = std::exp(-1.0f / (attack_ms * 0.001f * static_cast<float>(sample_rate_)));
        float release_coeff = std::exp(-1.0f / (release_ms * 0.001f * static_cast<float>(sample_rate_)));

        // Use sidechain for detection if available, otherwise use main input
        const auto* sc = sidechain_input();
        const auto& detect = sc ? *sc : input;

        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            // Peak detection across channels
            float peak = 0.0f;
            for (std::size_t ch = 0; ch < detect.num_channels(); ++ch) {
                peak = std::max(peak, std::abs(detect.channel(ch)[i]));
            }

            // Envelope follower (attack/release)
            if (peak > envelope_)
                envelope_ = attack_coeff * envelope_ + (1.0f - attack_coeff) * peak;
            else
                envelope_ = release_coeff * envelope_ + (1.0f - release_coeff) * peak;

            // Gain computation
            float gain = 1.0f;
            if (envelope_ > threshold_lin && ratio > 1.0f) {
                float db_over = 20.0f * std::log10(envelope_ / threshold_lin);
                float db_reduction = db_over * (1.0f - 1.0f / ratio);
                gain = std::pow(10.0f, -db_reduction / 20.0f);
            }

            // Apply gain + makeup to all output channels
            for (std::size_t ch = 0; ch < output.num_channels() && ch < input.num_channels(); ++ch) {
                output.channel(ch)[i] = input.channel(ch)[i] * gain * makeup_lin;
            }
        }
    }

private:
    double sample_rate_ = 48000.0;
    float envelope_ = 0.0f;
};

inline std::unique_ptr<format::Processor> create_pulp_compressor() {
    return std::make_unique<PulpCompressorProcessor>();
}

} // namespace pulp::examples
