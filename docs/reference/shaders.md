# Custom Shaders (SkSL → Dawn, write-once / multi-backend)

Pulp authors fragment shaders **once in SkSL** (Skia Shading Language).
Skia Graphite compiles them to its IR and emits WGSL, which Dawn then
translates to the native GPU API at runtime — Metal on Apple, D3D12 on
Windows, Vulkan on Linux and Android, and OpenGL / OpenGL ES on legacy
targets. There is no per-backend build step, no manual cross-compile,
no `.hlsl` / `.metal` / `.spv` artifacts to ship.

This is modeled on the write-once-multi-backend pattern common to GPU UI
frameworks: one shader source, every supported backend covered by the
Skia + Dawn + Tint translation chain.

## Pipeline at a glance

```
SkSL source string
    │   (SkRuntimeEffect::MakeForShader)
    ▼
Skia compiler / SkSL IR
    │   (Graphite backend)
    ▼
WGSL
    │   (Dawn → Tint translator)
    ▼
Native shader for the active backend
    ├── Metal Shading Language        (macOS, iOS)
    ├── HLSL → D3D12 bytecode         (Windows)
    ├── SPIR-V → Vulkan               (Linux, Android)
    └── GLSL → OpenGL / OpenGL ES     (legacy desktops)
```

The active backend is selected by `GpuSurface` at window-host setup
time and exposed via `getGPUInfo().backendType` from the JS bridge.

## Scope and limit (read this first)

What you can do through this API:

- **Fragment shaders** that fill a rectangle of the active `Canvas`.
- **Post-effects** layered on a `View`'s composited content via the
  `ViewEffect` chain (blur, glow, vignette, chromatic aberration). Note
  these composite *layers*; they are not SkSL over the subtree — see the
  honesty note below.
- **Uniforms** for time, value, resolution, and up to five named colors
  per draw call (see `Canvas::ShaderUniforms`).

What is **not** covered by `Canvas::draw_with_sksl` and friends:

- **Custom vertex pipelines.** SkSL runtime effects are fragment-only.
  Geometry comes from Skia's draw primitives (the runtime effect fills
  a rect, not arbitrary triangle lists).
- **Compute / mesh / tessellation / geometry shaders.** Use the Dawn
  compute path via the `GpuCompute` interface
  (`core/render/include/pulp/render/gpu_compute.hpp`) — it exposes
  spectral, batch-convolution, FFT, matmul, and neural-inference
  primitives (see `gpu_compute.hpp` for the full surface). PBR materials
  and Three.js bridging already ride this path. Direct WGSL authoring
  lives behind the same interface; see existing call sites for the pattern.
- **Raw vertex / index buffer authoring.** Out of scope through SkSL.
  Drop down to Dawn (`core/render/include/pulp/render/gpu_surface.hpp`)
  if you need it.

This matches the architectural ceiling laid out in
[`docs/reference/layout-model.md`](layout-model.md) for layout — the
write-once shader path covers the UI fragment / post-effect slice
deliberately, not the full custom-pipeline surface.

## Hello world

A minimal call that draws a horizontal gradient into a 200×100 rect:

```cpp
#include <pulp/canvas/canvas.hpp>

const char* kGradientSkSL = R"(
    uniform float2 resolution;
    uniform float4 accentColor;
    uniform float4 bgColor;

    half4 main(float2 coord) {
        float t = coord.x / resolution.x;     // 0 → 1 across width
        return half4(mix(bgColor, accentColor, t));
    }
)";

void paint(pulp::canvas::Canvas& canvas) {
    pulp::canvas::Canvas::ShaderUniforms u;
    u.accent_color = pulp::canvas::Color::rgba(0.2f, 0.6f, 1.0f, 1.0f);
    u.bg_color     = pulp::canvas::Color::rgba(0.1f, 0.1f, 0.15f, 1.0f);

    canvas.draw_with_sksl(kGradientSkSL, /*x*/0, /*y*/0,
                                          /*w*/200, /*h*/100, u);
}
```

The signature is declared in
`core/canvas/include/pulp/canvas/canvas.hpp`:

```cpp
virtual bool draw_with_sksl(const std::string& sksl,
                            float x, float y, float w, float h,
                            const ShaderUniforms& uniforms);
```

- Returns `true` if the shader compiled and rendered on the GPU.
- Returns `false` on non-GPU canvases (e.g. `RecordingCanvas` for
  testing, or hosts without Skia linked); the base implementation
  falls back to a flat-color rect so call sites stay safe.

## Uniforms

