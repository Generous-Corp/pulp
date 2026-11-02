# Graph-level multirate capability audit (2026-11-02)

Source head: `0f616a440cecdbdaff6234be48f682eec1184587` (`origin/main`).

## Decision

**No graph-level implementation change.** `SignalGraph` prepares one host
sample rate for a compiled snapshot. A node may implement local oversampling or
resampling, but the graph has no per-node rate domain, conversion edge, or
second scheduler. Adding one without a named consumer would expand latency,
real-time, and offline contracts together, so this slice records the boundary
instead of adding graph-wide runtime.

## Representative workflow attempted

The representative mixed-rate workflow is two serial custom nodes that would
ideally run at independent rates, followed by an output node. The available
workflow is `SignalGraph::prepare(sample_rate, max_block_size)` followed by
ordinary `connect` edges and `process()`; every node receives the same prepared
host rate and host partition size. The adjacent supported workflows are:

- node-local oversampling/resampling inside a `Processor` or custom node;
- sparse control-rate automation; and
- dense audio-rate modulation (a parameter lane, not sample-rate conversion).

## Rate-domain finding and controls

- `core/host/src/signal_graph_prepare.cpp` stores one graph sample rate and
  propagates it into each process block.
- `core/host/include/pulp/host/custom_node_type.hpp` gives custom preparation
  the graph rate and block size; latency metadata feeds the existing PDC plan.
- `core/host/include/pulp/host/signal_graph_connection.hpp` has no conversion
  or rate-domain fields; `audio_rate_modulation` is only a dense parameter lane.
- `test/test_signal_graph_executor_parity.cpp` is the positive control for
  dense modulation, including modulation through PDC.
- `test/test_signal_graph_offline_parity.cpp` is the positive control for
  sidechain/PDC and block-partition parity.

The planted negative is a hypothetical second node domain (for example,
96 kHz alongside a 48 kHz host). No public API or current preparation path can
produce that observation; seeing it would indicate an accidental graph-wide
rate split and should fail review.

## Realtime and portability limits

This is source and test-contract evidence, not a numerical qualification of
each node-local resampler. It does not prove conversion quality, alias
rejection, or cross-platform timing. A future graph rate-domain proposal must
specify conversion quality and latency, buffer accounting, PDC, modulation
clock semantics, allocation-free callback behavior, and online/offline parity
fixtures on the supported platforms before runtime work begins.

## Disposition

Keep this as a bounded no-change decision. Re-open only for a named
graph-authored mixed-rate workflow, starting with an explicit conversion node
and measured PDC contract rather than an implicit per-node rate field.
