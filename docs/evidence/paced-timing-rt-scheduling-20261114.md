# Real-time scheduling in the paced-timing matrix (2026-11-14)

Status: **TOOLING ONLY, NOT QUALIFIED**. The shared paced-timing matrix now
requests real-time scheduling for every cell's callback thread, records the
class the kernel actually granted, and refuses when it cannot get one. The
G-12 wake rule is unchanged: p99 absolute wake below 0.1 sample. No quiet-host
pilot was run (see "Pilot" below), and the roughly 10 h qualification run is
still to be scheduled.

This updates the runner described in
[`paced-timing-matrix-tooling-20261113.md`](paced-timing-matrix-tooling-20261113.md).
Its results section describes runs made before this change, at default
scheduling.

## What changed

**Harness** (`test/harness/paced_timing_cell.hpp`). `PULP_PACED_SCHED` selects
the callback thread's scheduling:

| value | request |
|---|---|
| `none` | default scheduling (timeshare). Never qualifies. |
| `realtime` | macOS: Mach `THREAD_TIME_CONSTRAINT_POLICY` with period = one block, computation = half a block (capped at the kernel's 50 ms real-time quantum), constraint = one block, preemptible. Linux: `SCHED_FIFO` at the policy maximum minus 10. |
| `realtime-invalid` | A malformed real-time request the kernel itself must reject (macOS: constraint below computation, `KERN_INVALID_ARGUMENT`; Linux: priority above the maximum, `EINVAL`). This is the negative control for the refusal path, not a mock. |

The request is applied on the callback thread before the first callback, then
read back from the kernel (`thread_policy_get` on macOS, `pthread_getschedparam`
on Linux). The cell refuses when the request fails, when the read-back is not
the requested class, or (macOS) when the read-back period, computation or
constraint differ from what was set. A refused cell runs no callback, prints its
`PACED_SCHED` line with the reason, and fails its Catch2 case. It never falls
back to default scheduling. Other platforms refuse with
`unsupported_platform`.

Every cell prints one line:

```
PACED_SCHED requested=realtime obtained=time_constraint status=obtained reason=none period_ns=2666625 computation_ns=1333291 constraint_ns=2666625 preemptible=1 priority=31
```

Period, computation and constraint are the read-back values, quantised to
Mach ticks (about 41.7 ns on Apple silicon). `priority` is the pthread view of
the thread and does not reflect the time-constraint band on macOS.

The G-12 activation fixture (`G12 paced activation receipt cell`) applies the
same request on its own callback thread, so all five fixtures are covered.

**Runner** (`tools/graph_paced_timing_matrix.py`). Receipt schema is now
`pulp.graph_paced_timing_matrix_receipt.v2`:

- `--scheduling {realtime,none,realtime-invalid}`, default `realtime`.
  Qualification accepts only `realtime`.
- New top-level `scheduling`: `{rule, requested, refusal}`. Each cell gains
  `scheduling` (the parsed `PACED_SCHED` line), bound to the raw file like every
  other raw-determined field.
- A cell that reports a scheduling class different from the requested one is
  rejected at record time (`cell scheduling request mismatch`), so a harness
  that silently ignores the request cannot produce a receipt.
- A refused cell stops the run in **every** mode: the runner writes a
  `REFUSED` receipt naming the refusing cell, its exit status and the digest of
  its raw output, and exits with code 3. `validate` re-hashes that raw file and
  checks the refusal against it.
- `timing_qualified` additionally requires `requested=realtime` and that every
  registered fixture cell obtained and read back `time_constraint` or `fifo`.
  New disqualifiers: `scheduling_not_realtime_request`, `scheduling_refused`,
  `scheduling_not_obtained:<cell>`.

## Tests and negative controls

- `Paced cell refuses a real-time scheduling request the kernel rejects` and
  `Paced cell obtains and reads back the requested real-time scheduling`
  (`test/test_trigger_graph.cpp`, discovered by ctest). The refusal case uses
  the kernel-rejected request and asserts that no callback ran and the cell
  reports a failure. The obtained case is the control on the same instrument: on
  macOS it requires `time_constraint` with the read-back period within one
  microsecond of the block.
- `tools/test_graph_paced_timing_matrix.py`, `SchedulingTest`: line
  consistency, obtained control, `none` never qualifying and its request not
  re-signable as realtime, silent fallback rejected, short-mode refusal with
  re-signed tamper rejection (reason, exit status, erased refusal, status flip,
  raw digest), and qualification refused at the first cell on a host that
  denies real-time scheduling. Fake fixtures stand in for the compiled cells.
- With `PULP_PACED_MATRIX_BUILD_DIR=build`, the compiled G-5 and G-12 cells
  obtain `time_constraint` on macOS and produce a validating `REFUSED` receipt
  under `realtime-invalid`.

`tools/scripts/confirm_failure.sh` was run on a clean commit for each guard:

| guard broken | test | verdict |
|---|---|---|
| harness skips the loop on refusal | `[scheduling]` Catch2 cases | CONFIRMED |
| runner stops on a refused cell | `SchedulingTest` | CONFIRMED |
| `scheduling_not_obtained` disqualifier | `SchedulingTest`, `QualificationRuleTest` | CONFIRMED |
| record-time request mismatch check | `SchedulingTest` | CONFIRMED |

The Linux `SCHED_FIFO` path compiles under the same harness but has not been
run; on a host without `CAP_SYS_NICE` or an `RLIMIT_RTPRIO` grant it is expected
to refuse with `errno_1`.

## Loaded-host smoke (not the pilot)

The pilot was not run: the host was m3 (Darwin 27.2.0, Apple M3 Ultra, 28
logical CPUs), which serves the required gate, and its 1-minute load was 25 to
31 against an admission limit of 7. Instead, two back-to-back short runs
(`--dry-run`, 48 cells x 32 callbacks, under `nice`) compared the two classes on
the same loaded host. Both report `timing_qualified=false` and validate. Binaries
were built before the commit, so every cell also carries `stale_binary`.

| scheduling | obtained | deadline misses | p50 wake (median over cells) | p99 wake (median, max) | p99 wake in samples (min, median) | cells with p99 wake < 0.1 sample |
|---|---|---|---|---|---|---|
| `realtime` | `time_constraint` (48/48) | 0 / 1536 | 8.0 us | 13.6 us, 28.4 us | 0.48, 0.69 | 0 / 48 |
| `none` | `timeshare` (48/48) | 20 / 1536 | 440 us | 1480 us, 5733 us | 13.0, 66.8 | 0 / 48 |

What this shows: on this host the time-constraint thread is granted for every
cell and cuts wake latency by roughly two orders of magnitude, and removes the
misses seen at default scheduling. It does **not** show that the 0.1-sample wake
bar (about 2.1 us at 48 kHz, 1.0 us at 96 kHz) is reachable: even the best
real-time cell woke at 0.48 sample p99. The callback still waits with
`std::this_thread::sleep_until`, so part of the residual is the kernel timer
wake path itself, which a quiet host may or may not remove. Thirty-two
callbacks per cell is far too few for a p99 claim either way.

## Pilot and qualification commands (still to be scheduled)

On a quiet host (1-minute load at most 0.25 x logical CPUs), at an exact clean
head, after the governed Release build of the four test targets:

```bash
PULP_BUILD_JOBS=4 nice tools/ci/governed-build.sh cmake --build build --target \
  pulp-test-group-trigger-graph pulp-test-signal-graph-executor-parity \
  pulp-test-voice-bank-binding pulp-test-routed-spectral-ports

# 1. Pilot (about 3 min of paced schedule, real-time scheduling). Never qualifies.
nice python3 tools/graph_paced_timing_matrix.py record --mode short --callbacks 512 \
  --scheduling realtime --build-dir build --output-dir <durable-dir>/paced-matrix-pilot
python3 tools/graph_paced_timing_matrix.py validate <durable-dir>/paced-matrix-pilot/receipt.json

# 2. Full qualification: 48 cells x 100,000 callbacks, about 10.2 h of paced
#    schedule. Refuses on a loaded host or when real-time scheduling is refused.
nice python3 tools/graph_paced_timing_matrix.py record --mode qualification \
  --scheduling realtime --build-dir build --output-dir <durable-dir>/paced-matrix-qualification
python3 tools/graph_paced_timing_matrix.py validate <durable-dir>/paced-matrix-qualification/receipt.json
```

`nice` lowers only the runner's timeshare priority; the cells' callback threads
run in the real-time band. Cells run one at a time, so one paced callback
thread (plus the G-7 attached reader) is active at once. macOS offers no CPU
pinning on Apple silicon, so the run is not bound to one core.

Read the pilot's wake p99 per cell before starting the 10 h run. If it is still
above 0.1 sample on a quiet host, the full run will fail on the wake term alone.
