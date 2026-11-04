# Routed spectral ports

Status: **PARTIAL — implementation in progress; no routed execution qualification.**

Authority: [admission](https://github.com/danielraffel/pulp-planning/blob/3920bf5bfa24f1ec9cd030a43dac13c212ca7a76/graph-program-status/G-10.md)
and [reviewed lowering contract](https://github.com/danielraffel/pulp-planning/blob/148602040cf0dce7f38d9353e1c3229778468bae/graph-program-status/g10-routed-lowering-design-20261103.md).

The implementation preserves one SignalGraph execution plan and the existing
STFT/WOLA engine. No callback codec, second execution runtime, unrestricted
native stage registration, saved graph restoration, or full G-10/A3 completion
is authorized by this work.

## Delivery sequence

1. Guarded split analysis/synthesis in the existing engine: transactional
   inverse/OLA commit, finite output, bounded storage and checked clocks.
2. Named analysis, closed identity/mask stages, synthesis and spectral edges:
   typed bindings in the existing serial plan, graph-owned bounded batches,
   identity/cardinality/reachability validation, N+H PDC and branch-local errors.
3. Render-owned epoch transitions and two-phase candidate handoff: stopped
   entry, access routing/refusals, mailbox preservation, reader retirement,
   exact reset/seek behavior and namespace exhaustion.
4. Installed public-consumer execution, adversarial controls, production
   mutations, callback-resource proof, governed builds and repository gates.

The guarded engine is a prerequisite, not proof of routed ports. It must not
be reported as the admitted first slice or as an independently connected
SignalGraph consumer. A safe PR boundary may separate it from graph integration;
the remaining obligations below remain open until independently evidenced.

## Outstanding contract obligations

- Published typed named handles with stale/foreign rejection and repeated
  validation through generic and prepared-edit entry points.
- Closed immutable catalog factories; invalid configuration and unknown IDs
  reject; constant-output production mutations fail qualification and zero guard.
- One serial node-major walk, no anticipation/workers or legacy fallback.
- F=ceil(B/H), B<=4096, disjoint fan-out storage, actual retained spectral
  allocation <=64 MiB including engine/provider/scratch/diagnostic overhead.
- Spectral latency propagation without audio delay storage; dry/wet alignment,
  upstream delay, nesting and fan-out proof.
- Frame chronology, single replacement for invalid scheduled slots, duplicate
  rejection, independent fatal markers and exact branch-local cutoff/prefix.
- Coherent fixed telemetry records even with timing disabled; sticky errors,
  publication drops/age and epoch reset semantics.
- Single render transition writer, process-lifetime namespaces, fresh epochs,
  genuine exhaustion behavior and checked chronology at synthetic limits.
- Three-slot offer/acknowledgment/finalization with no audio allocation,
  destruction, lifecycle callback, lock, retry or authoring mutation.
- Complete live-slot/raw-dispatch inventory; all snapshot-bound operations
  controller-routed or explicitly refused; no live pinned spectral execution.
- TransitionPending/Busy/PendingMailboxData refusal and exactly-once mailbox
  preservation, including exact parameters and retained metadata readers.
- Failed/cancelled/stale candidate versus active-only reset oracle using
  production barriers; successful offer plus reset advances once.
- Tail drain L+N-1 and nested composition; runtime-only save/load/bake refusal.
- Clean installed SDK consumer with <=200 glue lines, independently connected
  identity/mask stages, sibling synthesis and dry mixing; ctest, documentation
  and notation coverage (missing notation remains PARTIAL).
- Every acceptance row: passing control, production mutation detected by
  confirm_failure, restored green; callback matrix qualification is separate.

## Working evidence (not delivery qualification)

The routed focused suite has passed 32,060 assertions across 11 cases. It covers
named independent identity/mask execution, fixed/irregular partitions, fan-out,
nested dry PDC, staged replacement, barrier-controlled late offers, rejected
candidate/reset preservation, reader backpressure, exhaustion, fatal-prefix
preservation, missing-frame replacement, capacity markers and C++ allocation
probing. Additional chronology and zero-preservation controls are being added.
The guarded engine suite passed 28,120 assertions across three cases.

A guarded synthesis production mutation was detected and restored green at the
initial engine revision. It is not current-head routed mutation qualification.
The full build initially found an ambiguous generated prepared-edit connect
probe; its explicit method cast is repaired and the full build is running again.
The first gates attempt stopped at generated script-family drift. An independent
advisory review is running. No clean installed-SDK execution, complete mutation
matrix, callback timing qualification, gate pass, pushed head or PR is claimed.

The 83-line standalone public consumer and documentation example are present.
Notation/catalog coverage and the full 44.1/48/96 kHz by 32/64/128/256 callback
qualification matrix remain unqualified. The allocation probe detects C++ new;
it is not a lock/I/O audit, malloc interposer or timing receipt.
