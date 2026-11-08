# G-4 voice binding and persistence receipt

Date: 2026-11-06. Status: **bounded source evidence; G-4/G-6 runtime admission remains HOLD**.

The implementation follows the published G-4 → G-6 admission packet
`g4-g6-binding-sustain-admission-20261106.md` (planning head `c9af7250d`) and
uses only the public value-owned schema in
`pulp/host/voice_template.hpp`. The worktree source baseline is live Pulp `origin/main`
`a186d1c53b1860ea39907800ce3006041bfbd68a` (the requested short SHA
`ea1e029b` was not present in the fetched repository).

## Evidence surface

`prepare_voice_bank` accepts a reviewed template with parameter and stereo-port
bindings and produces sixteen records (two declarations × eight slots) with a
stable manifest digest. Reordering declarations preserves the digest. The
persistence control saves a finite per-slot parameter state, emits a tagged
length-delimited receipt with SHA-256, restores it transactionally, and uses a
retained prior state as an undo record.

The planted negatives refuse duplicate declarations, NFC aliases, cross-slot
mutable declarations, mismatched event rate/type, stale manifest migration,
and non-finite parameter values. Failed restore leaves the live state unchanged.

Focused test: `pulp-test-voice-template` (registered in
`test/cmake/sampler_runtime_tests.cmake`). Run with:

```text
ctest --test-dir build -R '^pulp-test-voice-template$' --output-on-failure
```

This receipt is source and in-tree public-header evidence. It does not claim an
installed-SDK consumer, graph voice rendering, host MIDI/MPE ingress, sustain,
epoch guards, realtime bounds, or the A1 output/tail oracle. Those prerequisites
remain open under the published admission packet.
