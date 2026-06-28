# DESIGN.md import

`pulp import-design --from designmd` reads Google's
[DESIGN.md](https://github.com/google-labs-code/design.md) format (Apache-2.0) — a
YAML-frontmatter + Markdown-body description of a design *system* — and
emits a W3C Design Tokens Community Group (DTCG) `tokens.json`. The
upstream spec is the source of truth for the format; this page documents
the Pulp parser, its detection rules, and the tokens-only import contract.

Because DESIGN.md describes a system (colors, typography, spacing,
component recipes), not a screen, the importer **does not** emit a
`ui.js`. That's a deliberate split from the other import sources, which
all start from a screen export. Pair this importer with a separate
screen import (Figma, Stitch, Pencil, v0, Claude) when you want a full
UI; use it standalone when you only need to bring a token system into
Pulp.

## Quickstart

```bash
pulp import-design --from designmd --file DESIGN.md --tokens tokens.json
```

Produces:

- `tokens.json` — DTCG token tree with `$type` and `$value` for every
  parsed key. Reference resolution is applied (see below). Composite
  typography references inside `components.*` are preserved verbatim so
  downstream tooling can resolve them in context.

Does **not** produce:

- `ui.js` — DESIGN.md has no screen. Component scaffolding from
  `components.*` remains future work; the current importer stops at
  tokens.
- `classnames.json` — that artifact is specific to the `claude` source.
- Any bridge or React scaffold — there's no view to render.

Detect-only (no file writes):

```bash
pulp import-design --from designmd --file DESIGN.md --detect-only
```

## Supported subset

The parser handles the canonical frontmatter keys (tracked against the
upstream format spec, pinned at tag `0.3.0`). Unrecognized top-level keys
are flagged with a warning and otherwise ignored, so a typo'd key surfaces
instead of silently dropping its tokens.

| Key | Parsed | Notes |
|-----|--------|-------|
| `version` | yes | Stored on the IR; not emitted into `tokens.json`. |
| `name` | yes | Required for detection. Stored on the IR. |
| `description` | yes | Block scalars (`|`, `>`) handled by yaml-cpp. |
| `colors.*` | yes | Each entry becomes a `color` token. The value may be any valid CSS color — hex (`#RGB`/`#RGBA`/`#RRGGBB`/`#RRGGBBAA`), a named keyword (`cornflowerblue`, `transparent`), or a functional notation (`rgb()`, `hsl()`, `hwb()`, `oklch()`, `lab()`, `color-mix()`, …) — and is preserved verbatim. Nested palettes nest to arbitrary depth and key on the dot-joined path (e.g. `colors.background.light` → token `background.light`). |
| `typography.*` | yes | Composite `typography` tokens with `fontFamily`, `fontSize`, `fontWeight`, `lineHeight`, `fontFeature`, `fontVariation`. |
| `rounded.*` | yes | `dimension` tokens. Nested levels nest to arbitrary depth (dot-joined path). |
| `spacing.*` | yes | `dimension` tokens. A bare number (e.g. `base: 8`) is read as px per spec; nested levels nest to arbitrary depth. Genuinely non-dimensional values (e.g. `auto`) are preserved as strings. |
| `components.*` | yes | Each component is a flat map of token references and literal style values; numeric and boolean YAML scalars (e.g. `fontWeight: 600`, `enabled: true`) flow through as strings. References are *not* resolved at parse time (see below). |
| Unknown top-level keys | warn | The parser emits one `designmd.unknown-key` warning per unrecognized top-level key (catches typos like `color:`/`typgrphy:`) and otherwise ignores it. The file still imports. |
| Unknown component properties | passthrough | Per the spec, unknown component props are preserved as opaque strings. |

When frontmatter is present it is the sole token source; the Markdown body
after the closing `---` fence is parsed for headings and lint context only.

When a file has **no frontmatter**, the parser falls back to scanning the
Markdown body so prose-authored files (common for Stitch / Brand-Kit
exports) don't import as an empty token set. It reads `name: value` list
items and `| name | value |` table rows under these sections:

| Body section | Tokens |
|--------------|--------|
| `## Colors` / `## Color Palette` | `color` tokens (header/separator rows skipped) |
| `## Spacing` | `spacing-<name>` dimensions |
| `## Border Radius` / `## Rounded` | `rounded-<name>` dimensions |
| `## Shadows` / `## Elevation` | `shadow-<name>` string tokens |

A `### Light Mode` / `### Dark Mode` subsection under `## Colors` routes
values to the bare token name (light/default) or a `<name>.dark` suffix
(dark) — the same multi-mode convention the Figma plugin uses, so dark
themes land in the flat token maps.

## Reference resolution

DESIGN.md uses `{group.key}` for references. Pulp's resolver runs at
parse time:

| Reference | Resolves to | Behavior |
|-----------|-------------|----------|
| `{colors.primary}` | `#1A1C1E` (the literal value) | Inlined in the emitted token's `$value`. |
| `{colors}` | (group reference) | Outside `components.*`: emits a `designmd.broken-ref` warning. DTCG has no concept of a group alias. |
| `{typography.label-md}` | (composite) | Inside `components.*`: preserved verbatim in the component's `$value`. Outside `components.*`: warns. |

Cycle detection is depth-limited at 10 hops; deeper chains warn and
stop expanding. Unresolved references (`{colors.does-not-exist}`) emit
a `designmd.broken-ref` diagnostic at the reference's source line and
leave the literal `{…}` string in the output.

## Detection

The `designmd` fingerprint in `compat.json` is **all-of** with a 95%
minimum confidence floor. All four clauses must match:

1. **`filename`** — the input file's basename matches `^DESIGN\.md$`
   (case-insensitive).
2. **`frontmatter-fence`** — the file starts with a `---` line followed
   by another `---` line later in the document.
3. **`frontmatter-key`** (required) — the frontmatter contains a
   `name:` key.
4. **`frontmatter-key`** (any-of) — the frontmatter contains at least
   one of `colors`, `typography`, `rounded`, `spacing`, `components`.

The combination is intentionally strict. A generic Jekyll or Hugo blog
post with `name:` in its frontmatter will not match because it has no
canonical DESIGN.md token group. Three decoys live under
`test/fixtures/imports/designmd/alpha/decoys/` and are exercised by
`pulp-test-cli-import-detect`:

- `jekyll/DESIGN.md` — Jekyll post with `name:` and `layout:` but no
  token groups → does not match.
- `missing-name/DESIGN.md` — has `colors:` but no `name:` → does not
  match.
- `missing-token-groups/DESIGN.md` — has `name:` and `description:`
  but no canonical token groups → does not match.

A `--detect-only` invocation on a real DESIGN.md prints:

```
detected source: designmd
  format-version: alpha
  parser-version: 0.1
  fingerprint match: 4/4 (filename, frontmatter-fence, frontmatter-key[name], frontmatter-key[colors|typography|rounded|spacing|components])
  confidence: 100%
```

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success — `tokens.json` written (or detect-only match printed). |
| `1` | Usage error or write failure (missing `--file`, unwritable `--tokens` path, permission denied). |
| `2` | Detect-only run found no match. |
| `3` | Parse error — malformed YAML frontmatter, duplicate top-level section, missing closing fence. |
| `4` | Unsupported feature — encountered a construct the parser explicitly rejects rather than silently dropping (reserved; nothing currently triggers it). |

## Diagnostics

The parser emits one diagnostic per line on stderr in this format:

```
[severity] code at path (line:column): message
```

Example:

```
[warning] designmd.broken-ref at colors.accent (14:12): reference {colors.does-not-exist} could not be resolved
[warning] designmd.unknown-section at extras (42:1): top-level key 'extras' is not part of the canonical schema; preserved as opaque string
[error]   designmd.duplicate-section at colors (3:1): 'colors' appears more than once in frontmatter
```

`severity` is one of `error` (exit code 3 or 4), `warning` (does not
affect exit code), or `info` (suppressed unless `--debug`).

## Current contract

The import path ships exactly the surface above:

- `pulp import-design --from designmd` → `tokens.json`.
- Detection in `compat.json` with the strict fingerprint.
- Test fixtures (paws-and-paths from upstream, plus a hand-authored
  edge-case fixture and three decoys).
- This documentation page and the cross-references listed below.
- `pulp design lint <DESIGN.md>` for DESIGN.md quality findings.
- `pulp design diff <before.md> <after.md>` for semantic token diffs.

Tailwind v3 + v4 exporters remain future work.

Treating DESIGN.md as a project source of truth remains future work:
`pulp design` does not yet hydrate the live theme from `DESIGN.md`, and
`pulp design save --update-design-md` does not yet round-trip changes
back. Component scaffolding from `components.*` into Pulp widgets is in
the same future-work bucket.

## Not yet supported

- Tailwind v3 / v4 export.
- Live runtime hydration of Pulp's theme from a DESIGN.md file.
- Widget scaffolding from `components.*`.
- An `npx @google/design.md spec` equivalent — out of scope. Pulp
  ships the format documentation as this page; the upstream spec at
  [github.com/google-labs-code/design.md](https://github.com/google-labs-code/design.md)
  remains authoritative for the format itself.

## Attribution

- yaml-cpp (MIT) is vendored to parse the frontmatter; see
  `DEPENDENCIES.md` for the pin and `NOTICE.md` for the formal license
  text.
- The `paws-and-paths` test fixture under
  `test/fixtures/imports/designmd/alpha/DESIGN.md` is copied verbatim
  from the upstream
  [google/design.md](https://github.com/google-labs-code/design.md) repository
  (Apache-2.0). `NOTICE.md` carries the upstream attribution alongside
  the project's other third-party notices.
