# SkSL Widget-Restyling Pipeline Audit

**Date:** 2026-03-30
**Scope:** Chat-to-render pipeline for widget material restyling via SkSL shaders
**Verdict:** Structurally sound, but output quality is bottlenecked by (1) sRGB-space shader math, (2) crude preset shaders, (3) an LLM prompt that invites raw shader generation, and (4) missing anti-aliasing infrastructure. Fixable without a rewrite.

---

## 1. Current Architecture (As It Exists in Code)

### Pipeline

```
User types in chat input
  → submitChat() in design-tool.js (line ~3449)
    → builds system prompt with theme JSON, token list, SkSL contract, presets
    → writes prompt to /tmp file
    → execAsync(buildAiCliCommand(...), "__design-chat__") — spawns CLI on background thread
      → poll_async_results() in widget_bridge.cpp:223 delivers stdout to JS
        → applyDesignChatResponse() in design-tool.js:3360
          → JSON.parse the response
          → applyTokenDiff() for color tokens
          → applyWidgetLook() per widget in diffObj.widgetLooks
            → tries preset → shaderBody → full shader → schema → fallback
              → compileShader() → C++ Canvas::compile_sksl() → RuntimeEffectCache::get_or_compile()
              → setWidgetShader(id, skslCode) → Knob/Fader/Toggle::set_custom_shader()
                → on next paint(): draw_with_sksl(custom_sksl_, 0, 0, w, h, uniforms)
                  → SkRuntimeShaderBuilder + SkPaint::setShader → canvas_->drawRect()
```

### Key Components

| Component | File | Role |
|---|---|---|
| Chat prompt builder | `design-tool.js:3468-3537` | Constructs system prompt with SkSL contract, presets, rules |
| Response parser | `design-tool.js:3360-3436` | Extracts JSON, dispatches to token/dimension/widgetLook handlers |
| applyWidgetLook | `design-tool.js:3287-3358` | Cascade: preset → shaderBody → shader → schema → fallback |
| buildWidgetShaderFromBody | `design-tool.js:3162-3247` | Wraps shaderBody snippet in full SkSL with uniform prelude + helpers |
| Shader presets | `design-tool.js:3041-3115` | 3 hand-written shaders: macos7_knob, glass_fader, capsule_toggle |
| Schema presets | `design-tool.js:3116-3133` | 2 declarative schemas: notched_knob, minimal_toggle |
| compileShader bridge | `widget_bridge.cpp:2098-2112` | Calls Canvas::compile_sksl() which uses RuntimeEffectCache |
| setWidgetShader bridge | `widget_bridge.cpp:2114-2122` | Calls Knob/Fader/Toggle::set_custom_shader() |
| RuntimeEffectCache | `runtime_effect_cache.hpp` | Process-lifetime hash-keyed cache with mutex |
| draw_with_sksl | `skia_canvas.cpp:435-481` | Sets uniforms, builds shader, draws rect |
| Knob/Fader/Toggle paint | `widgets.cpp:338-529` | Priority: schema → custom_sksl_ → render_style → default paint |

### Uniform Contract

The SkSL shaders receive:
- `resolution` (float2), `value` (float), `time` (float — but hardcoded to 0, never wired to FrameClock)
- `accentColor`, `bgColor`, `trackColor`, `fillColor`, `thumbColor` (all `layout(color) uniform float4`)

The `buildWidgetShaderFromBody()` wrapper for knobs also provides pre-computed `uv`, `p`, `r`, and helper functions `ringMask()` and `sdDiamond()`.

---

## 2. Weakest Links

### 2.1 Color Management — Shader Math in Wrong Color Space

**This is the single biggest quality issue.**

The shaders declare `layout(color) uniform float4` for colors. Skia's `layout(color)` qualifier tells the runtime to convert the uniform value into the shader's working color space. But in `draw_with_sksl()` (skia_canvas.cpp:456-458), colors are passed as:

```cpp
auto toSkV4 = [](Color c) -> SkV4 {
    return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
};
```

