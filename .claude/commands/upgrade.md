---
name: upgrade
description: "Discover new Pulp CLI releases, review applicable migration notes for the version hop, and run `pulp upgrade` with user consent. Shells out to `pulp doctor --versions --json` and `pulp upgrade --notes --json` — no hardcoded paths."
allowed-tools:
  - Bash
  - Read
  - AskUserQuestion
---

Interactive upgrade assistant for the Pulp CLI. Reuses the `upgrade`
skill (`.agents/skills/upgrade/SKILL.md`) for authoritative guidance;
this file is the Claude Code surface that wires the pieces together.

## 1. Confirm `pulp` is on PATH

```bash
command -v pulp >/dev/null 2>&1 || {
  echo "pulp CLI not found on PATH."
  echo "Install with: curl -fsSL https://raw.githubusercontent.com/danielraffel/pulp/main/install.sh | sh"
  exit 1
}
```

If `pulp` is missing, stop here — the skill cannot operate without the
installed CLI. The design decision locked in on 2026-04-21 explicitly
rules out hardcoded plugin paths.

## 2. Pull version diagnostics (structured)

```bash
pulp doctor --versions --json > /tmp/pulp-doctor-versions.json 2>/dev/null
```

Parse the JSON to extract:

- `cli.raw`                 — installed CLI semver (e.g. `"0.30.0"`)
- `plugin.raw`              — Claude plugin semver from `plugin_json_path`
- `plugin_json_path`        — absolute path to the plugin's `plugin.json`
- `project_sdk.raw`         — active project's SDK (if inside a Pulp project)
- `project_cli_min.raw`     — the active project's `cli_min_version` pin (if any)
- `findings[]`              — advisory skew messages

Derive the **latest available CLI** from the cache without hitting the
network twice, and CAPTURE the value into a shell variable so step 3
can reuse it:

```bash
LATEST="$(pulp upgrade --check-only 2>/dev/null | awk -F'v' '/^Latest:/ {print $2}' | tr -d '[:space:]')"
```

If the cache is empty, `--check-only` will query GitHub. Note:
`--check-only` does NOT persist its fetched result into the update
cache (see `tools/cli/cmd_upgrade.cpp`), so you MUST capture `$LATEST`
from its stdout and forward it to step 3 explicitly. Relying on the
`--notes` default cache lookup would resolve `to == from` on first-run
/ empty-cache machines and silently hide real migration hops.

## 3. Pull applicable migration notes

Pass `--to "$LATEST"` (captured in step 2) explicitly so the notes
surface sees the real hop even when the background cache refresh has
not yet landed (#583 Codex P1 / wave-4 sweep):

```bash
if [ -n "$LATEST" ]; then
  pulp upgrade --notes --json --to "$LATEST" > /tmp/pulp-upgrade-notes.json 2>/dev/null
else
  pulp upgrade --notes --json > /tmp/pulp-upgrade-notes.json 2>/dev/null
fi
```

Defaults when no `--to` is supplied: `from = installed CLI`,
`to = cached latest` (falls back to `from` on empty cache — which is
why step 2 captures `$LATEST` and this step forwards it). Override if
the user asked about a specific hop:

```bash
pulp upgrade --notes --json --from 0.27.0 --to 0.30.0
```

Expected JSON shape (stable surface — do NOT rename keys, they are
guaranteed by `test/test_cli_shellout.cpp [issue-548]`):

```json
{
  "from": "0.27.0",
  "to":   "0.30.0",
  "entries": [
    {
      "version":   "0.28.0",
      "breaking":  false,
      "summary":   "...",
      "applies_if":"cli_version_from < 0.28.0 && cli_version_to >= 0.28.0",
      "body":      "..."
    }
  ]
}
```

Entries whose `applies_if` didn't match the hop are filtered out by
the CLI; you only see what's relevant.

## 4. Present a compact version table

Render in the chat:

```
Pulp version status
-------------------
  CLI binary:   v<cli>  ->  v<latest>   (<upgrade-hop-or-"current">)
  Claude plugin: v<plugin>
  Project SDK:   v<project_sdk>   (at <project_root>)
  [skew]        <finding.message>    -- one line per advisory finding
```

Then the migration notes section. For each entry in `entries[]`:

```
-- v<version><BREAKING? " [breaking]">
   <summary>

<body>
```

If `entries[]` is empty, say so plainly: "No migration notes apply to
this upgrade." Do not manufacture reassurance beyond what the CLI told you.

## 5. Offer an action menu via AskUserQuestion

Use AskUserQuestion with the relevant options below. Prefer the first
three when an active project is present; offer the registry-wide option
only when the user asked for all projects.

- **"Upgrade CLI + bump this project SDK"** — runs `pulp upgrade`,
  then proposes `pulp project bump` in the active project resolved by
  `doctor --versions`. This changes the project's pinned Pulp SDK,
  not the app/plugin product version.
- **"Upgrade CLI only"** — runs `pulp upgrade` only. No project changes.
- **"Bump this project SDK only"** — skips `pulp upgrade`; proposes
  `pulp project bump` inside the active project. Useful when the CLI is
  already current but `project_sdk.raw` is behind.
- **"Bump all registered project SDKs"** — only offer this when the
  user explicitly wants registry-wide changes; propose
  `pulp project bump --all --dry-run` first, then a real
  `pulp project bump --all` after review.
- **"Dismiss"** — no action. Remind the user they can re-run `/upgrade`
  or `pulp doctor --versions` at any time.

## 6. Run the chosen action

For "Upgrade CLI" paths:

```bash
pulp upgrade
```

This downloads the tarball, swaps `~/.pulp/bin/pulp` in place, and
prints the `pulp upgrade --notes --from <old> --to <new>` hint. On
error, the CLI restores the pre-upgrade binary from `<path>.bak` — if
the swap step failed partway, tell the user to check for a stale
`.bak` file.

For "Bump project SDK" paths, do not execute the bump until the user
has seen the target project and target SDK version. Start with a dry
run unless the user already gave explicit permission:

```bash
pulp project bump --dry-run
pulp project bump
```

If the user wants the global CLI and the active project SDK moved in
one flow, run `pulp upgrade` first, then re-run `pulp doctor --versions`
and propose `pulp project bump --to <post-upgrade-cli-version>`. If
they are inside the Pulp source checkout, do not run `pulp project
bump`; that checkout uses the release/version workflow.

## 7. After-upgrade follow-up

Once `pulp upgrade` returns 0, re-read migration notes with explicit
`--from` / `--to` so the user sees the exact notes for the hop that
just happened:

```bash
pulp upgrade --notes --from <pre-upgrade-cli> --to <post-upgrade-cli>
```

If any of those notes are breaking and mention a symbol grep pattern,
offer to grep the user's project for it as the next turn. Don't
execute the grep eagerly — wait for explicit consent.

## References

- Skill: `.agents/skills/upgrade/SKILL.md` (full decision tree, common patterns, bypass trailers)
- CLI: `tools/cli/cmd_upgrade.cpp` (`--notes --json` output)
- Design: `planning/release-discovery-ux-design-2026-04-20.md` Section C + Slice 4
- Issue: #549 (parent #499)
