# G-12 flagship qualification readiness audit — 2026-11-06

**Disposition: OPEN / PARTIAL.** This is a current-head admission artifact. It
does not complete G-12, promote a hard-realtime claim, or waive the flagship or
independent-review gates.

## Exact source and evidence boundary

The audited Pulp source is `origin/main` at
`2f3d36dae75c3dca283c58e47f7731356f088e0d` (the merge for PR #11703). All
claims below are bound to this exact source unless a historical source is
named explicitly. The existing closeout matrix
(`docs/reports/g12-closeout-20261106.md`) predates this head and must not be
read as a current-head receipt.

The roadmap gate still requires compile and activation cost, memory and reuse,
scheduling overhead and jitter, a Release 44.1/48/96 kHz by 32/64/128/256
callback matrix, no callback allocation/lock/wait/I/O, p99 and jitter limits,
100,000 paced callbacks with no missed deadlines, WCET and memory receipts,
deterministic output controls, and permanent X9 plus A1/A2/A3/A9/A10 fixtures
with independent final review. Local Release output and general CI status do
not satisfy those gates.

## Current evidence readback

| Lane | Current evidence | Admission state |
| --- | --- | --- |
| R2 graph performance | `tools/graph_performance_qualification.py` and its focused tests are present. `docs/reports/g12-r2-qualification-20261106` contains a complete 40-cell receipt and `controls.txt` records positive validation plus five planted mutation failures. The receipt source is historical `7a63d4d99dc04cc651a3c47bdc974263b93c13e6`, not this head. | **PARTIAL, historical receipt.** Do not rebind it to current main or use it as callback, WCET, cross-platform, fleet, hosted, or flagship proof. A fresh exact-head R2 receipt is optional follow-up work, separate from the flagship lanes. |
| X9 permanent fixture | `docs/evidence/x9-flagship-audit-20261106.md` proves a bounded in-tree fixture: five generated cases, live/offline equality, MIDI/modulation, state, reusable gain components, and prepared mutation controls. Hosted CLAP/VST3/AU artifacts were unavailable. | **PASS (bounded local fixture only).** It is not an installed SDK consumer and does not prove allocator policy, MPE, latency, event-log replay, cross-platform behavior, or hard realtime. |
| A1 expressive eight-voice instrument | G-6 status and `docs/evidence/g6-polyphony-implementation-hold-20261106.md` keep the allocator consumer as a contract characterization. G-4 identity and G-5 event contracts do not yet bind graph Processor voices. | **OPEN / HOLD.** The safe successor is one reviewed G-5 sidecar-to-Processor path, then a fixed eight-voice external consumer with steal, tails, MPE, partition, persistence, and planted routing/timing/state negatives. |
| A2 modular performance instrument | X9 supplies bounded in-tree MIDI/modulation/state/edit/render evidence. No clean external A2 notation/consumer receipt combines parameterized subgraphs, instance state, shared controls, nonlinear time, save/restore/mutate, and independent failures. | **PARTIAL.** Keep the X9 refusal controls planted; add the external consumer only after event and graph-Processor state contracts are admitted. |
| A3 spectral morph instrument | G-10 routed acceptance remains explicitly PARTIAL. Its installed consumer and many production mutations cover bounded routed spectral behavior, but notation, analyzed two-asset morph/resynthesis, live modulation/time-index, and the roadmap callback matrix remain open. | **PARTIAL.** Continue the named missing G-10 controls and keep A3 separate from the R2 receipt. |
| A9 agent design loop | `examples/graph-agent-loop` is a bounded installed-SDK consumer for catalog → notation → validate → render → probe/compare → one edit → saved-input replay. Its status names missing model/tool pinning, action-log schema, network denial, CLI/MCP parity, semantic diff, and full mutation harness. | **PARTIAL.** This is the clearest dependency-safe parallel lane: add the action-plan and mutation-control harness around the existing consumer, with ambient plugin/network denial and a topology diff. It does not require A1 voice runtime or A10 hosted containment. |
| A10 routing/hosting environment | `docs/graph-program-status/A10-routing-hosting.md` and `docs/evidence/a10-routing-hosting-api-audit-20261106.md` record a public API map but an unavailable exact-head installed SDK at their older audited head. G-9 hostile containment remains HOLD. | **BLOCKED / HOLD.** Re-run SDK installation/provenance from a clean exact-`2f3d36d…` checkout before writing an external consumer. The later consumer needs sidechain, automation, PDC, transactional edit, hosted-node, and planted refusal controls; no arbitrary-host or descendant-cleanup claim is admissible yet. |
| Realtime, fleet, cross-platform, final review | Existing receipts classify hosted RTSan, fleet, browser/native parity, and cross-platform lanes as N/A or PARTIAL. | **N/A / PARTIAL.** No inference from local arm64 results or green general checks. |

## Dependency-safe work that can proceed in parallel

1. **A9 harness (admissible now):** extend the existing installed consumer with
   a pinned declarative action log, a topology semantic fingerprint/diff, an
   explicit network and ambient-plugin denial control, and independent wrong-
   edit / wrong-replay mutations. Keep the receipt bounded to the public
   consumer and state that A10, realtime, and arbitrary plugin claims remain
   out of scope.
2. **A10 SDK/provenance preparation (readiness only):** from a clean exact-head
   checkout, install the forge-dev SDK, capture source/archive/header hashes,
   and stop if the installer or exact-head provenance is unavailable. Do not
   substitute the stale SDK named by the current A10 report.
3. **A3 control completion (separate lane):** close the outstanding G-10
   production mutations and notation/catalog obligations listed by the routed
   acceptance audit. Keep analyzed-asset, morph, modulation, and callback
   receipts separate from R2 and X9.
4. **A1/A2 admission planning (no runtime implementation yet):** name owners
   and exact consumer fixtures for the G-5 sidecar-to-Processor binding,
   G-4 voice identity, graph state restore, and external A2 notation path.
   This is planning and evidence preparation; adding a second voice or event
   runtime before those dependencies are reviewed would be unsafe duplication.

## Why this audit does not add a fixture

The only bounded fixture already justified by the current graph program is the
R2 receipt wrapper, and it is merged with positive and planted-negative
controls. Its checked-in receipt is tied to `7a63d4d…`; running it again on
`2f3d36d…` would produce another bounded PARTIAL receipt, not a flagship or
callback qualification. X9 is already covered by a bounded audit, while A1,
A2, A3, and A10 each lack an admitted dependency or external consumer seam.
Adding a new in-tree fixture now would either duplicate those lanes or infer
completion from unavailable hosted/realtime evidence. This status artifact is
therefore the smallest safe current-head action.

## Admission rules for the next source PRs

- Bind every receipt to the full source SHA, executable or SDK hashes, build
  profile, and raw-output hashes; never silently rebind the historical R2 run.
- Require one positive control and an independently planted negative for each
  claimed quality rule, with a restored-green run after mutation.
- Keep A1/A2/A3/A9/A10 receipts distinct; an R2 or X9 pass cannot upgrade any
  flagship row.
- Leave G-12 **OPEN / PARTIAL** until the Release callback matrix, WCET/memory,
  cross-platform/hosted boundaries, all flagship fixtures, and independent
  final review are present at one exact head.
