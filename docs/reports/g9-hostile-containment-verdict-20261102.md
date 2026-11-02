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
negative controls (clean exit and slow-but-within-budget). The descendant case
is marked for process-tree inspection; this fixture does not kill or sandbox the
child, so any surviving descendant is containment evidence rather than a fix.

## Verdict

**HOLD / containment unproven.** Positive observations cover crash, hang,
deadline timeout, and descendant creation. The negative controls demonstrate the
fixture can abstain on non-hostile behavior. No process isolation, descendant
reaping, or crash containment is implemented by this change. Platform scope is
the invoking host (`sys.platform`); no cross-platform claim is made.

## Delivery state

Branch: `codex/g9-hostile-20261102` (base `origin/main`, head at authoring time
`0f616a440ce`). The fixture ran successfully on Darwin with focused syntax
validation; its JSON receipt was written to the run output directory. The
worktree is marked active. This report and the fixture are the only intended
committed artifacts; no isolation implementation is included.
PR: https://github.com/Generous-Corp/pulp/pull/11429 (open, unarmed; exact head is reported by GitHub).
