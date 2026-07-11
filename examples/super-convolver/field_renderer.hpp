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

// Draw one frame of the field into [x,y,w,h]. `flow` 0..1 = motion; `density` =
// emitter count; `energy` 0..1 = overall audio level (brightens + swells).
// Does NOT paint a background — caller fills the ground first.
inline void draw_acoustic_field(canvas::Canvas& c, float x, float y, float w, float h,
                                double t_seconds, float flow, int density, float energy) {
    static const FieldRGB PAL[5] = {{220, 230, 240}, {239, 226, 201}, {175, 206, 220},
                                    {231, 199, 154}, {206, 214, 226}};
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    const float fx = w * 0.46f, fy = h * 0.46f;
    const float dpr = 1.0f;  // caller works in device px already
    const float e0 = 0.55f + 0.45f * energy;  // global brightness from audio

    c.set_blend_mode(canvas::Canvas::BlendMode::lighter);
    // Atmospheric haze from the Source at center.
    detail::field_glow(c, cx, cy, std::fmax(fx, fy) * 1.15f, FieldRGB{50, 64, 82}, 20);

    const int n = density < 1 ? 1 : density;
    for (int k = 0; k < n; ++k) {
        const float a = field_hash(k * 7 + 1) * 6.2831853f;
        const float rr = std::sqrt(field_hash(k * 13 + 2));
        float sx = std::cos(a) * rr * 1.02f, sy = std::sin(a) * rr * 0.95f;
        const FieldRGB col = PAL[k % 5];
        const float z = 0.3f + 0.7f * field_hash(k * 5 + 3);
        const float energy_k = e0 * (0.55f + 0.45f *
            std::fabs(std::sin(6.2831853f * field_hash(k * 17 + 4) * 3.1f +
                               k + static_cast<float>(t_seconds) * 0.6f)));

        // Tracer streak: integrate the flow a few steps, dropping fading dots.
        for (int s = 0; s < 10; ++s) {
            float vx, vy; field_flow_at(sx, sy, t_seconds, vx, vy);
            sx += vx * 0.03f * flow; sy += vy * 0.03f * flow;
            const float tt = 1.0f - s / 10.0f;
            const int aA = static_cast<int>(30 * tt * energy_k);
            const float rad = (0.8f + 1.6f * z) * tt * dpr;
            c.set_fill_color(canvas::Color::rgba8(col.r, col.g, col.b,
                                                  static_cast<std::uint8_t>(aA)));
            c.fill_circle(cx + sx * fx, cy + sy * fy, rad);
        }
        const float hx = cx + sx * fx, hy = cy + sy * fy;
        const float halo = (5.0f + 8.0f * z) * (0.72f + 0.5f * energy_k) * dpr;
        detail::field_glow(c, hx, hy, halo * 2.8f, col, static_cast<int>(40 + 65 * energy_k));
        c.set_fill_color(canvas::Color::rgba8(col.r, col.g, col.b,
                                              static_cast<std::uint8_t>(80 + 80 * energy_k)));
        c.fill_circle(hx, hy, halo * 0.5f);
        c.set_fill_color(canvas::Color::rgba8(255, 255, 255,
                                              static_cast<std::uint8_t>(60 + 85 * energy_k)));
        c.fill_circle(hx, hy, halo * 0.2f);
    }
    c.set_blend_mode(canvas::Canvas::BlendMode::normal);
}

}  // namespace pulp::superconvolver
