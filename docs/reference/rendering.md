# Rendering Reference

Pulp's rendering pipeline is GPU-accelerated via Dawn (WebGPU) and Skia Graphite. This page covers the rendering infrastructure added for visual parity with purpose-built GPU UI frameworks.

## HDR Float Color

`Color` uses 4x `float` channels (0.0–1.0, >1.0 for HDR). Supports multiple color spaces:

```cpp
auto c = Color::rgba(0.5f, 0.8f, 1.0f);       // sRGB float
auto c = Color::rgba8(128, 200, 255);           // from uint8
auto c = Color::hex(0x3B82F6);                  // from hex

auto hsv = c.to_hsv();                          // Hue-Saturation-Value
auto hsl = c.to_hsl();                          // Hue-Saturation-Lightness
auto lch = c.to_oklch();                        // OKLCH (CSS Color Level 4)
auto hdr = c.with_hdr_intensity(2.0f);          // HDR overbright
auto mid = a.interpolate(b, 0.5f);              // Smooth blending
```

## SDF Shape Primitives

14 GPU-accelerated shapes via SkSL shaders with pixel-perfect anti-aliasing:

`rect`, `circle`, `rounded_rect`, `arc`, `diamond`, `squircle`, `triangle`, `ring`, `stadium`, `cross`, `flat_segment`, `rounded_segment`, `flat_arc`, `quadratic_bezier`

```cpp
Canvas::SDFStyle style;
style.fill_color = Color::rgba(0.2f, 0.6f, 1.0f);
style.corner_radius = 8.0f;
canvas.draw_sdf_shape(Canvas::SDFShape::squircle, x, y, w, h, style);
```

## Compositing Layers

Proper CSS opacity and filter:blur() via `save_layer()`:

```cpp
// Subtree paints into offscreen buffer, composited as single unit
canvas.save_layer(x, y, w, h, opacity, blur_radius);
// ... draw subtree ...
canvas.restore();
```

Works on both Skia (SkCanvas::saveLayer) and CoreGraphics (CGContextBeginTransparencyLayer).

## Post-Processing Effects

```cpp
view.set_effect(std::make_shared<GpuBlurEffect>());           // Gaussian blur
view.set_effect(std::make_shared<GpuBloomEffect>());          // HDR bloom
view.set_effect(std::make_shared<CustomShaderEffect>());      // Arbitrary SkSL
view.set_effect(std::make_shared<EffectChain>());             // Compose multiple
```

## DirtyTracker

Partial repaint optimization — only repaints changed regions:

```cpp
DirtyTracker tracker;
tracker.set_viewport(width, height, 0.6f);  // Full repaint if >60% dirty
tracker.invalidate(x, y, w, h);             // Mark region dirty
if (tracker.needs_full_repaint()) { /* repaint all */ }
else { for (auto& rect : tracker.dirty_rects()) { /* repaint rect */ } }
tracker.clear();                             // After frame submit
```

## Draw-call batching

Pulp does not interpose an extra batcher between Canvas calls and the
GPU. Skia (raster and Graphite) already coalesces compatible draw calls
inside the active `SkCanvas` / `Recorder` — adding a Pulp-level batcher
on top would compete with, not improve on, Skia's own analysis.

If you need to *measure* effective batching for a frame, the right place
to hook in is the Skia recorder / GPU stats once the
`pulp-inspect` HUD wiring lands (tracked separately); raw
`SkCanvas` does not expose per-frame batch counts.

## Atlas Systems

