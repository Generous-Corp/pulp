---
name: agent-capabilities
description: Maintain Pulp's installed design-time agent capability manifest and public-surface ledger. Use when adding, removing, renaming, or materially changing public audio, MIDI, signal, timebase, or sequence APIs; registering a new algorithm for generators; changing capability support or deprecation state; or repairing agent-capabilities freshness, schema, fingerprint, tombstone, or installed-SDK tests.
---

# Agent Capabilities

Maintain three related artifacts:

- `agent-capabilities.json` is the installed consumer contract: curated keys,
  versions, digests, status, evolution, typed C++ bindings, and partial-coverage
  semantics.
- `agent-capability-surface.json` is the maintenance ledger: every public header
  in the covered roots, its byte fingerprint, and its reviewed disposition.
- `tools/agent-capabilities/contract-history.json` is the repository-only,
  append-only evolution history checked against the protected Git tip. Shallow
  GitHub Actions checkouts fetch the immutable event base SHA when necessary.

The consumer manifest, its schema, and the handoff schema install into the SDK.
Official release packaging stamps `agent-capability-handoff.json` only after
installation; it binds the exact SDK source SHA and platform to the installed
importer's SHA-256 plus the installed manifest's exact content and byte hash.
The release archive verifier must require and revalidate that identity at the
configured capability-handoff floor. The surface ledger, surface schema, legacy
baseline, and contract history are maintenance artifacts and must not be
installed.

When adding an installed SDK library in `PulpInstallRules.cmake`, register its
archive stem in `release_product_matrix.json` and classify every newly covered
public header here in the same change. A successful CMake export alone does not
prove the release archive or installed agent-capability contract is complete.

Keep both separate from the unified runtime control platform. This contract may
describe what an SDK can design or generate; it must never contain runtime
operations, grants, policy, risk decisions, instances, activation, sessions,
revocation, or receipts.

The installed SDK also ships canonical runtime-control headers, CMake helpers,
and control-authoring examples. Their presence in the same install tree does
not make them agent-capability rows: keep runtime-control operations and policy
out of `agent-capabilities.json`, and keep the capability surface ledger focused
on the design-time public-header contract.

## Classify the change

For a new public header or symbol:

1. Decide whether it is a generator-facing capability. Do not infer this from a
   plausible class name.
2. If yes, add an explicit row to `EXPORTS` in the domain-appropriate
   `tools/scripts/agent_capability_catalog_*.py` module, add typed bindings for
   every advertised entrypoint/operation, and record the current header
   fingerprint. Add a nonempty `_link_probe` that constructs or invokes the
   real API rather than merely taking `sizeof`, and start a new key at contract
   version `1.0`. `agent_capability_manifest.py` assembles those catalogs; do
   not put capability rows back into that orchestrator.
3. If no, add a reviewed header classification to
   `tools/scripts/agent_capability_registry.py` instead:
   `capability_support`, `infrastructure`, or `unsupported_capability`. Give a
   durable rationale. Never grow the frozen `legacy_unreviewed` baseline.
4. Increase `SURFACE_INVENTORY_VERSION` for any ledger change. Increase
   `MANIFEST_REVISION` whenever the installed manifest changes.

For overloaded C++ free functions, keep the public `qualified_name` as the real
API name and give each overload a distinct binding role. Use private generator
metadata for an explicit `static_cast` address expression so the generated
compile fixture proves the intended signature without leaking fixture syntax
into the installed contract. Each overload still needs its own operational
probe with arguments that select and invoke that overload.

For a non-static member-function binding, keep the public `qualified_name` as
the real class-qualified method, provide an exact pointer-to-member
`address_expression`, and use an explicit-object `member_function_call` probe.
The generator must retain address references with `auto volatile`; `auto *`
cannot represent a pointer-to-member. The installed-SDK suite runs the matching
operational probe independently for every binding, not only in the aggregate
capability consumer.

A new TSP algorithm is therefore detected automatically but not advertised by
guesswork: the new/changed public header fails the ledger gate until its owner
makes the explicit registration or non-capability classification.

For bounded sampled-target FIR design, register the public
`pulp/signal/fir_design.hpp` entry point as `signal.fir-design`, keep it
offline-only, and have the generated compile fixture invoke an empty-target
request in addition to taking the exact function pointer. This preserves the
contract's proof that the published binding is operational rather than merely
type-visible.

