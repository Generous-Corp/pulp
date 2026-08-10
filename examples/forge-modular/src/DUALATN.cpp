#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/mod_tools.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct DUALATNModule : rack::engine::Module {
    using L = forge_modular::DUALATNLayout;

    // One attenuverter per panel channel. Gain/offset are per-sample settings,
    // so a single instance serves every polyphonic sub-channel.
    pulp::signal::AttenuverterT<float> atten_[2];

    DUALATNModule() {
        forge_modular::config_DUALATN(this);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        (void)e;  // gain + offset are memoryless; nothing depends on the rate
    }

    void process(const ProcessArgs& args) override {
        auto run = [&](int idx, int in_id, int att_id, int off_id, int out_id,
                       int light_id, auto&& read) {
            const int ch = std::max(1, inputs[in_id].getChannels());

            atten_[idx].set_gain(params[att_id].getValue());
            atten_[idx].set_offset(params[off_id].getValue());

            outputs[out_id].setChannels(ch);

            float peak = 0.f;
            for (int c = 0; c < ch; ++c) {
                const float y = std::clamp(atten_[idx].process(read(c)),
                                           -V::kCvUnipolar, V::kCvUnipolar);
                outputs[out_id].setVoltage(y, c);
                peak = std::max(peak, std::abs(y));
            }

            lights[light_id].setBrightnessSmooth(peak / V::kAudioPeak, args.sampleTime);
        };

        run(0, L::A_IN_INPUT, L::A_ATT_PARAM, L::A_OFF_PARAM, L::A_OUT_OUTPUT, L::A_LIGHT,
            [&](int c) { return forge_modular::read_DUALATN_A_IN_INPUT(this, c); });

        run(1, L::B_IN_INPUT, L::B_ATT_PARAM, L::B_OFF_PARAM, L::B_OUT_OUTPUT, L::B_LIGHT,
            [&](int c) { return forge_modular::read_DUALATN_B_IN_INPUT(this, c); });
    }
};

struct DUALATNWidget : rack::app::ModuleWidget {
    explicit DUALATNWidget(DUALATNModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/DUALATN.svg"),
            rack::asset::plugin(pluginInstance, "res/DUALATN-dark.svg")));
        forge_modular::place_DUALATN(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelDUALATN = rack::createModel<DUALATNModule, DUALATNWidget>("DUALATN");
