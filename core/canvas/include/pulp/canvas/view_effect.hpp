#pragma once

#include <pulp/canvas/canvas.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace pulp::canvas {

/// Base class for GPU post-processing effects.
/// Effects are applied to a View's compositing layer (via save_layer/restore).
/// The Canvas abstraction handles the GPU-side compositing; concrete effects
/// configure the layer paint properties.
class ViewEffect {
public:
    virtual ~ViewEffect() = default;

    /// Push the compositing layer(s) this effect needs, configured with its
    /// opacity, image filters, blend mode, etc. The subtree paints into the
    /// layer(s); the caller pops them once the subtree is done.
    ///
    /// An implementation MUST push exactly `layer_count()` layers. The caller
    /// (`View::paint_all`) pops that many, so an effect that saves a different
    /// number than it reports unbalances the canvas save stack for the rest of
    /// the frame.
    virtual void configure_layer(Canvas& canvas, float x, float y, float w, float h) = 0;

    /// How many layers `configure_layer()` pushes. One for a simple effect;
    /// `EffectChain` reports the sum across its children.
    virtual int layer_count() const { return 1; }

    /// Whether this effect requires a compositing layer (most do).
    virtual bool needs_layer() const { return true; }

    /// Paint an overlay on TOP of the already-painted subtree, but still INSIDE
    /// the effect's compositing layer(s) (called by `View::paint_all` just
    /// before the layers are popped). This is how an effect that darkens or
    /// tints the finished content — a vignette drawing a radial gradient — is
    /// implemented, instead of the wrong "reduce the whole layer's opacity"
    /// approximation. Default: nothing.
    virtual void paint_overlay(Canvas& canvas, float x, float y,
                               float w, float h) {
        (void)canvas; (void)x; (void)y; (void)w; (void)h;
    }
};

/// GPU blur effect — multi-pass Gaussian blur via SkImageFilters.
struct GpuBlurEffect : ViewEffect {
    float radius_x = 4.0f;
    float radius_y = 4.0f;

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        canvas.save_layer(x, y, w, h, 1.0f, std::max(radius_x, radius_y));
    }
};

/// GPU bloom/glow effect — HDR-aware bloom (threshold + blur + additive blend).
/// Requires float-based color pipeline for proper HDR threshold.
struct GpuBloomEffect : ViewEffect {
    float threshold = 0.8f;
    float intensity = 0.5f;
    float radius = 8.0f;

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        // Real bloom: SkiaCanvas thresholds + blurs + additively composites the
        // glow; other backends degrade to a plain blurred layer (base default).
        canvas.save_layer_with_bloom(x, y, w, h, intensity, threshold, radius);
    }
};

/// Chromatic aberration — real per-channel RGB offset for a glitch/lens effect.
/// Post-processes the painted subtree via the SkSL child-shader compositor:
/// the red channel samples `offset` px to one side and blue `offset` px to the
/// other, so edges fringe red on one side and blue on the other. On non-Skia
/// backends the compositor falls back to a plain (unfiltered) layer.
struct ChromaticAberrationEffect : ViewEffect {
    float offset = 2.0f;  ///< Pixel offset between channels

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        Canvas::ShaderUniforms u;
        u.value = offset;  // per-channel offset (px), passed via `value`
        canvas.save_layer_with_sksl_post_effect(
            x, y, w, h,
            "uniform shader content;"
            "uniform float value;"
            "half4 main(float2 xy) {"
            "  return half4(content.eval(xy + float2(value, 0.0)).r,"
            "               content.eval(xy).g,"
            "               content.eval(xy - float2(value, 0.0)).b,"
            "               content.eval(xy).a);"
            "}",
            u, /*sample_radius=*/offset);
    }
};

/// Vignette — darken the edges of the view by drawing a real radial-gradient
/// overlay on top of the finished content: transparent at the center, fading to
/// `edge_color` (scaled by `intensity`) at the corners. The overlay is painted
/// in `paint_overlay()`, INSIDE the effect's compositing layer, so it darkens
/// the edges rather than uniformly fading the whole view (the old
/// approximation, which just reduced the layer opacity).
struct VignetteEffect : ViewEffect {
    float intensity = 0.5f;     ///< Darkening strength (0=none, 1=full edge_color)
    float radius = 0.75f;       ///< Fraction of view half-extent where darkening starts
    Color edge_color = Color::rgba(0.0f, 0.0f, 0.0f, 0.5f);

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        // Plain compositing layer; the darkening happens in paint_overlay().
        canvas.save_layer(x, y, w, h, 1.0f, 0.0f);
    }

    void paint_overlay(Canvas& canvas, float x, float y,
                       float w, float h) override {
        if (intensity <= 0.0f || w <= 0.0f || h <= 0.0f) return;
        const float cx = x + w * 0.5f;
        const float cy = y + h * 0.5f;
        const float outer = 0.5f * std::sqrt(w * w + h * h);  // center→corner
        const float inner = radius * std::min(w, h) * 0.5f;   // darkening start
        const float inner_frac =
            outer > 0.0f ? std::min(inner / outer, 0.99f) : 0.0f;

        const Color clear = Color::rgba(edge_color.r, edge_color.g,
                                        edge_color.b, 0.0f);
        const Color edge = Color::rgba(edge_color.r, edge_color.g, edge_color.b,
                                       edge_color.a * intensity);
        const Color colors[3] = {clear, clear, edge};
        const float positions[3] = {0.0f, inner_frac, 1.0f};
        canvas.set_fill_gradient_radial(cx, cy, outer, colors, positions, 3);
        canvas.fill_rect(x, y, w, h);
        canvas.clear_fill_gradient();
    }
};

// A `CustomShaderEffect` (arbitrary SkSL as a view post-effect) used to live
// here. It stored `sksl`, `value`, and `time` and then ignored all three —
// `configure_layer()` pushed a plain layer, so setting one had no visual effect
// whatsoever. Applying SkSL to already-rendered subtree content needs a
// child-shader compositor (Skia's runtime-shader image filter), which now
// exists as `Canvas::save_layer_with_sksl_post_effect` (the SkSL declares
// `uniform shader content`). A view post-effect can be built on it; note that
// this is distinct from `draw_with_sksl()`, which fills a fresh rect from a
// generative shader and cannot post-process. Widget body shaders — which do
// work — are `CustomShaderHost`.

/// Chains multiple effects in sequence.
class EffectChain : public ViewEffect {
public:
    void add(std::shared_ptr<ViewEffect> effect) {
        effects_.push_back(std::move(effect));
    }

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        // Apply effects in order — each wraps the previous in a layer
        for (auto& effect : effects_) {
            effect->configure_layer(canvas, x, y, w, h);
        }
    }

    /// One layer per child, summed — a chain pushes as many layers as it has
    /// effects. Reporting 1 here (the base default) would leak every layer past
    /// the first on every paint.
    int layer_count() const override {
        int total = 0;
        for (const auto& effect : effects_) total += effect->layer_count();
        return total;
    }

    bool needs_layer() const override {
        return !effects_.empty();
    }

    /// Forward the overlay to every child (in order). Non-overlay effects have
    /// an empty paint_overlay, so only the overlay-drawing ones (vignette) act.
    void paint_overlay(Canvas& canvas, float x, float y,
                       float w, float h) override {
        for (auto& effect : effects_)
            effect->paint_overlay(canvas, x, y, w, h);
    }

    const std::vector<std::shared_ptr<ViewEffect>>& effects() const { return effects_; }

private:
    std::vector<std::shared_ptr<ViewEffect>> effects_;
};

} // namespace pulp::canvas
