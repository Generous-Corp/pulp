# A3 spectral save/replay workflow — 2026-11-10

Status: source-backed plan admitted by lead; implementation in progress.
Local governed Release persistence tests pass: 16,724 assertions in six cases.
Four actual production confirm_failure loops are confirmed with exact restoration.
Installed public consumer passed positive/corrupt/restored/missing with exits
0/42/0/42. All four production mutation receipts are 0/42/0. Final dependency
integration, exact-head gates/publication and qualification remain pending.

## Dependency and checkout provenance

- Functional runtime [PR 11998](https://github.com/Generous-Corp/pulp/pull/11998)
  merged from `0673ffadd14b7f0f6909c117adde1a991e4969a8` at
  `2026-11-10T19:03:00Z`; merge commit
  `aea67edf573c2b0d310b4e69434cab8c99942afd` is this branch's base.
- Notation [PR 11999](https://github.com/Generous-Corp/pulp/pull/11999) was open
  at audited exact head `82978bda7431325d6abae6a25de3b135cb06e45a`.
  Its changes must land through that PR, without duplicate publication here.
- Fresh classified active branch: `codex/a3-spectral-replay-20261110`.
  The prior functional handoff checkout and primary remain untouched.

## Source findings and proposed acceptance boundary

`core/signal/include/pulp/signal/spectral_frame_codec.hpp` defines canonical
profile-1 analyzed bins, without graph identity, epoch, or original source
length. `core/host/src/spectral_asset_morph_internal.hpp` already validates,
owns and hashes both streams and checks original length against frame count.
The replay workflow must reuse that factory and the existing runtime.

`core/host/src/graph_serializer.cpp` currently writes format 4, refuses any
spectral identity, omits spectral edge metadata, and clears the destination
before legacy migration/parsing. New spectral records therefore need bounded
preflight before that mutation boundary. Legacy tolerant loading remains a
separate contract.

At the exact G2 head above, `graph_patch.hpp/.cpp` provide one existing
`GraphPatchCatalog`, shared diagnostics, flow-1 declarations/named edges,
and strict loaders returning an isolated graph candidate. Spectral nodes,
spectral edges, and mono audio boundaries are explicitly unsupported there.

Proposed to lead: closed typed spectral catalog bindings; a versioned saved
envelope containing content-addressed canonical assets, explicit asset schema
and version, profile, original sample count and SHA256; typed stage/synthesis
records and spectral edge metadata. No new codec, callback registry, scheduler,
plugin coercion, or arbitrary filesystem asset resolver is proposed.

The proposed additive `GraphPatchCatalog` methods are
`bind_spectral_asset_morph(type_id, asset_a, asset_b, source_samples)`,
`bind_spectral_stage(type_id, identity, stage)`, and
`bind_spectral_synthesis(type_id, identity)`, returning existing shared
`GraphPatchDiagnostic` values. Instantiation delegates to existing typed
constructors. Formatting must match the immutable bound configuration, rather
than matching only port shape. Mono boundary entries project existing one-channel
constructors. Flow-1 declaration and named-edge syntax stay unchanged.

Proposed implementation surfaces are a focused spectral persistence helper,
narrow serializer hooks, the existing G2 catalog/parser/formatter/strict loader,
focused tests/fixtures, an installed public consumer and its CMake gate, and
required public capability projections. Serializer/G5 metadata hunks and G2
reload ownership must be coordinated before those edits. Version-5 records
must preflight without weakening older writer byte stability or introducing
spectral interpretation into an unsupported older envelope.

Lead admitted the proposal and confirmed one shared format 5. G5 owns the
shared discriminator and 4-to-5 identity migration scaffold plus Custom/Control
fields; this branch owns only spectral payload/helper/dispatch/preflight hooks.
The G2 catalog integration is conditional on its repaired dependency landing.

The first local public-consumer run exposed a harness expectation error:
`GraphSerializer::to_file` deliberately appends one newline to `to_json`.
The consumer now checks that existing file contract; production serialization
was unchanged. The same fixture then passed the independent audio oracle and
all four partitions. No installed-SDK proof is inferred from that local run.

Acceptance must bind saved graph and live-control input bytes, exercise
save/load/save and notation parse/format/reparse, and render through only
installed public APIs. Expected audio reuses the independent float64 direct
DFT/Hann/IDFT/WOLA oracle, with frozen tolerance and partition controls.
Missing/corrupt/digest-mismatched/unsupported records must refuse before old
graph/state changes. Positive controls, planted negatives and actual production
mutations must carry numeric exits and restoration evidence.

Planned planted negatives include missing asset references, duplicate asset
identities, canonical bytes corrupted with the original digest retained, valid
substituted bytes with mismatched digest, original-length/frame-count mismatch,
unsupported schema/codec/contract versions, wrong geometry/rate, mixed edge
flags, and graph/control input digest mismatches. Production mutation controls
must independently bypass digest/metadata/edge checks and substitute an asset;
the public replay or refusal assertions must catch each mutation, then pass
again after exact restoration. Existing finite EOF/reset/limits and unrelated
join/routing refusals remain acceptance requirements.

Full paced qualification, supported sanitizer lanes and remaining full G10
obligations remain required after this replay slice. This document is bounded
branch evidence; the lead owns planning roadmap and master-ledger updates.

## Production mutation evidence

Each loop purged/rebuilt the production helper object and subject, passed its
baseline, failed the intended assertion while broken, restored the exact source,
and passed again. The driver reported CONFIRMED for all four. Explicit per-phase numeric logs confirm baseline 0, broken 42, restored 0 for
each mutation; each `confirm_failure.sh` driver exited 0. The first rerun
attempt was INCONCLUSIVE (driver 2) because a semicolon in the no-spawn reason
prevented baseline CMake configuration. It changed no production source and
was rerun successfully after fixing the declaration.

| Mutation | Test observation while broken |
|---|---|
| Bypass canonical byte SHA256 check | Valid alternative canonical asset accepted with retained digest; refusal assertion fails |
| Bypass content ID binding to original extent | Changed original extent accepted; refusal assertion fails |
| Infer spectral edge kind instead of reading saved flag | Corrupted edge metadata accepted; refusal assertion fails |
| Substitute A for B in existing factory call | Independent audio oracle peak 0.433362, RMS 0.041823; frozen limits 0.00002 / 0.000002 |

The wrong-asset mutation selects `[a3-replay-audio-oracle]`, which has no
serialization equality proxy. The other three select `[negative]`. The valid
alternative canonical-byte digest control is retained unchanged in the final
source. No morph DSP or codec wire format was edited.

## Verification at the independent persistence milestone

- Full governed Release build completed twice, including the complete Node
  registration profile after installing locked jsx-runtime dependencies.
- Focused persistence: 16,724 assertions in six cases. Legacy serializer:
  4,391 assertions in 58 cases. No unrelated legacy restore contract changed.
- Installed consumer uses only `find_package(Pulp COMPONENTS host)`, public
  headers and `Pulp::host`. Its 148-line glue checks saved input hashes before
  loading, save/load/save bytes, independent numeric output and all partitions.
  Installed positive/corrupt/restored/missing exits: **0/42/0/42**.
- Production mutation baseline/broken/restored exits: **0/42/0** for digest,
  metadata/length binding, edge-kind validation and wrong-asset selection.
- Official complete-profile inventory was generated to a temporary destination;
  only `test/ctest_script_inputs/executables/pulp-test-spectral-graph-persistence.json`
  was selected for this branch. No public transitive loader API was added and
  existing loader coverage reports no gaps; no scanner rule was changed.
- Source gates correctly refuse source-selftest/build/diff-coverage suites on
  this dedicated-builder host. Those lanes remain NOT CHECKED locally. The
  examples-index entry, clang-format-21 changes and hosting skill note address
  the actionable local findings; final gates must be rerun before push.

Repaired notation dependency is still OPEN at
`7704749e0860016662491d128013ac34b6b19f42`, and G5 owns the unpublished shared
format-5 discriminator/migration scaffold. This branch has not copied or
published either dependency. Notation integration and a complete unarmed PR
remain pending those coordinated dependencies.
