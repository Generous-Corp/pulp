# MLX receipt provenance audit

Date: 2026-10-03 (refreshed 2026-10-04)
Audit ref: `origin/main` at `e260a88701eb85d1a22e11952826f98a4a36671c`
Audit worktree: fresh checkout from current `origin/main`

The original audit below was performed against `5ea08fd82495687c8e23c85de1a7d4ff429809a8`.
That snapshot is historical. This refresh reconciles the receipts with the
merged neural packages and the current source tree.

## Findings

The current `origin/main` ref contains the execution and packaging receipts and
the private implementation/test paths that were previously branch-local:

| Path | In current `origin/main` | Finding |
|---|---:|---|
| `docs/reports/neural-mlx-execution-receipt-20261002.md` | yes | landed documentation receipt |
| `docs/reports/neural-mlx-packaging-control-receipt-20261002.md` | yes | landed documentation receipt |
| `docs/reports/neural-mlx-worker-receipt-20261002.md` | yes | landed design receipt; it still records a CPU-only seam and open MLX gates |
| `tools/validation/mlx_worker_harness.py` | yes | landed tools-only synthetic probe; default-off and not a shipped product path |
| `core/gpu_audio/src/detail/neural_processor.hpp` | yes | landed private CPU-only lifecycle facade; non-CPU preferences still fall back or remain unexecutable |
| `core/gpu_audio/src/detail/neural_model_manifest.hpp` | yes | landed private control-plane manifest and installed-asset admission helper |
| `test/test_neural_model_manifest.cpp` | yes | landed private manifest/admission tests |
| `core/gpu_audio/src/detail/nam_tcn_artifact.hpp` | yes | landed private serialized NAM/TCN CPU bridge; no MLX or public ABI claim |
| `test/test_nam_tcn_adapter.cpp` | yes | landed private NAM/TCN bridge tests |

The paths above landed through the neural follow-up/integration and package
merges ([PR 9332](https://github.com/Generous-Corp/pulp/pull/9332),
[PR 9331](https://github.com/Generous-Corp/pulp/pull/9331),
[PR 9412](https://github.com/Generous-Corp/pulp/pull/9412), and
[PR 9415](https://github.com/Generous-Corp/pulp/pull/9415)). Landing private
sources and tests does not make an MLX provider or a shipped neural product.

The current `origin/main` `planning` gitlink is
`9986b6a383e981277f71b297be216fba750b4567`. The audit checkout leaves that
submodule uninitialized, so no planning claim was treated as locally verified.
The planning-pointer updates are merged in
[PR 9459](https://github.com/Generous-Corp/pulp/pull/9459) and
[PR 9466](https://github.com/Generous-Corp/pulp/pull/9466); they do not close
the MLX product gate.

The execution receipt's long hexadecimal strings are MLX wheel SHA-256 digests,
not Git commits. Separately, the landed neural admission helper calls
`pulp::runtime::sha256_file_hex` to hash installed artifact bytes and compare
them with the recorded manifest digest; that helper is content verification,
not source provenance. The receipts contain no Git commit presented as model or
provider execution evidence. All receipts retain the blocked/product-gate
language and explicitly classify synthetic timing as non-product evidence.

## Correction and limits

The packaging receipt links to the landed execution receipt and identifies the
worker design receipt as a design/measurement record. The execution receipt now
describes the harness as landed tools-only synthetic evidence, while stating
that it is not wired into a plugin build or shipped as a product. The worker
receipt's CPU-only decision remains correct for the landed facade. These are
documentation/provenance corrections only; no public ABI, provider selection,
or GPU capability claim is added.

## Checks

- `git cat-file -e origin/main:<path>` checked every receipt, harness, source,
  and test path in the table at `e260a88701eb85d1a22e11952826f98a4a36671c`.
- `git ls-tree origin/main planning` checked the submodule pointer.
- `git diff --check` is required before commit.

Next gate: adapt the landed private harness to a named model and the existing
Pulp paced transport, add CPU-oracle/fallback/thermal/residency evidence, and
rerun this audit against the resulting `origin/main` before making any product
or Phase 3 claim.
