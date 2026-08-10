#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/chaos.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct STRANGEModule : rack::engine::Module {
    using L = forge_modular::STRANGELayout;

    pulp::signal::LogisticMapT<float> map_a_;
    pulp::signal::LogisticMapT<float> map_b_;

    // Clock phase for the chaotic step. The output is read continuously from this
    // phase, so the rate control moves the signal every sample, not only on a step.
    double phase_ = 0.0;

    float prev_a_ = 0.0f;
    float target_a_ = 0.0f;
    float prev_b_ = 0.0f;
    float target_b_ = 0.0f;

    rack::dsp::PulseGenerator step_pulse_;

    STRANGEModule() {
        forge_modular::config_STRANGE(this);
        reset_chaos();
    }

    void reset_chaos() {
        phase_ = 0.0;
        map_a_.seed(0.311f);
        map_b_.seed(0.727f);
        prev_a_ = 0.0f;
        prev_b_ = 0.0f;
        target_a_ = 0.0f;
        target_b_ = 0.0f;
        advance_state(0.5f);
        step_pulse_.reset();
    }

    void advance_state(float character) {
        const float r = 3.4f + 0.6f * character;
        map_a_.set_r(r);
        map_b_.set_r(r);

        prev_a_ = target_a_;
        prev_b_ = target_b_;

        const float a = std::clamp(static_cast<float>(map_a_.next_bipolar()), -1.0f, 1.0f);
        const float b = std::clamp(static_cast<float>(map_b_.next_bipolar()), -1.0f, 1.0f);
        target_a_ = a * V::kCvBipolar;
        target_b_ = b * V::kCvBipolar;

        step_pulse_.trigger(1e-3f);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        (void)e;
        reset_chaos();
    }

    void process(const ProcessArgs& args) override {
        // Every control sums knob + CV — nothing overrides the knob, so each stays live.
        const float rate_volts =
            params[L::RATE_PARAM].getValue() + inputs[L::RATE_INPUT].getVoltage();
        const float character = std::clamp(
            params[L::CHAR_PARAM].getValue() + inputs[L::CHAR_INPUT].getVoltage() * 0.1f,
            0.0f, 1.0f);
        const float glide = std::clamp(
            params[L::GLIDE_PARAM].getValue() + inputs[L::GLIDE_INPUT].getVoltage() * 0.1f,
            0.0f, 1.0f);
        const float link = std::clamp(
            params[L::LINK_PARAM].getValue() + inputs[L::LINK_INPUT].getVoltage() * 0.1f,
            0.0f, 1.0f);

        const double max_hz = std::min(2000.0, static_cast<double>(args.sampleRate) * 0.25);
        const double hz = std::clamp(
            static_cast<double>(V::voct_to_hz(rate_volts, V::kLfoRefHz)), 0.01, max_hz);

        phase_ += hz * static_cast<double>(args.sampleTime);
        while (phase_ >= 1.0) {
            phase_ -= 1.0;
            advance_state(character);
        }

        // Glide is period-relative shaped interpolation between the last two chaos
        // values — no Pulp primitive slews in units of the clock period, so it is inline.
        const float span = std::max(glide, 1.0e-3f);
        float t = std::min(static_cast<float>(phase_) / span, 1.0f);
        t = t * t * (3.0f - 2.0f * t);

        const float step_a = prev_a_ + (target_a_ - prev_a_) * t;
        const float step_b = prev_b_ + (target_b_ - prev_b_) * t;

        const float out_a = std::clamp(step_a, -V::kCvBipolar, V::kCvBipolar);
        const float out_b = std::clamp(step_b + (step_a - step_b) * link,
                                       -V::kCvBipolar, V::kCvBipolar);

        outputs[L::A_OUTPUT].setVoltage(out_a);
        outputs[L::B_OUTPUT].setVoltage(out_b);

        const bool step_high = step_pulse_.process(args.sampleTime);
        outputs[L::STEP_OUTPUT].setVoltage(step_high ? V::kGateHigh : 0.0f);

        lights[L::A_LIGHT].setBrightnessSmooth(std::fabs(out_a) / V::kCvBipolar,
                                              args.sampleTime);
        lights[L::B_LIGHT].setBrightnessSmooth(std::fabs(out_b) / V::kCvBipolar,
                                              args.sampleTime);
        lights[L::STEP_LIGHT].setBrightnessSmooth(step_high ? 1.0f : 0.0f, args.sampleTime);
    }
};

struct STRANGEWidget : rack::app::ModuleWidget {
    explicit STRANGEWidget(STRANGEModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/STRANGE.svg"),
            rack::asset::plugin(pluginInstance, "res/STRANGE-dark.svg")));
        forge_modular::place_STRANGE(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelSTRANGE = rack::createModel<STRANGEModule, STRANGEWidget>("STRANGE");
