# G-9 hostile hosted-plugin fixture

## Status: PARTIAL

This is process-fixture evidence on Darwin only. It does not load a hosted
plugin, implement process isolation, prove sibling continuity, or define
restart/state recovery. The roadmap's POSIX and Windows descendant-cleanup
scope remains open. **Containment verdict: HOLD / unproven.**

## Reproduce

```sh
python3 tools/validation/g9_hostile_fixture.py --output /tmp/g9-descendant-proof-final-20261103 --timeout 0.5
```

The executable fixture launches real crash, hang, deadline, descendant, clean
exit, and slow-within-budget subprocesses. It exits nonzero if any expected
observation fails. The descendant prints its PID before the parent is
terminated. The harness checks the parent's exit and the descendant's live
Darwin process state. An absent PID or any state containing `Z` is not counted
as live; a zombie is terminated even if its PID remains listed until reaped.
The harness then sends the descendant SIGTERM and verifies that its PID
disappears. This cleanup is fixture hygiene, **not** hosted-plugin
containment. The descendant's stdout/stderr are redirected to DEVNULL so an
inherited pipe cannot hold the parent's `communicate()` open.

## Darwin receipt

Run on 2026-11-03 with Python's `sys.platform=darwin` and a 0.5-second budget.
Receipt: `/tmp/g9-descendant-proof-final-20261103/receipt.json`, SHA-256
`d6839a3953fd3d3e0c8c1f2feda061163cdcd65b06fda0220ee9a9026b0`.
The receipt is local run output, not a committed artifact.

| Case | PID | Exit | Timeout | Elapsed | Observation |
|---|---:|---:|---|---:|---|
| crash | 58292 | -6 | no | 6.4 ms | crash detected |
| hang | 58294 | -15 | yes | 504.0 ms | timeout detected |
| deadline | 58606 | -15 | yes | 506.5 ms | deadline overrun detected |
| descendant parent | 58798 | -15 | yes | 504.7 ms | parent terminated |
| descendant child | 58804 | — | — | — | state `S` after parent exit; SIGTERM cleanup; state absent afterward |
| clean control | 58959 | 0 | no | 29.8 ms | classifier abstained |
| slow control | 58992 | 0 | no | 158.1 ms | classifier abstained within budget |

All eight receipt checks passed; `isolation_implemented=false`. The PID and
liveness observations establish that the spawned child survived this fixture's
parent termination. They do not establish a platform-neutral cleanup contract
or prove how a production host would handle arbitrary descendants.

On 2026-11-03, the zombie-state repair was checked with a fresh Darwin run:
`/tmp/g9-zombie-repair-20261103.28Bgd5/receipt.json`, SHA-256
`49ec1ef2380e5d9a461bb84c84eb2e10802ae4c52eb9b5d65ce1252bfeca3302`.
All eight checks passed. The descendant had live state `S` after its parent
exited, then disappeared after fixture cleanup; `isolation_implemented=false`.
The classifier regression passes for `S`, absent PID, and zombie states `Z`,
`Z+`, `SZ`, and `z`, and the receipt path rejects a mocked zombie after parent
exit. A planted version of the old any-state classifier fails those zombie
checks. The receipt is local run output, not a committed artifact.

## Remaining G-9 gates

- Run the hostile fixture on Linux and Windows with explicit N/A for
  unsupported process-tree observations. Windows descendant cleanup is a
  platform claim, so macOS evidence cannot substitute for it.
- If isolation is justified, separately design and test callback IPC allocation,
  restart/state semantics, and sibling continuity in real hosted-plugin flows.
