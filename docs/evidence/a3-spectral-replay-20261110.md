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
  only the new `pulp-test-spectral-graph-persistence.json` and
  `pulp-a3-replay-fixture-writer.json` executable fragments were selected for
  this branch. No public transitive loader API was added and
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

## Clean dependency handoff

Root requested a stop after independent gates because repaired notation
`7704749e0860016662491d128013ac34b6b19f42` is OPEN with macOS running and no
counted verdict, and the shared G5 format-5 scaffold is still being authored.
No dependency commit was copied and no PR was created or pushed by this branch.
Resume only when root relays the actual landed dependency heads, then integrate
catalog spectral bindings, mono boundaries, exact-configuration formatting and
typed-edge strict loading, extend the pinned notation fixture/consumer, rerun
exact-head validation, and publish the single unarmed PR.

The final gate pass initially found the fixture-writer executable fragment
missing. Its official generated fragment is now included alongside the focused
test fragment. Advisory unrelated inherited inventory differences were left
untouched. A final reconfigure and source-gate rerun follows this evidence
commit; its exact process result is supplied in the cmux handoff.

Installed independent-slice executable: 4,003,440 bytes, SHA256
`9bc25d6131da3b026be4b4c2dcbd607e3230e79373006b4c102e3bfad2057cb5`.
Producer marker digest:
`28224cf0e9599bf217fbcf1d485f2db0c789ba8ee99a492212a34971cc8e7a22`.
Expected numeric output peak `8.0661982593266401e-08`, RMS
`6.7957485130856682e-09`; rendered binary32 output SHA256
`76553b80d5d0afdf7e99fd952fdcfbc9ea649603358c324d191925d07532d3b9`.
These are independent-slice receipts, not final integrated exact-head proof.

## Revival onto current main (2026-11-15)

The paused lane was revived on `graph/a3-spectral-persist-20261115` by merging
`codex/a3-spectral-replay-20261110` at `0375e435de28` onto `origin/main` at
`abb85fd983be`. The only textual conflict was the `pulp-host` source list,
where both sides added sources; both were kept.

Dependency status, checked against GitHub and `origin/main`:

- Notation [PR 11999](https://github.com/Generous-Corp/pulp/pull/11999) merged
  on 2026-11-11. Spectral constructor catalog rows
  ([PR 12258](https://github.com/Generous-Corp/pulp/pull/12258)) are on main.
  Notation still refuses spectral nodes by design; the catalog binding and
  notation spelling planned above were never implemented on this lane and are
  not part of this revival.
- `kFormatVersion` was 5 on main. Review found that a format-5 reader would
  admit a format-5 spectral save as unresolved Custom nodes and drop the
  assets on re-save, so the spectral envelope now writes and reads format 6,
  with an identity v5 -> v6 migration for ordinary files.
- The finite spectral stream rule
  ([PR 12319](https://github.com/Generous-Corp/pulp/pull/12319)) was OPEN at
  `399c5e5b9cfe`; this branch merges that head, so it is stacked on that PR.

Change on revival: each saved asset record stores `frame_count`, and the
reader derives it through `SpectralFiniteStreamRule` from the saved contract
geometry and original extent, refusing refused geometry, refused extent or a
mismatched count before decoding bytes. The writer takes the count from the
same rule and also refuses Control-signal edges. The fixture was re-recorded:
only `graph.pulpgraph` and its manifest line changed; asset bytes, the control
timeline and rendered output SHA256
`76553b80d5d0afdf7e99fd952fdcfbc9ea649603358c324d191925d07532d3b9` (peak
`8.0661982593266401e-08`, RMS `6.7957485130856682e-09`) are unchanged.

Owner-decision conflict: `docs/graph-program-status/G-10-notation-and-codec-source-proposal.md`
on main (written after this lane paused) lists spectral persistence as an open
owner question, recommends persisting analysis/stage/synthesis inside the G-3
v6 writer with asset morph refused, and asks whether assets should be embedded
or sidecar. This lane embeds assets in format 6 and does not persist analysis
nodes, per the lead admission recorded above. The revived lane writes format 6;
that ruling needs to be
reconciled before publication.
