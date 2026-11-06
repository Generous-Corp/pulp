# G-6 polyphony implementation HOLD (2026-11-06)

## Exact base and disposition

This audit starts from Pulp `origin/main` at
`194430623664c1850549c1bcda51408852c5f9d9` in a clean, fresh worktree.
Disposition: **HOLD; no G-6 runtime change and no duplicate G-5 PR**.

The installed `g6-voice-contract-consumer` and its `PASS checks=160` receipt
characterize the public `MpeVoiceAllocator`/`MpeSynthVoice` helper only. They do
not prove graph expansion, graph event delivery, audio output, sustain, or
realtime behavior.

## Dependency readback

### G-4 is merged, but its boundary is authoring identity only

The G-4 identity merge is present at `b641744a1a6f` (PR 11558). The current
status records a value-only canonical key for template/bank/slot/path/port and
explicitly says that plugin, event, polyphony, and nested-subgraph declarations
remain future work (`docs/graph-program-status/G-4.md:21-24,32-41`). The public
identity has no runtime node, allocator, epoch, or note-generation binding.

### G-5's merged contract is not yet a graph Processor/MPE ingress

The merged host contract (`e78c74749b73`) provides fixed-capacity epoch/sample/
sequence admission and `derive_mpe` (`core/host/include/pulp/host/graph_event_contract.hpp:58-64,217-240`).
The contract itself says host records are projected by the graph owner and are
not injected into SignalGraph's block-local mailbox
(`core/host/include/pulp/host/graph_event_contract.hpp:3-10`). The G-5 status
keeps this lane as a contract primitive, not Processor dispatch
(`docs/graph-program-status/G-5.md:5-25`).

On this exact base, `GraphRuntimeNodeProcessContext` carries per-node MIDI but
no MPE sidecar (`core/format/include/pulp/format/graph_runtime_executor.hpp:86-105`).
The Processor node's routed path therefore constructs an `EventBlock` from
MIDI, parameters, and modulation only; the shared-block path explicitly sets
`Processor::mpe_input()` and `Processor::ump_input()` to null
(`core/format/src/processor_node_adapter.cpp:214-258`). The lower-level
`process_processor_block` adapter can consume sidecars when a caller supplies
them (`core/format/src/processor_block_adapter.cpp:96-104`), but no graph
binding on this base supplies them.

The candidate sidecar propagation commit `a487a7aae958abf03e6ab769f4add97ff3864b3a`
is **not an ancestor** of this base (`git merge-base --is-ancestor` exits 1);
it lives on `codex/g6-ingress-final-20261105`. Reusing that unmerged work here
would duplicate the G-5 slice and would not be an admitted dependency.

## Existing public voice surface

`MpeVoiceAllocator<Voice>` is a preallocated pool with four steal policies
(`Oldest`, `LowestVelocity`, `LowestPitch`, `HighestPitch`) and generation-aware
dispatch (`core/midi/include/pulp/midi/mpe_synth_voice.hpp:110-176,215-243`).
It routes note and expression events to concrete voice objects, but it has no
graph slot binding, authored instance expansion, sample-interval scheduler,
sustain/pedal state, or graph output ownership. The existing consumer is thus a
valid allocator control, not a graph-authored polyphony implementation.

## Smallest admitted successor

1. Land or otherwise admit one exact-head G-5 sidecar slice spanning routed
   scratch, legacy walk, Processor binding, graph swap/mailbox lifetime, and
   stale/late/overflow controls. Do not copy that work into G-6.
2. Add a reviewed G-4 Processor voice declaration/instance binding that maps
   the canonical identity to one prepared slot namespace; keep graph mutation
   off the callback.
3. Only after those dependencies are on `origin/main`, implement one fixed
   eight-voice consumer using the existing MPE allocator. Its first controls
   must cover event offsets across block partitions, all four steal policies,
   stale-generation expression, overlapping same-pitch notes, sustain/pedal
   ordering, per-voice isolation, finite tails, and positive audio output with
   planted routing/timing/state negatives.

Until steps 1 and 2 are admitted, adding an allocator wrapper or graph voice API
would either duplicate G-5 or invent a second graph/voice contract. This HOLD
does not claim G-6 completion.
