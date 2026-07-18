---
name: pr-batching
description: Decide whether several finished branches ship as ONE PR or stay separate. Use when holding 2+ green branches before `shipyard pr`. Trades CI cost against revert granularity, urgency, and the diff-coverage gate.
---

# PR batching

**A PR costs ~an hour before it can merge:** a ~25-min local diff-coverage build,
then a Shipyard validation. Validations must run **serially** (concurrent runs
from worktrees sharing one `.git` race on `config.lock`). So *n* related PRs is
*n* **sequential** hours, not *n* parallel ones.

That makes "ship these together?" a real question. This is the checklist.

## Default: combine

If you hold 2+ finished, green, **related** branches — combine. Separate PRs are
what you justify, not the reverse.

## Combine when

- **They share unmerged work.** Check first — you may already have the combined
  branch:
  ```bash
  git merge-base --is-ancestor feature/a feature/b && echo "b already contains a"
  # siblings: is their common point AHEAD of origin/main?
  git rev-list --count origin/main..$(git merge-base feature/a feature/b)
  ```
  A chain *is* the combined PR — open it from the tip. Siblings fold with a
  cherry-pick.
- **One depends on the other.** If B only compiles because of A, they were always
  one change.
- **Same subsystem.** One context, one adoption note, one revert.
- **A well-tested branch would lift a weak one over the coverage gate.** Coverage
  is computed over the **whole PR diff**, so folding in a densely-tested branch
  *raises* the ratio. A branch stuck at 68% can clear 75% by merging with a
  sibling that ships 20 test cases. This is adding covered lines, not hiding
  uncovered ones — legitimate.
- **Fewer chances to re-conflict the version-bump line** on a busy `main`.

## Keep separate when — any ONE of these

- **One is urgent, the other is gated.** Never make a ready branch hostage to a
  blocked one. An hour of CI is cheaper than a day of someone else's blocked work.
- **One is unfinished** — a library with no caller, an unwired command,
  uncommitted files. **Never ship half-done work to save a CI run.** This is the
  trade that looks tempting and is always wrong.
- **Unrelated subsystems.** Costs revert granularity, muddies the migration note.
- **One is risky enough it might need reverting** — isolate it, or a revert drags
  good work out with it.
- **One carries a large untestable surface** (native window hosts, format adapters
  needing a real DAW). It drags combined coverage *down* — the inverse of the lift
  above. Check which way it cuts.

## Folding siblings

```bash
cd <worktree-of-primary-branch>
git log --oneline <shared-base>..feature/sibling   # confirm its OWN commits
git cherry-pick <sha>...
tools/ci/governed-build.sh cmake --build build
ctest --test-dir build --output-on-failure -R '<affected>'
```

Cherry-picks compile and still break behaviour — a change fine against the old
base can violate an invariant the other branch just introduced. Run the suites
**together** before shipping.

## Don't

- **Don't run two heavy builds at once to "save time."** They contend, and the
  contention surfaces as *test failures* (shellout tests time out under load),
  costing you an investigation into a bug that does not exist.
- **Don't batch to dodge a gate.** Adding covered lines to raise a ratio is fine.
  A `PULP_SKIP_*` bypass is not.

## Enforcement

This skill is the *judgment*. `tools/scripts/pr_batch_advisor.py` runs from
`.githooks/pre-push` (advisory, never blocks) so the question gets asked no matter
who is driving — Claude, Codex, a human, `gh pr create`, or `shipyard pr`. A skill
only reaches an agent that reads it; a push reaches everyone.

Silence one push: `PULP_SKIP_PR_BATCH_ADVICE=1`.
