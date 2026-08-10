#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/envelope.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct DUALADModule : rack::engine::Module {
    using L = forge_modular::DUALADLayout;

    static constexpr int kChannels = 2;

    // Time controls live in log2 seconds so the knob taper, the 1V/oct CV and
    // the tooltip readout are all the same exponential axis. Clamp the summed
    // exponent rather than the milliseconds, so CV cannot drive a stage to zero.
    static constexpr float kMinExp2 = -11.0f;  // ~0.5 ms
    static constexpr float kMaxExp2 = 4.0f;    // 16 s

    pulp::signal::AdT<float> env_[kChannels];
    rack::dsp::SchmittTrigger trigger_[kChannels];
    rack::dsp::PulseGenerator eoc_[kChannels];
    pulp::signal::EnvelopeStage stage_[kChannels] = {
        pulp::signal::EnvelopeStage::idle, pulp::signal::EnvelopeStage::idle};

    // Last values pushed into each engine, so stage coefficients are only
    // recomputed when a control actually moved. Negative forces a refresh.
    float attack_ms_[kChannels] = {-1.0f, -1.0f};
    float decay_ms_[kChannels] = {-1.0f, -1.0f};
    float curve_[kChannels] = {-1.0f, -1.0f};
    int loop_[kChannels] = {-1, -1};

    DUALADModule() {
        forge_modular::config_DUALAD(this);

        // Rack has not told us the real rate yet; 48 kHz is the safe assumption here.
        for (int i = 0; i < kChannels; ++i) env_[i].prepare(48000.0);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        for (int i = 0; i < kChannels; ++i) {
            env_[i].prepare(e.sampleRate);
            env_[i].reset();
            trigger_[i].reset();
            eoc_[i].reset();
            stage_[i] = pulp::signal::EnvelopeStage::idle;
            attack_ms_[i] = -1.0f;
            decay_ms_[i] = -1.0f;
            curve_[i] = -1.0f;
            loop_[i] = -1;
        }
    }

    void process(const ProcessArgs& args) override {
        static constexpr int kAtk[kChannels] = {L::A_ATK_PARAM, L::B_ATK_PARAM};
        static constexpr int kDec[kChannels] = {L::A_DEC_PARAM, L::B_DEC_PARAM};
        static constexpr int kCrv[kChannels] = {L::A_CURVE_PARAM, L::B_CURVE_PARAM};
        static constexpr int kLoopParam[kChannels] = {L::A_LOOP_PARAM, L::B_LOOP_PARAM};
        static constexpr int kEnvOut[kChannels] = {L::A_ENV_OUTPUT, L::B_ENV_OUTPUT};
        static constexpr int kEocOut[kChannels] = {L::A_EOC_OUTPUT, L::B_EOC_OUTPUT};
        static constexpr int kLight[kChannels] = {L::A_LIGHT, L::B_LIGHT};

        for (int i = 0; i < kChannels; ++i) {
            // Channel B's jacks normal to channel A's, so one trigger and one
            // time CV drive both halves until B is patched on its own.
            const float trig_v =
                (i == 0) ? inputs[L::A_TRIG_INPUT].getVoltage()
                         : forge_modular::read_DUALAD_B_TRIG_INPUT(this, 0);
            const float cv =
                (i == 0) ? inputs[L::A_TIME_CV_INPUT].getVoltage()
                         : forge_modular::read_DUALAD_B_TIME_CV_INPUT(this, 0);

            // CV sums with the knob in the exponent, so a volt halves or doubles
            // both stages together and the shape of the envelope is preserved.
            const float atk_exp =
                std::clamp(params[kAtk[i]].getValue() + cv, kMinExp2, kMaxExp2);
            const float dec_exp =
                std::clamp(params[kDec[i]].getValue() + cv, kMinExp2, kMaxExp2);

            const float attack_ms = 1000.0f * std::exp2(atk_exp);
            if (std::fabs(attack_ms - attack_ms_[i]) > 1.0e-4f) {
                env_[i].set_attack_ms(attack_ms);
                attack_ms_[i] = attack_ms;
            }

            const float decay_ms = 1000.0f * std::exp2(dec_exp);
            if (std::fabs(decay_ms - decay_ms_[i]) > 1.0e-4f) {
                env_[i].set_decay_ms(decay_ms);
                decay_ms_[i] = decay_ms;
            }

            // One knob drives both stages: concave attack against convex decay,
            // which is the linear-to-snappy axis the engine's set_curve() maps.
            const float curve = std::clamp(params[kCrv[i]].getValue(), 0.0f, 1.0f);
            if (std::fabs(curve - curve_[i]) > 1.0e-4f) {
                env_[i].set_curve(curve);
                curve_[i] = curve;
            }

            const int loop = params[kLoopParam[i]].getValue() > 0.5f ? 1 : 0;
            if (loop != loop_[i]) {
                env_[i].set_loop(loop != 0, 0);
                loop_[i] = loop;
            }

            if (trigger_[i].process(trig_v, V::kSchmittLow, V::kSchmittHigh)) {
                env_[i].trigger();
            } else if (loop != 0 && !env_[i].active()) {
                // Looping still needs a first cycle to start it, otherwise the
                // switch would sit silent until something happened to trigger.
                env_[i].trigger();
            }

            const float level = std::clamp(env_[i].next(), 0.0f, 1.0f);

            // End of cycle fires when decay finishes, whether it lands in idle
            // (one-shot) or wraps straight back into attack (looping).
            const pulp::signal::EnvelopeStage stage = env_[i].stage();
            if (stage_[i] == pulp::signal::EnvelopeStage::decay
                && stage != pulp::signal::EnvelopeStage::decay) {
                eoc_[i].trigger(1.0e-3f);
            }
            stage_[i] = stage;

            outputs[kEnvOut[i]].setVoltage(level * V::kCvUnipolar);
            outputs[kEocOut[i]].setVoltage(
                eoc_[i].process(args.sampleTime) ? V::kGateHigh : 0.0f);
            lights[kLight[i]].setBrightnessSmooth(level, args.sampleTime);
        }
    }
};

struct DUALADWidget : rack::app::ModuleWidget {
    explicit DUALADWidget(DUALADModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/DUALAD.svg"),
            rack::asset::plugin(pluginInstance, "res/DUALAD-dark.svg")));
        forge_modular::place_DUALAD(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelDUALAD = rack::createModel<DUALADModule, DUALADWidget>("DUALAD");
