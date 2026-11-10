# G-7 realtime qualification audit — explicit HOLD

Date: 2026-11-10  
Source head: `fb0ca2601cab3f082d72e4bba9f2aaf5675eb3a8`  
State: **PARTIAL / HOLD**

This is an evidence-only audit. It adds no graph runtime, scheduling, or
public API. The current G-7 implementation slice remains useful for bounded
control/event diagnostics, but the roadmap's realtime qualification is not
proven by those probes.

## Verified existing surfaces

- `examples/g7-latency-pdc-consumer/main.cpp` exercises prepared latency,
  planted unknown-node/connection negatives, and an eight-block wall-clock
  shaped loop. The example itself explicitly labels that loop diagnostic and
  makes no realtime or deadline claim.
- `tools/graph_performance_qualification.py` validates receipt structure,
  timing percentiles, allocation counters, and planted mutations. Its module
  contract explicitly excludes callback deadlines, WCET, and cross-platform
  realtime safety.
- `docs/evidence/g7-residual-qualification-20261109.md` records the public
  control/event probes and a bounded ring probe. It explicitly leaves callback
  deadline/WCET, paced realtime, parallel execution, long-run loss, host and
  provider coverage, cross-platform parity, and installed-SDK parity open.

## Qualification inventory

| Requirement | Disposition | Why the current evidence is insufficient |
|---|---|---|
| Callback deadline / WCET | **HOLD** | No audio-device scheduler, callback deadline receipt, or independent WCET campaign is bound to the graph executor. |
| Paced realtime | **HOLD** | The latency example sleeps from a control process and labels itself diagnostic; it does not establish callback-thread behavior. |
| Allocation/lock/wait/I/O matrix | **HOLD** | No exact-head callback probe with independent planted resource violations exists for this G-7 surface. |
| Parallel execution | **HOLD** | The public probe attachment rejects parallel/anticipated routing; no positive parallel qualification was run. |
| Long-run loss characterization | **HOLD** | There is no sustained run with a loss denominator, deadline accounting, and terminal disposition receipt. |
| Host/provider coverage | **HOLD** | Existing results are one local CPU process; no authenticated host/provider or DAW callback receipt exists. |
| Cross-platform parity | **NOT CHECKED** | No Windows/Linux or second-host G-7 realtime run is available. |
| Installed-SDK parity | **PARTIAL / HOLD** | Existing installed consumers cover bounded latency/edge controls, not callback timing or the matrix above. |

## Safe next experiment

Close this HOLD only with a clean exact-head Release installed consumer that
binds the public graph executor to a real callback-shaped harness and records
64/128/256/4096-frame paced runs at representative sample rates. The receipt
must include p50/p99/max deadline margins, allocation/lock/wait/I/O counters,
long-run loss accounting, independent planted-negative controls, and explicit
host/provider/platform identity. A control-thread sleep loop, offline timing
proxy, green CI check, or local arm64 result cannot substitute for that
receipt.

No runtime implementation is authorized by this audit. The correct roadmap
status remains **PARTIAL / HOLD**, with the missing experiment and its
acceptance fields named above.
