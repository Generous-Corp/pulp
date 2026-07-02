---
name: pr-review-sweep
description: >-
  Sweep a PR's automated + human review comments and act on them — especially
  for material (large / logic-bearing) PRs. Handles "sweep the PR comments",
  "check PR review feedback", "address review comments", "did the bots flag
  anything", and the pre-/post-merge review pass on any non-trivial PR.
requires:
  tools:
    - gh
    - ghapp
---

# PR Review Sweep

Automated reviewers (cubic-dev-ai, chatgpt-codex-connector / Codex) and humans
leave findings on PRs **asynchronously** — often seconds to minutes after the PR
opens, and sometimes only after it merges. A PR that built green is not a
reviewed PR. This skill makes the sweep a deliberate step so real P1s (races,
state-clobbers, unguarded destructive paths) don't slip through on the exact
kind of change where they hurt most.

The rule that motivated this skill: a self-identified P1 on a destructive path
was shipped as a "follow-up" and the reviewers confirmed it (plus three more)
*after* merge. Cheap to catch at sweep time; a roundtrip to catch later.

## When to sweep (and when not to)

**Always sweep — material PRs:**
- Touches shipped source / logic: `core/**`, `examples/**`, `ship/**`,
  `tools/cli/**`, `src/**` (Shipyard), CMake/build wiring.
- Concurrency, lock ordering, state machines, serialization, RT-audio, or any
  **destructive / mutating** path (delete, abandon, overwrite, force). Treat
  these as high-risk regardless of diff size.
- Large diffs, or a stack of commits — more surface, more asynchronous review.

**Skip is fine — non-material PRs:**
- Docs-only / comment-only / pure rename / formatting.
- Trivial config or version-bump-only changes.

If unsure, sweep. It costs one API read and a few minutes of triage.

## The sweep

Use `ghapp` for GitHub API **reads** (its own rate-limit bucket; plain `gh`
burns the shared personal token). Use `gh` for creating/merging PRs.

Pull all three comment surfaces — findings land in different places:

```bash
REPO=danielraffel/<repo>; N=<pr>
# Inline review comments (code-anchored — where the bots put P1/P2 findings)
ghapp api repos/$REPO/pulls/$N/comments \
  --jq '.[] | "--- \(.path):\(.line // .original_line) by \(.user.login)\n\(.body)\n"'
# PR-level reviews (approve/request-changes summaries)
ghapp api repos/$REPO/pulls/$N/reviews \
  --jq '.[] | "\(.user.login) [\(.state)]: \(.body)"'
# Issue comments (human notes, "potential issues flagged here")
ghapp api repos/$REPO/issues/$N/comments \
  --jq '.[] | "--- by \(.user.login)\n\(.body)\n"'
```

Bot findings are usually tagged `P1`/`P2` with a confidence and an "AI agents"
prompt block. They are signal, not gospel.

## Triage each finding

1. **Verify against the actual code**, not the bot's summary. Read the file at
   the anchored line. Confirm the failure scenario is reachable (concrete
   inputs → wrong output). Don't dismiss a P1 because it's "narrow" — a narrow
   race on a destructive path is still a must-fix.
2. **Bucket:** must-fix (correctness / data-loss / a live thing broken),
   should-fix (real but bounded), nit (style / naming). Default-off opt-ins
   still get their P1s fixed **before anyone can enable them**.
3. **Don't blindly apply** the bot's suggested patch — reproduce the reasoning,
   then write the fix that fits the code. Bots are right about *what*, often
   wrong about *how*.

## Act

- **PR still open:** push the fixes to the branch; re-run the local gates; let
  the same reviewers re-review before merge.
- **Already merged** (common on Shipyard — see below): ship a **follow-up PR**
  that fixes every confirmed finding, with a test per fix (tests ship with
  fixes), then post a closeout comment on the original PR mapping each
  finding → fix.
- **Ship the test.** Every correctness fix lands with the Catch2 / `cargo test`
  case that would have caught it. "It compiles" is not the acceptance criterion.

## Timing traps

- **Reviews are asynchronous.** Opening the PR does not mean the review has run.
  For a material PR, either wait for the review run before merging, or commit to
  a **post-merge sweep** and be ready to follow up.
- **Shipyard has no required status checks** → `gh pr merge --auto` fires the
  moment the PR is mergeable, which is typically *before* the reviewers finish.
  So on Shipyard, the post-merge sweep is not optional — always re-read
  `pulls/$N/comments` after the merge lands and follow up if anything is P1/P2.
- **Pulp PRs** route through `shipyard pr` and merge on green; Codex review
  comments can also arrive post-merge. Same discipline: sweep after, follow up.

## Cross-repo

This applies to any repo the workspace ships to — Pulp and Shipyard both get
automated bot review. This skill lives in Pulp's `.agents/skills/` (the shared
source of truth); the practice is identical when working in the Shipyard tree.
