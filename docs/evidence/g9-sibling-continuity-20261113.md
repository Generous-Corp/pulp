# G-9 sibling continuity under worker death — 2026-11-13

Status: **PASS for the stated scope (functional, macOS arm64, `MacClapStereo64V1`)**.
This does not lift the G-9 HOLD on full realtime qualification, host death,
Linux or Windows process-tree parity.

Bound to `origin/main` `d486ff70c6b02c59acdeac504af747724d1fb906`. The change
is test-only: the existing runtime already met the contract, so no production
source changed.

## Contract under test

A `SignalGraph` holds an isolated CLAP worker node (`load_isolated_clap`, the
healthy half-gain fixture) and an in-process built-in gain sibling on a
parallel path from the same input. Both feed separate output channels. The
worker process is killed with `SIGKILL` between callbacks while the graph is
rendering. The test then checks three things:

1. **Sibling continuity.** All 16 blocks of the sibling output match a
   fault-free reference render bit for bit (a second graph of the same shape
   with a healthy worker and the same input), before and after the kill.
2. **Documented status.** `IsolationStatus::reason` is
   `IsolationReason::worker_exited`, lifecycle is `terminal`, and the reason
   still reads `worker_exited` after `graph.release()`, because the first
   terminal reason never clears. The isolated output matches the reference
   until the kill and is exact silence afterwards. It does not hold or repeat
   the last completion.
3. **Callback bound.** Every graph callback from the kill onward completes
   inside one `MacClapStereo64V1` block period (64 frames at 48 kHz, 1333 µs).
   The callbacks run under `ScopedRtProcessProbe` in the trap build, which
   aborts on allocation, a mutex lock, `poll`, `send`/`recv` or `kill` inside
   the callback.

To make the reason deterministic, the test waits up to 2 s for the supervisor
to publish the worker exit before the next callback. Without that wait, the
callback endpoint can win the first-reason race and latch `deadline_missed`
for the missing completion. That outcome is also documented terminal silence,
but it is a different reason.

Test: `Isolated CLAP killed worker keeps in-process sibling bit-exact` in
`test/test_isolated_clap_worker.cpp` (`pulp-test-isolated-clap-worker`).

## Results

Measured on m3 at a load average of about 30 (shared fleet host), in a Release build:

| Run | Result |
|-----|--------|
| 20 consecutive runs of the test case | 20 passed, 0 failed |
| Worst post-kill callback, five sampled runs | 7.6–16.9 µs (budget 1333 µs) |

## Negative controls (`tools/scripts/confirm_failure.sh`)

Both mutations were applied to `core/host/src/isolated_plugin_slot.cpp`. For each,
the script observed the recompile at every step (baseline, broken, restored):

| Mutation | Verdict | Failing assertion |
|----------|---------|-------------------|
| Supervisor no longer publishes `worker_exited` | CONFIRMED | observed reason stays `none` |
| Faulted callback skips `silence()` (holds stale output) | CONFIRMED | `killed.isolated[i] == 0.0f` (`0.009765625f`) |

## Not established here

- Realtime qualification: these timings are a functional bound on a shared
  host, not a paced WCET/p99 campaign.
- A kill that lands while the callback itself is executing. The kill here falls
  between callbacks.
- Restart/recovery after worker death. `recover_isolated_clap` requires an
  accepted checkpoint and complete cleanup. This test checks cleanup completion
  (`stop()` returns `complete`) but does not recover.
- Host-process death, Linux, and Windows.
