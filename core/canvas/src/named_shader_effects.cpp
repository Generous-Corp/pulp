// named_shader_effects.cpp — curated named GPU post-effect library.
//
// See named_shader_effects.hpp for the contract. Each effect is an SkSL
// runtime shader with a single `uniform shader content;` child that
// SkImageFilters::RuntimeShader auto-binds to the layer's rendered content.
// Effects read the source pixel via `content.eval(coord)`, apply a bounded
// modulation, and return a premultiplied result.
//
// Uniform contract shared by every effect:
//   uniform shader content;    // required — bound to the layer content
//   uniform float2 resolution; // layer size in px (set by the host)
//   uniform float  intensity;  // 0..1, clamped host-side
//   uniform float  time;       // optional seconds; 0 = static (no anim loop)
//
// Determinism: no effect depends on wall-clock unless the host passes a
// non-zero `time`; the static default is fully deterministic, which keeps
// the headless render tests bit-stable.

#ifdef PULP_HAS_SKIA

#include "named_shader_effects.hpp"

#include "include/core/SkM44.h"      // SkV2 (resolution uniform)
#include "include/core/SkString.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkRuntimeEffect.h"

#include "runtime_effect_cache.hpp"

#include <algorithm>
#include <cmath>

namespace pulp::canvas {

namespace {

// ── CRT: scanlines + barrel curvature + vignette + aperture-grille mask ──
// The signature retro-tube look. Curvature warps the sample coordinate
// (bounded by intensity); coords outside the tube read as black bezel.
constexpr char kCrtSkSL[] = R"(
    uniform shader content;
    uniform float2 resolution;
    uniform float intensity;

    half4 main(float2 coord) {
        float2 uv = coord / resolution;
        // Barrel curvature — bounded pull toward the tube center.
        float2 cc = uv - 0.5;
        float r2 = dot(cc, cc);
        float k = 0.12 * intensity;
        float2 warped = uv + cc * r2 * k;
        // Tube bezel: anything warped off-screen is opaque black.
        if (warped.x < 0.0 || warped.x > 1.0 ||
            warped.y < 0.0 || warped.y > 1.0) {
            return half4(0.0, 0.0, 0.0, 1.0);
        }
        half4 col = content.eval(warped * resolution);
        // Scanlines — darken alternate device rows. floor()-based so the
        // pattern survives half-pixel sample centers (a cos(pi*y) form
        // aliases to a constant at y = N + 0.5).
        float line = mod(floor(coord.y), 2.0);   // 0 even rows, 1 odd rows
        float scanAmt = mix(1.0, mix(0.55, 1.0, line), intensity);
        col.rgb *= half(scanAmt);
        // Aperture grille — subtle per-column RGB emphasis.
        float c3 = mod(coord.x, 3.0);
        half3 mask = half3(0.95);
        if (c3 < 1.0)      mask = half3(1.05, 0.95, 0.95);
        else if (c3 < 2.0) mask = half3(0.95, 1.05, 0.95);
        else               mask = half3(0.95, 0.95, 1.05);
        col.rgb *= mix(half3(1.0), mask, half(intensity));
        // Vignette — gentle edge falloff.
        float vig = smoothstep(0.9, 0.1, r2 * 2.0);
        col.rgb *= half(mix(1.0, vig, intensity * 0.6));
        return col;
    }
)";

// ── Film grain: static per-pixel luminance noise ────────────────────────
constexpr char kGrainSkSL[] = R"(
    uniform shader content;
    uniform float2 resolution;
    uniform float intensity;
    uniform float time;

    float hash(float2 p) {
        p = fract(p * float2(123.34, 456.21));
        p += dot(p, p + 45.32);
        return fract(p.x * p.y);
    }

    half4 main(float2 coord) {
        half4 col = content.eval(coord);
        float n = hash(coord + time) - 0.5;       // -0.5..0.5
        float amt = intensity * 0.35;
        // Premultiplied: scale the additive term by alpha so rgb <= a holds.
        col.rgb += half3(half(n * amt)) * col.a;
        col.rgb = clamp(col.rgb, half3(0.0), half3(col.a));
        return col;
    }
)";

// ── Vignette: radial edge darkening ─────────────────────────────────────
constexpr char kVignetteSkSL[] = R"(
    uniform shader content;
    uniform float2 resolution;
    uniform float intensity;

    half4 main(float2 coord) {
        half4 col = content.eval(coord);
        float2 cc = coord / resolution - 0.5;
        float d = length(cc) * 1.41421356;         // 0 center .. 1 corner
        float vig = smoothstep(1.0, 0.3, d);
        col.rgb *= half(mix(1.0, vig, intensity));
        return col;
    }
)";

// ── Noise: isotropic procedural value-noise overlay ─────────────────────
constexpr char kNoiseSkSL[] = R"(
    uniform shader content;
    uniform float2 resolution;
    uniform float intensity;
    uniform float time;

    float hash(float2 p) {
        p = fract(p * float2(123.34, 456.21));
        p += dot(p, p + 45.32);
        return fract(p.x * p.y);
    }
    float vnoise(float2 p) {
        float2 i = floor(p);
        float2 f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
        float a = hash(i);
        float b = hash(i + float2(1.0, 0.0));
        float c = hash(i + float2(0.0, 1.0));
        float d = hash(i + float2(1.0, 1.0));
        return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    }

    half4 main(float2 coord) {
        half4 col = content.eval(coord);
        float n = vnoise(coord * 0.15 + time) - 0.5;
        float amt = intensity * 0.30;
        col.rgb += half3(half(n * amt)) * col.a;
        col.rgb = clamp(col.rgb, half3(0.0), half3(col.a));
        return col;
    }
)";