The `Color` struct is sRGB. When you use `layout(color)`, Skia expects you to provide values in the surface's working color space (typically sRGB but could be linear depending on the Graphite context). The bigger problem: **all shader math (mix, smoothstep on colors, gradient blending) happens in sRGB space.** This produces banding in dark gradients, non-perceptual midtone blends, and dull highlights. Every `mix()` of two colors in these shaders is wrong — it should happen in linear space.

**Fix:** Either (a) linearize inside the shader (`pow(c.rgb, half3(2.2))` before math, `pow(result, half3(1.0/2.2))` after), or (b) ensure the SkSurface uses a linear color space so `layout(color)` does the conversion automatically. Option (b) is correct; option (a) is a band-aid.

### 2.2 The LLM Generates Raw SkSL — And It's Bad At It

The prompt (design-tool.js:3508-3515) tells the LLM:

> "When you generate a custom shader, the code must compile as a SkRuntimeEffect shader."
> "Prefer widgetLooks.<id>.shaderBody over widgetLooks.<id>.shader."

This means the LLM is expected to write SkSL fragment shader code that:
- Compiles in Skia's restricted shader dialect (no loops over non-constant bounds, no textures without child shaders, limited function set)
- Produces premultiplied RGBA
- Looks premium

In practice, LLMs produce:
- Shaders that fail to compile (SkSL is NOT GLSL — half vs float matters, no `texture2D`, limited stdlib)
- Shaders that compile but look flat/ugly (no proper SDF AA, no lighting model, no noise)
- Shaders that work on one widget size and break on another (hardcoded pixel values instead of UV-relative)

The fallback cascade (applyWidgetLook) catches compile failures by falling back to a preset, which is good engineering. But the fundamental problem is that **asking an LLM to generate shader code is the wrong abstraction level.** It's like asking GPT to write assembly instead of Python.

### 2.3 The Preset Library Is Tiny and Low Quality

Three shader presets (`macos7_knob`, `glass_fader`, `capsule_toggle`) and two schema presets (`notched_knob`, `minimal_toggle`). That's it.

The `macos7_knob` shader (design-tool.js:3042-3074) is the best of the three, but it's still crude:
- No proper Phong/Blinn specular highlight — just a directional ramp (`1.0 - smoothstep(-0.26, 0.24, p.y + p.x * 0.18)`)
- No rim light
- No bevel
- The notch/thumb indicator is a single `smoothstep` blob, not a crisp line
- No texture or noise — everything is mathematically smooth, which reads as "CG demo" not "hardware knob"

The `glass_fader` and `capsule_toggle` are even simpler.

### 2.4 `time` Uniform Is Dead

`widgets.cpp:341`: `u.time = 0;` with the comment `// TODO: wire to FrameClock elapsed`. This means no animated shaders work. Any LLM-generated shader that uses `time` for pulse, glow, or animation is silently broken.

### 2.5 Rectangle Masking — No Circular Clip for Knobs

`draw_with_sksl` draws a full rectangle (`drawRect` at skia_canvas.cpp:478). For knobs, the shader itself must produce alpha=0 outside the circular body. If the shader author forgets this (or the LLM does), you get a square knob. This is a frequent source of "square artifacts."

The preset shaders handle this correctly (they compute a `body` mask), but generated shaders often don't.

### 2.6 Aspect Ratio Correction Is Inconsistent

The `buildWidgetShaderFromBody` wrapper for knobs (design-tool.js:3189) does:
```glsl
p.y *= resolution.y / max(resolution.x, 1.0);
```
This corrects for non-square widgets. But the full-shader presets (macos7_knob) don't do this — they assume square widgets. If a knob is laid out at a non-square size, the preset looks stretched.

### 2.7 No Error Recovery UI

When a shader fails to compile, the system:
1. Tries to fall back to a preset (good)
2. Shows a message in chat (good)
3. But if the fallback also fails, the widget just... doesn't change (silent failure)

There's no visual indicator on the widget itself that something went wrong.

### 2.8 Hot Reload Crash Risk

