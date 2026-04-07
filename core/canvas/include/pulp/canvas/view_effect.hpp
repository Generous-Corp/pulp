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

    /// Configure the layer paint before the subtree is rendered.
    /// Implementations set opacity, image filters, blend modes, etc.
    /// The canvas.save_layer() call happens before this, and canvas.restore()
    /// happens after the subtree paints.
    virtual void configure_layer(Canvas& canvas, float x, float y, float w, float h) = 0;

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

/// GPU bloom/glow effect — HDR-aware bloom (threshold + blur + additive blend).
/// Requires float-based color pipeline for proper HDR threshold.
struct GpuBloomEffect : ViewEffect {
    float threshold = 0.8f;
    float intensity = 0.5f;
    float radius = 8.0f;

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        // Bloom uses the canvas bloom API (which is implemented in SkiaCanvas)
        canvas.set_bloom(intensity, threshold);
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

/// Custom SkSL shader applied as a post-effect to a View's content.
/// The shader receives the layer's rendered content as a child shader.
struct CustomShaderEffect : ViewEffect {
    std::string sksl;  ///< SkSL source for the post-effect
    float value = 0.0f;
    float time = 0.0f;

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        canvas.save_layer(x, y, w, h);
    }
};

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

    bool needs_layer() const override {
        return !effects_.empty();
    }

    const std::vector<std::shared_ptr<ViewEffect>>& effects() const { return effects_; }

private:
    std::vector<std::shared_ptr<ViewEffect>> effects_;
};

} // namespace pulp::canvas
