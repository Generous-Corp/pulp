# Neural-audio production-admission disposition

**Review date:** 2026-10-08
**Scope:** latest Spectr 1.0.7 package and the DSPX-08 shared-renderer consumer, plus the remaining GPU-NAM/WaveNet admission gates.

This is an evidence disposition. It does not promote the experimental GPU path or infer neural production support from metadata, synthetic providers, or a signed package.

## Current gate matrix

| Gate | Status | Evidence and remaining condition |
| --- | --- | --- |
| Licensed production model | **OPEN / LICENSE BLOCKED** | A named WaveNet candidate exists at `/Users/danielraffel/Code/pulp-planning-timing-wgpu-status-20261008/research/artifacts/2026-09-27-gpu-audio-observability/nam-verified-m1/wavenet_a1_standard.nam`, SHA-256 `ceb53469a19ce278e2235da982ae676cb8d5451a8de22a7ecc7a2617d07224d1`, with architecture metadata and a provider-bound campaign. Its license and redistribution rights are absent from the receipt, so it cannot be treated as a production model. |
| Product consumer | **PASS for Spectr shared spectral path; OPEN for GPU-NAM** | DSPX-08 receipt `spectr-consumer-receipt-20261008.md` proves a real Spectr consumer against Pulp SDK `v0.931.3` (`68a48f899164056ac25691668138ec95cee86af9`), 16 focused runtime tests, 2 host probes, and 3 installed host artifact tests. It explicitly excludes Forge, `.pulpgraph`, GPU-NAM, and default-on GPU rendering. |
| Package signing and notarization | **PASS** | Latest package `/Users/danielraffel/Code/spectr-package-sparkle-20261008/artifacts-package-final/Spectr-1.0.7.pkg` is 75,253,219 bytes with SHA-256 `990fd3e7c4784bf48cfa0caf55c67c69495739dd62127fa3a7475a5e35a5385f`. `pkgutil --check-signature`, `spctl --assess --type install --verbose=4`, and `xcrun stapler validate` all passed. Spectr source head is `12eb3717d0660c198f0df79e4de8cb1e329a686b`; merged package fix is `70bddfaa4f65bd8ead4726e001eedb53d10a7cbc`. |
| Realtime deadline margin | **OPEN** | The newly found 48-attempt M1 campaign is useful provider-bound evidence, but it records four shared callback deadline misses across 18,144 measured callbacks, has no GPU/phase timestamps, and has no completion-p99 or DAW scheduling proof. It cannot support production realtime admission. |
| Fallback / underrun | **PARTIAL** | Prepared CPU fallback, typed rejection, late-result, cancellation, and trace accounting exist. Production admission still requires real paced host runs with classified underruns and installed-host contention evidence. |
| Device loss / recovery | **PARTIAL for private Pulp lifecycle; OPEN for product** | Private provider receipts cover loss before submit, during completion registration, after registration, fencing, epoch/reprepare, late completion, and drain. No authenticated Spectr campaign proves physical retirement/reprepare and stale-state suppression in the installed package. |
| Forge / graph exposure | **BLOCKED** | Forge metadata remains `catalog_registered=false` and `named_consumer=null`; no GPU-NAM or `.pulpgraph` consumer is proven. |

## Newly discovered provider-bound campaign

The planning checkout contains a frozen M1 campaign under
`/Users/danielraffel/Code/pulp-planning-timing-wgpu-status-20261008/research/artifacts/2026-09-27-gpu-audio-observability/nam-verified-m1`.
Its manifest records source SHA `a124520f14317e10bdb28c6a506e7843ac99cabc`, SDK SHA
`7858d7edb5a2bbb568c03d7e740043e8c897c0ee`, provider binary SHA-256
`a60627ffdf9679a28b53a04da75ac6f6a4c61bd7443de0c738874beef3ffdb22`, model
SHA-256 `ceb53469a19ce278e2235da982ae676cb8d5451a8de22a7ecc7a2617d07224d1`,
native library SHA-256 `315e83473e19dcfac60f613bcecb183b7baaadd8c70b59cf9e83addfd49ac5ac`,
an Apple M1 Max hardware guard, and 48/48 completed attempts.

