# Dawn production WaveNet depth comparison (2026-10-02)

> **Audit-only synthetic negative control.** This receipt records a temporary
> test seam, not a landed implementation or an exact-provider hardware result.

## Claim under test

Determine whether `GpuWaveNetRealtimeNode` pipelines more than one physical
provider submission when its stamped ingress capacity is increased. A pass for
the performance gate would additionally require useful throughput or deadline
margin improvement with ordered delivery and no stale, late, fallback, or
device-loss errors.

## Exact boundary

The experiment was run against parent source HEAD `c998455fc7` with the
preserved private telemetry/test diff applied in the worktree. It exercises the
production node owner and its private `WaveNetRealtimeChannel` seam; it does
not modify the public `GraphNode` or SDK ABI.

The node currently owns one `inflight` stamp. `Config::capacity` enlarges the
stamped ingress ring, but does not create additional provider submissions.

## Method and results

The parent worktree's temporary Dawn-configured build was rebuilt with the
governed build wrapper. The test used a fake `WaveNetRealtimeChannel` through
`WaveNetRealtimeTestAccess`; it did not authenticate a Dawn provider or submit
work to Apple GPU hardware. The temporary source/test diff was restored before
this closeout tree was prepared, so no source patch hash or reproducible
implementation commit exists here.

The historical command was:

```text
tools/ci/governed-build.sh cmake --build build-dawn-probe \
  --target pulp-test-gpu-wavenet-realtime-node
build-dawn-probe/test/pulp-test-gpu-wavenet-realtime-node \
  '[gpu_audio][wavenet][realtime][depth]'
```

The depth comparison passed **87 assertions in 1 test case**. Both requested
capacities admitted 6 blocks and delivered 5 in order:

| requested capacity | submitted | delivered | high-water in-flight | ordered | fallback | late | deadline misses | GPU timestamps |
|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 2 | 6 | 5 | 1 | 5 | 0 | 0 | 5 | unavailable |
| 8 | 6 | 5 | 1 | 5 | 0 | 0 | 5 | unavailable |

The full WaveNet realtime binary also passed: **895 assertions in 27 test
cases**.

## Disposition

This is a valid **synthetic production-owner negative control**: the fake
channel path serializes physical submissions at depth one even when ingress
capacity is eight. It does not prove exact-provider behavior, concurrent GPU
execution, GPU speedup, or a deadline-margin benefit. It is not reproducible
from this closeout tree until an owned source patch is implemented. The Phase 4
performance gate remains open; implementing it requires per-submission
state/output tracking and an authenticated provider-slot mapping before a depth
greater than one can be measured honestly.