// ── Brushed metal: high-frequency horizontal streaks ────────────────────
constexpr char kBrushedSkSL[] = R"(
    uniform shader content;
    uniform float2 resolution;
    uniform float intensity;

    float hash(float2 p) {
        p = fract(p * float2(123.34, 456.21));
        p += dot(p, p + 45.32);
        return fract(p.x * p.y);
    }

    half4 main(float2 coord) {
        half4 col = content.eval(coord);
        // Streaks vary quickly along x, slowly along y -> horizontal brushing.
        float s1 = hash(float2(floor(coord.x * 0.7), floor(coord.y * 0.04)));
        float s2 = hash(float2(floor(coord.x * 2.3), floor(coord.y * 0.02)));
        float n = ((s1 * 0.7 + s2 * 0.3) - 0.5) * 2.0;
        float amt = intensity * 0.14;
        col.rgb += half3(half(n * amt)) * col.a;
        col.rgb = clamp(col.rgb, half3(0.0), half3(col.a));
        return col;
    }
)";

// ── Bloom: threshold + small-kernel glow (neon / tube) ──────────────────
// Offset-samples the content around each pixel, keeps luminance above a
// soft threshold, and adds it back. `content` must be sampleable up to the
// kernel radius, so the host passes a matching sampleRadius.
constexpr char kBloomSkSL[] = R"(
    uniform shader content;
    uniform float2 resolution;
    uniform float intensity;

    half3 bright(half4 c) {
        float l = dot(c.rgb, half3(0.299, 0.587, 0.114));
        return c.rgb * half(smoothstep(0.55, 0.8, l));
    }

    half4 main(float2 coord) {
        half4 col = content.eval(coord);
        float rad = 3.0 + 5.0 * intensity;
        half3 sum = half3(0.0);
        sum += bright(content.eval(coord + float2( rad, 0.0)));
        sum += bright(content.eval(coord + float2(-rad, 0.0)));
        sum += bright(content.eval(coord + float2(0.0,  rad)));
        sum += bright(content.eval(coord + float2(0.0, -rad)));
        sum += bright(content.eval(coord + float2( rad,  rad)));
        sum += bright(content.eval(coord + float2(-rad,  rad)));
        sum += bright(content.eval(coord + float2( rad, -rad)));
        sum += bright(content.eval(coord + float2(-rad, -rad)));
        sum += bright(col) * half(2.0);
        sum *= half(0.1);
        col.rgb = clamp(col.rgb + sum * half(intensity * 1.5),
                        half3(0.0), half3(1.0));
        return col;
    }
)";

// Resolve a curated name to its SkSL source and the child-sampling radius
// its coordinate offsets require (0 for same-coord effects).
struct EffectDef {
    const char* src = nullptr;
    float sample_radius = 0.0f;
};

EffectDef resolve_effect(const std::string& name, float intensity,
                         float w, float h) {
    const float maxdim = std::max(w, h);
    if (name == "crt")      return {kCrtSkSL, 0.08f * maxdim};
    if (name == "grain")    return {kGrainSkSL, 0.0f};
    if (name == "vignette") return {kVignetteSkSL, 0.0f};
    if (name == "noise")    return {kNoiseSkSL, 0.0f};
    if (name == "brushed")  return {kBrushedSkSL, 0.0f};
    if (name == "bloom")    return {kBloomSkSL, (3.0f + 5.0f * intensity) * 1.5f + 2.0f};
    return {};  // unknown
}

} // namespace

bool is_known_shader_effect(const std::string& name) {
    return resolve_effect(name, 0.0f, 1.0f, 1.0f).src != nullptr;
}

sk_sp<SkImageFilter> make_named_shader_effect(const std::string& name,
                                              float intensity,
                                              float w, float h,
                                              float time) {
    if (w <= 0.0f || h <= 0.0f) return nullptr;
    intensity = std::clamp(intensity, 0.0f, 1.0f);

    EffectDef def = resolve_effect(name, intensity, w, h);
    if (!def.src) return nullptr;  // unknown name -> skip

    auto effect = RuntimeEffectCache::instance().get_or_compile(def.src);
    if (!effect) return nullptr;   // SkSL compile failure -> skip

    SkRuntimeShaderBuilder builder(effect);
    if (effect->findUniform("resolution"))
        builder.uniform("resolution") = SkV2{w, h};
    if (effect->findUniform("intensity"))
        builder.uniform("intensity") = intensity;
    if (effect->findUniform("time"))
        builder.uniform("time") = time;

    // input == nullptr -> the single child "content" auto-binds to the
    // implicit source image (the layer's rendered content).
    return SkImageFilters::RuntimeShader(builder, def.sample_radius,
                                         "content", /*input=*/nullptr);
}

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
