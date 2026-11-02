# G-9 hostile hosted-plugin fixture

## Scope

Evidence only. This fixture models a hosted plugin that crashes, hangs, misses a
deadline, and leaves a descendant process. It intentionally does **not** add or
claim process isolation.

## Run

```sh
python3 tools/validation/g9_hostile_fixture.py --output /tmp/g9-hostile-evidence
```

The receipt records each case, return code, timeout observation, and two planted
negative controls (clean exit and slow-but-within-budget). The descendant case is planned for process-tree inspection, but this fixture does not
capture a child PID or prove survival after parent termination. The evidence is
therefore unverified.

## Verdict

**HOLD / containment unproven.** Positive observations cover crash, hang, and
deadline timeout. Descendant survival and planted-negative controls are
metadata-only and remain unverified. No process isolation, descendant
reaping, or crash containment is implemented by this change. Platform scope is
the invoking host (`sys.platform`); no cross-platform claim is made.

## Delivery state

Branch: `codex/g9-hostile-20261102` (base `origin/main`, head at authoring time
`0f616a440ce`). The fixture ran successfully on Darwin with focused syntax
validation; its JSON receipt was written to the run output directory. The
worktree is marked active. This report and the fixture are the only intended
committed artifacts; no isolation implementation is included.
PR: https://github.com/Generous-Corp/pulp/pull/11429 (open, unarmed; exact head is reported by GitHub).
