#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
#include <pulp/signal/chaos.hpp>
#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/rng.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

namespace V = pulp::format::rack::volts;

/// Three outputs from one chaotic history.
///
/// X is a logistic map stepped by the clock. Y is a second map of the same
/// family whose control parameter is bent by X's current state, so it shares
/// X's turning points without ever landing on X's sequence. Z is an
/// Ornstein-Uhlenbeck walk whose mean is X, so it is continuously pulled toward
/// the steps but arrives late and overshoots — smooth where the other two are
/// stepped, and by construction non-repeating.
struct TANGLEModule : rack::engine::Module {
    using L = forge_modular::TANGLELayout;

    pulp::signal::LogisticMapT<float> map_x_;
    pulp::signal::LogisticMapT<float> map_y_;
    pulp::signal::OuWalkT<float> drift_;
    pulp::signal::SlewLimiterT<float> glide_x_;
    pulp::signal::SlewLimiterT<float> glide_y_;
    pulp::signal::Xorshift32 rng_;

    double clock_phase_ = 0.0;  // internal clock, in turns
    bool clock_high_ = false;
    bool reset_high_ = false;
    float x_step_ = 0.f;  // held map output, in [-1, 1]
    float y_step_ = 0.f;

    // Z advances on its own control clock and is interpolated between updates,
    // so the walk's speed follows RATE rather than the sample rate.
    double z_phase_ = 0.0;
    float z_prev_ = 0.f;
    float z_next_ = 0.f;

    // Coefficient updates in the walk and the glides cost transcendentals, so
    // they are only pushed when the control actually moved.
    float last_glide_ms_ = -1.f;
    float last_spread_ = -1.f;
    double last_z_rate_ = -1.0;

    TANGLEModule() {
        forge_modular::config_TANGLE(this);
        // 48 kHz is a placeholder: Rack delivers the real rate through
        // onSampleRateChange the moment the module is added.
        prepare(48000.0);
        reseed(0x7A17u);
    }

    void prepare(double sample_rate) {
        drift_.prepare(sample_rate);
        glide_x_.prepare(sample_rate);
        glide_y_.prepare(sample_rate);
        glide_x_.set_mode(pulp::signal::SlewMode::exponential);
        glide_y_.set_mode(pulp::signal::SlewMode::exponential);
    }

    /// Start both orbits from fresh initial conditions. Two maps seeded at the
    /// same point would track each other exactly for the first several steps,
    /// which is the one thing a three-output chaos source must not do.
    void reseed(std::uint32_t s) {
        rng_.seed(s | 1u);
        map_x_.seed(0.05 + 0.90 * static_cast<double>(rng_.next_unipolar()));
        map_y_.seed(0.05 + 0.90 * static_cast<double>(rng_.next_unipolar()));
        drift_.set_seed(s ^ 0x9E3779B9u);
        drift_.reset();
        clock_phase_ = 0.0;
        z_phase_ = 0.0;
        z_prev_ = z_next_ = 0.f;
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        prepare(e.sampleRate);
        last_glide_ms_ = -1.f;
        last_spread_ = -1.f;
        last_z_rate_ = -1.0;
    }

