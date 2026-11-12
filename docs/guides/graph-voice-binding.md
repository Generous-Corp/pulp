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
changes the work order, not the acceptance requirements. The data-only follow-
up carries only the generated vectors and in-process comparisons; the
independent verifier is a separate prerequisite.

This slice is **PARTIAL**. In its bounded MIDI 1.0 MPE profile, host ingress
accepts member and manager CC64 sustain, scoped CC120 all-sound-off, CC121
reset, and CC123 all-notes-off. Member pitch, pressure, and CC74 reach the
current note generation and change graph audio. Manager pitch, pressure, and
CC74 remain typed refusals because their zone expression composition is not
defined. The public consumer covers audible pedal/tail and expression traces
across multiple block partitions; its frozen A1 fixture covers seven stereo
sum, release, and steal cases in the installed consumer. The independent
scalar verifier and planted-negative checks are supplied separately. The
production-source mutation script requires positive,
broken, and restored outcomes for ingress, identity, pedal, and release
faults.

Saved bank activation, compatible edit, undo/redo, arbitrary graph shape,
hosted plugin formats, Linux/Windows execution, and the paced callback
allocation/lock/deadline matrix remain unqualified. Offline in-process audio
and an installed SDK consumer do not prove those runtime or platform lanes.
