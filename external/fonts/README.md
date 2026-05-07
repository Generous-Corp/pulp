# Embedded Fonts

These fonts are embedded into Pulp plugins at build time for deterministic text rendering.

| Font | Version | SHA-256 | License | Source |
|------|---------|---------|---------|--------|
| Inter Regular | `4.001;git-9221beed3` | `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82` | SIL Open Font License 1.1 | https://github.com/rsms/inter |
| JetBrains Mono Regular | `2.304` | `a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f` | SIL Open Font License 1.1 | https://github.com/JetBrains/JetBrainsMono |

Both fonts are used under the SIL OFL 1.1 which permits bundling in software.

The deterministic visual harness uses this explicit font set instead of host
system fallback. Changes to the files, versions, hashes, or fallback order are
golden-regeneration triggers.
