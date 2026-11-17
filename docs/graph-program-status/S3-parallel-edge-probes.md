# S3 parallel edge-probe observation

Status: **BUILT for audio, control and event probes on the levelized parallel
walk.** Measurement of probe overhead under parallel routing is noisy on a
loaded host and is reported as such, not as a bound.

## What changed

- Every probe kind records into storage owned by the block
  (`EdgeProbeStage` in `core/format/src/graph_runtime_executor.cpp`). The task
  that runs the observed node writes the one observation for that probe: an
  audio source right after its node runs, a control or event edge in its
  destination's gather. A feedback probe reads the retained slot after the last
  level and before capture.
- The thread that called the executor publishes every observation once after
  the final level barrier. The serial walk uses the same stage and the same
  publication point, so each ring keeps one producer on every backend and both
  backends publish identical records.
- A failed block publishes no record of any kind; probe clocks still advance,
  as before for audio.
- Attach admits parallel routing and the parallel PDC domain. It still refuses
  anticipation, a disabled routed executor and the legacy PDC domain, and it
  refuses when the parallel plan names the edge differently from the serial
  plan at the same index.
- A parallel block that fails setup and is retried on another backend stops the
  probes instead of observing and clocking the block twice.
- The generic CLI/MCP probe is unchanged apart from its refusal text. No
  synthetic Processor was added for a CLI/MCP control positive path; that
  remains an application-owned catalog decision.

## Evidence

`test/test_graph_parallel_edge_probe.cpp` (suite
`pulp-test-graph-parallel-edge-probe`), all through `SignalGraph::process()`:

- Serial and parallel records are equal field by field, and both equal values
  computed from the stimulus alone, over ragged partitions (8, 3, 5, 1, 8, 7,
  2, 8, 4, 6) for plain, PDC, feedback and in-place-clobber topologies, and for
  sparse control, dense control and event probes. Outputs are bit-identical and
  the parallel run must have dispatched at least one parallel level while the
  serial run dispatched none.
- PDC: the graph runs the parallel PDC domain; the probe sees the undelayed
  source while the output carries it delayed.
- Switching between the serial and parallel walk keeps the attachment, the
  sequence and the clock.
- Slow reader: 300 blocks without reading keep 256 records, count 44 drops, and
  the next record follows the clock.
- Concurrent attach, detach and read against a running audio thread whose
  probed level ran on worker threads; parking stays within the cap and drains
  to zero once quiescent.
- No allocation on the calling thread during parallel processing
  (`RtAllocationProbe`, with its self-check).

Mutation controls (`tools/scripts/confirm_failure.sh`, clean committed tree):

| Mutation | Result |
|---|---|
| Parallel audio capture moved after the level barrier | CONFIRMED |
| Parallel audio capture taken before the source node runs | CONFIRMED |
| Parallel feedback observed after capture | CONFIRMED |
| Serial audio capture moved after the walk | CONFIRMED |
| Timestamp stamped one block late | CONFIRMED |
| Control and event gathers not observed on worker tasks | CONFIRMED |
| Host does not pass probes to the parallel walk | CONFIRMED |
| Attach refuses parallel routing again | CONFIRMED |
| Parallel/serial plan agreement check always passes | NOT CONFIRMED |
| Probe stop on a retried parallel block removed | NOT CONFIRMED |

The two NOT CONFIRMED guards are defensive: no public graph built through the
current API produces a parallel plan that disagrees with the serial plan, or a
parallel setup failure after nodes have run, so neither path has a public-API
fixture.

ThreadSanitizer (`-DPULP_SANITIZER=thread`, RelWithDebInfo, GPU off) ran the
whole suite, including the hammer, with no report. A planted control that lets
every worker task write the audio stage record produced a data-race report on
`EdgeProbeStageRecord::put`.

## Overhead

Hidden benchmark `[bench]`, 8-frame blocks, a five-node graph with all three
probes attached, on m3 under a load average of about 18:

| Walk | No probes | Three probes |
|---|---|---|
| Serial | 329 to 332 ns/block | 365 to 383 ns/block |
| Parallel | 18.7 to 28.7 us/block | 23.4 to 40.1 us/block |

The parallel figures are dominated by fork/join on a loaded host and vary more
between runs than between probed and unprobed, so no parallel probe overhead
bound is claimed. The unprobed serial cost was not compared against the code
before this change.

## Not checked

- Worker-thread allocation and lock freedom. The allocation probe counts only
  the calling thread; the worker path is fixed-size arithmetic on preallocated
  storage, but no RTSan or trap build ran it.
- A quiet-host overhead measurement and any paced realtime qualification.
- The installed-SDK probe consumers were not rebuilt against this change.
