#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/oscillator.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace pfr = pulp::format::rack;
using Osc = pulp::signal::OscillatorT<float>;

/// Variable-width pulse.
///
/// OscillatorT's square is a fixed 50% duty cycle, so it cannot serve a PW
/// control — and a knob wired to nothing is worse than no knob. This keeps its
/// own phase so the width is adjustable, and applies the same polyBLEP
/// correction at both edges to keep the harmonics from aliasing.
struct PulseOsc {
    float phase = 0.0f, freq = 440.0f, sr = 48000.0f;

    void set_sample_rate(float s) { sr = s; }
    void set_frequency(float f) { freq = f; }
    void reset() { phase = 0.0f; }

    static float blep(float t, float dt) {
        if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
        if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
        return 0.0f;
    }

    float next(float width) {
        const float dt = freq / sr;
        width = std::clamp(width, 0.02f, 0.98f);
        float out = phase < width ? 1.0f : -1.0f;
        out += blep(phase, dt);
        // The falling edge sits at `width`, not at 0.5 — that is the whole
        // point of a PW control, and the correction has to follow it.
        float t = phase - width;
        if (t < 0.0f) t += 1.0f;
        out -= blep(t, dt);
        phase += dt;
        if (phase >= 1.0f) phase -= 1.0f;
        return out;
    }
};

/// Four simultaneous waveform outputs, polyphonic to 16 channels.
///
/// Rack calls process() once per sample, and Pulp's OscillatorT is already a
/// per-sample generator (`next()`), so there is no block adaptation here at
/// all — this is the modular-native tier. Voltage conventions come from the
/// shared header so no module can drift from the published standards.
struct VCOModule : rack::engine::Module {
    using L = forge_modular::VCOLayout;

    static constexpr int kMaxCh = rack::engine::PORT_MAX_CHANNELS;
    Osc saw_[kMaxCh], tri_[kMaxCh], sine_[kMaxCh];
    PulseOsc pulse_[kMaxCh];

    VCOModule() {
        forge_modular::config_VCO(this);
        configBypass(L::VOCT_INPUT, L::SAW_OUTPUT);
        on_sample_rate(static_cast<float>(APP->engine->getSampleRate()));
        for (int c = 0; c < kMaxCh; ++c) {
            saw_[c].set_waveform(Osc::Waveform::saw);
            tri_[c].set_waveform(Osc::Waveform::triangle);
            sine_[c].set_waveform(Osc::Waveform::sine);
        }
    }

    void on_sample_rate(float sr) {
        for (int c = 0; c < kMaxCh; ++c) {
            saw_[c].set_sample_rate(sr);
            pulse_[c].set_sample_rate(sr);
            tri_[c].set_sample_rate(sr);
            sine_[c].set_sample_rate(sr);
        }
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        on_sample_rate(e.sampleRate);
    }

    void process(const ProcessArgs& args) override {
        // Polyphony follows the pitch input; a disconnected input still plays
        // one voice from the knob, which is what a patched-in-isolation
        // oscillator must do.
        const int ch = std::max(1, inputs[L::VOCT_INPUT].getChannels());
        for (int id : {L::SAW_OUTPUT, L::PULSE_OUTPUT, L::TRI_OUTPUT, L::SINE_OUTPUT})
            outputs[id].setChannels(ch);

        const float coarse = params[L::FREQ_PARAM].getValue();
        const float fine = params[L::FINE_PARAM].getValue();
        const float fm_amt = params[L::FM_PARAM].getValue();
        const float width = params[L::PW_PARAM].getValue();

        for (int c = 0; c < ch; ++c) {
            // 1V/oct: pitch is summed in the VOLT domain, then converted once.
            // Summing volts (not hertz) is what makes tracking exact and is the
            // whole reason the standard is logarithmic.
            float v = coarse + fine / 12.0f
                    + inputs[L::VOCT_INPUT].getPolyVoltage(c);
            if (inputs[L::FM_INPUT].isConnected())
                v += fm_amt * inputs[L::FM_INPUT].getPolyVoltage(c);

            const float hz = std::clamp(pfr::volts::voct_to_hz(v), 1.0f,
                                        static_cast<float>(args.sampleRate) * 0.45f);

            saw_[c].set_frequency(hz);
            pulse_[c].set_frequency(hz);
            tri_[c].set_frequency(hz);
            sine_[c].set_frequency(hz);

            // Audio leaves at ±5 V, the published level for audio signals.
            constexpr float kPk = pfr::volts::kAudioPeak;
            if (outputs[L::SAW_OUTPUT].isConnected())
                outputs[L::SAW_OUTPUT].setVoltage(saw_[c].next() * kPk, c);
            if (outputs[L::PULSE_OUTPUT].isConnected())
                outputs[L::PULSE_OUTPUT].setVoltage(pulse_[c].next(width) * kPk, c);
            if (outputs[L::TRI_OUTPUT].isConnected())
                outputs[L::TRI_OUTPUT].setVoltage(tri_[c].next() * kPk, c);
            if (outputs[L::SINE_OUTPUT].isConnected())
                outputs[L::SINE_OUTPUT].setVoltage(sine_[c].next() * kPk, c);
        }
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        // Versioned from the first release: a regenerated module must be able
        // to load state written by an earlier generation of itself.
        json_object_set_new(root, "schema", json_integer(1));
        return root;
    }
    void dataFromJson(json_t*) override {}
};

struct VCOWidget : rack::app::ModuleWidget {
    explicit VCOWidget(VCOModule* module) {
        setModule(module);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/VCO.svg"),
            rack::asset::plugin(pluginInstance, "res/VCO-dark.svg")));
        forge_modular::place_VCO(this, module);
    }
};

}  // namespace

rack::plugin::Model* modelVCO =
    rack::createModel<VCOModule, VCOWidget>("VCO");