- **ImageAtlas**: Packs small images into shared GPU texture with ref counting
- **GradientAtlas**: Caches evaluated gradient ramps by hash
- **GlyphAtlas**: Per-font-size glyph cache (supplements Skia's internal cache)
- **PathAtlas**: Caches rasterized vector paths
- **BufferPool\<T\>**: Reuses std::vector allocations in hot paths

## GPU Visualization

Waveform and spectrum views use GPU shaders for anti-aliased rendering:

```cpp
Canvas::WaveformStyle style;
style.line_color = wave_color;
style.fill_color = fill_color;
style.line_thickness = 2.0f;
canvas.draw_waveform(samples, count, x, y, w, h, style);
```

## GPU Render Time

Pulp can report **true GPU-side render time** alongside the CPU wall-time the
inspector has always shown. It is exposed on the Skia surface:

```cpp
auto* skia = /* pulp::render::SkiaSurface* */;
if (skia->gpu_render_timing_available()) {
    double ms = skia->gpu_render_time_ms();  // 0 until the first sample lands
}
```

**This is whole-recording GPU render time, not per-pass attribution.** That
distinction is deliberate and load-bearing:

- The number comes from Skia Graphite's own GPU-stats API
  (`InsertRecordingInfo::fGpuStatsFlags = kElapsedTime`), which measures the GPU
  elapsed time of the *recording Pulp submits each frame* — not Pulp's logical
  render passes (background / content / effects / overlay / post). Skia Graphite
  owns the Dawn command encoder and every render-pass descriptor, so Pulp cannot
  inject per-pass `timestampWrites`. True per-pass GPU attribution would require
  Skia to expose that granularity (or Pulp to own more of the render graph).
- WebGPU timestamp queries *can* measure command timing, but the API in use here
  surfaces recording-level elapsed time, not detailed per-pass attribution. See
  the WebGPU/Chromium discussion of this exact distinction:
  <https://groups.google.com/a/chromium.org/g/blink-dev/c/dtYJ0MQYMlU>.
- On the **Metal** backend (Apple platforms) Skia disables Dawn command-buffer
  timestamps and falls back to render/compute-pass timestamp writes, so the
  measurement spans first-pass-begin → last-pass-end of the recording and
  **excludes** non-pass work (texture uploads/copies) and `Present()`.

**Availability and honesty rules:**

- Requires the Dawn `timestamp-query` feature. Pulp requests it only when the
  adapter advertises it; otherwise GPU render time is reported **unavailable**.
- `gpu_render_timing_available()` reflects device/feature support; an
  unsupported platform reports unavailable rather than a fake `0`.
- A failed sample or a zero elapsed time is treated as "no sample" — the last
  good value is retained, never overwritten with a misleading `0`.

Design rationale and the per-pass feasibility analysis live in
`planning/2026-05-21-gpu-timestamp-readback-proposal.md`.

## Gradients

```cpp
// Conic (sweep) gradient — CSS conic-gradient equivalent
canvas.set_fill_gradient_conic(cx, cy, start_angle, colors, positions, count);

// FillStyle unifies solid, linear, radial, conic
FillStyle fill(ConicGradient{cx, cy, 0, stops});
fill.set_tile_mode(GradientTileMode::repeat);
```

## SpriteStrip Animation

Designer-created filmstrip knob/fader skins:

```cpp
auto strip = std::make_shared<SpriteStrip>();
strip->load(data, size, width, height, frame_count);
knob.set_sprite_strip(strip);  // Value selects frame
```

## Viewport-Relative Dimensions

```cpp
auto d = Dimension::parse("50vw");
float px = d.resolve(parent_size, viewport_w, viewport_h, dpi_scale);
// Units: px, %, vw, vh, vmin, vmax, auto
```

## ThemeEditor

Live theme editing widget:

```cpp
ThemeEditor editor;
editor.set_theme(Theme::dark());
editor.on_color_changed = [](const std::string& token, Color c) { /* update */ };
auto json = editor.export_json();
```

## Global Undo

```cpp
EditHistory history;
history.perform([&]{ value = 42; }, [&]{ value = 0; }, "set value");
history.undo();  // value = 0
history.redo();  // value = 42
// Coalescing: rapid changes with same description merge automatically
```

Integrates with parameter Bindings:
```cpp
binding.set_edit_history(&history);
binding.begin_gesture();  // Captures start value
// ... user drags knob ...
binding.end_gesture();    // Pushes undo action
```

## Platform Features

```cpp
window->set_mouse_relative_mode(true);     // Infinite knob drag
window->set_client_decoration(true);       // Custom title bar
window->set_fixed_aspect_ratio(16.0f/9);   // Constrained resize
window->set_always_on_top(true);           // Floating window
float dpi = window->dpi_scale();           // HiDPI
auto monitors = window->get_monitors();    // Multi-monitor enumeration
```

## PBR / 3D Pipeline

Compute pipeline for Three.js PBR materials:

- **Compute dispatch**: JS→C++ bridge creates Dawn compute pipelines and dispatches workgroups
- **Storage buffers**: Bind group serialization with GPU buffer creation
- **Cube textures**: 6-face textures with mip levels for environment maps
- **DRACO**: Native C++ mesh decoder (Apache 2.0, optional via `PULP_ENABLE_DRACO`)
- **KTX2**: Texture header parser with platform-optimal format detection (ASTC/BC7/ETC2)
- **Binary transfer**: Native buffer registration for zero-copy GPU upload

## Asset Embedding

```cmake
# In CMakeLists.txt:
pulp_embed_files(my-plugin FILES fonts/Inter.ttf icons/logo.svg)
```

```cpp
// In C++:
auto* font = EmbeddedAsset::get("Inter.ttf");
// font->data, font->size
```

Bundled fonts: Inter Regular, JetBrains Mono Regular (SIL OFL 1.1). The
exact versions, hashes, and fallback order are tracked in
[Text Shaping Determinism](text-shaping.md).