For a fixed-capacity record algebra such as `music.pattern-development`, bind
the stable record, error, configuration, and result types as well as every
advertised free function. Each free function needs its own operational probe;
a type-only row or one aggregate probe cannot establish that installed
consumers can execute density, fill, set-algebra, ID, and morph operations.
Keep scheduling, clocks, note ownership, and publication outside this manifest.

For an existing capability change:

- Update the reviewed header fingerprint for every public-header byte change,
  even when the consumer contract is unchanged. Increase the surface inventory
  version.
- Increase the capability minor version for compatible additive contract
  changes.
- Increase the capability major version when a binding is removed, renamed, or
  replaced, or when lifecycle, RT, state, seed, domain, units, latency, tail, or
  scheduling semantics change incompatibly.
- Keep `seed_model` and `determinism` separate. When
  `determinism-contract-v1` is required, every live row must declare
  repeatability, block-partition behavior, platform scope, and whether transport
  history is an input. Strengthening a determinism promise is additive;
  weakening or removing one requires a major increase or a new successor key.
- Treat a minor-0 row with no `determinism` as unspecified. Consumers that
  require determinism must reject it and must reject unknown required features.
- Leave the capability version unchanged for summary-only wording. The generated
  digest excludes the summary but covers the material contract.
- Keep numeric parameter ranges/defaults/choices in `forge-catalog.json`; use a
  `forge_descriptor` reference instead of copying them.

For removal:

1. First publish the live capability as `status: deprecated` with a matching
   deprecated evolution state and ordered lifecycle versions. A capability that
   is active in the protected base may not be removed in the current change.
2. In a later published revision, add a `status: removed` capability tombstone
   with its introduction/deprecation versions, last version, and digest before
   deleting the row. Never reuse a tombstoned key.
3. Add a surface tombstone with the immediately prior reviewed header
   fingerprint before deleting a
   covered public header.
4. Increase the manifest and surface inventory revisions as applicable.
5. Preserve tombstones permanently; do not rewrite history. Replacement keys
   must exist, may not reference themselves, and may not form cycles.

## Interpret support honestly

The published capability table now includes bounded, versioned registered clip
content and trusted note-renderer hooks. Keep that row aligned with the compile
contract: note output/reset state only, a 4096-note fragment cap, and explicit
refusal of trimmed nesting and nondefault-production wire serialization.

- A live capability row's `status` is the explicit support claim. Use only
  `stable`, `usable`, `experimental`, `partial`, `unsupported`, or `deprecated`;
  never publish planned work.
- A header classified `unsupported_capability` is an explicit negative claim.
- `legacy_unreviewed` means only that no machine-readable claim has been
  reviewed yet.
- Coverage is currently `partial`, so an absent key means unknown, not
  unsupported.

## Reuse the spectral-mask processor

`signal.spectral-mask-processor` is the shared streaming STFT/WOLA layer for
products that apply authored spectral gain tables. Installed-SDK consumers
include `<pulp/signal/spectral_mask_processor.hpp>` and link `Pulp::signal`.
Prepare it off the audio thread, publish layouts or compiled tables from a
control thread, and call `process()` or `process_frame()` on the audio thread.
The processor owns frame-boundary table adoption, gain interpolation,
overlap-add reconstruction, latency reporting, and latency-aligned dry/wet.

Use categorical mask entries for true mute: a muted bin is multiplied by exact
zero, not represented by a finite decibel floor. Reuse this processor for
zoomable filter banks, spectral gates, freezes, morphing, and related products
instead of rebuilding an application-local STFT lifecycle or publication
protocol. Analyzer snapshots and captured-frame storage are separate layers;
do not infer them from this capability or duplicate them inside the processor.

Before presenting every authored band as independently controllable, call
`analyze_spectral_band_resolution()` with the product's layout, sample rate,
and FFT size. Its fixed-capacity report counts directly owned viewport bins per
band and excludes exterior edge-band extension. `fully_represented() == false`
means the UI or profile selector must disclose the resolution limit, select a
higher supported geometry, or use a different filter architecture; zoom alone
cannot create additional FFT bins.

## Reuse the realtime visualization bridge

