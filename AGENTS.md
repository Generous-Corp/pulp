# AGENTS.md

Read `CLAUDE.md` before making any changes.

Before changing files, run `tools/scripts/worktree_lineage.sh show --path .`.
If the worktree is superseded, merged, or archived, follow its recorded
successor/disposition instead of resuming it. If no status is recorded,
classify the worktree before editing.

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

## Vellum source-authority routing

`.github/vellum-ownership.json` is the authoritative source-routing projection.
For slices whose state is `framework-authoritative-transferred`, new generic
framework/rendering/UI work originates in `Generous-Corp/vellum`; Pulp changes
are limited to explicitly Pulp-owned integration, adaptation, or compatibility
work and must carry the required immutable change-event declaration.

**Temporary inheritance exception (owner decision 2026-10-04).** Until a target
flips, generic design-import, UI compiler, bridge-contract, SDK, and supporting
UI/render work may still land in Pulp, including on transferred-slice files.
Such a change declares a `pulp-only` change event whose rationale starts with
the fixed prefix in `docs/contracts/vellum-extraction-freeze.md`, and records an
entry file `docs/contracts/vellum-inheritance-ledger/<event_id>.json`. The exception ends per
target, in the same Pulp change that switches that target to an exact installed
Vellum package and freezes the Pulp copy; from then on the rule above applies to
that target without exception. The exception transfers no authority and does
not authorize Pulp to consume Vellum.

Do not widen, narrow, or reverse an active slice in a routine Pulp change. Any
such boundary change requires the separately reviewed cross-repository
protocol and evidence plan in
[`danielraffel/pulp-planning@d7cd07b8f9a3fff0061bf3c1e84c3e9f719b3ea7`](https://github.com/danielraffel/pulp-planning/blob/d7cd07b8f9a3fff0061bf3c1e84c3e9f719b3ea7/2026-08-08-vellum-catch-up-convergence-and-adoption-plan.md).
If that artifact is unavailable, stop and request the plan rather than
inferring a boundary change.