The hot-reload path (main.cpp:140-177) creates a probe bridge, validates the script, then tears down and rebuilds the real bridge. But shader state (`custom_sksl_` on widgets) is not preserved across hot reloads — only widget values are (via `snapshot_values`). A hot reload wipes all custom shaders.

---

## 3. Should the Hierarchy Be Preset → shaderBody → Full Shader?

**Yes, but the proportions are wrong.** The current hierarchy is:

1. Preset (named shader from library) — **correct, should be primary**
2. shaderBody (LLM writes main() body, system wraps) — **wrong abstraction**
3. Full shader (LLM writes everything) — **should almost never happen**
4. Schema (declarative JSON) — **underused, should be promoted**

### Recommended hierarchy:

1. **Preset + parameters** (95% of requests) — LLM picks a preset and tunes numeric/color parameters
2. **Declarative schema** (4% of requests) — LLM assembles shapes/arcs/gradients from a structured vocabulary
3. **Escape hatch: validated shaderBody** (1% of requests) — only for requests that can't be expressed any other way

**Full raw shader should be removed from the LLM's output vocabulary entirely.** It adds complexity, increases failure rate, and the LLM can't write good SkSL.

---

## 4. Proposed Architecture: "LLM Outputs Intent, System Renders Deterministically"

### The Core Insight

The LLM should never write rendering code. Instead, it should output a **material specification** — a structured description of what the widget should look like — and a deterministic renderer converts that to pixels.

### Material Spec Format

```json
{
  "widgetLooks": {
    "k1": {
      "material": {
        "body": {
          "shape": "circle",
          "fill": "radial-gradient",
          "gradientStops": [
            { "offset": 0.0, "color": "bg.elevated", "lightness": 1.3 },
            { "offset": 0.8, "color": "bg.surface", "lightness": 0.9 },
            { "offset": 1.0, "color": "bg.primary", "lightness": 0.6 }
          ],
          "bevel": { "width": 2, "highlight": 0.4, "shadow": 0.3, "angle": 135 },
          "rim": { "width": 1.5, "color": "accent.primary", "opacity": 0.15 }
        },
        "track": {
          "shape": "arc",
          "startAngle": 135,
          "sweepAngle": 270,
          "width": 3,
          "color": "control.track",
          "activeColor": "control.fill"
        },
        "indicator": {
          "type": "notch",
          "length": 0.15,
          "width": 2.5,
          "color": "control.thumb"
        },
        "effects": {
          "highlight": { "position": "top-left", "intensity": 0.35, "spread": 0.6 },
          "noise": { "type": "fine-grain", "intensity": 0.03 },
          "dropShadow": { "blur": 4, "offset": [0, 2], "opacity": 0.3 }
        }
      }
    }
  }
}
```

### Why This Works

1. **Deterministic rendering** — the same material spec always produces the same pixels
2. **LLM-friendly** — structured JSON is what LLMs are good at; shader code is what they're bad at
3. **Validatable** — you can schema-validate the material spec before rendering
4. **Interpolatable** — you can animate between two material specs
5. **Composable** — presets become named material specs; the LLM can modify individual fields
6. **Exportable** — material specs can be saved, shared, version-controlled

### Renderer Implementation

A single deterministic SkSL "uber-shader" per widget type (knob, fader, toggle) that takes all material parameters as uniforms. The shader is hand-written, heavily tested, and never changes at runtime. Only its uniform values change.

Alternatively: a C++ material renderer that uses the Canvas 2D API to composite layers (body gradient, bevel, rim, track, indicator, effects). This is simpler to debug and doesn't require SkSL at all for most looks.

---

## 5. What the LLM Should Output

### Recommended: Semantic Material Spec

The LLM should output:
- **Preset ID + parameter overrides** for common requests ("make it glossy" → `{ "preset": "metallic", "params": { "highlight": 0.5, "roughness": 0.2 } }`)
- **Material deltas** for targeted changes ("warmer highlight" → `{ "material": { "body": { "gradientStops": [{"offset": 0.0, "lightness": 1.4}] }, "effects": { "highlight": { "intensity": 0.45 } } } }`)
- **Color token diffs** for color-only changes (existing system works fine for this)

