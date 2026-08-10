#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/stage_sequencer.hpp>
#include <pulp/signal/trigger.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct STEPSModule : rack::engine::Module {
    using L = forge_modular::STEPSLayout;

    static constexpr int kSteps = 8;

    // The whole sequencer: playhead, direction, per-step voltage and gate.
    pulp::signal::StageSeqT<float> seq_;
    pulp::signal::HystereticTriggerDetectT<float> clock_;
    pulp::signal::HystereticTriggerDetectT<float> reset_;
    rack::dsp::PulseGenerator eoc_;
    int prev_stage_ = -1;

    STEPSModule() {
        forge_modular::config_STEPS(this);
        clock_.set_thresholds(V::kSchmittHigh, V::kSchmittLow);
        reset_.set_thresholds(V::kSchmittHigh, V::kSchmittLow);
        // One clock per step, and a gate that occupies the GATE knob's share of
        // the measured clock period -- which is what `repeat` mode means.
        for (int s = 0; s < kSteps; ++s) {
            seq_.set_stage_pulse_count(s, 1);
            seq_.set_stage_gate_mode(s, pulp::signal::StageGateMode::repeat);
        }
        // APP is off limits in a constructor; the real rate arrives below.
        seq_.prepare(48000.0);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        seq_.prepare(e.sampleRate);
        seq_.reset();
        prev_stage_ = -1;
        eoc_.reset();
    }

    void process(const ProcessArgs& args) override {
        for (int s = 0; s < kSteps; ++s)
            seq_.set_stage_pitch(s, params[L::STEP1_PARAM + s].getValue());

        // Length in whole steps: knob plus 1 V per step of CV, so a ramp or a
        // stepped voltage can shorten the pattern on the fly.
        const float len_cv = forge_modular::read_STEPS_LEN_INPUT(this, 0);
        const int length = std::clamp(
            static_cast<int>(std::lround(params[L::LENGTH_PARAM].getValue() + len_cv)),
            1, kSteps);
        seq_.set_num_stages(length);

        static constexpr pulp::signal::SeqDirection kDirs[3] = {
            pulp::signal::SeqDirection::forward,
            pulp::signal::SeqDirection::reverse,
            pulp::signal::SeqDirection::pingpong,
        };
        const int dir = std::clamp(
            static_cast<int>(params[L::DIR_PARAM].getValue()), 0, 2);
        seq_.set_direction(kDirs[dir]);

        seq_.set_repeat_duty(params[L::GATE_PARAM].getValue());

        bool clock_edge = false, clock_fell = false;
        clock_.process(inputs[L::CLK_INPUT].getVoltage(), clock_edge, clock_fell);
        bool reset_edge = false, reset_fell = false;
        reset_.process(inputs[L::RST_INPUT].getVoltage(), reset_edge, reset_fell);
        if (reset_edge) prev_stage_ = -1;

        const auto frame = seq_.process(/*run_high=*/true, reset_edge, clock_edge);

        // End of cycle: the clock that lands the playhead back on step 1. A
        // one-step pattern wraps on every clock, which no index change reports.
        if (clock_edge && seq_.started()) {
            const int s = seq_.stage();
            if (s == 0 && (prev_stage_ != 0 || length == 1)) eoc_.trigger(1e-3f);
            prev_stage_ = s;
        }

        outputs[L::CV_OUTPUT].setVoltage(
            std::clamp(frame.pitch_v, -V::kCvBipolar, V::kCvBipolar));
        outputs[L::GATE_OUTPUT].setVoltage(frame.gate ? V::kGateHigh : 0.f);
        outputs[L::EOC_OUTPUT].setVoltage(
            eoc_.process(args.sampleTime) ? V::kGateHigh : 0.f);

        const int here = seq_.started() ? seq_.stage() : -1;
        for (int s = 0; s < kSteps; ++s)
            lights[L::STEP1_LIGHT + s].setBrightnessSmooth(
                (s == here) ? 1.f : (s < length ? 0.05f : 0.f), args.sampleTime);
    }
};

struct STEPSWidget : rack::app::ModuleWidget {
    explicit STEPSWidget(STEPSModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/STEPS.svg"),
            rack::asset::plugin(pluginInstance, "res/STEPS-dark.svg")));
        forge_modular::place_STEPS(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelSTEPS = rack::createModel<STEPSModule, STEPSWidget>("STEPS");
