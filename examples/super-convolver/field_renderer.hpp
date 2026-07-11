#pragma once

// The living acoustic field, drawn into a rectangle. One implementation shared
// by the still (field_shot.cpp) and the live plugin editor (super_convolver_ui).
//
// Stateless: emitter positions are a per-index hash advected by a shared coherent
// flow field evaluated at `t_seconds`, so the same arguments reproduce the same
// frame (and animation is just advancing t). Draws with real additive bloom
// (BlendMode::lighter → SkBlendMode::kPlus), so overlaps glow to white on their
// own. Restrained cool-white / champagne palette with rare cool/warm accents.

#include <pulp/canvas/canvas.hpp>

#include <cmath>
#include <cstdint>

namespace pulp::superconvolver {

struct FieldRGB { std::uint8_t r, g, b; };

inline float field_hash(int k) {
    float x = std::sin(k * 127.1f + 0.7f) * 43758.5453f;
    return x - std::floor(x);
}

// Coherent flow field (drifting vortices + turbulence). All emitters sample it,
// so nearby tracers move together — vortices, laminar streams.
inline void field_flow_at(float x, float y, double t, float& vx, float& vy) {
    struct V { float ax, ay, wx, wy, px, py, s; };
    static const V vs[3] = {{0.5f, 0.4f, 0.13f, 0.11f, 0.f, 1.f, 0.15f},
                            {0.62f, 0.5f, 0.09f, 0.16f, 2.f, 0.5f, -0.12f},
                            {0.32f, 0.58f, 0.18f, 0.08f, 4.f, 3.f, 0.10f}};
    vx = 0; vy = 0;
    const float tf = static_cast<float>(t);
    for (const auto& v : vs) {
        const float ccx = v.ax * std::sin(tf * v.wx + v.px);
        const float ccy = v.ay * std::cos(tf * v.wy + v.py);
        const float dx = x - ccx, dy = y - ccy, r2 = dx * dx + dy * dy + 0.07f;
        vx += v.s * (-dy) / r2; vy += v.s * (dx) / r2;
    }
    vx += 0.32f * std::sin(1.7f * y + tf * 0.3f) + 0.18f * std::cos(1.1f * (x + y) - tf * 0.22f);
    vy += 0.32f * std::cos(1.6f * x - tf * 0.26f) + 0.18f * std::sin(1.3f * (x - y) + tf * 0.2f);
}

namespace detail {
inline void field_glow(canvas::Canvas& c, float x, float y, float radius,
                       FieldRGB col, int peakA) {
    const canvas::Color cols[3] = {
        canvas::Color::rgba8(col.r, col.g, col.b, static_cast<std::uint8_t>(peakA)),
        canvas::Color::rgba8(col.r, col.g, col.b, static_cast<std::uint8_t>(peakA * 0.32f)),
        canvas::Color::rgba8(col.r, col.g, col.b, 0)};
    const float pos[3] = {0.0f, 0.38f, 1.0f};
    c.set_fill_gradient_radial(x, y, radius, cols, pos, 3);
    c.fill_circle(x, y, radius);
}
}  // namespace detail

// Draw one frame of the field into [x,y,w,h] as many thin, dotted TRACER streaks
// that flow along a coherent field — Schlieren / dye-in-water / long-exposure,
// near-monochrome silver with rare cool/warm accents (not fat glowing dots).
// `flow` 0..1 sets streak length (motion); `density` scales tracer count;
// `energy` 0..1 brightens. Additive. Caller fills the ground first.
inline void draw_acoustic_field(canvas::Canvas& c, float x, float y, float w, float h,
                                double t_seconds, float flow, int density, float energy) {
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    const float fx = w * 0.54f, fy = h * 0.54f;  // fill most of the frame
    const float bright = 0.55f + 0.45f * energy;

    c.set_blend_mode(canvas::Canvas::BlendMode::lighter);
    // Faint atmospheric vignette (not a hot central glow).
    detail::field_glow(c, cx, cy, std::fmax(fx, fy) * 1.25f, FieldRGB{34, 42, 56}, 16);

    const int d = density < 1 ? 1 : density;
    const int tracers = 220 + d * 10;                // dense; grows with density
    const int steps = 30;
    const float slen = 0.006f + 0.032f * flow;       // streak length grows with Flow

    for (int k = 0; k < tracers; ++k) {
        // Seed across the whole frame (slightly beyond so streaks enter/leave).
        float sx = field_hash(k * 3 + 1) * 2.3f - 1.15f;
        float sy = field_hash(k * 7 + 2) * 2.1f - 1.05f;
        // Palette: mostly silver; rare low-saturation cool/warm accents.
        const float acc = field_hash(k * 13 + 5);
        const FieldRGB col = acc < 0.06f ? FieldRGB{196, 214, 230}
                           : acc < 0.11f ? FieldRGB{228, 210, 182}
                                         : FieldRGB{234, 240, 248};
        const float gain = bright * (0.85f + 0.7f * field_hash(k * 23 + 8));

        // Integrate a dotted streak through the flow; faint tail → bright head.
        for (int s = 0; s < steps; ++s) {
            float vx, vy; field_flow_at(sx, sy, t_seconds, vx, vy);
            sx += vx * slen; sy += vy * slen;
            const float frac = static_cast<float>(s) / steps;   // 0 tail → 1 head
            const int a = static_cast<int>((14.0f + 105.0f * frac) * gain);
            const float rad = 0.5f + 1.05f * frac;
            c.set_fill_color(canvas::Color::rgba8(col.r, col.g, col.b,
                                                  static_cast<std::uint8_t>(a > 255 ? 255 : a)));
            c.fill_circle(cx + sx * fx, cy + sy * fy, rad);
        }
        // Bright head — a hot point that makes the streak read.
        const float hx = cx + sx * fx, hy = cy + sy * fy;
        c.set_fill_color(canvas::Color::rgba8(col.r, col.g, col.b,
                                              static_cast<std::uint8_t>(std::min(255.0f, 120.0f * gain))));
        c.fill_circle(hx, hy, 1.6f);
        c.set_fill_color(canvas::Color::rgba8(255, 255, 255,
                                              static_cast<std::uint8_t>(std::min(255.0f, 150.0f * gain))));
        c.fill_circle(hx, hy, 0.9f);
    }
    c.set_blend_mode(canvas::Canvas::BlendMode::normal);
}

}  // namespace pulp::superconvolver