`Canvas::ShaderUniforms` is a fixed struct — uniforms that exist in
your shader are written, the rest are silently skipped. Available
slots:

| SkSL name      | C++ field                | Type     | Notes                              |
|----------------|--------------------------|----------|------------------------------------|
| `resolution`   | (auto from `w`, `h`)     | `float2` | Set automatically per draw         |
| `value`        | `uniforms.value`         | `float`  | Typically widget value 0–1         |
| `time`         | `uniforms.time`          | `float`  | Animation seconds, FrameClock-fed  |
| `accentColor`  | `uniforms.accent_color`  | `float4` | sRGB-float, premultiplied alpha    |
| `bgColor`      | `uniforms.bg_color`      | `float4` |                                    |
| `trackColor`   | `uniforms.track_color`   | `float4` |                                    |
| `fillColor`    | `uniforms.fill_color`    | `float4` |                                    |
| `thumbColor`   | `uniforms.thumb_color`   | `float4` |                                    |

If your shader declares a uniform name not in this set, the call
returns `false` (the runtime effect compiles but `makeShader()`
rejects the unbound uniform). For richer parameter sets you currently
need to inline the constants into the SkSL source string — a single
hash bucket per (source, parameter) combo, which trades cache size
for flexibility.

### Texture / child shader uniforms

`uniform shader` slots are supported by Skia's runtime effects (see
`core/canvas/shaders/sdf_text.sksl`), but the convenience entry point
`draw_with_sksl` does not bind them. To pass a child shader, build the
`SkRuntimeShaderBuilder` directly via the Skia headers; the bundled
SDF/MSDF text shaders show the pattern.

## Compile, cache, and validation

Use `Canvas::compile_sksl(source)` to validate a shader without
drawing — useful for design-tool import paths and JS bridges:

```cpp
auto error = pulp::canvas::Canvas::compile_sksl(my_sksl);
if (!error.empty()) {
    // surface error to the user / log / design tool
}
```

Under the hood, both `compile_sksl` and `draw_with_sksl` route through
`RuntimeEffectCache` (`core/canvas/src/runtime_effect_cache.hpp`),
which:

- **Dedupes by source-string hash.** Identical shader text compiles
  once per process. Don't worry about calling `draw_with_sksl` from
  inside a paint loop — the second hit is a hash lookup, not a
  recompile.
- **Is process-lifetime.** The cache lives in a function-static
  singleton, not on `SkiaCanvas` (which is recreated every frame).
- **Is thread-safe.** A `std::mutex` guards inserts; compilation
  happens outside the lock.
- **Surfaces the last compile error** via `last_error()` for
  diagnostics.
- **Supports hot reload** via `RuntimeEffectCache::instance().clear()`
  — call it when your design-tool host detects a `.sksl` file change.
  Subsequent draws recompile from the new source.

The translation step beyond SkSL (SkSL → WGSL → native) is also
cached by Skia / Dawn per backend; you do not need to manage it.

## Post-effects on a `View` subtree

For effects that wrap an entire view subtree (blur the background behind a
popover, add a soft glow to a meter), use `ViewEffect` from
`core/canvas/include/pulp/canvas/view_effect.hpp`.

> **What `ViewEffect` is not.** These effects composite *layers* (blur,
> opacity, transform). **You cannot currently run an SkSL pass over a
> view's rendered content** — doing so needs a child-shader compositor
> that Pulp does not wire up today. Widget-body shaders
> (`setWidgetShader`, `CustomShaderHost`) are real, but they paint a
> *fresh* rect and cannot see rendered content. Read the per-effect doc
> comments before relying on one: `GpuBloomEffect` is a blur-based glow
> approximation, not a true bloom.

```cpp
#include <pulp/canvas/view_effect.hpp>

view.set_effect(std::make_shared<pulp::canvas::GpuBlurEffect>());

// Compose multiple in order
auto chain = std::make_shared<pulp::canvas::EffectChain>();
chain->add(std::make_shared<pulp::canvas::GpuBlurEffect>());
chain->add(std::make_shared<pulp::canvas::VignetteEffect>());
view.set_effect(chain);
```

Built-in effects: `GpuBlurEffect`, `GpuBloomEffect`,
`ChromaticAberrationEffect`, `VignetteEffect`, `EffectChain`. Each
pushes `layer_count()` compositing layers before the subtree paints
(one for a simple effect; `EffectChain` pushes one per child), and the
layers composite back with the configured filter / opacity when
`View::paint_all` pops them. See the
[Rendering Reference](rendering.md) for the broader effect-graph
context.