### What It Should NEVER Output

- Raw SkSL shader code (shaderBody or full shader)
- Pixel-level coordinates
- Hardcoded color values in non-token positions
- Anything that requires compilation to validate

### Transition Plan

Keep the existing `shaderBody` path as a hidden escape hatch for developer use, but remove it from the LLM's system prompt. The LLM should not know it can write shaders.

---

## 6. Making Knobs/Sliders/Toggles Look Premium

### 6.1 Shading Model

**Current:** Single directional ramp (`mix(dark, light, highlight_mask)`). No physical basis.

**Recommended:** Per-widget-type shading with:
- **Diffuse:** Radial gradient from center (lighter) to edge (darker), simulating a convex surface lit from above
- **Specular:** Blinn-Phong highlight positioned at ~135 degrees (top-left light). Use `pow(max(dot(N, H), 0.0), shininess)` with `shininess` controllable (matte=4, satin=16, glossy=64, mirror=256)
- **Fresnel rim:** Subtle brightening at the silhouette edge (`(1.0 - dot(N, V))^3 * rim_intensity`)

For knobs specifically, the "normal" is the sphere normal: `N = float3(p.x, p.y, sqrt(1.0 - dot(p, p)))` where `p` is the UV offset from center, clamped to the unit circle.

### 6.2 Bevel/Rim/Highlight Structure

**Knob:**
- Outer rim: 1-2px beveled edge (inner shadow on bottom-right, highlight on top-left)
- Body face: convex sphere shading
- Track ring: recessed groove (inset shadow effect)
- Active arc: filled portion of track, slightly raised
- Indicator notch: thin line from center toward edge, with slight glow

**Fader:**
- Recessed track: inset groove with inner shadow
- Filled portion: slightly raised, different surface treatment
- Thumb: convex circle or rectangle with bevel

**Toggle:**
- Track capsule: recessed groove
- Thumb circle: convex sphere with shadow, positioned by animated `value`

### 6.3 Anti-Aliasing

**Current:** Each shape uses `smoothstep` for AA, but the AA width is hardcoded (typically 0.012 in UV space or 1.0 in pixel space). This means:
- Small widgets (32px): AA is too wide, everything is blurry
- Large widgets (80px): AA is too narrow, edges are aliased

**Fix:** Compute AA width from pixel derivatives:
```glsl
float aa = fwidth(d);  // SkSL supports fwidth()
float mask = 1.0 - smoothstep(-aa, aa, d);
```
If `fwidth()` is not available in SkSL (it may not be in all backends), use `2.0 / min(resolution.x, resolution.y)` as an approximation.

### 6.4 Color Management

**Fix:** All color blending must happen in linear space. For a "premium" look:
1. Linearize inputs: `linear = pow(srgb, 2.2)` (or use `layout(color)` + linear surface)
2. Do all `mix()`, gradient interpolation, and lighting math in linear space
3. Apply tone mapping if needed (for HDR highlights)
4. Convert back: `srgb = pow(linear, 1.0/2.2)`

Also: use OKLCH or OKLAB for perceptual color manipulation in the material spec. The oklch.js library is already loaded.

### 6.5 Texture and Noise

Premium hardware knobs have surface texture — brushed metal, anodized aluminum, powder coat. Add:
- **Hash-based noise:** `fract(sin(dot(uv, float2(12.9898, 78.233))) * 43758.5453)` — zero-cost, no textures needed
- **Fine-grain noise** (0.01-0.03 intensity): simulates matte/satin surface
- **Directional noise** (circular for knobs): simulates brushed metal
- **Stipple/dither** (at output): prevents banding in dark gradients

Keep noise intensity as a material parameter (0 = perfectly smooth/plastic, 0.02 = satin, 0.05 = matte, 0.1 = rough).

### 6.6 Dimensions and Proportions

