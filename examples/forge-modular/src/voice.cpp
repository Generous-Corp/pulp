// The rest of the Forge Modular voice: ENV, VCF, VCA, EUCLID, LFO, MULT, ATT, SEQ.
//
// Every module is per-sample native. Pulp's DSP primitives already expose
// per-sample generators (AdsrT::next(), SvfT::process(), OscillatorT::next()),
// so Rack's one-sample-per-call contract needs no block adaptation anywhere.
// Voltage conventions come from the shared header, so no module can drift from
// the published standards.

#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/adsr.hpp>
#include <pulp/signal/oscillator.hpp>
#include <pulp/signal/svf.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace pfr = pulp::format::rack;
namespace V = pulp::format::rack::volts;

/// Schmitt-triggered edge detector on the published gate thresholds. Shared so
/// every clock/gate input in the pack behaves identically.
struct Edge {
    bool high = false;
    /// Returns true on the rising edge only.
    bool process(float v) {
        if (!high && v >= V::kSchmittHigh) { high = true; return true; }
        if (high && v <= V::kSchmittLow) high = false;
        return false;
    }
    void reset() { high = false; }
};

constexpr int kCh = rack::engine::PORT_MAX_CHANNELS;

// ── ENV ─────────────────────────────────────────────────────────────────────
struct ENVModule : rack::engine::Module {
    using L = forge_modular::ENVLayout;
    pulp::signal::AdsrT<float> env_[kCh];
    Edge gate_[kCh];
    bool open_[kCh] = {};

    ENVModule() {
        forge_modular::config_ENV(this);
        setRate(48000.f);          // Rack calls onSampleRateChange on add
    }
    void setRate(float sr) { for (auto& e : env_) e.set_sample_rate(sr); }
    void onSampleRateChange(const SampleRateChangeEvent& e) override { setRate(e.sampleRate); }

    void process(const ProcessArgs& args) override {
        const int ch = std::max(1, inputs[L::GATE_INPUT].getChannels());
        outputs[L::ENV_OUTPUT].setChannels(ch);
        outputs[L::INV_OUTPUT].setChannels(ch);
        pulp::signal::AdsrT<float>::Params p;
        p.attack  = params[L::ATTACK_PARAM].getValue();
        p.decay   = params[L::DECAY_PARAM].getValue();
        p.sustain = params[L::SUSTAIN_PARAM].getValue();
        p.release = params[L::RELEASE_PARAM].getValue();

        float lit = 0.f;
        for (int c = 0; c < ch; ++c) {
            env_[c].set_params(p);
            const float g = inputs[L::GATE_INPUT].getPolyVoltage(c);
            // A gate is a level, not an edge: note_on when it goes high,
            // note_off when it falls, which is what makes it hold correctly.
            if (gate_[c].process(g) && !open_[c]) { env_[c].note_on(); open_[c] = true; }
            else if (open_[c] && !gate_[c].high) { env_[c].note_off(); open_[c] = false; }

            const float e = env_[c].next();
            // Envelopes are unipolar CV: 0..10 V.
            outputs[L::ENV_OUTPUT].setVoltage(e * V::kCvUnipolar, c);
            outputs[L::INV_OUTPUT].setVoltage((1.0f - e) * V::kCvUnipolar, c);
            lit = std::max(lit, e);
        }
        lights[L::ENV_LIGHT].setBrightnessSmooth(lit, args.sampleTime);
    }
};

// ── VCF ─────────────────────────────────────────────────────────────────────
struct VCFModule : rack::engine::Module {
    using L = forge_modular::VCFLayout;
    pulp::signal::SvfT<float> lp_[kCh], hp_[kCh];

