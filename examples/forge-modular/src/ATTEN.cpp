#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/smoothed_value.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct ATTENModule : rack::engine::Module {
    using L = forge_modular::ATTENLayout;

    pulp::signal::AttenuverterT<float> atten_;
    pulp::signal::SmoothedValue<float> amount_;

    ATTENModule() {
        forge_modular::config_ATTEN(this);
        configBypass(L::IN_INPUT, L::OUT_OUTPUT);

        // Rack has not told us the real rate yet; 48 kHz is the safe assumption here.
        amount_.set_ramp_time(0.005f, 48000.0f);
        amount_.set_immediate(params[L::ATT_PARAM].getValue());
        atten_.set_offset(0.0f);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        amount_.set_ramp_time(0.005f, e.sampleRate);
        amount_.set_immediate(std::clamp(params[L::ATT_PARAM].getValue(), 0.0f, 1.0f));
    }

    void process(const ProcessArgs& args) override {
        amount_.set_target(std::clamp(params[L::ATT_PARAM].getValue(), 0.0f, 1.0f));
        atten_.set_gain(amount_.next());

        const int ch = forge_modular::channels_ATTEN(this);
        outputs[L::OUT_OUTPUT].setChannels(ch);

        float peak = 0.0f;
        for (int c = 0; c < ch; ++c) {
            const float in = forge_modular::read_ATTEN_IN_INPUT(this, c);
            const float out = std::clamp(atten_.process(in), -12.0f, 12.0f);
            outputs[L::OUT_OUTPUT].setVoltage(out, c);
            peak = std::max(peak, std::abs(out));
        }

        lights[L::LVL_LIGHT].setBrightnessSmooth(peak / V::kAudioPeak, args.sampleTime);
    }
};

struct ATTENWidget : rack::app::ModuleWidget {
    explicit ATTENWidget(ATTENModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/ATTEN.svg"),
            rack::asset::plugin(pluginInstance, "res/ATTEN-dark.svg")));
        forge_modular::place_ATTEN(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelATTEN = rack::createModel<ATTENModule, ATTENWidget>("ATTEN");