- Knob body should fill 80-85% of the widget rect (leave room for labels and glow)
- Track arc: 3-4px wide at 60-80px widget size, scale proportionally
- Indicator notch: 2-3px wide, extending from 55% to 85% of body radius
- Bevel width: 1.5-2.5px, scale with `min(resolution) / 64.0`
- Drop shadow: 2-4px offset, 4-8px blur, keep within the widget rect

---

## 7. Specific Bugs and Ugly-Output Sources

### 7.1 Square Artifacts on Knobs
**Cause:** Shaders that don't mask outside the circular body. Any LLM-generated shader that forgets `float body = 1.0 - smoothstep(...)` and `return color * body` produces a filled square.
**Fix:** The material renderer should always apply a circular mask for knob-type widgets.

### 7.2 Bad Blending / Banding
**Cause:** sRGB-space `mix()` in shader math. Dark-to-dark gradients produce visible steps.
**Fix:** Linear-space math (see 6.4).

### 7.3 Inconsistent Sizing
**Cause:** Some shaders use absolute UV thresholds (e.g., `dist > 0.35 && dist < 0.45`) that assume specific aspect ratios. When widgets are laid out at non-square sizes, proportions break.
**Fix:** The material renderer should normalize coordinates to a square domain for knobs, using `min(resolution.x, resolution.y)` as the reference.

### 7.4 Flat/CG Look
**Cause:** No noise, no bevel, no Fresnel rim. Everything is mathematically perfect, which looks synthetic.
**Fix:** Add noise and proper shading model (see 6.1, 6.5).

### 7.5 Dead `time` Uniform
**Location:** `widgets.cpp:341` — `u.time = 0;`
**Impact:** Any animated effect (pulsing glow, rotating indicator) is frozen.
**Fix:** Wire to FrameClock. But this also means animated shaders trigger continuous repainting — needs a `wants_animation()` flag.

### 7.6 `stroke_current_path` Double Color Set
**Location:** `skia_canvas.cpp:305-307` — color is set twice, first with wrong ARGB order, then corrected. The first set is dead code but indicates past bugs in the color pipeline.

### 7.7 Shader Hash Collision Risk
**Location:** `runtime_effect_cache.hpp:25` — uses `std::hash<std::string>` which is not collision-resistant. Two different shader strings with the same hash would silently return the wrong cached effect.
**Fix:** Use the full string as key, or SHA-256 hash. Given the small number of active shaders this is low-probability but high-severity.

---

## 8. What You Should Not Claim Yet

1. **"AI-driven shader design system"** — The AI generates raw shader code that frequently fails to compile or looks bad. This is AI-assisted prompt-to-JSON, not a design system.

2. **"Premium widget rendering"** — The preset shaders are functional but not premium. They lack proper lighting, texture, and bevel. They look like shader toy demos, not Ableton/Logic controls.

3. **"Four rendering strategies"** — Lottie (Phase 10.3) is specced but not implemented. The schema renderer exists but has only 2 presets. Realistically you have 2 rendering strategies: default C++ paint and custom SkSL.

4. **"Headless/live parity"** — You haven't demonstrated this. The headless screenshot path and the live GPU path may produce different results due to different SkSurface configurations, color spaces, or Skia backend differences.

5. **"Color management"** — Colors are passed as sRGB bytes and shader math happens in gamma space. There is no color management.

6. **"Widget shader time/animation"** — The `time` uniform is hardcoded to 0.

---

## 9. Milestone Plan

### Must Fix Now (Before Any Demo)

1. **Linear-space shader math** — Add `pow(color.rgb, half3(2.2))` linearization at shader input and `pow(result.rgb, half3(0.4545))` at output in all preset shaders. Or better: configure the SkSurface with a linear color space.

2. **Improve macos7_knob preset** — Add proper specular highlight, Fresnel rim, fine-grain noise, and bevel. This is the flagship visual. Target: "someone screenshots this and asks what plugin it's from."

3. **Wire `time` uniform** — Connect to FrameClock in Knob/Fader/Toggle paint. Gate continuous repaint behind `custom_sksl_.find("time") != npos`.

