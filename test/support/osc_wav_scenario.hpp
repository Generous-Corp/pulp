#pragma once

/// @file osc_wav_scenario.hpp
/// Render an in-tree oscillator through RenderScenario, so the offline analysis
/// lane can study it without a plugin bundle.
///
/// Three engines are selectable (`OscEngine`), each a minimal instrument
/// Processor that emits one oscillator voice at a fixed frequency:
///
///   * `vco` (`VcoSourceProcessor`) — `VcoOscillator`, the circuit-flavored
///     analog voice with seeded drift/jitter pitch noise.
///   * `dco` (`DcoSourceProcessor`) — `DcoOscillator`, the divider-clocked
///     voice with quantized (not drifting) pitch; no noise parameters.
///   * `wt` (`WtSourceProcessor`) — `WtOscillator` over a single default
///     bandlimited saw table; no shape/noise parameters, since a wavetable
///     is smooth everywhere and this render never scans its (one-entry) set.
///
/// Their configuration — frequency, shape, the two seeded pitch-noise depths
/// (drift / jitter, `vco` only), and the noise seed (`vco` only) — flows in
/// through parameters, because a RenderScenario factory is a bare function
/// pointer and cannot capture per-render state. `vco`'s noise seed defaults to
/// the oscillator's built-in deterministic value (`OscRenderSpec::seed` unset,
/// which reads back as parameter value 0 — see `kOscSeed` below), so a given
/// configuration renders bit-for-bit reproducibly (identical seed and inputs →
/// identical output — the same contract the OSC-VCO suite gates), which is
/// exactly what an offline drift/jitter analysis needs.
///
/// Test/tool layer only.

#include "render_scenario.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/signal/osc/dco.hpp>
#include <pulp/signal/osc/va.hpp>
#include <pulp/signal/osc/vco.hpp>
#include <pulp/signal/osc/wt.hpp>
#include <pulp/signal/wavetable.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::test::audio {

/// Parameter ids for the in-tree oscillator source processors. Each engine's
/// processor registers only the subset that applies to it (see the per-class
/// `define_parameters()`); the ids are shared across engines purely to avoid
/// three parallel enums; they never collide because each engine's Processor
/// owns its own StateStore.
enum OscSourceParam : pulp::state::ParamID {
    kOscFrequencyHz = 1,  ///< Fundamental, Hz. All engines.
    kOscShape = 2,        ///< 0=sine, 1=saw, 2=square, 3=triangle. vco/dco only.
    kOscDriftCents = 3,   ///< Slow drift depth, cents RMS (0 = off). vco only.
    kOscJitterCents = 4,  ///< Per-sample jitter depth, cents RMS (0 = off). vco only.
    kOscSeed = 5,         ///< Noise seed, 0 = "no override". vco only.
};

/// `kOscSeed`'s upper bound: the largest integer a `float` (the StateStore's
/// value type) represents exactly, `2^24 - 1`. A seed above this would lose
/// precision round-tripping through the parameter, silently landing on a
/// different (still deterministic, but not the requested) seed.
inline constexpr std::uint64_t kOscMaxExactSeed = (std::uint64_t{1} << 24) - 1;

/// The oscillator engine an `OscRenderSpec` renders through.
enum class OscEngine { vco, dco, wt };

/// A minimal instrument that plays one `VcoOscillator` at the frequency held in
/// its parameters. No MIDI, no input — it just fills its output block with the
/// oscillator's samples, so an in-tree oscillator can be rendered offline
/// through HeadlessHost and captured to a WAV.
class VcoSourceProcessor : public pulp::format::Processor {
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
        store.add_parameter({kOscSeed, "Seed", "",
                             {0.0f, static_cast<float>(kOscMaxExactSeed), 0.0f,
                              1.0f}});
    }

    void prepare(const pulp::format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate > 0.0 ? ctx.sample_rate : sample_rate_;
        osc_.prepare(sample_rate_);
        // A fixed non-zero start phase avoids sitting exactly on a zero of the
        // shape; reset() also restarts the seeded noise streams, so the render
        // begins from a known, reproducible point.
        osc_.reset(kStartPhase);
        seed_applied_ = false;
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        // `set_param()` on the RenderScenario applies AFTER prepare(), so the
        // seed override can only be read (and applied) once the first block
        // starts — before this. Applying it once here, before any sample is
        // generated, keeps the render fully seeded from the start; applying it
        // every block would re-seed (and truncate) the noise streams
        // repeatedly. A seed of 0 means "no override": the oscillator keeps
        // the built-in deterministic seed set by reset() in prepare().
        if (!seed_applied_) {
            const float raw_seed = state().get_value(kOscSeed);
            if (raw_seed > 0.0f)
                osc_.set_seed(static_cast<std::uint64_t>(raw_seed));
            seed_applied_ = true;
        }

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
    bool seed_applied_ = false;
};

inline std::unique_ptr<pulp::format::Processor> create_osc_source_vco() {
    return std::make_unique<VcoSourceProcessor>();
}

