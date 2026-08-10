#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

/// Four-channel mixer.
///
/// Exercises the manifest features a generator needs: slider arrays, a
/// three-position switch with named positions, bipolar activity lights, and --
/// the important one -- **normalled CV inputs**. Each channel's level CV is
/// normalled to 10 V, so an unpatched channel passes at its slider setting
/// instead of being silent. That is the difference between a mixer that works
/// out of the box and one that looks broken until you patch eight cables.
struct MIXModule : rack::engine::Module {
    using L = forge_modular::MIXLayout;
    static constexpr int kStrips = 4;

    MIXModule() {
        forge_modular::config_MIX(this);
        configBypass(L::IN1_INPUT, L::OUT_OUTPUT);
    }

    void process(const ProcessArgs& args) override {
        const int ch = forge_modular::channels_MIX(this);
        outputs[L::OUT_OUTPUT].setChannels(ch);

        const int mode = static_cast<int>(std::round(params[L::MODE_PARAM].getValue()));
        const float master = params[L::MASTER_PARAM].getValue();

        float sum[rack::engine::PORT_MAX_CHANNELS] = {};
        for (int s = 0; s < kStrips; ++s) {
            const float lvl = params[L::LVL1_PARAM + s].getValue();
            float peak = 0.0f;
            for (int c = 0; c < ch; ++c) {
                // The normal is read through the generated accessor, so the
                // manifest's declaration and the DSP cannot disagree about it.
                const float cv = read_cv(s, c);
                float g = lvl * std::clamp(cv / V::kCvUnipolar, 0.0f, 1.0f);
                if (mode == 1) g *= g;                       // exponential response
                const float v = inputs[L::IN1_INPUT + s].getPolyVoltage(c) * g;
                sum[c] += v;
                peak = std::max(peak, v);
            }
            // Bipolar activity: green for positive, red for negative. A single
            // colour cannot show sign, which is why the corpus uses GreenRed.
            lights[L::LVL1_LIGHT + s * 2 + 0]
                .setBrightnessSmooth(std::max(0.0f, peak) / V::kAudioPeak, args.sampleTime);
            lights[L::LVL1_LIGHT + s * 2 + 1]
                .setBrightnessSmooth(std::max(0.0f, -peak) / V::kAudioPeak, args.sampleTime);
        }

        for (int c = 0; c < ch; ++c) {
            float v = sum[c] * master;
            if (mode == 2) {
                // Soft clip: tanh keeps a hot mix musical instead of squaring off.
                v = V::kAudioPeak * std::tanh(v / V::kAudioPeak);
            }
            outputs[L::OUT_OUTPUT].setVoltage(v, c);
        }
    }

    float read_cv(int strip, int c) {
        switch (strip) {
            case 0: return forge_modular::read_MIX_CV1_INPUT(this, c);
            case 1: return forge_modular::read_MIX_CV2_INPUT(this, c);
            case 2: return forge_modular::read_MIX_CV3_INPUT(this, c);
            default: return forge_modular::read_MIX_CV4_INPUT(this, c);
        }
    }
};

struct MIXWidget : rack::app::ModuleWidget {
    explicit MIXWidget(MIXModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/MIX.svg"),
            rack::asset::plugin(pluginInstance, "res/MIX-dark.svg")));
        forge_modular::place_MIX(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelMIX = rack::createModel<MIXModule, MIXWidget>("MIX");
