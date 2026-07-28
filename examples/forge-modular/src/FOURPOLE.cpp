#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/analog_vcf.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct FOURPOLEModule : rack::engine::Module {
    using L = forge_modular::FOURPOLELayout;
    using Vcf = pulp::signal::AnalogVcfT<float>;

    static constexpr int kMaxChannels = 16;

    // The voicing laws are unity-referenced; Rack audio is +/-5 V.
    static constexpr float kToUnity = 1.0f / V::kAudioPeak;

    // 2x oversampling keeps the drive stage's harmonics below Nyquist. Its
    // half-band filter is linear phase, so the path carries a fixed delay
    // (latency_samples()) that Rack does not compensate.
    static constexpr int kOversampling = 2;

    std::array<Vcf, kMaxChannels> filters_{};
    int voicing_index_ = -1;

    FOURPOLEModule() {
        forge_modular::config_FOURPOLE(this);
        configBypass(L::IN_INPUT, L::OUT_OUTPUT);

        // Rack delivers the real rate through onSampleRateChange when the
        // module is added; 48 kHz is the safe assumption until then.
        prepare(48000.0f);
    }

    void prepare(float sample_rate) {
        for (auto& f : filters_) {
            f.set_sample_rate(sample_rate);
            f.set_oversampling(kOversampling);
            f.set_smoothing_time_ms(1.0);
        }
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        prepare(e.sampleRate);
    }

    static Vcf::Voicing voicing_for(int index) {
        switch (index) {
            case 1: return Vcf::Voicing::prophet5;
            case 2: return Vcf::Voicing::minimoog;
            default: return Vcf::Voicing::juno;
        }
    }

    void process(const ProcessArgs& args) override {
        const int voice = std::clamp(
            static_cast<int>(std::lround(params[L::VOICE_PARAM].getValue())), 0, 2);
        if (voice != voicing_index_) {
            voicing_index_ = voice;
            const Vcf::Voicing v = voicing_for(voice);
            for (auto& f : filters_) f.set_voicing(v);
        }

        const float cutoff_knob = std::clamp(params[L::CUTOFF_PARAM].getValue(), 0.0f, 1.0f);
        const float res_knob = std::clamp(params[L::RES_PARAM].getValue(), 0.0f, 1.0f);
        const float drive_db = std::clamp(params[L::DRIVE_PARAM].getValue(), -6.0f, 24.0f);
        const float fm_amount = std::clamp(params[L::FM_PARAM].getValue(), -1.0f, 1.0f);

        const int ch = forge_modular::channels_FOURPOLE(this);
        outputs[L::OUT_OUTPUT].setChannels(ch);

        const bool wanted = outputs[L::OUT_OUTPUT].isConnected();
        float peak = 0.0f;

        for (int c = 0; c < ch; ++c) {
            // Cutoff modulation is summed in VOLTS and handed over as octaves,
            // so V/OCT tracks exactly and FM adds on top of it.
            const float octaves =
                inputs[L::VOCT_INPUT].getPolyVoltage(c) +
                fm_amount * inputs[L::FM_INPUT].getPolyVoltage(c);

            Vcf& f = filters_[c];
            f.set_parameters(cutoff_knob, octaves, res_knob, drive_db);

            if (!wanted) continue;

            const float in = inputs[L::IN_INPUT].getPolyVoltage(c) * kToUnity;
            const float out = std::clamp(f.process(in) * V::kAudioPeak,
                                         -2.0f * V::kAudioPeak, 2.0f * V::kAudioPeak);
            outputs[L::OUT_OUTPUT].setVoltage(out, c);
            peak = std::max(peak, std::abs(out));
        }

        lights[L::LVL_LIGHT].setBrightnessSmooth(peak / V::kAudioPeak, args.sampleTime);
    }
};

struct FOURPOLEWidget : rack::app::ModuleWidget {
    explicit FOURPOLEWidget(FOURPOLEModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/FOURPOLE.svg"),
            rack::asset::plugin(pluginInstance, "res/FOURPOLE-dark.svg")));
        forge_modular::place_FOURPOLE(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelFOURPOLE = rack::createModel<FOURPOLEModule, FOURPOLEWidget>("FOURPOLE");
