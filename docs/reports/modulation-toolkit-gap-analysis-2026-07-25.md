# Modulation toolkit gap analysis

Audited 2026-07-25 against:

- the normative requirements in `mod-utilities-pulp-prompt.md`;
- Pulp `e92a7fb2fad41532631594cfd0d382b0c4c7e8c2`;
- Forge `main` through `814cb151d273eb2744b325e9176ae0287d76fd3f`
  (catalog PR #26 plus hardening PR #35).

The toolkit is functionally present: all ten requested headers ship, the optional
chaos milestone ships, the three required cross-cutting compositions plus the
starred cookbook patches are tested, every runtime type is in an allocation
probe, and Forge exposes the agreed five-node effect-lane pack. The gaps below
are the remaining differences from the prompt, not a second implementation
backlog.

## Missing items

### G1 — Double-alias parity is not proved

The prompt requires double-alias parity as a cross-cutting test. All
sample-valued primitives expose `Foo64`, but the tests exercise only the float
aliases except for a `ModMatrix64` trivial-copy assertion. A regression that
breaks a double specialization can therefore merge green.

Close by adding one parity suite that instantiates every `Foo64` surface, drives
the same deterministic inputs through float and double forms, and compares
events exactly and numeric outputs within precision-appropriate tolerances.

### G2 — Three precision-independent event types have no `Foo64` spelling

`ClockDividerT`, `ClockMultT`, and `TrigDelayT` contain only integer/event state,
so their implementation does not need a `SampleType`. They nevertheless omit
the house-convention `ClockDivider64`, `ClockMult64`, and `TrigDelay64` aliases
while the public reference says every toolkit type has a `Foo` / `Foo64` pair.

Close without dummy template parameters: add the three `*64` compatibility
aliases to the same precision-independent types and document why those pairs are
identical.

### G3 — Zero-initialized freshness is only spot-checked

The prompt requires zero-init to be valid across the toolkit. Current tests pin
that property for `Xorshift32` and `TrigDelayT`, and default construction is used
incidentally elsewhere, but there is no roster proving every public type can be
value-initialized, reset, and safely advanced from its fresh state.

Close with a table-style smoke test covering every public type. Assert finite,
bounded, or inactive fresh output as appropriate before and after `reset()`.

### G4 — LPG roll brightness is implied, not directly asserted

The normative LPG roll test requires each 30 ms re-strike to peak both louder
and brighter than a cold strike. The current roll test asserts increasing cell
control and output peak. Separate tests prove that cell control and brightness
co-vary, but the roll test never observes the successive cutoff/brightness
peaks itself.

Close by exposing read-only commanded-cutoff telemetry (or measuring spectral
centroid directly) and asserting monotonically increasing brightness for the
three-hit roll, followed by a recovered cold-hit baseline.

### G5 — The migration inventory is incomplete

The prompt requires the known duplication sites to be named for later migration
prompts. The reference currently names the private LFOs in `chorus.hpp` and
`phaser.hpp`, Forge's legacy four-shape LFO, and generic duplicated xorshifts.
It does not give the requested concrete inventory for:

- xorshift implementations in `lofi_chain.hpp`, `noise_source.hpp`,
  `character_delay/primitives.hpp`, `drum/clap.hpp`, and `fdn/modulation.hpp`
  (plus the separate xorshift64 family in spectral code);
- the OU/walk lineage in `fdn/modulation.hpp` and
  `character_delay/primitives.hpp`;
- the drum low-pass gate implemented in `lowpass_gate.hpp`, consumed directly
  by `drum/membrane.hpp`;
- the FDN transient ducker in `fdn/stages.hpp`.

Close by making this an exact file-level inventory and retaining the existing
warning that migrations need compatibility tests rather than silent swaps.

## Forge exposure boundary

Forge's `mod_lfo` node is a zero-input free-running source. It exposes rate,
depth, wave, pulse width, random blend, delay, and fade-in, but no trigger/note
input. Consequently its delay/fade lifecycle is graph-startup behavior, not
per-note delayed vibrato. PR #35 corrected the generation prompt so it no longer
claims otherwise.

This is not a missing `LfoT` feature: the Pulp primitive already exposes
`Mode::retrig` and `retrigger()`. Treat per-note retrigger in Forge as a separate
graph/event-lane design decision. Do not add a fake effect-lane claim or conflate
it with G1–G5.

## Acceptance

- G1–G5 are closed in Pulp with focused tests and documentation.
- The existing modulation, event, voice/composition, RT-safety, and Forge
  catalog suites remain green.
- An adversarial branch review reports no actionable findings.
- Any Forge per-note retrigger work begins only from an explicit event-lane
  design, not by overloading graph startup.

## Resolution

Closed on `feature/mod-toolkit-gap-closure-20260725`:

- G1/G3: float/double fresh-state parity rosters now drive every public
  sample-valued alias and every precision-independent event type.
- G2: the three event-only `*64` compatibility aliases now ship and are tested.
- G4: `LpgT::cutoff_hz()` exposes read-only commanded-cutoff telemetry, and the
  roll test proves each re-strike is brighter before proving cold recovery.
- G5: the reference now carries the exact file-level migration inventory and
  compatibility boundary.

Focused validation: 144 test cases and 1,790,769 assertions across the
modulation scalar, event, voice/composition, RT-safety, and Forge catalog
suites.
