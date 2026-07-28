#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

namespace V = pulp::format::rack::volts;

constexpr int kMaxChannels = 16;

struct SANDHModule : rack::engine::Module {
    using L = forge_modular::SANDHLayout;

    rack::dsp::SchmittTrigger trig_[kMaxChannels];
    float held_[kMaxChannels] = {};
    float slewed_[kMaxChannels] = {};
    std::uint32_t rng_[kMaxChannels] = {};

    SANDHModule() {
        forge_modular::config_SANDH(this);
        configBypass(L::IN_INPUT, L::OUT_OUTPUT);
        for (int c = 0; c < kMaxChannels; ++c) {
            rng_[c] = 0x9E3779B9u + 0x85EBCA6Bu * static_cast<std::uint32_t>(c + 1);
        }
    }

    // White noise in volts, bipolar CV range. Allocation-free, per channel.
    float next_noise(int c) {
        std::uint32_t& s = rng_[c];
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        const float unit = static_cast<float>(s) * (1.f / 2147483648.f) - 1.f;
        return unit * V::kCvBipolar;
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        (void)e;
        for (int c = 0; c < kMaxChannels; ++c) {
            trig_[c].reset();
            held_[c] = 0.f;
            slewed_[c] = 0.f;
        }
    }

    void process(const ProcessArgs& args) override {
        const int ch = forge_modular::channels_SANDH(this);

        const float slew = std::clamp(params[L::SLEW_PARAM].getValue(), 0.f, 1.f);
        const float level = std::clamp(params[L::LEVEL_PARAM].getValue(), 0.f, 1.f);

        // 0.5 ms (near-instant) up to 1 s glide.
        const float tau = 0.0005f * std::pow(2000.f, slew);
        const float coeff = 1.f - std::exp(-args.sampleTime / tau);

        const bool patched = inputs[L::IN_INPUT].isConnected();

        outputs[L::OUT_OUTPUT].setChannels(ch);

        float sum = 0.f;
        for (int c = 0; c < ch; ++c) {
            const float trig_v = inputs[L::TRIG_INPUT].getPolyVoltage(c);
            if (trig_[c].process(trig_v, V::kSchmittLow, V::kSchmittHigh)) {
                held_[c] = patched ? inputs[L::IN_INPUT].getPolyVoltage(c) : next_noise(c);
            }

            slewed_[c] += (held_[c] - slewed_[c]) * coeff;

            const float out = std::clamp(slewed_[c] * level,
                                         -2.f * V::kCvBipolar, 2.f * V::kCvBipolar);
            outputs[L::OUT_OUTPUT].setVoltage(out, c);
            sum += std::fabs(out);
        }

        const float lit = (ch > 0) ? (sum / (static_cast<float>(ch) * V::kCvBipolar)) : 0.f;
        lights[L::OUT_LIGHT].setBrightnessSmooth(std::clamp(lit, 0.f, 1.f), args.sampleTime);
    }
};

struct SANDHWidget : rack::app::ModuleWidget {
    explicit SANDHWidget(SANDHModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/SANDH.svg"),
            rack::asset::plugin(pluginInstance, "res/SANDH-dark.svg")));
        forge_modular::place_SANDH(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelSANDH = rack::createModel<SANDHModule, SANDHWidget>("SANDH");
