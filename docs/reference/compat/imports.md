# Imports compat

The `imports/*` prefix tracks **design-import format detection** —
the heuristics Pulp's import-design pipeline uses to recognize a
directory or file payload from a particular external design tool.

This is distinct from the CSS / RN / HTML / Canvas2D matrices. Those
track property-level support; this section tracks source-format
support.

The authoritative inventory is `compat.json` (`imports/*` prefix).

## Generation

Last refresh: **2026-05-04** against `origin/main` at SHA `a5f4f5ac`.

Wired by PR #1047 (pulp #1031) — versioned import-design detection.
See `core/dsl/import-design/` and `tools/cli/src/import_design.cpp`.

## Sources

| Key | Parser version | Detected formats |
|-----|---------------:|-----------------:|
| `claude` | 2.1 | 1 (Anthropic Labs Claude Design 2024.10) |
| `figma` | 0.1 | 0 (placeholder) |
| `pencil` | 0.1 | 0 (placeholder) |
| `stitch` | 1.0 | 1 (Stitch 2025.04) |
| `v0` | 0.1 | 0 (placeholder) |

## Format-detection convention

Each `detected-formats` entry carries:

- `format-version`: a date-stringly version (when the export shape
  changed).
- `introduced` / `deprecated`: when this Pulp parser started / stopped
  matching it.
- `fingerprint`: an array of heuristics, each `{kind, ...}`. Recognized
  kinds:
  - `directory-files` — required files at the top of a directory ZIP
  - `html-script-type` — required `<script type="...">` value
  - `html-script-src` — regex match against `<script src="...">`
  - `tailwind-config-token` — Tailwind utility token presence (any-of)
- `notes`: human-readable description.

## Adding a new format

When a new import source lands or an existing source's export format
changes:

1. **Add** a new `detected-formats` entry rather than mutating an
   existing one (so historical exports keep parsing).
2. Update `parser-version` if the detector logic itself changed.
3. Set `deprecated` on the old entry only if you are *also* removing
   the parser path.

## Recently changed

- `claude/2024.10`: existing entry. Anthropic Labs Claude Design —
  manually-exported standalone HTML detected via the inline bundler
  script tag inserted by the export.
- `stitch/2025.04`: existing entry. Material 3 token vocab + Tailwind
  CDN with forms/container-queries plugins is the strongest signal.
