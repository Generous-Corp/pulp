# ADR: Use host parameter mappings for plugin scene coordination

- **Status:** Accepted (NO-GO for a Pulp collaboration transport or pilot)
- **Date:** 2026-08-01
- **Decision scope:** Plugin-to-plugin scene and macro coordination
- **Evidence baseline:** Pulp `d1ff653cefe022ce920576b3e1086c7db8eaa066`

Product B remains unshipped under this NO-GO; this policy is not an API or
product commitment.

## Context

The candidate use case is a producer that selects or morphs a scene and several
plugin instances that apply the corresponding sound changes. Examples include a
single performance macro moving the filter, mix, and space controls on several
effects, or a host scene recalling a bounded set of parameter values across a
track group.

That use case does not require plugins to discover or message one another. The
DAW already owns the project, plugin-instance placement, automation timeline,
undo history, and format/sandbox boundary. Pulp already exposes stable,
automatable parameters through the audited VST3, Audio Unit, and CLAP
adapters:

- `ParamID` maps directly to the VST3, Audio Unit, and CLAP host-facing IDs, and
  those IDs and registration indexes are the persisted automation identity
  ([`docs/guides/parameters.md`, lines 64–73](../guides/parameters.md#ordering-identity-stability-across-versions)).
- The VST3, AUv3, and CLAP adapters preserve host automation points and their
  sample offsets in a bounded `ParameterEventQueue`
  ([`docs/guides/parameters.md`, lines 172–219](../guides/parameters.md#sample-accurate-automation)).
- UI gestures are forwarded to host automation and undo, while host changes are
  reflected back into plugin state
  ([`docs/guides/parameters.md`, lines 379–391](../guides/parameters.md#format-adapter-integration)).
- Parameter state and optional plugin-owned scene banks already round-trip in
  the host-facing state blob
  ([`docs/guides/parameters.md`, lines 310–364](../guides/parameters.md#state-serialization)).

That automation-fidelity evidence is deliberately format-scoped. AUv2 exposes
block-rate `StateStore` values: its effect adapter attaches an empty, non-null
`ParameterEventQueue` for processor-contract uniformity, while its instrument
adapter exposes no separate parameter-event sidecar
([`docs/guides/formats.md`](../guides/formats.md#au-v2)).
AAX applies one block-level parameter packet to `StateStore` before processing
and does not attach a `ParameterEventQueue` to the processor
(`core/format/src/aax_runtime.cpp:871–940`). WAM v2 likewise has no claimed
sample-accurate automation coverage
([`docs/status/support-matrix.yaml`](../status/support-matrix.yaml)). Products
targeting either format may use its documented host-parameter surface at that
surface's supported granularity, but this decision does not promote it to the
VST3/AUv3/CLAP sample-offset contract.

The adapters implement those claims rather than merely documenting an intended
surface. AUv3 builds and retains the host-observed `AUParameterTree`, relays
host writes through the real-time-safe store path, and sends UI changes and
gesture bounds back to the host (`core/format/src/au_adapter.mm:545–700`). Its
project state delegates to `plugin_state_io` (`core/format/src/au_adapter.mm:1478–1504`),
whose restore contract validates a versioned envelope and rolls back on failure
(`core/format/include/pulp/format/plugin_state_io.hpp:69–97`).

These standard host surfaces are the least-privilege fit for scene
coordination. The relevant upstream contracts are:

- [VST 3 Parameters and Automation](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Parameters%2BAutomation/Index.html),
  including stable parameter IDs, host automation recording, and block-local
  parameter queues;
- [CLAP parameter extension](https://github.com/free-audio/clap/blob/main/include/clap/ext/params.h)
  and [CLAP state extension](https://github.com/free-audio/clap/blob/main/include/clap/ext/state.h),
  which define typed host/plugin parameter events, thread roles, and project
  save/load streams;
- [Apple `AUParameterTree`](https://developer.apple.com/documentation/audiotoolbox/auparametertree)
  and [`implementorValueObserver`](https://developer.apple.com/documentation/audiotoolbox/auparameternode/implementorvalueobserver),
  through which a host discovers parameters and delivers external changes;
- [MIDI 2.0 core specifications](https://midi.org/midi-2-0), for cases where a
  performance controller is more appropriate than a project parameter mapping;
- [MTS-ESP's developer contract](https://oddsound.com/devs.php), which is an
  existing purpose-built producer/client mechanism for shared tuning, not a
  general scene protocol.

## Decision

Scene and macro coordination uses the host's existing parameter mapping,
automation, or controller facilities. A product exposes a small set of stable,
meaningful parameters—such as `Morph` or product-specific macros—and the user
or product host maps those controls to the intended plugin instances. A macro's
DSP interpretation must not rewrite other exported automatable parameters; a
host scene that recalls several controls maps those target parameters directly.
If a product owns its host, it may provide a host-side scene UI over the same
parameter contracts.

Pulp will not add a collaboration broker, peer-discovery API, shared scene bus,
or producer/consumer pilot for this use case. In particular:

- no product collaboration API is added to `inspect/`;
- no plugin receives another plugin's inspector token or calls its development
  RPC methods;
- no peer pointers, process-global registries, loopback sockets, JSON RPC, or
  background network service are introduced;
- no shared parameter or value-channel schema implies shared authority or a
  stable production transport.

This is a standards decision, not a promise that every DAW presents scene
mapping with identical UX. If a host lacks a convenient macro surface, the
fallback is its automation lanes, MIDI/controller mapping, or a product-owned
host. That portability variation does not justify putting a second project and
identity system inside Pulp plugins.

## Contract boundaries

### Identity and lifetime

The host project selects each target plugin instance. Pulp's durable identity is
the parameter ID within that plugin's versioned parameter set; it is not a
globally unique plugin-instance identity. Duplication, replacement, track moves,
sandbox restarts, and project reopen remain host operations. No plugin stores a
pointer, process ID, socket address, inspector discovery record, or token for a
peer. There is no peer negotiation: the host discovers each plugin's parameter
descriptors, and the user or product host chooses the mapping.

### State and project persistence

The host owns mappings, automation, and the association with plugin instances.
Each plugin owns only its local parameter values and optional plugin-owned scene
bank in its normal host state blob. Missing or replaced instances are resolved
by the host and must not leave hidden shared Pulp state. Parameter evolution
remains append-only so existing project mappings do not silently retarget.

### Real-time behavior

For VST3, AUv3, and CLAP, the audio thread consumes the bounded
`ParameterEventQueue` path supplied by the adapter. AUv2 and AAX remain
block-rate `StateStore` paths, and other formats retain their documented
delivery granularity; this decision adds neither sample-accuracy nor a new queue
to them. No format path discovers peers, allocates collaboration
messages, locks a registry, parses JSON, performs I/O, or waits for
acknowledgements. Hosts may order or sample-align automation only to the degree
their format path supports. Event size, rate, and overflow remain the existing
adapter contracts; this decision adds no second queue or unbounded payload. It
does **not** claim an atomic transaction across plugin instances. A use case
that requires a cross-instance all-or-nothing commit or tighter timing than the
host and selected adapter can provide is outside this contract and is a
possible reopen input, not an implicit Pulp guarantee.

### Security and authority

Only parameters deliberately exposed by each plugin are controllable, through
authority the user has already granted to the host project. This design adds no
discovery surface, ambient same-user trust, credential, filesystem access,
shell access, or cross-sandbox channel. The development inspector remains a
privileged debugging control plane and is never a plugin identity or production
authority mechanism.

## Alternatives considered

| Mechanism | Result |
| --- | --- |
| Host parameter mappings and automation | **Selected.** They match the scene/macro payload, project ownership, format adapters, and real-time delivery path already present. |
| MIDI or MIDI 2.0 control | Useful when the producer is a performance controller; not required for host-owned scene recall and not a project-identity system. |
| Audio or sidechain buses | Appropriate for signals and envelopes, not discrete scene ownership or persisted mappings. |
| CLAP/vendor extension | Premature for a use case already represented by ordinary parameters, and would not cover VST3/AU hosts. |
| MTS-ESP | Use it for shared tuning. Its deliberately narrow tuning semantics are not a precedent for a general scene bus. |
| Inspector RPC reuse | Rejected. Debug authority and changing development schemas are broader and less stable than the product needs. |
| New Pulp collaboration service | Rejected for this use case. It would duplicate host identity, persistence, automation, and sandbox policy without a demonstrated standards gap. |

## Consequences

- Phase 9 requires no code pilot or conformance fixture: its design gate chose
  existing standards rather than a new Pulp contract.
- Plugin authors should expose stable semantic macros that do not rewrite other
  automation lanes, instead of attempting to locate peer instances.
- A product host may make scene mapping easier, but it should remain a host
  feature over standard parameter interfaces.
- Pulp does not claim universal cross-DAW scene UX, atomic cross-instance
  transactions, or plugin networking.
- Inspector schemas may describe the same parameter metadata for debugging,
  but transport, authorization, compatibility, and stability remain separate.

## Reopen criteria

Reconsider this NO-GO only through a new planning proposal that provides all of
the following evidence:

1. **Unserved product case:** one named producer/consumer workflow with measured
   requirements that cannot be met by host parameter mappings, automation,
   MIDI, audio/sidechain buses, CLAP or vendor extensions, or a purpose-built
   standard such as MTS-ESP. Host UX inconvenience alone is insufficient.
2. **Identity and lifetime:** an identity model with an explicit owner that
   survives plugin duplication, replacement, sandbox/process restart, missing
   peers, and project reopen without pointers, process IDs, ports, or inspector
   credentials.
3. **State ownership:** a versioned persistence and migration design identifying
   which component owns mappings and shared state, how partial restore behaves,
   and how old/new peers negotiate without silent data loss.
4. **Real-time bounds:** typed payloads with explicit maximum size and rate,
   thread ownership, preallocation, bounded queue behavior, overflow policy,
   cancellation, and proof that audio callbacks never lock, allocate, parse
   unbounded data, perform I/O, or wait.
5. **Security boundary:** explicit opt-in discovery, least-privilege permissions,
   user-visible activation, sandbox and out-of-process behavior, abuse limits,
   and a threat model that excludes inspector tokens and debug capabilities.
6. **Portability evidence:** a format/host matrix with real-host proof covering at
   least one in-process and one sandboxed or out-of-process path, plus a reason a
   host service, standard extension, or optional package cannot own the feature.
7. **Narrow pilot gate:** security and real-time reviews approve one versioned
   producer/consumer contract and its negative conformance tests before any
   implementation PR begins.

Until every item is present and approved, the decision remains NO-GO and no
collaboration transport or pilot belongs in Pulp core.
