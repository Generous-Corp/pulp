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

## Effects (planned)

Phase 4 adds a reusable effects layer: `glow`, `shadow`, `outline`,
`bevel`. These are implemented as shader uniforms on top of the SDF
sampler so any SDF text call-site gets the same effect module.

## Related

- `planning/next-features-plan.md` § Feature 4 — full phase plan
- `examples/sdf-text-demo/` — SDF vs MSDF comparison (planned)
- `docs/reference/modules.md` — module index
