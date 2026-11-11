# G12 bounded graph measurement phases

This change extends the existing R2 executable and receipt. G12 remains **PARTIAL**.
The receipt uses version 2 because the former activation and execution-percentile
labels described different measurement boundaries. Historical version-1 evidence
must be interpreted with its historical tool; it is not converted into version-2
or callback evidence.

## What is measured

Each of the existing 40 topology/scale/block cells retains 31 individual samples
for plan compilation, snapshot preparation, and fresh scratch-pool allocation.
Compilation times only `build_graph_runtime_plan`; snapshot preparation times only
`GraphRuntimeSnapshot::reset`; pool allocation times only
`GraphRuntimeBufferPool::reset`. Authored fixture construction, bindings, host
buffers, and the independent output oracle are outside these timed phases.

Each phase also retains cumulative requested C++ `new`/`new[]` bytes, including
its temporary allocations. This is neither retained memory nor RSS and does not
intercept arbitrary `malloc` calls. Scratch payload memory is separately measured
from actual pool slots, frame count, and float width. A public reuse-disabled
buffer assignment provides the same-plan control. Independently checked
node/edge/port counts make graph-size scaling explicit.

Execution retains 31 means of 100,000 consecutive **unpaced** blocks after 500
warmup blocks and measured empty-loop overhead subtraction. Summaries are
recomputed from the retained samples. These means include the fixture processor,
routing and executor work; they do not isolate scheduler overhead. Their p99,
maximum and spread are statistics of batch means, not callback p99, jitter or
WCET. No timing threshold or cross-run speedup is claimed on this shared host.

The v2 receipt binds raw output, retained executable bytes, and captured fixture
bytes. Recording requires the fixture to match its Git commit and rejects
source/binary changes during execution. `validate --source-root .` also checks
the captured fixture against that commit. Governed build logs and the actual
`-O3 -DNDEBUG` compile command are separate build evidence; a binary hash alone
is not a source-to-binary build attestation.

## Remaining activation measurement

The public graph seam already separates candidate preparation from publication:
`SignalGraph::begin_prepared_topology_edit()` in
`core/host/include/pulp/host/signal_graph_runtime.hpp`, then
`PreparedTopologyEdit::prepare()` and `commit()` in
`core/host/include/pulp/host/signal_graph_prepared_topology_edit.hpp`.
`SignalGraphControlAuthority::apply()` uses that same transaction for modulation
routes. `begin_swap_edit()` / `prepare_swap()` is also public, but the latter
combines compilation and publication and cannot by itself isolate activation.

The smallest additional fixture is a mono gain graph plus one prepared
replacement with a distinguishable, deterministic output. The existing paced
and reload owners can time `prepare()` and `commit()` separately and observe the
first replacement-output block through `SignalGraph::process()`. Retain the
publication-request and first-observed-block times, sample/block identity, old
and new output oracles, and failed/stale-commit controls. In concurrent runs,
publication may become visible before `commit()` returns; commit duration and
request-to-first-observed-output latency must remain separate measures. The
existing prepared-swap continuity tests supply public rendering/oracle patterns.
No new runtime API or second pacing loop is needed.

Live activation/publication latency, paced callback p99/jitter/WCET, full graph
retained memory, isolated scheduler overhead, the required sample-rate/block
qualification matrix, hosted fixtures, final flagships and independent final
reviews remain outstanding. This receipt closes none of those lanes by proxy.

## Local evidence

Measured implementation commit: `9c96f7d30ce7971c86d2bc0064e0bc5732237796`.

Receipt digest: `ffae517e2efd99b0ef7e9d914d85211ed159247380e8da636b463217e75c8890`.

Retained executable: 3,727,832 bytes; SHA-256
`f35319bfdb0a4bd4c865dadbe785e982a71268b0fc25d6cb3d40d4af3094f948`.

The companion evidence directory is
`/Volumes/Workshop/Code/agent-worktrees/g12-measurement-phases-20261110-proof`. Its `receipt-final/` directory
retains the current executable, raw stdout/stderr, source fixture and receipt.
The enclosing directory retains governed build/compile evidence, structured
review summaries and mutation phase logs. The earlier `receipt/` is explicitly
historical to the first local implementation commit.

Validation: complete 40-cell real run; zero process and timed-process allocations;
seven Python tests; 16 re-signed planted-negative controls; four production
break/restore checks confirmed (C++ slot accounting, Python memory/reuse
oracle, timing median, and source-commit binding); structured review clean.
The final C++ edit was whitespace-only and rebuilt to the same executable
SHA-256 as the mutation-tested binary; the validator source is unchanged.

Representative 512-frame cells below are descriptive measurements on this
shared host. All time columns are sample medians in nanoseconds.

| Topology | Compile | Snapshot preparation | Pool allocation | Unpaced block mean | Scratch payload bytes | Slots saved |
|---|---:|---:|---:|---:|---:|---:|
| passthrough, 1 processor(s) | 375.000 | 709.000 | 83.000 | 131.208 | 6144 | 1 |
| chain, 64 processor(s) | 5167.000 | 5625.000 | 125.000 | 2591.815 | 6144 | 127 |
| fan-out, 64 processor(s) | 7333.000 | 4375.000 | 1708.000 | 7092.492 | 135168 | 64 |

Repository gates passed on the measured source commit, with two checks explicitly
**NOT CHECKED**: dedicated-builder policy refuses the source/workflow self-test
suites, and changed-surface-script-family verification needs the full gate-profile
configure (this CPU-only build omits unrelated JSX-runtime npm registrations).
The first gate run's formatting/UTF-8 failures were fixed and the final run has
no failing gate. No host policy, CI configuration, or generated inventory was
changed to bypass either missing lane. The evidence report is the only addition
after the measured source commit.
