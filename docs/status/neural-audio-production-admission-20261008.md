# Neural-audio production-admission disposition

**Review date:** 2026-10-08
**Scope:** latest Spectr 1.0.7 package and the DSPX-08 shared-renderer consumer, plus the remaining GPU-NAM/WaveNet admission gates.

This is an evidence disposition. It does not promote the experimental GPU path or infer neural production support from metadata, synthetic providers, or a signed package.

## Current gate matrix

| Gate | Status | Evidence and remaining condition |
| --- | --- | --- |
| Licensed production model | **BLOCKED** | No redistributable NAM/Magenta model artifact is present in the reviewed receipts. `example.nam` is a feasibility fixture. A named model SHA, license/redistribution terms, architecture, and provider receipt are required. |
| Product consumer | **PASS for Spectr shared spectral path; OPEN for GPU-NAM** | DSPX-08 receipt `spectr-consumer-receipt-20261008.md` proves a real Spectr consumer against Pulp SDK `v0.931.3` (`68a48f899164056ac25691668138ec95cee86af9`), 16 focused runtime tests, 2 host probes, and 3 installed host artifact tests. It explicitly excludes Forge, `.pulpgraph`, GPU-NAM, and default-on GPU rendering. |
| Package signing and notarization | **PASS** | Latest package `/Users/danielraffel/Code/spectr-package-sparkle-20261008/artifacts-package-final/Spectr-1.0.7.pkg` is 75,253,219 bytes with SHA-256 `990fd3e7c4784bf48cfa0caf55c67c69495739dd62127fa3a7475a5e35a5385f`. `pkgutil --check-signature`, `spctl --assess --type install --verbose=4`, and `xcrun stapler validate` all passed. Spectr source head is `12eb3717d0660c198f0df79e4de8cb1e329a686b`; merged package fix is `70bddfaa4f65bd8ead4726e001eedb53d10a7cbc`. |
| Realtime deadline margin | **OPEN** | Existing tests prove correctness, typed fallback, and accounting. They do not prove absolute sample-paced delivery, deadline distributions, DAW scheduling, or sustained load. Required campaign: quiet Apple Silicon host, exact provider/SDK/model hashes, `max_inflight={1,4}`, absolute pacing, retained raw sidecars, and terminal/fallback/retirement identities. |
| Fallback / underrun | **PARTIAL** | Prepared CPU fallback, typed rejection, late-result, cancellation, and trace accounting exist. Production admission still requires real paced host runs with classified underruns and installed-host contention evidence. |
| Device loss / recovery | **PARTIAL for private Pulp lifecycle; OPEN for product** | Private provider receipts cover loss before submit, during completion registration, after registration, fencing, epoch/reprepare, late completion, and drain. No authenticated Spectr campaign proves physical retirement/reprepare and stale-state suppression in the installed package. |
| Forge / graph exposure | **BLOCKED** | Forge metadata remains `catalog_registered=false` and `named_consumer=null`; no GPU-NAM or `.pulpgraph` consumer is proven. |

## What is feasible now

The latest package closes the distribution-signing prerequisite and the DSPX-08 receipt is sufficient to accept the opt-in shared spectral consumer as an experimental product path. The remaining neural campaign cannot run to acceptance with the reviewed artifacts because the named licensed model and authenticated GPU-NAM provider/SDK are absent. A synthetic or metadata-only run would not close any production gate.

The next dependency-satisfied packet is therefore:

1. Obtain a redistributable named model and record its immutable SHA-256, license, architecture, and source revision.
2. Pin the exact authenticated GPU-NAM provider and Pulp SDK build.
3. Reserve a quiet Apple Silicon host and run 100,000 absolute sample-paced blocks for `max_inflight=1` and `4`, retaining raw per-block receipts.
4. Include CPU-oracle parity, deadline-miss/underrun distributions, fallback and late-result identities, device-loss/reprepare, and installed AU/VST3/CLAP host behavior.
5. Review the receipt adversarially before any Forge registration, default-on policy, or production claim.

## External blockers

- licensed/redistributable NAM or Magenta model artifact;
- authenticated GPU-NAM provider and exact SDK/source provenance;
- quiet Apple Silicon capacity (the prior long-run audit was invalidated by VM/QEMU/Tart/indexing contention);
- DAW host access for installed realtime and recovery acceptance;
- a public ownership-safe recovery contract if third-party consumers must observe device-loss/reprepare.

Until those dependencies are present and the packet above passes, neural GPU-NAM production acceptance remains **FAIL-CLOSED**.