**Not supported: arbitrary SkSL as a view post-effect.** Running a
shader over a View's *already-rendered* content needs a child-shader
compositor (Skia's runtime-shader image filter), which Pulp does not
have — `draw_with_sksl()` fills a fresh rect and cannot post-process a
subtree. SkSL reaches widgets through the **body shader** path below
(`setWidgetShader` / `CustomShaderHost`), which replaces a widget's
body/track/fill drawing rather than filtering rendered pixels.

## JS bridge

The same pipeline is reachable from JS UIs:

```js
const result = compileShader(skslSource);
if (!result.success) console.error(result.error);

// Shader-capable widgets (Knob / Fader / Toggle — any CustomShaderHost).
// Returns { success, error }; the shader is compiled first and is NOT
// installed if it fails to compile.
const applied = setWidgetShader('my-knob', skslSource);
if (!applied.success) console.error(applied.error);

const info = getGPUInfo();                // { backendType: 'Metal' | 'D3D12' | ... }
clearWidgetShader('my-knob');             // also returns { success, error }
```

`setWidgetShader` and `clearWidgetShader` never fail silently: an unknown
id, a widget with no shader support, empty source, and SkSL that does not
compile each come back as `{ success: false, error }`.

The four functions are registered under `core/view/src/widget_bridge/`
(`compileShader`, `setWidgetShader`, and `clearWidgetShader` in
`shader_api.cpp`; `getGPUInfo` in `gpu_api.cpp`) and route to the same
`RuntimeEffectCache`, so a shader compiled from JS is shared with C++ paint
code that uses the same source.

There is no `applyShader`. It existed, but never compiled or applied
anything — it reported success for any non-empty string, including
un-compilable SkSL and ids matching no widget. Canvas widgets have no
shader path, so there was nothing an honest version of it could do.

## Shape shaders: `PulpGeom`, `PulpChart` and presets

A body shader paints a rectangle. A **shape shader** paints one of the SDF
shapes (`Canvas::SDFShape`) and receives that shape's geometry per fragment,
so the author writes the look and Pulp supplies the distances. There are two
contracts, chosen by the signature the source defines.

### `PulpFragment shade(PulpGeom g, float2 p)`: any SDF shape

```sksl
struct PulpGeom { float sdf; float2 grad; float2 pos; float2 uv; float coverage; };
struct PulpFragment { half4 color; float strokeWidth; float sigma; };
```

| Field | Meaning |
|---|---|
| `g.sdf` | Signed distance to the shape, negative inside, in local units |
| `g.grad` | Unit gradient of the distance field (zero where it has none) |
| `g.pos` | Fragment position relative to the shape's centre |
| `g.uv` | Fragment position over the draw rect, 0..1 |
| `g.coverage` | One-unit anti-aliased coverage of the fill |

Return the colour (premultiplied) plus an optional `strokeWidth` (0 fills the
shape, > 0 strokes its outline at that width). Pulp applies the coverage. From
JS, install it with `setWidgetShader(id, source, { geometry: { shape: 'stadium',
strokeWidth: 8, ... } })`; the shape names match `Canvas::SDFShape`. Operator
trees (`setWidgetShaderGeometry`) feed composed distances into the same `g.sdf`.

### `half4 shade(PulpChart g)`: bands with a centre line

```sksl
struct PulpChart {
    float t; float d; float side; float2 tan; float px; float valid;
    float cross; float halfWidth;
};
```

A chart shader sees the shape as a band around a centre line, which is what
knob arcs, LED rings, fader tracks and bar meters are.

| Field | Meaning |
|---|---|
| `g.t` | Position along the centre line, 0 at the start, 1 at the end |
| `g.cross` | Signed offset from the centre line along the normal `(tan.y, -tan.x)`: outward on arcs and rings, upward on horizontal shapes; `±halfWidth` at the band's edges. Inside a rounded cap it is the distance from the end point, so `cross >= 0` there |
| `g.halfWidth` | Half the band's width |
| `g.d` | Signed distance to the band, negative inside, including flat caps |
| `g.side` | `-1` inside the band, `+1` outside |
| `g.tan` | Unit tangent of the centre line, the direction `t` grows. Inside a rounded cap it is perpendicular to the radius from the end point, so `(tan.y, -tan.x)` points away from it; below the axis `tan` and `cross` both flip sign at the cap's edge. Their product `cross * (tan.y, -tan.x)` and anything that depends only on `abs(cross)` stay continuous there, but a shader keyed on the sign of `cross` alone, or on `tan` alone, shows a seam |
| `g.px` | One device pixel in the units of `d`, so `2.0 * g.px` is two pixels at any backing scale |
| `g.valid` | `0` for degenerate geometry (no sweep, no length, no band width); ignore the other fields then |

The shader runs over the whole draw rect and is responsible for its own
coverage, for example `clamp(0.5 - g.d / g.px, 0.0, 1.0)`.

| Shape | Centre line | `t` | Notes |
|---|---|---|---|
| `flat_arc` | arc of radius `(outer + inner) / 2` from `arc_start`, sweeping `arc_sweep` | 0 at `arc_start` | flat caps |
| `ring` | full circle starting at `arc_start` | wraps from 1 to 0 at `arc_start` | no caps |
| `flat_segment` | horizontal line through the centre, `halfSize.x` each side | 0 at the left end | band half width `stroke_width / 2`, flat caps |
| `rounded_segment` | as `flat_segment` | as `flat_segment` | rounded caps, each a dome about its end point: `cross` is the distance from the end point and `tan` turns so the normal points away from it |
| `stadium` | horizontal line between the two cap centres | 0 at the left cap | half width is half the height; caps are domes as for `rounded_segment`; a stadium whose width does not exceed its height is a circle and reports `valid = 0` |

Every chart matches the shape `draw_sdf_shape` draws for the same
`SDFStyle`. The horizontal shapes are horizontal in the canvas's local space;
draw a vertical track by rotating the canvas. `arc`, `quadratic_bezier` and
the filled shapes have no chart and are refused by name, at compile time
(`Canvas::compile_sdf_chart_sksl`) and at draw time. A Bezier's arc length has
no closed form, so it has no `t`.

From JS, `setWidgetShaderChart(id, source)` installs a chart shader on a knob,
whose arc is the `flat_arc` from `Knob::start_angle` to `Knob::end_angle`. A
chart shader paints the knob's material, the same full arc at every value, so
something else must show the value. With a chart alone the knob draws its
rotating value pointer over it. With a chart and a body shader, the body shader
paints over the chart and owns the value display, so no pointer is drawn; give
it transparent areas where the chart should show through. If the chart draw is
refused, the body shader paints alone.

### Feather modes and the `reach` rule

A chart draw can be feathered analytically (no blur pass). From JS the
feather is declared with the shape, `setWidgetShader(id, source, { geometry:
{ shape: 'flat_arc' }, feather: { sigma: 6, curve: 'gaussian', mode: 'glow' } })`;
in C++ it is `SDFStyle::feather_sigma`, `feather_curve` (0 Gaussian, 1 linear)
and `feather_mode`:

| Mode | Coverage |
|---|---|
| `uniform` (0) | soft edge on both sides of the outline |
| `glow` (1) | solid inside, falloff outside |
| `inner` (2) | falloff inside only |
| `outer` (3) | outside only, like a drop shadow of the shape |
| `inset` (4) | inside only, darkest at the outline |
| `radial` (5) / `sweep` (6) | sigma scaled by the distance across or along the band |

The shader is evaluated over the shape's rect grown by **`reach`** on every
side, so anything painted outside the shape (a glow, an outer feather, a
preset's halo) needs reach to land in. A `glow` or `outer` feather whose reach
is below three sigma is **refused**, not drawn with its falloff cut off at the
rect edge: `draw_sdf_shape_with_shader` returns false and the widget paints its
body shader or default body instead, logging why once.
`Canvas::sdf_feather_min_reach(style)` returns the floor without drawing. From
JS, `feather.sigma` raises the reach to `ceil(3 * sigma)` automatically, and
`setWidgetShaderReach(id, px)` sets it explicitly.

### Named presets

Six ready-made chart shaders cover the common hardware looks. Each is plain
SkSL defining `shade(PulpChart g)` with its options baked in as constants, so
it works on every charted shape and can be printed, copied and edited.

| Preset | Look | Options it reads |
|---|---|---|
| `bevel` | a plate whose edges catch the light; `pressed` sinks it | `color`, `light`, `elevation`, `pressed`, `bevelWidth` |
| `inset-channel` | a recessed track: walls facing inward, a shadowed floor | `color`, `light`, `elevation`, `bevelWidth` |
| `tube` | a solid rail with a specular highlight | `color`, `light`, `elevation`, `profile`, `intensity` |
| `light-pipe` | an emissive LED pipe, brightest where it is thickest, with a glint | `accent`, `light`, `elevation`, `profile`, `intensity` |
| `centre-highlight` | a bright line along the centre of the band (`center-highlight` also accepted) | `color`, `accent`, `intensity` |
| `glow` | an opaque core with a Gaussian halo outside the band | `accent`, `intensity`, `sigma` (at most 256) |

`profile` is the cross-section: `round` (a cylinder), `flat` (a flat face with
narrow rounded rims) or `chamfer` (a flat centre with 45 degree chamfers).
`light` is the direction the light comes from in degrees counter-clockwise
from the right (90 is above, 270 below; default 120, upper left) and
`elevation` its height above the surface (default 45). Colours are CSS colour
strings.

```js
// Install on a knob. A glow's extra reach belongs to the chart: it pads only the
// chart draw and is replaced when another chart or preset is installed.
setWidgetShaderChartPreset('drive', 'light-pipe', { accent: '#5ab0ff', profile: 'round' });

// Or get the source to inspect or edit.
const { success, sksl, reach } = getShaderChartPreset('bevel', { light: 90, pressed: true });
```

```cpp
#include <pulp/view/chart_shader_presets.hpp>

pulp::view::ChartShaderPresetOptions o;
o.profile = pulp::view::ChartProfile::chamfer;
auto preset = pulp::view::make_chart_shader_preset("tube", o);  // .sksl, .reach, .error
pulp::canvas::Canvas::ShaderDrawOptions draw;
draw.reach = preset.reach;
canvas.draw_sdf_shape_with_shader(pulp::canvas::Canvas::SDFShape::rounded_segment,
                                  x, y, w, h, style, preset.sksl, draw);
```

The shading model: `u = cross / halfWidth` runs across the band, the profile
gives a surface height over `u`, and its slope tilts the surface normal along
the chart normal. A directional light shades it with Lambert diffuse, divided
by the value for a face toward the viewer so a flat face shows exactly the
authored colour, plus a Blinn-Phong highlight. All of it happens in linear
light: colours are decoded from sRGB first and encoded once at the end, since
lighting sRGB values directly looks plausible and is wrong. The light lives in
the canvas's local space, so a rotated draw rotates it too.

Options are baked into the source, and each distinct source compiles once and
is cached; build presets when the look changes, not every frame. The widget
gallery's "Shader presets" row renders each one.

## Performance notes

- SkSL is compiled **once per source string per process**, then cached
  by hash. Do not generate per-frame SkSL strings — that defeats the
  cache, forces a recompile every frame, and shows up immediately in
  profile traces.
- The downstream Skia → Dawn translation runs once per (source,
  backend) and is cached by Skia / Dawn internally. You do not need
  to invalidate anything on backend selection.
- Uniform updates are cheap — `SkRuntimeShaderBuilder` rebuilds the
  uniform block per draw, but the compiled effect is reused.
- For animation, drive `uniforms.time` from `FrameClock` (the same
  source `ai_shader_design.shader_engine` uses) so all shaders share a
  monotonic clock.

## Authoring template

The bundled text shaders are the canonical examples of well-formed
runtime effects, including child-shader sampling and full SkSL
comments:

- `core/canvas/shaders/sdf_text.sksl` — single-channel SDF, edge AA
  via `fwidth`, gamma correction.
- `core/canvas/shaders/msdf_text.sksl` — multi-channel SDF sampler
  contract for sharper corners once true MSDF atlas generation is wired.
- `core/canvas/shaders/sdf_text_effects.sksl` — outline, glow, drop
  shadow effect shader; host presets exist, but the visible draw path is
  still being wired.

If you are writing a new shader, copy one of these as a starting point
— they encode the uniform-naming conventions, the
derivative-based AA pattern, and the gamma handling that Pulp uses
elsewhere.

## When to drop out of this API

Reach for the lower-level surfaces when the SkSL fragment / post-
effect path can't express what you need:

| Need                                      | Use                                                                  |
|-------------------------------------------|----------------------------------------------------------------------|
| Custom vertex / mesh / instanced draws    | Dawn directly (`core/render/include/pulp/render/gpu_surface.hpp`)    |
| GPU compute (PBR, simulation, mipmap gen) | `GpuCompute` (`core/render/include/pulp/render/gpu_compute.hpp`)     |
| Mixing native Dawn textures into a paint  | `Canvas::draw_native_dawn_texture` on `SkiaCanvas`                   |
| Sampling external textures in SkSL        | `SkRuntimeShaderBuilder` directly with `uniform shader` children     |

These paths are intentionally separate — they are not promised to be
"write once, all backends," because they expose backend-specific Dawn
surfaces and resources. The trade-off is the same one called out in
[Layout Model](layout-model.md): the write-once contract is real
inside a deliberately bounded scope, not across the entire GPU
surface.