4. **Fix circular masking for knobs** — Either clip the draw rect to a circle before `drawRect`, or enforce that the material renderer always applies a circular alpha mask.

5. **Fix AA scaling** — Replace hardcoded smoothstep widths with `2.0 / min(resolution.x, resolution.y)` or `fwidth()` where available.

### Should Do Next (Quality Gate for Shipping)

6. **Expand preset library to 8-12 shaders** — Add: brushed-metal knob, LED-ring knob, vintage Bakelite knob, VU-meter fader, analog slider, rocker toggle, illuminated toggle, minimal-dot knob.

7. **Implement material spec format** — Define JSON schema, write a C++ material renderer that composites layers via Canvas 2D API (no SkSL needed for most looks). This replaces `shaderBody` as the LLM's primary output format.

8. **Remove raw shader from LLM prompt** — Stop telling the LLM it can write SkSL. Let it pick presets and set material parameters.

9. **Add per-widget aspect-ratio normalization** — Knobs should always render in a square domain. Faders should normalize to their track axis.

10. **Preserve shader state across hot reload** — Add shader/schema to `snapshot_values` / `restore_values`.

### Later / Nice to Have

11. **Uber-shader per widget type** — A single optimized SkSL shader per widget type that takes all material parameters as uniforms. Eliminates shader recompilation on material changes.

12. **Visual regression testing** — Screenshot-diff preset shaders against golden images in CI.

13. **Shader preview panel** — In-app panel showing uniform values, compile status, and a zoomed preview of the shader output.

14. **OKLCH-space material blending** — Use OKLCH for gradient stops and color interpolation in the material renderer.

15. **Noise textures via child shaders** — Pre-baked noise textures (blue noise, Perlin) passed as `shader` children for high-quality surface effects.

---

## 10. Open-Source Projects That Could Help

### Shader Preview / Debugging

