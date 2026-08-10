#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/ladder_filter.hpp>
#include <pulp/signal/vactrol.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct LPGATEModule : rack::engine::Module {
    using L = forge_modular::LPGATELayout;
    using Vactrol = pulp::signal::VactrolConditionerT<float>;
    using Ladder = pulp::signal::LadderFilterT<float>;

    static constexpr int kMaxChannels = 16;

    // The ladder saturates around unity; Rack audio is +/-5 V.
    static constexpr float kToUnity = 1.0f / V::kAudioPeak;

    // Above 1 the gate spends more of its travel near silence, which is what
    // gives a vactrol its long quiet tail instead of a linear fade.
    static constexpr double kGainExponent = 1.5;

    static constexpr double kClosedHz = 40.0;
    static constexpr double kOpenHz = 12000.0;
    static constexpr double kRiseMs = 2.0;

    // The vactrol law and the power-law gain come from VactrolConditionerT;
    // the sweeping filter is a LadderFilterT rather than LowpassGateT because
    // that gate's internal filter is a non-resonant one-pole and this module
    // needs resonance.
    std::array<Vactrol, kMaxChannels> vactrol_{};
    std::array<Ladder, kMaxChannels> filter_{};
    std::array<rack::dsp::SchmittTrigger, kMaxChannels> strike_{};
    std::array<rack::dsp::PulseGenerator, kMaxChannels> ping_{};
    std::array<double, kMaxChannels> fall_ms_{};

    double open_limit_ = kOpenHz;

    LPGATEModule() {
        forge_modular::config_LPGATE(this);
        configBypass(L::IN_INPUT, L::OUT_OUTPUT);

        // Rack delivers the real rate through onSampleRateChange when the
        // module is added; 48 kHz is the safe assumption until then.
        prepare(48000.0f);
    }

    void prepare(float sample_rate) {
        open_limit_ = std::clamp(kOpenHz, 20.0,
                                0.45 * static_cast<double>(sample_rate));
        for (int c = 0; c < kMaxChannels; ++c) {
            vactrol_[c].prepare(sample_rate);
            vactrol_[c].set_rise_ms(kRiseMs);
            vactrol_[c].reset();
            filter_[c].set_sample_rate(sample_rate);
            filter_[c].reset();
            strike_[c].reset();
            ping_[c].reset();
            fall_ms_[c] = -1.0;  // force a coefficient update on the next sample
        }
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        prepare(e.sampleRate);
    }

    void process(const ProcessArgs& args) override {
        const float level = std::clamp(params[L::LEVEL_PARAM].getValue(), 0.0f, 1.0f);
        const float decay_knob = std::clamp(params[L::DECAY_PARAM].getValue(), -1.0f, 1.0f);
        const float res = std::clamp(params[L::RES_PARAM].getValue(), 0.0f, 1.0f);
        const double colour = std::clamp(params[L::COLOR_PARAM].getValue(), 0.0f, 1.0f);
        const float dcv_amount = std::clamp(params[L::DCVAMT_PARAM].getValue(), -1.0f, 1.0f);

        const int ch = forge_modular::channels_LPGATE(this);
        outputs[L::OUT_OUTPUT].setChannels(ch);

        const bool wanted = outputs[L::OUT_OUTPUT].isConnected();
        double control_0 = 0.0;

        for (int c = 0; c < ch; ++c) {
            // Decay is summed as an exponent so a volt is a fixed ratio of time.
            const float dcv = forge_modular::read_LPGATE_DCV_INPUT(this, c);
            const float exponent =
                std::clamp(decay_knob + dcv_amount * dcv * 0.2f, -1.0f, 1.0f);
            const double fall = 200.0 * std::pow(10.0, static_cast<double>(exponent));
            // Two exponentials per update, so only re-derive on a real change.
            if (std::abs(fall - fall_ms_[c]) > 0.005 * fall) {
                vactrol_[c].set_fall_ms(fall);
                fall_ms_[c] = fall;
            }

            const float gate = forge_modular::read_LPGATE_GATE_INPUT(this, c);
            double target = std::clamp(static_cast<double>(level) +
                                           static_cast<double>(gate) / V::kCvUnipolar,
                                       0.0, 1.0);

            if (strike_[c].process(inputs[L::STRIKE_INPUT].getPolyVoltage(c),
                                   V::kSchmittLow, V::kSchmittHigh))
                ping_[c].trigger(1e-3f);
            if (ping_[c].process(args.sampleTime)) target = 1.0;

            const double control = vactrol_[c].process(target);
            if (c == 0) control_0 = control;

            if (!wanted) continue;

            const double vca = std::pow(control, kGainExponent);
            const double in =
                static_cast<double>(inputs[L::IN_INPUT].getPolyVoltage(c)) * kToUnity;

            // The amplitude term carries the whole gate at colour 0 and is
            // bypassed at colour 1; the filter blend below does the opposite.
            double x = in * (1.0 - (1.0 - colour) * (1.0 - vca));

            // The corner sweeps geometrically, because brightness is heard
            // geometrically: a linear sweep sounds like it does nothing.
            const double hz = std::clamp(
                kClosedHz * std::pow(open_limit_ / kClosedHz, control),
                20.0, open_limit_);
            Ladder& f = filter_[c];
            f.set_frequency(static_cast<float>(hz));
            f.set_resonance(res);
            const double filtered = f.process(static_cast<float>(x));

            x = x + colour * (filtered - x);
            const float out = std::clamp(static_cast<float>(x) * V::kAudioPeak,
                                         -2.0f * V::kAudioPeak, 2.0f * V::kAudioPeak);
            outputs[L::OUT_OUTPUT].setVoltage(out, c);
        }

        lights[L::GATE_LIGHT].setBrightnessSmooth(static_cast<float>(control_0),
                                                 args.sampleTime);
    }
};

struct LPGATEWidget : rack::app::ModuleWidget {
    explicit LPGATEWidget(LPGATEModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/LPGATE.svg"),
            rack::asset::plugin(pluginInstance, "res/LPGATE-dark.svg")));
        forge_modular::place_LPGATE(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelLPGATE = rack::createModel<LPGATEModule, LPGATEWidget>("LPGATE");
