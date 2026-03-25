#pragma once

// PulpEffect — biquad filter effect with multiple parameter types
// Validates: frequency parameter (wide range), resonance, stepped enum (filter type),
// dry/wet mix, and more complex DSP than PulpGain.
// Phase 5 validation: demonstrates parameter diversity across format adapters.

#include <pulp/format/processor.hpp>
#include <cmath>
#include <algorithm>
#include <array>

namespace pulp::examples {

enum EffectParams : state::ParamID {
    kFrequency  = 1,
    kResonance  = 2,
    kFilterType = 3,  // 0=lowpass, 1=highpass, 2=bandpass
    kMix        = 4,
    kBypass     = 5,
};

// Simple biquad filter state (per channel)
struct BiquadState {
    float x1 = 0, x2 = 0; // input history
    float y1 = 0, y2 = 0; // output history

    void reset() { x1 = x2 = y1 = y2 = 0; }
};

// Biquad coefficients
struct BiquadCoeffs {
    float b0 = 1, b1 = 0, b2 = 0;
    float a1 = 0, a2 = 0;
};

// Compute biquad coefficients for lowpass/highpass/bandpass
inline BiquadCoeffs compute_biquad(int type, float freq, float q, double sample_rate) {
    BiquadCoeffs c;
    float w0 = 2.0f * static_cast<float>(M_PI) * freq / static_cast<float>(sample_rate);
    float cos_w0 = std::cos(w0);
    float sin_w0 = std::sin(w0);
    float alpha = sin_w0 / (2.0f * q);

    float a0;
    switch (type) {
        case 0: // lowpass
            c.b0 = (1.0f - cos_w0) / 2.0f;
            c.b1 = 1.0f - cos_w0;
            c.b2 = (1.0f - cos_w0) / 2.0f;
            a0 = 1.0f + alpha;
            c.a1 = -2.0f * cos_w0;
            c.a2 = 1.0f - alpha;
            break;
        case 1: // highpass
            c.b0 = (1.0f + cos_w0) / 2.0f;
            c.b1 = -(1.0f + cos_w0);
            c.b2 = (1.0f + cos_w0) / 2.0f;
            a0 = 1.0f + alpha;
            c.a1 = -2.0f * cos_w0;
            c.a2 = 1.0f - alpha;
            break;
        case 2: // bandpass
        default:
            c.b0 = alpha;
            c.b1 = 0.0f;
            c.b2 = -alpha;
            a0 = 1.0f + alpha;
            c.a1 = -2.0f * cos_w0;
            c.a2 = 1.0f - alpha;
            break;
    }

    // Normalize
    c.b0 /= a0; c.b1 /= a0; c.b2 /= a0;
    c.a1 /= a0; c.a2 /= a0;
    return c;
}

class PulpEffectProcessor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpEffect",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.effect",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .default_input_channels = 2,
            .default_output_channels = 2,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kFrequency,
            .name = "Frequency",
            .unit = "Hz",
            .range = {20.0f, 20000.0f, 1000.0f, 0.0f}, // continuous, wide range
        });
        store.add_parameter({
            .id = kResonance,
            .name = "Resonance",
            .unit = "",
            .range = {0.1f, 10.0f, 0.707f, 0.0f}, // Q factor
        });
        store.add_parameter({
            .id = kFilterType,
            .name = "Type",
            .unit = "",
            .range = {0.0f, 2.0f, 0.0f, 1.0f}, // stepped: 0=LP, 1=HP, 2=BP
        });
        store.add_parameter({
            .id = kMix,
            .name = "Mix",
            .unit = "%",
            .range = {0.0f, 100.0f, 100.0f, 0.1f},
        });
        store.add_parameter({
            .id = kBypass,
            .name = "Bypass",
            .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
        });
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate;
        for (auto& s : filter_state_) s.reset();
    }

    void release() override {
        for (auto& s : filter_state_) s.reset();
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

        float freq = state().get_value(kFrequency);
        float q = state().get_value(kResonance);
        int type = static_cast<int>(state().get_value(kFilterType));
        float mix = state().get_value(kMix) / 100.0f;

        auto coeffs = compute_biquad(type, freq, q, sample_rate_);

        for (std::size_t ch = 0; ch < output.num_channels() && ch < input.num_channels(); ++ch) {
            auto in = input.channel(ch);
            auto out = output.channel(ch);
            auto& st = filter_state_[ch];

            for (std::size_t i = 0; i < output.num_samples(); ++i) {
                float x = in[i];
                float y = coeffs.b0 * x + coeffs.b1 * st.x1 + coeffs.b2 * st.x2
                        - coeffs.a1 * st.y1 - coeffs.a2 * st.y2;

                st.x2 = st.x1; st.x1 = x;
                st.y2 = st.y1; st.y1 = y;

                out[i] = x * (1.0f - mix) + y * mix;
            }
        }
    }

private:
    double sample_rate_ = 48000.0;
    std::array<BiquadState, 8> filter_state_{}; // up to 8 channels
};

inline std::unique_ptr<format::Processor> create_pulp_effect() {
    return std::make_unique<PulpEffectProcessor>();
}

} // namespace pulp::examples
