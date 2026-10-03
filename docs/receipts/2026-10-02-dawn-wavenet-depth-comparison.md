# Historical Dawn production WaveNet depth comparison (2026-10-02)

> **Audit-only synthetic negative control.** This receipt records a temporary
> test seam from before the production multi-flight implementation landed. It
> is superseded for source capability by commits
> [`794d54f8d4`](https://github.com/Generous-Corp/pulp/commit/794d54f8d417ef76c5b7f66e281286beb68a0b45)
> and
> [`9f37814c22`](https://github.com/Generous-Corp/pulp/commit/9f37814c225902c6f273c189e439e1ad712e221c),
> and remains historical evidence only. It is not an exact-provider hardware
> result.

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

At the receipt's source boundary, the node owned one `inflight` stamp.
`794d54f8d4` replaced that seam with bounded per-submission pending state and a
`max_inflight` configuration; `9f37814c22` corrected the telemetry assertion
to use `in_flight_high_water` and recorded the pending count. Current source
capability must be assessed against those landed commits, not this historical
one-flight snapshot.

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

The pre-landing depth comparison passed **87 assertions in 1 test case**. Both requested
capacities admitted 6 blocks and delivered 5 in order:

| requested capacity | submitted | delivered | high-water in-flight | ordered | fallback | late | deadline misses | GPU timestamps |
|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 2 | 6 | 5 | 1 | 5 | 0 | 0 | 5 | unavailable |
| 8 | 6 | 5 | 1 | 5 | 0 | 0 | 5 | unavailable |

The full WaveNet realtime binary also passed: **895 assertions in 27 test
cases**.

## Disposition

This remains a valid **historical synthetic production-owner negative control**
for the pre-`794d54f8d4` implementation. It does not prove exact-provider
behavior, concurrent GPU execution, GPU speedup, or a deadline-margin benefit.
The landed replacement adds bounded multi-flight source coverage, but an
authenticated provider-backed throughput/deadline result is still a separate
gate.
