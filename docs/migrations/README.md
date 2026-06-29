# Migration Notes

This directory holds per-release migration notes that ship embedded in
the `pulp` CLI binary. `pulp upgrade --notes` filters this set against
the hop the user is performing (from the installed version to the
target) and prints only the entries that actually apply.

## File layout

One file per release that ships notes:

```
docs/migrations/
├── README.md          (this file — never emitted into the binary)
├── v0.24.0.md
├── v0.25.0.md
└── ...
```

Names are advisory — the tooling identifies each file by the `version`
field in its TOML frontmatter, not by the filename. Keep them in sync
anyway to make grep easy.

## Frontmatter schema

Every migration doc starts with a TOML frontmatter block:

```markdown
---
version = "0.27.0"
breaking = true
applies_if = "cli_version_from < 0.27.0 && cli_version_to >= 0.27.0"
summary = "CLI config file moved from ~/.pulp/config to ~/.pulp/config.toml"
---

## Breaking changes
- ...

## Migration steps
- ...
```

| Field        | Type    | Required | Notes |
|--------------|---------|----------|-------|
| `version`    | string  | yes      | Semver `"M.N.P"`, no leading `v`. |
| `breaking`   | bool    | no       | Defaults to `false`. Tags the note as a breaking change in CLI output and `--notes --json`, drives the top-level `has_breaking` / `breaking_count` agent signal, and triggers the proactive banner on a `pulp project bump` that crosses it. See "Agent consumption" below. |
| `applies_if` | string  | no       | Boolean expression over `cli_version_from` / `cli_version_to`. Empty / absent = always applies. |
| `summary`    | string  | no       | One-line human-readable summary. |

Arrays, inline tables, and multi-line strings are NOT supported. The
parser is intentionally minimal (see
`tools/scripts/build_migration_index.py`) so the schema stays easy to
read and diff-review.

## `applies_if` expression language

A tiny Boolean mini-language for filtering. Grammar:

```
expr    := or
or      := and ("||" and)*
and     := cmp ("&&" cmp)*
cmp     := ident op version | "(" expr ")"
ident   := "cli_version_from" | "cli_version_to"
op      := "<" | "<=" | ">" | ">=" | "==" | "!="
version := M "." N "." P    (optionally prefixed with 'v')
```

Typical usage for a breaking change landed in 0.27.0:

```toml
applies_if = "cli_version_from < 0.27.0 && cli_version_to >= 0.27.0"
```

Unknown syntax fails closed (the entry is treated as non-applicable).
Empty expression matches every hop — used for purely informational
notes that a maintainer wants printed on every upgrade that crosses
this version.

## Authoring workflow

1. On a release PR that adds a breaking change or user-visible
   behaviour shift, add `docs/migrations/vX.Y.Z.md` for the version
   the change lands in.
2. Fill in `version`, `breaking`, `applies_if`, and `summary`. Write
   a short Markdown body describing what changed and what to do
   about it.
3. Rebuild (`cmake --build build`). CMake re-runs
   `tools/scripts/build_migration_index.py` at configure time, so
   reconfigure (`cmake -S . -B build`) if the file list changes.
4. Verify locally: `./build/pulp upgrade --notes
   --from 0.26.0 --to 0.27.0`.
5. Commit. The generated `tools/cli/migration_index.cpp` lives at
   `${CMAKE_BINARY_DIR}/generated/migration_index.cpp` — it is NOT
   checked in.

## Agent consumption (inherit breaking changes before stumbling on them)

`pulp upgrade --notes --json` is the agent-facing view. Alongside the per-entry
`breaking` flag, the top-level object carries a one-glance signal so tooling can
branch without parsing every entry:

```json
{ "from": "0.26.0", "to": "0.30.0",
  "has_breaking": true, "breaking_count": 2,
  "entries": [ { "version": "...", "breaking": true, "summary": "...", "body": "..." } ] }
```

The intended flow, when a project's pinned SDK version moves from X to Y: run
`pulp upgrade --notes --json --from X --to Y` *first*, and if `has_breaking` is
true, read the breaking entries' bodies and adapt the code before building — so
the break is inherited up front, not discovered as a compile error later.

`pulp project bump` already prints the applicable notes after a pin change, and
when the hop crosses a breaking note it adds a short banner pointing at this
JSON. That banner is **on by default**; silence it with
`pulp config set upgrade.breaking_notes false` or `PULP_NO_BREAKING_NOTES=1`.
The JSON `has_breaking` / `breaking_count` fields are ALWAYS present regardless of
that setting — the setting only gates the human-facing banner, never the data.

Low-noise by design: only `breaking = true` notes drive the banner and the
`breaking_count`, so an ordinary feature release produces no agent prompt.

## Non-goals

- Not every PR deserves a migration note. Only write one when a
  reasonable pro developer needs to change code, config, or habits.
- Notes are advisory text only. The CLI never modifies user files
  based on them. Code changes require explicit user action.
- The agent signal is breaking-only on purpose; do not set `breaking = true`
  for non-breaking notes just to raise visibility.

Issue: #548 (parent #499).
