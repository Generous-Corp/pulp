#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/mod_tools.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct OFFSETLYModule : rack::engine::Module {
    using L = forge_modular::OFFSETLYLayout;

    pulp::signal::AttenuverterT<float> atten_;

    OFFSETLYModule() {
        forge_modular::config_OFFSETLY(this);
        configBypass(L::IN_INPUT, L::OUT_OUTPUT);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        (void) e;
        atten_.set_gain(0.f);
        atten_.set_offset(0.f);
    }

    void process(const ProcessArgs& args) override {
        const int ch = forge_modular::channels_OFFSETLY(this);
        outputs[L::OUT_OUTPUT].setChannels(ch);

        const float gain_knob = params[L::GAIN_PARAM].getValue();
        const float offset_volts = params[L::OFFSET_PARAM].getValue();

        float most_positive = 0.f;
        float most_negative = 0.f;

        for (int c = 0; c < ch; ++c) {
            const float in = forge_modular::read_OFFSETLY_IN_INPUT(this, c);
            const float cv = inputs[L::CV_INPUT].getPolyVoltage(c);

            // Gain CV is a full-swing bipolar CV: +/-5 V spans the whole knob range.
            const float gain = std::clamp(gain_knob + cv / V::kCvBipolar, -1.f, 1.f);

            atten_.set_gain(gain);
            atten_.set_offset(offset_volts);

            const float out = std::clamp(atten_.process(in), -12.f, 12.f);
            outputs[L::OUT_OUTPUT].setVoltage(out, c);

            most_positive = std::max(most_positive, out);
            most_negative = std::min(most_negative, out);
        }

        lights[L::POS_LIGHT].setBrightnessSmooth(most_positive / V::kCvBipolar,
                                                args.sampleTime);
        lights[L::NEG_LIGHT].setBrightnessSmooth(-most_negative / V::kCvBipolar,
                                                 args.sampleTime);
    }
};

struct OFFSETLYWidget : rack::app::ModuleWidget {
    explicit OFFSETLYWidget(OFFSETLYModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/OFFSETLY.svg"),
            rack::asset::plugin(pluginInstance, "res/OFFSETLY-dark.svg")));
        forge_modular::place_OFFSETLY(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelOFFSETLY =
    rack::createModel<OFFSETLYModule, OFFSETLYWidget>("OFFSETLY");