`pulp::view::VisualizationBridge` is the shared realtime-safe audio-to-UI tap
for spectrum, waveform, and meter consumers. Configure it while fully
quiescent, call `process()` from the audio callback, and give exactly one UI
thread ownership of `poll()` plus the snapshot reads. The callback path only
meters and copies into fixed SPSC storage; FFT and waveform assembly happen in
the bounded, non-realtime `poll()` call.

`read_spectrum()` and `read_waveform()` remain cheap snapshot reads for source
compatibility. They do not analyze newly captured audio. A consumer that needs
fresh data must schedule `poll()` first, then read or use the explicit
`peek_spectrum()` / `peek_waveform()` aliases. Treat capture overflow, rejected
channel topology, and positive-length missing-channel callbacks as continuity
breaks: the bridge advances its epoch and never joins audio across the gap.
Keep `configure()` and `reset()` quiescent; neither is concurrent with the
audio producer or UI consumer.

## Regenerate and validate

Do not use the bootstrap or unpublished-migration switches during normal work.
When the base branch has already advanced either counter, recompute the next
`MANIFEST_REVISION` and `SURFACE_INVENTORY_VERSION` from that exact base before
running `--write`; replaying stale projection counters can silently reuse an
already-published contract identity.
Use Python 3.10 or newer: transactional generation uses modern standard-library
APIs such as `zip(..., strict=True)`, so an older system `python3` can fail before
validating the contract. Run installed-SDK capability tests from a Release build.
A Debug/coverage build is an invalid positive control because the installed SDK
guard intentionally refuses unacknowledged Debug SDK consumers. Also keep the
build tree free of stale nested SDK install prefixes: archive-mutation checks
require one owning build-tree library per target, and an old consumer-smoke
`prefix/lib` can create a false duplicate-owner failure. Move such generated
fixtures aside and rerun the same test before changing capability code or
weakening the archive check.

Run:

```bash
python3 tools/scripts/agent_capability_manifest.py --write
python3 tools/scripts/agent_capability_manifest.py --check
python3 tools/scripts/test_agent_capability_manifest.py
cmake --build build --config Release --target pulp-test-agent-capability-compile
ctest --test-dir build -C Release -R '^agent-capability-' --output-on-failure
```

The installed-SDK test must install to an isolated prefix, verify maintenance
artifacts are absent, read the installed schema and manifest, and independently
compile/link/run every capability and every typed binding against only its
declared minimal target. It must reject wrong-target declarations and checkout-
path leakage, and use configuration-aware build/install and executable paths.
When checking CMake File API include paths, permit paths outside the install
prefix only when CMake marks them as system includes; transitive platform and
third-party headers may be legitimate, but non-system source/build leakage is
still a failure. When finding build-tree archives for mutation controls, exclude
the staged install prefix because retrying the test leaves installed archives
under the build directory.
The official-SDK handoff self-test separately covers exact identity plus wrong
source SHA, importer hash, capability hash, and schema-invalid documents.

`test/cmake/quality_tests.cmake` is also the registry for capability-manifest
and adjacent policy self-tests. When adding a new Python policy test there,
register the test explicitly in the same change; merely creating a
`tools/ci/test_*.py` file does not make CTest execute it.

The surface fingerprint is intentionally conservative SHA-256 over full header
bytes. Do not weaken it with regex symbol extraction. A future pinned-Clang AST
inventory may reduce comment/private-detail churn only if its version and
toolchain are pinned and mutation tests retain add/remove/change detection.

Because the fingerprint covers full bytes, **editing only comments in a
capability header is a surface change** and `--write` will refuse it twice
before it succeeds. The declared fingerprint lives in the catalog source, not
just the generated JSON, so the order is: edit the header, then replace every
occurrence of the old digest in the owning `agent_capability_catalog_*.py`
(one per binding, so a single header can hold a dozen copies), then raise
`MANIFEST_REVISION` and `SURFACE_INVENTORY_VERSION`, then run `--write` once.

Derive both counters from the CURRENT protected base every time. A capability
transaction can land while yours waits in the merge queue, which takes the
numbers you reserved and leaves your branch conflicting on exactly those two
constant lines. Re-read them from main and regenerate rather than resolving
that conflict by hand.

Regenerate exactly once from final header bytes. Each `--write` appends a full
entry to `contract-history.json`, so editing the header again after a
successful `--write` leaves two entries for one logical change.
