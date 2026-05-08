---
name: upgrade
description: |
  Guide users through `pulp upgrade` — discover new CLI releases, interpret
  migration notes for the hop they're performing, and apply breaking-change
  fixes (CMake macro renames, API surface changes, config file moves).
  Handles "upgrade pulp", "what's new", "migrate my project", "show
  breaking changes", and the `/upgrade` slash command. Pairs with the
  embedded migration index shipped in the CLI binary (release-discovery
  Slice 3, #571).
requires:
  tools:
    - pulp
---

# upgrade

## When this skill applies

- User asks to upgrade, update, or bump the Pulp CLI.
- User runs `/upgrade` in Claude Code.
- User asks "what changed", "what's new", "what breaks", or "how do I
  migrate" after bumping an SDK / CLI version.
- A `pulp doctor --versions` run surfaces CLI-vs-SDK skew the user wants
  to resolve.

This skill does NOT cover:

- Bumping the **project's** pinned SDK version — that's the
  `cli-maintenance` skill + `pulp project bump`.
- Bumping the Pulp framework/source checkout version — that's
  `pulp version bump` plus the release workflow.
- Shipping a PR from a dev branch — that's the `ci` skill via `pulp pr`.
- Updating the Shipyard pin — that's the Dependency Update Workflow in
  `CLAUDE.md` (runs through `tools/deps/audit.py`).

## Mental model

Four independently-versioned surfaces, two of which this skill acts on
directly and one of which it may hand off to `pulp project bump`:

| Surface | Source of truth | How to upgrade |
|---------|----------------|----------------|
| Pulp CLI / SDK | `~/.pulp/bin/pulp` (installed binary) | `pulp upgrade` |
| Consumer project SDK pin | project `pulp.toml` `sdk_version` + `find_package(Pulp X.Y.Z ...)` | `pulp project bump` |
| Pulp source checkout version | Pulp repo `CMakeLists.txt` / release metadata | `pulp version bump` + release workflow |
| Claude plugin | `.claude-plugin/plugin.json` | `/plugin install pulp` in Claude Code |
| Shipyard pin | `tools/install-shipyard.sh` | Dependency Update Workflow (out of scope) |

`pulp upgrade` downloads the new CLI binary and replaces the installed
one. After the Phase 8 Rust cutover, release archives are dual-binary:
Rust `pulp` is the user-facing CLI and sibling `pulp-cpp` is installed
as the C++ fallthrough delegate. A healthy install has both
`~/.pulp/bin/pulp` and `~/.pulp/bin/pulp-cpp`; use `PULP_USE_CPP=1
pulp <args>` or direct `pulp-cpp <args>` only for rollback/debug
comparisons. Do not hand-swap binaries in user/system locations.
The C++ `cmd_upgrade.cpp` path still matters for pre-cutover users
upgrading into a Rust release: it must copy the archive's sibling
payloads, including `pulp-cpp`, before replacing the running `pulp`
binary (#1673).

`pulp upgrade --notes` prints migration notes for the hop without
downloading anything. `pulp upgrade --notes --json` emits the same data
as a stable-shape JSON document for agent consumption (the `/upgrade`
slash command is the primary consumer).

Use `pulp project bump` only after deciding a consumer project should
move its SDK pin. It operates on the active project or the registry
(`--all`); it does not upgrade the global CLI and it refuses to treat
the Pulp source checkout as a consumer project.

## Discovery workflow (`pulp` on PATH is assumed)

The skill shells out to `pulp` — no hardcoded paths, no env-var
requirement. If `pulp` is not on PATH, tell the user to install first
(`curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh`)
and stop.

### Plugin ↔ CLI skew banner (Slice 6, #551)

Before running any `pulp` command, source the shared skew-check helper
so the user sees a single-line hint when the installed CLI is older
than the plugin's declared `min_cli_version`:

```bash
source "$(git rev-parse --show-toplevel 2>/dev/null || echo .)/tools/scripts/cli_version_check.sh"
pulp_cli_version_check   # no-ops silently if already checked this session
```

Behaviour:

- Banner (stderr, at most once per session):
  `[pulp] Claude plugin requires CLI >= v<MIN> but installed CLI is v<HAVE>. Run \`pulp upgrade\` or \`/upgrade\` in Claude Code.`
- Silent when CLI ≥ min, when the plugin manifest omits
  `min_cli_version` (older plugin builds), or when either version is
  non-numeric (dev builds).
- Overrides: `PULP_SKEW_CHECK_DISABLE=1` turns it off;
  `PULP_SKEW_CHECK_CACHE` overrides the session-marker directory.

The same skew logic is surfaced by `pulp doctor --versions` inline, so
users who prefer running the diagnostic directly see the finding there
too — the helper is a convenience for skill authors, not a second
source of truth.

1. **Identify the active plugin directory** (so the skill can resolve
   `docs/migrations/` for full-body lookups):

   ```bash
   pulp doctor --versions --json
   ```

   The JSON output contains `plugin_json_path` — walk up two levels
   (`dirname $(dirname $plugin_json_path)`) to reach the plugin root.

2. **Report the version skew at a glance** so the user sees where they are
   before deciding what to do. The JSON response also includes `cli`,
   `plugin`, `project_sdk`, and `findings[]` — surface those in a
   compact table.

3. **Fetch applicable migration notes** for the current hop.

   **Important:** user-invoked `pulp upgrade` and `pulp upgrade
   --install` force a synchronous release refresh instead of trusting
   the 24h cache. This is deliberate: v0.78.3 showed that a cache
   written minutes before a release can make a just-published version
   invisible. `pulp upgrade --check-only` remains the lightweight
   cache-aware probe; if you need release-fresh notes, capture the
   `Latest:` value from a refreshed/default `pulp upgrade` run and
   forward it as `--to "$LATEST"`.

   In CI / sandbox lanes, `PULP_UPDATE_CHECK_DISABLED=1` makes
   `--check-only` network-free. If the cache is empty in that mode, the
   command reports the installed CLI version plus an explicit
   disabled/not-queried latest line instead of querying GitHub Releases.

   ```bash
   pulp upgrade --notes --json                  # defaults: from = installed CLI, to = cached latest
   pulp upgrade --notes --json --to "$LATEST"   # recommended — use value captured from refreshed upgrade check
   pulp upgrade --notes --json --from X --to Y  # explicit hop
   ```

## Release verification gotcha

When verifying a newly published release, test from the previously
published CLI with a deliberately fresh cache that still names the old
release:

```bash
PULP_HOME="$(mktemp -d)" pulp upgrade --check-only --json
# seed/update-cache.json to latest_version=<previous> with a current timestamp
pulp upgrade
```

The default `pulp upgrade` path must refresh through GitHub and report
the new tag immediately. Then run `pulp upgrade --install --to X.Y.Z`
in an isolated install directory and confirm a follow-up `pulp upgrade`
reports both `Installed` and `Latest` as the new version. This catches
the stale-cache failure from #1685 before the release is announced.

   Stable-shape output (do NOT rename these keys — they are a public
   surface the skill depends on, see `tools/cli/migration_index.hpp`):

   ```json
   {
     "from": "0.27.0",
     "to":   "0.30.0",
     "entries": [
       {
         "version":   "0.28.0",
         "breaking":  false,
         "summary":   "…",
         "applies_if":"cli_version_from < 0.28.0 && cli_version_to >= 0.28.0",
         "body":      "…"
       }
     ]
   }
   ```

4. **Render the notes inline in the chat**. For each entry:
   - If `breaking == true`, prefix with a loud tag (e.g. "BREAKING").
   - Print the `summary` on the first line.
   - Include the `body` verbatim — it's already Markdown.

## Interpreting migration notes

Each note describes one release's visible behaviour shift. Read them
like a pro-developer changelog, not a marketing blurb.

### Breaking vs non-breaking

- `breaking: true` — requires user action: code change, config edit,
  habit change. Do not skip. Offer to grep the project for the affected
  symbols as a follow-up.
- `breaking: false` — informational. The upgrade works without
  intervention, but something worth knowing has changed.

### `applies_if` expressions

The filter runs in the CLI — by the time the skill receives JSON,
non-applicable entries are already stripped. You'll only see entries
whose `applies_if` matched the hop. Still: print the expression alongside
the note so a curious user can verify why it applied.

Grammar: Boolean combinations (`&&`, `||`, parentheses) over comparisons
of `cli_version_from` / `cli_version_to` against a literal semver. Six
operators: `<`, `<=`, `>`, `>=`, `==`, `!=`. See
`docs/migrations/README.md` for full details.

## Common breaking-change patterns

These are the patterns that have shown up in Pulp migration notes. When
you spot one, suggest the exact grep / replacement to the user instead
of leaving them to figure it out from a prose paragraph.

### CMake macro renames

**Pattern.** A `pulp_add_*` function in `tools/cmake/Pulp*.cmake` is
renamed, folded into `pulp_add_plugin(FORMATS ...)`, or split out of
one macro into several.

**What to do.**

```bash
# Find every call site.
grep -rn "pulp_add_ios_auv3\|pulp_add_auv3\|pulp_add_vst3_only" .
```

Propose the replacement form explicitly; do NOT rewrite the user's
CMakeLists.txt without confirmation. For unified-form migrations, show
a diff like:

```diff
- pulp_add_ios_auv3(MyPlugin …)
+ pulp_add_plugin(MyPlugin FORMATS AUv3 …)
```

### API surface changes

**Pattern.** A public C++ header under `core/*/include/pulp/**` adds,
removes, or renames a symbol that processors / view code use.

**What to do.**

```bash
# Find call sites of the old symbol.
grep -rn "OldProcessorAPI\|deprecated_function_name" .
```

Read the migration body for the replacement signature. If the change is
a deprecation (old API still works, emits a warning), flag that —
teams may want to migrate at their own pace.

### Config file / path moves

**Pattern.** A file Pulp reads (e.g. `~/.pulp/config.toml`,
`pulp.toml`, `.pulp/projects.json`) changes location or key shape.

**What to do.** Most config moves are handled by the CLI on first run
(it migrates the old location to the new one and prints a one-line
notice). The note tells you whether manual action is needed. If it is,
quote the exact `pulp config set` / `pulp config unset` commands from
the body.

### CLI flag / subcommand changes

**Pattern.** A subcommand is renamed (e.g. `pulp check` → `pulp doctor`),
a flag changes shape (`--sign-key` → `--ed25519-key`), or an exit code
semantic changes (silent-success → loud-failure as in #295 / #560).

**What to do.** If the user has CI scripts (`ci/`, `.github/workflows`,
`tools/local-ci/`, team-specific shell scripts), grep those for the
old invocation first — those are the scripts that'll silently break.

```bash
grep -rn "pulp <old-command>\|--<old-flag>" ci/ .github/ tools/ || true
```

## Bypass trailers

Slice 4 itself doesn't add new bypass trailers — it consumes Slice 3's.
But users will ask about trailers around upgrade-triggered PRs, so keep
the syntax handy (full table in `CLAUDE.md` → "Versioning & Skill-Sync
Policy"):

| Gate | Trailer (tip commit only, NEVER PR body) |
|------|-------------------------------------------|
| Version bump | `Version-Bump: <surface>=<patch\|minor\|major\|skip> reason="..."` |
| Skill update | `Skill-Update: skip skill=<name> reason="..."` |
| Auto-release | `Release: skip reason="..."` |

Trailer blocks must be **contiguous** — no blank lines between
trailers, or `git interpret-trailers --parse` stops reading. When
amending, rewrite the whole block, don't append.

## Decision tree (used by the `/upgrade` slash command)

```
pulp doctor --versions --json   → parse cli / plugin / project_sdk / findings
pulp upgrade --notes --json     → parse entries[]

present table:  CLI   vX.Y.Z → vA.B.C    (<hop>)
                plugin vP.Q.R → vP'.Q'.R' (if drift)
                project SDK vS.T.U  (skew warn from findings)

list applicable notes (inline, breaking flagged)

ask via AskUserQuestion:
  - "Upgrade CLI + bump project SDK" → `pulp upgrade` + `pulp project bump` in the active project
  - "Upgrade CLI only"            → `pulp upgrade`
  - "Bump project SDK only"       → `pulp project bump` in the active project or `--all` when explicitly requested
  - "Dismiss"                     → no action, remind user they can re-run `/upgrade`
```

## Testing the slash command

The `/upgrade` command shells out to `pulp upgrade --notes --json`.
A regression test lives in `test/test_cli_shellout.cpp` (the
`[issue-549]` tag) — it asserts that a synthetic hop produces the keys
the slash command depends on. If you add or rename a key in
`render_notes_json` (`tools/cli/migration_runtime.cpp`), update this
skill AND the slash command AND the test in the same PR.

## References

- Design: `planning/release-discovery-ux-design-2026-04-20.md` Section C + Slice 4
- CLI surface: `tools/cli/cmd_upgrade.cpp`, `tools/cli/migration_index.hpp`
- Migration doc schema: `docs/migrations/README.md`
- Slice 1 (diagnostics): #546 (`pulp doctor --versions`)
- Slice 2 (update check): #562 (`pulp upgrade --check-only`)
- Slice 3 (migration index): #571 (`pulp upgrade --notes --json`)
- Slice 4 (this skill): #549
- Slice 6 (plugin ↔ CLI skew): #551 (`min_cli_version` +
  `tools/scripts/cli_version_check.sh` + `plugin_min_cli` JSON field)
