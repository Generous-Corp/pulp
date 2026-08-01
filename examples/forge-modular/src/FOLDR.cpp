#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/smoothed_value.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

// Triangle wavefolder. Nothing in pulp/signal implements a reflecting folder
// (SaturatorT / WaveShaperT are monotonic saturators, which cannot fold), so the
// wrap itself is written here; everything around it uses the shared primitives.
inline float fold_triangle(float x) {
    const float t = x * 0.25f + 0.25f;
    const float frac = t - std::floor(t);
    return 1.0f - 4.0f * std::fabs(frac - 0.5f);
}

struct FOLDRModule : rack::engine::Module {
    using L = forge_modular::FOLDRLayout;

    pulp::signal::SmoothedValue<float> drive_;
    pulp::signal::SmoothedValue<float> symmetry_;
    pulp::signal::DcBlocker<float> dc_;
    float sample_rate_ = 48000.0f;

    FOLDRModule() {
        forge_modular::config_FOLDR(this);
        configBypass(L::IN_INPUT, L::OUT_OUTPUT);

        drive_.set_immediate(2.0f);
        symmetry_.set_immediate(0.0f);
        prepare(48000.0f);
    }

    void prepare(float sample_rate) {
        sample_rate_ = sample_rate;
        drive_.set_ramp_time(0.005f, sample_rate);
        symmetry_.set_ramp_time(0.005f, sample_rate);
        dc_.set_pole(1.0f - 20.0f * 2.0f * float(M_PI) / sample_rate);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        prepare(e.sampleRate);
        dc_.process(0.0f);
    }

    void process(const ProcessArgs& args) override {
        const float drive_knob = params[L::DRIVE_PARAM].getValue();
        const float sym_knob = params[L::SYM_PARAM].getValue();
        const float cv_amount = params[L::CVAMT_PARAM].getValue();

        // Fold CV is bipolar +/-5 V; it pushes the drive up or down by up to +/-8x.
        const float cv = forge_modular::read_FOLDR_CV_INPUT(this, 0) / V::kCvBipolar;
        const float drive_target =
            std::clamp(drive_knob + cv_amount * cv * 8.0f, 0.25f, 20.0f);

        drive_.set_target(drive_target);
        symmetry_.set_target(sym_knob);

        const float drive = drive_.next();
        const float symmetry = symmetry_.next();

        const float in = inputs[L::IN_INPUT].getVoltage() / V::kAudioPeak;
        const float folded_input = in * drive + symmetry;
        const float folded = dc_.process(fold_triangle(folded_input));

        if (outputs[L::OUT_OUTPUT].isConnected()) {
            outputs[L::OUT_OUTPUT].setVoltage(
                std::clamp(folded, -1.0f, 1.0f) * V::kAudioPeak);
        }

        // How far past the first fold point the signal has been pushed.
        const float depth = std::clamp(std::fabs(folded_input) - 1.0f, 0.0f, 4.0f) * 0.25f;
        lights[L::FOLD_LIGHT].setBrightnessSmooth(depth, args.sampleTime);
    }
};

struct FOLDRWidget : rack::app::ModuleWidget {
    explicit FOLDRWidget(FOLDRModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/FOLDR.svg"),
            rack::asset::plugin(pluginInstance, "res/FOLDR-dark.svg")));
        forge_modular::place_FOLDR(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelFOLDR = rack::createModel<FOLDRModule, FOLDRWidget>("FOLDR");
