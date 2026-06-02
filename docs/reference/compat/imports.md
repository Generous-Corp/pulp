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

## Object coverage

Beyond *format detection*, the `imports/object-coverage` block tracks
which normalized **DesignIR object types** the pipeline lowers, at three
stages:

- `detected` — an adapter recognizes the source object.
- `parsed` — it is lowered into the normalized IR.
- `codegen` — `generate_pulp_js` emits a real renderable primitive for it.

Levels are `handled | partial | missing`. The matrix is **IR-level and
source-agnostic**: because the importer and codegen operate on the
normalized IR, a `handled` type renders identically for every source
(figma, figma-plugin, pencil, stitch, v0, claude).

`types` rows are **drift-checked** by
`test/test_import_object_coverage.cpp` (binary
`pulp-test-import-object-coverage`, Catch2 tag `[object-coverage]`): it
builds a synthetic `IRNode` per type, runs `generate_pulp_js`, and fails
if a `codegen: handled` type drops to an empty frame, or a
`codegen: missing` vector/path kind does not trip the dropped-vector
invariant. This keeps the matrix honest against the code — a claim
cannot silently rot.

`features` rows track cross-cutting paint/layout capabilities
(per-range text, gradients, masks, mix-blend-mode, constraints, grid)
owned by separate hardening slices; they are documented here but not
type-drift-checked.

The current snapshot: `frame`/`text`/`label`/`image` are `codegen:
handled`. The vector SHAPE PRIMITIVES (`rect`/`rectangle`/`svg_rect`/
`line`/`svg_line`/`ellipse`/`circle`/`polygon`/`star`) are now `codegen:
handled` — the importer synthesizes SVG path-data from their geometry so
they lower to a native `SvgPath` instead of dropping. The generic
`vector`/`path`/`svg_path` kinds are `codegen: partial` (render only when
they carry authored `path_data`); `polyline` stays `codegen: missing`
(an open run of explicit points, not synthesizable from geometry). Among
`features`: radial/conic background gradients now render (was a flat
fallback), Figma resize constraints map to flex/position, and grid
containers lower to the native grid bridge. Seeded from
`planning/2026-05-31-import-coverage-hardening-plan.md` §3.

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