The campaign retained CPU shadow parity and reported zero numerical mismatches.
Shared delivery selected all 18,000 GPU results, but four callback deadlines were
missed; staged delivery had 210,176 dropped input frames. The receipt explicitly
says GPU timestamps, phase timestamps, completion p99, CPU-only comparison, and
production readiness are unavailable. This is feasibility evidence, not production
admission.

## What is feasible now

The latest package closes the distribution-signing prerequisite and the DSPX-08 receipt is sufficient to accept the opt-in shared spectral consumer as an experimental product path. The newly discovered campaign provides provider-bound feasibility evidence, but the named model's license/redistribution terms, a current Spectr GPU-NAM product consumer, hard realtime evidence, and product-level recovery proof remain open. A synthetic or metadata-only run would not close any production gate.

The next dependency-satisfied packet is therefore:

1. Obtain redistribution terms for the named model and record its immutable SHA-256, license, architecture, and source revision.
2. Reconcile the campaign's provider and SDK SHAs against the current Spectr/Pulp SDK and pin the exact authenticated build.
3. Reserve a quiet Apple Silicon host and run 100,000 absolute sample-paced blocks for `max_inflight=1` and `4`, retaining raw per-block receipts.
4. Include CPU-oracle parity, deadline-miss/underrun distributions, fallback and late-result identities, device-loss/reprepare, and installed AU/VST3/CLAP host behavior.
5. Review the receipt adversarially before any Forge registration, default-on policy, or production claim.

## External blockers

- license/redistribution terms for the named NAM artifact;
- current authenticated GPU-NAM provider and exact SDK/source provenance;
- quiet Apple Silicon capacity (the prior long-run audit was invalidated by VM/QEMU/Tart/indexing contention);
- DAW host access for installed realtime and recovery acceptance;
- a public ownership-safe recovery contract if third-party consumers must observe device-loss/reprepare.

Until those dependencies are present and the packet above passes, neural GPU-NAM production acceptance remains **FAIL-CLOSED**.

## Next packet execution contract

The next packet is dependency-gated and must be executed in this order:

1. **Model rights:** attach a signed license/redistribution record to the exact model SHA `ceb53469a19ce278e2235da982ae676cb8d5451a8de22a7ecc7a2617d07224d1`. Without that record, the model remains research-only.
2. **Provider reconciliation:** rebuild or authenticate the current Spectr/Pulp SDK path and prove its source, SDK, provider binary, native library, and model hashes in one receipt. The prior M1 campaign used source `a124520f14317e10bdb28c6a506e7843ac99cabc` and SDK `7858d7edb5a2bbb568c03d7e740043e8c897c0ee`; those are evidence anchors, not current-product proof.
3. **Realtime campaign:** run absolute sample-paced blocks with GPU/phase timestamps, completion p99, deadline distributions, and `max_inflight={1,4}` on a quiet Apple Silicon host. Any deadline miss fails admission until explained and corrected.
4. **Fallback and recovery:** inject late, dropped, cancelled, and device-loss events; prove CPU fallback continuity, terminal accounting, physical retirement/reprepare, epoch fencing, and stale-state suppression in the installed product.
5. **Product acceptance:** run the exact installed Spectr AU/VST3/CLAP package in a host/DAW campaign and retain immutable raw sidecars. Forge registration, `.pulpgraph` exposure, and default-on policy remain prohibited until every preceding gate passes.

The adversarial review must independently check the model rights, exact-hash lineage, timestamp completeness, miss accounting, fallback/recovery identities, installed-host scope, and the absence of any acceptance claim derived only from metadata, synthetic providers, or CPU-shadow parity.
