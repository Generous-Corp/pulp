# Neural audio program status (2026-10-04)

This is the current coordination receipt for the model-neutral streaming and
Apple-Silicon-first neural-audio work. It is a status record, not a product
performance claim. Each lane below remains fail-closed unless its stated
receipt proves the corresponding behavior on the real provider.

## Protected-main baseline

- Pulp protected-main head observed: `0130b8b9709e8386f56e12d1b99807519a08f486`.
- Planning pointer merged previously: `9986b6a383e981277f71b297be216fba750b4567`.
- The superseded primary checkout is not an implementation surface; all new
  work must use a fresh worktree from protected `origin/main`.

## Open implementation PRs

| Lane | PR and head | Evidence observed | Current disposition |
|---|---|---|---|
| Versioned streaming benchmark | [PR 9495](https://github.com/Generous-Corp/pulp/pull/9495), `e184331c0b913302a062816c2bf5500263f14281` | Linux/macOS benchmark jobs passed in the merge-group; unrelated view-bridge editor-open failures recur on older merge groups | Still queued on the original head. Hardened successor `1940a69b5dfaf0d5dcdb0c58c5e16783c9393521` is not applied until the queued head has a safe disposition. |
| Neural model-package sidecars | [PR 9497](https://github.com/Generous-Corp/pulp/pull/9497), `1ab8c95e7ea298c77029b377caa4abda01c90a45` | Private layout validation, tamper/completeness checks, fresh-process reload, and installer-layout tests are green in the focused worktree | Open; sanitizer and hosted checks are still running. Generic `runtime::install_model` remains intentionally outside the private neural sidecar API. |
| Dawn WaveNet depth audit | [PR 9502](https://github.com/Generous-Corp/pulp/pull/9502), `13575659ef58d0e47b14742bbe112a83607d6fff` | Synthetic depth test 10 assertions/1 case; full synthetic realtime 463/18; slot-ledger 42/2, 112/6, 108/6; convolution/session 171/9 and 232/18; WaveNet matrix 123/4 | Open and behind its base while CodeQL/sanitizer checks run. Exact Dawn-provider depth proof is still open; `HAS_DAWN_SHARED_IO=TRUE` alone is insufficient. |
| Existing shared-I/O receipt contract | [PR 9494](https://github.com/Generous-Corp/pulp/pull/9494), `7aad55c81f8afd52fd46ac1b1d769182b1da6ae7` | Build, docs, Linux, Windows, and macOS checks observed green; CodeQL and sanitizers were still running at last snapshot | Open/unstable pending terminal checks; retain GPU/Dawn ownership boundaries. |

## Ready isolated package packets

- Package A gatefix: commit `47f6b491f86ace46de59123a8548a6b54fea50d3` in
  `/Users/danielraffel/Code/pulp-neural-package-a-gatefix-20261003`. The
  focused adapter proof passed 5 cases/29 assertions, generated-input check
  passed with 384 declarations, and `tools/scripts/gates.sh origin/main`
  passed. It has not been pushed.
- Package H streaming contract: commit
  `d18a9557d61e6155cbb6821499f303e0de75ce39` in
  `/Users/danielraffel/Code/pulp-neural-package-h-20261003`. Governed build,
  direct test (93 assertions/19 cases), exact CTest lookup (1/1), and
  `tools/scripts/gates.sh origin/main` passed. It has not been pushed.
- Package B model packaging: original commit
  `7d026b12a585a6109579bd26c79a57c4e17440bb` has the focused private manifest
  proof (57 assertions/10 cases), but the full gate is blocked by generated
  test-input drift (stale `pulp-cpp`, missing
  `pulp-test-group-core-gpu-audio-private`). A fresh gatefix worktree is
  rebuilding the governed test inventory before committing any generated
  change.

These packets are isolated and must be reconciled against the then-current
protected-main head before any push or PR creation.

## Product gates still open

1. **Real MLX model execution:** the named-model receipt proves MIT fixture
   provenance and CPU parity, but the checked-in MLX harness remains
   synthetic/tools-only. There is no shipped MLX provider, signed/notarized
   MLX bundle, or measured acceleration claim.
2. **Exact Dawn depth:** prove depth 1 versus depth 2 with the actual Apple
   provider, ordered delivery, late/device-loss fencing, and timestamp/deadline
   margin. Synthetic providers do not close this gate.
3. **Paced Magenta/Forge consumer:** obtain a named, licensed model artifact and
   a fresh paced producer receipt before any Forge/Spectr registration. The
   current metadata contract deliberately refuses catalog registration and
   does not claim sustained generation.
4. **Cross-platform RT receipts:** Package H documents the contract; Linux and
   Windows execution receipts remain required before broad platform claims.
5. **Competitive proof:** the benchmark JSON is feasibility evidence only until
   lock/blocking/fallback/late counters are measured rather than unavailable,
   bootstrap confidence intervals are hardened, and a real provider workload
   has a CPU-oracle comparison.

## Coordination rules

- Preserve E126 `LOCK-capability-transaction` and GPU-NAM/Dawn ownership.
- Do not force-push or mutate queued PR 9495.
- Every future receipt must identify the provider actually executed, model and
  artifact hashes, host, latency/lead, reset epoch, fallback reason, and whether
  the run was synthetic, CPU-only, or real-provider evidence.
- The program remains active until each open gate above has either landed with
  its evidence or received an explicit, reviewed disposition.
