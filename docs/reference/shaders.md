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
  `ViewEffect` chain (blur, bloom, custom SkSL, vignette, chromatic
  aberration).
- **Uniforms** for time, value, resolution, and up to five named colors
  per draw call (see `Canvas::ShaderUniforms`).

What is **not** covered by `Canvas::draw_with_sksl` and friends:

- **Custom vertex pipelines.** SkSL runtime effects are fragment-only.
  Geometry comes from Skia's draw primitives (the runtime effect fills
  a rect, not arbitrary triangle lists).
- **Compute / mesh / tessellation / geometry shaders.** Use the Dawn
  compute path directly via `core/render/include/pulp/render/gpu_compute.hpp`
  (`GpuCompute::create_pipeline`, `dispatch_workgroups`) when you need
  WGSL compute. PBR materials and Three.js bridging already ride this
  path.
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

For effects that wrap an entire view subtree (blur the background
behind a popover, bloom a meter, run a custom SkSL film-grain pass
over a panel), use `ViewEffect` from
`core/canvas/include/pulp/canvas/view_effect.hpp`:

```cpp
#include <pulp/canvas/view_effect.hpp>

auto fx = std::make_shared<pulp::canvas::CustomShaderEffect>();
fx->sksl  = my_post_effect_sksl;
fx->value = 0.5f;
fx->time  = clock_seconds();
view.set_effect(fx);

// Compose multiple in order
auto chain = std::make_shared<pulp::canvas::EffectChain>();
chain->add(std::make_shared<pulp::canvas::GpuBlurEffect>());
chain->add(fx);
view.set_effect(chain);
```

Built-in effects: `GpuBlurEffect`, `GpuBloomEffect`,
`ChromaticAberrationEffect`, `VignetteEffect`, `CustomShaderEffect`,
`EffectChain`. Each calls `Canvas::save_layer(...)` before the
subtree paints; the layer is composited back with the configured
filter / opacity on `restore()`. See the
[Rendering Reference](rendering.md) for the broader effect-graph
context.

## JS bridge

The same pipeline is reachable from JS UIs:

```js
const result = compileShader(skslSource);
if (!result.success) console.error(result.error);

setWidgetShader('my-knob', skslSource);   // Knob / Fader / Toggle
applyShader('my-canvas', skslSource);     // canvas-targeted variant
const info = getGPUInfo();                 // { backendType: 'Metal' | 'D3D12' | ... }
clearWidgetShader('my-knob');
```

All four functions are registered in `core/view/src/widget_bridge.cpp`
and route to the same `RuntimeEffectCache`, so a shader compiled from
JS is shared with C++ paint code that uses the same source.

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
- `core/canvas/shaders/msdf_text.sksl` — multi-channel SDF for sharper
  corners.
- `core/canvas/shaders/sdf_text_effects.sksl` — outline, glow, drop
  shadow built on the same atlas.

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
