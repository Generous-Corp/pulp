# MLX receipt provenance audit

Date: 2026-10-03
Audit ref: `origin/main` at `5ea08fd82495687c8e23c85de1a7d4ff429809a8`
Audit worktree: `codex/mlx-receipt-audit-20261002`

## Findings

The audited `origin/main` ref contains the execution and packaging receipts, but
not every path those receipts describe:

| Path | In audited `origin/main` | Finding |
|---|---:|---|
| `docs/reports/neural-mlx-execution-receipt-20261002.md` | yes | landed documentation receipt |
| `docs/reports/neural-mlx-packaging-control-receipt-20261002.md` | yes | landed documentation receipt |
| `docs/reports/neural-mlx-worker-receipt-20261002.md` | no | feature-lineage design receipt; related link corrected to the landed execution receipt |
| `tools/validation/mlx_worker_harness.py` | no | feature-lineage-only harness; execution receipt says it is not shipped |
| `core/gpu_audio/src/detail/neural_processor.hpp` | no | feature-lineage source reference, not a landed public implementation |
| `core/gpu_audio/src/detail/neural_model_manifest.hpp` | no | feature-lineage source reference |
| `test/test_neural_model_manifest.cpp` | no | feature-lineage test reference |

The `planning` submodule pointer also differs: feature lineage points at
`787f9f8eef42bf2ba1a76c79d2796efceccb95c4`, while `origin/main` points at
`ea04f9eab039bb6c7550cb2e7225acdf6c56087c`. The audit worktree leaves that
submodule uninitialized, so no planning claim was treated as locally verified.

Receipt bodies contain no Git commit IDs presented as implementation evidence.
The long hexadecimal strings in the execution receipt are MLX wheel SHA-256
hashes, not commits. All receipts retain the blocked/product-gate language and
explicitly classify synthetic timing as non-product evidence.

## Correction and limits

The packaging receipt now links to the landed execution receipt and identifies
the worker design receipt as feature-lineage-only. The execution receipt names
the harness as feature-lineage-only and says it is not shipped. The worker
receipt labels its source references as absent from the audited ref. These are
documentation/provenance corrections only; no public ABI, provider selection, or
GPU capability claim is added.

## Checks

- `git cat-file -e origin/main:<path>` checked every receipt, harness, source,
  and test path in the table.
- `git ls-tree origin/main planning` checked the submodule pointer.
- `git diff --check` is required before commit.

Next gate: land the private harness and any named-model adapter together with
their source/test paths, then rerun this audit against the new `origin/main`
before making any product or Phase 3 claim.
