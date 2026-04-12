#pragma once

// SDF text effects: `glow`, `shadow`, `outline`, `bevel`. These mirror
// the uniforms exposed by `core/canvas/shaders/sdf_text_effects.sksl`
// so host code and SkSL shader share a single source of truth for the
// effect parameters.

#include <array>
#include <cstdint>

namespace pulp::canvas {

enum class SdfEffect : std::uint8_t {
    None    = 0,
    Outline = 1u << 0,
    Glow    = 1u << 1,
    Shadow  = 1u << 2,
    Bevel   = 1u << 3,
};

inline std::uint8_t operator|(SdfEffect a, SdfEffect b) {
    return static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b);
}
inline bool has_effect(std::uint8_t mask, SdfEffect e) {
    return (mask & static_cast<std::uint8_t>(e)) != 0;
}

struct SdfEffectParams {
    // Outline
    float outline_width = 0.0f;               // pixels
    std::array<float, 4> outline_color{0, 0, 0, 1};

    // Glow
    float glow_radius = 0.0f;                 // pixels of falloff
    std::array<float, 4> glow_color{1, 1, 1, 1};

    // Shadow
    float shadow_offset_x = 0.0f;
    float shadow_offset_y = 0.0f;
    float shadow_softness = 0.0f;
    std::array<float, 4> shadow_color{0, 0, 0, 0.5f};

    // Bevel — normalized light direction in image space.
    float bevel_light_x = 0.7071f;
    float bevel_light_y = -0.7071f;
    float bevel_mix = 0.0f;                   // 0 = off, 1 = full

    // Active effect mask (bitset of SdfEffect values).
    std::uint8_t active = 0;
};

// Design-token friendly presets. Matches the style-guide tiers in the
// Pulp design system: subtle (UI accents), bold (splash text),
// neon (callouts), pressed (tactile buttons).
inline SdfEffectParams preset_subtle_shadow() {
    SdfEffectParams p;
    p.active = static_cast<std::uint8_t>(SdfEffect::Shadow);
    p.shadow_offset_x = 0.0f;
    p.shadow_offset_y = 1.0f;
    p.shadow_softness = 0.5f;
    p.shadow_color = {0, 0, 0, 0.25f};
    return p;
}

inline SdfEffectParams preset_outline(float width = 2.0f) {
    SdfEffectParams p;
    p.active = static_cast<std::uint8_t>(SdfEffect::Outline);
    p.outline_width = width;
    p.outline_color = {0, 0, 0, 1};
    return p;
}

inline SdfEffectParams preset_glow(float radius = 6.0f,
                                    std::array<float, 4> color = {0.3f, 0.6f, 1.0f, 1.0f}) {
    SdfEffectParams p;
    p.active = static_cast<std::uint8_t>(SdfEffect::Glow);
    p.glow_radius = radius;
    p.glow_color  = color;
    return p;
}

inline SdfEffectParams preset_pressed_bevel() {
    SdfEffectParams p;
    p.active = static_cast<std::uint8_t>(SdfEffect::Bevel);
    p.bevel_mix = 0.5f;
    p.bevel_light_x = 0.0f;
    p.bevel_light_y = 1.0f;  // light from below = pressed look
    return p;
}

}  // namespace pulp::canvas
