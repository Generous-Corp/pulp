---
name: ci
description: Run CI validation, create PRs, and merge on green
---

Use the `ci` skill for all CI workflows. This is the primary gate for landing code on main.

## Ship (branch → PR → CI → merge)

Use Shipyard-managed PR flow:

```bash
shipyard pr
```

`shipyard pr` is the canonical path for PR creation, tracking, validation,
and merge-on-green. It runs the repo gates, pushes the branch, opens the PR,
records Shipyard state for the macOS GUI / `shipyard ship-state`, validates
macOS + Linux + Windows, and merges only after all required evidence is green.

Do not use `gh pr create` for normal work. Treat direct GitHub PR creation as
an explicit emergency/manual bypass only; if it is used, report the Shipyard
tracking gap and reconcile by resuming or re-shipping through Shipyard where
possible.

`pulp pr` defaults to this same Shipyard path. Humans can opt out locally with
`pulp config set pr.workflow github` or `manual`, but agents should only use
those workflows when the user explicitly asks for that bypass. `pulp status`
shows the effective workflow and whether its local tool is available.

## Just validate (no PR)

```bash
shipyard run
shipyard run --smoke
```

## Check status

```bash
shipyard ship-state list
```

## Cloud CI

```bash
shipyard cloud run build <branch>
```

See `.agents/skills/ci/SKILL.md` for the full workflow reference.
