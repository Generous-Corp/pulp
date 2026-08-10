#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/osc/phase.hpp>
#include <pulp/signal/trigger.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct CLOCKWEAVEModule : rack::engine::Module {
    using L = forge_modular::CLOCKWEAVELayout;

    pulp::signal::osc::PhaseAccumulator phase_;
    pulp::signal::HystereticTriggerDetectT<float> reset_edge_;

    CLOCKWEAVEModule() {
        forge_modular::config_CLOCKWEAVE(this);
        reset_edge_.set_thresholds(V::kSchmittHigh, V::kSchmittLow);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        phase_.reset();
        reset_edge_.reset();
        reset_edge_.set_thresholds(V::kSchmittHigh, V::kSchmittLow);
    }

    void process(const ProcessArgs& args) override {
        const float rate_octaves = params[L::RATE_PARAM].getValue();
        const float width =
            std::clamp(params[L::WIDTH_PARAM].getValue(), 0.05f, 0.95f);
        const float rate_cv =
            forge_modular::read_CLOCKWEAVE_RATE_CV_INPUT(this, 0);
        const float reset_voltage =
            forge_modular::read_CLOCKWEAVE_RESET_INPUT(this, 0);

        const float frequency = std::clamp(
            V::voct_to_hz(rate_octaves + rate_cv, V::kLfoRefHz),
            0.f,
            args.sampleRate * 0.45f);

        if (reset_edge_.process(reset_voltage)) {
            phase_.reset(0.0);
        } else {
            phase_.advance(
                static_cast<double>(frequency) /
                static_cast<double>(args.sampleRate));
        }

        const float phase = static_cast<float>(phase_.phase());

        outputs[L::CLOCK_OUTPUT].setVoltage(
            phase < width ? V::kGateHigh : 0.f);
        outputs[L::PHASE_OUTPUT].setVoltage(
            phase * V::kCvUnipolar);
    }
};

struct CLOCKWEAVEWidget : rack::app::ModuleWidget {
    explicit CLOCKWEAVEWidget(CLOCKWEAVEModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/CLOCKWEAVE.svg"),
            rack::asset::plugin(pluginInstance, "res/CLOCKWEAVE-dark.svg")));
        forge_modular::place_CLOCKWEAVE(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelCLOCKWEAVE =
    rack::createModel<CLOCKWEAVEModule, CLOCKWEAVEWidget>("CLOCKWEAVE");