    VCFModule() {
        forge_modular::config_VCF(this);
        configBypass(L::IN_INPUT, L::LP_OUTPUT);
        setRate(48000.f);          // Rack calls onSampleRateChange on add
        for (int c = 0; c < kCh; ++c) {
            lp_[c].set_mode(pulp::signal::SvfT<float>::Mode::lowpass);
            hp_[c].set_mode(pulp::signal::SvfT<float>::Mode::highpass);
        }
    }
    void setRate(float sr) {
        for (int c = 0; c < kCh; ++c) { lp_[c].set_sample_rate(sr); hp_[c].set_sample_rate(sr); }
    }
    void onSampleRateChange(const SampleRateChangeEvent& e) override { setRate(e.sampleRate); }

    void process(const ProcessArgs& args) override {
        const int ch = std::max(1, inputs[L::IN_INPUT].getChannels());
        outputs[L::LP_OUTPUT].setChannels(ch);
        outputs[L::HP_OUTPUT].setChannels(ch);
        const float base = params[L::CUTOFF_PARAM].getValue();
        const float amt  = params[L::CVAMT_PARAM].getValue();
        // Resonance maps to Q; 0.95 -> a high but stable Q rather than self-osc.
        const float q = 0.707f + params[L::RES_PARAM].getValue() * 12.0f;

        for (int c = 0; c < ch; ++c) {
            // The Eurorack triple: knob + attenuverter x CV, summed in volts
            // so the filter tracks 1V/oct exactly like the oscillator does.
            const float v = base + amt * inputs[L::CV_INPUT].getPolyVoltage(c);
            const float hz = std::clamp(V::voct_to_hz(v), 20.0f,
                                        static_cast<float>(args.sampleRate) * 0.45f);
            const float x = inputs[L::IN_INPUT].getPolyVoltage(c);
            if (outputs[L::LP_OUTPUT].isConnected()) {
                lp_[c].set_frequency(hz); lp_[c].set_resonance(q);
                outputs[L::LP_OUTPUT].setVoltage(lp_[c].process(x), c);
            }
            if (outputs[L::HP_OUTPUT].isConnected()) {
                hp_[c].set_frequency(hz); hp_[c].set_resonance(q);
                outputs[L::HP_OUTPUT].setVoltage(hp_[c].process(x), c);
            }
        }
    }
};

// ── VCA ─────────────────────────────────────────────────────────────────────
struct VCAModule : rack::engine::Module {
    using L = forge_modular::VCALayout;
    VCAModule() {
        forge_modular::config_VCA(this);
        configBypass(L::IN_INPUT, L::OUT_OUTPUT);
    }
    void process(const ProcessArgs&) override {
        const int ch = std::max(1, inputs[L::IN_INPUT].getChannels());
        outputs[L::OUT_OUTPUT].setChannels(ch);
        const float lvl = params[L::LEVEL_PARAM].getValue();
        for (int c = 0; c < ch; ++c) {
            float g = lvl;
            // Unipolar 0..10 V CV scales the level; unpatched leaves the knob
            // in charge, so the VCA is useful with nothing connected.
            if (inputs[L::CV_INPUT].isConnected())
                g *= std::clamp(inputs[L::CV_INPUT].getPolyVoltage(c) / V::kCvUnipolar, 0.f, 1.f);
            outputs[L::OUT_OUTPUT].setVoltage(inputs[L::IN_INPUT].getPolyVoltage(c) * g, c);
        }
    }
};

// ── EUCLID ──────────────────────────────────────────────────────────────────
/// Bjorklund's algorithm: distribute `pulses` as evenly as possible over
/// `steps`. This is the one genuinely new algorithm in the pack -- there is no
/// DAW-plugin equivalent, which is exactly why it belongs in a modular set.
bool euclid_hit(int step, int steps, int pulses, int rotate) {
    if (steps <= 0 || pulses <= 0) return false;
    pulses = std::min(pulses, steps);
    const int i = ((step - rotate) % steps + steps) % steps;
    // The closed form of Bjorklund: a step is a hit when the pulse index
    // advances, which spreads pulses evenly without building the pattern.
    return ((i * pulses) % steps) < pulses;
}

struct EUCLIDModule : rack::engine::Module {
    using L = forge_modular::EUCLIDLayout;
    Edge clock_, reset_;
    int step_ = 0;
    rack::dsp::PulseGenerator pulse_;

