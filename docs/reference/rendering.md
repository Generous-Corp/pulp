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

## Retained Paths — `pulp::canvas::Path`

`Path` is a geometry **value**. Before it, a shape could only exist *during* a
paint, because it was built by calling the canvas directly — so it could not be
measured before it was drawn, transformed, hit-tested, cached, or handed to
anything else.

```cpp
#include <pulp/canvas/path.hpp>

pulp::canvas::Path icon;
icon.move_to(0, 0).line_to(10, 0).cubic_to(12, 4, 12, 8, 10, 10).close();

icon.scale_to_fit(0, 0, 64, 64, /*preserve_proportions=*/true);

if (icon.contains(mouse)) { … }              // hit-test, no canvas needed
canvas.fill_path(icon, FillRule::nonzero);   // draw it
```

Copy is O(1) (copy-on-write) with real value semantics: assigning a `Path` is not
sharing one.

**Three details that are easy to get silently wrong, so the API makes them explicit:**

- **`bounds()` is TIGHT** — computed from each curve's extrema, not its control
  hull. The two differ for nearly every real curve, because a cubic's control
  points usually fall *outside* the curve they steer. Use `control_bounds()` when
  you want the cheap conservative hull instead (a repaint rect, where
  over-estimating costs a few redrawn pixels and under-estimating is an artifact).
- **`contains(p, rule)` takes a `FillRule`.** `nonzero` and `evenodd` genuinely
  disagree about a region enclosed twice, and that disagreement is the entire
  reason the rule is a parameter rather than a constant. `Canvas::fill_path` takes
  one too.
- **`scale_to_fit()` declines on a degenerate path.** Scaling a zero-width path
  (a vertical line, a single point) to a non-zero width is a division by zero, and
  the "result" is a path whose every coordinate is NaN — which renders as nothing
  at all, from anywhere, forever. It returns identity instead, leaving the path
  unchanged. With `preserve_proportions`, the slack on whichever axis did not bind
  is **centred**, not dumped at one edge.

Supporting types: `Point2D`, `AffineTransform` (`pulp/canvas/affine_transform.hpp`),
`FillRule`, `StrokeStyle`.

`pulp::view::Rect` gained **`encloses(other)`** for whole-rect containment. It is
deliberately **not** an overload of `contains()`: `Rect` has default member
initializers, so a braced `{50, 30}` is a valid two-field `Rect` as well as a valid
`Point` — and a `contains(const Rect&)` overload therefore makes the idiomatic
`r.contains({50, 30})` **ambiguous at every call site**. `contains(Point)` and
`contains(x, y)` are unchanged.

> `Point2D` is named that, and not `Point`, **on purpose**: Apple's `MacTypes.h`
> declares a global Carbon `Point` (and `Rect`), and a `pulp::canvas::Point` becomes
> ambiguous with it in every Objective-C++ translation unit in the view layer. Don't
> rename it back.

## Compositing Layers

Proper CSS opacity and filter:blur() via `save_layer()`:

```cpp
// Subtree paints into offscreen buffer, composited as single unit
canvas.save_layer(x, y, w, h, opacity, blur_radius);
// ... draw subtree ...
canvas.restore();
```

Works on both Skia (SkCanvas::saveLayer) and CoreGraphics (CGContextBeginTransparencyLayer).

### Cacheable layers — `begin_layer()` / `end_layer()`

`save_layer()` is a **scope**: the only way back into it is to re-run the drawing
that filled it, which defeats the point of caching it. `begin_layer()` /
`end_layer()` return a **`LayerHandle`** you can keep **across frames** and
re-composite without redrawing its contents. Query
`canvas.supports(CanvasCapability::retained_layer_cache)` before selecting
this path on an arbitrary backend. Always call `layer_valid()` as shown:
explicit invalidation, a backing-scale change, or GPU context loss can retire
the cached texture.

```cpp
if (!canvas.layer_valid(cached_)) {
    canvas.begin_layer(bounds, /*cacheable=*/true);
    paint_expensive_subtree(canvas);
    cached_ = canvas.end_layer();     // seals it; keep the handle
}
canvas.draw_layer(cached_);           // later frames: composite, don't redraw
```

`draw_layer(handle, alpha = 1.0f, mode = BlendMode::normal)` composites it;
`draw_layer_fitted(handle, dest)` scales it into a destination rect.

Use it for a subtree whose pixels are expensive and rarely change. `save_layer()`
is unchanged and remains correct for the one-shot opacity/blur case.

## Effect, Mask, And Composite Vocabulary

Pulp keeps three rendering concepts separate in importer output, view APIs, and
paint backends:

- **Effects** modify pixels produced by a view or by content behind it. CSS
  `filter`, `filter: blur(...)`, and `backdrop-filter` live here.
- **Masks and clips** constrain where pixels are visible. CSS `clip-path`,
  `mask`, `mask-image`, and `mask-size` live here. Some mask values may be
  stored before every backend can paint them.
- **Composition** controls how a finished subtree is blended back into its
  parent. CSS/RN `mix-blend-mode`, Canvas `globalCompositeOperation`, subtree
  opacity, and layer restore behavior live here.

This split matters for design import and renderer work. A blur should not be
modeled as a mask, a mask should not be treated as a blend mode, and
`mix-blend-mode` should remain a layer-composition decision rather than a
per-draw color transform. When adding CSS or design-tool coverage, first decide
which bucket the feature belongs to, then wire it through the matching view
state and backend path.

## Post-Processing Effects

```cpp
view.set_effect(std::make_shared<GpuBlurEffect>());           // Gaussian blur
view.set_effect(std::make_shared<GpuBloomEffect>());          // Glow approximation
view.set_effect(std::make_shared<EffectChain>());             // Compose multiple
```

An effect pushes `layer_count()` compositing layers (one for a simple
effect; `EffectChain` pushes one per child) and `View::paint_all` pops
exactly that many.

`GpuBloomEffect` is a glow *approximation*, not a true bloom: it blurs the
subtree uniformly by `radius * intensity` and composites it back normally.
There is no bright-pass and no additive composite, so nothing gets brighter
and dark pixels smear as much as light ones. It is not HDR-aware either —
every Pulp surface is 8-bit unorm + sRGB, so there is no headroom above 1.0
to threshold against. Use it for soft glow on light-on-dark content.

Arbitrary SkSL as a *view post-effect* is not supported — filtering a
subtree's rendered pixels needs a child-shader compositor Pulp does not
have. SkSL reaches widgets as a **body shader** instead; see the
[Shader Reference](shaders.md).

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

## GPU-Assisted Audio Analysis

GPU-assisted audio work is an offline/background analysis capability, not a
live audio-thread DSP primitive. Any path that wants a GPU backend for offline
render analysis should first call
`audio::evaluate_offline_render_compute_policy()`.

That policy helper accepts GPU work only from `OfflineAnalysis` or
`BackgroundAnalysis` scopes. `RealtimeAudioThread` requests are rejected even
when a GPU is available, because GPU submission, queue progress, resource
mapping, and fallback behavior are not bounded enough for the audio callback.
When the GPU is unavailable, callers must either take the explicit CPU fallback
decision or fail the offline analysis request; silent live fallback is not part
of the contract.
For a fixed policy input, the decision is deterministic: repeated offline or
background analysis evaluations return the same accepted/backend/fallback/reason
tuple and do not inspect live render state.

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
- **KTX2**: Texture header parser and native-gap classifier; Basis Universal payload transcoding remains deferred
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
