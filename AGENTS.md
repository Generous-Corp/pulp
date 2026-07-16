# AGENTS.md

Read `CLAUDE.md` before making any changes.

## Decisions contract (read before touching fleet/CI config)

Settled build-system, CI, and release-automation decisions are recorded in
[`.agents/contract.toml`](.agents/contract.toml) — a schema-versioned, layered
(generic `default` layer + `pulp` overlay), agent-neutral file. Each decision
was bought with an incident; several were re-proposed by past agents and cost a
planning cycle each. Before proposing or writing a change to CI/fleet config
(the merge queue, Namespace/paid runners, auto-rebase or `--adopt-head`,
bump-at-merge, ccache depend-mode, squash auto-merge, static runner names, the
Debug macOS lane, warm build dirs), read the relevant rows:

```bash
python3 tools/scripts/decisions_contract.py --mode list                          # all rows
python3 tools/scripts/decisions_contract.py --mode surface --base origin/main    # rows your diff touches
python3 tools/scripts/decisions_contract.py --mode validate                      # schema-check the file
```

Reversing a decision requires first proving its motivating incident class can no
longer occur (Step Zero). The read surface and the SessionStart/PostToolUse
hooks (`hooks/scripts/decisions-contract-*.sh`, wired for both Codex via
`.codex/hooks.json` and Claude via the plugin) are advisory context only — the
authoritative block is this CLI's `validate` gate plus CI required checks.