    EUCLIDModule() { forge_modular::config_EUCLID(this); }

    void process(const ProcessArgs& args) override {
        const int steps  = static_cast<int>(std::round(params[L::LENGTH_PARAM].getValue()));
        const int pulses = static_cast<int>(std::round(params[L::FILL_PARAM].getValue()));
        const int rot    = static_cast<int>(std::round(params[L::ROTATE_PARAM].getValue()));

        if (reset_.process(inputs[L::RESET_INPUT].getVoltage())) step_ = 0;
        if (clock_.process(inputs[L::CLOCK_INPUT].getVoltage())) {
            if (euclid_hit(step_, steps, pulses, rot))
                pulse_.trigger(1e-3f);  // published trigger length: 10 V for ~1 ms
            step_ = steps > 0 ? (step_ + 1) % steps : 0;
        }
        const bool hi = pulse_.process(args.sampleTime);
        outputs[L::GATE_OUTPUT].setVoltage(hi ? V::kGateHigh : 0.0f);
        lights[L::STEP_LIGHT].setBrightnessSmooth(hi ? 1.f : 0.f, args.sampleTime);
    }
};

// ── LFO ─────────────────────────────────────────────────────────────────────
struct LFOModule : rack::engine::Module {
    using L = forge_modular::LFOLayout;
    using Osc = pulp::signal::OscillatorT<float>;
    Osc tri_, sqr_, sin_;
    Edge reset_;

    LFOModule() {
        forge_modular::config_LFO(this);
        tri_.set_waveform(Osc::Waveform::triangle);
        sqr_.set_waveform(Osc::Waveform::square);
        sin_.set_waveform(Osc::Waveform::sine);
        setRate(48000.f);          // Rack calls onSampleRateChange on add
    }
    void setRate(float sr) { tri_.set_sample_rate(sr); sqr_.set_sample_rate(sr); sin_.set_sample_rate(sr); }
    void onSampleRateChange(const SampleRateChangeEvent& e) override { setRate(e.sampleRate); }

    void process(const ProcessArgs&) override {
        // LFOs reference 2 Hz (120 BPM), not C4 -- the published split.
        const float hz = std::clamp(
            V::voct_to_hz(params[L::RATE_PARAM].getValue(), V::kLfoRefHz), 0.01f, 400.0f);
        tri_.set_frequency(hz); sqr_.set_frequency(hz); sin_.set_frequency(hz);
        if (reset_.process(inputs[L::RESET_INPUT].getVoltage())) {
            tri_.reset(); sqr_.reset(); sin_.reset();
        }
        // Bipolar CV is ±5 V.
        outputs[L::TRI_OUTPUT].setVoltage(tri_.next() * V::kCvBipolar);
        outputs[L::SQR_OUTPUT].setVoltage(sqr_.next() * V::kCvBipolar);
        outputs[L::SIN_OUTPUT].setVoltage(sin_.next() * V::kCvBipolar);
    }
};

// ── MULT ────────────────────────────────────────────────────────────────────
struct MULTModule : rack::engine::Module {
    using L = forge_modular::MULTLayout;
    MULTModule() { forge_modular::config_MULT(this); }
    void process(const ProcessArgs&) override {
        const int ch = std::max(1, inputs[L::IN_INPUT].getChannels());
        // Polyphony is forwarded intact, so a mult splits a poly cable as a
        // poly cable rather than collapsing it to one voice.
        for (int id : {L::OUT1_OUTPUT, L::OUT2_OUTPUT, L::OUT3_OUTPUT}) {
            outputs[id].setChannels(ch);
            for (int c = 0; c < ch; ++c)
                outputs[id].setVoltage(inputs[L::IN_INPUT].getPolyVoltage(c), c);
        }
    }
};

