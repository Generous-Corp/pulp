#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/drum/kick.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct KICKModule : rack::engine::Module {
    using L = forge_modular::KICKLayout;

    // The swept-oscillator body is the analog kick: an explicit pitch envelope
    // over a sine, so TUNE and SWP mean exactly what the panel says.
    pulp::signal::drum::KickVoice voice_;
    rack::dsp::SchmittTrigger trigger_;

    // The voice mixes to roughly unity at full velocity; the divisor leaves a
    // default hit near +/-5 V with the click layer's peak still inside it.
    static constexpr float kOutputScale = V::kAudioPeak / 1.5f;

    KICKModule() {
        forge_modular::config_KICK(this);

        voice_.set_body(pulp::signal::drum::KickBody::oscillator);
        voice_.set_click_decay_ms(2.0);
        voice_.set_click_tone_hz(4000.0);
        voice_.set_noise_level(0.05);
        voice_.set_noise_decay_ms(40.0);

        // Rack delivers the real rate through onSampleRateChange when the
        // module is added; 48 kHz is the safe assumption until then.
        voice_.prepare(48000.0);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        voice_.prepare(static_cast<double>(e.sampleRate));
        trigger_.reset();
    }

    void process(const ProcessArgs& args) override {
        const float tune_knob = std::clamp(params[L::TUNE_PARAM].getValue(), 30.0f, 120.0f);
        const float decay_ms = std::clamp(params[L::DECAY_PARAM].getValue(), 30.0f, 1200.0f);
        const float click = std::clamp(params[L::CLICK_PARAM].getValue(), 0.0f, 1.0f);
        const float sweep_oct = std::clamp(params[L::SWEEP_PARAM].getValue(), 0.0f, 4.0f);
        const float cv_amount = std::clamp(params[L::TUNECV_PARAM].getValue(), -1.0f, 1.0f);

        // Tune modulation is summed in VOLTS and converted once, so the CV
        // input tracks 1V/oct exactly at full attenuverter.
        const float tune_volts = cv_amount * inputs[L::TUNECV_INPUT].getVoltage();
        const float tune_hz =
            std::clamp(tune_knob * std::exp2(tune_volts), 20.0f, 400.0f);

        voice_.set_tune_hz(tune_hz);
        voice_.set_body_decay_ms(decay_ms);
        voice_.set_click_level(click);
        // A harder beater brightens as well as loudens, which is the click's
        // job; a longer sweep wants a little more time to travel.
        voice_.set_click_tone_hz(2000.0 + 6000.0 * click);
        voice_.set_pitch_sweep_octaves(sweep_oct);
        voice_.set_pitch_sweep_ms(20.0 + 15.0 * sweep_oct);

        if (trigger_.process(inputs[L::TRIG_INPUT].getVoltage(),
                             V::kSchmittLow, V::kSchmittHigh)) {
            const float accent = forge_modular::read_KICK_ACCENT_INPUT(this, 0);
            const float velocity = std::clamp(accent / V::kCvUnipolar, 0.05f, 1.0f);
            voice_.note_on(velocity);
        }

        float sample = 0.0f;
        voice_.process(&sample, 1);  // additive: `sample` starts at silence

        const float out = std::clamp(sample * kOutputScale,
                                     -2.0f * V::kAudioPeak, 2.0f * V::kAudioPeak);
        outputs[L::OUT_OUTPUT].setVoltage(out);
        lights[L::HIT_LIGHT].setBrightnessSmooth(
            std::abs(out) / V::kAudioPeak, args.sampleTime);
    }
};

struct KICKWidget : rack::app::ModuleWidget {
    explicit KICKWidget(KICKModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/KICK.svg"),
            rack::asset::plugin(pluginInstance, "res/KICK-dark.svg")));
        forge_modular::place_KICK(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelKICK = rack::createModel<KICKModule, KICKWidget>("KICK");
