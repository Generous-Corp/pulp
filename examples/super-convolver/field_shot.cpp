// Native render of the SuperConvolver "acoustic field" concept using Pulp's
// real canvas (Skia raster capture) — a first proof that the browser concept
// translates to the plugin's own renderer. Headless, no window, no audio.
//
//   super-convolver-field-shot [out.png]
//
// This is a still frame of the field: emitters as soft airy disks with tracer
// streaks along a coherent flow, restrained palette, dark ground. It is a
// visual target for the editor rebuild (Phase B), not the editor itself.

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace pulp;
using canvas::Color;

namespace {

float frac_hash(int k) {
    float x = std::sin(k * 127.1f + 0.7f) * 43758.5453f;
    return x - std::floor(x);
}

// One coherent flow field — drifting vortices + turbulence (frozen at t=0 for a
// still). All emitters sample this, so nearby tracers share motion.
void flow_at(float x, float y, float& vx, float& vy) {
    struct V { float cx, cy, s; };
    static const V vs[3] = {{0.0f, 0.4f, 0.15f}, {0.55f, -0.35f, -0.12f}, {-0.5f, 0.1f, 0.10f}};
    vx = 0; vy = 0;
    for (const auto& v : vs) {
        float dx = x - v.cx, dy = y - v.cy, r2 = dx * dx + dy * dy + 0.07f;
        vx += v.s * (-dy) / r2; vy += v.s * (dx) / r2;
    }
    vx += 0.32f * std::sin(1.7f * y) + 0.18f * std::cos(1.1f * (x + y));
    vy += 0.32f * std::cos(1.6f * x) + 0.18f * std::sin(1.3f * (x - y));
}

// Restrained, near-monochrome palette (cool white / champagne + rare accents).
struct RGB { uint8_t r, g, b; };
const RGB PAL[5] = {{220, 230, 240}, {239, 226, 201}, {175, 206, 220},
                    {231, 199, 154}, {206, 214, 226}};

class FieldView : public view::View {
public:
    int density = 72;
    float flow = 0.62f;

    void paint(canvas::Canvas& c) override {
        const auto b = local_bounds();
        const float W = b.width, H = b.height, cx = W * 0.5f, cy = H * 0.5f;
        const float fx = W * 0.46f, fy = H * 0.44f;

        // Ground.
        c.set_fill_color(Color::rgba8(5, 6, 8));
        c.fill_rect(0, 0, W, H);
        // Everything luminous composites additively (SkBlendMode::kPlus), so
        // overlaps bloom to white on their own — real glow, not alpha stacking.
        c.set_blend_mode(canvas::Canvas::BlendMode::lighter);
        // Atmospheric haze from the Source at center.
        drawGlow(c, cx, cy, std::fmax(fx, fy) * 1.15f, RGB{50, 64, 82}, 20);

        for (int k = 0; k < density; ++k) {
            const float a = frac_hash(k * 7 + 1) * 6.2831853f;
            const float rr = std::sqrt(frac_hash(k * 13 + 2));
            float px = std::cos(a) * rr * 1.02f, py = std::sin(a) * rr * 0.95f;
            const RGB col = PAL[k % 5];
            const float z = 0.3f + 0.7f * frac_hash(k * 5 + 3);
            const float energy = 0.45f + 0.55f * std::fabs(std::sin(6.2831853f * frac_hash(k * 17 + 4) * 3.1f + k));

            // Tracer streak: integrate the flow a few steps, dropping fading dots.
            float sx = px, sy = py;
            for (int s = 0; s < 10; ++s) {
                float vx, vy; flow_at(sx, sy, vx, vy);
                sx += vx * 0.03f * flow; sy += vy * 0.03f * flow;
                const float tt = 1.0f - s / 10.0f;
                const int aA = static_cast<int>(30 * tt * energy);
                const float rad = (0.8f + 1.6f * z) * tt;
                c.set_fill_color(Color::rgba8(col.r, col.g, col.b, static_cast<uint8_t>(aA)));
                c.fill_circle(cx + sx * fx, cy + sy * fy, rad);
            }
            // The emitter head: a colored halo + a bright core. Additive blend
            // makes overlaps bloom to white on their own.
            const float hx = cx + sx * fx, hy = cy + sy * fy;
            const float halo = (5.0f + 8.0f * z) * (0.72f + 0.5f * energy);
            drawGlow(c, hx, hy, halo * 2.8f, col, static_cast<int>(40 + 65 * energy));
            c.set_fill_color(Color::rgba8(col.r, col.g, col.b, static_cast<uint8_t>(80 + 80 * energy)));
            c.fill_circle(hx, hy, halo * 0.5f);
            c.set_fill_color(Color::rgba8(255, 255, 255, static_cast<uint8_t>(60 + 85 * energy)));
            c.fill_circle(hx, hy, halo * 0.2f);
        }
        c.set_blend_mode(canvas::Canvas::BlendMode::normal);
    }

private:
    static void drawGlow(canvas::Canvas& c, float x, float y, float radius, RGB col, int peakA) {
        const Color cols[3] = {Color::rgba8(col.r, col.g, col.b, static_cast<uint8_t>(peakA)),
                               Color::rgba8(col.r, col.g, col.b, static_cast<uint8_t>(peakA * 0.32f)),
                               Color::rgba8(col.r, col.g, col.b, 0)};
        const float pos[3] = {0.0f, 0.38f, 1.0f};
        c.set_fill_gradient_radial(x, y, radius, cols, pos, 3);
        c.fill_circle(x, y, radius);
    }
};

}  // namespace

int main(int argc, char** argv) {
    const char* out = (argc > 1) ? argv[1] : "/tmp/super_convolver_field.png";
    FieldView view;
    view.set_bounds({0, 0, 960, 600});
    const bool ok = view::render_to_file(view, 960, 600, out, 2.0f,
                                         view::ScreenshotBackend::skia);
    std::printf("SuperConvolver field: %s -> %s\n", ok ? "OK" : "FAILED", out);
    return ok ? 0 : 1;
}
