#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/smoothed_value.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct SIXMIXModule : rack::engine::Module {
    using L = forge_modular::SIXMIXLayout;

    static constexpr int kStrips = 6;
    static constexpr float kRampSeconds = 0.01f;

    pulp::signal::SmoothedValue<float> strip_gain_[kStrips];
    pulp::signal::SmoothedValue<float> master_gain_;

    SIXMIXModule() {
        forge_modular::config_SIXMIX(this);
        configBypass(L::IN1_INPUT, L::MIX_OUTPUT);
        prepare(48000.f);
        for (auto& g : strip_gain_) g.set_immediate(1.f);
        master_gain_.set_immediate(1.f);
    }

    void prepare(float sample_rate) {
        for (auto& g : strip_gain_) g.set_ramp_time(kRampSeconds, sample_rate);
        master_gain_.set_ramp_time(kRampSeconds, sample_rate);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        prepare(e.sampleRate);
    }

    void process(const ProcessArgs& args) override {
        float sum = 0.f;
        for (int i = 0; i < kStrips; ++i) {
            strip_gain_[i].set_target(params[L::CH1_PARAM + i].getValue());
            const float gain = strip_gain_[i].next();
            // A poly cable into a mono strip collapses to its channel sum.
            sum += inputs[L::IN1_INPUT + i].getVoltageSum() * gain;
        }

        const float cv = std::clamp(
            forge_modular::read_SIXMIX_MCV_INPUT(this, 0) / V::kCvUnipolar, 0.f, 1.f);
        master_gain_.set_target(params[L::MASTER_PARAM].getValue() * cv);

        const float out = std::clamp(sum * master_gain_.next(),
                                     -2.f * V::kAudioPeak, 2.f * V::kAudioPeak);
        outputs[L::MIX_OUTPUT].setVoltage(out);

        lights[L::LVL_LIGHT].setBrightnessSmooth(
            std::abs(out) / V::kAudioPeak, args.sampleTime);
    }
};

struct SIXMIXWidget : rack::app::ModuleWidget {
    explicit SIXMIXWidget(SIXMIXModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/SIXMIX.svg"),
            rack::asset::plugin(pluginInstance, "res/SIXMIX-dark.svg")));
        forge_modular::place_SIXMIX(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelSIXMIX = rack::createModel<SIXMIXModule, SIXMIXWidget>("SIXMIX");
