#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/smoothed_value.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

constexpr int kMaxChannels = 16;
constexpr float kMaxFoldGain = 11.0f;   // knob at max multiplies the input 12x
constexpr float kMaxOffset = 0.9f;      // symmetry offset, in normalized units

// pulp::signal has no reflecting/wrapping folder: WaveShaperT and SaturatorT are
// compressive shapers, so they flatten the peaks instead of folding them back.
float fold_sine(float x) {
    return std::sin(x * 1.5707963268f);
}

float fold_triangle(float x) {
    const float u = x * 0.25f;
    return 4.0f * std::fabs(u - std::floor(u + 0.75f) + 0.25f) - 1.0f;
}

float fold_wrap(float x) {
    const float u = (x + 1.0f) * 0.5f;
    return 2.0f * (u - std::floor(u)) - 1.0f;
}

struct FOLDModule : rack::engine::Module {
    using L = forge_modular::FOLDLayout;

    pulp::signal::SmoothedValue<float> drive_;
    pulp::signal::SmoothedValue<float> symmetry_;
    pulp::signal::SmoothedValue<float> level_;
    pulp::signal::DcBlocker<float> dc_[kMaxChannels];

    FOLDModule() {
        forge_modular::config_FOLD(this);
        configBypass(L::IN_INPUT, L::OUT_OUTPUT);

        prepare(48000.0f);
        drive_.set_immediate(0.25f);
        symmetry_.set_immediate(0.0f);
        level_.set_immediate(0.8f);
    }

    void prepare(float sample_rate) {
        drive_.set_ramp_time(0.01f, sample_rate);
        symmetry_.set_ramp_time(0.01f, sample_rate);
        level_.set_ramp_time(0.01f, sample_rate);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        prepare(e.sampleRate);
        for (int c = 0; c < kMaxChannels; ++c) {
            dc_[c] = pulp::signal::DcBlocker<float>{};
        }
    }

    void process(const ProcessArgs& args) override {
        drive_.set_target(params[L::DRIVE_PARAM].getValue());
        symmetry_.set_target(params[L::SYM_PARAM].getValue());
        level_.set_target(params[L::LEVEL_PARAM].getValue());

        const float drive_base = drive_.next();
        const float sym_base = symmetry_.next();
        const float level = level_.next();

        const float drive_cv_amount = params[L::DRIVE_CV_PARAM].getValue();
        const float sym_cv_amount = params[L::SYM_CV_PARAM].getValue();
        const int shape = static_cast<int>(std::round(params[L::SHAPE_PARAM].getValue()));

        const int channels = forge_modular::channels_FOLD(this);
        outputs[L::OUT_OUTPUT].setChannels(channels);

        float peak_excursion = 0.0f;

        for (int c = 0; c < channels; ++c) {
            const float drive_cv = inputs[L::DRIVE_CV_INPUT].getPolyVoltage(c);
            const float sym_cv = forge_modular::read_FOLD_SYM_CV_INPUT(this, c);

            const float amount = std::clamp(
                drive_base + drive_cv_amount * drive_cv / V::kCvBipolar, 0.0f, 1.0f);
            const float offset = std::clamp(
                sym_base + sym_cv_amount * sym_cv / V::kCvBipolar, -1.0f, 1.0f) * kMaxOffset;

            // Square-law drive so the low end of the knob still has usable resolution.
            const float gain = 1.0f + kMaxFoldGain * amount * amount;
            const float in = inputs[L::IN_INPUT].getPolyVoltage(c) / V::kAudioPeak;
            const float x = in * gain + offset;

            float y = (shape == 1) ? fold_triangle(x)
                    : (shape == 2) ? fold_wrap(x)
                                   : fold_sine(x);

            // The symmetry offset is a DC term the folder turns into audible thump.
            y = dc_[c].process(y);
            y = std::clamp(y * level, -1.0f, 1.0f);

            peak_excursion = std::max(peak_excursion, std::fabs(x));
            outputs[L::OUT_OUTPUT].setVoltage(y * V::kAudioPeak, c);
        }

        const float folding = std::clamp((peak_excursion - 1.0f) / 3.0f, 0.0f, 1.0f);
        lights[L::FOLD_LIGHT].setBrightnessSmooth(folding, args.sampleTime);
    }
};

struct FOLDWidget : rack::app::ModuleWidget {
    explicit FOLDWidget(FOLDModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/FOLD.svg"),
            rack::asset::plugin(pluginInstance, "res/FOLD-dark.svg")));
        forge_modular::place_FOLD(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelFOLD = rack::createModel<FOLDModule, FOLDWidget>("FOLD");
