#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/trigger.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

// A four-corner morph (sine -> triangle -> ramp -> square) needs a single phase
// shared by all four evaluators plus a hard write-to-zero on reset; the library
// phase accumulators advance but do not expose a phase write, so the phase is
// kept inline here.
float wave_at(int which, double p) {
    switch (which) {
        case 0: return std::sin(2.0f * static_cast<float>(M_PI) * static_cast<float>(p));
        case 1: return 1.0f - 4.0f * std::fabs(static_cast<float>(p) - 0.5f);
        case 2: return 2.0f * static_cast<float>(p) - 1.0f;
        default: return p < 0.5 ? -1.0f : 1.0f;
    }
}

struct MORPHLFOModule : rack::engine::Module {
    using L = forge_modular::MORPHLFOLayout;

    double phase_ = 0.0;
    pulp::signal::HystereticTriggerDetectT<float> reset_trig_;

    MORPHLFOModule() {
        forge_modular::config_MORPHLFO(this);
        reset_trig_.set_thresholds(V::kSchmittHigh, V::kSchmittLow);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        (void)e;
        phase_ = 0.0;
        reset_trig_.set_thresholds(V::kSchmittHigh, V::kSchmittLow);
    }

    void process(const ProcessArgs& args) override {
        bool rising = false;
        bool falling = false;
        reset_trig_.process(inputs[L::RST_INPUT].getVoltage(), rising, falling);
        if (rising) phase_ = 0.0;

        // Both CV jacks normal to +5 V, so the attenuverters act as offsets unpatched.
        const float rate_cv = forge_modular::read_MORPHLFO_RATE_CV_INPUT(this, 0);
        const float shape_cv = forge_modular::read_MORPHLFO_SHAPE_CV_INPUT(this, 0);

        // Sum in volts, convert once.
        const float rate_volts = params[L::RATE_PARAM].getValue()
                               + params[L::RATE_CV_PARAM].getValue() * rate_cv;
        float hz = V::voct_to_hz(rate_volts, V::kLfoRefHz);
        hz = std::clamp(hz, 0.01f, 100.0f);

        phase_ += static_cast<double>(hz) * static_cast<double>(args.sampleTime);
        if (phase_ >= 1.0) phase_ -= std::floor(phase_);

        // ±5 V of shape CV sweeps the full morph range at unity attenuverter.
        float shape = params[L::SHAPE_PARAM].getValue()
                    + params[L::SHAPE_CV_PARAM].getValue() * shape_cv * 0.1f;
        shape = std::clamp(shape, 0.0f, 1.0f);

        const float pos = shape * 3.0f;
        const int idx = std::min(2, static_cast<int>(pos));
        const float frac = pos - static_cast<float>(idx);
        const float a = wave_at(idx, phase_);
        const float b = wave_at(idx + 1, phase_);
        const float y = std::clamp(a + (b - a) * frac, -1.0f, 1.0f);

        outputs[L::OUT_OUTPUT].setVoltage(y * V::kCvBipolar);
        outputs[L::UNI_OUTPUT].setVoltage((y * 0.5f + 0.5f) * V::kCvUnipolar);

        lights[L::LFO_LIGHT].setBrightnessSmooth(y * 0.5f + 0.5f, args.sampleTime);
    }
};

struct MORPHLFOWidget : rack::app::ModuleWidget {
    explicit MORPHLFOWidget(MORPHLFOModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/MORPHLFO.svg"),
            rack::asset::plugin(pluginInstance, "res/MORPHLFO-dark.svg")));
        forge_modular::place_MORPHLFO(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelMORPHLFO =
    rack::createModel<MORPHLFOModule, MORPHLFOWidget>("MORPHLFO");
