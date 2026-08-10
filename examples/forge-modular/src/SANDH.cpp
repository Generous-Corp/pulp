#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/rng.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct SANDHModule : rack::engine::Module {
    using L = forge_modular::SANDHLayout;

    pulp::signal::SampleHoldT<float> hold_;
    pulp::signal::Xorshift32 noise_{0x1F35D2A7u};
    rack::dsp::SchmittTrigger clock_;

    // Last values pushed into the hold, so the glide coefficients are only
    // recomputed when a control actually moved.
    float glide_ms_ = -1.0f;
    int curve_ = -1;

    SANDHModule() {
        forge_modular::config_SANDH(this);

        // Rack has not told us the real rate yet; 48 kHz is the safe assumption here.
        hold_.prepare(48000.0f);
        hold_.reset(0.0f);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        hold_.prepare(e.sampleRate);
        hold_.reset(0.0f);
        clock_.reset();
        glide_ms_ = -1.0f;
        curve_ = -1;
    }

    void process(const ProcessArgs& args) override {
        const float noise = noise_.next_bipolar() * V::kAudioPeak;

        // Knob and attenuverted CV sum before the taper, so the CV sweeps the
        // same curve the knob does rather than a second, differently shaped one.
        const float cv = inputs[L::SCV_INPUT].getVoltage() / V::kCvBipolar;
        const float amount =
            std::clamp(params[L::SLEW_PARAM].getValue()
                           + params[L::SLEWCV_PARAM].getValue() * cv,
                       0.0f, 1.0f);
        const float ms = 1000.0f * amount * amount;
        if (std::fabs(ms - glide_ms_) > 1.0e-4f) {
            hold_.set_glide_ms(ms);
            glide_ms_ = ms;
        }

        const int curve = params[L::CURVE_PARAM].getValue() > 0.5f ? 1 : 0;
        if (curve != curve_) {
            hold_.set_glide_mode(curve ? pulp::signal::SlewMode::exponential
                                       : pulp::signal::SlewMode::linear);
            curve_ = curve;
        }

        // The signal jack normals to the internal noise: unpatched the module is
        // a random voltage source, patched it samples whatever arrives.
        const float source = inputs[L::IN_INPUT].isConnected()
                                 ? inputs[L::IN_INPUT].getVoltage()
                                 : noise;

        clock_.process(inputs[L::CLK_INPUT].getVoltage(),
                       V::kSchmittLow, V::kSchmittHigh);
        const float out = std::clamp(hold_.process(source, clock_.isHigh()),
                                     -12.0f, 12.0f);

        outputs[L::OUT_OUTPUT].setVoltage(out);
        if (outputs[L::NOISE_OUTPUT].isConnected())
            outputs[L::NOISE_OUTPUT].setVoltage(noise);

        lights[L::HOLD_LIGHT].setBrightnessSmooth(
            std::fabs(out) / V::kAudioPeak, args.sampleTime);
    }
};

struct SANDHWidget : rack::app::ModuleWidget {
    explicit SANDHWidget(SANDHModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/SANDH.svg"),
            rack::asset::plugin(pluginInstance, "res/SANDH-dark.svg")));
        forge_modular::place_SANDH(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelSANDH = rack::createModel<SANDHModule, SANDHWidget>("SANDH");
