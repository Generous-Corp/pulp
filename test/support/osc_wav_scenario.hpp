#pragma once

/// @file osc_wav_scenario.hpp
/// Render an in-tree oscillator through RenderScenario, so the offline analysis
/// lane can study it without a plugin bundle.
///
/// `OscSourceProcessor` is a minimal instrument Processor that emits one
/// `VcoOscillator` voice at a fixed frequency. Its configuration — frequency,
/// shape, and the two seeded pitch-noise depths (drift / jitter) — flows in
/// through parameters, because a RenderScenario factory is a bare function
/// pointer and cannot capture per-render state. The oscillator's noise seed is
/// its deterministic default, so a given configuration renders bit-for-bit
/// reproducibly (identical seed and inputs → identical output — the same
/// contract the OSC-VCO suite gates), which is exactly what an offline
/// drift/jitter analysis needs.
///
/// Test/tool layer only.

#include "render_scenario.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/signal/osc/va.hpp>
#include <pulp/signal/osc/vco.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::test::audio {

/// Parameter ids for the in-tree oscillator source processor.
enum OscSourceParam : pulp::state::ParamID {
    kOscFrequencyHz = 1,  ///< Fundamental, Hz.
    kOscShape = 2,        ///< 0=sine, 1=saw, 2=square, 3=triangle.
    kOscDriftCents = 3,   ///< Slow drift depth, cents RMS (0 = off).
    kOscJitterCents = 4,  ///< Per-sample jitter depth, cents RMS (0 = off).
};

/// A minimal instrument that plays one `VcoOscillator` at the frequency held in
/// its parameters. No MIDI, no input — it just fills its output block with the
/// oscillator's samples, so an in-tree oscillator can be rendered offline
/// through HeadlessHost and captured to a WAV.
class OscSourceProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "OscSource",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.osc-source",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 1}},
            .accepts_midi = false,
            .produces_midi = false,
            .tail_samples = 0,
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({kOscFrequencyHz, "Frequency", "Hz",
                             {1.0f, 40000.0f, 440.0f, 0.0f}});
        store.add_parameter({kOscShape, "Shape", "", {0.0f, 3.0f, 0.0f, 1.0f}});
        store.add_parameter({kOscDriftCents, "Drift", "ct",
                             {0.0f, 1200.0f, 0.0f, 0.0f}});
        store.add_parameter({kOscJitterCents, "Jitter", "ct",
                             {0.0f, 1200.0f, 0.0f, 0.0f}});
    }

    void prepare(const pulp::format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate > 0.0 ? ctx.sample_rate : sample_rate_;
        osc_.prepare(sample_rate_);
        // A fixed non-zero start phase avoids sitting exactly on a zero of the
        // shape; reset() also restarts the seeded noise streams, so the render
        // begins from a known, reproducible point.
        osc_.reset(kStartPhase);
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const auto shape = static_cast<pulp::signal::osc::VaShape>(
            std::clamp(static_cast<int>(state().get_value(kOscShape)), 0, 3));
        osc_.set_shape(shape);
        osc_.set_drift_depth(state().get_value(kOscDriftCents));
        osc_.set_jitter_depth(state().get_value(kOscJitterCents));

        const double frequency = state().get_value(kOscFrequencyHz);
        const double increment = frequency / sample_rate_;

        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            const auto sample = static_cast<float>(osc_.next(increment));
            for (std::size_t ch = 0; ch < output.num_channels(); ++ch)
                output.channel(ch)[i] = sample;
        }
    }

private:
    /// Cleanly off any shape's zero, matching the OSC-VCO suite's clean-start
    /// convention so an offline render lines up with in-suite references.
    static constexpr double kStartPhase = 0.13;

    pulp::signal::osc::VcoOscillator osc_;
    double sample_rate_ = 48000.0;
};

inline std::unique_ptr<pulp::format::Processor> create_osc_source() {
    return std::make_unique<OscSourceProcessor>();
}

/// One named oscillator render request. Field defaults render a quarter-second
/// 440 Hz sine at 48 kHz, mono.
struct OscRenderSpec {
    pulp::signal::osc::VaShape shape = pulp::signal::osc::VaShape::sine;
    double frequency_hz = 440.0;
    double sample_rate = 48000.0;
    int block_size = 128;
    int channels = 1;
    double duration_ms = 250.0;
    double drift_cents = 0.0;
    double jitter_cents = 0.0;
    std::string name = "osc";
};

/// Build (but do not run) a RenderScenario that plays the spec's oscillator.
RenderScenario make_oscillator_scenario(const OscRenderSpec& spec);

/// Parse a shape name; std::nullopt on an unknown name.
std::optional<pulp::signal::osc::VaShape> parse_shape(std::string_view name);

/// Canonical lowercase name for a shape ("sine"/"saw"/"square"/"triangle").
const char* shape_name(pulp::signal::osc::VaShape shape);

} // namespace pulp::test::audio
