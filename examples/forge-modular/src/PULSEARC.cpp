#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/osc/phase.hpp>
#include <pulp/signal/trigger.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct PULSEARCModule : rack::engine::Module {
    using L = forge_modular::PULSEARCLayout;

    pulp::signal::osc::PhaseAccumulator phase_;
    pulp::signal::HystereticTriggerDetectT<float> reset_edge_;

    PULSEARCModule() {
        forge_modular::config_PULSEARC(this);
        reset_edge_.set_thresholds(V::kSchmittHigh, V::kSchmittLow);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        reset_edge_.reset();
    }

    void onReset(const ResetEvent& e) override {
        phase_.reset();
        reset_edge_.reset();
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "phase", json_real(phase_.phase()));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* value = json_object_get(root, "phase")) {
            phase_.reset(std::clamp(json_number_value(value), 0.0, 1.0));
        }
    }

    void process(const ProcessArgs& args) override {
        const float rate_cv =
            forge_modular::read_PULSEARC_RATE_CV_INPUT(this, 0);
        const float rate_volts = std::clamp(
            params[L::RATE_PARAM].getValue() + rate_cv, -16.0f, 16.0f);
        const float frequency = std::clamp(
            V::voct_to_hz(rate_volts, V::kLfoRefHz),
            0.001f,
            args.sampleRate * 0.45f);
        const float width = std::clamp(
            params[L::WIDTH_PARAM].getValue(), 0.05f, 0.95f);

        if (reset_edge_.process(inputs[L::RESET_INPUT].getVoltage())) {
            phase_.reset();
        }

        const float phase = static_cast<float>(phase_.phase());
        outputs[L::CLOCK_OUTPUT].setVoltage(
            phase < width ? V::kGateHigh : 0.0f);
        outputs[L::PHASE_OUTPUT].setVoltage(
            phase * V::kCvUnipolar);

        phase_.advance(
            static_cast<double>(frequency) *
            static_cast<double>(args.sampleTime));
    }
};

struct PULSEARCWidget : rack::app::ModuleWidget {
    explicit PULSEARCWidget(PULSEARCModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/PULSEARC.svg"),
            rack::asset::plugin(pluginInstance, "res/PULSEARC-dark.svg")));
        forge_modular::place_PULSEARC(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelPULSEARC =
    rack::createModel<PULSEARCModule, PULSEARCWidget>("PULSEARC");