| Project | License | What It Helps With | Recommendation |
|---|---|---|---|
| [SkSL Debugger (Skia)](https://skia.org/docs/user/sksl/) | BSD-3-Clause | Official SkSL docs, test shaders in Skia Fiddle | **Reference** — use Skia Fiddle to validate preset shaders during development |
| [Shadertoy](https://www.shadertoy.com) | Various (per shader) | High-quality SDF/material shader examples | **Reference** — study knob/button shaders for technique, rewrite clean-room |
| [glslViewer](https://github.com/nickvdp/glslViewer) | BSD-3-Clause | CLI GLSL shader previewer with uniform injection | **Ignore** — SkSL is not GLSL; Skia Fiddle is more appropriate |
| [KodeLife](https://hexler.net/kodelife) | Proprietary | Real-time shader editor | **Ignore** — not OSS |

### Screenshot Diff / Visual Regression

| Project | License | What It Helps With | Recommendation |
|---|---|---|---|
| [pixelmatch](https://github.com/mapbox/pixelmatch) | ISC | Pixel-level image comparison, anti-aliased diff | **Integrate** — use for CI screenshot regression of preset shaders |
| [reg-suit](https://github.com/reg-viz/reg-suit) | MIT | Visual regression testing framework | **Reference** — overkill for current needs, but good architecture model |
| [BackstopJS](https://github.com/garris/BackstopJS) | MIT | Visual regression for web UIs | **Ignore** — web-focused |
| [looksame](https://github.com/gemini-testing/looks-same) | MIT | Perceptual image comparison with configurable tolerance | **Reference** — good algorithm for non-exact comparison |

### Structured Shader Metadata / Material Description

| Project | License | What It Helps With | Recommendation |
|---|---|---|---|
| [MaterialX](https://github.com/AcademySoftwareFoundation/MaterialX) | Apache 2.0 | Industry-standard material description format (ASWF) | **Reference** — the material node graph model is relevant; too heavy to integrate directly |
| [glTF Material Extensions](https://github.com/KhronosGroup/glTF/tree/main/extensions) | Apache 2.0 | KHR_materials_* specs for PBR material parameters | **Reference** — good parameter naming and range conventions for specular, roughness, etc. |
| [Filament Material Guide](https://google.github.io/filament/Materials.html) | Apache 2.0 | Google's material model documentation for Filament renderer | **Reference** — excellent practical guide to PBR material parameters for real-time rendering |

### Design Tokens → Rendering Pipeline

| Project | License | What It Helps With | Recommendation |
|---|---|---|---|
| [Style Dictionary](https://github.com/amzn/style-dictionary) | Apache 2.0 | Design token transformation pipeline (Amazon) | **Reference** — good model for token → platform-specific output transforms |
| [Theo](https://github.com/salesforce-ux/theo) | BSD-3-Clause | Design token conversion (Salesforce) | **Ignore** — Style Dictionary is more mature |
| [Design Tokens W3C Spec](https://design-tokens.github.io/community-group/format/) | W3C | Standard format for design tokens | **Reference** — already mentioned in Phase 10 spec; implement import/export |

### High-Quality 2D Material Rendering References

| Project | License | What It Helps With | Recommendation |
|---|---|---|---|
| [Rive](https://github.com/rive-app/rive-cpp) | MIT | High-quality 2D vector rendering with state machines | **Reference** — state machine model for widget states (hover/active/disabled transitions) |
| [Lottie (rlottie)](https://github.com/nickvdp/rlottie) | MIT | Lightweight Lottie animation renderer | **Reference** — if you proceed with Lottie support (Phase 10.3) |
| [NanoVG](https://github.com/nickvdp/nanovg) | zlib | Antialiased 2D vector rendering on OpenGL | **Reference** — excellent SDF-based AA implementation for rounded shapes |
| [imgui-knobs](https://github.com/altschuler/imgui-knobs) | MIT | Audio knob rendering for Dear ImGui | **Reference** — small, focused examples of audio-style knob rendering with proper AA |
| [surge-synthesizer/surge](https://github.com/surge-synthesizer/surge) | GPL-3.0 | Professional synth UI with custom widget rendering | **Ignore** — GPL, but worth studying their knob aesthetics for clean-room reference of what "premium" looks like |
| [Dexed](https://github.com/asb2m10/dexed) | GPL-3.0 | FM synth with custom knob rendering | **Ignore** — GPL |

### Deterministic Style/Preset Systems

| Project | License | What It Helps With | Recommendation |
|---|---|---|---|
| [Open Props](https://github.com/argyleink/open-props) | MIT | Comprehensive CSS custom property presets | **Reference** — good model for a "preset library" of token values |
| [Radix Themes](https://github.com/radix-ui/themes) | MIT | Composable design system with token-driven theming | **Reference** — their color scale system (12 steps per hue) is well-designed |
| [Yoga](https://github.com/nickvdp/yoga) | MIT | CSS Flexbox layout engine | **Already integrated** |

---

## Final Recommendation

**Stop letting the LLM write shaders. Start letting it describe materials.**

The path forward is:

1. **Hand-write 10-12 premium preset shaders** with proper lighting, AA, noise, and bevel. These are your product. Test them on multiple widget sizes. Screenshot-diff them in CI. Make them look as good as Logic Pro or Ableton.

2. **Define a material spec JSON format** with body (shape, fill, gradient, bevel, rim), track (shape, width, colors), indicator (type, size, color), and effects (highlight, noise, shadow). Validate it with a JSON schema.

3. **Rewrite the LLM prompt** to output `{ "preset": "...", "overrides": {...} }` or `{ "material": {...} }`. Remove all mention of SkSL, shaderBody, or shader from the prompt. The LLM's job is creative direction, not rendering.

4. **Fix color management** so your preset shaders actually look correct. Linear-space math is non-negotiable for premium results.

5. **Keep raw SkSL as a developer escape hatch** (the `setWidgetShader` bridge function stays), but remove it from the LLM-facing API surface.

The current system is a prototype that proves the pipeline works end-to-end. The next step is to move the rendering quality bar from "tech demo" to "product" — and that requires taking shader generation away from the LLM and giving it to hand-tuned, deterministic renderers fed by structured material descriptions.
