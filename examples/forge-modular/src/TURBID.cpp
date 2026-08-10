#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/chaos.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct TURBIDModule : rack::engine::Module {
    using L = forge_modular::TURBIDLayout;

    // Two logistic-map cores; B is seeded differently so it decorrelates from A.
    pulp::signal::LogisticMapT<float> chaos_a;
    pulp::signal::LogisticMapT<float> chaos_b;

    // Step interpolation state. The ramp between successive chaos values is plain
    // arithmetic on the running phase (nothing in pulp covers "interpolate the
    // current step by a shape-controlled fraction of its own period").
    float phase = 0.f;
    float prev_a = 0.f, next_a = 0.f;
    float prev_b = 0.f, next_b = 0.f;

    rack::dsp::SchmittTrigger clock_trig;
    rack::dsp::PulseGenerator step_pulse;

    TURBIDModule() {
        forge_modular::config_TURBID(this);

        chaos_a.seed(0.31);
        chaos_b.seed(0.67);
        chaos_a.set_r(3.8);
        chaos_b.set_r(3.8);

        prev_a = static_cast<float>(chaos_a.next_bipolar());
        next_a = static_cast<float>(chaos_a.next_bipolar());
        prev_b = static_cast<float>(chaos_b.next_bipolar());
        next_b = static_cast<float>(chaos_b.next_bipolar());
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        (void)e;
        phase = 0.f;
        clock_trig.reset();
    }

    void reseed_if_degenerate() {
        const double a = chaos_a.current();
        const double b = chaos_b.current();
        if (!(a > 1e-6 && a < 1.0 - 1e-6) || !std::isfinite(a)) chaos_a.seed(0.31);
        if (!(b > 1e-6 && b < 1.0 - 1e-6) || !std::isfinite(b)) chaos_b.seed(0.67);
    }

    void process(const ProcessArgs& args) override {
        const float rate_v = params[L::RATE_PARAM].getValue()
                           + inputs[L::RATE_INPUT].getVoltage();
        const float hz = std::clamp(
            static_cast<float>(V::voct_to_hz(rate_v, V::kLfoRefHz)), 0.02f, 400.f);

        const float character = std::clamp(
            params[L::CHAR_PARAM].getValue()
                + inputs[L::CHAR_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
        const float shape = std::clamp(params[L::SHAPE_PARAM].getValue(), 0.f, 1.f);
        const float spread = std::clamp(params[L::SPREAD_PARAM].getValue(), 0.f, 1.f);
        const float depth = std::clamp(params[L::DEPTH_PARAM].getValue(), 0.f, 1.f);

        // Character sweeps the map from period-doubling order into full chaos.
        const double r = 3.6 + 0.39 * static_cast<double>(character);
        chaos_a.set_r(r);
        chaos_b.set_r(r);

        // Phase always runs at the knob rate: it drives both the internal step
        // clock and the interpolation ramp, so Rate moves the output every sample.
        phase += hz * args.sampleTime;

        bool step = false;
        if (inputs[L::CLK_INPUT].isConnected()) {
            if (clock_trig.process(inputs[L::CLK_INPUT].getVoltage(),
                                   V::kSchmittLow, V::kSchmittHigh)) {
                step = true;
                phase = 0.f;
            }
            if (phase > 1.f) phase = 1.f;
        } else if (phase >= 1.f) {
            phase -= std::floor(phase);
            step = true;
        }

        if (step) {
            reseed_if_degenerate();
            prev_a = next_a;
            prev_b = next_b;
            next_a = static_cast<float>(chaos_a.next_bipolar());
            next_b = static_cast<float>(chaos_b.next_bipolar());
            step_pulse.trigger(1e-3f);
        }

        // Shape: 0 = stepped (jump immediately), 1 = glide across the whole step.
        const float frac = std::clamp(phase, 0.f, 1.f);
        float t = (shape <= 1e-4f) ? 1.f : std::clamp(frac / shape, 0.f, 1.f);
        t = t * t * (3.f - 2.f * t);

        const float a_norm = prev_a + (next_a - prev_a) * t;
        const float b_free = prev_b + (next_b - prev_b) * t;
        // Spread crossfades B from a copy of A toward its own independent chaos.
        const float b_norm = a_norm + spread * (b_free - a_norm);

        const float gain = depth * V::kCvBipolar;
        const float out_a = std::clamp(a_norm * gain, -V::kCvBipolar, V::kCvBipolar);
        const float out_b = std::clamp(b_norm * gain, -V::kCvBipolar, V::kCvBipolar);

        outputs[L::A_OUTPUT].setVoltage(out_a);
        outputs[L::B_OUTPUT].setVoltage(out_b);
        outputs[L::TRIG_OUTPUT].setVoltage(
            step_pulse.process(args.sampleTime) ? V::kGateHigh : 0.f);

        lights[L::A_LIGHT].setBrightnessSmooth(
            std::abs(out_a) / V::kCvBipolar, args.sampleTime);
        lights[L::B_LIGHT].setBrightnessSmooth(
            std::abs(out_b) / V::kCvBipolar, args.sampleTime);
    }
};

struct TURBIDWidget : rack::app::ModuleWidget {
    explicit TURBIDWidget(TURBIDModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/TURBID.svg"),
            rack::asset::plugin(pluginInstance, "res/TURBID-dark.svg")));
        forge_modular::place_TURBID(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelTURBID = rack::createModel<TURBIDModule, TURBIDWidget>("TURBID");
