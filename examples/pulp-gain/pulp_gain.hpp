#pragma once

// PulpGain — the simplest possible Pulp plugin
// A stereo gain effect with input gain, output gain, and bypass parameters.
// Validates the full pipeline: Processor → format adapter → loadable bundle.

#include <pulp/format/processor.hpp>
#include <cmath>
#include <limits>

namespace pulp::examples {

// Parameter IDs — stable across versions
enum GainParams : state::ParamID {
    kInputGain  = 1,
    kOutputGain = 2,
    kBypass     = 3,
};

class PulpGainProcessor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpGain",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.gain",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = false,
            .produces_midi = false,
            .tail_samples = 0,
            .supports_f64_audio = true,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kInputGain,
            .name = "Input Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = kOutputGain,
            .name = "Output Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = kBypass,
            .name = "Bypass",
            .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
            // A step of 1 quantizes the value; it does NOT declare the semantic
            // role. Adapters derive "stepped" from `kind` (see
            // state::is_discrete_param), so a Toggle must say so — otherwise a
            // CLAP/VST3 host renders bypass as a continuous knob.
            //
            // Must stay AFTER `.range`: ParamInfo declares `range` (line ~282)
            // before `kind` (~317), and C++20 requires designated initializers to
            // follow declaration order. Clang tolerates the wrong order; GCC does
            // NOT, so getting this backwards breaks both Linux legs of every
            // release build while every macOS check stays green.
            .kind = state::ParamKind::Toggle,
        });
    }

    void prepare(const format::PrepareContext&) override {}

    void process(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>& input,
        midi::MidiBuffer&,
        midi::MidiBuffer&,
        const format::ProcessContext&) override
    {
        bool bypass = state().get_value(kBypass) >= 0.5f;

        if (bypass) {
            // Pass-through
            for (std::size_t ch = 0; ch < output.num_channels() && ch < input.num_channels(); ++ch) {
                auto in = input.channel(ch);
                auto out = output.channel(ch);
                for (std::size_t i = 0; i < output.num_samples(); ++i) {
                    out[i] = in[i];
                }
            }
            return;
        }

        // dB -> linear only when a gain parameter actually moved; the two pow()
        // calls otherwise dominate the cost of this whole block.
        float input_db = state().get_value(kInputGain);
        float output_db = state().get_value(kOutputGain);
        if (input_db != cached_input_db_ || output_db != cached_output_db_) {
            cached_input_db_ = input_db;
            cached_output_db_ = output_db;
            cached_gain_ = std::pow(10.0f, input_db / 20.0f) * std::pow(10.0f, output_db / 20.0f);
        }
        float total_gain = cached_gain_;

        for (std::size_t ch = 0; ch < output.num_channels() && ch < input.num_channels(); ++ch) {
            auto in = input.channel(ch);
            auto out = output.channel(ch);
            for (std::size_t i = 0; i < output.num_samples(); ++i) {
                out[i] = in[i] * total_gain;
            }
        }
    }

    void process_f64(
        audio::BufferView<double>& output,
        const audio::BufferView<const double>& input,
        midi::MidiBuffer&,
        midi::MidiBuffer&,
        const format::ProcessContext&) override
    {
        bool bypass = state().get_value(kBypass) >= 0.5f;

        if (bypass) {
            // Pass-through
            for (std::size_t ch = 0; ch < output.num_channels() && ch < input.num_channels(); ++ch) {
                auto in = input.channel(ch);
                auto out = output.channel(ch);
                for (std::size_t i = 0; i < output.num_samples(); ++i) {
                    out[i] = in[i];
                }
            }
            return;
        }

        double input_db = static_cast<double>(state().get_value(kInputGain));
        double output_db = static_cast<double>(state().get_value(kOutputGain));
        double input_gain = std::pow(10.0, input_db / 20.0);
        double output_gain = std::pow(10.0, output_db / 20.0);
        double total_gain = input_gain * output_gain;

        for (std::size_t ch = 0; ch < output.num_channels() && ch < input.num_channels(); ++ch) {
            auto in = input.channel(ch);
            auto out = output.channel(ch);
            for (std::size_t i = 0; i < output.num_samples(); ++i) {
                out[i] = in[i] * total_gain;
            }
        }
    }

  private:
    // NaN never compares equal, so the first block always computes the gain.
    float cached_input_db_ = std::numeric_limits<float>::quiet_NaN();
    float cached_output_db_ = std::numeric_limits<float>::quiet_NaN();
    float cached_gain_ = 1.0f;
};

inline std::unique_ptr<format::Processor> create_pulp_gain() {
    return std::make_unique<PulpGainProcessor>();
}

} // namespace pulp::examples