/// A minimal instrument that plays one `DcoOscillator` at the frequency held in
/// its parameters. Unlike the VCO engine, a DCO has no drift/jitter — its
/// characteristic imperfection is pitch quantization from a fixed master
/// clock — so it exposes only frequency and shape.
class DcoSourceProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "OscSourceDco",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.osc-source-dco",
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
    }

    void prepare(const pulp::format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate > 0.0 ? ctx.sample_rate : sample_rate_;
        pulp::signal::osc::DcoProfile profile;
        profile.master_clock_hz = kMasterClockHz;
        profile.divider_scheme = pulp::signal::osc::DcoDivider::integer_n;
        osc_.set_profile(profile);
        osc_.prepare(sample_rate_);
        // Same clean-start convention as the VCO engine; a DCO reset carries
        // no seeded state (there is none), just phase and the divider
        // schedule.
        osc_.reset(kStartPhase);
        last_note_hz_ = -1.0;
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const auto shape = static_cast<pulp::signal::osc::VaShape>(
            std::clamp(static_cast<int>(state().get_value(kOscShape)), 0, 3));
        osc_.set_shape(shape);

        // `set_note_hz()` re-derives the divider and calls `reset(phase())`,
        // which drops any in-flight discontinuity correction — harmless once,
        // but calling it every block for an unchanged note would do that
        // needlessly on every block boundary. Guard on an actual change.
        const double frequency = state().get_value(kOscFrequencyHz);
        if (frequency != last_note_hz_) {
            osc_.set_note_hz(frequency);
            last_note_hz_ = frequency;
        }

        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            const auto sample = static_cast<float>(osc_.next());
            for (std::size_t ch = 0; ch < output.num_channels(); ++ch)
                output.channel(ch)[i] = sample;
        }
    }

private:
    /// 1 MHz: a plausible mid-range DCO master clock, giving a small,
    /// audible-if-you-look-for-it quantization error at the top of the
    /// keyboard rather than an unrealistically fine (or coarse) one.
    static constexpr double kMasterClockHz = 1'000'000.0;
    static constexpr double kStartPhase = 0.13;

    pulp::signal::osc::DcoOscillator osc_;
    double sample_rate_ = 48000.0;
    double last_note_hz_ = -1.0;
};

inline std::unique_ptr<pulp::format::Processor> create_osc_source_dco() {
    return std::make_unique<DcoSourceProcessor>();
}

/// A minimal instrument that plays one `WtOscillator` over a single default
/// bandlimited saw table. No shape parameter (a wavetable set replaces the
/// classical-shape switch) and no drift/jitter (the wavetable engine has no
/// noise source).
class WtSourceProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "OscSourceWt",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.osc-source-wt",
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
    }

    void prepare(const pulp::format::PrepareContext& ctx) override {
        sample_rate_ = ctx.sample_rate > 0.0 ? ctx.sample_rate : sample_rate_;
        // A one-entry table set: this render is a single fixed-shape voice,
        // so the scan position (left at its default, 0) never moves off the
        // only table in the set. `WavetableBankT`'s single-table fast path
        // then makes this equivalent to playing one `WavetableT` directly.
        osc_.set_wavetable_set({pulp::signal::Wavetable64::make_saw(
            /*bands=*/10, /*table_length=*/2048,
            static_cast<double>(sample_rate_))});
        osc_.prepare(sample_rate_);
        osc_.reset();
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const double frequency = state().get_value(kOscFrequencyHz);
        const double increment = frequency / sample_rate_;

        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            const auto sample = static_cast<float>(osc_.next(increment));
            for (std::size_t ch = 0; ch < output.num_channels(); ++ch)
                output.channel(ch)[i] = sample;
        }
    }

private:
    pulp::signal::osc::WtOscillator osc_;
    double sample_rate_ = 48000.0;
};

inline std::unique_ptr<pulp::format::Processor> create_osc_source_wt() {
    return std::make_unique<WtSourceProcessor>();
}

/// One named oscillator render request. Field defaults render a quarter-second
/// 440 Hz sine at 48 kHz, mono, through the `vco` engine.
struct OscRenderSpec {
    OscEngine engine = OscEngine::vco;
    pulp::signal::osc::VaShape shape = pulp::signal::osc::VaShape::sine;
    double frequency_hz = 440.0;
    double sample_rate = 48000.0;
    int block_size = 128;
    int channels = 1;
    double duration_ms = 250.0;
    double drift_cents = 0.0;
    double jitter_cents = 0.0;
    /// Noise seed override, `vco` engine only. Unset (the default) renders
    /// with the oscillator's built-in deterministic seed — the same output
    /// as before this field existed. Values above `kOscMaxExactSeed` lose
    /// precision round-tripping through the (float) StateStore parameter.
    std::optional<std::uint64_t> seed;
    std::string name = "osc";
};

/// Build (but do not run) a RenderScenario that plays the spec's oscillator.
RenderScenario make_oscillator_scenario(const OscRenderSpec& spec);

/// Parse a shape name; std::nullopt on an unknown name.
std::optional<pulp::signal::osc::VaShape> parse_shape(std::string_view name);

/// Canonical lowercase name for a shape ("sine"/"saw"/"square"/"triangle").
const char* shape_name(pulp::signal::osc::VaShape shape);

/// Parse an engine name ("vco"/"dco"/"wt"); std::nullopt on an unknown name.
std::optional<OscEngine> parse_engine(std::string_view name);

/// Canonical lowercase name for an engine ("vco"/"dco"/"wt").
const char* engine_name(OscEngine engine);

} // namespace pulp::test::audio
