# Custom Rendering

Pulp provides four layers of custom rendering, from re-skinning the stock widgets to full GPU shader access. Choose the layer that matches your needs.

## When to Use Each Layer

| Layer | API | Language | GPU-Accelerated | Best For |
|-------|-----|----------|-----------------|----------|
| **A** — Paint / metrics delegates | `WidgetPainter`, `WidgetMetrics` | C++ | Yes (via Skia) | Re-skinning the **stock** widgets without subclassing them |
| **B** — CanvasWidget | JS bridge canvas functions | JavaScript | Yes (via Skia) | Custom meters, visualizations, simple graphics |
| **C** — Canvas API | `Canvas` abstract interface | C++ | Yes (via Skia) | Custom `View` subclasses, complex procedural drawing |
| **C+** — Dawn/WebGPU | WebGPU render pipeline | C++ / WGSL | Direct GPU | Shader-driven visuals, particle systems, spectrograms |

Start with Layer A if the widget you want already exists and you only want it to
*look* different. Move to Layer B for a bespoke visual. Move to Layer C when you
need C++ performance or complex state, and to C+ when you need custom shaders.

---

## Layer A: skin the stock widgets — `WidgetPainter` / `WidgetMetrics`

Two independent delegates let you restyle and re-size Pulp's stock widgets
**without subclassing any of them**:

| Delegate | Answers | Installed with |
|---|---|---|
| `WidgetPainter` (`pulp/view/widget_painter.hpp`) | *what pixels?* | `View::set_painter(...)` |
| `WidgetMetrics` (`pulp/view/widget_metrics.hpp`)  | *how big?*     | `View::set_metrics(...)` |

They are deliberately **separate objects**, because they run at different times.
Metrics is consulted during **layout** — with no canvas, possibly many times per
frame. Painting happens once, afterwards. Fusing them would drag every skin that
only wants to restyle into the layout pass.

```cpp
struct MySkin : pulp::view::WidgetPainter {
    bool paint_rotary(pulp::canvas::Canvas& c,
                      const pulp::view::RotaryPaintState& s,
                      pulp::view::View& v) override {
        // s.position is 0..1
        draw_my_knob(c, s.bounds, s.position, s.hovered, s.enabled);
        return true;                       // claimed — the stock look does NOT also draw
    }
    // every other hook is left alone, and keeps the stock look
};

panel.set_painter(std::make_shared<MySkin>());   // applies to the whole subtree
```

**Three properties make this usable, and all three are load-bearing:**

- **It cascades.** A delegate installed on a view applies to that view *and its
  whole subtree*, so you can skin a panel in one call. A descendant that installs
  its own wins for its own subtree; clearing falls back to the nearest ancestor.
- **Every hook declines by default.** Each returns `false` — "no opinion, use the
  stock look" — so a delegate that restyles one knob is never conscripted into
  reimplementing the entire widget set. Return `true` to claim a hook, and the
  widget will not also draw over you.
- **The units are the ones each shape actually reasons in**, and mixing them up
  renders *plausibly* while being wrong at every value:
  - a **rotary** hook gets a normalized `position` (0..1) — it reasons in sweep angle;
  - a **linear / scroll-bar** hook gets `thumb_pos` and `thumb_size` in **pixels**,
    plus the value and its range in your own units.

`WidgetMetrics` works identically, and feeds the same `intrinsic_width()` /
`intrinsic_height()` the layout engine already calls — there is no second measure
protocol. A widget that positions itself rather than being laid out (a
`ContextMenu`) asks the delegate directly, and `ContextMenu::layout()` will compute
its whole geometry **with no canvas and no paint**, so you can ask a menu how big it
wants to be before it is ever shown.

### `SliderCore` — the value engine

If you are building a *custom* continuous control and want Pulp's exact behaviour,
use `pulp::view::SliderCore` (`pulp/view/slider_core.hpp`) rather than
reimplementing it: range, quantization interval, skew (including
`set_skew_from_midpoint`), the absolute drag law, fine-drag, default/reset, and
edge-triggered gesture bracketing.

Its writes take a notification mode:

```cpp
core.set_value(v, Notify::none);   // change the value, tell nobody
core.set_value(v, Notify::sync);   // fire on_value_change now  (default)
core.set_value(v, Notify::async);  // defer to flush_async_notifications()
```

`Notify::none` is what stops a programmatic sync from echoing a change back at
whatever just made it. Gesture begin/end is **edge-triggered**, so a widget that
funnels two event sources into one handler still records **one** host edit, not two.

---

## Layer B: CanvasWidget from JS

Create a `CanvasWidget` and issue draw commands from JavaScript. The widget queues commands that are executed on the render thread by the Skia backend.

### Setup

```js
const canvas = createCanvas("viz", "root");
setFlex("viz", "width", 300);
setFlex("viz", "height", 200);
```