    void process(const ProcessArgs& args) override {
        // --- controls ------------------------------------------------------
        // Rate is summed in volts and converted once, so the CV tracks 1 V/oct.
        const float rate_v = std::clamp(
            params[L::RATE_PARAM].getValue()
                + params[L::RATE_CV_PARAM].getValue()
                      * forge_modular::read_TANGLE_RATE_CV_INPUT(this, 0),
            -8.f, 8.f);
        const float hz = std::clamp(V::voct_to_hz(rate_v, V::kLfoRefHz), 0.01f, 200.f);

        const float chaos = std::clamp(
            params[L::CHAOS_PARAM].getValue()
                + params[L::CHAOS_CV_PARAM].getValue()
                      * forge_modular::read_TANGLE_CHAOS_CV_INPUT(this, 0) * 0.1f,
            0.f, 1.f);
        const float spread = std::clamp(
            params[L::SPREAD_PARAM].getValue()
                + params[L::SPREAD_CV_PARAM].getValue()
                      * forge_modular::read_TANGLE_SPREAD_CV_INPUT(this, 0) * 0.1f,
            0.f, 1.f);
        const float glide_ms = params[L::SLEW_PARAM].getValue();

        // --- reset ---------------------------------------------------------
        const float rst = inputs[L::RST_INPUT].getVoltage();
        if (!reset_high_ && rst >= V::kSchmittHigh) {
            reset_high_ = true;
            // A fresh orbit rather than the one we started with: re-running the
            // same sequence is what "never repeats" rules out.
            reseed(rng_.next_u32());
        } else if (reset_high_ && rst <= V::kSchmittLow) {
            reset_high_ = false;
        }

        // --- clock ---------------------------------------------------------
        bool step_now = false;
        if (inputs[L::CLK_INPUT].isConnected()) {
            const float clk = inputs[L::CLK_INPUT].getVoltage();
            if (!clock_high_ && clk >= V::kSchmittHigh) {
                clock_high_ = true;
                step_now = true;
            } else if (clock_high_ && clk <= V::kSchmittLow) {
                clock_high_ = false;
            }
        } else {
            clock_phase_ += static_cast<double>(hz) * args.sampleTime;
            if (clock_phase_ >= 1.0) {
                clock_phase_ -= std::floor(clock_phase_);
                step_now = true;
            }
        }

        // --- the two maps --------------------------------------------------
        if (step_now) {
            // 3.5 is a clean 4-cycle, 4.0 fills the interval; the knob is that
            // window and nothing outside it is musically interesting.
            const double r_x = 3.5 + 0.5 * static_cast<double>(chaos);
            map_x_.set_r(r_x);
            x_step_ = map_x_.next_bipolar();

            // Y sits slightly behind X on the bifurcation diagram, and X's
            // state pushes it further. That coupling is what keeps the two
            // related; the offset is what keeps them distinct.
            map_y_.set_r(r_x - 0.30 * static_cast<double>(spread)
                             + 0.12 * static_cast<double>(spread)
                                   * static_cast<double>(x_step_));
            y_step_ = map_y_.next_bipolar();
        }

        // --- glide ---------------------------------------------------------
        if (glide_ms != last_glide_ms_) {
            last_glide_ms_ = glide_ms;
            glide_x_.set_time_ms(glide_ms);
            // Y glides slower, so the pair never resolves to the same value at
            // the same instant even when the maps momentarily agree.
            glide_y_.set_time_ms(glide_ms * 1.7f);
        }
        const float x = glide_x_.process(x_step_);
        const float y = glide_y_.process(y_step_);

        // --- drift ---------------------------------------------------------
        // The walk runs on its own clock at a fixed multiple of RATE, so Z
        // wanders at the module's speed whether the steps come from inside or
        // from CLK, and the cost does not scale with the sample rate.
        const double z_rate = std::clamp(static_cast<double>(hz) * 48.0, 1.0,
                                         static_cast<double>(args.sampleRate) * 0.5);
        if (std::abs(z_rate - last_z_rate_) > 1e-4 * z_rate) {
            last_z_rate_ = z_rate;
            drift_.prepare(z_rate);
        }
        if (spread != last_spread_) {
            last_spread_ = spread;
            // More spread: wanders further and is pulled home more slowly.
            drift_.set_sigma(0.05 + 0.70 * static_cast<double>(spread));
            drift_.set_theta(1.0 - 0.90 * static_cast<double>(spread));
        }
        z_phase_ += z_rate * args.sampleTime;
        // z_rate is capped at half the sample rate, so this advances at most
        // one step per sample — bounded by construction, not by luck.
        while (z_phase_ >= 1.0) {
            z_phase_ -= 1.0;
            z_prev_ = z_next_;
            drift_.set_mu(static_cast<double>(x));  // pulled toward the steps
            z_next_ = drift_.next();
        }
        const float z = z_prev_ + static_cast<float>(z_phase_) * (z_next_ - z_prev_);

        // --- outputs -------------------------------------------------------
        outputs[L::X_OUTPUT].setVoltage(std::clamp(x, -1.f, 1.f) * V::kCvBipolar);
        outputs[L::Y_OUTPUT].setVoltage(std::clamp(y, -1.f, 1.f) * V::kCvBipolar);
        outputs[L::Z_OUTPUT].setVoltage(std::clamp(z, -1.f, 1.f) * V::kCvBipolar);

        lights[L::X_LIGHT].setBrightnessSmooth(std::abs(x), args.sampleTime);
        lights[L::Y_LIGHT].setBrightnessSmooth(std::abs(y), args.sampleTime);
        lights[L::Z_LIGHT].setBrightnessSmooth(std::abs(z), args.sampleTime);
    }
};

struct TANGLEWidget : rack::app::ModuleWidget {
    explicit TANGLEWidget(TANGLEModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/TANGLE.svg"),
            rack::asset::plugin(pluginInstance, "res/TANGLE-dark.svg")));
        forge_modular::place_TANGLE(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelTANGLE = rack::createModel<TANGLEModule, TANGLEWidget>("TANGLE");
