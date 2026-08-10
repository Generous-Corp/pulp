#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/mod_tools.hpp>

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

// Times are held as log2(milliseconds) so one knob spans 0.5 ms to 8 s with a
// musically even taper, and a CV volt is a clean doubling of the time.
constexpr float kMinLog2Ms = -1.0f;
constexpr float kMaxLog2Ms = 13.0f;

// The rate limiters are normalised: a 0 -> 1 move takes the configured time,
// so the signal is scaled to unit range going in and back to volts coming out.
// A "rise time" is therefore the time a full 10 V move takes.
constexpr float kVoltScale = 0.1f;

// Below this the output has arrived, and a rise that has arrived is what the
// EOR gate reports. Small enough that a slow ramp is never called finished
// early, large enough that the exponential law's asymptote still trips it.
constexpr float kSettleVolts = 1.0e-3f;

struct SLEWRFModule : rack::engine::Module {
    using L = forge_modular::SLEWRFLayout;
    using Mode = pulp::signal::SlewMode;

    // Two laws that are deliberately different objects: the distance-dependent
    // limiter (linear and exponential) and the constant-time one, where every
    // move takes the same time however far it has to go.
    pulp::signal::SlewLimiterT<float> slew_;
    pulp::signal::ConstantTimeSlewLimiterT<float> const_slew_;

    int shape_ = -1;
    float rise_ms_ = -1.0f;
    float fall_ms_ = -1.0f;
    float unit_ = 0.0f;  // last output, in the limiters' unit range

    bool rising_ = false;
    bool eor_ = false;

    SLEWRFModule() {
        forge_modular::config_SLEWRF(this);
        configBypass(L::IN_INPUT, L::OUT_OUTPUT);

        // Rack has not told us the real rate yet; 48 kHz is the safe assumption.
        slew_.prepare(48000.0);
        const_slew_.prepare(48000.0);
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        slew_.prepare(e.sampleRate);
        const_slew_.prepare(e.sampleRate);
        slew_.set_immediate(unit_);
        const_slew_.set_immediate(unit_);
        rise_ms_ = -1.0f;
        fall_ms_ = -1.0f;
    }

    void process(const ProcessArgs& args) override {
        const int shape = static_cast<int>(
            std::clamp(std::round(params[L::SHAPE_PARAM].getValue()), 0.0f, 2.0f));
        if (shape != shape_) {
            // Hand the new law the level the old one had reached, or the switch
            // would jump the output.
            if (shape == 2)
                const_slew_.set_immediate(unit_);
            else
                slew_.set_immediate(unit_);
            slew_.set_mode(shape == 1 ? Mode::exponential : Mode::linear);
            shape_ = shape;
            rise_ms_ = -1.0f;  // the newly selected object may hold stale times
            fall_ms_ = -1.0f;
        }

        // Knob and CV sum in the log domain: 1 V doubles the time, which is the
        // same law the knob taper uses.
        const float rise_v = std::clamp(
            params[L::RISE_PARAM].getValue() + forge_modular::read_SLEWRF_RCV_INPUT(this, 0),
            kMinLog2Ms, kMaxLog2Ms);
        const float fall_v = std::clamp(
            params[L::FALL_PARAM].getValue() + forge_modular::read_SLEWRF_FCV_INPUT(this, 0),
            kMinLog2Ms, kMaxLog2Ms);
        const float rise_ms = std::exp2(rise_v);
        const float fall_ms = std::exp2(fall_v);

        if (rise_ms != rise_ms_) {
            if (shape == 2)
                const_slew_.set_rise_ms(rise_ms);
            else
                slew_.set_rise_ms(rise_ms);
            rise_ms_ = rise_ms;
        }
        if (fall_ms != fall_ms_) {
            if (shape == 2)
                const_slew_.set_fall_ms(fall_ms);
            else
                slew_.set_fall_ms(fall_ms);
            fall_ms_ = fall_ms;
        }

        const float target = inputs[L::IN_INPUT].getVoltage() * kVoltScale;
        unit_ = (shape == 2) ? const_slew_.process(target) : slew_.process(target);
        const float out = unit_ * (1.0f / kVoltScale);

        // End of rise: high once an upward move has arrived, and it stays high
        // until the output moves again in either direction.
        const float delta = (target - unit_) * (1.0f / kVoltScale);
        if (delta > kSettleVolts) {
            rising_ = true;
            eor_ = false;
        } else if (delta < -kSettleVolts) {
            rising_ = false;
            eor_ = false;
        } else if (rising_) {
            rising_ = false;
            eor_ = true;
        }

        outputs[L::OUT_OUTPUT].setVoltage(out);
        outputs[L::EOR_OUTPUT].setVoltage(eor_ ? V::kGateHigh : 0.0f);
        lights[L::EOR_LIGHT].setBrightnessSmooth(eor_ ? 1.0f : 0.0f, args.sampleTime);
    }
};

struct SLEWRFWidget : rack::app::ModuleWidget {
    explicit SLEWRFWidget(SLEWRFModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/SLEWRF.svg"),
            rack::asset::plugin(pluginInstance, "res/SLEWRF-dark.svg")));
        forge_modular::place_SLEWRF(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelSLEWRF = rack::createModel<SLEWRFModule, SLEWRFWidget>("SLEWRF");
