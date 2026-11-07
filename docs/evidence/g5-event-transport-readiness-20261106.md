# G-5 event transport readiness packet — 2026-11-06

**Disposition: READY FOR CONTRACT ADMISSION; NO RUNTIME WIRING IN THIS SLICE.**

This packet binds the next G-5 implementation boundary to Pulp
`origin/main` at `a09d5269eda655aaa12726a8fa6808daa47a375a` in a fresh
classified worktree. It records the frozen event semantics, dependency
readback, and the public-consumer evidence that must gate a later
`SignalGraph`/`Processor` wiring change. It does not add a scheduler, change
the current-sample trigger runtime, or claim realtime or plugin parity.

The source contract was reviewed at planning revision
`62be44f7e7f4ebbc2f28d6049a9179eee20845bb` (`design/g5-event-transport-20261104`),
and the roadmap at planning revision
`6b87a31fe2c8e5ef75884efc654abafc03693e14`.

## Frozen bounded semantics

The implementation owner must preserve these limits and refusal rules. A
change to a number or to the producer scope returns to contract review.

| Area | Admitted behavior |
| --- | --- |
| Producer scope | One declared typed trigger producer and one downstream consumer on an acyclic event edge. Arbitrary callbacks, multiple producers, fan-out, event feedback, and MIDI wire encoding remain outside this slice. |
| Coordinates | One unsigned 64-bit epoch-local sample cursor. A render owns `[S, S + N)` with `1 <= N <= 4096`; checked addition rejects cursor exhaustion before state or output mutation. Host time and beat time require an explicit adapter projection and do not move the graph cursor. |
| Same-sample order | Producer-local `uint64` sequence, increasing for every attempt, orders ties. Sequence starts at zero on reset and survives block boundaries within the epoch. |
| Producer phase | A producer may emit only at the canonical sample currently in its phase. A target after the current sample, including the next block boundary, returns `Future`; an already consumed target returns `Late`. No refusal retimes, queues, or delivers an event. |
| Refusal priority | `StaleEpoch`, `Unbound`, `OutOfRange`, `Future`, then `Late`; each refusal leaves accepted records, consumer state, audio, and cursor unchanged. |
| Capacity | At most 16 attempts and 8 accepted records per canonical sample. Attempts 1–16 consume sequence identity even when invalid or overflowing. The 17th call latches `AttemptLimit` and does not consume sequence. Overflow and diagnostic refusal counters saturate independently. |
| Reset/swap | The graph controller owns epoch creation. Reset or a validated preallocated replacement occurs only between completed renders, clears records/counters/cursor/producer state, and refuses `Busy` or epoch exhaustion without mutation. Old-epoch records cannot cross the boundary. |
| Partitioning | Accepted complete event streams must produce identical `(epoch, sample, port, sequence, payload)` traces under one legal block and arbitrary legal positive partitions. Rejected terminal-exhaustion calls are a separate refusal oracle. |
| Allocation and work | Activation reserves at most `4096 * 8 * 32 = 1,048,576` event-record bytes and no more than a 2 MiB total graph-event budget. Rendering uses fixed storage, no allocation, blocking, I/O, or unbounded retry. |
| Time-index control | A dedicated control input may repeat, reverse, or jump without changing event order or graph cursor. It must name its domain and projection; the scalar `IndexedControl` table-sample behavior is not implicitly a graph timestamp. |

## Dependency and ownership matrix

| Dependency | Live state on `origin/main` | Consequence for this slice |
| --- | --- | --- |
| G-2 notation/restore | Merged and implemented on main; the status remains PARTIAL for broader notation goals. | No G-2 merge admission blocks this readiness packet. Persistence of event producers/queues remains out of scope until a separate schema decision. |
| G-7 probes/PDC | Bounded latency/PDC API is on main; the CLI/MCP edge-probe follow-up is on `origin/codex/g7-wave2-20261106` and is not an ancestor of `origin/main`. | G-7 is not a runtime dependency for the contract, but a future event probe may consume this slice only after its own exact-head admission. Do not claim G-7 wave2 is merged. |
| G-4 identity | The bounded identity slice is present, but event and voice declarations remain future work. | A producer/consumer must be plan-stable and declared by the later admission; do not infer identity from insertion order. |
| G-5 current-sample runtime | Merged typed trigger runtime is current-sample only and refuses future targets. | Preserve that behavior until the event transport contract and graph binding land together. |
| Host ingress | `HostEventIngress` projects absolute host records and derives a caller-owned MPE sidecar, but explicitly does not inject the graph mailbox. | Wiring must carry MIDI, UMP, and MPE together through routed scratch, legacy walk, Processor binding, and swap mailboxes. A partial path is not admissible. |
| Processor boundary | `ProcessorSignalGraphBinding` exposes control authority only; graph dispatch currently supplies MIDI/UMP but no host absolute-time projection to Processor nodes. | The next source PR must add one end-to-end binding and retain v1 Processor behavior for nodes that do not opt in. |
| Realtime qualification | No current receipt proves callback timing, allocation, locks, waits, or plugin parity for this event lane. | Keep realtime, scheduler, adapter parity, and hosted-plugin claims explicitly out of scope. |

## Evidence gates for the implementation PR

The later source change is independently valid only when all gates below are
present at the same exact head:

1. A focused in-tree test exercises the bounded `HostEventIngress` and
   `GraphAuthoredEventLane` controls already described by this packet, plus
   SignalGraph partition and prepared-swap wiring.
2. A clean external consumer builds against the installed public SDK and
   renders a graph-produced event workflow. It emits the canonical event trace,
   counters, and feed-forward output for one block and split partitions.
3. Planted negatives fail the consumer or test when one expected sample is
   shifted, a tie is reversed, a stale epoch is accepted, a future or consumed
   target is delivered, event capacity is exceeded, or a one-byte-over-budget
   allocation is admitted. A private-header compile control must also fail.
4. The consumer repeats the positive trace across reset boundaries and records
   the exact host/graph projection used. Delayed audio feedback is measured on
   its own fixed block schedule; it must not be presented as partition-invariant
   event or audio proof.
5. The receipt names the source SHA, SDK prefix and manifest hashes, binary hash,
   build profile, focused test counts, and the return codes for every planted
   negative. It states that scheduler/realtime/plugin parity remain unqualified.

The existing installed-SDK receipt is a prerequisite control, not completion:
`docs/evidence/g5-host-event-installed-consumer.md` reports 79 checks and
planted private-header and wrong-output negatives for the contract primitive.
It intentionally records that SignalGraph admission and Processor dispatch are
not wired. The in-tree contract suite is the matching positive/negative
control surface in `test/test_g5_host_event_contract.cpp`.

## Safe next action

After this packet is admitted by the graph lead, implement one source slice
that maps the fixed ingress projection into both SignalGraph execution paths
and the Processor node binding. Keep the producer scope and limits above
unchanged, add the consumer and mutation controls in the same PR, and stop at
the exact-head receipt. Until then, this packet is a readiness artifact only.
