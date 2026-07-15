#pragma once

// EqCurveDemo — a six-band parametric EQ whose editor IS the interactive
// EqCurveView response curve.
//
// This is the showcase for the EqCurveView fix: the curve you drag is the true
// magnitude response of the exact biquad cascade the audio path runs, because
// both derive from the same designed coefficients. Grab a point, the curve
// bends, and the sound follows — a low-shelf that plateaus, two peaks, a
// high-shelf, each drawn as its real shape rather than a lookalike bump.
//
// Layout follows a classic console EQ: a low-shelf, four sweepable peaks, and a
// high-shelf. Every band exposes frequency, gain, and Q as automatable
// parameters; the editor writes them back on drag.

#include <pulp/format/processor.hpp>
#include <pulp/signal/biquad.hpp>
#if !PULP_HEADLESS
#include <pulp/view/eq_curve_view.hpp>
#include <pulp/view/view.hpp>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace pulp::examples {

// Six fixed-role bands — a low shelf, four sweepable peaks, a high shelf. Types
// are fixed per slot (not a parameter) so the demo reads as a recognizable
// console EQ and every handle drags in both axes.
inline constexpr int kEqBandCount = 6;

struct EqBandLayout {
    signal::Biquad::Type type;
    float default_freq;
    float default_q;
};

inline constexpr std::array<EqBandLayout, kEqBandCount> kEqBands = {{
    {signal::Biquad::Type::low_shelf, 80.0f, 0.707f},
    {signal::Biquad::Type::peaking, 250.0f, 1.0f},
    {signal::Biquad::Type::peaking, 700.0f, 1.2f},
    {signal::Biquad::Type::peaking, 2000.0f, 1.2f},
    {signal::Biquad::Type::peaking, 5000.0f, 1.0f},
    {signal::Biquad::Type::high_shelf, 12000.0f, 0.707f},
}};

// Parameter IDs are laid out three-per-band: freq, gain, Q. Band b owns
// [3b+1, 3b+3]. Kept as a helper so the processor and editor agree.
enum EqParamBase : state::ParamID { kEqParamFirst = 1 };
inline constexpr state::ParamID eq_freq_param(int band) {
    return static_cast<state::ParamID>(kEqParamFirst + band * 3 + 0);
}
inline constexpr state::ParamID eq_gain_param(int band) {
    return static_cast<state::ParamID>(kEqParamFirst + band * 3 + 1);
}
inline constexpr state::ParamID eq_q_param(int band) {
    return static_cast<state::ParamID>(kEqParamFirst + band * 3 + 2);
}

#if !PULP_HEADLESS
std::unique_ptr<view::View> build_eq_curve_editor(state::StateStore& store, float sample_rate);
#endif

class EqCurveDemoProcessor : public format::Processor {
public:
#if !PULP_HEADLESS
    std::unique_ptr<view::View> create_view() override {
        return build_eq_curve_editor(state(), sample_rate_);
    }
#endif

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "EQ Curve Demo",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.examples.eq-curve-demo",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        for (int b = 0; b < kEqBandCount; ++b) {
            const auto& layout = kEqBands[b];
            // Frequency: log-skewed across the audible band, geometric center.
            store.add_parameter({.id = eq_freq_param(b),
                                 .name = band_param_name(b, "Freq"),
                                 .unit = "Hz",
                                 .range = state::ParamRange::with_centre(
                                     20.0f, 20000.0f, std::sqrt(20.0f * 20000.0f),
                                     layout.default_freq)});
            // Gain: linear -18..+18 dB, default flat.
            store.add_parameter({.id = eq_gain_param(b),
                                 .name = band_param_name(b, "Gain"),
                                 .unit = "dB",
                                 .range = {-18.0f, 18.0f, 0.0f, 0.0f}});
            // Q: linear 0.1..12, default per layout.
            store.add_parameter({.id = eq_q_param(b),
                                 .name = band_param_name(b, "Q"),
                                 .unit = "",
                                 .range = {0.1f, 12.0f, layout.default_q, 0.0f}});
        }
    }

    void prepare(const format::PrepareContext& ctx) override {
        sample_rate_ = static_cast<float>(ctx.sample_rate);
        for (auto& ch : filters_)
            for (auto& b : ch) b.reset();
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        const std::size_t channels =
            std::min({output.num_channels(), input.num_channels(), filters_.size()});
        const std::size_t frames = output.num_samples();

        if (ctx.should_reset_dsp_state())
            for (auto& ch : filters_)
                for (auto& b : ch) b.reset();

        // Recompute the four sections per block from the current parameters —
        // the SAME design call (FilterDesign via Biquad::set_coefficients) the
        // editor's curve is sampled from. RT-safe: no allocation, and assigning
        // coefficients preserves each filter's running state (no zipper reset).
        const float nyq = sample_rate_ * 0.49f;
        for (int b = 0; b < kEqBandCount; ++b) {
            const float freq = std::clamp(state().get_value(eq_freq_param(b)), 20.0f, nyq);
            const float gain = state().get_value(eq_gain_param(b));
            const float q = std::clamp(state().get_value(eq_q_param(b)), 0.1f, 12.0f);
            for (std::size_t ch = 0; ch < channels; ++ch)
                filters_[ch][b].set_coefficients(kEqBands[b].type, freq, q, sample_rate_, gain);
        }

        for (std::size_t ch = 0; ch < channels; ++ch) {
            auto in = input.channel(ch);
            auto out = output.channel(ch);
            for (std::size_t i = 0; i < frames; ++i) {
                float s = in[i];
                for (auto& section : filters_[ch]) s = section.process(s);
                out[i] = s;
            }
        }
        for (std::size_t ch = channels; ch < output.num_channels(); ++ch) {
            auto o = output.channel(ch);
            for (std::size_t i = 0; i < frames; ++i) o[i] = 0.0f;
        }
    }

private:
    // Stable, human-readable parameter names ("Low Freq", "Peak 1 Gain", ...).
    // Main-thread only: define_parameters() is the sole caller and runs once at
    // construction, so the lazy fill of the shared static cache is unsynchronized
    // by design. Do not call this from a worker thread.
    static const char* band_param_name(int band, const char* suffix) {
        static std::array<std::array<std::string, 3>, kEqBandCount> cache;
        static constexpr std::array<const char*, kEqBandCount> role = {
            "Low", "Peak 1", "Peak 2", "Peak 3", "Peak 4", "High"};
        const int idx = std::string_view(suffix) == "Freq" ? 0
                        : std::string_view(suffix) == "Gain" ? 1
                                                             : 2;
        auto& slot = cache[band][idx];
        if (slot.empty()) slot = std::string(role[band]) + " " + suffix;
        return slot.c_str();
    }

    float sample_rate_ = 48000.0f;
    // [channel][band] second-order sections; stereo, four bands in series.
    std::array<std::array<signal::Biquad, kEqBandCount>, 2> filters_{};
};

inline std::unique_ptr<format::Processor> create_eq_curve_demo() {
    return std::make_unique<EqCurveDemoProcessor>();
}

} // namespace pulp::examples

#if !PULP_HEADLESS
#include "eq_curve_editor.hpp"
#endif
