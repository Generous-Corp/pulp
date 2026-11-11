# Bounded graph voice binding

This source slice binds one authored `SubgraphTemplate` containing a versioned
in-process `Processor` to eight named slots in one `SignalGraph`. The caller
supplies a `VoiceBankOwner`; `MpeVoiceBankOwner<Voice>` retains one existing
`MpeVoiceAllocator<Voice>` and constructs one Processor per slot. The graph
retains the Processor instances, and the bank retains the owner until after
the graph is destroyed. Factories run only during control-thread preparation.
The callable and allocator address are transient and never enter the G-4
manifest, identity encoding, or saved state.

The bank resolves declared stereo output, the owner's exact named note and
expression endpoints, and named Processor parameter targets before publication.
It rejects null or repeated mutable slot identities. It uses G-5
`HostEventIngress` for absolute sample,
producer, sequence, overflow, late, and epoch checks. It dispatches all events
at each sample boundary before rendering that sample, splitting one prepared
graph render into intervals. Each interval passes a zero-channel input view
with the interval's exact frame count, as required by `ProcessBlock` even for
an instrument. A control-side failed candidate leaves the
previous bank live when the caller retains it. The example voice used by the
test chooses a triangle oscillator, a four-frame release, duplicated stereo,
and fixed `1/8` gain; these are fixture choices, not framework DSP defaults.

## Admission ordering and current limit

The 2026-11-06 G-4/G-6 packet ordered the A1 audio oracle before runtime
implementation. That formed a circular dependency for a production graph
render: the independent scalar oracle could be frozen first, but no installed
graph consumer could verify it until a binding existed. The lead authorized
implementing the binding and its independent audio oracle together. This
changes the work order, not the acceptance requirements.

This slice is **PARTIAL**. Host ingress currently accepts the bounded note and
per-note expression subset; CC64/120/121/123 sustain and scoped panic are
explicit refusals. Saved bank activation, compatible edit, undo/redo,
and the full A1 realtime matrix remain completion requirements. The installed
consumer test exercises the bounded audio path; the production-source mutation
script plants independent interval-input, tail-input, alias, binding-path,
stale-epoch, steal-policy, and release faults and requires positive, broken,
and restored outcomes. These checks do not prove paced callback bounds.
