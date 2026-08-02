# Embedded Fonts

These fonts are embedded into Pulp plugins at build time for deterministic text rendering.

| Font | Version | SHA-256 | License | Source |
|------|---------|---------|---------|--------|
| Inter Regular | `4.001;git-9221beed3` | `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82` | SIL Open Font License 1.1 | https://github.com/rsms/inter |
| JetBrains Mono Regular | `2.304` | `a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f` | SIL Open Font License 1.1 | https://github.com/JetBrains/JetBrainsMono |
| Noto Color Emoji (COLRv1) | `noto-emoji main @ 2026-05-17` | `0ae57fe58645638523ba35f388d93739d292539a9acb84df5700c81b1e1a28d2` | SIL Open Font License 1.1 | https://github.com/googlefonts/noto-emoji |
| Funnel Display (VariableFont wght) | `google/fonts @ ofl/funneldisplay` | `b4151c9c4b7b07eb74320096b4ff4156cca8821f5adfab34af9fd9a2d6c1179d` | SIL Open Font License 1.1 | https://github.com/google/fonts/tree/main/ofl/funneldisplay |
| Jost Regular | `3.710` | `c3143e923ed1ca7bdf27f96c351fbafaebcbd3cf3f4c2d30d03e6c7f98e73d7a` | SIL Open Font License 1.1 | https://github.com/indestructible-type/Jost |

All fonts are used under the SIL OFL 1.1 which permits bundling in software.

Funnel Display is a **test-only** fixture: it is NOT compiled into plugin
binaries (it is absent from `bundled_blobs()`); it exists solely as a
deterministic variable-font (`wght` axis 300–800) for the
`register_font`/SkParagraph variable-weight regression tests
(`PULP_TEST_VARIABLE_FONT_PATH`). The bundled set is Inter +
JetBrains Mono + Jost (+ optional Noto Color Emoji).

Jost is bundled because imported designs ask for it and no host ships it, so
resolution falls through to a substitute roughly 10% wider per glyph — which
puts every wrap point and every centred run in the wrong place, a whole-panel
error that reads as a layout bug rather than a font one. This file's advances
agree with the face Chrome shaped the reference corpus with to within 0.16%
(median over 55 single-line runs). The already-bundled JetBrains Mono agrees to
0.002% over 169 runs by the same measurement, which is what says the comparison
is detecting drift rather than reporting its own noise.

The Noto Color Emoji bundle is gated by the CMake option
`PULP_BUNDLE_NOTO_COLOR_EMOJI`:
- Defaults **ON** for Linux, Android, headless / CI builds, and macOS /
  Windows builds that ask for deterministic emoji rendering.
- Defaults **OFF** for the standard macOS / Windows release path, which
  delegates to the platform color-emoji typeface (Apple Color Emoji /
  Segoe UI Emoji). The platform path is preferred for visual integration
  with the host OS; the bundled path is preferred for tests, CI goldens,
  and any deployment where the host emoji set is unknown.

The deterministic visual harness uses this explicit font set instead of host
system fallback. Changes to the files, versions, hashes, or fallback order are
golden-regeneration triggers.
