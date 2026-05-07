# Text Shaping Determinism

Pulp's text shaping path is deterministic only when the shaping stack and font
inputs are fixed. The visual harness treats these files and pins as part of the
golden-file contract.

## Rendering Stack

| Component | Pin |
|-----------|-----|
| Skia | `chrome/m144 @ cd0c5f445516ea4e90e02b5f634cbc5ca23b5a44` |
| skia-builder | `7eecb8abf1f77b2a8bac2e81c38e20708cb79c24` |
| Dawn | `6acf6ef3fe237cd4be7b825389602c93a1f16f2f` from Skia `DEPS` |
| HarfBuzz | `08b52ae2e44931eef163dbad71697f911fadc323` from Skia `DEPS` |
| ICU | `364118a1d9da24bb5b770ac3d762ac144d6da5a4` from Skia `DEPS` |

Skia, HarfBuzz, ICU, SkParagraph, and SkUnicode are consumed through the
prebuilt Skia toolchain tracked in `external/skia-build/VERSION.md` and
`tools/deps/manifest.json`.

## Bundled Fonts

| Family | File | Version | SHA-256 |
|--------|------|---------|---------|
| Inter | `external/fonts/Inter-Regular.ttf` | `4.001;git-9221beed3` | `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82` |
| JetBrains Mono | `external/fonts/JetBrainsMono-Regular.ttf` | `2.304` | `a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f` |

The fallback chain for deterministic rendering is:

1. Requested bundled family when present.
2. Inter Regular for proportional UI text.
3. JetBrains Mono Regular for monospace text.
4. Character-width estimation only when Skia text shaping is disabled or a
   bundled typeface cannot be loaded.

Host system fonts are not part of the visual harness contract. Tests that need
stable glyph boxes must use this bundled font set.

## Golden Regeneration Triggers

Regenerate affected visual goldens when any of these change:

- Skia branch, commit, skia-builder ref, or release asset digest.
- Dawn, HarfBuzz, or ICU revision in Skia `DEPS`.
- Any bundled font file, version, SHA-256, or fallback order.
- Sampler configuration, color type, alpha type, DPI, or raster/GPU backend.
- Text measurement behavior in `pulp::canvas::TextShaper`.

Layout-only Yoga snapshots do not depend on these font pins unless a fixture
captures measured text boxes.
