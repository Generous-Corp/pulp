# Shipyard merge-gate evidence blocker

Status: blocked pending a source-backed aggregate producer contract.

The current protected-validation receipts emitted by
`tools/scripts/protected_merge_receipt.py` and `.github/workflows/build.yml`
prove an exact PR head/tree, protected base, workflow/run identity, policy,
toolchain, artifact digests, and successful CTest validation. The
`protected-receipt-reuse` job additionally proves an exact merge-group tree
and emits a reuse decision.

Those artifacts do not contain a review verdict, the final merge commit and
merge timestamp, or a complete required-check conclusion set. Emitting
`shipyard-merge-receipt/v1` from them would therefore infer safety from
partial evidence and is not allowed.

Next mutation: add a protected workflow producer for a versioned aggregate
`shipyard-merge-gate-evidence/v1` artifact. It must bind the exact repository,
base/head/merge-group identities, every required check and successful
conclusion, the review verdict, source receipt/decision digests, and an
immutable artifact digest. A follow-up Shipyard change can consume that
artifact, re-fetch the merged PR, fail closed on any mismatch, and emit the
durable merge receipt.

Required tests cover digest stability, missing or failed checks, missing review
approval, head/base/merge identity mismatch, and rejection of merged-at-only
evidence.
