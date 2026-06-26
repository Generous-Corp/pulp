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
| MSDF    | 3 (RGB)  | scaffold | intended for sharp corners once `msdfgen` supplies true channel-separated distances |
| PSDF    | 1        | planned | cheaper-to-generate alternative for simple geometry |

Multi-channel SDF (Chlumsky 2015) encodes three distance signals and
recovers the true edge via `median(R, G, B)` in the shader. This keeps
corners sharp where single-channel SDF rounds them off.

Pulp's current `MsdfAtlas` is a structural scaffold: it packs RGB/RGBA
atlas tiles and exercises the `median(R, G, B)` sampler contract, but
the generator writes equal RGB placeholder distances until `msdfgen` is
integrated. The current MSDF atlas therefore validates plumbing, not
the final sharp-corner quality target.

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

The host-side effects contract is exposed via `SdfEffectParams` in
`<pulp/canvas/sdf_effects.hpp>` and mirrored by the
`sdf_text_effects.sksl` shader for `glow`, `shadow`, `outline`, and
`bevel`. Design-token presets (`preset_subtle_shadow()`,
`preset_outline()`, `preset_glow()`, `preset_pressed_bevel()`) define
the intended uniform payload.

The visible effect rendering path is still pending: the current
`examples/sdf-effects-demo/` enumerates the presets and writes
software-rendered baseline files, but the software renderer does not
interpret `SdfEffectParams`. The presets become visible once the SkSL
effects shader is wired into an SDF text draw call.

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
- `examples/sdf-text-demo/` — runtime SDF text rendering and bloom demo
- `examples/sdf-vs-msdf-demo/` — console SDF/MSDF atlas scaffold dump
- `examples/sdf-effects-demo/` — host preset enumeration plus software baseline files
- `docs/reference/modules.md` — module index
