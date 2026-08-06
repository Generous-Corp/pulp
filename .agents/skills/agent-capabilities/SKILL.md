---
name: agent-capabilities
description: Maintain Pulp's installed design-time agent capability manifest and public-surface ledger. Use when adding, removing, renaming, or materially changing public audio, MIDI, signal, timebase, or sequence APIs; registering a new algorithm for generators; changing capability support or deprecation state; or repairing agent-capabilities freshness, schema, fingerprint, tombstone, or installed-SDK tests.
---

# Agent Capabilities

Maintain two related artifacts:

- `agent-capabilities.json` is the installed consumer contract: curated keys,
  versions, digests, status, evolution, typed C++ bindings, and partial-coverage
  semantics.
- `agent-capability-surface.json` is the maintenance ledger: every public header
  in the covered roots, its byte fingerprint, and its reviewed disposition.

Keep both separate from the unified runtime control platform. This contract may
describe what an SDK can design or generate; it must never contain runtime
operations, grants, policy, risk decisions, instances, activation, sessions,
revocation, or receipts.

## Classify the change

For a new public header or symbol:

1. Decide whether it is a generator-facing capability. Do not infer this from a
   plausible class name.
2. If yes, add an explicit row to `EXPORTS` in
   `tools/scripts/agent_capability_manifest.py`, add typed bindings for every
   advertised entrypoint/operation, and record the current header fingerprint.
   Start a new key at contract version `1.0`.
3. If no, add a reviewed header classification instead:
   `capability_support`, `infrastructure`, or `unsupported_capability`. Give a
   durable rationale. Never grow the frozen `legacy_unreviewed` baseline.
4. Increase `SURFACE_INVENTORY_VERSION` for any ledger change. Increase
   `MANIFEST_REVISION` whenever the installed manifest changes.

A new TSP algorithm is therefore detected automatically but not advertised by
guesswork: the new/changed public header fails the ledger gate until its owner
makes the explicit registration or non-capability classification.

For an existing capability change:

- Update the reviewed header fingerprint for every public-header byte change,
  even when the consumer contract is unchanged. Increase the surface inventory
  version.
- Increase the capability minor version for compatible additive contract
  changes.
- Increase the capability major version when a binding is removed, renamed, or
  replaced, or when lifecycle, RT, state, seed, domain, units, latency, tail, or
  scheduling semantics change incompatibly.
- Leave the capability version unchanged for summary-only wording. The generated
  digest excludes the summary but covers the material contract.
- Keep numeric parameter ranges/defaults/choices in `forge-catalog.json`; use a
  `forge_descriptor` reference instead of copying them.

For removal:

1. Add a capability tombstone with the last version and digest before deleting
   the row. Never reuse a tombstoned key.
2. Add a surface tombstone with the last header fingerprint before deleting a
   covered public header.
3. Increase the manifest and surface inventory revisions as applicable.
4. Preserve tombstones permanently; do not rewrite history.

## Interpret support honestly

- A live capability row's `status` is the explicit support claim. Use only
  `stable`, `usable`, `experimental`, `partial`, or `unsupported`; never publish
  planned work.
- A header classified `unsupported_capability` is an explicit negative claim.
- `legacy_unreviewed` means only that no machine-readable claim has been
  reviewed yet.
- Coverage is currently `partial`, so an absent key means unknown, not
  unsupported.

## Regenerate and validate

Do not use the bootstrap or unpublished-migration switches during normal work.
Run:

```bash
python3 tools/scripts/agent_capability_manifest.py --write
python3 tools/scripts/agent_capability_manifest.py --check
python3 tools/scripts/test_agent_capability_manifest.py
cmake --build build --config Release --target pulp-test-agent-capability-compile
ctest --test-dir build -C Release -R '^agent-capability-' --output-on-failure
```

The installed-SDK test must install to an isolated prefix, read the installed
schema/manifest/surface files, compile every typed binding, link every advertised
target, and reject checkout-path leakage.

The surface fingerprint is intentionally conservative SHA-256 over full header
bytes. Do not weaken it with regex symbol extraction. A future pinned-Clang AST
inventory may reduce comment/private-detail churn only if its version and
toolchain are pinned and mutation tests retain add/remove/change detection.
