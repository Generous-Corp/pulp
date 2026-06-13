# Ink & Signal — Provenance & License

This directory vendors the **"Ink & Signal"** design language assets that seed
Pulp's global design system (see `planning/Design-System-Import-Plan.md`, Phase 0).
It exists so the token foundation, Figma library, and fidelity baseline are
reproducible from a clean clone — no external `~/Downloads` bundle required.

## What's here

| Path | Contents | Origin |
|---|---|---|
| `tokens/pulp.tokens.json` | Tokens-Studio-format design tokens (`core` + `dark` + `light`) | Original Pulp work |
| `tokens/css/*.css` | The same language as CSS custom properties (colors, semantic, themes, spacing, typography, elevation, motion, fonts) | Original Pulp work |
| `reference/screenshots/*.png` | Per-component reference renders used as the fidelity-diff baseline (Phase 6) | Original Pulp work |

## License determination (MIT-compatible)

The Pulp repository is MIT-licensed and forbids GPL/LGPL/AGPL/SSPL/proprietary
code or shipped artifacts (see `CLAUDE.md` → License Policy). Each vendored input
was checked against that policy:

- **Tokens, CSS, and reference screenshots** are original work produced for the
  Pulp project itself (the export's `readme.md` records the direction was "set
  with the maintainer" and names the source repo `danielraffel/pulp`, MIT). No
  third-party lineage → MIT, ships freely.
- **Fonts** — the language references **Jost** (display/UI) and **JetBrains
  Mono** (values/code) *by family name only*. No font binaries are vendored here.
  Both are SIL OFL 1.1 (MIT-compatible); if/when binaries are bundled for
  shipping, add an OFL `NOTICE.md` entry at that time.
- **Icons** — the export's `readme.md` flags the icon set as **Lucide** (ISC
  license) stand-ins, to be swapped for Pulp's own in-engine SDF set in
  production. ISC is MIT-compatible, but to keep provenance crisp the icons are
  **intentionally NOT vendored in Phase 0**. They are brought in at Phase 3
  (Figma component library) together with a `NOTICE.md` / `DEPENDENCIES.md`
  Lucide (ISC) attribution entry, or replaced with original SDF glyphs.

No reviewed input is copyleft or otherwise incompatible with the MIT release.

## Notes for implementers

- This JSON is the **buildable source of truth**. Figma Variables are the
  designer-facing *editor*; the round-trip exports back to this format
  (`design_tokens.hpp` → `export_figma_variables` / W3C / CSS).
- The `dark`/`light` token sets reference `core` inks via `{ink.signal}`-style
  aliases — resolve those when mapping into Pulp's `SemanticColors`.
- Source bundle for anything not vendored here (JSX components, `ui_kits/`,
  full icon set, HTML specimens): the maintainer's `Pulp Design System-2` export.
