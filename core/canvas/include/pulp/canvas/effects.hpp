#pragma once

#include <pulp/canvas/canvas.hpp>
#include <memory>

namespace pulp::canvas {

// Post-processing effects for the Canvas rendering pipeline
// These can be applied to any Canvas backend (Skia GPU, CoreGraphics, etc.)
// When using Skia, they map to SkImageFilter for GPU-accelerated processing

// ── Effect types ─────────────────────────────────────────────────────────────

// Gaussian blur effect
struct BlurEffect {
    float radius_x = 4.0f;  // Horizontal blur radius in pixels
    float radius_y = 4.0f;  // Vertical blur radius in pixels
};

// Drop shadow effect
struct ShadowEffect {
    float offset_x = 2.0f;
    float offset_y = 2.0f;
    float blur_radius = 4.0f;
    Color color = Color::rgba(0.0f, 0.0f, 0.0f, 0.502f);  // Semi-transparent black
};

// Bloom/glow effect (bright areas bleed outward)
struct BloomEffect {
    float threshold = 0.8f;  // Brightness threshold (0-1)
    float intensity = 0.5f;  // Bloom intensity
    float radius = 8.0f;     // Blur radius for bloom
};

// Color adjustment
struct ColorAdjust {
    float brightness = 0.0f;   // -1 to +1
    float contrast = 1.0f;     // 0 to 2 (1 = normal)
    float saturation = 1.0f;   // 0 to 2 (1 = normal)
    float opacity = 1.0f;      // 0 to 1
};

// ── Effect application ───────────────────────────────────────────────────────

// Apply a blur effect to subsequent drawing operations
// Call before drawing, restore canvas state after
void apply_blur(Canvas& canvas, const BlurEffect& effect);

// Apply a drop shadow to subsequent drawing
void apply_shadow(Canvas& canvas, const ShadowEffect& effect);

// Apply color adjustment to the entire canvas
void apply_color_adjust(Canvas& canvas, const ColorAdjust& adjust);

// ── Save layer with effect ───────────────────────────────────────────────────

// Begin a layer that will have an effect applied when restored
// Usage: begin_effect_layer(canvas, blur); draw...; end_effect_layer(canvas);
void begin_effect_layer(Canvas& canvas, const BlurEffect& effect);
void begin_effect_layer(Canvas& canvas, const ShadowEffect& effect);
void end_effect_layer(Canvas& canvas);

} // namespace pulp::canvas
