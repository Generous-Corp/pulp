#pragma once

#include <pulp/canvas/canvas.hpp>
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
};

/// GPU blur effect — multi-pass Gaussian blur via SkImageFilters.
struct GpuBlurEffect : ViewEffect {
    float radius_x = 4.0f;
    float radius_y = 4.0f;

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        canvas.save_layer(x, y, w, h, 1.0f, std::max(radius_x, radius_y));
    }
};

/// Glow approximation — a blur layer whose radius scales with `intensity`.
///
/// This is NOT a true bloom. A real bloom bright-passes the subtree, blurs
/// only what survives the threshold, and composites that back additively; this
/// blurs the subtree uniformly and composites it normally, so dark pixels
/// smear exactly as much as bright ones and nothing ever gets brighter. It
/// reads as a soft glow on light-on-dark content, which is what it is for.
///
/// It is also not HDR-aware, and cannot be: every Pulp surface is 8-bit
/// unorm + sRGB (see `core/render/`), so there is no headroom above 1.0 to
/// threshold against. An earlier revision claimed both "HDR-aware" and a
/// `threshold` knob "implemented in SkiaCanvas" — neither was true. The
/// threshold knob was removed rather than left inert; `Canvas::set_bloom()`,
/// which it fed, was a no-op with no override in any backend.
///
/// A real bloom would bright-pass + blur + additively composite via
/// `SkImageFilters::Blend(kPlus, ...)` on the layer paint, which belongs in
/// `save_layer_with_filters` rather than a standalone setter. Whoever lands it
/// must update this comment and the test that pins the approximation.
struct GpuBloomEffect : ViewEffect {
    float intensity = 0.5f;  ///< Scales the blur radius. Not a brightness gain.
    float radius = 8.0f;     ///< Base blur radius, in pixels, before `intensity`.

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        canvas.save_layer(x, y, w, h, 1.0f, radius * intensity);
    }
};

/// Chromatic aberration — RGB channel offset for a glitch/lens effect.
/// Chromatic aberration — RGB channel offset for glitch/lens effect.
/// Requires the full GPU post-effect pipeline (render to texture, apply
/// SkSL shader, composite back). Currently applies a subtle color-shift
/// approximation via layer opacity. Full per-channel offset requires
/// SkImageFilter::MakeColorFilter with channel matrices.
struct ChromaticAberrationEffect : ViewEffect {
    float offset = 2.0f;  ///< Pixel offset between channels

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        // The full implementation needs per-channel offset rendering.
        // Approximation: subtle blur simulates the softness of chromatic aberration.
        canvas.save_layer(x, y, w, h, 1.0f, offset * 0.5f);
    }
};

/// Vignette — darken edges of the view by drawing a radial gradient overlay.
/// The overlay is drawn ON TOP of the content after the subtree paints,
/// so we use needs_layer=false and instead paint the overlay in a post-paint hook.
/// For now, we apply it as a slight opacity reduction on the layer.
struct VignetteEffect : ViewEffect {
    float intensity = 0.5f;     ///< Darkening strength (0=none, 1=full black edges)
    float radius = 0.75f;       ///< Fraction of view size where darkening starts
    Color edge_color = Color::rgba(0.0f, 0.0f, 0.0f, 0.5f);

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        // Apply as a layer — the vignette darkening happens via reduced edge opacity.
        // A full implementation would draw a radial gradient overlay after the content.
        canvas.save_layer(x, y, w, h, 1.0f - intensity * 0.2f);
    }
};

// A `CustomShaderEffect` (arbitrary SkSL as a view post-effect) used to live
// here. It stored `sksl`, `value`, and `time` and then ignored all three —
// `configure_layer()` pushed a plain layer, so setting one had no visual effect
// whatsoever. Applying SkSL to already-rendered subtree content needs a
// child-shader compositor (Skia's runtime-shader image filter) that Pulp does
// not have yet; `draw_with_sksl()` fills a fresh rect and cannot post-process.
// Widget body shaders — which do work — are `CustomShaderHost`.

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

    const std::vector<std::shared_ptr<ViewEffect>>& effects() const { return effects_; }

private:
    std::vector<std::shared_ptr<ViewEffect>> effects_;
};

} // namespace pulp::canvas