### Basic Shapes

```js
// Clear previous frame
canvasClear("viz");

// Filled rectangle
canvasRect("viz", 10, 10, 100, 50, "#2a2a4a");

// Stroked rectangle
canvasStrokeRect("viz", 10, 10, 100, 50, "#6666aa", 2);

// Filled circle
canvasFillCircle("viz", 80, 120, 30, "#44ccff");

// Line
canvasStrokeLine("viz", 0, 100, 300, 100, "#333333", 1);

// Text
canvasSetFont("viz", "Inter", 14);
canvasFillText("viz", "Hello", 10, 180, 14, "#ffffff");
```

### Paths

```js
// Bezier curve
canvasBeginPath("viz");
canvasMoveTo("viz", 10, 150);
canvasCubicTo("viz", 60, 50, 150, 200, 290, 80);
canvasStrokePath("viz");

// Filled polygon
canvasSetFillColor("viz", "#ff6b6b");
canvasBeginPath("viz");
canvasMoveTo("viz", 150, 10);
canvasLineTo("viz", 180, 60);
canvasLineTo("viz", 120, 60);
canvasClosePath("viz");
canvasFillPath("viz");
```

### State Management

```js
canvasSave("viz");          // Push state (color, transform, clip)
canvasTranslate("viz", 50, 50);
canvasRotate("viz", 0.3);   // Radians
canvasRect("viz", -20, -20, 40, 40, "#44ff44");
canvasRestore("viz");       // Pop state — transform reset
```

### Example: Custom Level Meter

```js
function drawMeter(peak, rms) {
    canvasClear("viz");
    const w = 300, h = 200;

    // Background
    canvasRect("viz", 0, 0, w, h, "#0a0a12");

    // RMS bar
    const rmsH = rms * h;
    canvasRect("viz", 20, h - rmsH, 60, rmsH, "#225533");

    // Peak bar
    const peakH = peak * h;
    const color = peak > 0.9 ? "#ff4444" : peak > 0.7 ? "#ffaa00" : "#44ff44";
    canvasRect("viz", 20, h - peakH, 60, peakH, color);

    // Peak hold line
    canvasStrokeLine("viz", 15, h - peakH, 85, h - peakH, "#ffffff", 2);

    // dB grid lines
    [-6, -12, -24, -48].forEach((db) => {
        const y = h - Math.pow(10, db / 20) * h;
        canvasStrokeLine("viz", 0, y, w, y, "#222222", 1);
        canvasFillText("viz", db + " dB", 100, y + 4, 10, "#555555");
    });
}
```

### Making a canvas control interactive

A canvas is hit-testable and receives pointer events with no opt-in — subscribing
with `on(id, "pointerdown"| "pointermove"| "pointerup", fn)` is all the wiring it
needs. **The canvas owns its own value**: `bindWidgetToParam` drives the stock
value widgets (knob, fader, slider, toggle, progress) and returns `false` for a
canvas, so a custom-drawn control reads with `getParam` and writes with
`setParam` from its own handlers.

The gesture shape is the usual one — latch the origin on press, apply the delta
on move:

```js
const knob = createCanvas("drive-knob", "root");
const st = { val: getParam("drive"), dragging: false, y0: 0, v0: 0 };

on(knob, "pointerdown", (e) => {
    st.dragging = true;
    st.y0 = e.clientY;      // where the gesture started
    st.v0 = st.val;         // and the value it started from
});
on(knob, "pointermove", (e) => {
    if (!st.dragging) return;
    // 150px of vertical travel spans the full range.
    st.val = Math.max(0, Math.min(1, st.v0 + (st.y0 - e.clientY) / 150));
    setParam("drive", st.val);
    drawKnob(st.val);
});
on(knob, "pointerup", () => { st.dragging = false; });
```

`pointerdown` and `pointerup` are the gesture *edges* — they fire once each, so
latching on `pointerdown` is safe. Every sample in between arrives as
`pointermove`, whether or not a button is held.

Headless tests drive this with `View::simulate_drag`, which delivers through the
same routing the platform hosts use.

---

## Layer C: Canvas API in C++

Create a custom `View` subclass and override `paint()` to draw directly to a `Canvas`. This is the same API that all built-in widgets use internally.

### Custom View

