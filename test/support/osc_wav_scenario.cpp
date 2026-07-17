// osc_wav_scenario.cpp — build a RenderScenario for the in-tree oscillator
// source, plus the shape-name mapping the CLI tool and tests share.

#include "osc_wav_scenario.hpp"

namespace pulp::test::audio {

using pulp::signal::osc::VaShape;

RenderScenario make_oscillator_scenario(const OscRenderSpec& spec) {
    RenderScenario scenario(create_osc_source);
    scenario.name(spec.name)
        .sample_rate(spec.sample_rate)
        .block_size(spec.block_size)
        .channels(0, spec.channels)
        .duration_ms(spec.duration_ms)
        .set_param(kOscFrequencyHz, static_cast<float>(spec.frequency_hz))
        .set_param(kOscShape,
                   static_cast<float>(static_cast<int>(spec.shape)))
        .set_param(kOscDriftCents, static_cast<float>(spec.drift_cents))
        .set_param(kOscJitterCents, static_cast<float>(spec.jitter_cents));
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

} // namespace pulp::test::audio
