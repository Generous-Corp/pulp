# Ink & Signal — Provenance & License

This directory vendors the **"Ink & Signal"** design language assets that seed
Pulp's global design system.
It exists so the token foundation, Figma library, and fidelity baseline are
reproducible from a clean clone — no external `~/Downloads` bundle required.

## What's here

| Path | Contents | Origin |
|---|---|---|
| `tokens/pulp.tokens.json` | Tokens-Studio-format design tokens (`core` + `dark` + `light`) | Original Pulp work |
| `tokens/css/*.css` | The same language as CSS custom properties (colors, semantic, themes, spacing, typography, elevation, motion, fonts) | Original Pulp work |
| `reference/screenshots/*.png` | Per-component reference renders used as the fidelity-diff baseline | Original Pulp work |

## License determination (MIT-compatible)

The Pulp repository is MIT-licensed and forbids GPL/LGPL/AGPL/SSPL/proprietary
code or shipped artifacts (see `CLAUDE.md` → License Policy). Each vendored input
was checked against that policy:

- **Tokens, CSS, and reference screenshots** are original work produced for the
  Pulp project itself (the export's `readme.md` records the direction was "set
  with the maintainer" and names the source repo `Generous-Corp/pulp`, MIT). No
  third-party lineage → MIT, ships freely.
- **Fonts** — the language references **Jost** (display/UI) and **JetBrains
  Mono** (values/code) *by family name only*. No font binaries are vendored here.
  Both are SIL OFL 1.1 (MIT-compatible); if/when binaries are bundled for
  shipping, add an OFL `NOTICE.md` entry at that time.
- **Icons** — vendored under `icons/` (66 line-style 24×24 SVGs,
  signal-teal stroke). The generic glyphs are **Lucide**-derived (ISC); the
  audio glyphs (compressor, delay, distortion, eq, filter, fader, envelope,
  reverb, …) are original Pulp work. ISC is MIT-compatible; attribution is
  recorded in `NOTICE.md` and `DEPENDENCIES.md` (Lucide, ISC). These are
  reference assets only — not compiled into or shipped in plugin/app binaries.
  They may later be replaced with Pulp's own in-engine SDF glyph set.

No reviewed input is copyleft or otherwise incompatible with the MIT release.

## Notes for implementers

- This JSON is the **buildable source of truth**. Figma Variables are the
  designer-facing *editor*; the round-trip exports back to this format
  (`design_tokens.hpp` → `export_figma_variables` / W3C / CSS).
- The `dark`/`light` token sets reference `core` inks via `{ink.signal}`-style
  aliases — resolve those when mapping into Pulp's `SemanticColors`.
- Source bundle for anything not vendored here (JSX components, `ui_kits/`,
  full icon set, HTML specimens): the maintainer's `Pulp Design System-2` export.
