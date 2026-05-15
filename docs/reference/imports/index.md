# Versioned Import Detection

`pulp import-design` ships a three-layer version model so the CLI surface
stays stable as external tools evolve their export formats. The model is
declared in [`compat.json`](../../../compat.json) and consumed by the
detector behind `pulp import-design --detect-only`.

## The three layers

| Layer | What it pins | Bumped when |
|-------|--------------|-------------|
| `parser-version`         | The Pulp parser implementation for a given source | The parser changes (bug fix, new IR mapping) |
| `format-version`         | The external export shape Pulp recognises | The upstream tool ships a new export shape |
| `compat-schema-version`  | The `compat.json` schema itself | The fingerprint vocabulary or top-level shape changes |

The three layers move independently. A new Stitch export shape is a
`format-version` bump (new entry under `detected-formats`). A bug fix
in the Stitch parser is a `parser-version` bump on the source-level
entry. A new fingerprint clause kind (`html-meta-tag`, say) is a
`compat-schema-version` bump.

## Recognized matrix

Auto-derived from `compat.json`. When a fixture is present, the detector
asserts the row in `pulp-test-cli-import-detect`.

| Source | Format-Version | Parser-Version | Fingerprint | Fixture |
|--------|----------------|----------------|-------------|---------|
| `claude` | `2024.10` | `2.1` | `html-script-type` (`__bundler/template`) | [`test/fixtures/imports/claude/2024.10/`](../../../test/fixtures/imports/claude/2024.10/) |
| `designmd` | `alpha` | `0.1` | `filename` (`DESIGN.md`) + `frontmatter-fence` + `frontmatter-key` (`name`) + `frontmatter-key` any-of (`colors`, `typography`, `rounded`, `spacing`, `components`); all-of, 95% min confidence. See [`reference/imports/designmd.md`](designmd.md). | [`test/fixtures/imports/designmd/alpha/`](../../../test/fixtures/imports/designmd/alpha/) |
| `figma`  | _(none yet — parser at `0.1`)_ | `0.1` | — | — |
| `pencil` | _(none yet — parser at `0.1`)_ | `0.1` | — | — |
| `stitch` | `2025.04` | `1.0` | `directory-files` (`code.html`, `DESIGN.md`, `screen.png`) + `html-script-src` (Tailwind CDN with forms+container-queries plugins) + `tailwind-config-token` (`surface-container`, `on-primary`) | [`test/fixtures/imports/stitch/2025.04/`](../../../test/fixtures/imports/stitch/2025.04/) |
| `v0`     | _(none yet — parser at `0.1`)_ | `0.1` | — | — |

The empty `detected-formats: []` arrays for figma / pencil / v0 reserve
the source-level `parser-version` slot — the per-source parsers exist
already; their format-versioning lands incrementally as the
buildable-React-project pipeline matures.

## Fingerprint vocabulary

Each entry under `detected-formats[].fingerprint` is one clause from
this vocabulary:

| `kind`                    | Required keys      | Match rule |
|---------------------------|--------------------|------------|
| `directory-files`         | `files: [...]`     | Every listed basename is present in the input directory's top level |
| `filename`                | `regex: "..."`     | The input file's basename matches the ECMAScript regex |
| `frontmatter-fence`       | `value: "..."`     | The input file starts with `value` and contains a matching closing fence later |
| `frontmatter-key`         | `required: "..."` or `any-of: [...]` | A `required` key is present in the YAML frontmatter; `any-of` matches if at least one listed key is present |
| `html-script-src`         | `regex: "..."`     | Some `<script src="...">` value matches the ECMAScript regex |
| `html-script-type`        | `value: "..."`     | Some `<script type="...">` value equals the string (case-insensitive trim) |
| `tailwind-config-token`   | `any-of: [...]`    | Some scraped Tailwind config identifier appears in `any-of` |

Confidence is `100 * matched-clauses / total-clauses`. The detector
picks the entry with the highest match count; ties resolve in
manifest order. Below 80% confidence the CLI emits a warning and a
`--report-new-format` invitation.

A detected-format entry may set `match: all-of` and an explicit
`min-confidence-pct` floor (e.g. 95). When present, the entry only
matches if every clause matches *and* the resulting confidence is at
or above the floor. The `designmd` entry uses this to keep generic
Markdown files with YAML frontmatter from being mis-detected; entries
without `match` retain the existing highest-match-wins behavior.

## CLI surface

```bash
# Detect only — no parsing, no codegen, no file writes
pulp import-design --detect-only --file <path>
pulp import-design --detect-only --directory <path>

# Override compat.json discovery (default: walk up from input path)
pulp import-design --detect-only --file <path> --compat /path/to/compat.json

# Emit a fingerprint diff suitable for hand-editing into a new
# detected-formats entry
pulp import-design --file <path> --report-new-format > stitch-2026.04.json
```

`--detect-only` exit codes:

- `0` — match found, printed to stdout
- `1` — usage error (missing input, malformed compat.json, …)
- `2` — no match (`source: ""`)

### Example output

```
detected source: stitch
  format-version: 2025.04
  parser-version: 1.0
  fingerprint match: 3/3 (directory-files, html-script-src, tailwind-config-token)
  confidence: 100%
```

Below 80%:

```
detected source: stitch
  format-version: 2025.04
  parser-version: 1.0
  fingerprint match: 1/3 (html-script-src)
  confidence: 33%
warning: confidence below 80% — this export may be a newer
         format-version than Pulp recognises. Pulp will use
         the most-recent matching parser; gaps surface in
         import-report.json. To file a new format detector:
  pulp import-design --file <path> --report-new-format
```

## Adding a new format-version

1. Run `pulp import-design --file <new-export> --report-new-format` to
   diff the new export against the closest known format.
2. Hand-edit the output into a new entry under
   `compat.json[imports/<source>/detected-formats]` — set
   `introduced` to the export date.
3. Add a fixture under `test/fixtures/imports/<source>/<format-version>/`
   with an `expected.json` sidecar.
4. Run `ctest --test-dir build -R pulp-test-cli-import-detect` to
   confirm the fixture asserts the row.
5. Bump `parser-version` if the parser also changed; bump
   `compat-schema-version` if you added a new `kind` to the
   fingerprint vocabulary.

## Cross-references

- [`compat.json`](../../../compat.json) — the source of truth.
- [Style props reference](../style-props.md) — RN-style style vocab
  consumed downstream by the per-source parsers.
- [Design Import API reference](../design-import.md) — the parser
  output shape and CLI flag list.
- Buildable React project support — the
  `parser-version`-vs-`format-version` matrix exercises end-to-end
  once the parser path is available; until then the schema +
  detect-only surface ship in isolation.
- `compat.json` ownership — import entries are alphabetically sorted
  so broader compat-surface merges stay mechanical.
- `compat-sync` hook — `tools/scripts/compat_path_map.json` routes
  edits to per-source parsers into a `parser-version` bump check.
