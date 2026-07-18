# Canvas Module

The canvas module provides a 2D drawing abstraction with multiple backends. Widgets paint against the `Canvas` interface; concrete implementations handle the actual rendering.

**Status**: experimental
**Dependencies**: runtime
**Headers**: `pulp/canvas/canvas.hpp`, `pulp/canvas/cg_canvas.hpp`, `pulp/canvas/skia_canvas.hpp`, `pulp/canvas/effects.hpp`, `pulp/canvas/svg.hpp`

## Canvas Interface

All drawing goes through the abstract `Canvas` class. This decouples widget painting from the rendering backend.

```cpp
void paint(Canvas& canvas) {
    canvas.save();
    canvas.set_fill_color(Color::hex(0x2196F3));
    canvas.fill_rounded_rect(0, 0, 200, 40, 8);
    canvas.set_fill_color(Color::rgba8(255, 255, 255));
    canvas.set_font("system", 14);
    canvas.set_text_align(TextAlign::center);
    canvas.fill_text("Click Me", 100, 24);
    canvas.restore();
}
```

### Drawing API

| Category | Methods |
|----------|---------|
| State | `save()`, `restore()` |
| Transform | `translate(x, y)`, `scale(sx, sy)`, `rotate(radians)` |
| Clipping | `clip_rect(x, y, w, h)` |
| Style | `set_fill_color()`, `set_stroke_color()`, `set_line_width()`, `set_line_cap()`, `set_line_join()` |
| Shapes | `fill_rect()`, `stroke_rect()`, `fill_rounded_rect()`, `fill_circle()`, `stroke_circle()` |
| Lines | `stroke_line()`, `stroke_arc()` |
| Text | `set_font()`, `set_text_align()`, `fill_text()`, `measure_text()` |

### Paint Types

Fill and stroke colors can be solid or gradient:

```cpp
// Solid color
canvas.set_fill_color(Color::hex(0xFF5722));
canvas.set_fill_color(Color::rgba8(255, 87, 34, 200));

// Gradients are set on the current fill/stroke style.
const Color colors[] = {Color::hex(0x2196F3), Color::hex(0x1565C0)};
const float positions[] = {0.0f, 1.0f};
canvas.set_fill_gradient_linear(0, 0, 0, 100, colors, positions, 2);
canvas.fill_rect(0, 0, 200, 100);
canvas.clear_fill_gradient();
```

## Backends

### CoreGraphicsCanvas (macOS)

Full macOS rendering via Core Graphics / Core Text. Used by the plugin view host and standalone apps on macOS.

```cpp
#include <pulp/canvas/cg_canvas.hpp>
pulp::canvas::CoreGraphicsCanvas canvas(cg_context, width, height);
widget.paint(canvas);
```

### SkiaCanvas (cross-platform)

GPU-accelerated rendering via Skia Graphite. Works on macOS (Metal), Windows (D3D12), Linux (Vulkan).

```cpp
#include <pulp/canvas/skia_canvas.hpp>
pulp::canvas::SkiaCanvas canvas(sk_canvas);
widget.paint(canvas);
```

### RecordingCanvas (testing)

Captures draw commands without rendering. Used for unit tests and headless validation.

```cpp
RecordingCanvas rec;
widget.paint(rec);
REQUIRE(rec.count(DrawCommand::Type::fill_rounded_rect) == 1);
REQUIRE(rec.count(DrawCommand::Type::fill_text) == 1);
```

## Post-Processing Effects

The effects system applies GPU-accelerated post-processing to rendered content.

```cpp
#include <pulp/canvas/effects.hpp>

BlurEffect blur{.radius_x = 10.0f, .radius_y = 10.0f};
ShadowEffect shadow{.offset_x = 2, .offset_y = 4, .blur_radius = 8, .color = Color::rgba8(0, 0, 0, 128)};
// NOTE: BloomEffect (effects.hpp) is a parameter struct only — nothing in
// core/ consumes it, so constructing one has no rendering effect today. For a
// working blur-based glow over a view subtree, use GpuBloomEffect from
// core/canvas/include/pulp/canvas/view_effect.hpp instead.
BloomEffect bloom{.threshold = 0.8f, .intensity = 1.5f};
ColorAdjust color{.brightness = 0.1f, .contrast = 1.2f, .saturation = 0.9f};
```

## SVG Loading

Load and render SVG files via nanosvg:

```cpp
#include <pulp/canvas/svg.hpp>

auto icon = SvgImage::from_file("path/to/icon.svg");
if (icon.is_valid()) {
    icon.render(canvas, x, y, width, height);
}
```

SVG files are parsed into vector paths and rendered at any resolution without pixelation.
