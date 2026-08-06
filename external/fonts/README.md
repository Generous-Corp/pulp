# Embedded Fonts

These fonts are embedded into Pulp plugins at build time for deterministic text rendering.

| Font | Version | SHA-256 | License | Source |
|------|---------|---------|---------|--------|
| Inter Regular | `4.001;git-9221beed3` | `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82` | SIL Open Font License 1.1 | https://github.com/rsms/inter |
| JetBrains Mono Regular | `2.304` | `a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f` | SIL Open Font License 1.1 | https://github.com/JetBrains/JetBrainsMono |
| Noto Color Emoji (COLRv1) | `noto-emoji main @ 2026-05-17` | `0ae57fe58645638523ba35f388d93739d292539a9acb84df5700c81b1e1a28d2` | SIL Open Font License 1.1 | https://github.com/googlefonts/noto-emoji |
| Funnel Display (VariableFont wght) | `google/fonts @ ofl/funneldisplay` | `b4151c9c4b7b07eb74320096b4ff4156cca8821f5adfab34af9fd9a2d6c1179d` | SIL Open Font License 1.1 | https://github.com/google/fonts/tree/main/ofl/funneldisplay |
| Jost Regular | `3.710` | `c3143e923ed1ca7bdf27f96c351fbafaebcbd3cf3f4c2d30d03e6c7f98e73d7a` | SIL Open Font License 1.1 | https://github.com/indestructible-type/Jost |
| Jost Medium | `3.710` | `d6ff7726ec21576cf2fdac55080b2d43832780fa981f03f0b66d2723a7c1ea09` | SIL Open Font License 1.1 | https://github.com/indestructible-type/Jost |
| Jost SemiBold | `3.710` | `a63c8d75600a2d42e0e152e4c4810474a90a0b93206f47530a741dbb78a9e571` | SIL Open Font License 1.1 | https://github.com/indestructible-type/Jost |
| Jost Bold | `3.710` | `3e49280c154002dcbab4344a77ad291d5587d4157b24b5a02341f68cccd24615` | SIL Open Font License 1.1 | https://github.com/indestructible-type/Jost |

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

Four Jost weights ship, not one, because `match_bundled_typeface` refuses a face
whose weight is too far from the request: with only Regular bundled, a 600
heading did not fall back to Jost Regular — it fell past the bundle entirely to
a platform substitute. Every heading, button and title in a design lives at a
non-400 weight, so a family bundled at one weight abandons itself exactly where
it is most visible.

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
