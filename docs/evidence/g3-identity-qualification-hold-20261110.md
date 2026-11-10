# G3 identity qualification HOLD — 2026-11-10

## Disposition

`PARTIAL / HOLD`. This is an evidence-only qualification update. It does not
add a public fingerprint API, change prepared-plan execution, or claim G-3
closure.

## Exact source

- Source checkout: `origin/main` at `570a0a85d940a925420d5c1ac3c378ab3008bd92`
- Worker branch: `codex/g3-identity-retry2-20261110`
- Scope: current source and installed-consumer evidence already present on
  main; no runtime or CI changes.

## Verified controls

The existing value-owned authored identity vectors were run directly from the
source tree:

```text
python3 test/test_g3_io_gain_identity.py
Ran 10 tests in 0.060s
OK
```

These controls cover the frozen empty and closed I/O + Gain vectors, digest
stability under object-member reorder, gain mutation, duplicate JSON keys,
legacy-shape refusal, malformed float/non-finite refusal, channel mismatch,
path-degree refusal, and a 1,100-node bounded path.

The public prepared-description D4 consumer and its focused fourteen-control
receipt are already described in `docs/graph-program-status/G-3.md`. This audit
does not rerun that governed build because the current requirement is to
separate the existing D4 receipt from the missing broader qualification.

## Remaining G3 requirements

The current source proves a control-side authored identity contract and a
focused closed-profile prepared export. It does not prove:

1. one public whole-graph semantic fingerprint covering the broader graph
   vocabulary and its resolved prepared execution plan;
2. a final-head installed consumer that binds that broader fingerprint to the
   prepared runtime rather than only the closed D4 profile;
3. realtime safety, allocation/lock/I/O behavior, portability, or undo/state
   migration for identity changes;
4. a reviewed compatibility policy for hosted/custom nodes and unresolved
   identities.

The existing G3 status and A9 replay evidence correctly keep those claims
`PARTIAL`/`HOLD`; this document preserves that boundary. The next safe action
is a separately reviewed contract for whole-graph/resolved-plan identity,
followed by an installed consumer and independent planted-negative controls.

## Evidence limits

No source or runtime implementation was changed. No claim is made about
hard-realtime behavior, arbitrary nodes, plugin hosting, cross-platform parity,
or graph mutation semantics.
