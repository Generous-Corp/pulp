#include <pulp/view/chart_shader_presets.hpp>

#include <cmath>
#include <cstdio>
#include <string>

namespace pulp::view {
namespace {

// SkSL float literal: always with a decimal point so it never reads as int.
std::string lit(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(v));
    return buf;
}

std::string rgb_literal(const canvas::Color& c) {
    return "float3(" + lit(c.r) + ", " + lit(c.g) + ", " + lit(c.b) + ")";
}

bool finite_color(const canvas::Color& c) {
    return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b) && std::isfinite(c.a);
}

// Shared helpers every preset is built on. The light is baked as a constant
// direction; the Lambert term is divided by its value for a face toward the
// viewer, so a flat face reads exactly the authored colour and only tilted
// faces brighten or darken.
std::string prelude(const ChartShaderPresetOptions& o) {
    constexpr float kDegToRad = 3.14159265358979f / 180.0f;
    const float a = o.light_angle_deg * kDegToRad;
    const float e = o.light_elevation_deg * kDegToRad;
    // Screen space is y-down, so a light from above has a negative y.
    const float lx = std::cos(a) * std::cos(e);
    const float ly = -std::sin(a) * std::cos(e);
    const float lz = std::sin(e);
    return R"(
float pulpPresetU(PulpChart g) {
    return clamp(g.cross / max(g.halfWidth, 0.0001), -1.0, 1.0);
}
float pulpPresetCoverage(PulpChart g) {
    return g.valid > 0.5 ? clamp(0.5 - g.d / max(g.px, 0.0001), 0.0, 1.0) : 0.0;
}
float3 pulpPresetLinear(float3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, float3(2.4)), step(float3(0.04045), c));
}
float3 pulpPresetEncode(float3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055,
               step(float3(0.0031308), c));
}
float3 pulpPresetLight() {
    return float3()" +
           lit(lx) + ", " + lit(ly) + ", " + lit(lz) + R"();
}
float3 pulpPresetNormal(PulpChart g, float slope) {
    float2 n = float2(g.tan.y, -g.tan.x);
    return normalize(float3(-slope * n, 1.0));
}
float pulpPresetLambert(float3 N) {
    float3 L = pulpPresetLight();
    return (0.35 + 0.65 * max(dot(N, L), 0.0)) / (0.35 + 0.65 * L.z);
}
float pulpPresetSpecular(float3 N, float shininess) {
    float3 H = normalize(pulpPresetLight() + float3(0.0, 0.0, 1.0));
    return pow(max(dot(N, H), 0.0), shininess);
}
half4 pulpPresetOut(float3 linearRgb, float alpha) {
    return half4(half3(pulpPresetEncode(linearRgb) * alpha), half(alpha));
}
)";
}

// Height profile across the band (in half-width units) and its slope dh/du.
std::string profile_functions(ChartProfile profile) {
    switch (profile) {
    case ChartProfile::flat:
        return R"(
float pulpPresetSlope(float u) {
    float a = abs(u) - 0.8;
    if (a <= 0.0) return 0.0;
    a = min(a, 0.19);
    return -sign(u) * a / sqrt(0.04 - a * a);
}
float pulpPresetHeight(float u) {
    float a = max(abs(u) - 0.8, 0.0) / 0.2;
    return sqrt(max(1.0 - a * a, 0.0));
}
)";
    case ChartProfile::chamfer:
        return R"(
float pulpPresetSlope(float u) {
    return abs(u) > 0.6 ? -sign(u) : 0.0;
}
float pulpPresetHeight(float u) {
    return 1.0 - max(abs(u) - 0.6, 0.0) / 0.4;
}
)";
    case ChartProfile::round:
        break;
    }
    return R"(
float pulpPresetSlope(float u) {
    float s = clamp(u, -0.97, 0.97);
    return -s / sqrt(1.0 - s * s);
}
float pulpPresetHeight(float u) {
    return sqrt(max(1.0 - u * u, 0.0));
}
)";
}

std::string validate(const ChartShaderPresetOptions& o) {
    if (!finite_color(o.color))
        return "color must be finite";
    if (!finite_color(o.accent))
        return "accent must be finite";
    if (!std::isfinite(o.light_angle_deg))
        return "light angle must be finite";
    if (!(o.light_elevation_deg > 0.0f && o.light_elevation_deg <= 90.0f))
        return "light elevation must be in (0, 90] degrees";
    if (!(o.bevel_width > 0.0f && o.bevel_width <= 1.0f))
        return "bevel width must be in (0, 1]";
    if (!(o.intensity >= 0.0f) || !std::isfinite(o.intensity))
        return "intensity must be finite and >= 0";
    if (!(o.glow_sigma >= 0.0f) || !std::isfinite(o.glow_sigma))
        return "glow sigma must be finite and >= 0";
    return {};
}

std::string bevel(const ChartShaderPresetOptions& o) {
    // A raised plate falls away at its edges, so each edge faces outward; a
    // pressed one rises toward them and faces inward.
    const float relief = o.pressed ? -1.2f : 1.2f;
    return R"(
half4 shade(PulpChart g) {
    float u = pulpPresetU(g);
    float slope = abs(u) > )" +
           lit(1.0f - o.bevel_width) + " ? -sign(u) * " + lit(relief) +
           R"( : 0.0;
    float3 base = pulpPresetLinear()" +
           rgb_literal(o.color) + R"();
    return pulpPresetOut(base * pulpPresetLambert(pulpPresetNormal(g, slope)),
                         )" +
           lit(o.color.a) + R"( * pulpPresetCoverage(g));
}
)";
}

