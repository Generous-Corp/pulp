#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/trigger.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

constexpr int kMinDivision = 1;
constexpr int kMaxDivision = 16;

struct DIVModule : rack::engine::Module {
    using L = forge_modular::DIVLayout;

    pulp::signal::HystereticTriggerDetectT<float> clock_detect_;
    pulp::signal::HystereticTriggerDetectT<float> reset_detect_;
    pulp::signal::ClockDividerT<float> divider_;
    rack::dsp::PulseGenerator pulse_;

    // Gate mode mirrors the source clock's high phase on the selected beat, so it
    // needs a latch spanning the clock's rising and falling edges. ClockDividerT
    // reports only the divided edge, so the latch is held here.
    bool beat_active_ = false;

    DIVModule() {
        forge_modular::config_DIV(this);
        clock_detect_.set_thresholds(V::kSchmittHigh, V::kSchmittLow);
        reset_detect_.set_thresholds(V::kSchmittHigh, V::kSchmittLow);
        divider_.set_division(2);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        (void)e;
        divider_.reset_phase();
        pulse_.reset();
        beat_active_ = false;
    }

    void process(const ProcessArgs& args) override {
        const float knob = params[L::DIV_PARAM].getValue();
        const float cv = forge_modular::read_DIV_CV_INPUT(this, 0);
        const int division =
            std::clamp(static_cast<int>(std::lround(knob + cv)), kMinDivision, kMaxDivision);
        divider_.set_division(division);

        bool reset_rising = false;
        bool reset_falling = false;
        reset_detect_.process(inputs[L::RST_INPUT].getVoltage(), reset_rising, reset_falling);
        if (reset_rising) {
            divider_.reset_phase();
            beat_active_ = false;
        }

        bool clock_rising = false;
        bool clock_falling = false;
        clock_detect_.process(inputs[L::CLK_INPUT].getVoltage(), clock_rising, clock_falling);

        if (clock_rising && divider_.process(true)) {
            beat_active_ = true;
            pulse_.trigger(1e-3f);
        }
        if (clock_falling) {
            beat_active_ = false;
        }

        const bool trigger_high = pulse_.process(args.sampleTime);
        const bool gate_mode = params[L::MODE_PARAM].getValue() > 0.5f;
        const bool out_high = gate_mode ? beat_active_ : trigger_high;

        outputs[L::OUT_OUTPUT].setVoltage(out_high ? V::kGateHigh : 0.f);
        lights[L::ACT_LIGHT].setBrightnessSmooth(out_high ? 1.f : 0.f, args.sampleTime);
    }
};

struct DIVWidget : rack::app::ModuleWidget {
    explicit DIVWidget(DIVModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/DIV.svg"),
            rack::asset::plugin(pluginInstance, "res/DIV-dark.svg")));
        forge_modular::place_DIV(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelDIV = rack::createModel<DIVModule, DIVWidget>("DIV");
