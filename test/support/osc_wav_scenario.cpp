// osc_wav_scenario.cpp — build a RenderScenario for the in-tree oscillator
// engines, plus the shape/engine name mappings the CLI tool and tests share.

#include "osc_wav_scenario.hpp"

namespace pulp::test::audio {

using pulp::signal::osc::VaShape;

RenderScenario make_oscillator_scenario(const OscRenderSpec& spec) {
    pulp::format::ProcessorFactory factory = create_osc_source_vco;
    switch (spec.engine) {
        case OscEngine::vco: factory = create_osc_source_vco; break;
        case OscEngine::dco: factory = create_osc_source_dco; break;
        case OscEngine::wt:  factory = create_osc_source_wt;  break;
    }

    RenderScenario scenario(factory);
    scenario.name(spec.name)
        .sample_rate(spec.sample_rate)
        .block_size(spec.block_size)
        .channels(0, spec.channels)
        .duration_ms(spec.duration_ms)
        .set_param(kOscFrequencyHz, static_cast<float>(spec.frequency_hz));

    // `wt` has no shape parameter — it plays its own (fixed) wavetable set.
    if (spec.engine != OscEngine::wt) {
        scenario.set_param(kOscShape,
                            static_cast<float>(static_cast<int>(spec.shape)));
    }

    // Drift, jitter, and the noise seed are `vco`-only: `dco` has no noise
    // (its imperfection is quantization, not drift) and `wt` has no noise
    // source at all.
    if (spec.engine == OscEngine::vco) {
        scenario.set_param(kOscDriftCents, static_cast<float>(spec.drift_cents))
            .set_param(kOscJitterCents, static_cast<float>(spec.jitter_cents));
        if (spec.seed.has_value())
            scenario.set_param(kOscSeed, static_cast<float>(*spec.seed));
    }

    return scenario;
}

std::optional<VaShape> parse_shape(std::string_view name) {
    if (name == "sine") return VaShape::sine;
    if (name == "saw") return VaShape::saw;
    if (name == "square") return VaShape::square;
    if (name == "triangle") return VaShape::triangle;
    return std::nullopt;
}

const char* shape_name(VaShape shape) {
    switch (shape) {
        case VaShape::sine: return "sine";
        case VaShape::saw: return "saw";
        case VaShape::square: return "square";
        case VaShape::triangle: return "triangle";
    }
    return "unknown";
}

std::optional<OscEngine> parse_engine(std::string_view name) {
    if (name == "vco") return OscEngine::vco;
    if (name == "dco") return OscEngine::dco;
    if (name == "wt") return OscEngine::wt;
    return std::nullopt;
}

const char* engine_name(OscEngine engine) {
    switch (engine) {
        case OscEngine::vco: return "vco";
        case OscEngine::dco: return "dco";
        case OscEngine::wt: return "wt";
    }
    return "unknown";
}

} // namespace pulp::test::audio