```cpp
#include <pulp/view/view.hpp>
#include <pulp/canvas/canvas.hpp>

class SpectrumDisplay : public pulp::view::View {
public:
    void set_data(const std::vector<float>& magnitudes) {
        magnitudes_ = magnitudes;
        set_needs_repaint();
    }

    void paint(pulp::canvas::Canvas& canvas) override {
        auto [w, h] = bounds().size();

        // Background
        canvas.set_fill_color(resolve_color("surface", Color::hex(0x1a1a2e)));
        canvas.fill_rect(0, 0, w, h);

        if (magnitudes_.empty()) return;

        // Draw spectrum as filled path
        canvas.set_fill_color(Color::rgba(88, 166, 255, 80));
        canvas.begin_path();
        canvas.move_to(0, h);

        float bar_width = w / static_cast<float>(magnitudes_.size());
        for (size_t i = 0; i < magnitudes_.size(); ++i) {
            float x = i * bar_width;
            float y = h - magnitudes_[i] * h;
            canvas.line_to(x, y);
        }

        canvas.line_to(w, h);
        canvas.close_path();
        canvas.fill_current_path();

        // Stroke the top edge
        canvas.set_stroke_color(Color::rgba(88, 166, 255, 200));
        canvas.set_line_width(1.5f);
        canvas.begin_path();
        for (size_t i = 0; i < magnitudes_.size(); ++i) {
            float x = i * bar_width;
            float y = h - magnitudes_[i] * h;
            if (i == 0) canvas.move_to(x, y);
            else canvas.line_to(x, y);
        }
        canvas.stroke_current_path();
    }

private:
    std::vector<float> magnitudes_;
};
```

### Canvas API Highlights

```cpp
// Gradients
canvas.set_fill_gradient_linear(0, 0, 0, h,
    {Color::hex(0x58a6ff), Color::hex(0x0f0f1a)},  // colors
    {0.0f, 1.0f},                                     // positions
    2);                                                // count

// Rounded rectangles
canvas.fill_rounded_rect(x, y, w, h, 8.0f);

// Arcs
canvas.stroke_arc(cx, cy, radius, start_angle, end_angle);

// Blend modes
canvas.set_blend_mode(BlendMode::screen);

// Opacity
canvas.set_opacity(0.5f);

// Text metrics
auto metrics = canvas.measure_text_full("Hello");
// metrics.width, metrics.ascent, metrics.descent, metrics.line_height

// SDF shapes (GPU-accelerated signed distance field rendering)
canvas.draw_sdf_shape(SDFShape::rounded_rect, x, y, w, h, {
    .fill_color = Color::hex(0x1a1a2e),
    .stroke_color = Color::hex(0x333333),
    .stroke_width = 1.0f,
    .corner_radius = 8.0f,
});

// Backdrop blur (frosted glass effect)
canvas.draw_blurred_backdrop(x, y, w, h,
    12.0f,                    // blur radius
    8.0f,                     // corner radius
    Color::rgba(0, 0, 0, 80)); // tint color

// GPU-accelerated waveform
canvas.draw_waveform(samples, count, x, y, w, h, {
    .line_color = Color::hex(0x58a6ff),
    .fill_color = Color::rgba(88, 166, 255, 40),
    .line_thickness = 1.5f,
    .show_fill = true,
    .fill_center = 0.5f,
});
```

### Testing with RecordingCanvas

Verify draw commands without a GPU:

```cpp
#include <pulp/canvas/recording_canvas.hpp>

TEST_CASE("SpectrumDisplay draws correctly") {
    SpectrumDisplay display;
    display.set_bounds({0, 0, 300, 200});
    display.set_data({0.5f, 0.8f, 0.3f, 0.6f});

    pulp::canvas::RecordingCanvas canvas;
    display.paint(canvas);

    // Verify draw commands were issued
    REQUIRE(canvas.command_count() > 0);
    REQUIRE(canvas.count(RecordingCanvas::Type::fill_rect) >= 1);
    REQUIRE(canvas.count(RecordingCanvas::Type::fill_current_path) >= 1);
}
```

---

## Layer C+: Custom Shaders and GPU Compute

When the Canvas primitives aren't enough — a per-pixel effect, a spectrogram,
or an offline GPU DSP kernel — Pulp exposes three escalating drop-down paths.
There is no `RenderContext` façade; you reach for the real interface that fits
the job.

### Custom fragment shaders on the canvas

The supported "custom shader" path is `Canvas::draw_with_sksl`
(`core/canvas/include/pulp/canvas/canvas.hpp`). It fills a rectangle with a
Skia runtime effect (SkSL, fragment-only) and passes the standard
`Canvas::ShaderUniforms` (value, time, and up to five named theme colors).
GPU backends (`SkiaCanvas`) render it; CPU backends draw a fallback rect.

```cpp
void paint(pulp::canvas::Canvas& canvas) override {
    const auto b = bounds();
    pulp::canvas::Canvas::ShaderUniforms u;
    u.value = value_;                       // widget value, 0..1
    u.time = animation_time_;               // seconds (FrameClock-fed)
    u.accent_color = pulp::canvas::Color::rgba(0.2f, 0.6f, 1.0f, 1.0f);
    // Fills the rect with the SkSL effect. Returns false and draws a
    // flat-color fallback rect on non-GPU canvases.
    canvas.draw_with_sksl(kFragmentSksl, b.x, b.y, b.width, b.height, u);
}
```

