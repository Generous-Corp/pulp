# G-3 identity and prepared-export qualification HOLD — 2026-11-10

Status: **HOLD / no new implementation admitted**

## Scope and source

This audit was run in a fresh classified worktree at exact `origin/main`
`570a0a85d940a925420d5c1ac3c378ab3008bd92`. It is an evidence and planning
packet only. It does not add a fingerprint format, runtime mutation, or a new
prepared-plan API.

## Verified current surface

The current tree contains the control-side authored-port identity contract in
`docs/contracts/g3-identity-contract.md` and the implementation in
`core/host/include/pulp/host/graph_identity.hpp` plus
`core/host/src/graph_identity.cpp`. The implementation normalizes authored
names, validates aliases and encoding, and exposes a 64-bit
`port_identity_fingerprint`. The existing identity tests cover reorder and
alias positives plus duplicate, missing, empty, invalid-encoding, and
non-finite-value refusals. This is a value-owned control identity, not a
whole-graph execution identity.

The current tree also contains the D4 prepared-description surface in
`core/host/include/pulp/host/prepared_description.hpp` and
`examples/g3-d4-prepared-export-consumer`. The focused consumer exports a
closed stereo input → two Gain nodes → output profile, canonical bytes, a
content hash, serial execution facts, generation and pool identity, and typed
refusals. `test/test_prepared_description.cpp` provides the positive and
planted-negative controls for that bounded profile.

## Qualification boundary

The verified surface does **not** prove the broader G-3 requirement:

* no resolved whole-graph identity/fingerprint packet is admitted for arbitrary
  node families, event lanes, feedback, multirate domains, or dynamic topology;
* no public contract binds a prepared export to an audio `Processor` voice
  lifecycle or render/tail oracle;
* no exact-current installed consumer covers a broader graph than the D4
  closed serial I/O + Gain profile;
* realtime safety, allocation/lock/I/O behavior, undo/state migration,
  cross-platform parity, and hosted-plugin identity remain unverified;
* the existing 64-bit port fingerprint must not be promoted to a portable
  whole-graph content identity without a reviewed schema, versioning and
  collision policy.

These are missing evidence and contracts, not failures of the bounded D4
fixture. The appropriate disposition is HOLD until a planning owner admits the
next identity contract and its independent consumer/oracle.

## Required next experiment

Define and review a versioned resolved-graph identity schema first. Its fixture
must include at least two node families, event and audio edges, a reorder that
preserves authored identity, a declared alias, a changed name without an alias,
and an independent planted-negative for every refusal. The receipt must bind
the exact source head, SDK, canonical bytes, executable, and consumer hashes.
Only after that contract is admitted should implementation proceed; do not
infer whole-graph, realtime, portability, or voice claims from the current D4
consumer.

## Evidence classification

* **Verified:** files and controls named above exist on the exact source head.
* **Reasoned:** the listed missing gates follow from the current API scope and
  the roadmap's G-3 requirements.
* **Speculative:** any broader fingerprint schema or runtime binding remains
  uncommitted and is intentionally excluded from this packet.
