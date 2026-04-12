# SDF Text Rendering

Pulp renders text through a Signed Distance Field (SDF) pipeline so a
single atlas serves every font size with crisp edges. This page documents
the current state and the phased path to production quality.

## Pipeline

```
glyph → SkFont rasterize → EDT (Felzenszwalb) → SDF tile → atlas → GPU sampler
```

- `SdfAtlas` (`core/canvas/include/pulp/canvas/sdf_atlas.hpp`) builds the
  atlas. Real glyph metrics (bearing, advance) come from `SkFont` so
  layout matches the rasterized glyph pixels.
- Felzenszwalb & Huttenlocher (2004) two-pass distance transform produces
  the signed distance per texel, mapped to `uint8` with `128 = edge`.
- Padding around each glyph defines the SDF spread radius.

## Variants

| Variant | Channels | Status | Strength |
| ------- | -------- | ------ | -------- |
| SDF     | 1 (A8)   | working | simple, cheap, small atlas |
| MSDF    | 3 (RGB)  | planned | sharp corners, crisp thin strokes |
| PSDF    | 1        | planned | cheaper-to-generate alternative for simple geometry |

Multi-channel SDF (Chlumsky 2015) encodes three distance signals and
recovers the true edge via `median(R, G, B)` in the shader. This keeps
corners sharp where single-channel SDF rounds them off.

## Sampling shader

SkSL smoothstep sampler — single-channel:

```glsl
// See core/canvas/shaders/sdf_text.sksl
half4 main(float2 coord) {
    half d = sample(atlas, coord).r;
    half aa = fwidth(d);
    half a  = smoothstep(0.5 - aa, 0.5 + aa, d);
    return color * a;
}
```

MSDF adds `median(r, g, b)`:

```glsl
half3 s = sample(atlas, coord).rgb;
half d  = max(min(s.r, s.g), min(max(s.r, s.g), s.b));
```

## Subpixel positioning

`fwidth(d)` in the sampler shader gives a pixel-accurate AA width at
any zoom, so glyph quads can be placed at fractional pixel positions
without introducing distance-field aliasing. For animated UIs that
prefer stable edges to minimal sample error, use the snapping helper
in `<pulp/canvas/sdf_text.hpp>`:

```cpp
#include <pulp/canvas/sdf_text.hpp>
using pulp::canvas::SdfPenSnap;
using pulp::canvas::snap_pen_x;

float pen_x = snap_pen_x(fractional_x, SdfPenSnap::Nearest);
```

`SdfPenSnap` values:
- `Free` — pass-through; smoothest animation.
- `Nearest` — round to whole pixels; crisp at rest.
- `SubpixelThird` — round x to 1/3 px (LCD subpixel stripe); y to
  whole pixels.

The sampler shader is unchanged regardless of policy.

## Effects

A reusable effects layer — `glow`, `shadow`, `outline`, `bevel` — is
exposed via `SdfEffectParams` in `<pulp/canvas/sdf_effects.hpp>` and
backed by the `sdf_text_effects.sksl` shader. Design-token presets
(`preset_subtle_shadow()`, `preset_outline()`, `preset_glow()`,
`preset_pressed_bevel()`) compose onto any SDF or MSDF atlas without
extra geometry — outline and glow are shader-space ring sweeps and
bevel is a `dFdx`/`dFdy` light dot product. See
`examples/sdf-effects-demo/` for a runnable showcase of the four
presets plus a plain baseline.

## Runtime atlas management

`SdfAtlasCache` (in `<pulp/canvas/sdf_atlas_cache.hpp>`) lets UIs share
a single atlas across every `fill_text_sdf` call-site with per-glyph
dirty-rect upload hints and frame-based LRU eviction:

```cpp
SdfAtlasCache cache;
cache.initialize(font, seed_chars);
cache.ensure(U'☃');           // dynamic growth: rebuild atlas if missing
cache.touch(U'A');             // record recency for LRU
cache.next_frame();            // call once per rendered frame
cache.evict_older_than(600);   // drop glyphs unused for 10 seconds at 60fps
```

For procedural UI that needs SDFs beyond the font atlas (vector icons,
generated glyphs), `<pulp/canvas/path_to_sdf.hpp>` runs the same
Felzenszwalb-Huttenlocher EDT on a caller-supplied binary mask and
emits the `128 == edge` field the SDF samplers expect.

## Related

- `planning/next-features-plan.md` § Feature 4 — full phase plan
- `examples/sdf-text-demo/` — SDF vs MSDF comparison
- `examples/sdf-effects-demo/` — effects showcase across presets
- `docs/reference/modules.md` — module index
