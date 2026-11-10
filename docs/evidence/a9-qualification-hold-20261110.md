# A9 qualification hold — current-head audit

Date: 2026-11-10  
Audited source: `origin/main` at `6bc132fcb7f4f742ee4ea78f50c8a85a4eca95a3`  
Disposition: **PARTIAL / HOLD**

## Verified capability

The merged A9 material provides two bounded installed-SDK consumers using the
public `Pulp::host` surface:

* `examples/graph-agent-loop` validates a pinned offline action plan, catalog
  discovery, notation save/load, three-block render, an audio-edge probe, one
  gain edit, and saved-input replay. Its positive receipt records `258`
  checks and its three independent mutation plans reject network opt-in,
  ambient-plugin opt-in, and a missing action.
* `examples/graph-agent-loop-replay` validates a stereo Input → Gain → Gain →
  Output edit/replay profile. Its receipt records `89` checks, an edited plan
  hash distinct from the baseline, and a replay hash equal to the baseline;
  wrong-edit and wrong-replay controls fail closed.

These are useful public-consumer and fail-closed action-plan controls. They do
not establish a general agent execution environment or a complete graph
runtime.

## Evidence provenance and limits

The checked-in A9 status names the action-plan qualification as sourced from
`3fad8b42a44d496361f97eeb469639e3a2b9e3c6`, while the replay receipt names an
older source `cc6430e25529e3c3f511c3f33178975ac58fc05d`. The replay document
records a later non-rebased merge and says its current-main install stopped at
the missing optional `inspect/pulp-control-broker` artifact; it therefore does
not provide exact-current installed-consumer evidence for this audited head.
The action-plan receipt is explicitly non-distributable source-install
evidence (`distribution_eligible=false`).

No claim below is inferred from the positive consumer runs. A clean exact-head
rerun would need a fresh installed-prefix provenance marker, executable hash,
raw action log, and restored-positive plus planted-negative outputs.

## Requirements not proven

The following roadmap requirements remain **NOT CHECKED / HOLD**:

| Requirement | Why the current evidence is insufficient |
| --- | --- |
| Model execution and pinned model/tool behavior | The action plan validates a deterministic offline plan and digest fields; it does not execute or qualify a model runtime. |
| CLI and MCP parity | Both consumers call the C++ public host API directly; no CLI/MCP invocation receipt exists. |
| Arbitrary node families or hosted plugins | Plugin loading is disabled and the graph profile is fixed to built-in Input/Gain/Output nodes. |
| Whole-graph identity and semantic diff | The action-plan fingerprint is topology-only; the replay fixture covers one fixed topology and parameter edit. |
| Realtime safety and callback qualification | No callback-thread, allocation, lock, I/O, WCET, deadline, or long-run receipt is present. |
| Browser/native or cross-platform parity | The receipts are macOS installed-prefix runs and contain no browser, Linux, or Windows result. |
| A10 routing/containment | The consumers deliberately disable plugin loading and do not exercise hosted routing, isolation, recovery, or sibling continuity. |

## Safe next action

Do not add another graph runtime or broaden the existing consumer's claims.
The dependency-safe follow-up is one exact-current installed-prefix rerun of
the existing A9 consumer, retaining its current mutation controls and adding
only the missing provenance/raw-output artifacts. CLI/MCP, model, arbitrary
plugin, realtime, and platform lanes should remain separate qualification
work until their owners provide reviewed contracts and fixtures. The existing
A9 status and G-12 audit remain the authoritative roadmap dispositions.

This packet is evidence-only; it does not modify the roadmap or implement any
runtime/API behavior.
