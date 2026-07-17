---
name: sdf-text
description: Work with Pulp's SDF / MSDF / PSDF glyph atlases — building, sampling via SkSL, and the shared text-layout helpers.
---

# SDF Text

Pulp renders text through a GPU-sampled signed-distance-field pipeline so
a single atlas serves every font size with crisp edges. This skill is
for when you are touching `core/canvas/src/sdf_atlas.cpp`,
`msdf_atlas.cpp`, or the shader files, or when you add call-sites that
consume `pulp/canvas/sdf_text.hpp`.

## Where things live

- `core/canvas/include/pulp/canvas/sdf_atlas.hpp` — single-channel atlas
- `core/canvas/include/pulp/canvas/msdf_atlas.hpp` — multi-channel atlas (RGB/RGBA)
- `core/canvas/include/pulp/canvas/psdf_atlas.hpp` — pseudo-SDF + vector-fallback policy
- `core/canvas/include/pulp/canvas/sdf_text.hpp` — `SdfPenSnap`, `SdfTextOptions`,
  `build_text_quads<AtlasT>()`, `fill_text_sdf/msdf/psdf<AtlasT>()`
- `core/canvas/shaders/sdf_text.sksl` — single-channel sampler
- `core/canvas/shaders/msdf_text.sksl` — `median(r,g,b)` sampler
- `docs/reference/sdf-text.md` — rendering pipeline reference
- `examples/sdf-text-demo/` — runtime SDF text rendering and bloom demo
- `examples/sdf-vs-msdf-demo/` — console SDF/MSDF atlas dump; MSDF RGB
  output is currently single-channel-equivalent until `msdfgen` is wired
- `examples/sdf-effects-demo/` — enumerates host-side effect presets and
  writes software-rendered baseline files; visual effects wait for the SkSL
  draw path

## Gotchas

1. **Metrics fall back silently when Skia has no default typeface.** In
   headless tests `SdfAtlas::build("")` can leave `advance = base_size`
   and `bearing_y = base_size` for every glyph. Write tests that assert
   *invariants that hold in both the real and fallback paths*, and
   tighten only when you can detect the real path was taken
   (`gM->advance != gi->advance`). See `test_sdf_atlas.cpp`.
2. **`fwidth` gives you subpixel AA for free.** Do not add a second
   softness uniform for "anti-alias width" — snapping belongs at the
   pen (host side), not in the shader.
3. **`include_alpha=true` changes the pixel stride** from 3 to 4 bytes.
   Use `MsdfAtlas::channels()` rather than assuming 3.
4. **Templated quad builder**. `build_text_quads<AtlasT>()` only requires
   that the atlas expose `glyph(c)`, `base_size()`, and the usual
   `SdfGlyph`-shaped fields. That means the same helper works for every
   atlas variant; do not branch on type.
5. **Vector fallback threshold defaults to 8×**. `should_use_vector_fallback`
   is a pure policy helper; callers decide at draw time whether to hit
   the Skia path rasterizer instead of the SDF sampler.
6. **Always call `SkFont` via the `SkSpan` overloads.** The historical
   `(pointer, count, ...)` forms of `unicharsToGlyphs`, `textToGlyphs`,
   `getWidthsBounds`, `getWidths`, `getPos`, `getXPos`, `getPaths`, and
   `getIntercepts` are gated behind `SK_SUPPORT_UNSPANNED_APIS` in
   `include/core/SkFont.h`. On Skia builds where that macro is not
   defined (including some chrome/m144 drops), the overloads vanish and
   `sdf_atlas.cpp` fails to compile. On this repo `PULP_HAS_SKIA` is
   only set when `libskia.a` links successfully, so the break can hide
   behind a headers-only pin — confirm by building `pulp-canvas`
   against a tree that ships `libskia.a` (e.g.
   `SKIA_DIR=<other-worktree>/external/skia-build`) so the compile path
   exercises real Skia libraries, not just headers.

## Build + test loop

```bash
tools/ci/governed-build.sh cmake --build build --target \
  pulp-test-sdf-atlas pulp-test-msdf-atlas pulp-test-psdf-atlas \
  pulp-test-sdf-text pulp-test-sdf-effects \
  pulp-sdf-text-demo pulp-sdf-vs-msdf-demo pulp-sdf-effects-demo

./build/test/pulp-test-sdf-atlas
./build/test/pulp-test-msdf-atlas
./build/test/pulp-test-psdf-atlas
./build/test/pulp-test-sdf-text
./build/test/pulp-test-sdf-effects
./build/examples/sdf-text-demo/pulp-sdf-text-demo
./build/examples/sdf-vs-msdf-demo/pulp-sdf-vs-msdf-demo   # writes /tmp/pulp-*-atlas.{pgm,ppm}
./build/examples/sdf-effects-demo/pulp-sdf-effects-demo   # writes /tmp/pulp-sdf-effects-*.pgm
```

## Future work (deferred, see `planning/next-features-plan.md` § Feature 4)

- Vendor Chlumský's `msdfgen` library (MIT) for shape-decomposed channels
- Wire `SdfEffectParams` through a SkSL-backed draw path so glow / shadow /
  outline / bevel visibly render
- Runtime atlas growth + LRU eviction