Validate a shader without drawing it via the static
`Canvas::compile_sksl(source)` (returns an error string; empty on success).
The shader receives `uniform float2 resolution`, `float value`, `float time`,
and `layout(color) float4 accentColor/bgColor/trackColor/fillColor/thumbColor`.
See [`docs/reference/shaders.md`](../reference/shaders.md) for the full uniform
contract and the JS-side `compileShader` / `setWidgetShader` API.

### GPU compute kernels

For non-rendering GPU work — spectral analysis, batch convolution, FFT,
matmul, neural inference — use `pulp::render::GpuCompute`
(`core/render/include/pulp/render/gpu_compute.hpp`). It runs WebGPU compute
(not fragment shaders) on a Dawn device that can be shared with the render
surface, and is explicitly **not** for the audio callback (upload/readback
latency is too high — do it on a background/offline path).

```cpp
auto gpu = pulp::render::GpuCompute::create();
if (gpu && gpu->initialize_standalone()) {
    gpu->batch_magnitude(complex_frames, magnitude_frames, num_frames, num_bins);
}
```

Read the header for the full primitive surface before calling — don't wrap it
in a convenience layer that doesn't exist.

### Raw Dawn / WebGPU

The lowest escape hatch is `pulp::render::GpuSurface`
(`core/render/include/pulp/render/gpu_surface.hpp`), which owns the Dawn
instance, adapter, device, queue, and native surface. It exposes the device
through opaque handles (`dawn_device_handle()`, `dawn_queue_handle()`,
`dawn_instance_handle()`, `current_texture_handle()`) that you cast to the
Dawn types yourself when you need native device access for a custom render
pass. This is a deliberate escape hatch — most UIs never touch it.

### Passing audio data to visuals

Move data from the audio thread to the UI thread with the real meter bridge,
`pulp::view::AudioBridge`
(`core/view/include/pulp/view/audio_bridge.hpp`) — a lock-free triple buffer:

```cpp
// Audio thread, in process():
bridge.analyze_and_push(channels, num_channels, num_samples);  // peak/RMS
// or push a pre-computed MeterData with bridge.push_meter(data);

// UI thread, in paint():
pulp::view::MeterData meter;
if (bridge.pop_latest_meter(meter)) {
    // meter.peak[ch] / meter.rms[ch] are per-channel linear levels ...
}
```

For a full magnitude spectrum, feed a `SpectrogramView`
(`core/view/include/pulp/view/widgets.hpp`) directly with
`push_spectrum(const float* magnitudes_db, int num_bins)` from the UI thread;
the widget owns the scroll/paint of the spectrogram.

---

## Performance Guidelines

| Technique | When to Use | Overhead |
|-----------|------------|----------|
| `canvasRect` / `canvasFillCircle` | Simple shapes, few per frame | Very low |
| Canvas paths | Complex shapes, curves | Low |
| `draw_sdf_shape` | Anti-aliased shapes at any size | Very low (GPU) |
| `draw_waveform` | Audio waveforms | Very low (GPU batch) |
| `draw_blurred_backdrop` | Frosted glass effects | Medium (GPU blur pass) |
| Custom WGSL shader | Per-pixel effects, particles | Depends on shader complexity |

### Tips

1. **Minimize `canvasClear` + redraw.** Only redraw what changed. The Canvas queues commands — unchanged regions are cheap.

---

## GPU Capabilities: What Is Real Today

Pulp uses the GPU in three distinct ways. Do not conflate them:

| Capability | Status | What It Does |
|------------|--------|-------------|
| **Dawn/Skia Graphite rendering** | Shipped | All UI drawing goes through the GPU via Skia Graphite on a Dawn wgpu::Device. This is the rendering pipeline, not compute. |
| **SkSL runtime effects** | Shipped | Fragment shaders for visual effects (SDF shapes, blur, gradients). These run per-pixel during rendering. Not compute shaders. |
| **WebGPU compute for audio** | Experimental | WGSL compute shaders for batch spectral processing. Viable for large offline workloads (>64K elements). Not viable for real-time per-buffer audio. See `docs/reports/webgpu-compute-feasibility.md`. |

The first two are production rendering features. The third is a separate compute pipeline that shares the same Dawn device but operates independently of rendering. It does NOT run in the audio callback.

2. **Use SDF shapes over path-based shapes** when possible. SDF rendering is resolution-independent and faster for rounded rectangles, circles, and arcs.

3. **Use `draw_waveform` over manual line drawing** for audio displays. It batches all samples into a single GPU draw call.

4. **Avoid per-frame shader compilation.** Compile once in `initialize()`, reuse the pipeline.

5. **Keep shader uniforms small.** A few floats and a texture reference — don't upload entire audio buffers every frame. Use storage buffers for large data.
