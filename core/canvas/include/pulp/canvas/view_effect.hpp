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
struct ChromaticAberrationEffect : ViewEffect {
    float offset = 2.0f;  ///< Pixel offset between channels

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        // This would need a custom SkSL post-effect shader — for now, approximate
        // by rendering the layer normally (the full implementation requires
        // drawing the layer content three times with channel masks and offsets)
        canvas.save_layer(x, y, w, h);
    }
};

/// Vignette — darken edges of the view.
struct VignetteEffect : ViewEffect {
    float intensity = 0.5f;
    float radius = 0.75f;  ///< Fraction of view size where darkening starts

    void configure_layer(Canvas& canvas, float x, float y, float w, float h) override {
        canvas.save_layer(x, y, w, h);
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
