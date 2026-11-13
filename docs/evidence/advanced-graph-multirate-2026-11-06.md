# Advanced graph multirate audit — 2026-11-06

## Disposition

**NO-CHANGE / PARTIAL evidence.** At live-main source head
`14a859df310eaaecaedb4a8d0735b51f10c1e853`, the graph has one host sample rate
and block size per prepared snapshot. Processor-local conversion is tested;
graph-level rate domains are not implemented or claimed.

This is a source and control audit. It does not add a second scheduler, an
implicit per-node rate field, or realtime qualification.

## Live-main source contract

- `core/host/include/pulp/host/signal_graph_runtime.hpp:445` exposes only
  `prepare(double sample_rate, int max_block_size)`; the compiled snapshot stores
  one `sample_rate` and `max_block_size`.
- `core/host/include/pulp/host/custom_node_type.hpp:120` gives each custom node
  the graph host rate and block size. Its latency callback is evaluated in that
  same domain.
- `core/host/include/pulp/host/signal_graph_connection.hpp:18-43` has audio,
  MIDI, sparse automation, dense audio-rate modulation, sidechain, and spectral
  flags, with no rate-domain or conversion metadata.
- `core/graph/include/pulp/graph/graph_runtime_plan.hpp:57-111` distinguishes
  audio, event, automation, spectral, and trigger lanes. `audio_rate` selects
  dense modulation; it is not sample-rate conversion.
- Processor-local `ResamplerT` and `OversamplerT` remain available in
  `core/signal/include/pulp/signal/resampler.hpp` and `oversampling.hpp`.

The planted source negative searched the graph connection and runtime-plan
headers for `rate_domain`, `rate-domain`, `resampler`, `conversion_edge`, and
sample-rate domain/edge metadata; no graph-level match was found.

## G-5 controls

The governed Release build passed the focused G-5 positive and planted-negative
controls: **8/8** event, automation, PDC, partition, MPE, and host-ingress
cases, followed by **4/4** epoch, forward-jump, cursor-exhaustion, and bounded
refusal cases. The event fixture includes a planted negative that removes the
event edge and expects an empty consumer trace. These controls establish
same-rate event/timestamp behavior and bounded refusal only; none requires an
independent audio rate.

## Processor-local conversion controls

The existing Release controls passed without graph changes:

- Resampler: **4/4**, including block-chunk invariance, mid-stream ratio change,
  out-of-band suppression above output Nyquist, and a 44.1 kHz → 48 kHz → 44.1
  kHz round trip.
- Oversampler: **2/2**, including caller partition invariance and allocation-free
  `process_block` after configuration.

These are Processor-local quality and allocation controls. They do not prove a
graph-owned rate domain, conversion-edge admission, PDC, or mixed-rate
scheduling.

## Reproducing the counts

Line anchors in this audit are pinned to source head `14a859df`; they drift on
later heads. The counts above are case subsets selected inside these suites
(the suites hold more cases than the counts), run from a Release build:

- G-5 (8/8 and 4/4): `ctest -R 'pulp-test-trigger-graph'` (sources
  `test/test_trigger_graph.cpp`, `test/test_g5_host_event_contract.cpp`), plus
  `ctest -R 'trigger-runtime-public-consumer|g5-host-event-public-consumer'`.
- Resampler (4/4): `ctest -R 'pulp-test-resampler'`, cases "Resampler output is
  invariant to input block chunking", "Resampler ratio change mid-stream does
  not click", and "Resampler 1 kHz round-trip 44.1→48→44.1 reconstructs the
  sine" in `test/test_resampler.cpp`.
- Oversampler (2/2): `ctest -R 'pulp-test-oversampling-quality'`, case
  "Linear-phase oversampler is invariant to caller block partitioning" in
  `test/test_oversampling_quality.cpp`. The allocation-free `process_block`
  case behind the second count is not named by a test at the current head and
  is not independently reproduced here.

## Reopening trigger

Reopen G-8 only when the first named G-5 or G-6 workflow requires a node to run
at a different rate from its graph host. The implementation packet must state
the workflow and conversion ratio and include buffer mapping, state and tail
semantics, control/event timestamp mapping, scheduler admission, PDC,
explicit unsupported-conversion errors, block-boundary continuity, an aliasing
negative, bounded callback cost, offline parity, and supported-platform
behavior. Until that trigger exists, keep the disposition **NO-CHANGE**.