std::string inset_channel(const ChartShaderPresetOptions& o) {
    // A recessed track: its walls face inward and its floor sits in their
    // shadow, darker than the authored colour.
    const std::string edge = lit(1.0f - o.bevel_width);
    return R"(
half4 shade(PulpChart g) {
    float u = pulpPresetU(g);
    float slope = abs(u) > )" +
           edge + R"( ? sign(u) * 1.2 : 0.0;
    float occlusion = mix(0.62, 1.0, smoothstep()" +
           edge + R"( - 0.12, )" + edge +
           R"(, abs(u)));
    float3 base = pulpPresetLinear()" +
           rgb_literal(o.color) + R"();
    return pulpPresetOut(base * pulpPresetLambert(pulpPresetNormal(g, slope)) * occlusion,
                         )" +
           lit(o.color.a) + R"( * pulpPresetCoverage(g));
}
)";
}

std::string tube(const ChartShaderPresetOptions& o) {
    return profile_functions(o.profile) + R"(
half4 shade(PulpChart g) {
    float3 N = pulpPresetNormal(g, pulpPresetSlope(pulpPresetU(g)));
    float3 base = pulpPresetLinear()" +
           rgb_literal(o.color) + R"();
    float3 c = base * pulpPresetLambert(N) +
               float3()" +
           lit(0.45f * o.intensity) + R"( * pulpPresetSpecular(N, 48.0));
    return pulpPresetOut(c, )" +
           lit(o.color.a) + R"( * pulpPresetCoverage(g));
}
)";
}

std::string light_pipe(const ChartShaderPresetOptions& o) {
    // Emission scales with how much of the pipe a ray crosses (the profile's
    // height); the polished surface adds a specular glint toward the light.
    return profile_functions(o.profile) + R"(
half4 shade(PulpChart g) {
    float u = pulpPresetU(g);
    float3 N = pulpPresetNormal(g, pulpPresetSlope(u));
    float3 core = pulpPresetLinear()" +
           rgb_literal(o.accent) + R"() *
                  ()" +
           lit(o.intensity) + R"( * (0.25 + 0.75 * pulpPresetHeight(u)));
    float3 c = core + float3()" +
           lit(0.5f * o.intensity) + R"( * pulpPresetSpecular(N, 24.0));
    return pulpPresetOut(c, )" +
           lit(o.accent.a) + R"( * pulpPresetCoverage(g));
}
)";
}

std::string centre_highlight(const ChartShaderPresetOptions& o) {
    return R"(
half4 shade(PulpChart g) {
    float u = pulpPresetU(g);
    float3 base = pulpPresetLinear()" +
           rgb_literal(o.color) + R"();
    float3 lift = pulpPresetLinear()" +
           rgb_literal(o.accent) + R"() *
                  ()" +
           lit(o.intensity) + R"( * exp(-u * u / 0.08));
    return pulpPresetOut(base + lift, )" +
           lit(o.color.a) + R"( * pulpPresetCoverage(g));
}
)";
}

std::string glow(const ChartShaderPresetOptions& o) {
    // An opaque core plus a Gaussian falloff on the outside of the band's
    // distance field; nothing is blurred, so it costs one shader evaluation.
    const float s = o.glow_sigma;
    const std::string halo = s > 0.0f ? "g.d > 0.0 ? " + lit(o.intensity) + " * exp(-g.d * g.d / " +
                                            lit(2.0f * s * s) + ") : 0.0"
                                      : std::string("0.0");
    return R"(
half4 shade(PulpChart g) {
    float halo = g.valid > 0.5 ? ()" +
           halo + R"() : 0.0;
    float alpha = max(pulpPresetCoverage(g), min(halo, 1.0)) * )" +
           lit(o.accent.a) + R"(;
    return pulpPresetOut(pulpPresetLinear()" +
           rgb_literal(o.accent) + R"(), alpha);
}
)";
}

} // namespace

const std::vector<std::string>& chart_shader_preset_names() {
    static const std::vector<std::string> names = {
        "bevel", "inset-channel", "tube", "light-pipe", "centre-highlight", "glow",
    };
    return names;
}

bool parse_chart_profile(std::string_view name, ChartProfile& out) {
    if (name == "round")
        out = ChartProfile::round;
    else if (name == "flat")
        out = ChartProfile::flat;
    else if (name == "chamfer")
        out = ChartProfile::chamfer;
    else
        return false;
    return true;
}

ChartShaderPreset make_chart_shader_preset(std::string_view name,
                                           const ChartShaderPresetOptions& options) {
    ChartShaderPreset preset;
    std::string body;
    if (name == "bevel")
        body = bevel(options);
    else if (name == "inset-channel")
        body = inset_channel(options);
    else if (name == "tube")
        body = tube(options);
    else if (name == "light-pipe")
        body = light_pipe(options);
    else if (name == "centre-highlight" || name == "center-highlight")
        body = centre_highlight(options);
    else if (name == "glow")
        body = glow(options);
    else {
        preset.error = "Unknown chart shader preset '" + std::string(name) + "'; expected one of:";
        for (const auto& known : chart_shader_preset_names())
            preset.error += " " + known;
        return preset;
    }
    if (auto error = validate(options); !error.empty()) {
        preset.error = "Chart shader preset '" + std::string(name) + "': " + error;
        return preset;
    }
    preset.sksl = prelude(options) + body;
    if (name == "glow")
        preset.reach = 3.0f * options.glow_sigma;
    return preset;
}

} // namespace pulp::view
