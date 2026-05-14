# DESIGN.md import

`pulp import-design --from designmd` reads Google's
[DESIGN.md](https://github.com/google-labs-code/design.md) format (Apache-2.0) — a
YAML-frontmatter + Markdown-body description of a design *system* — and
emits a W3C Design Tokens Community Group (DTCG) `tokens.json`. The
upstream spec is the source of truth for the format; this page documents
the Pulp parser, its detection rules, and the Phase 1 contract.

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

- `ui.js` — DESIGN.md has no screen. Phase 3 will scaffold widgets from
  `components.*`, but Phase 1 stops at tokens.
- `classnames.json` — that artifact is specific to the `claude` source.
- Any bridge or React scaffold — there's no view to render.

Detect-only (no file writes):

```bash
pulp import-design --from designmd --file DESIGN.md --detect-only
```

## Supported subset

Phase 1 parses the canonical frontmatter keys. Everything else passes
through as opaque strings, mirroring the upstream spec's "ignore
unknowns" policy.

| Key | Parsed | Notes |
|-----|--------|-------|
| `version` | yes | Stored on the IR; not emitted into `tokens.json`. |
| `name` | yes | Required for detection. Stored on the IR. |
| `description` | yes | Block scalars (`|`, `>`) handled by yaml-cpp. |
| `colors.*` | yes | Each entry becomes a `color` token. |
| `typography.*` | yes | Composite `typography` tokens with `fontFamily`, `fontSize`, `fontWeight`, `lineHeight`, `fontFeature`, `fontVariation`. |
| `rounded.*` | yes | `dimension` tokens. |
| `spacing.*` | yes | `dimension` tokens. Non-dimensional values (e.g. `"5"`) are preserved as strings. |
| `components.*` | yes | Each component is a flat map of token references and literal style values. References are *not* resolved at parse time (see below). |
| Unknown top-level keys | passthrough | Stored on the IR, not emitted into `tokens.json`. The parser warns once per unknown key. |
| Unknown component properties | passthrough | Per the spec, unknown component props are preserved as opaque strings. |

The Markdown body after the closing `---` fence is parsed for headings
and code blocks but is not currently used to drive token emission.
Phase 2's `pulp design lint` will use the body for cross-references
between sections and prose.

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
| `4` | Unsupported feature — encountered a construct the parser explicitly rejects rather than silently dropping (reserved; nothing currently triggers it in Phase 1). |

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

## Phase 1 contract

Phase 1 ships exactly the surface above:

- `pulp import-design --from designmd` → `tokens.json`.
- Detection in `compat.json` with the strict fingerprint.
- Test fixtures (paws-and-paths from upstream, plus a hand-authored
  edge-case fixture and three decoys).
- This documentation page and the cross-references listed below.

Phase 2 will add `pulp design lint` (token coverage, broken refs,
unused tokens, naming conventions, contrast ratios, body/frontmatter
drift, unknown sections), `pulp design diff` (semantic diff between
two DESIGN.md files), and Tailwind v3 + v4 exporters.

Phase 3 will treat DESIGN.md as a project source of truth: `pulp
design` hydrates the live theme from `DESIGN.md`; `pulp design save
--update-design-md` round-trips changes back. Component scaffolding
from `components.*` into Pulp widgets lands in the same phase.

## Not yet supported

- Tailwind v3 / v4 export — Phase 2.
- Live runtime hydration of Pulp's theme from a DESIGN.md file —
  Phase 3.
- Widget scaffolding from `components.*` — Phase 3.
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
