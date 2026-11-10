# G-5 installed-consumer and realtime qualification audit — 2026-11-10

**Disposition: PARTIAL / HOLD. Evidence only; no runtime or public API changes.**

## Exact source and checks

This audit was performed from `origin/main` at
`fb0ca2601cab3f082d72e4bba9f2aaf5675eb3a8`. The existing installed-SDK host
event receipt and graph-authored event contract were inspected at that exact
source. The frozen public-consumer trace oracle was run with:

```text
python3 -m unittest discover -s examples/g5-event-consumer -p 'test_*.py' -v
```

Result: **4/4 tests passed**, including the positive trace and planted one-sample,
reversed-tie, and ninth-accepted-counter negatives.

## Verified current capabilities

* `HostEventIngress` provides fixed-capacity epoch and absolute-sample staging,
  deterministic producer/sequence ordering, late/stale/duplicate/capacity
  refusal, rollback, and transactional projection into MIDI/UMP/MPE sidecars.
* `GraphAuthoredEventLane` provides a bounded same-block producer/consumer
  contract with epoch checks, deterministic ordering, and the 16-attempt/8-
  accepted-per-sample refusal budget.
* The installed host-event consumer receipt exercises projection and partition
  invariance controls. The trace-oracle fixture independently rejects shifted
  samples, reversed ties, and an invalid accepted-count mutation.
* The trigger-runtime consumer exercises the admitted built-in trigger pair;
  this is distinct from arbitrary graph-authored Custom-node callbacks.

## Unproven or explicitly held requirements

The current public consumer does not prove an end-to-end graph-authored
producer/consumer event edge. The existing `g5-event-consumer` fixture is
pre-runtime and calls no graph event API. The installed host-event consumer
proves the host contract, not SignalGraph admission or Processor dispatch.

The following gates therefore remain **NOT CHECKED / HOLD**:

1. A public graph-authored producer and consumer wired through both routed and
   reference execution paths.
2. Queued cross-block delivery, exactly-once semantics, bounded pending-ring
   overflow, reset/seek retirement, and epoch exhaustion.
3. Installed-SDK execution of the same graph-authored trace, including private
   header refusal and wrong-output/sample mutation controls.
4. Backend parity for event tuples and audio under split block partitions;
   parallel and anticipation modes remain unqualified.
5. Callback deadline, allocation, lock, wait, filesystem/network I/O, and
   p99/jitter evidence at 64/128/256/4096-frame blocks.
6. Hosted-plugin and format-adapter parity, non-Darwin parity, and hard
   realtime qualification.

These are architectural/runtime gates, not failures of the current bounded
contract. No speculative scheduler, callback registry, queue, or plugin event
surface was added. The safe next implementation requires the reviewed
single-edge contract in `g5-graph-authored-event-followup-contract-20261109.md`
and must satisfy its seven evidence gates at one exact source head.

## Controls and limitations

The four oracle tests are meaningful controls for the frozen trace parser, but
they do not substitute for a runtime consumer. Existing receipts are retained
as historical evidence and must be regenerated whenever the public event
surface changes. Dedicated-builder source-selftest, diff-coverage, realtime,
cross-platform, and hosted-plugin lanes remain unavailable in this audit.