// ── ATT ─────────────────────────────────────────────────────────────────────
struct ATTModule : rack::engine::Module {
    using L = forge_modular::ATTLayout;
    ATTModule() { forge_modular::config_ATT(this); }
    void process(const ProcessArgs&) override {
        const int ch = std::max(1, inputs[L::IN_INPUT].getChannels());
        outputs[L::OUT_OUTPUT].setChannels(ch);
        const float amt = params[L::AMT_PARAM].getValue();
        const float ofs = params[L::OFFSET_PARAM].getValue();
        for (int c = 0; c < ch; ++c) {
            // scale-and-invert, then offset: the canonical attenuverter.
            const float v = inputs[L::IN_INPUT].getPolyVoltage(c) * amt + ofs;
            outputs[L::OUT_OUTPUT].setVoltage(std::clamp(v, -12.0f, 12.0f), c);
        }
    }
};

// ── SEQ ─────────────────────────────────────────────────────────────────────
struct SEQModule : rack::engine::Module {
    using L = forge_modular::SEQLayout;
    static constexpr int kSteps = 8;
    Edge clock_, reset_;
    // -1 means no clock has selected a step yet. The output still reads Step 1
    // while idle, and the first edge advances this sentinel to index 0 instead
    // of skipping directly to Step 2.
    int step_ = -1;
    rack::dsp::PulseGenerator gate_;

    SEQModule() { forge_modular::config_SEQ(this); }

    void process(const ProcessArgs& args) override {
        if (reset_.process(inputs[L::RESET_INPUT].getVoltage())) step_ = -1;
        if (clock_.process(inputs[L::CLOCK_INPUT].getVoltage())) {
            step_ = (step_ + 1) % kSteps;
            gate_.trigger(1e-3f);
        }
        // Step values are already in volts, so they feed a 1V/oct input
        // directly -- no scaling, which is what keeps the pitch exact.
        const int active_step = std::max(step_, 0);
        outputs[L::CV_OUTPUT].setVoltage(
            params[L::STEP1_PARAM + active_step].getValue());
        outputs[L::GATE_OUTPUT].setVoltage(gate_.process(args.sampleTime) ? V::kGateHigh : 0.0f);
    }
};

// ── Widgets ─────────────────────────────────────────────────────────────────
#define FORGE_WIDGET(SLUG)                                                       \
    struct SLUG##Widget : rack::app::ModuleWidget {                              \
        explicit SLUG##Widget(SLUG##Module* m) {                                 \
            setModule(m);                                                        \
            setPanel(rack::createPanel(                                          \
                rack::asset::plugin(pluginInstance, "res/" #SLUG ".svg"),        \
                rack::asset::plugin(pluginInstance, "res/" #SLUG "-dark.svg"))); \
            forge_modular::place_##SLUG(this, m);                                \
        }                                                                        \
    };
FORGE_WIDGET(ENV) FORGE_WIDGET(VCF) FORGE_WIDGET(VCA) FORGE_WIDGET(EUCLID)
FORGE_WIDGET(LFO) FORGE_WIDGET(MULT) FORGE_WIDGET(ATT) FORGE_WIDGET(SEQ)
#undef FORGE_WIDGET

}  // namespace

rack::plugin::Model* modelENV    = rack::createModel<ENVModule, ENVWidget>("ENV");
rack::plugin::Model* modelVCF    = rack::createModel<VCFModule, VCFWidget>("VCF");
rack::plugin::Model* modelVCA    = rack::createModel<VCAModule, VCAWidget>("VCA");
rack::plugin::Model* modelEUCLID = rack::createModel<EUCLIDModule, EUCLIDWidget>("EUCLID");
rack::plugin::Model* modelLFO    = rack::createModel<LFOModule, LFOWidget>("LFO");
rack::plugin::Model* modelMULT   = rack::createModel<MULTModule, MULTWidget>("MULT");
rack::plugin::Model* modelATT    = rack::createModel<ATTModule, ATTWidget>("ATT");
rack::plugin::Model* modelSEQ    = rack::createModel<SEQModule, SEQWidget>("SEQ");
