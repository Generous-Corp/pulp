#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/trigger.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct DIVIDELYModule : rack::engine::Module {
    using L = forge_modular::DIVIDELYLayout;

    static constexpr int kLanes = 4;

    pulp::signal::HystereticTriggerDetectT<float> clock_detect_;
    pulp::signal::HystereticTriggerDetectT<float> reset_detect_;
    pulp::signal::ClockDividerT<float> divider_[kLanes];
    int division_[kLanes] = {2, 4, 8, 16};
    bool armed_[kLanes] = {false, false, false, false};

    DIVIDELYModule() {
        forge_modular::config_DIVIDELY(this);
        clock_detect_.set_thresholds(V::kSchmittHigh, V::kSchmittLow);
        reset_detect_.set_thresholds(V::kSchmittHigh, V::kSchmittLow);
        for (int i = 0; i < kLanes; ++i) {
            divider_[i].set_division(division_[i]);
        }
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        (void)e;
        for (int i = 0; i < kLanes; ++i) {
            divider_[i].reset_phase();
            armed_[i] = false;
        }
    }

    void process(const ProcessArgs& args) override {
        static constexpr int param_id[kLanes] = {
            L::DIV1_PARAM, L::DIV2_PARAM, L::DIV3_PARAM, L::DIV4_PARAM};
        static constexpr int output_id[kLanes] = {
            L::OUT1_OUTPUT, L::OUT2_OUTPUT, L::OUT3_OUTPUT, L::OUT4_OUTPUT};
        static constexpr int light_id[kLanes] = {
            L::DIV1_LIGHT, L::DIV2_LIGHT, L::DIV3_LIGHT, L::DIV4_LIGHT};

        bool reset_rising = false;
        bool reset_falling = false;
        reset_detect_.process(inputs[L::RST_INPUT].getVoltage(), reset_rising, reset_falling);

        bool clock_rising = false;
        bool clock_falling = false;
        clock_detect_.process(inputs[L::CLK_INPUT].getVoltage(), clock_rising, clock_falling);
        const bool clock_high = clock_detect_.high();

        for (int i = 0; i < kLanes; ++i) {
            const int wanted = std::clamp(
                static_cast<int>(std::lround(params[param_id[i]].getValue())), 1, 32);
            if (wanted != division_[i]) {
                division_[i] = wanted;
                divider_[i].set_division(wanted);
            }

            if (reset_rising) {
                divider_[i].reset_phase();
                armed_[i] = false;
            }

            if (clock_rising && divider_[i].process(true)) {
                armed_[i] = true;
            }
            if (clock_falling) {
                armed_[i] = false;
            }

            const bool gate = armed_[i] && clock_high;
            outputs[output_id[i]].setVoltage(gate ? V::kGateHigh : 0.f);
            lights[light_id[i]].setBrightnessSmooth(gate ? 1.f : 0.f, args.sampleTime);
        }
    }
};

struct DIVIDELYWidget : rack::app::ModuleWidget {
    explicit DIVIDELYWidget(DIVIDELYModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/DIVIDELY.svg"),
            rack::asset::plugin(pluginInstance, "res/DIVIDELY-dark.svg")));
        forge_modular::place_DIVIDELY(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelDIVIDELY =
    rack::createModel<DIVIDELYModule, DIVIDELYWidget>("DIVIDELY");
